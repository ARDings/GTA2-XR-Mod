// Production cache, synthetic indexed artwork and a COM-counting upload
// double. No game, GPU, filesystem dump or headset.
#include "texture_store.h"
#include "texture_filter.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

namespace {
int alive = 0;
int uploads = 0;
bool failUpload = false;
gta2ceiling::MipOptions lastOptions;
std::vector<uint32_t> lastPixels;

class Texture final : public IDirect3DTexture9 {
public:
    Texture() { ++alive; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG result = --references_;
        if (!result) { --alive; delete this; }
        return result;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void*, DWORD, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return E_NOTIMPL; }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
    DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
    void STDMETHODCALLTYPE PreLoad() override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_TEXTURE; }
    DWORD STDMETHODCALLTYPE SetLOD(DWORD) override { return 0; }
    DWORD STDMETHODCALLTYPE GetLOD() override { return 0; }
    DWORD STDMETHODCALLTYPE GetLevelCount() override { return 1; }
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) override { return E_NOTIMPL; }
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override { return D3DTEXF_NONE; }
    void STDMETHODCALLTYPE GenerateMipSubLevels() override {}
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT, D3DSURFACE_DESC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT, IDirect3DSurface9**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRect(UINT, D3DLOCKED_RECT*, const RECT*, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT*) override { return E_NOTIMPL; }
private:
    ULONG references_ = 1;
};

IDirect3DDevice9* Device() {
    // Only the upload double sees this sentinel; it must not dereference it.
    return reinterpret_cast<IDirect3DDevice9*>(1);
}

struct Artwork {
    std::array<uint32_t, 256 * 64> palette{};
    std::array<uint8_t, 256 * 4> page{};
    gta2dx9::TextureRecord record{};
    Artwork(uint16_t width = 1, uint16_t height = 1) {
        record.width = width;
        record.height = height;
        record.palette = 7;
        record.pixels = page.data();
        page[0] = 1;
        gta2dx9::StorePalette(7, palette.data());
    }
    ~Artwork() { gta2dx9::ForgetPalette(7); }
};

void Reset() {
    gta2dx9::ReleaseDeviceTextures();
    assert(alive == 0);
    uploads = 0;
    failUpload = false;
    gta2dx9::g_trouble = {};
    gta2dx9::EffectSprites() = {};
    gta2dx9::SetTextureDumping(false);
    gta2dx9::SetCloseHoles(false);
}

void ProvisionalLifetime() {
    Reset();
    Artwork art;
    auto* first = gta2dx9::DeviceTextureFor(Device(), &art.record);
    assert(first && alive == 1 && uploads == 1);
    auto* second = gta2dx9::DeviceTextureFor(Device(), &art.record);
    assert(second && second != first && alive == 1 && uploads == 2);
    failUpload = true;
    assert(gta2dx9::DeviceTextureFor(Device(), &art.record) == second);
    assert(alive == 1);
    failUpload = false;
    art.palette[64] = 0x00112233;
    auto* valid = gta2dx9::DeviceTextureFor(Device(), &art.record);
    assert(valid && alive == 1);
    gta2dx9::ForgetDeviceTexture(&art.record);
    assert(alive == 1); // retained by the shared frame cache
    Reset();
    art.palette[64] = 0;
    assert(gta2dx9::DeviceTextureFor(Device(), &art.record));
    gta2dx9::ForgetDeviceTexture(&art.record);
    assert(alive == 0); // provisional has no shared frame owner
    assert(gta2dx9::DeviceTextureFor(Device(), &art.record));
    Reset(); // release-all also frees provisional artwork
}

void RecycledPointerAndSharedFrames() {
    Reset();
    Artwork art;
    art.palette[64] = 0x00ff0000;
    art.palette[128] = 0x0000ff00;
    auto* first = gta2dx9::DeviceTextureFor(Device(), &art.record);
    assert(first && lastPixels[0] == 0xffff0000);
    std::array<uint8_t, 256> replacement{};
    replacement[0] = 2;
    art.record.pixels = replacement.data(); // same size, revision and palette
    auto* second = gta2dx9::DeviceTextureFor(Device(), &art.record);
    assert(second != first && uploads == 2 && lastPixels[0] == 0xff00ff00);
    assert(gta2dx9::g_trouble.pixelsMovedSilently == 1);
    assert(gta2dx9::DeviceTextureFor(Device(), &art.record) == second && uploads == 2);
    art.record.pixels = art.page.data();
    assert(gta2dx9::DeviceTextureFor(Device(), &art.record) == first && uploads == 2);
    auto shared = art.record;
    assert(gta2dx9::DeviceTextureFor(Device(), &shared) == first && uploads == 2);
    gta2dx9::ForgetDeviceTexture(&art.record);
    assert(gta2dx9::DeviceTextureFor(Device(), &shared) == first && alive == 2);
    gta2dx9::ForgetDeviceTexture(&shared);
    Reset();
}

