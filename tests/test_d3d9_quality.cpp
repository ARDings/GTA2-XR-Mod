#include "renderer.h"
#include "texture_filter.h"
#include "ceiling_mask.h"
#include <array>
#include <iostream>
#include <stdexcept>

namespace gta2 {
struct RendererOwnershipTestAccess {
    static bool AnimatedTileReleasesInitial(Renderer& renderer) {
        IDirect3DTexture9* initial=nullptr;
        if(FAILED(renderer.Device()->CreateTexture(64,64,1,0,D3DFMT_A8R8G8B8,
                                                   D3DPOOL_MANAGED,&initial,nullptr))) return false;
        initial->AddRef(); // independent test reference, retained through release
        renderer.textures_.assign(1,initial);
        renderer.initialTextures_.push_back(initial);
        std::array<uint32_t,64*64> frame{};frame.fill(0xff123456);
        renderer.UpdateTileTexture(0,frame.data());
        const bool changed=renderer.textures_[0]!=initial;
        const auto* selected=renderer.textures_[0];
        renderer.UpdateTileTexture(0,frame.data());
        const bool reused=renderer.textures_[0]==selected && renderer.tileFrames_.size()==1;
        renderer.ReleaseResources();
        // A leaked original texture would still have its renderer reference.
        return initial->Release()==0 && changed && reused;
    }
};
}

