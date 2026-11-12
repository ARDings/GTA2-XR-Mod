#pragma once

#include "gta2ceiling/types.h"
#include <string>
#include <vector>

namespace gta2ceiling {

struct HudState {
    int health = 100;
    int armor = 100;
    int score = 54200;
    int multiplier = 3;
    std::string current_weapon = "Pistol";
    int ammo = 48;
    int wanted_stars = 2;
};

class SpatialHudRenderer {
public:
    static constexpr float HUD_PLANE_OFFSET_Z = -0.05f; // Floating 5cm in front of diorama frame

    SpatialHudRenderer() = default;

    // Generates 3D quad vertices for the floating spatial HUD
    static void generate_hud_quads(const HudState& hud, const CeilingConfig& config, std::vector<Vec3>& out_quad_vertices) {
        // Place HUD at top-left and top-right corners of the diorama window
        float frame_half_w = 0.5f;
        float frame_half_h = 0.35f;

        // Health / Armor indicator at Top-Left
        Vec3 tl_min(-frame_half_w, frame_half_h - 0.08f, HUD_PLANE_OFFSET_Z);
        Vec3 tl_max(-frame_half_w + 0.3f, frame_half_h, HUD_PLANE_OFFSET_Z);

        out_quad_vertices.push_back({tl_min.x, tl_min.y, tl_min.z});
        out_quad_vertices.push_back({tl_max.x, tl_min.y, tl_min.z});
        out_quad_vertices.push_back({tl_max.x, tl_max.y, tl_max.z});
        out_quad_vertices.push_back({tl_min.x, tl_max.y, tl_max.z});

        // Score / Multiplier at Top-Right
        Vec3 tr_min(frame_half_w - 0.3f, frame_half_h - 0.08f, HUD_PLANE_OFFSET_Z);
        Vec3 tr_max(frame_half_w, frame_half_h, HUD_PLANE_OFFSET_Z);

        out_quad_vertices.push_back({tr_min.x, tr_min.y, tr_min.z});
        out_quad_vertices.push_back({tr_max.x, tr_min.y, tr_min.z});
        out_quad_vertices.push_back({tr_max.x, tr_max.y, tr_max.z});
        out_quad_vertices.push_back({tr_min.x, tr_max.y, tr_max.z});
    }
};

} // namespace gta2ceiling

