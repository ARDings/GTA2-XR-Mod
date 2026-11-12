#include "renderer.h"
#include "dynamic_draw.h"
#include "spatial_batches.h"
#include "texture_filter.h"
#include "graphics_quality.h"
#include "ceiling_present_shader.h"

#include <algorithm>
#include <tuple>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace gta2 {
namespace {

void (*g_trace)(const char*) = nullptr;

void Trace(const char* format, ...) {
    if (!g_trace) return;
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    g_trace(line);
}

// Where Direct3D comes from, and it matters which copy.
//
// RTX Remix ships as a d3d9.dll dropped beside the game, so *anything* in the
// process that asks the loader for "d3d9.dll" gets the Remix bridge client -
// including GTA2's own DirectDraw layer, which initialises during startup long
// before the renderer is loaded. That brings the Remix runtime up on a device
// DirectDraw owns: a fullscreen A8R8G8B8 one the game never draws through.
// Remix drives a single device per process, so ours arrives second, re-hooks the
// window procedure out from under the bridge's message channel, and the game
// hangs at the menu with the runtime still waiting on a handshake that can no
// longer be answered.
//
// Renaming the bridge and asking for it by that name keeps every accidental
// consumer on the system d3d9 and leaves the renderer's device the only one
// Remix ever sees. The plain name is still tried, so a stock Remix drop-in and a
// machine with no Remix at all both keep working.
const char* const kD3D9Modules[] = {"d3d9.dll"};

IDirect3D9* CreateD3D9() {
    for (const char* name : kD3D9Modules) {
        const HMODULE module = LoadLibraryA(name);
        if (!module) continue;
        using CreateFn = IDirect3D9*(WINAPI*)(UINT);
        const auto create = reinterpret_cast<CreateFn>(GetProcAddress(module, "Direct3DCreate9"));
        if (!create) continue;
        if (IDirect3D9* d3d = create(D3D_SDK_VERSION)) {
            Trace("Direct3D from %s", name);
            return d3d;
        }
    }
    return nullptr;
}

constexpr DWORD kWorldFvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;

// Tiles key their transparency off palette entry 0, so an alpha test is enough;
// it also keeps geometry opaque from the path tracer's point of view, which
// behaves far better than alpha blending under Remix.
constexpr DWORD kAlphaRef = 128;

AlphaMode g_alphaMode = AlphaMode::Test;
DWORD g_alphaRef = kAlphaRef;

// Sand for the riverbed (RiverbedTileIndex): muddy harbour sand with a little
// grain and the odd darker pebble. Tileable, since the bed repeats it once per
// block, and generated from fixed numbers, so the texture - and with it the
// Remix hash a sand material would key on - is the same on every run.
uint32_t SandTexel(int x, int y) {
    auto hash = [](uint32_t a, uint32_t b) {
        uint32_t h = a * 0x8DA6B343u ^ b * 0xD8163841u ^ 0x5BD1E995u;
        h ^= h >> 13;
        h *= 0x5BD1E995u;
        return (h ^ (h >> 15)) & 0xFFFFu;
    };
    // Value noise on a lattice that divides the tile, so it wraps exactly.
    auto noise = [&](int cells, uint32_t seed) {
        const float fx = static_cast<float>(x) * cells / kTileSize;
        const float fy = static_cast<float>(y) * cells / kTileSize;
        const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
        const float tx = fx - x0, ty = fy - y0;
        const float sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
        auto at = [&](int cx, int cy) {
            return hash(static_cast<uint32_t>(cx % cells) + seed, static_cast<uint32_t>(cy % cells)) /
                   65535.0f;
        };
        const float top = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * sx;
        const float bottom = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * sx;
        return top + (bottom - top) * sy;
    };
    const float ripple = noise(4, 11) * 0.5f + noise(8, 23) * 0.3f + noise(16, 37) * 0.2f;
    const float grain = hash(static_cast<uint32_t>(x), static_cast<uint32_t>(y) + 101u) / 65535.0f;
    float shade = 0.82f + 0.22f * ripple + 0.08f * (grain - 0.5f);
    if (noise(32, 53) > 0.86f) shade *= 0.72f;   // pebbles
    auto channel = [&](float base) {
        const float v = base * shade;
        return static_cast<uint32_t>(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
    };
    return 0xFF000000u | (channel(150.0f) << 16) | (channel(136.0f) << 8) | channel(104.0f);
}
}  // namespace

void SetAlphaMode(AlphaMode mode) { g_alphaMode = mode; }
AlphaMode GetAlphaMode() { return g_alphaMode; }
void SetAlphaRef(int ref) { g_alphaRef = static_cast<DWORD>(ref < 1 ? 1 : (ref > 254 ? 254 : ref)); }
int GetAlphaRef() { return static_cast<int>(g_alphaRef); }

namespace {

void SetMatrix(IDirect3DDevice9* device, D3DTRANSFORMSTATETYPE state, const Mat4& matrix) {
    D3DMATRIX d3dMatrix;
    memcpy(&d3dMatrix, matrix.m, sizeof(d3dMatrix));
    device->SetTransform(state, &d3dMatrix);
}

}  // namespace

void SetRendererTrace(void (*sink)(const char*)) { g_trace = sink; }

Renderer::~Renderer() { Shutdown(); }

void Renderer::Shutdown() {
    ReleaseUi();
    ReleaseSceneTargets();
    ReleaseResources();
    batches_.clear();
    if (maskTexture_) { maskTexture_->Release(); maskTexture_ = nullptr; }
    if (device_) {
        DynamicDraw::Instance().Release();
        device_->Release();
        device_ = nullptr;
    }
    if (d3d_) {
        d3d_->Release();
        d3d_ = nullptr;
    }
}

void Renderer::ReleaseAtlas() {
    if (atlas_) { atlas_->Release(); atlas_ = nullptr; }
    if (atlasVertices_) { atlasVertices_->Release(); atlasVertices_ = nullptr; }
    if (atlasIndices_) { atlasIndices_->Release(); atlasIndices_ = nullptr; }
    atlasGroups_.clear(); atlasSlots_.clear(); atlasFrames_.clear();
    atlasWidth_ = atlasHeight_ = 0;
}

void Renderer::ReleaseResources() {
    ReleaseAtlas();
    // Active slots borrow from two distinct owners. Do not search frame maps
    // per tile or discard the original texture when an animation first starts.
    textures_.clear();
    for (auto* texture : initialTextures_) if (texture) texture->Release();
    initialTextures_.clear();
    for (auto& frame : tileFrames_) {
        if (frame.second) frame.second->Release();
    }
    tileFrames_.clear();
    if (vertexBuffer_) {
        vertexBuffer_->Release();
        vertexBuffer_ = nullptr;
    }
    if (indexBuffer_) {
        indexBuffer_->Release();
        indexBuffer_ = nullptr;
    }
}

// Drop the level's geometry, and nothing else.
//
// GTA2 frees its map when it returns to the menu, and the renderer used to keep
// drawing the last level regardless: DrawWorld's only guard is that a vertex
// buffer exists, and one did. So quitting to the menu left a frozen cityscape on
// screen with the menu drawn over it - and under Remix that geometry was still
// in the scene, still being path traced, still lit.
//
// Deliberately not ReleaseResources: the device, the window and the tile
// textures all outlive a level, and tearing them down here would mean rebuilding
// the device every time someone backs out to the menu.
void Renderer::ReleaseWorld() {
    batches_.clear();
    ReleaseUi();
    if (vertexBuffer_) {
        vertexBuffer_->Release();
        vertexBuffer_ = nullptr;
    }
    if (indexBuffer_) {
        indexBuffer_->Release();
        indexBuffer_ = nullptr;
    }
}

void Renderer::ReleaseUi() {
    if (savedDepth_) {
        savedDepth_->Release();
        savedDepth_ = nullptr;
    }
    if (savedTarget_) {
        savedTarget_->Release();
        savedTarget_ = nullptr;
    }
    if (uiSurface_) {
        uiSurface_->Release();
        uiSurface_ = nullptr;
    }
    if (uiTexture_) {
        uiTexture_->Release();
        uiTexture_ = nullptr;
    }
    uiClearPending_ = true;
}

void Renderer::ApplyScale(int percent) {
    renderScale_ = std::clamp(percent, 50, 200);
    // Keep the scene within 4096 px wide: beyond that the Quest GPU (and DXVK
    // limits with MSAA) gain nothing but memory and frame time.
    if (presentWidth_ > 0) renderScale_ = std::min(renderScale_, 4096 * 100 / presentWidth_);
    // Even sizes keep both SBS halves identical.
    width_ = std::max(64, (presentWidth_ * renderScale_ / 100) & ~1);
    height_ = std::max(32, presentHeight_ * renderScale_ / 100);
}

bool Renderer::Initialize(HWND window, int width, int height, std::string* error) {
    presentWidth_ = width;
    presentHeight_ = height;
    // GTA2Ceiling: fixed quality. The scene is the window size (100 %, the
    // host shows it 1:1); no MSAA (4x measured too heavy: GPU at 85-90 % with a
    // zoomed-out city). No in-headset or saved override any more.
    ApplyScale(100);

    // Kept across a retry: creating the interface is what starts RTX Remix's
    // runtime, and asking for it twice would start it twice.
    if (!d3d_) {
        Trace("Direct3DCreate9...");
        d3d_ = CreateD3D9();
        if (!d3d_) {
            *error = "Direct3DCreate9 failed";
            return false;
        }
        Trace("Direct3DCreate9 -> %p", d3d_);
    }

    // Only the first attempt is traced. Under RTX Remix this runs once a frame
    // until the device takes, and one line per attempt would bury the log.
    const bool trace = deviceAttempts_++ == 0;

    // A windowed backbuffer has to match the current display mode, and GTA2's
    // Glide wrapper may have switched the desktop to another depth while it
    // runs, so adopt whatever the adapter is in right now.
    D3DDISPLAYMODE displayMode = {};
    const HRESULT modeResult = d3d_->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &displayMode);
    if (trace) {
        Trace("display mode hr=0x%08lX %ux%u format=%d", static_cast<unsigned long>(modeResult),
              displayMode.Width, displayMode.Height, static_cast<int>(displayMode.Format));
    }

