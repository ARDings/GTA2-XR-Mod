#include <iostream>
#include <cassert>
#include "gta2ceiling/input.h"

using namespace gta2ceiling;

void run_controller_test_suite() {
    std::cout << "=========================================================\n";
    std::cout << "  Xbox Bluetooth Controller Verification Suite  \n";
    std::cout << "=========================================================\n";

    InputController ctrl;

    // 1. Single Action Recognition & No-Double-Triggering Test
    std::cout << "[1/4] Testing Button & Trigger isolation...\n";
    XboxRawState raw;
    raw.is_connected = true;

    // Test A button (Enter/Select in menu, Accelerate in vehicle, Jump on foot)
    raw.btn_a = true;
    GameActions act_menu = ctrl.process_input(raw, false, true);
    assert(act_menu.menu_confirm == true && act_menu.menu_cancel == false);

    GameActions act_car = ctrl.process_input(raw, true, false);
    assert(act_car.accelerate == true && act_car.reverse_brake == false);

    raw.btn_a = false;
    raw.trigger_left = 0.8f; // Left trigger: Brake/Reverse
    GameActions act_brake = ctrl.process_input(raw, true, false);
    assert(act_brake.reverse_brake == true && act_brake.accelerate == false);

    // 2. Deadzone Filtering Test
    std::cout << "[2/4] Testing Analog Stick Deadzone Filtering...\n";
    raw.trigger_left = 0.0f;
    raw.thumb_lx = 0.10f; // Below 0.15 deadzone
    raw.thumb_ly = 0.12f; // Below 0.15 deadzone
    GameActions act_deadzone = ctrl.process_input(raw, false, false);
    assert(!act_deadzone.walk_forward && !act_deadzone.turn_right);

    raw.thumb_lx = 0.60f; // Above deadzone
    GameActions act_turn = ctrl.process_input(raw, false, false);
    assert(act_turn.turn_right == true);

    // 3. Five Consecutive Disconnect / Reconnect Cycles
    std::cout << "[3/4] Testing 5 Disconnect / Reconnect Recovery Cycles...\n";
    for (int cycle = 1; cycle <= 5; ++cycle) {
        // Connected with active forward movement and shooting
        raw.is_connected = true;
        raw.thumb_ly = 0.9f;
        raw.btn_b = true;
        GameActions act_active = ctrl.process_input(raw, false, false);
        assert(act_active.walk_forward == true && act_active.attack == true);

        // Sudden Bluetooth Disconnect event
        raw.is_connected = false;
        GameActions act_disconnected = ctrl.process_input(raw, false, false);
        // All actions must be immediately released to prevent stuck characters or runaway cars
        assert(act_disconnected.walk_forward == false);
        assert(act_disconnected.attack == false);
        assert(act_disconnected.accelerate == false);
        assert(act_disconnected.reverse_brake == false);

        // Reconnect event
        raw.is_connected = true;
        raw.btn_b = false;
        raw.thumb_ly = 0.0f;
        GameActions act_idle = ctrl.process_input(raw, false, false);
        assert(!act_idle.walk_forward && !act_idle.attack);
    }
    std::cout << "      -> 5/5 Disconnect/Reconnect cycles passed without stuck state.\n";

    // 4. System Reserved Keys Verification
    std::cout << "[4/4] Verifying System Reserved Keys handling...\n";
    // Guide/Xbox button is marked system-reserved (handled by Quest shell, not passed to game)
    raw.btn_guide = true;
    raw.btn_back = true; // Used for Recenter Diorama
    GameActions act_recenter = ctrl.process_input(raw, false, false);
    assert(act_recenter.recenter_diorama == true);

    std::cout << "=========================================================\n";
    std::cout << "  ALL CONTROLLER TESTS PASSED        \n";
    std::cout << "=========================================================\n";
}

int main() {
    run_controller_test_suite();
    return 0;
}

