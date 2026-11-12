#pragma once

#include "gta2ceiling/types.h"
#include "gta2ceiling/room_surface.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>

struct IDirect3DDevice9;

namespace gta2ceiling {

// Reserved transport key, encoded only AFTER alpha-aware scene MSAA resolve.
// The host keys exact RGB before filtering; source artwork avoids this code.
inline constexpr uint32_t kCeilingChromaPink = 0xFFFF00FFu;
inline constexpr uint32_t kCeilingBaseColor = 0xFF34302Au;

struct CeilingBaseVertex {
    float x, y, z;
    uint32_t color;
};
static_assert(sizeof(CeilingBaseVertex) == 16, "D3DFVF_XYZ | D3DFVF_DIFFUSE layout");
using CityClipPlanes = std::array<std::array<float, 4>, 4>;

namespace ceiling_mask_detail {
inline bool Finite(const Vec3& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
inline bool Positive(float value) noexcept {
    return std::isfinite(value) && value > 0;
}
}

// Both eyes share this physical rectangle in OpenXR LOCAL metres. It sits 1 cm
// beyond the city origin, away from the person below. Invalid input leaves output
// unchanged, so callers must check the return value before drawing.
inline bool TryMakeCeilingBaseQuad(const Vec3& anchor, float widthMeters,
                                   float depthMeters,
                                   std::array<CeilingBaseVertex, 4>& output,
                                   bool onFloor = false) noexcept {
    if (!ceiling_mask_detail::Finite(anchor) ||
        !ceiling_mask_detail::Positive(widthMeters) ||
        !ceiling_mask_detail::Positive(depthMeters)) return false;
    const float x = widthMeters * 0.5f, z = depthMeters * 0.5f;
    const float y = anchor.y + (onFloor ? -0.01f : 0.01f);
    const std::array<CeilingBaseVertex, 4> candidate{{
        {anchor.x-x, y, anchor.z+z, kCeilingBaseColor},
        {anchor.x+x, y, anchor.z+z, kCeilingBaseColor},
        {anchor.x+x, y, anchor.z-z, kCeilingBaseColor},
        {anchor.x-x, y, anchor.z-z, kCeilingBaseColor},
    }};
    for (const auto& vertex : candidate)
        if (!ceiling_mask_detail::Finite({vertex.x, vertex.y, vertex.z})) return false;
    output = candidate;
    return true;
}

// Fixed-function D3D9 clip planes are WORLD-space equations and keep dot>=0:
// https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-setclipplane
// Our D3D WORLD is identity and the game-to-ceiling model is folded into VIEW.
// Consequently these planes must be in original game coordinates, NOT metres
// and NOT already transformed by model/view/projection. X/Z bound the city; Y
// remains free so buildings can extend down from the ceiling.
inline bool TryMakeCityClipPlanes(const Vec3& cityCenter, float metersPerTile,
                                  float widthMeters, float depthMeters,
                                  CityClipPlanes& output) noexcept {
    if (!ceiling_mask_detail::Finite(cityCenter) ||
        !ceiling_mask_detail::Positive(metersPerTile) ||
        !ceiling_mask_detail::Positive(widthMeters) ||
        !ceiling_mask_detail::Positive(depthMeters)) return false;
    const float x = (widthMeters * 0.5f) / metersPerTile;
    const float z = (depthMeters * 0.5f) / metersPerTile;
    const CityClipPlanes candidate{{
        {{ 1, 0, 0, x-cityCenter.x}}, // x >= centre.x - half width
        {{-1, 0, 0, x+cityCenter.x}}, // x <= centre.x + half width
        {{ 0, 0, 1, z-cityCenter.z}}, // z >= centre.z - half depth
        {{ 0, 0,-1, z+cityCenter.z}}, // z <= centre.z + half depth
    }};
    for (const auto& plane : candidate)
        for (float value : plane) if (!std::isfinite(value)) return false;
    output = candidate;
    return true;
}

// Render-thread only, inside an active scene and the caller's current eye
// viewport. Eye view is the physical XR view WITHOUT the city model transform.
// Sets fixed-function draw state; the following city/HUD pass must set its usual
// matrices, material, stream and depth state. No D3D resources are allocated.
bool DrawCeilingBase(IDirect3DDevice9* device, const Mat4& eyeView,
                     const Mat4& projection, const Vec3& anchor,
                     float widthMeters, float depthMeters, bool onFloor = false);

// Requires fixed-function rendering and game-coordinate WORLD as explained
// above. On false, skip the city pass: drawing without clips would cover the room.
bool SetCityClip(IDirect3DDevice9* device, const Vec3& cityCenter,
                 float metersPerTile, float widthMeters, float depthMeters);
bool ClearClip(IDirect3DDevice9* device);
// Round play field: six planes (game coordinates, like SetCityClip) forming a
// hexagon around the circle of radiusMeters. Falls back to SetCityClip's square
// on devices with fewer than six user clip planes.
bool SetCityHexClip(IDirect3DDevice9* device, const Vec3& cityCenter,
                    float metersPerTile, float radiusMeters);
// Marks the scanned ceiling AND a band of the room walls below it (wallDepth
// metres) in stencil and depth. Buildings hanging down near a wall stay
// visible in front of it; anything beyond the real walls fails the depth test.
// The walls write no colour: only drawn city pixels become opaque later.
bool DrawRoomCeiling(IDirect3DDevice9* device,const Mat4& eyeView,const Mat4& projection,
                     const RoomSurface& surface,float wallDepthMeters=0,bool onFloor=false,
                     const Vec3* outsideViewer=nullptr,const std::function<void()>* underlay=nullptr);
// underlay: drawn right after the stencil is cleared, before the base and the
// wall band (whose depth would hide it from behind) - the walk-out player,
// which may stand anywhere around the field. The mask itself only ever sets
// its own stencil bit, so whatever the underlay marked as city stays city.
// outsideViewer (round fields): walls whose outer side faces this point are
// the near rim seen from outside. They still mark the stencil but write no
// depth, so you can look into the field instead of against an invisible cone.
// After the city pass: give every sample the city actually covered full alpha,
// including those in front of the wall band (the ceiling already has it).
void FillCeilingCoverage(IDirect3DDevice9* device);
void EndRoomCeilingMask(IDirect3DDevice9* device);
void PreserveCeilingCoverage(IDirect3DDevice9* device);

// Clear once for the complete SBS target, BEFORE selecting individual eye
// viewports. Requires a depth buffer. The host must enable passthrough and pink
// colour-key mode. The private scene clears transparent black; FinishFrame
// encodes the key after resolve, preventing pink MSAA fringes.
bool ClearCeilingBackground(IDirect3DDevice9* device);

} // namespace gta2ceiling
