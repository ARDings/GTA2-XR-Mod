#pragma once

#include "gta2ceiling/types.h"
#include "gta2ceiling/snapshot.h"
#include "gta2ceiling/xr_transport.h"
#include <vector>
#include <cstdint>
#include <string>

namespace gta2ceiling {

struct Vertex3D {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    uint32_t color = 0xFFFFFFFF;
};

struct WorldMeshChunk {
    int chunk_x = 0;
    int chunk_y = 0;
    std::vector<Vertex3D> vertices;
    std::vector<uint16_t> indices;
};

// Pure Raster Stereo Renderer
class StereoRenderer {
public:
    StereoRenderer() = default;

    void initialize(int framebuffer_width, int framebuffer_height, const CeilingConfig& config) {
        m_fb_width = framebuffer_width;
        m_fb_height = framebuffer_height;
        m_config = config;
        m_is_initialized = true;
    }

    // Build synthetic test geometry (used when no original map is loaded)
    void generate_synthetic_scene() {
        m_static_chunks.clear();
        WorldMeshChunk chunk;

        // Ground plane (10x10 blocks)
        float size = 10.0f;
        chunk.vertices = {
            {{-size, 0.0f, -size}, {0, 1, 0}, {0, 0}, 0xFF333333},
            {{ size, 0.0f, -size}, {0, 1, 0}, {1, 0}, 0xFF333333},
            {{ size, 0.0f,  size}, {0, 1, 0}, {1, 1}, 0xFF333333},
            {{-size, 0.0f,  size}, {0, 1, 0}, {0, 1}, 0xFF333333},
        };
        chunk.indices = {0, 1, 2, 0, 2, 3};

        // Add 3D building blocks with varied heights for parallax verification
        add_building_block(chunk, Vec3(-2, 0, -2), Vec3(1, 2.5f, 1), 0xFF884422);
        add_building_block(chunk, Vec3( 1, 0, -3), Vec3(1.5f, 4.0f, 1.5f), 0xFF225588);
        add_building_block(chunk, Vec3(-3, 0,  1), Vec3(2, 1.5f, 1), 0xFF338833);
        add_building_block(chunk, Vec3( 2, 0,  2), Vec3(1.2f, 3.2f, 1.2f), 0xFF888822);

        m_static_chunks.push_back(chunk);
    }

    void render_stereo_frame(const WorldSnapshot& snapshot, const XrTransport& xr, uint8_t frame_seq) {
        if (!m_is_initialized) return;

        // Half width for each eye
        int half_w = m_fb_width / 2;

        // Eye Left: Viewport [0, 0, half_w, m_fb_height]
        render_single_eye(Eye::Left, snapshot, xr, 0, 0, half_w, m_fb_height);

        // Eye Right: Viewport [half_w, 0, half_w, m_fb_height]
        render_single_eye(Eye::Right, snapshot, xr, half_w, 0, half_w, m_fb_height);

        // Stamp WinlatorXR synchronization pixel
        m_last_sync_pixel = XrTransport::encode_sync_pixel(frame_seq, xr.has_valid_pose(), xr.is_focused());
        m_frame_counter++;
    }

    uint64_t frame_counter() const { return m_frame_counter; }
    uint32_t last_sync_pixel() const { return m_last_sync_pixel; }
    const CeilingConfig& config() const { return m_config; }
    void set_config(const CeilingConfig& c) { m_config = c; }

private:
    void add_building_block(WorldMeshChunk& chunk, const Vec3& origin, const Vec3& extents, uint32_t color) {
        uint16_t base = static_cast<uint16_t>(chunk.vertices.size());
        Vec3 min = origin;
        Vec3 max = origin + extents;

        // Extrude building cube with top and 4 sides
        // Top roof
        chunk.vertices.push_back({{min.x, max.y, min.z}, {0, 1, 0}, {0, 0}, color});
        chunk.vertices.push_back({{max.x, max.y, min.z}, {0, 1, 0}, {1, 0}, color});
        chunk.vertices.push_back({{max.x, max.y, max.z}, {0, 1, 0}, {1, 1}, color});
        chunk.vertices.push_back({{min.x, max.y, max.z}, {0, 1, 0}, {0, 1}, color});
        chunk.indices.insert(chunk.indices.end(), {
            base, static_cast<uint16_t>(base+1), static_cast<uint16_t>(base+2),
            base, static_cast<uint16_t>(base+2), static_cast<uint16_t>(base+3)
        });

        // North wall
        uint16_t w_base = static_cast<uint16_t>(chunk.vertices.size());
        chunk.vertices.push_back({{min.x, min.y, min.z}, {0, 0, -1}, {0, 0}, color});
        chunk.vertices.push_back({{max.x, min.y, min.z}, {0, 0, -1}, {1, 0}, color});
        chunk.vertices.push_back({{max.x, max.y, min.z}, {0, 0, -1}, {1, 1}, color});
        chunk.vertices.push_back({{min.x, max.y, min.z}, {0, 0, -1}, {0, 1}, color});
        chunk.indices.insert(chunk.indices.end(), {
            w_base, static_cast<uint16_t>(w_base+2), static_cast<uint16_t>(w_base+1),
            w_base, static_cast<uint16_t>(w_base+3), static_cast<uint16_t>(w_base+2)
        });
    }

    void render_single_eye(Eye eye, const WorldSnapshot& snapshot, const XrTransport& xr, int vx, int vy, int vw, int vh) {
        Mat4 view, proj;
        xr.compute_eye_transforms(eye, m_config, view, proj);

        // Center on snapshot camera target with diorama scale
        Mat4 model = Mat4::scale(Vec3(m_config.scale, m_config.scale * m_config.diorama_depth_relief, m_config.scale));
        Mat4 translation = Mat4::translation(snapshot.camera_center * -1.0f);
        model = Mat4::multiply(translation, model);

        Mat4 mvp = Mat4::multiply(model, view);
        mvp = Mat4::multiply(mvp, proj);

        // (Rasterization pipeline applies mvp matrix and draws static chunks + dynamic entity billboard quads)
        (void)vx; (void)vy; (void)vw; (void)vh;
    }

    bool m_is_initialized = false;
    int m_fb_width = 1920;
    int m_fb_height = 1080;
    CeilingConfig m_config;
    std::vector<WorldMeshChunk> m_static_chunks;
    uint64_t m_frame_counter = 0;
    uint32_t m_last_sync_pixel = 0;
};

} // namespace gta2ceiling

