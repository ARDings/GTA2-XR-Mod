#include <iostream>
#include <cassert>
#include <cmath>

#include "gta2ceiling/camera_controller.h"

using namespace gta2ceiling;

void test_ceiling_and_camera_systems() {
    std::cout << "=========================================================\n";
    std::cout << "  Ceiling Camera & Comfort Presets  \n";
    std::cout << "=========================================================\n";

    CameraController cam;

    // 1. Test Comfort Presets
    std::cout << "[1/4] Verifying Comfort Presets...\n";
    cam.apply_preset(ComfortPreset::LyingDown);
    assert(cam.config().distance_meters == 2.1f);
    assert(cam.config().tilt_degrees == 12.0f);

    cam.apply_preset(ComfortPreset::Reclined);
    assert(cam.config().distance_meters == 1.8f);
    assert(cam.config().tilt_degrees == 28.0f);

    cam.apply_preset(ComfortPreset::DeskUpright);
    assert(cam.config().distance_meters == 1.5f);
    assert(cam.config().tilt_degrees == 45.0f);
    std::cout << "      -> All 3 comfort presets (Lying, Reclined, Desk) confirmed.\n";

    // 2. Camera Deadzone & Smoothing
    std::cout << "[2/4] Testing Player Tracking Camera Deadzone & Smoothing...\n";
    cam.recenter(Vec3(0, 0, 0));
    assert(cam.camera_focus().length() == 0.0f);

    // Player moves 1.5 blocks (inside 3.0 block deadzone) -> Camera must NOT move
    cam.update_tracking(Vec3(1.5f, 0.0f, 0.0f), 0.033f);
    assert(cam.camera_focus().length() == 0.0f);
    std::cout << "      -> Micro-movements within 3.0 blocks ignored (No camera jitter).\n";

    // Player moves 10.0 blocks (exceeding deadzone) -> Camera must smoothly interpolate
    cam.update_tracking(Vec3(10.0f, 0.0f, 0.0f), 0.1f);
    assert(cam.camera_focus().x > 0.0f && cam.camera_focus().x < 10.0f);
    std::cout << "      -> Smooth tracking engaged: camera moved to x=" << cam.camera_focus().x << "\n";

    // 3. Room-Fixed Placement & Recenter
    std::cout << "[3/4] Testing Room-Fixed Anchor & Recenter...\n";
    cam.recenter(Vec3(50, 0, 50));
    assert(cam.camera_focus().x == 50.0f && cam.camera_focus().z == 50.0f);
    std::cout << "      -> Instant snap on recenter confirmed.\n";

    // 4. Calibration & Distance Adjustments
    std::cout << "[4/4] Testing User Height & Distance Calibration Controls...\n";
    cam.adjust_distance(0.2f);
    assert(std::abs(cam.config().distance_meters - 1.7f) < 1e-4f);
    cam.adjust_tilt(-5.0f);
    assert(std::abs(cam.config().tilt_degrees - 40.0f) < 1e-4f);
    std::cout << "      -> Continuous analog adjustment confirmed.\n";

    std::cout << "=========================================================\n";
    std::cout << "  CEILING & CAMERA TESTS PASSED SUCCESSFULLY!            \n";
    std::cout << "=========================================================\n";
}

int main() {
    test_ceiling_and_camera_systems();
    return 0;
}