void ProvisionalToExistingFrame() {
    Reset();
    Artwork art;
    auto provisional = art.record;
    assert(gta2dx9::DeviceTextureFor(Device(), &provisional));
    art.palette[64] = 0x00224466;
    auto* valid = gta2dx9::DeviceTextureFor(Device(), &art.record);
    assert(alive == 2 && uploads == 2);
    assert(gta2dx9::DeviceTextureFor(Device(), &provisional) == valid);
    assert(alive == 1 && uploads == 2);
    Reset();
}

void FailedReplacementRetainsBlendAndFrame() {
    Reset();
    Artwork art(3, 3);
    art.palette[64] = 0x00040000;
    art.page.fill(0);
    art.page[257] = 1; // dim cutout, classified as additive artwork
    bool effect = false;
    auto* original = gta2dx9::DeviceTextureFor(Device(), &art.record, &effect);
    assert(original && effect && alive == 1);
    assert(lastOptions.max_levels == 1 && !lastOptions.bleed_transparent_edges);
    assert(!lastOptions.avoid_exact_magenta);
    ++art.record.revision;
    art.page[257] = 2;
    art.palette[128] = 0x00080000;
    failUpload = true;
    effect = false;
    assert(gta2dx9::DeviceTextureFor(Device(), &art.record, &effect) == original && effect);
    assert(alive == 1);
    failUpload = false;
    art.palette.fill(0); // palette registered but not filled yet
    effect = false;
    assert(gta2dx9::DeviceTextureFor(Device(), &art.record, &effect) == original && effect);
    assert(alive == 1);
    gta2dx9::ForgetPalette(7);
    effect = false;
    assert(gta2dx9::DeviceTextureFor(Device(), &art.record, &effect) == original && effect);
    art.record.pixels = nullptr;
    effect = false;
    assert(gta2dx9::DeviceTextureFor(Device(), &art.record, &effect) == original && effect);
    Reset();
}

void ReclassifiedFrameHasIndependentOwners() {
    Reset();
    Artwork art(3, 3);
    art.palette[64] = 0x00040000;
    art.page.fill(0);
    art.page[257] = 1;
    bool effect = false;
    auto* additive = gta2dx9::DeviceTextureFor(Device(), &art.record, &effect);
    assert(additive && effect);
    auto other = art.record;
    assert(gta2dx9::DeviceTextureFor(Device(), &other, &effect) == additive && effect);
    auto* cutout = gta2dx9::DeviceTextureFor(Device(), &art.record, &effect, false);
    assert(cutout != additive && !effect && alive == 2);
    assert(lastOptions.max_levels == 0 && lastOptions.bleed_transparent_edges);
    assert(lastOptions.avoid_exact_magenta);
    assert(gta2dx9::DeviceTextureFor(Device(), &other, &effect) == additive && effect);
    gta2dx9::ForgetDeviceTexture(&other);
    assert(alive == 1); // no orphaned superseded frame
    Reset();
}
}

namespace gta2dx9 {
void Log(const char*, ...) {}
float SpriteFeather() { return 0.0f; }
}
namespace gta2ceiling {
HRESULT CreateManagedArgbMipTexture(IDirect3DDevice9* device,
                                    std::span<const uint32_t> pixels,
                                    uint32_t width, uint32_t height,
                                    IDirect3DTexture9** output, MipOptions options) {
    assert(device == Device() && output && !*output);
    assert(pixels.size() == static_cast<size_t>(width) * height);
    if (failUpload) return E_OUTOFMEMORY;
    ++uploads;
    lastOptions = options;
    lastPixels.assign(pixels.begin(), pixels.end());
    *output = new Texture;
    return S_OK;
}
}

int main() {
    ProvisionalLifetime();
    RecycledPointerAndSharedFrames();
    ProvisionalToExistingFrame();
    FailedReplacementRetainsBlendAndFrame();
    ReclassifiedFrameHasIndependentOwners();
    std::puts("Texture-store lifetime, recycled-pointer and failure regressions passed.");
}
