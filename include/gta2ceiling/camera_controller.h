#pragma once

#include "gta2ceiling/types.h"
#include <cmath>
#include <algorithm>

namespace gta2ceiling {

enum class ComfortPreset {
    LyingDown = 0,   // Completely lying down in bed looking straight up (0-15 deg tilt)
    Reclined = 1,    // Reclined sofa / gaming chair (25-35 deg tilt)
    DeskUpright = 2  // Sitting upright (45-60 deg tilt)
};

class CameraController {
public:
    static constexpr float CAMERA_DEADZONE_RADIUS = 3.0f; // Blocks before camera tracks player
    static constexpr float SMOOTHING_FACTOR = 5.0f;       // Exponential lerp speed

    CameraController() {
        apply_preset(ComfortPreset::LyingDown);
    }

    void apply_preset(ComfortPreset preset) {
        m_current_preset = preset;
        switch (preset) {
            case ComfortPreset::LyingDown:
                m_config.distance_meters = 2.1f;
                m_config.tilt_degrees = 12.0f;
                m_config.diorama_depth_relief = 1.3f;
                m_config.scale = 0.08f;
                break;
            case ComfortPreset::Reclined:
                m_config.distance_meters = 1.8f;
                m_config.tilt_degrees = 28.0f;
                m_config.diorama_depth_relief = 1.0f;
                m_config.scale = 0.08f;
                break;
            case ComfortPreset::DeskUpright:
                m_config.distance_meters = 1.5f;
                m_config.tilt_degrees = 45.0f;
                m_config.diorama_depth_relief = 0.8f;
                m_config.scale = 0.07f;
                break;
        }
    }

    // Smooth camera tracking: only tracks player when exceeding deadzone boundary
    void update_tracking(const Vec3& player_world_pos, float delta_time_sec) {
        Vec3 diff = player_world_pos - m_camera_focus;
        diff.y = 0.0f; // Ignore vertical height in deadzone calculation

        float dist = diff.length();
        if (dist > CAMERA_DEADZONE_RADIUS) {
            Vec3 target = player_world_pos - (diff.normalized() * CAMERA_DEADZONE_RADIUS);
            float t = std::clamp(delta_time_sec * SMOOTHING_FACTOR, 0.0f, 1.0f);
            m_camera_focus = m_camera_focus + (target - m_camera_focus) * t;
        }
    }

    void recenter(const Vec3& player_world_pos) {
        m_camera_focus = player_world_pos;
    }

    void adjust_distance(float delta_m) {
        m_config.distance_meters = std::clamp(m_config.distance_meters + delta_m, 1.0f, 3.5f);
    }

    void adjust_tilt(float delta_deg) {
        m_config.tilt_degrees = std::clamp(m_config.tilt_degrees + delta_deg, 0.0f, 75.0f);
    }

    const CeilingConfig& config() const { return m_config; }
    const Vec3& camera_focus() const { return m_camera_focus; }
    ComfortPreset current_preset() const { return m_current_preset; }

private:
    CeilingConfig m_config;
    Vec3 m_camera_focus{0.0f, 0.0f, 0.0f};
    ComfortPreset m_current_preset = ComfortPreset::LyingDown;
};

} // namespace gta2ceiling