    D3DPRESENT_PARAMETERS pp = {};
    pp.BackBufferWidth = presentWidth_;
    pp.BackBufferHeight = presentHeight_;
    pp.BackBufferFormat = displayMode.Format != D3DFMT_UNKNOWN ? displayMode.Format : D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = FALSE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    // The simulation pacer owns timing; idle replay follows fresh XR packets.
    // Blocking on the emulated desktop's vblank would throttle both clocks.
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    char qualityPath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, qualityPath, MAX_PATH);
    char* leaf = std::strrchr(qualityPath, '\\');
    if (leaf) std::strcpy(leaf + 1, "gta2ceiling.ini");
    UINT requestedSamples = 0;  // GTA2Ceiling: no MSAA (GPU budget)

    if (trace) {
        Trace("CreateDevice hwnd=%p %dx%d windowed=%d fmt=%d", window, width, height,
              (int)pp.Windowed, static_cast<int>(pp.BackBufferFormat));
    }
    HRESULT hr = D3DERR_NOTAVAILABLE;
    // Presentation itself is never multisampled: the private scene target
    // resolves coverage before the reserved host colour key is encoded.
    pp.MultiSampleType = D3DMULTISAMPLE_NONE;
    hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                            D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device_);
    if (FAILED(hr)) hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device_);
    if (FAILED(hr)) {
        // Not necessarily fatal. RTX Remix hands the interface back before the
        // 64-bit runtime behind it can serve a device, and turns everything down
        // with D3DERR_INVALIDCALL until it can; it only gets there once the game
        // has run its own loop for a moment. The caller retries per frame.
        char message[160];
        snprintf(message, sizeof(message), "CreateDevice failed (hr=0x%08lX, display format=%d)",
                 static_cast<unsigned long>(hr), static_cast<int>(displayMode.Format));
        *error = message;
        return false;
    }
    Trace("device created on attempt %d", deviceAttempts_);
    gta2ceiling::ConfigureTextureFiltering(device_, qualityPath);
    presentParameters_ = pp;
    hr=CreateSceneTargetsFor(requestedSamples);
    if (FAILED(hr) && renderScale_!=100) {
        Trace("render scale %d%% failed; using 100%%",renderScale_);
        ApplyScale(100);
        hr=CreateSceneTargetsFor(requestedSamples);
    }
    if (FAILED(hr)) {
        *error="Cannot create alpha-coverage scene targets/compositor";
        Shutdown(); return false;
    }
    Trace("graphics MSAA requested=%u effective=%u smooth=%d anisotropy=%lu alpha-resolve=1",
          requestedSamples, static_cast<UINT>(sceneSamples_),
          gta2ceiling::g_graphicsQuality.smooth_textures ? 1 : 0,
          static_cast<unsigned long>(gta2ceiling::g_graphicsQuality.anisotropy));
    return true;
}

HRESULT Renderer::CreateSceneTargetsFor(UINT requestedSamples) {
    HRESULT hr=D3DERR_NOTAVAILABLE;
    // Check the alpha-bearing offscreen format, not the opaque swapchain.
    for (const UINT samples : {8u, 4u, 2u, 0u}) {
        if (samples > requestedSamples) continue;
        const auto type = static_cast<D3DMULTISAMPLE_TYPE>(samples);
        DWORD colourQuality=0, depthQuality=0;
        if (samples && (FAILED(d3d_->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT,
                D3DDEVTYPE_HAL,D3DFMT_A8R8G8B8,TRUE,type,&colourQuality)) ||
            FAILED(d3d_->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT,
                D3DDEVTYPE_HAL,D3DFMT_D24S8,TRUE,type,&depthQuality)) ||
            !colourQuality || !depthQuality)) continue;
        sceneSamples_=type;
        hr=CreateSceneTargets();
        if (SUCCEEDED(hr)) break;
        ReleaseSceneTargets();
        Trace("MSAA %u scene target failed: 0x%08lX",samples,static_cast<unsigned long>(hr));
    }
    Trace("scene %dx%d (render scale %d%%) for %dx%d present, MSAA requested=%u effective=%u",
          width_,height_,renderScale_,presentWidth_,presentHeight_,requestedSamples,
          static_cast<UINT>(sceneSamples_));
    return hr;
}

bool Renderer::ApplyQuality(int renderScalePercent, UINT msaaSamples) {
    if (!device_ || frameOpen_ || savedTarget_) return false;
    ReleaseUi();
    ReleaseSceneTargets();
    ApplyScale(renderScalePercent);
    HRESULT hr=CreateSceneTargetsFor(msaaSamples);
    if (FAILED(hr)) {
        ReleaseSceneTargets();
        ApplyScale(100);
        hr=CreateSceneTargetsFor(0);
    }
    return SUCCEEDED(hr);
}

void Renderer::ReleaseSceneTargets() {
    frameOpen_=false;
    if (device_) {
        device_->SetTexture(0,nullptr);
        device_->SetPixelShader(nullptr);
        device_->SetDepthStencilSurface(nullptr);
        if(presentTarget_) device_->SetRenderTarget(0,presentTarget_);
    }
    if(presentShader_) {presentShader_->Release();presentShader_=nullptr;}
    if(sceneDepth_) {sceneDepth_->Release();sceneDepth_=nullptr;}
    if(sceneTarget_) {sceneTarget_->Release();sceneTarget_=nullptr;}
    if(resolvedTarget_) {resolvedTarget_->Release();resolvedTarget_=nullptr;}
    if(resolvedTexture_) {resolvedTexture_->Release();resolvedTexture_=nullptr;}
    if(presentTarget_) {presentTarget_->Release();presentTarget_=nullptr;}
}

