#pragma once

#include "gta2ceiling/types.h"
#include <algorithm>

namespace gta2ceiling {

struct LightSource {
    Vec3 sun_direction{0.577f, 0.577f, -0.577f}; // Angled sunlight from upper-south
    Vec3 ambient_color{0.3f, 0.3f, 0.35f};       // Cool skylight ambient
    Vec3 sun_color{0.8f, 0.8f, 0.75f};           // Warm sunlight
};

class DioramaShaderMath {
public:
    // Calculates facet shading for buildings, roads, and walls
    static uint32_t calculate_shading(const Vec3& normal, const LightSource& light, uint32_t base_color) {
        float n_dot_l = std::max(0.0f, Vec3::dot(normal, light.sun_direction));

        float r = ((base_color >> 16) & 0xFF) / 255.0f;
        float g = ((base_color >> 8) & 0xFF) / 255.0f;
        float b = (base_color & 0xFF) / 255.0f;

        float lit_r = r * (light.ambient_color.x + light.sun_color.x * n_dot_l);
        float lit_g = g * (light.ambient_color.y + light.sun_color.y * n_dot_l);
        float lit_b = b * (light.ambient_color.z + light.sun_color.z * n_dot_l);

        uint32_t ir = static_cast<uint32_t>(std::clamp(lit_r * 255.0f, 0.0f, 255.0f));
        uint32_t ig = static_cast<uint32_t>(std::clamp(lit_g * 255.0f, 0.0f, 255.0f));
        uint32_t ib = static_cast<uint32_t>(std::clamp(lit_b * 255.0f, 0.0f, 255.0f));

        return (0xFF000000) | (ir << 16) | (ig << 8) | ib;
    }

    // Audio distance attenuation relative to diorama center
    static float calculate_audio_volume(const Vec3& sound_pos, const Vec3& player_pos, float max_dist = 20.0f) {
        float dist = (sound_pos - player_pos).length();
        if (dist >= max_dist) return 0.0f;
        return 1.0f - (dist / max_dist);
    }
};

} // namespace gta2ceiling

