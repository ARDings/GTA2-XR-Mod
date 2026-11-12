#include <iostream>
#include <cassert>
#include "gta2ceiling/gmp_parser.h"
#include "gta2ceiling/renderer.h"

using namespace gta2ceiling;

void run_raster_renderer_spike_test() {
    std::cout << "=========================================================\n";
    std::cout << "  Pure Raster Renderer Spike Test               \n";
    std::cout << "=========================================================\n";

    WorldMeshChunk chunk;

    // 1. Generate road block (flat lid)
    BlockData road;
    road.lid_tile = 10;
    road.slope_type = SLOPE_NONE;
    GmpGeometryBuilder::generate_block_mesh(0, 0, 0, road, chunk);

    // 2. Generate ramp block (45-degree slope)
    BlockData ramp;
    ramp.lid_tile = 11;
    ramp.slope_type = SLOPE_UP_45;
    GmpGeometryBuilder::generate_block_mesh(0, 1, 0, ramp, chunk);

    // 3. Generate 3-story building block with left and top walls
    BlockData building;
    building.lid_tile = 20;
    building.left_tile = 21;
    building.top_tile = 22;
    building.slope_type = SLOPE_NONE;
    GmpGeometryBuilder::generate_block_mesh(1, 0, 2, building, chunk);

    std::cout << "[1/3] Geometry Generation:\n";
    std::cout << "  Vertices generated: " << chunk.vertices.size() << "\n";
    std::cout << "  Indices generated:  " << chunk.indices.size() << "\n";

    assert(chunk.vertices.size() > 0);
    assert(chunk.indices.size() % 3 == 0); // Must form valid triangle soup

    // 4. Test Rasterizer Pipeline without RTX
    std::cout << "[2/3] Initializing Pure Direct3D 9 / OpenGL Rasterizer (No RTX)...\n";
    StereoRenderer renderer;
    CeilingConfig config;
    config.distance_meters = 2.0f;
    config.scale = 0.08f;
    config.tilt_degrees = 15.0f;
    renderer.initialize(2048, 1024, config);

    WorldSnapshot snap;
    snap.sequence_id = 1;
    snap.camera_center = Vec3(0, 1, 0);

    XrTransport xr;
    XrApiPose pose{0, 0, 0, 1, 0, 1.7f, 0};
    XrApiFov fov{-0.785f, 0.785f, 0.785f, -0.785f};
    xr.update_hmd_pose(pose, 0.063f, fov, fov);

    renderer.render_stereo_frame(snap, xr, 1);
    assert(renderer.frame_counter() == 1);
    std::cout << "  -> Rendered Stereo Frame 1 successfully. Sync pixel: 0x"
              << std::hex << renderer.last_sync_pixel() << std::dec << "\n";

    // 5. Performance benchmark for 1000 frames
    std::cout << "[3/3] Benchmarking Rasterizer Throughput (1000 stereo passes)...\n";
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 1000; ++i) {
        renderer.render_stereo_frame(snap, xr, static_cast<uint8_t>(i));
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = end - start;
    double avg_frame_ms = elapsed.count() / 1000.0;
    double potential_fps = 1000.0 / avg_frame_ms;

    std::cout << "  -> 1000 frames completed in " << elapsed.count() << " ms\n";
    std::cout << "  -> Avg CPU time per Stereo Frame: " << avg_frame_ms << " ms (~" << potential_fps << " FPS)\n";
    assert(potential_fps > 500.0); // Extremely fast CPU budget, leaving abundant room for Snapdragon XR2 Gen 2

    std::cout << "=========================================================\n";
    std::cout << "  RASTER RENDERER SPIKE COMPLETED SUCCESSFULLY \n";
    std::cout << "=========================================================\n";
}

int main() {
    run_raster_renderer_spike_test();
    return 0;
}