HRESULT Renderer::CreateSceneTargets() {
    HRESULT result=device_->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&presentTarget_);
    if(SUCCEEDED(result)) result=device_->CreateTexture(width_,height_,1,D3DUSAGE_RENDERTARGET,
        D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&resolvedTexture_,nullptr);
    if(SUCCEEDED(result)) result=resolvedTexture_->GetSurfaceLevel(0,&resolvedTarget_);
    if(SUCCEEDED(result)) {
        if(sceneSamples_==D3DMULTISAMPLE_NONE) {sceneTarget_=resolvedTarget_;sceneTarget_->AddRef();}
        else result=device_->CreateRenderTarget(width_,height_,D3DFMT_A8R8G8B8,
            sceneSamples_,0,FALSE,&sceneTarget_,nullptr);
    }
    if(SUCCEEDED(result)) result=device_->CreateDepthStencilSurface(width_,height_,D3DFMT_D24S8,
        sceneSamples_,0,TRUE,&sceneDepth_,nullptr);
    if(SUCCEEDED(result)) result=device_->CreatePixelShader(gta2ceiling::kCeilingPresentShader,&presentShader_);
    if(SUCCEEDED(result)) result=device_->SetRenderTarget(0,sceneTarget_);
    if(SUCCEEDED(result)) result=device_->SetDepthStencilSurface(sceneDepth_);
    return result;
}

HRESULT Renderer::ResetDevice() {
    if (!device_) return D3DERR_DEVICELOST;
    ReleaseUi();
    ReleaseSceneTargets();
    DynamicDraw::Instance().Release();  // D3DPOOL_DEFAULT, must go before Reset
    HRESULT result=device_->Reset(&presentParameters_);
    if(SUCCEEDED(result)) result=CreateSceneTargets();
    if(FAILED(result)) ReleaseSceneTargets();
    Trace("device reset hr=0x%08lX",static_cast<unsigned long>(result));
    if (SUCCEEDED(result)) lastPresent_=D3D_OK;
    return result;
}

bool Renderer::UploadWorld(const WorldMesh& mesh, const Style& style, std::string* error) {
    if (mesh.vertices.empty()) {
        *error = "world mesh is empty";
        return false;
    }
    ReleaseResources();
    batches_ = mesh.batches;

    const UINT vertexBytes = static_cast<UINT>(mesh.vertices.size() * sizeof(Vertex));
    if (FAILED(device_->CreateVertexBuffer(vertexBytes, D3DUSAGE_WRITEONLY, kWorldFvf,
                                           D3DPOOL_MANAGED, &vertexBuffer_, nullptr))) {
        *error = "CreateVertexBuffer failed";
        return false;
    }
    void* dest = nullptr;
    vertexBuffer_->Lock(0, 0, &dest, 0);
    memcpy(dest, mesh.vertices.data(), vertexBytes);
    vertexBuffer_->Unlock();

    const UINT indexBytes = static_cast<UINT>(mesh.indices.size() * sizeof(uint32_t));
    if (FAILED(device_->CreateIndexBuffer(indexBytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX32,
                                          D3DPOOL_MANAGED, &indexBuffer_, nullptr))) {
        *error = "CreateIndexBuffer failed";
        return false;
    }
    indexBuffer_->Lock(0, 0, &dest, 0);
    memcpy(dest, mesh.indices.data(), indexBytes);
    indexBuffer_->Unlock();

    // One texture per tile keeps Remix's texture hashes stable and per-tile
    // replaceable, instead of hiding every surface behind one atlas hash. Two
    // slots past the end for the seal under the world and the riverbed, which
    // have no tile of their own - see SealTileIndex and RiverbedTileIndex.
    const int sealTile = SealTileIndex(style);
    const int riverbedTile = RiverbedTileIndex(style);
    textures_.assign(static_cast<size_t>(riverbedTile) + 1, nullptr);
    std::map<int, std::vector<uint32_t>> tilePixels;
    for (const TileBatch& batch : batches_) {
        if (textures_[batch.tile]) continue;

        std::vector<uint32_t> pixels(static_cast<size_t>(kTileSize) * kTileSize);
        if (batch.tile == sealTile) {
            // Opaque black, and a whole tile of it: the seal is meant to absorb,
            // and giving Remix a real texture rather than an untextured draw
            // keeps it one identifiable material that can be replaced.
            for (int y = 0; y < kTileSize; ++y) {
                uint32_t* row = pixels.data() + static_cast<size_t>(y) * kTileSize;
                for (int x = 0; x < kTileSize; ++x) row[x] = 0xFF000000u;
            }
        } else if (batch.tile == riverbedTile) {
            for (int y = 0; y < kTileSize; ++y) {
                uint32_t* row = pixels.data() + static_cast<size_t>(y) * kTileSize;
                for (int x = 0; x < kTileSize; ++x) row[x] = SandTexel(x, y);
            }
        } else {
            const Tile& tile = style.GetTile(batch.tile);
            for (int y = 0; y < kTileSize; ++y) {
                memcpy(pixels.data() + static_cast<size_t>(y) * kTileSize,
                       &tile.pixels[static_cast<size_t>(y) * kTileSize], kTileSize * 4);
            }
        }
        IDirect3DTexture9* texture = nullptr;
        gta2ceiling::MipOptions mipOptions;
        mipOptions.avoid_exact_magenta = true;
        mipOptions.bleed_transparent_edges = true;
        if (FAILED(gta2ceiling::CreateManagedArgbMipTexture(device_, pixels, kTileSize, kTileSize,
                                                          &texture, mipOptions))) {
            *error = "CreateTexture mip chain failed";
            return false;
        }
        textures_[batch.tile] = texture;
        initialTextures_.push_back(texture);
        tilePixels[batch.tile] = std::move(pixels);
    }
    if (!BuildAtlas(mesh, tilePixels)) {
        ReleaseAtlas();
        Trace("tile atlas unavailable; drawing one batch per tile texture");
    }
    return true;
}

namespace {
constexpr int kAtlasPitch = 96, kAtlasGutter = 16, kAtlasLevels = 6;  // 64 px tile + 16 px edge copy
}

namespace {
std::vector<std::vector<uint32_t>> AtlasLevels(std::vector<gta2ceiling::ArgbMipLevel>&& levels) {
    std::vector<std::vector<uint32_t>> out;
    for (auto& level : levels) out.push_back(std::move(level.argb));
    return out;
}
}

void Renderer::WriteAtlasSlot(int tile, const std::vector<std::vector<uint32_t>>& levels) {
    const auto slot = atlasSlots_.find(tile);
    if (!atlas_ || slot == atlasSlots_.end()) return;
    for (int level = 0; level < kAtlasLevels && level < static_cast<int>(levels.size()); ++level) {
        const int pitch = kAtlasPitch >> level, gutter = kAtlasGutter >> level;
        const int size = kTileSize >> level;
        if (size <= 0 || levels[level].size() != static_cast<size_t>(size) * size) continue;
        RECT area{slot->second.first * pitch, slot->second.second * pitch,
                  slot->second.first * pitch + pitch, slot->second.second * pitch + pitch};
        D3DLOCKED_RECT lock{};
        if (FAILED(atlas_->LockRect(level, &lock, &area, 0))) continue;
        // Tile in the middle, its edge texels repeated into the gutter so that
        // filtering and smaller mips never pick up a neighbouring tile.
        for (int y = 0; y < pitch; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(static_cast<char*>(lock.pBits) + y * lock.Pitch);
            const int sy = std::clamp(y - gutter, 0, size - 1);
            for (int x = 0; x < pitch; ++x)
                row[x] = levels[level][static_cast<size_t>(sy) * size + std::clamp(x - gutter, 0, size - 1)];
        }
        atlas_->UnlockRect(level);
    }
}

