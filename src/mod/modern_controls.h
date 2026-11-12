// GTA2Ceiling "modern" controls (optional, RT in the in-headset menu).
//
// GTA2's own controls are tank controls: left/right turn the character, up
// walks where it faces. The modern scheme translates the pad into those keys:
//
//  * On foot the left stick points where the character should go, as you see
//    it: the character turns that way by itself and walks.
//  * In a car RT accelerates, LT brakes/reverses, the stick steers, B fires.
//
// Nothing in the game is changed; only which keys the pad holds. It needs to
// know where the player faces (read from the game) and how the stick's
// directions lie on the map for the current view; without either, the pad
// behaves classically. Pure and free of Windows, so it is tested on its own.
#pragma once

#include "gamepad_bridge.h"

#include <cmath>

namespace gta2ceiling {

struct ModernPlayer {
    bool known = false, inCar = false;
    float x = 0, y = 0;          // map cells, x east, y south
    float faceX = 0, faceY = 0;  // facing in the same frame (on foot)
};

// Map direction (x east, y south) for a stick deflection (x right, y up).
struct StickToMap {
    bool valid = false;
    float m[2][2] = {{1, 0}, {0, -1}};
};

// From where the player and its two map neighbours (one cell east, one cell
// north) appear on screen (x right, y up): which map direction looks like
// "stick right" and "stick up" there.
inline StickToMap SolveStickToMap(const float origin[2], const float east[2], const float north[2]) {
    StickToMap out;
    const float ax = east[0] - origin[0], ay = east[1] - origin[1];
    const float bx = north[0] - origin[0], by = north[1] - origin[1];
    const float det = ax * by - ay * bx;
    if (!(std::fabs(det) > 1e-9f)) return out;
    // east/north amounts for a screen direction (sx, sy); south = -north.
    out.m[0][0] = by / det;  out.m[0][1] = -bx / det;
    out.m[1][0] = ay / det;  out.m[1][1] = -ax / det;
    out.valid = true;
    return out;
}

class ModernControls {
public:
    // Rewrites `state` into what the classic mapping then turns into keys.
    // True when the modern scheme was applied to this sample.
    bool Apply(GamepadState& state, const ModernPlayer& player, const StickToMap& view, int stickDeadzone,
               int triggerThreshold) {
        if (!state.available || !player.known) { Forget(); return false; }
        if (player.inCar) {
            Forget();
            const bool gas = state.right_trigger > triggerThreshold, brake = state.left_trigger > triggerThreshold;
            const bool fire = (state.buttons & kB) != 0;
            state.buttons = static_cast<uint16_t>(state.buttons & ~kB);
            if (gas) state.buttons |= kUp;
            if (brake) state.buttons |= kDown;
            state.right_trigger = fire ? 255 : 0;
            state.left_trigger = 0;
            return true;
        }
        const float sx = state.left_x / 32767.0f, sy = state.left_y / 32767.0f;
        const float deflection = std::hypot(sx, sy);
        if (!view.valid || deflection * 32767.0f <= static_cast<float>(stickDeadzone)) { Forget(); return false; }
        float tx = view.m[0][0] * sx + view.m[0][1] * sy, ty = view.m[1][0] * sx + view.m[1][1] * sy;
        const float length = std::hypot(tx, ty), facing = std::hypot(player.faceX, player.faceY);
        if (!(length > 1e-6f) || !(facing > 1e-6f)) { Forget(); return false; }
        tx /= length; ty /= length;
        Learn(player, tx, ty);
        const float fx = player.faceX / facing * faceSignX_, fy = player.faceY / facing * faceSignY_;
        // y points south, so a positive cross product is a turn to the right.
        const float angle = std::atan2(fx * ty - fy * tx, fx * tx + fy * ty);
        state.left_x = state.left_y = 0;
        state.buttons = static_cast<uint16_t>(state.buttons & ~(kUp | kDown | kLeft | kRight));
        int turn = 0;
        if (std::fabs(angle) > kAligned) turn = (angle > 0) == (turnSign_ > 0) ? 1 : -1;
        // Straight behind: keep turning the way it turns, whatever the sign says there.
        if (have_ && lastTurn_ != 0 && std::fabs(angle) > 2.6f) turn = lastTurn_;
        if (turn > 0) state.buttons |= kRight;
        if (turn < 0) state.buttons |= kLeft;
        const bool walk = std::fabs(angle) < kWalk;
        if (walk) state.buttons |= kUp;
        have_ = true; lastTurn_ = turn; lastAngle_ = std::fabs(angle); lastWalk_ = walk;
        lastX_ = player.x; lastY_ = player.y; lastTx_ = tx; lastTy_ = ty;
        return true;
    }

private:
    static constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kB = 0x2000;
    static constexpr float kAligned = 0.17f;  // ~10 degrees: close enough, stop turning
    static constexpr float kWalk = 1.2f;      // ~70 degrees: walk while turning the rest

    void Forget() { have_ = false; worse_ = 0; }

    // The game's turn direction and the axes of its facing are checked against
    // what happens: if turning keeps taking the character away from a steady
    // target, or it keeps walking against one axis of its facing, that sign
    // is the other one.
    void Learn(const ModernPlayer& player, float tx, float ty) {
        if (!have_) return;
        const bool steady = lastTx_ * tx + lastTy_ * ty > 0.995f;
        const float fx = player.faceX * faceSignX_, fy = player.faceY * faceSignY_;
        const float now = std::fabs(std::atan2(fx * ty - fy * tx, fx * tx + fy * ty));
        if (lastTurn_ != 0 && steady && lastAngle_ < 2.6f) {
            if (now > lastAngle_ + 0.01f) { if (++worse_ >= 6) { turnSign_ = -turnSign_; worse_ = 0; } }
            else if (now < lastAngle_ - 0.01f) worse_ = 0;
        }
        const float mx = player.x - lastX_, my = player.y - lastY_;
        if (lastWalk_ && lastTurn_ == 0 && std::hypot(mx, my) > 0.02f) {
            const float scale = std::hypot(fx, fy), step = std::hypot(mx, my);
            Vote(mx / step, fx / scale, &againstX_, &faceSignX_);
            Vote(my / step, fy / scale, &againstY_, &faceSignY_);
        }
    }

    static void Vote(float moved, float faced, int* against, float* sign) {
        if (std::fabs(moved) < 0.4f || std::fabs(faced) < 0.4f) return;  // says nothing about this axis
        if (moved * faced < 0) { if (++*against >= 6) { *sign = -*sign; *against = 0; } }
        else *against = 0;
    }

    bool have_ = false, lastWalk_ = false;
    int lastTurn_ = 0, worse_ = 0, againstX_ = 0, againstY_ = 0;
    float lastAngle_ = 0, lastX_ = 0, lastY_ = 0, lastTx_ = 1, lastTy_ = 0;
    float turnSign_ = 1.0f, faceSignX_ = 1.0f, faceSignY_ = 1.0f;
};

}  // namespace gta2ceiling
