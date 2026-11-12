#pragma once
#include "gta2ceiling/types.h"
#include <array>
#include <cmath>
#include <cstdint>

namespace gta2ceiling {
namespace ceiling_geometry_detail {
inline bool Finite(const Vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
}

// Row-vector rotation about the physical up axis. The same rotation must be
// applied to city, ceiling mask and HUD so their edges remain aligned.
inline Mat4 CeilingYaw(float radians) noexcept {
    Mat4 rotation;
    const float c=std::cos(radians), s=std::sin(radians);
    rotation.m[0]=c; rotation.m[2]=-s;
    rotation.m[8]=s; rotation.m[10]=c;
    return rotation;
}

// Upstream renderer coordinates: X=east, Y=height, Z=north (left-handed).
// Output is OpenXR LOCAL space in metres (right-handed). The city centre lands
// exactly at anchor; greater building height extends DOWN towards the viewer.
// This is a handedness conversion, not a 180-degree rotation. Together with the
// RH->LH XR view it preserves the upstream D3DCULL_CCW front-face convention.
// Row vectors: point * translate(-centre) * scale(s,-s,s) * translate(anchor).
// Head pose/IPD are deliberately absent: apply the physical eye view afterwards.
// On invalid/overflowing input, return false and leave output unchanged.
inline bool TryMakeCeilingCityModel(const Vec3& cityCenter, const Vec3& anchor,
                                    float metersPerTile, Mat4& output, float yawRadians=0,
                                    bool onFloor=false) noexcept {
    if (!ceiling_geometry_detail::Finite(cityCenter) ||
        !ceiling_geometry_detail::Finite(anchor) ||
        !std::isfinite(metersPerTile) || metersPerTile <= 0 || !std::isfinite(yawRadians)) return false;
    Mat4 candidate = Mat4::translation({-cityCenter.x, -cityCenter.y, -cityCenter.z});
    // Ceiling: reflect height so rooftops face the person below. Floor: the
    // same city rotated 180 degrees about X (still one reflection overall, so
    // GTA2's left-handed data stays unmirrored and rooftops face upwards).
    candidate = Mat4::multiply(candidate, onFloor ?
        Mat4::scale({metersPerTile, metersPerTile, -metersPerTile}) :
        Mat4::scale({metersPerTile, -metersPerTile, metersPerTile}));
    candidate = Mat4::multiply(candidate, CeilingYaw(yawRadians));
    candidate = Mat4::multiply(candidate, Mat4::translation(anchor));
    for (float value : candidate.m) if (!std::isfinite(value)) return false;
    output = candidate;
    return true;
}

// Plain XYZ/TEX1 layout, usable by DrawPrimitiveUP without graphics headers.
struct CeilingHudVertex {
    float x, y, z, u, v;
};
static_assert(sizeof(CeilingHudVertex) == 5 * sizeof(float));

// A physical rectangle below the ceiling. Height stays fixed while width follows
// the actual texture aspect ratio; neither the city scale nor the eye projection
// changes the rectangle. Texture top (v=0) is north/+Z, readable looking upward
// with neutral yaw. Caller renders both eyes from this same quad and texture.
inline bool TryMakeCeilingHudQuad(const Vec3& anchor, uint32_t textureWidth,
                                 uint32_t textureHeight,
                                 std::array<CeilingHudVertex, 4>& output,
                                 float halfHeightMeters = 0.48f,
                                 float belowCeilingMeters = 0.18f) noexcept {
    if (!ceiling_geometry_detail::Finite(anchor) || !textureWidth || !textureHeight ||
        !std::isfinite(halfHeightMeters) || halfHeightMeters <= 0 ||
        !std::isfinite(belowCeilingMeters) || belowCeilingMeters < 0) return false;
    const float halfWidth = halfHeightMeters * static_cast<float>(textureWidth) / textureHeight;
    const float y = anchor.y - belowCeilingMeters;
    const std::array<CeilingHudVertex, 4> candidate{{
        {anchor.x-halfWidth, y, anchor.z+halfHeightMeters, 0, 0},
        {anchor.x+halfWidth, y, anchor.z+halfHeightMeters, 1, 0},
        {anchor.x+halfWidth, y, anchor.z-halfHeightMeters, 1, 1},
        {anchor.x-halfWidth, y, anchor.z-halfHeightMeters, 0, 1},
    }};
    for (const auto& vertex : candidate)
        if (!ceiling_geometry_detail::Finite({vertex.x, vertex.y, vertex.z})) return false;
    output = candidate;
    return true;
}
} // namespace gta2ceiling