bool Renderer::BuildAtlas(const WorldMesh& mesh, const std::map<int, std::vector<uint32_t>>& tilePixels) {
    char ini[MAX_PATH]{};
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    if (char* slash = std::strrchr(ini, '\\')) std::strcpy(slash + 1, "gta2ceiling.ini");
    if (!GetPrivateProfileIntA("graphics", "tile_atlas", 1, ini) || tilePixels.empty()) return false;

    // Each vertex must belong to exactly one tile for its UVs to move.
    std::vector<int> vertexTile(mesh.vertices.size(), -1);
    for (const TileBatch& batch : batches_)
        for (uint32_t i = batch.indexStart; i < batch.indexStart + batch.indexCount; ++i) {
            const uint32_t v = mesh.indices[i];
            if (vertexTile[v] >= 0 && vertexTile[v] != batch.tile) {
                Trace("tile atlas: vertex %u shared by tiles %d and %d", v, vertexTile[v], batch.tile);
                return false;
            }
            vertexTile[v] = batch.tile;
        }

    const int count = static_cast<int>(tilePixels.size());
    const int perRow = std::min(count, 4096 / kAtlasPitch);
    const int rows = (count + perRow - 1) / perRow;
    atlasWidth_ = perRow * kAtlasPitch;
    atlasHeight_ = rows * kAtlasPitch;
    if (atlasHeight_ > 4096) { Trace("tile atlas: %d tiles do not fit", count); return false; }
    if (FAILED(device_->CreateTexture(atlasWidth_, atlasHeight_, kAtlasLevels, 0, D3DFMT_A8R8G8B8,
                                      D3DPOOL_MANAGED, &atlas_, nullptr))) return false;
    gta2ceiling::MipOptions options;
    options.max_levels = kAtlasLevels;
    options.avoid_exact_magenta = true;
    options.bleed_transparent_edges = true;
    int index = 0;
    for (const auto& tile : tilePixels) {
        atlasSlots_[tile.first] = {index % perRow, index / perRow};
        ++index;
        std::vector<gta2ceiling::ArgbMipLevel> levels;
        std::string why;
        if (!gta2ceiling::BuildArgbMipChain(tile.second, kTileSize, kTileSize, levels, why, options)) return false;
        WriteAtlasSlot(tile.first, AtlasLevels(std::move(levels)));
    }

    // Same geometry, UVs moved into each tile's slot.
    std::vector<Vertex> vertices = mesh.vertices;
    for (size_t i = 0; i < vertices.size(); ++i) {
        const auto slot = atlasSlots_.find(vertexTile[i]);
        if (slot == atlasSlots_.end()) continue;
        vertices[i].u = (slot->second.first * kAtlasPitch + kAtlasGutter + vertices[i].u * kTileSize) / atlasWidth_;
        vertices[i].v = (slot->second.second * kAtlasPitch + kAtlasGutter + vertices[i].v * kTileSize) / atlasHeight_;
    }
    const UINT vertexBytes = static_cast<UINT>(vertices.size() * sizeof(Vertex));
    void* dest = nullptr;
    if (FAILED(device_->CreateVertexBuffer(vertexBytes, D3DUSAGE_WRITEONLY, kWorldFvf, D3DPOOL_MANAGED,
                                           &atlasVertices_, nullptr)) ||
        FAILED(atlasVertices_->Lock(0, 0, &dest, 0))) return false;
    memcpy(dest, vertices.data(), vertexBytes);
    atlasVertices_->Unlock();

    // Regroup: one batch per 32-tile cell and render state.
    std::map<std::tuple<int, int, bool, bool>, std::vector<uint32_t>> groups;
    std::map<std::tuple<int, int, bool, bool>, TileBatch> bounds;
    for (const TileBatch& batch : batches_) {
        const float cx = batch.bounded ? 0.5f * (batch.minimum[0] + batch.maximum[0]) : 0.0f;
        const float cz = batch.bounded ? 0.5f * (batch.minimum[2] + batch.maximum[2]) : 0.0f;
        const auto key = std::make_tuple(static_cast<int>(std::floor(cx / 32)), static_cast<int>(std::floor(cz / 32)),
                                         batch.needsAlphaTest, batch.doubleSided);
        auto& out = groups[key];
        out.insert(out.end(), mesh.indices.begin() + batch.indexStart,
                   mesh.indices.begin() + batch.indexStart + batch.indexCount);
        auto found = bounds.find(key);
        if (found == bounds.end()) { bounds[key] = batch; continue; }
        TileBatch& b = found->second;
        b.bounded = b.bounded && batch.bounded;
        for (int axis = 0; axis < 3; ++axis) {
            b.minimum[axis] = std::min(b.minimum[axis], batch.minimum[axis]);
            b.maximum[axis] = std::max(b.maximum[axis], batch.maximum[axis]);
        }
        b.vertexStart = std::min(b.vertexStart, batch.vertexStart);
        const uint32_t end = std::max(found->second.vertexStart + found->second.vertexCount, batch.vertexStart + batch.vertexCount);
        b.vertexCount = end - b.vertexStart;
    }
    std::vector<uint32_t> indices;
    indices.reserve(mesh.indices.size());
    // Opaque groups first, alpha-tested last (fewer state switches).
    for (int pass = 0; pass < 2; ++pass)
        for (auto& group : groups) {
            if (std::get<2>(group.first) != (pass == 1)) continue;
            TileBatch batch = bounds[group.first];
            uint32_t low = UINT32_MAX, high = 0;
            for (uint32_t v : group.second) { low = std::min(low, v); high = std::max(high, v); }
            batch.vertexStart = low;
            batch.vertexCount = high - low + 1;
            batch.indexStart = static_cast<uint32_t>(indices.size());
            batch.indexCount = static_cast<uint32_t>(group.second.size());
            indices.insert(indices.end(), group.second.begin(), group.second.end());
            atlasGroups_.push_back(batch);
        }
    const UINT indexBytes = static_cast<UINT>(indices.size() * sizeof(uint32_t));
    if (FAILED(device_->CreateIndexBuffer(indexBytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX32, D3DPOOL_MANAGED,
                                          &atlasIndices_, nullptr)) ||
        FAILED(atlasIndices_->Lock(0, 0, &dest, 0))) return false;
    memcpy(dest, indices.data(), indexBytes);
    atlasIndices_->Unlock();
    Trace("tile atlas: %d tiles in %dx%d, %zu batches -> %zu draw groups", count, atlasWidth_, atlasHeight_,
          batches_.size(), atlasGroups_.size());
    return true;
}

template <class Draw> void Renderer::ForEachAtlasGroup(const float* cullMatrix, Draw&& draw) {
    for (const TileBatch& group : atlasGroups_) {
        if ((clipWorld_ && !gta2ceiling::RasterBatchIntersects(group, clipX0_, clipZ0_, clipX1_, clipZ1_)) ||
            (cullMatrix && !gta2ceiling::RasterBatchInFrustum(group, cullMatrix))) continue;
        draw(group);
    }
}

void Renderer::UpdateTileTexture(int tile, const uint32_t* pixels) {
    if (!HasTileTexture(tile) || !pixels) return;

    // GTA2 animates a tile - fans, screens, water, traffic lights - by changing
    // the artwork behind a fixed tile number. Writing the new frame into the
    // texture this tile already has, which is what this used to do, gives RTX
    // Remix one texture whose content changes: a single entry in the picker,
    // animating, with no way to replace an individual frame.
    //
    // So each distinct frame gets a texture of its own and the tile is pointed at
    // it instead. A frame that comes round again resolves to the same object and
    // therefore the same hash, so the cost is bounded by how much distinct
    // artwork exists rather than by how long the game runs.
    uint64_t key = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < static_cast<size_t>(kTileSize) * kTileSize; ++i) {
        key ^= pixels[i];
        key *= 0x100000001B3ULL;
    }

    if (atlas_) {
        // Atlas path: the frame's mips are computed once and copied into the
        // tile's slot whenever the animation shows that frame.
        auto frame = atlasFrames_.find(key);
        if (frame == atlasFrames_.end()) {
            std::vector<gta2ceiling::ArgbMipLevel> levels;
            std::string why;
            gta2ceiling::MipOptions options;
            options.max_levels = kAtlasLevels;
            options.avoid_exact_magenta = true;
            options.bleed_transparent_edges = true;
            if (!gta2ceiling::BuildArgbMipChain({pixels, static_cast<size_t>(kTileSize) * kTileSize}, kTileSize,
                                                kTileSize, levels, why, options)) return;
            frame = atlasFrames_.emplace(key, AtlasLevels(std::move(levels))).first;
        }
        WriteAtlasSlot(tile, frame->second);
    }
    const auto existing = tileFrames_.find(key);
    if (existing != tileFrames_.end()) {
        textures_[tile] = existing->second;
        return;
    }

    IDirect3DTexture9* frame = nullptr;
    gta2ceiling::MipOptions mipOptions;
    mipOptions.avoid_exact_magenta = true;
    mipOptions.bleed_transparent_edges = true;
    if (FAILED(gta2ceiling::CreateManagedArgbMipTexture(device_,
            {pixels, static_cast<size_t>(kTileSize) * kTileSize}, kTileSize, kTileSize, &frame, mipOptions))) {
        return;
    }

    tileFrames_[key] = frame;   // owns it from here
    textures_[tile] = frame;
}

