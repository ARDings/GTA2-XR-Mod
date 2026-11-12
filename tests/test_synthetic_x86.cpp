#include <iostream>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <thread>
#include <chrono>

#include "gta2ceiling/types.h"
#include "gta2ceiling/snapshot.h"
#include "gta2ceiling/input.h"
#include "gta2ceiling/xr_transport.h"
#include "gta2ceiling/renderer.h"
#include "gta2ceiling/adapter.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace gta2ceiling;

void test_path_safety() {
    std::cout << "[TEST] Running Path Safety Check...\n";
#ifdef _WIN32
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    std::cout << "  Executable running from: " << exe_path << "\n";
    // Builds live under the work root (checked by CMakeLists.txt): the folder
    // in GTA2CEILING_WORK_ROOT, or on D: by default.
    const char* workRoot = std::getenv("GTA2CEILING_WORK_ROOT");
    if ((!workRoot || !*workRoot) && exe_path[0] != 'D' && exe_path[0] != 'd') {
        std::cerr << "FATAL: Builds go to D: by default (set GTA2CEILING_WORK_ROOT for another folder), got: " << exe_path << "\n";
        std::exit(1);
    }
#endif
    std::cout << "  -> Path Safety: PASSED\n";
}

void test_math_and_transforms() {
    std::cout << "[TEST] Running Math & Transform calculations...\n";
    Vec3 a(1.0f, 0.0f, 0.0f);
    Vec3 b(0.0f, 1.0f, 0.0f);
    Vec3 c = Vec3::cross(a, b);
    assert(std::abs(c.x) < 1e-5f && std::abs(c.y) < 1e-5f && std::abs(c.z - 1.0f) < 1e-5f);

    Mat4 t = Mat4::translation(Vec3(10, 20, 30));
    assert(t.m[12] == 10.0f && t.m[13] == 20.0f && t.m[14] == 30.0f);

    Mat4 s = Mat4::scale(Vec3(2, 3, 4));
    Mat4 st = Mat4::multiply(s, t);
    assert(st.m[0] == 2.0f && st.m[5] == 3.0f && st.m[10] == 4.0f);

    std::cout << "  -> Math & Transforms: PASSED\n";
}

void test_snapshot_timing_and_interpolation() {
    std::cout << "[TEST] Running Snapshot Timing & Decoupling test...\n";
    SnapshotRingBuffer ring;

    // Simulate Producer tick 1
    WorldSnapshot s1;
    s1.sequence_id = 1;
    s1.timestamp_seconds = 0.0;
    s1.player_position = Vec3(0, 0, 0);
    s1.player_heading_rad = 0.0f;
    ring.push_snapshot(s1);

    // Simulate Producer tick 2 (after ~33ms, typical for 30.3 Hz simulation)
    WorldSnapshot s2;
    s2.sequence_id = 2;
    s2.timestamp_seconds = 0.033;
    s2.player_position = Vec3(10, 0, 0);
    s2.player_heading_rad = 1.0f;
    ring.push_snapshot(s2);

    WorldSnapshot older, newer;
    bool has_pair = ring.get_latest_pair(older, newer);
    assert(has_pair);
    assert(older.sequence_id == 1);
    assert(newer.sequence_id == 2);

    // Interpolate at t = 0.5 (representing ~60/72Hz mid-frame)
    WorldSnapshot mid = SnapshotRingBuffer::interpolate(older, newer, 0.5f);
    assert(std::abs(mid.player_position.x - 5.0f) < 1e-4f);
    assert(std::abs(mid.player_heading_rad - 0.5f) < 1e-4f);

    // Check stall detection logic
    assert(!ring.is_simulation_stalled());

    std::cout << "  -> Snapshot Timing & Interpolation: PASSED\n";
}

void test_xbox_input_controller() {
    std::cout << "[TEST] Running Xbox Controller Mapping & Deadzone test...\n";
    InputController ctrl;

    XboxRawState raw;
    raw.is_connected = true;
    raw.thumb_lx = 0.05f; // within deadzone (< 0.15)
    raw.thumb_ly = 0.8f;  // above threshold
    raw.trigger_right = 0.75f;

    // On-foot mode
    GameActions foot = ctrl.process_input(raw, false, false);
    assert(foot.walk_forward == true);
    assert(foot.turn_left == false && foot.turn_right == false); // deadzone filtered
    assert(foot.attack == true); // trigger pressed

    // In-vehicle mode
    GameActions car = ctrl.process_input(raw, true, false);
    assert(car.accelerate == true);

    // Disconnect test
    raw.is_connected = false;
    GameActions disc = ctrl.process_input(raw, true, false);
    assert(disc.accelerate == false && disc.attack == false && disc.walk_forward == false);

    std::cout << "  -> Xbox Controller Mapping: PASSED\n";
}

