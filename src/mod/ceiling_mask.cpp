#include "ceiling_mask.h"
#include <d3d9.h>
#include "../../third_party/gta2-renderer/src/dynamic_draw.h"
#include <cmath>
#include <cstring>
#include <vector>

namespace gta2ceiling {
namespace {
bool SetMatrix(IDirect3DDevice9* device, D3DTRANSFORMSTATETYPE type, const Mat4& value) {
    D3DMATRIX matrix;
    static_assert(sizeof(matrix) == sizeof(value.m));
    std::memcpy(&matrix, value.m, sizeof(matrix));
    return SUCCEEDED(device->SetTransform(type, &matrix));
}
bool Finite(const Mat4& matrix) {
    for (float value : matrix.m) if (!std::isfinite(value)) return false;
    return true;
}
}

bool ClearClip(IDirect3DDevice9* device) {
    return device && SUCCEEDED(device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0));
}

bool SetCityClip(IDirect3DDevice9* device, const Vec3& cityCenter,
                 float metersPerTile, float widthMeters, float depthMeters) {
    if (!device) return false;
    CityClipPlanes planes;
    D3DCAPS9 caps{};
    if (!ClearClip(device) ||
        !TryMakeCityClipPlanes(cityCenter, metersPerTile, widthMeters, depthMeters, planes) ||
        FAILED(device->GetDeviceCaps(&caps)) || caps.MaxUserClipPlanes < planes.size()) return false;
    for (DWORD i = 0; i < static_cast<DWORD>(planes.size()); ++i) {
        if (FAILED(device->SetClipPlane(i, planes[i].data()))) return false;
    }
    if (FAILED(device->SetRenderState(D3DRS_CLIPPING, TRUE)) ||
        FAILED(device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0x0Fu))) {
        ClearClip(device);
        return false;
    }
    return true;
}

bool SetCityHexClip(IDirect3DDevice9* device, const Vec3& cityCenter,
                    float metersPerTile, float radiusMeters) {
    if (!device || !std::isfinite(metersPerTile) || metersPerTile <= 0 ||
        !std::isfinite(radiusMeters) || radiusMeters <= 0) return false;
    D3DCAPS9 caps{};
    if (FAILED(device->GetDeviceCaps(&caps))) return false;
    if (caps.MaxUserClipPlanes < 6)
        return SetCityClip(device, cityCenter, metersPerTile, 2 * radiusMeters, 2 * radiusMeters);
    const float inradius = radiusMeters / metersPerTile + 0.25f; // tiles, small margin
    for (DWORD i = 0; i < 6; ++i) {
        const float angle = 1.04719755f * static_cast<float>(i);
        const float nx = std::cos(angle), nz = std::sin(angle);
        // Keep dot >= 0:  inradius - n.(p - centre) >= 0
        const float plane[4] = {-nx, 0, -nz, inradius + nx * cityCenter.x + nz * cityCenter.z};
        if (FAILED(device->SetClipPlane(i, plane))) return false;
    }
    return SUCCEEDED(device->SetRenderState(D3DRS_CLIPPING, TRUE)) &&
           SUCCEEDED(device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0x3Fu));
}