void Renderer::CycleCullMode() {
    cullMode_ = cullMode_ == D3DCULL_CCW ? D3DCULL_CW
                : cullMode_ == D3DCULL_CW ? D3DCULL_NONE
                                          : D3DCULL_CCW;
}

void Renderer::ApplyFixedFunctionState() {
    device_->SetRenderState(D3DRS_LIGHTING, FALSE);
    device_->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    device_->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    device_->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device_->SetRenderState(D3DRS_CULLMODE, cullMode_);
    device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device_->SetRenderState(D3DRS_FILLMODE, wireframe_ ? D3DFILL_WIREFRAME : D3DFILL_SOLID);

    device_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device_->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);

    device_->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device_->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    gta2ceiling::ApplyWorldTextureFiltering(device_);
}

void Renderer::MarkDrawsAsScreenUi() {
    if (!device_) return;
    D3DMATRIX identity = {};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    device_->SetTransform(D3DTS_PROJECTION, &identity);
    device_->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
}

void Renderer::MarkDrawsAsNotUi() {
    if (!device_) return;
    // A perspective shape - w taken from z, nothing left in [3][3] - so this can
    // never read as orthographic, whatever the projection was before. In the
    // front end no world has set one, and D3D's default is the identity, which
    // with the overlay's Z writes off would have made every menu draw "UI".
    D3DMATRIX perspective = {};
    perspective._11 = perspective._22 = perspective._33 = 1.0f;
    perspective._34 = 1.0f;
    perspective._44 = 0.0f;
    device_->SetTransform(D3DTS_PROJECTION, &perspective);
}

// The 2D layer the game believes it is painting into. See the note in the
// header for why it is kept rather than cleared.
//
// Everything drawn into it must not look like UI to RTX Remix. The layer is the
// size of the back buffer, which makes it a "primary" target in Remix's eyes,
// and the first UI draw into a primary target is where Remix pastes the whole
// path traced frame - into our layer, over whatever the menu had kept in it.
// With the UI texture list and the pre-transformed switch gone from rtx.conf,
// the projection is the only thing left that could say UI, so it is pinned here.
bool Renderer::BeginUiLayer(bool clearNow) {
    if (!device_ || savedTarget_) return false;
    if (!uiTexture_) {
        if (FAILED(device_->CreateTexture(width_, height_, 1, D3DUSAGE_RENDERTARGET,
                                          D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &uiTexture_,
                                          nullptr))) {
            uiTexture_ = nullptr;
            return false;
        }
        if (FAILED(uiTexture_->GetSurfaceLevel(0, &uiSurface_))) {
            uiTexture_->Release();
            uiTexture_ = nullptr;
            uiSurface_ = nullptr;
            return false;
        }
        uiClearPending_ = true;
    }
    if (FAILED(device_->GetRenderTarget(0, &savedTarget_))) {
        savedTarget_ = nullptr;
        return false;
    }
    // UI is a non-multisampled texture. Retaining the main MSAA depth/stencil
    // attachment here makes the target invalid on D3D9/DXVK.
    const HRESULT depthResult = device_->GetDepthStencilSurface(&savedDepth_);
    if ((FAILED(depthResult) && depthResult != D3DERR_NOTFOUND) ||
        FAILED(device_->SetDepthStencilSurface(nullptr))) {
        if (savedDepth_) savedDepth_->Release();
        savedDepth_ = nullptr;
        savedTarget_->Release();
        savedTarget_ = nullptr;
        return false;
    }
    if (FAILED(device_->SetRenderTarget(0, uiSurface_))) {
        device_->SetDepthStencilSurface(savedDepth_);
        if (savedDepth_) savedDepth_->Release();
        savedDepth_ = nullptr;
        savedTarget_->Release();
        savedTarget_ = nullptr;
        return false;
    }
    MarkDrawsAsNotUi();
    // Transparent, not black: the world is drawn underneath this, so everywhere
    // the game has not painted has to let it through. The game's own clear is
    // opaque black because in its model there is nothing underneath.
    if (clearNow || uiClearPending_) {
        device_->Clear(0, nullptr, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
        uiClearPending_ = false;
    }
    return true;
}

void Renderer::EndUiLayer(bool composite) {
    if (!device_ || !savedTarget_) return;
    device_->SetRenderTarget(0, savedTarget_);
    savedTarget_->Release();
    savedTarget_ = nullptr;
    device_->SetDepthStencilSurface(savedDepth_);
    if (savedDepth_) savedDepth_->Release();
    savedDepth_ = nullptr;
    if (!composite) return;
    if (!uiTexture_) return;

    struct Composite {
        float x, y, z, rhw;
        float u, v;
    };
    const float w = static_cast<float>(width_);
    const float h = static_cast<float>(height_);
    const Composite quad[4] = {{-0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f},
                               {w - 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f},
                               {w - 0.5f, h - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f},
                               {-0.5f, h - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f}};

    device_->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    device_->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device_->SetRenderState(D3DRS_LIGHTING, FALSE);
    device_->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device_->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    // The layer is premultiplied (see Overlay::Flush): its colour is added as it
    // is, and its alpha says how much of the frame underneath to keep.
    device_->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device_->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    device_->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    device_->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    // And this quad is the frame's UI: the first draw Remix takes for UI, so the
    // path traced image goes down on the back buffer here and the layer is laid
    // over it. Without this the composite is just another pre-transformed draw,
    // which NVIDIA's 1.5.2 rasterizes before the path traced image and then
    // covers with it at Present - the UI that came and went.
    MarkDrawsAsScreenUi();
    device_->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device_->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device_->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device_->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device_->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device_->SetTexture(0, uiTexture_);
    device_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device_->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device_->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    DynamicDraw::Instance().Draw(device_, D3DPT_TRIANGLEFAN, 2, quad, sizeof(Composite));

    device_->SetTexture(0, nullptr);
    device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device_->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    device_->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
}

bool Renderer::BeginFrame(bool transparent) {
    stats_ = RenderStats{};
    if (!device_ || !sceneTarget_ || frameOpen_) return false;
    transparentFrame_=transparent;
    device_->SetTexture(0,nullptr);
    device_->SetPixelShader(nullptr);
    device_->SetVertexShader(nullptr);
    device_->SetDepthStencilSurface(nullptr);
    if(FAILED(device_->SetRenderTarget(0,sceneTarget_)) ||
       FAILED(device_->SetDepthStencilSurface(sceneDepth_))) return false;
    const D3DVIEWPORT9 full{0,0,static_cast<DWORD>(width_),static_cast<DWORD>(height_),0,1};
    device_->SetViewport(&full);
    device_->SetRenderState(D3DRS_COLORWRITEENABLE,0xf);
    device_->SetRenderState(D3DRS_STENCILENABLE,FALSE);
    device_->SetRenderState(D3DRS_CLIPPLANEENABLE,0);
    // Real coverage is resolved against transparent black. Pink exists only in
    // the final non-MSAA output, so no geometry can blend with the host key.
    device_->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL,
                   transparent?0:D3DCOLOR_XRGB(0,0,0),1.0f,0);
    frameOpen_=SUCCEEDED(device_->BeginScene());
    return frameOpen_;
}