void test_stereo_xr_transport() {
    std::cout << "[TEST] Running Stereo XR Transport & Projection test...\n";
    XrTransport transport;
    CeilingConfig config;

    XrApiPose hmd_pose;
    hmd_pose.position_x = 0.0f;
    hmd_pose.position_y = 1.7f; // Player eye height 1.7m
    hmd_pose.position_z = 0.0f;
    hmd_pose.orientation_w = 1.0f;

    XrApiFov fov{-0.785f, 0.785f, 0.785f, -0.785f};
    transport.update_hmd_pose(hmd_pose, 0.064f, fov, fov);

    Mat4 view_l, proj_l;
    Mat4 view_r, proj_r;
    transport.compute_eye_transforms(Eye::Left, config, view_l, proj_l);
    transport.compute_eye_transforms(Eye::Right, config, view_r, proj_r);

    // Verify left and right view matrices differ by IPD parallax
    assert(view_l.m[12] != view_r.m[12]);

    // In production this value comes from HMD_SYNC in the received UDP packet.
    // Pixel (0,0) is opaque sRGB red; XrAPI has no magic byte or focus flags.
    uint32_t sync_px = XrTransport::encode_sync_pixel(48, true, true);
    assert(((sync_px >> 24) & 0xFF) == 255);
    assert(((sync_px >> 16) & 0xFF) == 48);
    assert((sync_px & 0xFFFF) == 0);
    assert(transport.session_state() == XrSessionState::Unknown);

    std::cout << "  -> Stereo XR Transport: PASSED\n";
}

void test_synthetic_renderer() {
    std::cout << "[TEST] Running synthetic scene/recording helper checks (no GPU output)...\n";
    StereoRenderer renderer;
    CeilingConfig config;
    renderer.initialize(1920, 1080, config);
    renderer.generate_synthetic_scene();

    WorldSnapshot snap;
    snap.sequence_id = 100;
    snap.camera_center = Vec3(0, 0, 0);

    XrTransport transport;
    XrApiPose pose{0, 0, 0, 1, 0, 1.7f, 0};
    XrApiFov fov{-0.785f, 0.785f, 0.785f, -0.785f};
    transport.update_hmd_pose(pose, 0.063f, fov, fov);

    renderer.render_stereo_frame(snap, transport, 1);
    assert(renderer.frame_counter() == 1);
    assert(renderer.last_sync_pixel() != 0);

    std::cout << "  -> Synthetic scene/recording helper: PASSED (no rasterization or Quest verification)\n";
}

void test_adapter_safety() {
    std::cout << "[TEST] Running Adapter Safety & PE header verification...\n";
    // Construct synthetic valid DOS+PE image buffer
    std::vector<uint8_t> synthetic_pe(4096, 0);
    synthetic_pe[0] = 'M';
    synthetic_pe[1] = 'Z';
    // e_lfanew at 0x3C pointing to 0x80
    *reinterpret_cast<uint32_t*>(&synthetic_pe[0x3C]) = 0x80;
    synthetic_pe[0x80] = 'P';
    synthetic_pe[0x81] = 'E';
    synthetic_pe[0x82] = 0;
    synthetic_pe[0x83] = 0;

    // Place a synthetic pattern
    synthetic_pe[0x200] = 0x55; // push ebp
    synthetic_pe[0x201] = 0x8B; // mov ebp, esp
    synthetic_pe[0x202] = 0xEC;

    std::string err;
    bool is_safe = GameAdapter::verify_module_safety(synthetic_pe.data(), synthetic_pe.size(), err);
    assert(is_safe && err.empty());

    const char pattern[] = "\x55\x8B\xEC";
    const char mask[] = "xxx";
    const uint8_t* found = GameAdapter::scan_pattern(synthetic_pe.data(), synthetic_pe.size(), pattern, mask);
    assert(found == &synthetic_pe[0x200]);

    std::cout << "  -> Adapter Safety & Pattern Scanner: PASSED\n";
}

int main() {
    std::cout << "=========================================================\n";
    std::cout << "  GTA2Ceiling Synthetic x86 Helper Tests                \n";
    std::cout << "=========================================================\n";

    test_path_safety();
    test_math_and_transforms();
    test_snapshot_timing_and_interpolation();
    test_xbox_input_controller();
    test_stereo_xr_transport();
    test_synthetic_renderer();
    test_adapter_safety();

    std::cout << "=========================================================\n";
    std::cout << "  ALL SYNTHETIC x86 TESTS PASSED SUCCESSFULLY!          \n";
    std::cout << "=========================================================\n";
    return 0;
}

