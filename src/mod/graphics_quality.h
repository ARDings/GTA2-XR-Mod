#pragma once

#include <d3d9.h>
#include <algorithm>
#include <cstring>

namespace gta2ceiling {
// One render device is owned by the game thread. Cache capabilities at device
// creation, never query the Wine/DXVK bridge for every sprite or stereo replay.
struct GraphicsQuality {
    bool smooth_textures = true;
    DWORD anisotropy = 1;
    D3DTEXTUREFILTERTYPE min_filter = D3DTEXF_LINEAR;
    DWORD requested_anisotropy = 4;  // from gta2ceiling.ini, the Standard profile
    DWORD max_anisotropy = 1;        // device capability
    float lod_bias = 0;              // negative = sharper distant texels (High-End)
};
inline GraphicsQuality g_graphicsQuality;

inline void ConfigureTextureFiltering(IDirect3DDevice9* device, const char* ini) {
    g_graphicsQuality = {};
    g_graphicsQuality.smooth_textures = GetPrivateProfileIntA("graphics", "smooth_textures", 1, ini) != 0;
    D3DCAPS9 caps{};
    const auto requested = std::clamp(GetPrivateProfileIntA("graphics", "anisotropy", 4, ini), 1u, 16u);
    g_graphicsQuality.requested_anisotropy = requested;
    if (g_graphicsQuality.smooth_textures && SUCCEEDED(device->GetDeviceCaps(&caps)) &&
        (caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC)) {
        g_graphicsQuality.max_anisotropy = std::max<DWORD>(1, caps.MaxAnisotropy);
        g_graphicsQuality.anisotropy = std::min<DWORD>(requested, g_graphicsQuality.max_anisotropy);
        if (g_graphicsQuality.anisotropy > 1) g_graphicsQuality.min_filter = D3DTEXF_ANISOTROPIC;
    }
}

// High-End: 16x anisotropic filtering (device limited). No negative mip bias:
// the scene is no longer supersampled, so sharper mips would only shimmer on
// head movement (and cost texture bandwidth).
inline void SetHighEndTextureFiltering(bool highEnd) {
    auto& q = g_graphicsQuality;
    if (!q.smooth_textures) return;
    q.anisotropy = std::min<DWORD>(highEnd ? 16u : q.requested_anisotropy, q.max_anisotropy);
    q.min_filter = q.anisotropy > 1 ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR;
    q.lod_bias = 0.0f;
}

inline void ApplyWorldTextureFiltering(IDirect3DDevice9* device) {
    const auto& quality = g_graphicsQuality;
    device->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, quality.anisotropy);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, quality.smooth_textures ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, quality.smooth_textures ? quality.min_filter : D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, quality.smooth_textures ? D3DTEXF_LINEAR : D3DTEXF_NONE);
    DWORD bias = 0;
    static_assert(sizeof(bias) == sizeof(quality.lod_bias));
    std::memcpy(&bias, &quality.lod_bias, sizeof(bias));
    device->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, bias);
    device->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
    device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
}
} // namespace gta2ceiling
