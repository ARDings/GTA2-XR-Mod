#include <iostream>
#include <cassert>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>

#include "gta2ceiling/types.h"
#include "gta2ceiling/snapshot.h"
#include "gta2ceiling/xr_transport.h"
#include "gta2ceiling/renderer.h"

using namespace gta2ceiling;

// Host scheduling model only: no product simulation, GPU or headset compositor.
void run_snapshot_scheduling_model_test() {
    std::cout << "=========================================================\n";
    std::cout << "  CPU snapshot producer/consumer scheduling model       \n";
    std::cout << "=========================================================\n";

    SnapshotRingBuffer ring_buffer;
    StereoRenderer renderer;
    CeilingConfig config;
    renderer.initialize(1920, 1080, config);

    std::atomic<bool> running{true};
    std::atomic<uint64_t> sim_ticks{0};
    std::atomic<uint64_t> render_frames{0};
    std::atomic<bool> stall_injected{false};
    std::atomic<bool> stall_detected{false};

    // Synthetic producer requests 33 ms scheduling intervals on the host OS.
    std::thread producer([&]() {
        uint64_t seq = 0;
        auto next_tick = std::chrono::steady_clock::now();
        const auto tick_interval = std::chrono::microseconds(33000); // ~30.3 Hz

        while (running) {
            seq++;
            sim_ticks++;

            WorldSnapshot snap;
            snap.sequence_id = seq;
            snap.timestamp_seconds = seq * 0.033;
            snap.player_position = Vec3(static_cast<float>(seq), 0.0f, 0.0f);
            snap.camera_center = snap.player_position;

            ring_buffer.push_snapshot(snap);

            // Inject artificial 500ms stall around tick 30
            if (seq == 30 && !stall_injected) {
                stall_injected = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            next_tick += tick_interval;
            std::this_thread::sleep_until(next_tick);
        }
    });

    // Synthetic consumer requests 13.888 ms intervals; no compositor runs here.
    std::thread consumer([&]() {
        XrTransport xr;
        XrApiFov fov{-0.785f, 0.785f, 0.785f, -0.785f};
        uint8_t frame_seq = 0;

        auto next_frame = std::chrono::steady_clock::now();
        const auto frame_interval = std::chrono::microseconds(13888); // 72 Hz

        while (running) {
            frame_seq++;
            render_frames++;

            // Inject a constant synthetic pose; no HMD is sampled.
            XrApiPose pose{0, 0, 0, 1, 0, 1.7f, 0};
            xr.update_hmd_pose(pose, config.ipd_meters, fov, fov);

            // Check stall state
            if (stall_injected && ring_buffer.is_simulation_stalled()) {
                stall_detected = true;
            }

            WorldSnapshot older, newer;
            if (ring_buffer.get_latest_pair(older, newer)) {
                // Interpolate
                WorldSnapshot interp = SnapshotRingBuffer::interpolate(older, newer, 0.5f);
                // The helper receives one interpolated snapshot for this call.
                assert(interp.sequence_id == newer.sequence_id);
                renderer.render_stereo_frame(interp, xr, frame_seq);
            }

            next_frame += frame_interval;
            std::this_thread::sleep_until(next_frame);
        }
    });

    // Run the two test threads concurrently for 2.0 seconds.
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    // Shutdown threads
    running = false;
    producer.join();
    consumer.join();

    std::cout << "[RESULTS] Host test-model counters:\n";
    std::cout << "  Synthetic producer iterations: " << sim_ticks.load() << "\n";
    std::cout << "  Synthetic consumer iterations: " << render_frames.load() << "\n";
    std::cout << "  Artificial 500ms Stall Injected: " << (stall_injected.load() ? "YES" : "NO") << "\n";
    std::cout << "  Stale-data flag observed:        " << (stall_detected.load() ? "YES" : "NO") << "\n";

    // Verifications
    assert(sim_ticks.load() > 0);
    assert(render_frames.load() > sim_ticks.load()); // Different requested host cadences.
    assert(stall_detected.load() == true);            // 500ms stall must be caught

    std::cout << "=========================================================\n";
    std::cout << "  CPU scheduling-model checks passed.                  \n";
    std::cout << "  No G2 gate, GTA2 timing or headset rate verified.     \n";
    std::cout << "=========================================================\n";
}

int main() {
    run_snapshot_scheduling_model_test();
    return 0;
}