bool PrepareCeilingDraw(IDirect3DDevice9* device,const Mat4& eyeView,const Mat4& projection) {
    if (!device || !Finite(eyeView) || !Finite(projection) || !ClearClip(device)) return false;
    if (FAILED(device->SetVertexShader(nullptr)) || FAILED(device->SetPixelShader(nullptr)) ||
        !SetMatrix(device, D3DTS_WORLD, Mat4::identity()) ||
        !SetMatrix(device, D3DTS_VIEW, eyeView) || !SetMatrix(device, D3DTS_PROJECTION, projection)) return false;
    struct State { D3DRENDERSTATETYPE type; DWORD value; };
    const State states[] = {
        {D3DRS_ZENABLE, D3DZB_TRUE}, {D3DRS_ZWRITEENABLE, TRUE}, {D3DRS_ZFUNC, D3DCMP_LESSEQUAL},
        {D3DRS_CULLMODE, D3DCULL_NONE}, {D3DRS_FILLMODE, D3DFILL_SOLID},
        {D3DRS_LIGHTING, FALSE}, {D3DRS_FOGENABLE, FALSE}, {D3DRS_SPECULARENABLE, FALSE},
        {D3DRS_ALPHATESTENABLE, FALSE}, {D3DRS_ALPHABLENDENABLE, FALSE},
        // Coverage resolves against transparent black. The renderer encodes
        // the host key only after MSAA, so boundary samples stay uncontaminated.
        {D3DRS_MULTISAMPLEANTIALIAS, TRUE},
        {D3DRS_CLIPPING, TRUE}, {D3DRS_SCISSORTESTENABLE, FALSE},
        {D3DRS_SRGBWRITEENABLE, FALSE},
        {D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA},
    };
    for (const auto& state : states)
        if (FAILED(device->SetRenderState(state.type, state.value))) return false;
    if (FAILED(device->SetTexture(0, nullptr)) ||
        FAILED(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1)) ||
        FAILED(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE)) ||
        FAILED(device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1)) ||
        FAILED(device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE)) ||
        FAILED(device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE)) ||
        FAILED(device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE))) return false;
    return true;
}
bool DrawCeilingBase(IDirect3DDevice9* device,const Mat4& eyeView,const Mat4& projection,
                     const Vec3& anchor,float widthMeters,float depthMeters,bool onFloor) {
    std::array<CeilingBaseVertex,4> quad;
    if(!TryMakeCeilingBaseQuad(anchor,widthMeters,depthMeters,quad,onFloor) ||
       !PrepareCeilingDraw(device,eyeView,projection)) return false;
    EndRoomCeilingMask(device);
    return SUCCEEDED(gta2::DynamicDraw::Instance().Draw(device,D3DPT_TRIANGLEFAN,2,quad.data(),sizeof(CeilingBaseVertex)));
}
void EndRoomCeilingMask(IDirect3DDevice9* device) {
    if(device) device->SetRenderState(D3DRS_STENCILENABLE,FALSE);
}
void PreserveCeilingCoverage(IDirect3DDevice9* device) {
    // Alpha belongs to the physical portal, not to the filtered cutout texture
    // of a car/tree or to additive lights drawn over it. Keep its MSAA samples.
    if(device) device->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED|D3DCOLORWRITEENABLE_GREEN|D3DCOLORWRITEENABLE_BLUE);
}
namespace {
constexpr DWORD kMaskBit=0x01, kCityBit=0x80;
}
bool DrawRoomCeiling(IDirect3DDevice9* device,const Mat4& eyeView,const Mat4& projection,
                     const RoomSurface& surface,float wallDepthMeters,bool onFloor,const Vec3* outsideViewer,
                     const std::function<void()>* underlay) {
    // The base sits just beyond the city (above a ceiling, below a floor); the
    // wall band extends from it towards the room interior.
    const float baseOffset=onFloor?-0.01f:0.01f;
    const float inward=onFloor?1.0f:-1.0f;
    if(surface.triangles.empty() || surface.triangles.size()%3 || !PrepareCeilingDraw(device,eyeView,projection)) return false;
    std::array<CeilingBaseVertex,3*(256-2)> vertices{};
    if(surface.triangles.size()>vertices.size()) return false;
    size_t count=0;
    for(auto index:surface.triangles) {
        if(index>=surface.boundary.size()) return false;
        const auto& p=surface.boundary[index];vertices[count++]={p.x,p.y+baseOffset,p.z,kCeilingBaseColor};
    }
    D3DVIEWPORT9 viewport{};
    if(FAILED(device->GetViewport(&viewport))) return false;
    const D3DRECT eyeRect{static_cast<LONG>(viewport.X),static_cast<LONG>(viewport.Y),
        static_cast<LONG>(viewport.X+viewport.Width),static_cast<LONG>(viewport.Y+viewport.Height)};
    if(FAILED(device->SetRenderState(D3DRS_STENCILWRITEMASK,0xFF))) return false;
    if(FAILED(device->Clear(1,&eyeRect,D3DCLEAR_STENCIL,0,1,0))) return false;
    if(underlay && *underlay) {
        (*underlay)();
        if(!PrepareCeilingDraw(device,eyeView,projection)) return false;
    }
    const std::array<std::pair<D3DRENDERSTATETYPE,DWORD>,8> states{{
        {D3DRS_STENCILENABLE,TRUE},{D3DRS_STENCILMASK,0xFF},{D3DRS_STENCILREF,1},{D3DRS_STENCILWRITEMASK,kMaskBit},
        {D3DRS_STENCILFUNC,D3DCMP_ALWAYS},{D3DRS_STENCILFAIL,D3DSTENCILOP_KEEP},
        {D3DRS_STENCILZFAIL,D3DSTENCILOP_KEEP},{D3DRS_STENCILPASS,D3DSTENCILOP_REPLACE}}};
    for(const auto& state:states) if(FAILED(device->SetRenderState(state.first,state.second))) {
        EndRoomCeilingMask(device);return false;
    }
    auto result=gta2::DynamicDraw::Instance().Draw(device,D3DPT_TRIANGLELIST,static_cast<UINT>(count/3),
                                       vertices.data(),sizeof(CeilingBaseVertex));
    const size_t corners=surface.boundary.size();
    if(SUCCEEDED(result) && std::isfinite(wallDepthMeters) && wallDepthMeters>0 && corners>=3) {
        // Wall band: depth + stencil only, so the real wall stays visible
        // through it wherever no building hangs in front of it.
        std::vector<CeilingBaseVertex> farWall,nearWall;
        farWall.reserve(corners*6);
        for(size_t i=0;i<corners;++i) {
            const auto& a=surface.boundary[i];
            const auto& b=surface.boundary[(i+1)%corners];
            const CeilingBaseVertex at{a.x,a.y+baseOffset,a.z,0},bt{b.x,b.y+baseOffset,b.z,0};
            const CeilingBaseVertex ab{a.x,a.y+inward*wallDepthMeters,a.z,0},bb{b.x,b.y+inward*wallDepthMeters,b.z,0};
            bool facesViewer=false;
            if(outsideViewer) {
                // Outward normal from the field centre; viewer on that side?
                const float mx=0.5f*(a.x+b.x)-surface.centre.x,mz=0.5f*(a.z+b.z)-surface.centre.z;
                float nx=b.z-a.z,nz=-(b.x-a.x);
                if(nx*mx+nz*mz<0) {nx=-nx;nz=-nz;}
                facesViewer=nx*(outsideViewer->x-0.5f*(a.x+b.x))+nz*(outsideViewer->z-0.5f*(a.z+b.z))>0;
            }
            (facesViewer?nearWall:farWall).insert((facesViewer?nearWall:farWall).end(),{at,bt,bb,at,bb,ab});
        }
        device->SetRenderState(D3DRS_COLORWRITEENABLE,0);
        if(!farWall.empty())
            result=gta2::DynamicDraw::Instance().Draw(device,D3DPT_TRIANGLELIST,static_cast<UINT>(farWall.size()/3),
                                           farWall.data(),sizeof(CeilingBaseVertex));
        if(SUCCEEDED(result) && !nearWall.empty()) {
            device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
            result=gta2::DynamicDraw::Instance().Draw(device,D3DPT_TRIANGLELIST,static_cast<UINT>(nearWall.size()/3),
                                           nearWall.data(),sizeof(CeilingBaseVertex));
            device->SetRenderState(D3DRS_ZWRITEENABLE,TRUE);
        }
        device->SetRenderState(D3DRS_COLORWRITEENABLE,D3DCOLORWRITEENABLE_RED|D3DCOLORWRITEENABLE_GREEN|
                                                      D3DCOLORWRITEENABLE_BLUE|D3DCOLORWRITEENABLE_ALPHA);
    }
    // City: only inside the mask; every drawn sample also sets the city bit.
    if(FAILED(result) || FAILED(device->SetRenderState(D3DRS_STENCILFUNC,D3DCMP_EQUAL)) ||
       FAILED(device->SetRenderState(D3DRS_STENCILMASK,kMaskBit)) ||
       FAILED(device->SetRenderState(D3DRS_STENCILREF,kCityBit|kMaskBit)) ||
       FAILED(device->SetRenderState(D3DRS_STENCILPASS,D3DSTENCILOP_REPLACE)) ||
       FAILED(device->SetRenderState(D3DRS_STENCILWRITEMASK,kCityBit))) {
        EndRoomCeilingMask(device);return false;
    }
    return true;
}
void FillCeilingCoverage(IDirect3DDevice9* device) {
    if(!device) return;
    DWORD stencil=FALSE;
    if(FAILED(device->GetRenderState(D3DRS_STENCILENABLE,&stencil)) || !stencil) return;
    D3DVIEWPORT9 viewport{};
    if(FAILED(device->GetViewport(&viewport))) return;
    struct Vertex { float x,y,z,rhw; DWORD color; };
    const float x0=static_cast<float>(viewport.X)-0.5f,y0=static_cast<float>(viewport.Y)-0.5f;
    const float x1=x0+viewport.Width,y1=y0+viewport.Height;
    const Vertex quad[]{{x0,y0,0,1,0xFF000000u},{x1,y0,0,1,0xFF000000u},
                        {x1,y1,0,1,0xFF000000u},{x0,y1,0,1,0xFF000000u}};
    device->SetVertexShader(nullptr); device->SetPixelShader(nullptr); device->SetTexture(0,nullptr);
    device->SetRenderState(D3DRS_CLIPPLANEENABLE,0);
    device->SetRenderState(D3DRS_ZENABLE,FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
    device->SetRenderState(D3DRS_FOGENABLE,FALSE);
    device->SetRenderState(D3DRS_LIGHTING,FALSE);
    device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    device->SetRenderState(D3DRS_STENCILFUNC,D3DCMP_EQUAL);
    device->SetRenderState(D3DRS_STENCILMASK,kCityBit);
    device->SetRenderState(D3DRS_STENCILREF,kCityBit);
    device->SetRenderState(D3DRS_STENCILWRITEMASK,0);
    device->SetRenderState(D3DRS_STENCILPASS,D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_COLORWRITEENABLE,D3DCOLORWRITEENABLE_ALPHA);
    device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);
    device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
    device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
    device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);
    device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);
    device->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);
    gta2::DynamicDraw::Instance().Draw(device,D3DPT_TRIANGLEFAN,2,quad,sizeof(Vertex));
    // Back to the city/HUD state set up by DrawRoomCeiling.
    device->SetRenderState(D3DRS_STENCILMASK,kMaskBit);
    device->SetRenderState(D3DRS_STENCILREF,kCityBit|kMaskBit);
    device->SetRenderState(D3DRS_STENCILWRITEMASK,kCityBit);
    device->SetRenderState(D3DRS_STENCILPASS,D3DSTENCILOP_REPLACE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE,D3DCOLORWRITEENABLE_RED|D3DCOLORWRITEENABLE_GREEN|
                                                  D3DCOLORWRITEENABLE_BLUE);
    device->SetRenderState(D3DRS_ZENABLE,D3DZB_TRUE);
}

bool ClearCeilingBackground(IDirect3DDevice9* device) {
    if (!device || !ClearClip(device) ||
        FAILED(device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE))) return false;
    return SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                   0, 1.0f, 0));
}
} // namespace gta2ceiling
