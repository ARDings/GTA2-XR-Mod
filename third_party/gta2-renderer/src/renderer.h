// D3D9 fixed-function renderer for GTA2 world geometry.
//
// Deliberately fixed-function and untransformed: RTX Remix path traces draws
// whose vertices are in object/world space and whose projection is perspective,
// and skips anything using pre-transformed (D3DFVF_XYZRHW) vertices. Lighting
// and shadowing are left to Remix.
#pragma once

#include <d3d9.h>

#include <map>
#include <string>
#include <vector>

#include "camera.h"
#include "gta2_style.h"
#include "world_mesh.h"

namespace gta2 {

struct RenderStats {
    int drawCalls = 0;
    int triangles = 0;
};

// Optional trace sink for device bring-up. The in-process DLL points it at
// gta2dx9.log so each D3D9 call can be timestamped and lined up against RTX
// Remix's own bridge logs; the standalone viewer leaves it null.
void SetRendererTrace(void (*sink)(const char*));

// How a cutout's edge is resolved. Alpha test is the default and is what a path
// tracer wants - a blended surface has no single depth for a ray to hit - but
// now that transparent texels carry a sensible colour (see alpha_bleed.h) it is
// worth being able to compare them by eye rather than by argument.
enum class AlphaMode { Test, Blend };
void SetAlphaMode(AlphaMode mode);
AlphaMode GetAlphaMode();
void SetAlphaRef(int ref);   // 1..254, the alpha-test cutoff
int  GetAlphaRef();

class Renderer {
public:
    ~Renderer();

    bool Initialize(HWND window, int width, int height, std::string* error);
    bool UploadWorld(const WorldMesh& mesh, const Style& style, std::string* error);

    // Split so the screen-space pass (HUD, menus, sprites) can be drawn between
    // the world and the present.
    bool BeginFrame(bool transparent = false);
    void DrawWorld(const Camera& camera);
    void SetWorldClip(float minX,float minZ,float maxX,float maxZ) {
        clipX0_=minX; clipZ0_=minZ; clipX1_=maxX; clipZ1_=maxZ; clipWorld_=true;
    }
    void ClearWorldClip() { clipWorld_=false; }
    // GTA2Ceiling holo cut: cut the world mesh exactly at a circle (x, z in
    // tiles) instead of at the six clip planes around it, so the cut follows
    // the round field and can be closed accurately (WorldView::DrawHoloCut).
    // Per pixel, through a second texture stage: a circle mask looked up at
    // each fragment's own world position, then the alpha test.
    void SetCircleMask(bool on, float x = 0, float z = 0, float radius = 0) {
        circleMask_ = on && radius > 0.0f; maskX_ = x; maskZ_ = z; maskRadius_ = radius;
    }
    void EndFrame();
    // Finish/resolve without presenting, also used by actual GPU readback tests.
    bool FinishFrame();

    // GTA2 animates a tile - ceiling fans, screens, water - by changing the
    // artwork behind its number while the map itself stays put, so the world
    // mesh never changes but its textures have to be refreshed.
    bool HasTileTexture(int tile) const {
        return tile >= 0 && tile < static_cast<int>(textures_.size()) && textures_[tile] != nullptr;
    }
    void UpdateTileTexture(int tile, const uint32_t* pixels);
    int TileTextureCount() const { return static_cast<int>(textures_.size()); }

    bool HasWorld() const { return vertexBuffer_ != nullptr; }

    // Drop the level's geometry without touching the device, the window or the
    // tile textures. The game frees its map when it returns to the menu, and
    // without this the last level goes on being drawn - and path traced - behind
    // the menu.
    void ReleaseWorld();

    // --- The 2D layer ------------------------------------------------------
    //
    // GTA2's menu is an incremental painter. gta2.exe!0x00461960 draws the
    // menu, flips, and then clears the screen *only* if one of five repaint
    // flags is set (0x5EAD9C, 0x5EAD8C, 0x5EAD5F, 0x5EAD67, 0x5EAD59) - and
    // otherwise returns without clearing. So the front end sends its background
    // once, its animated panel now and then, and its text every frame, on the
    // understanding that whatever it drew before is still on screen.
    //
    // A pass that rebuilds from an empty list every frame therefore shows each
    // of those for exactly the frame it arrives on. These give the 2D stream the
    // surface it thinks it has: an offscreen target that is kept between frames
    // and cleared only when the game says to.
    bool BeginUiLayer(bool clearNow);
    void EndUiLayer(bool composite = true);
    IDirect3DTexture9* UiTexture() const { return uiTexture_; }
    void NoteScreenClear() { uiClearPending_ = true; }
    bool HasUiLayer() const { return uiTexture_ != nullptr; }

    // How RTX Remix tells UI from world, on NVIDIA's runtime and Remix Plus alike
    // (d3d9_rtx.cpp, isRenderingUI): a fixed-function draw with an orthographic
    // projection - D3DTS_PROJECTION[3][3] == 1 - and Z writes off. The first such
    // draw of a frame is where the path traced image is put down; everything
    // after it is rasterized on top, with our own blend and texture states.
    //
    // Pre-transformed vertices ignore the projection entirely, so this changes
    // what Remix concludes and nothing about what is drawn. Screen: the draws
    // that follow are the frame's UI. Layer: they are not, whatever the target.
    void MarkDrawsAsScreenUi();
    void MarkDrawsAsNotUi();

