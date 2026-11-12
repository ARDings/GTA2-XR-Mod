#pragma once

#include "gta2ceiling/types.h"
#include <cstdint>
#include <string>
#include <chrono>

namespace gta2ceiling {

struct XboxRawState {
    // Buttons
    bool btn_a = false;
    bool btn_b = false;
    bool btn_x = false;
    bool btn_y = false;
    bool btn_lb = false;
    bool btn_rb = false;
    bool btn_back = false;
    bool btn_start = false;
    bool btn_guide = false; // Xbox logo - reserved by system on Android/Quest
    bool btn_thumbl = false;
    bool btn_thumbr = false;

    // D-Pad
    bool dpad_up = false;
    bool dpad_down = false;
    bool dpad_left = false;
    bool dpad_right = false;

    // Analog Triggers [0.0 .. 1.0]
    float trigger_left = 0.0f;
    float trigger_right = 0.0f;

    // Thumbsticks [-1.0 .. 1.0]
    float thumb_lx = 0.0f;
    float thumb_ly = 0.0f;
    float thumb_rx = 0.0f;
    float thumb_ry = 0.0f;

    bool is_connected = false;
};

// GTA2 Input Action Set
struct GameActions {
    // On Foot
    bool walk_forward = false;
    bool walk_backward = false;
    bool turn_left = false;
    bool turn_right = false;
    bool attack = false;          // Punch / Shoot weapon
    bool jump = false;
    bool enter_exit_vehicle = false;
    bool next_weapon = false;
    bool prev_weapon = false;

    // In Vehicle
    bool accelerate = false;
    bool reverse_brake = false;
    bool steer_left = false;
    bool steer_right = false;
    bool handbrake = false;
    bool vehicle_fire = false;
    bool horn = false;

    // Menu Navigation
    bool menu_up = false;
    bool menu_down = false;
    bool menu_left = false;
    bool menu_right = false;
    bool menu_confirm = false;
    bool menu_cancel = false;
    bool menu_pause = false;

    // XR Comfort Controls
    bool recenter_diorama = false;
    float adjust_distance = 0.0f;  // D-pad / Right stick modifier
    float adjust_height = 0.0f;
};

enum class InputScheme {
    ClassicTank = 0,    // Authentic GTA2 directional tank controls
    ModernTwinStick = 1 // Modern twin-stick shooter (Left stick moves, right stick aims)
};

class InputController {
public:
    static constexpr float DEFAULT_DEADZONE = 0.15f;
    static constexpr float TRIGGER_THRESHOLD = 0.30f;

    InputController() = default;

    void set_input_scheme(InputScheme scheme) { m_scheme = scheme; }
    InputScheme current_scheme() const { return m_scheme; }

    static float apply_deadzone(float val, float deadzone = DEFAULT_DEADZONE) {
        if (std::abs(val) < deadzone) return 0.0f;
        float sign = (val > 0.0f) ? 1.0f : -1.0f;
        return sign * ((std::abs(val) - deadzone) / (1.0f - deadzone));
    }

    GameActions process_input(const XboxRawState& raw, bool in_vehicle, bool in_menu) {
        GameActions actions;
        if (!raw.is_connected) {
            // Safety release: when disconnected, immediately drop all active actions
            m_was_connected = false;
            return actions;
        }

        m_was_connected = true;

        float lx = apply_deadzone(raw.thumb_lx);
        float ly = apply_deadzone(raw.thumb_ly);
        float rx = apply_deadzone(raw.thumb_rx);
        float ry = apply_deadzone(raw.thumb_ry);

        // Menu Mode
        if (in_menu) {
            actions.menu_up = raw.dpad_up || (ly > 0.5f);
            actions.menu_down = raw.dpad_down || (ly < -0.5f);
            actions.menu_left = raw.dpad_left || (lx < -0.5f);
            actions.menu_right = raw.dpad_right || (lx > 0.5f);
            actions.menu_confirm = raw.btn_a;
            actions.menu_cancel = raw.btn_b;
            actions.menu_pause = raw.btn_start;
            return actions;
        }

        // In-Game Common
        actions.menu_pause = raw.btn_start;
        actions.recenter_diorama = raw.btn_back;

        // XR Diorama distance adjustment via right stick when bumper held or in classic mode
        actions.adjust_distance = ry;
        actions.adjust_height = rx;

        if (in_vehicle) {
            // Vehicle Controls (Always use authentic steering)
            actions.accelerate = (raw.trigger_right > TRIGGER_THRESHOLD) || raw.btn_a;
            actions.reverse_brake = (raw.trigger_left > TRIGGER_THRESHOLD) || raw.btn_x;
            actions.handbrake = raw.btn_b || raw.btn_rb;
            actions.steer_left = (lx < -0.2f) || raw.dpad_left;
            actions.steer_right = (lx > 0.2f) || raw.dpad_right;
            actions.enter_exit_vehicle = raw.btn_y;
            actions.vehicle_fire = raw.btn_lb;
            actions.horn = raw.btn_thumbl;
        } else {
            // On Foot Controls
            if (m_scheme == InputScheme::ModernTwinStick) {
                // Modern Twin Stick: Left stick world direction, Right stick aiming & fire
                actions.walk_forward = (ly > 0.2f);
                actions.walk_backward = (ly < -0.2f);
                actions.turn_left = (lx < -0.2f);
                actions.turn_right = (lx > 0.2f);

                // Right stick activates aiming and shooting
                float right_mag = std::sqrt(rx * rx + ry * ry);
                if (right_mag > 0.5f) {
                    actions.attack = true;
                }
            } else {
                // Classic Tank Controls
                actions.walk_forward = (ly > 0.2f) || raw.dpad_up;
                actions.walk_backward = (ly < -0.2f) || raw.dpad_down;
                actions.turn_left = (lx < -0.2f) || raw.dpad_left;
                actions.turn_right = (lx > 0.2f) || raw.dpad_right;
                actions.attack = (raw.trigger_right > TRIGGER_THRESHOLD) || raw.btn_b;
            }

            actions.jump = raw.btn_x;
            actions.enter_exit_vehicle = raw.btn_y;
            actions.next_weapon = raw.btn_rb;
            actions.prev_weapon = raw.btn_lb;
        }

        return actions;
    }

    bool was_connected() const { return m_was_connected; }

private:
    bool m_was_connected = false;
    InputScheme m_scheme = InputScheme::ClassicTank;
};

} // namespace gta2ceiling