namespace {
void Require(bool good, const char* message) {
    if (!good) throw std::runtime_error(message);
}
void Trace(const char* message) { std::cout << message << '\n'; }
void CheckUi(gta2::Renderer& renderer) {
    auto* device = renderer.Device();
    Require(renderer.BeginFrame(), "BeginFrame failed");
    IDirect3DSurface9* beforeTarget = nullptr;
    IDirect3DSurface9* beforeDepth = nullptr;
    Require(SUCCEEDED(device->GetRenderTarget(0, &beforeTarget)), "Missing main target");
    Require(SUCCEEDED(device->GetDepthStencilSurface(&beforeDepth)), "Missing main depth");
    D3DSURFACE_DESC colour{}, depth{};
    beforeTarget->GetDesc(&colour); beforeDepth->GetDesc(&depth);
    Require(colour.MultiSampleType == depth.MultiSampleType &&
            colour.MultiSampleQuality == depth.MultiSampleQuality, "MSAA attachment mismatch");
    Require(renderer.BeginUiLayer(true), "UI target switch failed");
    IDirect3DSurface9* uiDepth = nullptr;
    Require(device->GetDepthStencilSurface(&uiDepth) == D3DERR_NOTFOUND && !uiDepth,
            "UI retained incompatible depth/stencil attachment");
    IDirect3DSurface9* uiTarget = nullptr;
    Require(SUCCEEDED(device->GetRenderTarget(0, &uiTarget)), "Missing UI target");
    D3DSURFACE_DESC ui{}; uiTarget->GetDesc(&ui); uiTarget->Release();
    Require(ui.MultiSampleType == D3DMULTISAMPLE_NONE, "UI must be a sampleable non-MSAA texture");
    Require(!renderer.BeginUiLayer(false), "Nested UI scope accepted");
    renderer.EndUiLayer(false);
    IDirect3DSurface9* restoredTarget = nullptr;
    IDirect3DSurface9* restoredDepth = nullptr;
    Require(SUCCEEDED(device->GetRenderTarget(0, &restoredTarget)), "Target not restored");
    Require(SUCCEEDED(device->GetDepthStencilSurface(&restoredDepth)), "Depth not restored");
    Require(restoredTarget == beforeTarget && restoredDepth == beforeDepth,
            "UI restore changed main attachments");
    restoredTarget->Release(); restoredDepth->Release();
    beforeTarget->Release(); beforeDepth->Release();
    renderer.EndFrame();
}
void CheckTextureUpload(IDirect3DDevice9* device) {
    const std::array<uint32_t, 16> pixels{
        0xffffffff,0xff000000,0xffffffff,0xff000000,
        0xff000000,0xffffffff,0xff000000,0xffffffff,
        0xffffffff,0xff000000,0xffffffff,0xff000000,
        0xff000000,0xffffffff,0xff000000,0xffffffff};
    IDirect3DTexture9* texture = nullptr;
    Require(SUCCEEDED(gta2ceiling::CreateManagedArgbMipTexture(device, pixels, 4, 4, &texture)),
            "Actual D3D mip upload failed");
    Require(texture->GetLevelCount() == 3, "Missing GPU mip levels");
    for (UINT level = 1; level < 3; ++level) {
        D3DLOCKED_RECT lock{};
        Require(SUCCEEDED(texture->LockRect(level, &lock, nullptr, D3DLOCK_READONLY)), "Mip readback failed");
        const auto value = *static_cast<const uint32_t*>(lock.pBits);
        texture->UnlockRect(level);
        Require(value == 0xff808080, "GPU mip contents differ from filtered source");
    }
    texture->Release();
}
void CheckStereoMask(gta2::Renderer& renderer) {
    using namespace gta2ceiling;
    RoomSurface surface;
    std::string error;
    const std::vector<Vec3> boundary{{-.8f,0,-.8f},{.8f,0,-.8f},{.8f,0,0},
                                     {0,0,0},{0,0,.8f},{-.8f,0,.8f}};
    Require(BuildRoomSurface(boundary,surface,error), "Synthetic concave ceiling invalid");
    auto* device=renderer.Device();
    Require(renderer.BeginFrame(true), "Mask BeginFrame failed");
    Require(ClearCeilingBackground(device), "Passthrough clear failed");
    auto view=Mat4::identity();
    view.m[5]=0; view.m[10]=0; view.m[6]=1; view.m[9]=1; view.m[14]=.5f;
    struct PixelVertex {float x,y,z,rhw; DWORD colour;};
    for(DWORD eye=0;eye<2;++eye) {
        const D3DVIEWPORT9 viewport{eye*160,0,160,240,0,1};
        Require(SUCCEEDED(device->SetViewport(&viewport)), "Eye viewport failed");
        Require(DrawRoomCeiling(device,view,Mat4::identity(),surface), "Concave stencil mask failed");
        PreserveCeilingCoverage(device);
        // Attempt a full-screen HUD. Only this eye's ceiling may accept it.
        device->SetRenderState(D3DRS_ZENABLE,FALSE);
        device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS,TRUE);
        device->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);
        const float x=static_cast<float>(eye*160);
        const PixelVertex quad[]{{x-.5f,-.5f,0,1,0xff00ff00},{x+159.5f,-.5f,0,1,0xff00ff00},
            {x+159.5f,239.5f,0,1,0xff00ff00},{x-.5f,239.5f,0,1,0xff00ff00}};
        Require(SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN,2,quad,sizeof(PixelVertex))),
                "Masked colour draw failed");
        EndRoomCeilingMask(device);
    }
    // Resolve MSAA before CPU readback. Read before Present discards contents.
    Require(renderer.FinishFrame(), "Mask alpha resolve/composition failed");
    IDirect3DSurface9 *target=nullptr,*resolved=nullptr,*readback=nullptr;
    Require(SUCCEEDED(device->GetRenderTarget(0,&target)), "Mask target missing");
    D3DSURFACE_DESC desc{};target->GetDesc(&desc);
    Require(SUCCEEDED(device->CreateRenderTarget(320,240,desc.Format,D3DMULTISAMPLE_NONE,0,FALSE,&resolved,nullptr)),
            "Mask resolve target failed");
    Require(SUCCEEDED(device->StretchRect(target,nullptr,resolved,nullptr,D3DTEXF_NONE)), "MSAA resolve failed");
    Require(SUCCEEDED(device->CreateOffscreenPlainSurface(320,240,desc.Format,D3DPOOL_SYSTEMMEM,&readback,nullptr)),
            "Mask readback surface failed");
    Require(SUCCEEDED(device->GetRenderTargetData(resolved,readback)), "Mask readback failed");
    D3DLOCKED_RECT lock{};
    Require(SUCCEEDED(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY)), "Mask read lock failed");
    auto pixel=[&](int x,int y){return reinterpret_cast<const uint32_t*>(
        static_cast<const char*>(lock.pBits)+y*lock.Pitch)[x]&0xffffffu;};
    bool correct=true;
    for(int eye=0;eye<2;++eye) {
        correct &= pixel(eye*160+48,168)==0x00ff00u; // inside the L
        correct &= pixel(eye*160+112,72)==0xff00ffu; // concave notch
        correct &= pixel(eye*160+4,4)==0xff00ffu; // real room outside
    }
    unsigned mixedPixels=0;
    std::map<uint32_t,unsigned> mixedColours;
    for(int y=0;y<240;++y) for(int x=0;x<320;++x) {
        const auto colour=pixel(x,y);
        if(colour!=0x00ff00u && colour!=0xff00ffu) {++mixedPixels;++mixedColours[colour];}
    }
    readback->UnlockRect();readback->Release();resolved->Release();target->Release();
    Require(correct,"Per-eye ceiling mask leaked or lost its concave boundary");
    if(mixedPixels) std::cerr << "Mixed key pixels before host: " << mixedPixels << '\n';
    for(const auto& [colour,count]:mixedColours) std::cerr << std::hex << colour << std::dec << ": " << count << '\n';
    Require(!mixedPixels,"MSAA resolve contaminated the exact passthrough key at mask boundaries");
    const D3DVIEWPORT9 full{0,0,320,240,0,1};device->SetViewport(&full);
}
}
int main() {
    // Real, hidden Windows D3D9 smoke test using synthetic pixels only. It does
    // not substitute for DXVK/OpenXR/Quest acceptance. Headless CI may skip.
    HWND window = CreateWindowW(L"STATIC", L"GTA2Ceiling local D3D check", WS_OVERLAPPED,
                                0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return 77;
    gta2::SetRendererTrace(Trace);
    gta2::Renderer renderer;
    std::string error;
    if (!renderer.Initialize(window, 320, 240, &error)) {
        std::cout << "SKIP: local D3D9 device unavailable: " << error << '\n';
        DestroyWindow(window); return 77;
    }
    try {
        CheckTextureUpload(renderer.Device());
        Require(gta2::RendererOwnershipTestAccess::AnimatedTileReleasesInitial(renderer),
                "Animated tile lost original texture ownership or failed to reuse its frame");
        CheckUi(renderer);
        CheckUi(renderer);
        CheckStereoMask(renderer);
        Require(SUCCEEDED(renderer.ResetDevice()), "Device Reset failed after UI release");
        CheckUi(renderer);
        renderer.Shutdown(); DestroyWindow(window);
        std::cout << "PASS: real local D3D9 mip upload, UI roundtrip, stereo concave stencil/readback, reset. No Quest claim.\n";
        return 0;
    } catch (const std::exception& e) {
        renderer.Shutdown(); DestroyWindow(window);
        std::cerr << e.what() << '\n'; return 1;
    }
}
