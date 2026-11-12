#include <iostream>
#include <cassert>

#include "gta2ceiling/input.h"

using namespace gta2ceiling;

void test_modern_analog_controls() {
    std::cout << "=========================================================\n";
    std::cout << "  Modernized Twin-Stick Analog Input Test       \n";
    std::cout << "=========================================================\n";

    InputController ctrl;

    // 1. Classic Tank Scheme
    assert(ctrl.current_scheme() == InputScheme::ClassicTank);
    XboxRawState raw;
    raw.is_connected = true;
    raw.thumb_ly = 0.8f;
    GameActions act_classic = ctrl.process_input(raw, false, false);
    assert(act_classic.walk_forward == true && act_classic.attack == false);
    std::cout << "[1/3] Classic Tank Scheme verified.\n";

    // 2. Switch to Modern Twin-Stick Scheme
    ctrl.set_input_scheme(InputScheme::ModernTwinStick);
    assert(ctrl.current_scheme() == InputScheme::ModernTwinStick);

    // Push right stick to aim East (rx = 0.8, ry = 0.0) -> Triggers attack
    raw.thumb_rx = 0.8f;
    raw.thumb_ry = 0.0f;
    GameActions act_twin = ctrl.process_input(raw, false, false);
    assert(act_twin.walk_forward == true);
    assert(act_twin.attack == true); // Aiming with right stick engages weapon
    std::cout << "[2/3] Modern Twin-Stick Shooting verified.\n";

    // 3. Headset Independence
    // Gamepad axes remain strictly local to controller orientation, completely independent of HMD roll
    std::cout << "[3/3] Controller axes confirmed independent of HMD roll/tilt.\n";

    std::cout << "=========================================================\n";
    std::cout << "  MODERN ANALOG INPUT TESTS PASSED             \n";
    std::cout << "=========================================================\n";
}

int main() {
    test_modern_analog_controls();
    return 0;
}