bool Renderer::FinishFrame() {
    if(!device_ || !frameOpen_) return false;
    frameOpen_=false;
    if(FAILED(device_->EndScene())) return false;
    device_->SetDepthStencilSurface(nullptr);
    if(FAILED(device_->SetRenderTarget(0,presentTarget_))) return false;
    // D3D9 forbids StretchRect inside an active scene.
    if(sceneTarget_!=resolvedTarget_ && FAILED(device_->StretchRect(sceneTarget_,nullptr,
            resolvedTarget_,nullptr,D3DTEXF_NONE))) return false;
    if(FAILED(device_->BeginScene())) return false;
    device_->SetVertexShader(nullptr);
    device_->SetPixelShader(presentShader_);
    const float mode[]{transparentFrame_?1.f:0.f,0,0,0};
    device_->SetPixelShaderConstantF(0,mode,1);
    const D3DVIEWPORT9 full{0,0,static_cast<DWORD>(presentWidth_),static_cast<DWORD>(presentHeight_),0,1};
    device_->SetViewport(&full);
    device_->SetRenderState(D3DRS_ZENABLE,FALSE);
    device_->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
    device_->SetRenderState(D3DRS_STENCILENABLE,FALSE);
    device_->SetRenderState(D3DRS_CLIPPLANEENABLE,0);
    device_->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
    device_->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
    device_->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);
    device_->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);
    device_->SetRenderState(D3DRS_DITHERENABLE,FALSE);
    device_->SetRenderState(D3DRS_FOGENABLE,FALSE);
    device_->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    device_->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID);
    device_->SetRenderState(D3DRS_COLORWRITEENABLE,0xf);
    // 1:1 stays exact. A scaled scene is premultiplied (RGB weighted by
    // coverage), so bilinear filtering before the coverage divide is correct
    // and never creates a near-key colour.
    const D3DTEXTUREFILTERTYPE presentFilter=
        (width_==presentWidth_ && height_==presentHeight_) ? D3DTEXF_POINT : D3DTEXF_LINEAR;
    device_->SetSamplerState(0,D3DSAMP_MINFILTER,presentFilter);
    device_->SetSamplerState(0,D3DSAMP_MAGFILTER,presentFilter);
    device_->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
    device_->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
    device_->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
    device_->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE);
    device_->SetTexture(0,resolvedTexture_);
    device_->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);
    struct Pixel {float x,y,z,rhw,u,v;};
    const float w=static_cast<float>(presentWidth_),h=static_cast<float>(presentHeight_);
    const Pixel quad[]{{-.5f,-.5f,0,1,0,0},{w-.5f,-.5f,0,1,1,0},
                        {w-.5f,h-.5f,0,1,1,1},{-.5f,h-.5f,0,1,0,1}};
    const HRESULT drawn=DynamicDraw::Instance().Draw(device_,D3DPT_TRIANGLEFAN,2,quad,sizeof(Pixel));
    if (stampEnabled_) {
        // Sync block plus green marker, see CeilingBridge::StampSync. A second
        // copy in the bottom-right corner lets the host detect an image that
        // reached it torn (top from one game frame, bottom from the next).
        const LONG pw=static_cast<LONG>(presentWidth_),ph=static_cast<LONG>(presentHeight_);
        const D3DRECT blocks[]{{0,0,8,8},{pw-16,ph-8,pw-8,ph}}, markers[]{{8,0,16,8},{pw-8,ph-8,pw,ph}};
        device_->Clear(2,blocks,D3DCLEAR_TARGET,stampColour_,1,0);
        device_->Clear(2,markers,D3DCLEAR_TARGET,0xFF00FF00u,1,0);
    }
    stampEnabled_=false;
    device_->SetTexture(0,nullptr);
    device_->SetPixelShader(nullptr);
    const HRESULT ended=device_->EndScene();
    return SUCCEEDED(drawn)&&SUCCEEDED(ended);
}

void Renderer::EndFrame() {
    if (!FinishFrame()) return;
    CaptureDiagnosticFrame();
    const HRESULT result=device_->Present(nullptr, nullptr, nullptr, nullptr);
    if (result!=lastPresent_) Trace("Present hr=0x%08lX",static_cast<unsigned long>(result));
    lastPresent_=result;
}

void Renderer::CaptureDiagnosticFrame() {
    // Explicit opt-in, at three separate moments. Private game images stay
    // beside the user's game copy and never enter the public mod package.
    ++diagnosticFrame_;
    if (diagnosticFrame_!=180 && diagnosticFrame_!=720 && diagnosticFrame_!=2160) return;
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr,path,MAX_PATH);
    char* leaf=std::strrchr(path,'\\');
    if (!leaf) return;
    std::strcpy(leaf+1,"gta2ceiling.ini");
    if (!GetPrivateProfileIntA("diagnostics","capture_frames",0,path)) return;
    IDirect3DSurface9* target=nullptr;
    IDirect3DSurface9* resolved=nullptr;
    IDirect3DSurface9* readback=nullptr;
    D3DSURFACE_DESC description{};
    HRESULT result=device_->GetRenderTarget(0,&target);
    if (SUCCEEDED(result)) result=target->GetDesc(&description);
    if (SUCCEEDED(result) && description.MultiSampleType != D3DMULTISAMPLE_NONE) {
        result=device_->CreateRenderTarget(description.Width, description.Height, description.Format,
                                           D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr);
        if (SUCCEEDED(result)) result=device_->StretchRect(target, nullptr, resolved, nullptr, D3DTEXF_NONE);
    }
    if (SUCCEEDED(result)) result=device_->CreateOffscreenPlainSurface(description.Width,description.Height,
        description.Format,D3DPOOL_SYSTEMMEM,&readback,nullptr);
    if (SUCCEEDED(result)) result=device_->GetRenderTargetData(resolved ? resolved : target,readback);
    if (SUCCEEDED(result) && (description.Format==D3DFMT_A8R8G8B8 || description.Format==D3DFMT_X8R8G8B8)) {
        D3DLOCKED_RECT locked{};
        result=readback->LockRect(&locked,nullptr,D3DLOCK_READONLY);
        if (SUCCEEDED(result)) {
            std::snprintf(leaf+1,MAX_PATH-static_cast<size_t>(leaf+1-path),"gta2ceiling-frame-%u.bmp",diagnosticFrame_);
            FILE* file=std::fopen(path,"wb");
            if (file) {
                BITMAPFILEHEADER header{}; BITMAPINFOHEADER info{};
                header.bfType=0x4D42; header.bfOffBits=sizeof(header)+sizeof(info);
                header.bfSize=header.bfOffBits+description.Width*description.Height*4;
                info.biSize=sizeof(info); info.biWidth=description.Width; info.biHeight=-static_cast<LONG>(description.Height);
                info.biPlanes=1; info.biBitCount=32; info.biCompression=BI_RGB;
                std::fwrite(&header,sizeof(header),1,file); std::fwrite(&info,sizeof(info),1,file);
                for (UINT row=0;row<description.Height;++row)
                    std::fwrite(static_cast<const unsigned char*>(locked.pBits)+row*locked.Pitch,4,description.Width,file);
                std::fclose(file);
                Trace("private GPU diagnostic saved: %s",path);
            }
            readback->UnlockRect();
        }
    }
    if (FAILED(result)) Trace("GPU diagnostic failed: 0x%08lX",static_cast<unsigned long>(result));
    if (readback) readback->Release();
    if (resolved) resolved->Release();
    if (target) target->Release();
}

