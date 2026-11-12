// Modern controls: stick direction on foot, triggers in a car. See
// src/mod/modern_controls.h.
#include "../src/mod/modern_controls.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace gta2ceiling;
namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
constexpr uint16_t kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kB = 0x2000;

// A character that turns `turnRate` per key tick (positive: the right key
// turns clockwise on a north-up map) and walks along `walkSign` * facing.
struct Walker {
    float angle = 0, x = 100, y = 100, turnRate = 0.12f, walkSign = 1, flipY = 1;
    ModernPlayer Player() const {
        ModernPlayer p; p.known = true; p.x = x; p.y = y;
        p.faceX = walkSign * std::sin(angle); p.faceY = -walkSign * flipY * std::cos(angle);
        return p;
    }
    void Step(const GamepadState& s) {
        if (s.buttons & kRight) angle += turnRate;
        if (s.buttons & kLeft) angle -= turnRate;
        if (s.buttons & kUp) { x += 0.05f * std::sin(angle); y -= 0.05f * std::cos(angle); }
    }
};

float Walk(Walker walker, float stickX, float stickY, const StickToMap& view, float* endX, float* endY) {
    ModernControls controls;
    const float startX = walker.x, startY = walker.y;
    for (int i = 0; i < 400; ++i) {
        GamepadState s;
        s.left_x = static_cast<int16_t>(stickX * 30000); s.left_y = static_cast<int16_t>(stickY * 30000);
        Require(controls.Apply(s, walker.Player(), view, 7849, 30), "applied on foot");
        Require(s.left_x == 0 && s.left_y == 0, "the stick itself is consumed");
        walker.Step(s);
    }
    *endX = walker.x - startX; *endY = walker.y - startY;
    return std::hypot(*endX, *endY);
}
}  // namespace

int main() {
    try {
        // A plain top-down view: east is right, north is up.
        const float o[2] = {0, 0}, e[2] = {0.1f, 0}, n[2] = {0, 0.1f};
        const StickToMap top = SolveStickToMap(o, e, n);
        Require(top.valid && top.m[0][0] > 0 && top.m[1][1] < 0, "stick up is north (y south negative)");
        float dx = 0, dy = 0;
        Require(Walk({}, 1, 0, top, &dx, &dy) > 10 && dx > 0.95f * std::hypot(dx, dy), "stick right walks east");
        Require(Walk({}, 0, -1, top, &dx, &dy) > 10 && dy > 0.9f * std::hypot(dx, dy), "stick down walks south");
        // The game turning the other way, or reporting the facing reversed, is learnt.
        Walker mirrored; mirrored.turnRate = -0.12f;
        Require(Walk(mirrored, 1, 0, top, &dx, &dy) > 10 && dx > 0.9f * std::hypot(dx, dy), "opposite turn keys");
        Walker reversed; reversed.walkSign = -1;
        Require(Walk(reversed, 1, 0, top, &dx, &dy) > 10 && dx > 0.9f * std::hypot(dx, dy), "reversed facing");
        Walker flipped; flipped.flipY = -1;  // the facing's y axis the other way round
        Require(Walk(flipped, 0.7f, 0.7f, top, &dx, &dy) > 8 && dx > 0.5f * std::hypot(dx, dy) && dy < -0.5f * std::hypot(dx, dy),
                "mirrored facing axis");
        // Seen from below (the ceiling): east appears on the left.
        const float eMirror[2] = {-0.1f, 0};
        const StickToMap below = SolveStickToMap(o, eMirror, n);
        Require(Walk({}, 1, 0, below, &dx, &dy) > 10 && dx < -0.95f * std::hypot(dx, dy), "stick right walks west there");
        // A degenerate view, an unknown player, a resting stick: classic.
        Require(!SolveStickToMap(o, e, e).valid, "edge-on view has no mapping");
        ModernControls controls;
        GamepadState s; s.left_y = 30000;
        Require(!controls.Apply(s, ModernPlayer{}, top, 7849, 30) && s.left_y == 30000, "unknown player: untouched");
        GamepadState rest; rest.buttons = kLeft;
        Require(!controls.Apply(rest, Walker{}.Player(), top, 7849, 30) && rest.buttons == kLeft, "d-pad stays classic");
        // In a car: RT gas, LT brake, B fires, the stick still steers.
        ModernPlayer car; car.known = true; car.inCar = true;
        GamepadState drive; drive.right_trigger = 200; drive.left_x = 20000; drive.buttons = kB;
        Require(controls.Apply(drive, car, top, 7849, 30), "applied in a car");
        Require((drive.buttons & kUp) && !(drive.buttons & kB) && drive.right_trigger == 255 && drive.left_x == 20000,
                "gas on RT, fire on B, steering kept");
        GamepadState brake; brake.left_trigger = 200;
        controls.Apply(brake, car, top, 7849, 30);
        Require((brake.buttons & kDown) && brake.left_trigger == 0 && brake.right_trigger == 0, "brake on LT");
        std::cout << "test_modern_controls passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