    // Scene (render) size. The presented back buffer keeps the window size;
    // the scene may be rendered smaller/larger and is filtered on present.
    int Width() const { return width_; }
    int Height() const { return height_; }
    int RenderScalePercent() const { return renderScale_; }
    UINT MsaaSamples() const { return static_cast<UINT>(sceneSamples_); }
    // Outside a frame only. Rebuilds the scene targets; falls back to 100 %
    // without MSAA if the device refuses. Returns the effective settings.
    bool ApplyQuality(int renderScalePercent, UINT msaaSamples);
    // Written after the final present pass, so scaling never filters it.
    void SetPresentStamp(bool enabled, DWORD colour) { stampEnabled_=enabled; stampColour_=colour; }
    // GTA2Ceiling: sun shadows of the buildings onto the visible ground levels
    // (game heights). Stencil-banded, so they never z-fight the street. Call
    // right after DrawWorld, inside the room stencil mask; restores the state.
    void DrawWorldShadows(const Camera& camera, const float* levels, int levelCount);

    // Releases the device and every resource. Must run before the game tears its
    // window down: a live device on a destroyed window keeps the process alive.
    void Shutdown();

    void ToggleWireframe() { wireframe_ = !wireframe_; }
    void CycleCullMode();
    const RenderStats& Stats() const { return stats_; }
    IDirect3DDevice9* Device() const { return device_; }
    HRESULT DeviceStatus() const { return device_ ? device_->TestCooperativeLevel() : D3DERR_DEVICELOST; }
    HRESULT ResetDevice();

private:
    friend struct RendererOwnershipTestAccess;
    bool clipWorld_=false;
    float clipX0_=0,clipZ0_=0,clipX1_=0,clipZ1_=0;
    void ApplyFixedFunctionState();
    void ReleaseResources();
    void ReleaseUi();
    HRESULT CreateSceneTargets();
    HRESULT CreateSceneTargetsFor(UINT requestedSamples);
    // GTA2Ceiling performance: every tile in one mip-mapped atlas, batches
    // regrouped per 32-tile cell and render state, so a stereo pair needs a
    // few dozen draw calls instead of one per tile texture in view.
    bool BuildAtlas(const WorldMesh& mesh, const std::map<int, std::vector<uint32_t>>& tilePixels);
    // levels[k]: (64 >> k)^2 straight-alpha ARGB texels of mip level k.
    void WriteAtlasSlot(int tile, const std::vector<std::vector<uint32_t>>& levels);
    void ReleaseAtlas();
    template <class Draw> void ForEachAtlasGroup(const float* cullMatrix, Draw&& draw);
    void ApplyScale(int percent);
    void ReleaseSceneTargets();
    void CaptureDiagnosticFrame();

    IDirect3D9* d3d_ = nullptr;
    D3DPRESENT_PARAMETERS presentParameters_{};
    HRESULT lastPresent_ = D3D_OK;
    unsigned diagnosticFrame_ = 0;
    IDirect3DDevice9* device_ = nullptr;
    IDirect3DSurface9* presentTarget_ = nullptr;
    IDirect3DSurface9* sceneTarget_ = nullptr;
    IDirect3DSurface9* sceneDepth_ = nullptr;
    IDirect3DTexture9* resolvedTexture_ = nullptr;
    IDirect3DSurface9* resolvedTarget_ = nullptr;
    IDirect3DPixelShader9* presentShader_ = nullptr;
    D3DMULTISAMPLE_TYPE sceneSamples_ = D3DMULTISAMPLE_NONE;
    bool transparentFrame_ = false;
    bool frameOpen_ = false;
    IDirect3DTexture9* uiTexture_ = nullptr;
    IDirect3DSurface9* uiSurface_ = nullptr;
    IDirect3DSurface9* savedTarget_ = nullptr;
    IDirect3DSurface9* savedDepth_ = nullptr;
    bool uiClearPending_ = true;

    IDirect3DVertexBuffer9* vertexBuffer_ = nullptr;
    IDirect3DIndexBuffer9* indexBuffer_ = nullptr;
    std::vector<IDirect3DTexture9*> textures_;  // borrowed active tile slots
    // Keep initial artwork owned even after an animated slot selects a frame.
    // Otherwise the first animation update loses the only pointer to it.
    std::vector<IDirect3DTexture9*> initialTextures_;
    // Every distinct frame of tile artwork, keyed by content, and the owner of
    // all of them. An animated tile swaps textures_[tile] to point at a
    // different one of these rather than rewriting the pixels of the one it has,
    // which is what gives each frame its own Remix hash. See UpdateTileTexture.
    std::map<uint64_t, IDirect3DTexture9*> tileFrames_;
    std::vector<TileBatch> batches_;

    int width_ = 0;
    int height_ = 0;
    IDirect3DTexture9* atlas_ = nullptr;
    IDirect3DVertexBuffer9* atlasVertices_ = nullptr;
    IDirect3DIndexBuffer9* atlasIndices_ = nullptr;
    std::vector<TileBatch> atlasGroups_;
    std::map<int, std::pair<int, int>> atlasSlots_;  // tile -> slot column/row
    std::map<uint64_t, std::vector<std::vector<uint32_t>>> atlasFrames_;
    int atlasWidth_ = 0, atlasHeight_ = 0;
    int presentWidth_ = 0;
    int presentHeight_ = 0;
    int renderScale_ = 100;
    bool stampEnabled_ = false;
    DWORD stampColour_ = 0;
    int deviceAttempts_ = 0;  // Initialize is retried until the device takes
    bool wireframe_ = false;
    DWORD cullMode_ = D3DCULL_CCW;
    bool circleMask_ = false;
    float maskX_ = 0, maskZ_ = 0, maskRadius_ = 0;
    IDirect3DTexture9* maskTexture_ = nullptr;
    bool BeginCircleMask(const Mat4& view);
    void EndCircleMask();
    RenderStats stats_;
};

}  // namespace gta2