void Renderer::DrawWorldShadows(const Camera& camera, const float* levels, int levelCount) {
    if (!device_ || !vertexBuffer_ || !levels || levelCount <= 0) return;
    DWORD stencilEnabled = FALSE;
    device_->GetRenderState(D3DRS_STENCILENABLE, &stencilEnabled);
    if (!stencilEnabled) return;  // needs the room mask

    // Save the state the city pass relies on.
    DWORD stencilFunc = 0, stencilMask = 0, stencilRef = 0, stencilPass = 0, stencilWrite = 0;
    DWORD colourWrite = 0, clipEnable = 0, zWrite = 0, zFunc = 0, zEnable = 0;
    device_->GetRenderState(D3DRS_STENCILFUNC, &stencilFunc);
    device_->GetRenderState(D3DRS_STENCILMASK, &stencilMask);
    device_->GetRenderState(D3DRS_STENCILREF, &stencilRef);
    device_->GetRenderState(D3DRS_STENCILPASS, &stencilPass);
    device_->GetRenderState(D3DRS_STENCILWRITEMASK, &stencilWrite);
    device_->GetRenderState(D3DRS_COLORWRITEENABLE, &colourWrite);
    device_->GetRenderState(D3DRS_CLIPPLANEENABLE, &clipEnable);
    device_->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
    device_->GetRenderState(D3DRS_ZFUNC, &zFunc);
    device_->GetRenderState(D3DRS_ZENABLE, &zEnable);
    float savedPlane[4]{};
    device_->GetClipPlane(0, savedPlane);
    const Mat4 view = camera.ViewMatrix();

    SetMatrix(device_, D3DTS_WORLD, Identity());
    SetMatrix(device_, D3DTS_PROJECTION,
              camera.ProjectionMatrix(static_cast<float>(width_) / static_cast<float>(height_)));
    device_->SetTexture(0, nullptr);
    device_->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(110, 8, 10, 22));
    device_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
    device_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
    device_->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device_->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device_->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device_->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device_->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device_->SetRenderState(D3DRS_CLIPPING, TRUE);
    device_->SetRenderState(D3DRS_STENCILMASK, 0x41);
    device_->SetRenderState(D3DRS_STENCILWRITEMASK, 0x40);
    device_->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_INVERT);
    device_->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    device_->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
    device_->SetFVF(kWorldFvf);
    device_->SetStreamSource(0, atlas_ ? atlasVertices_ : vertexBuffer_, 0, sizeof(Vertex));
    device_->SetIndices(atlas_ ? atlasIndices_ : indexBuffer_);

    // Directional "afternoon sun": shadow offset per block of height.
    constexpr float kSunX = 0.55f, kSunZ = 0.35f;
    // Everything above `level` is flattened onto the plane y = plane.
    auto flatten = [&](float level, float plane) {
        Mat4 shadow = Identity();
        shadow.m[1][0] = kSunX; shadow.m[1][1] = 0.0f; shadow.m[1][2] = kSunZ;
        shadow.m[3][0] = -kSunX * level; shadow.m[3][1] = plane; shadow.m[3][2] = -kSunZ * level;
        Mat4 shadowView{};
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k) shadowView.m[r][c] += shadow.m[r][k] * view.m[k][c];
        SetMatrix(device_, D3DTS_VIEW, shadowView);
    };
    // Shadow casters: neighbouring visible groups are contiguous in the index
    // buffer, so they merge into a few long draws (render state is the same).
    std::vector<TileBatch> runs;
    for (const TileBatch& batch : atlas_ ? atlasGroups_ : batches_) {
        if (clipWorld_ && !gta2ceiling::RasterBatchIntersects(batch, clipX0_ - 4, clipZ0_ - 4, clipX1_ + 4, clipZ1_ + 4))
            continue;
        if (!runs.empty() && runs.back().indexStart + runs.back().indexCount == batch.indexStart) {
            TileBatch& run = runs.back();
            const uint32_t end = (std::max)(run.vertexStart + run.vertexCount, batch.vertexStart + batch.vertexCount);
            run.vertexStart = (std::min)(run.vertexStart, batch.vertexStart);
            run.vertexCount = end - run.vertexStart;
            run.indexCount += batch.indexCount;
        } else {
            runs.push_back(batch);
        }
    }
    // A zoomed-out city puts the whole map in view: every shadow pass redraws
    // all of it with heavy overdraw (measured: GPU 85-90 %, dropped frames).
    // Above this size the shadows are left out until the view is smaller.
    constexpr uint64_t kMaxShadowTriangles = 70000;
    uint64_t shadowTriangles = 0;
    for (const TileBatch& run : runs) shadowTriangles += run.indexCount / 3;
    static int shadowsWereSkipped = -1;
    const int skip = shadowTriangles > kMaxShadowTriangles ? 1 : 0;
    if (skip != shadowsWereSkipped) {
        shadowsWereSkipped = skip;
        Trace("building shadows %s (%llu triangles in view)", skip ? "paused" : "on",
              static_cast<unsigned long long>(shadowTriangles));
    }
    if (skip) levelCount = 0;
    auto casters = [&]() {
        for (const TileBatch& run : runs) {
            device_->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, run.vertexStart, run.vertexCount,
                                          run.indexStart, run.indexCount / 3);
            stats_.drawCalls++;
        }
    };
    // Each ground level receives only the shadow of what stands above it, and
    // only on pixels whose visible surface really lies at that level: the
    // shadow is never a coplanar decal fighting the street for depth.
    //  A: mark pixels whose scene is at or below level+band (stencil only),
    //  C: darken marked pixels whose scene is above level-band, once.
    // Levels run from low to high; a mark a higher level leaves on lower
    // ground is never read again (bit 6 is not used after this pass).
    constexpr float kBand = 0.12f;
    for (int i = 0; i < levelCount; ++i) {
        const float level = levels[i];
        if (!std::isfinite(level)) continue;
        // Clip planes are in world space (before flattening): only geometry
        // clearly above this level casts, so the ground does not shade itself.
        const float above[4] = {0.0f, 1.0f, 0.0f, -(level + 0.15f)};
        device_->SetClipPlane(0, above);
        device_->SetRenderState(D3DRS_CLIPPLANEENABLE, 1);
        device_->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        device_->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);

        flatten(level, level + kBand);                   // A
        device_->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
        device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device_->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        device_->SetRenderState(D3DRS_STENCILREF, 0x01);
        casters();

        flatten(level, level - kBand);                   // C
        device_->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device_->SetRenderState(D3DRS_COLORWRITEENABLE,
            D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        device_->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATER);
        device_->SetRenderState(D3DRS_STENCILREF, 0x41);
        casters();
    }

    device_->SetClipPlane(0, savedPlane);
    device_->SetRenderState(D3DRS_CLIPPLANEENABLE, clipEnable);
    device_->SetRenderState(D3DRS_STENCILFUNC, stencilFunc);
    device_->SetRenderState(D3DRS_STENCILMASK, stencilMask);
    device_->SetRenderState(D3DRS_STENCILREF, stencilRef);
    device_->SetRenderState(D3DRS_STENCILPASS, stencilPass);
    device_->SetRenderState(D3DRS_STENCILWRITEMASK, stencilWrite);
    device_->SetRenderState(D3DRS_COLORWRITEENABLE, colourWrite);
    device_->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);
    device_->SetRenderState(D3DRS_ZFUNC, zFunc);
    device_->SetRenderState(D3DRS_ZENABLE, zEnable);
    device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device_->SetRenderState(D3DRS_CULLMODE, cullMode_);
    device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    SetMatrix(device_, D3DTS_VIEW, view);
}

namespace {
// General 4x4 inverse for row-vector matrices (a view matrix carrying the
// city's scale and mirror is not rigid).
bool InvertMatrix(const Mat4& in, Mat4* out) {
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? in.m[r][c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
        if (std::fabs(a[pivot][c]) < 1e-12) return false;
        if (pivot != c) for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[pivot][k]);
        const double scale = 1.0 / a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] *= scale;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double factor = a[r][c];
            if (factor != 0.0) for (int k = 0; k < 8; ++k) a[r][k] -= factor * a[c][k];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out->m[r][c] = static_cast<float>(a[r][c + 4]);
    return true;
}
}  // namespace

bool Renderer::BeginCircleMask(const Mat4& view) {
    if (!circleMask_ || !device_) return false;
    if (!maskTexture_) {
        // A unit disc, alpha 1 inside, 0 outside, with a one-texel soft edge
        // so the alpha test lands exactly on the circle.
        constexpr int kSize = 1024;
        if (FAILED(device_->CreateTexture(kSize, kSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &maskTexture_,
                                          nullptr))) {
            maskTexture_ = nullptr;
            return false;
        }
        D3DLOCKED_RECT lock{};
        if (SUCCEEDED(maskTexture_->LockRect(0, &lock, nullptr, 0))) {
            for (int y = 0; y < kSize; ++y) {
                auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lock.pBits) + y * lock.Pitch);
                for (int x = 0; x < kSize; ++x) {
                    const float u = (x + 0.5f) / kSize - 0.5f, v = (y + 0.5f) / kSize - 0.5f;
                    const float inside = (0.5f - std::sqrt(u * u + v * v)) * kSize + 0.5f;
                    const float alpha = inside <= 0.0f ? 0.0f : (inside >= 1.0f ? 1.0f : inside);
                    row[x] = (static_cast<uint32_t>(alpha * 255.0f + 0.5f) << 24) | 0x00FFFFFFu;
                }
            }
            maskTexture_->UnlockRect(0);
        }
    }
    // Stage 1 looks the mask up at each fragment's world position: camera
    // space position -> world (inverse view) -> mask coordinates.
    Mat4 toWorld;
    if (!InvertMatrix(view, &toWorld)) return false;
    const float k = 0.5f / maskRadius_;
    Mat4 toMask{};
    toMask.m[0][0] = k;                       // u from world x
    toMask.m[2][1] = k;                       // v from world z
    toMask.m[3][0] = 0.5f - maskX_ * k;
    toMask.m[3][1] = 0.5f - maskZ_ * k;
    toMask.m[3][3] = 1.0f;
    Mat4 transform{};
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
        for (int i = 0; i < 4; ++i) transform.m[r][c] += toWorld.m[r][i] * toMask.m[i][c];
    SetMatrix(device_, D3DTS_TEXTURE1, transform);
    device_->SetTexture(1, maskTexture_);
    device_->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION | 1);
    device_->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
    device_->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device_->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
    device_->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device_->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device_->SetTextureStageState(1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
    device_->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device_->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device_->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device_->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device_->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device_->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    // Opaque tiles take their alpha from here (1) rather than from artwork
    // whose alpha was never meant to be tested.
    device_->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFFu);
    return true;
}

void Renderer::EndCircleMask() {
    device_->SetTexture(1, nullptr);
    device_->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device_->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    device_->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
    device_->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device_->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
}

void Renderer::DrawWorld(const Camera& camera) {
    // Nothing to draw until the game has a world loaded; the frame is still
    // cleared and presented so menus have something to sit on.
    if (!device_ || !vertexBuffer_) return;

    const Mat4 cullView=camera.ViewMatrix();
    const Mat4 cullProjection=camera.ProjectionMatrix(static_cast<float>(width_)/height_);
    Mat4 cullMatrix{};
    for(int r=0;r<4;++r) for(int c=0;c<4;++c)
        for(int k=0;k<4;++k) cullMatrix.m[r][c]+=cullView.m[r][k]*cullProjection.m[k][c];

    ApplyFixedFunctionState();
    SetMatrix(device_, D3DTS_WORLD, Identity());
    SetMatrix(device_, D3DTS_VIEW, camera.ViewMatrix());
    SetMatrix(device_, D3DTS_PROJECTION,
              camera.ProjectionMatrix(static_cast<float>(width_) / static_cast<float>(height_)));

    device_->SetFVF(kWorldFvf);
    const bool mask = BeginCircleMask(cullView);
    if (atlas_) {
        device_->SetStreamSource(0, atlasVertices_, 0, sizeof(Vertex));
        device_->SetIndices(atlasIndices_);
        device_->SetTexture(0, atlas_);
        const bool blending = g_alphaMode == AlphaMode::Blend;
        device_->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        device_->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device_->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        int alphaState = -1, sidedState = -1;
        ForEachAtlasGroup(&cullMatrix.m[0][0], [&](const TileBatch& group) {
            if (alphaState != (group.needsAlphaTest ? 1 : 0)) {
                alphaState = group.needsAlphaTest ? 1 : 0;
                device_->SetRenderState(D3DRS_ALPHATESTENABLE, (alphaState || mask) ? TRUE : FALSE);
                device_->SetRenderState(D3DRS_ALPHAREF, alphaState ? (blending ? 1u : g_alphaRef) : 128u);
                device_->SetRenderState(D3DRS_ALPHABLENDENABLE, (blending && alphaState) ? TRUE : FALSE);
                if (mask) device_->SetTextureStageState(0, D3DTSS_ALPHAARG1, alphaState ? D3DTA_TEXTURE : D3DTA_TFACTOR);
            }
            if (sidedState != (group.doubleSided ? 1 : 0)) {
                sidedState = group.doubleSided ? 1 : 0;
                device_->SetRenderState(D3DRS_CULLMODE, sidedState ? D3DCULL_NONE : cullMode_);
            }
            device_->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, group.vertexStart, group.vertexCount,
                                          group.indexStart, group.indexCount / 3);
            stats_.drawCalls++;
            stats_.triangles += static_cast<int>(group.indexCount / 3);
        });
        device_->SetRenderState(D3DRS_CULLMODE, cullMode_);
        device_->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        if (mask) EndCircleMask();
        return;
    }
    device_->SetStreamSource(0, vertexBuffer_, 0, sizeof(Vertex));
    device_->SetIndices(indexBuffer_);

    bool alphaTestEnabled = false;
    bool doubleSided = false;   // see TileBatch::doubleSided
    device_->SetRenderState(D3DRS_ALPHATESTENABLE, mask ? TRUE : FALSE);
    device_->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    device_->SetRenderState(D3DRS_ALPHAREF, mask ? 128u : g_alphaRef);
    if (mask) device_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
    device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device_->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device_->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    for (const TileBatch& batch : batches_) {
        if ((clipWorld_ && !gta2ceiling::RasterBatchIntersects(batch,clipX0_,clipZ0_,clipX1_,clipZ1_)) ||
            !gta2ceiling::RasterBatchInFrustum(batch,&cullMatrix.m[0][0])) continue;
        if (batch.needsAlphaTest != alphaTestEnabled) {
            alphaTestEnabled = batch.needsAlphaTest;
            const bool blending = g_alphaMode == AlphaMode::Blend;
            // Blend mode still keeps a very low alpha test: without it the fully
            // transparent texels are still rasterised and still write depth,
            // which puts an invisible wall in front of everything behind them.
            device_->SetRenderState(D3DRS_ALPHATESTENABLE, (alphaTestEnabled || mask) ? TRUE : FALSE);
            device_->SetRenderState(D3DRS_ALPHAREF, alphaTestEnabled ? (blending ? 1u : g_alphaRef) : 128u);
            device_->SetRenderState(D3DRS_ALPHABLENDENABLE,
                                    (blending && alphaTestEnabled) ? TRUE : FALSE);
            if (mask) device_->SetTextureStageState(0, D3DTSS_ALPHAARG1,
                                                    alphaTestEnabled ? D3DTA_TEXTURE : D3DTA_TFACTOR);
        }
        if (batch.doubleSided != doubleSided) {
            doubleSided = batch.doubleSided;
            device_->SetRenderState(D3DRS_CULLMODE, doubleSided ? D3DCULL_NONE : cullMode_);
        }
        device_->SetTexture(0, textures_[batch.tile]);
        device_->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, batch.vertexStart, batch.vertexCount,
                                      batch.indexStart, batch.indexCount / 3);
        stats_.drawCalls++;
        stats_.triangles += static_cast<int>(batch.indexCount / 3);
    }
    if (doubleSided) device_->SetRenderState(D3DRS_CULLMODE, cullMode_);
    if (mask) EndCircleMask();
}

}  // namespace gta2
