#pragma once

#include "gta2ceiling/types.h"
#include "gta2ceiling/renderer.h"
#include <vector>
#include <cstdint>
#include <string>

namespace gta2ceiling {

// Slope definitions from GTA2 engine specification
enum SlopeType : uint8_t {
    SLOPE_NONE = 0,
    SLOPE_UP_26_LOW = 1,
    SLOPE_UP_26_HIGH = 2,
    SLOPE_DOWN_26_LOW = 3,
    SLOPE_DOWN_26_HIGH = 4,
    SLOPE_LEFT_26_LOW = 5,
    SLOPE_LEFT_26_HIGH = 6,
    SLOPE_RIGHT_26_LOW = 7,
    SLOPE_RIGHT_26_HIGH = 8,
    SLOPE_UP_45 = 9,
    SLOPE_DOWN_45 = 10,
    SLOPE_LEFT_45 = 11,
    SLOPE_RIGHT_45 = 12,
    SLOPE_DIAGONAL_CUSTOM = 13
};

struct BlockData {
    uint16_t left_tile = 0;
    uint16_t right_tile = 0;
    uint16_t top_tile = 0;
    uint16_t bottom_tile = 0;
    uint16_t lid_tile = 0;
    uint8_t slope_type = SLOPE_NONE;
    bool is_flat = true;
};

class GmpGeometryBuilder {
public:
    static constexpr int MAP_WIDTH = 256;
    static constexpr int MAP_HEIGHT = 256;
    static constexpr int MAP_DEPTH = 8;
    static constexpr float BLOCK_SIZE = 1.0f;

    GmpGeometryBuilder() = default;

    // Generates 3D mesh vertices for a single map block
    static void generate_block_mesh(int x, int y, int z, const BlockData& block, WorldMeshChunk& out_chunk) {
        float fx = static_cast<float>(x);
        float fy = static_cast<float>(z); // In diorama, Z is vertical height (0..7)
        float fz = static_cast<float>(y);

        Vec3 min(fx, fy, fz);
        Vec3 max = min + Vec3(BLOCK_SIZE, BLOCK_SIZE, BLOCK_SIZE);

        // 1. Lid (Roof / Ground plane)
        if (block.lid_tile > 0) {
            uint16_t base = static_cast<uint16_t>(out_chunk.vertices.size());
            uint32_t color = 0xFFCCCCCC; // Default diffuse lit

            if (block.slope_type == SLOPE_NONE) {
                out_chunk.vertices.push_back({{min.x, max.y, min.z}, {0, 1, 0}, {0, 0}, color});
                out_chunk.vertices.push_back({{max.x, max.y, min.z}, {0, 1, 0}, {1, 0}, color});
                out_chunk.vertices.push_back({{max.x, max.y, max.z}, {0, 1, 0}, {1, 1}, color});
                out_chunk.vertices.push_back({{min.x, max.y, max.z}, {0, 1, 0}, {0, 1}, color});
            } else if (block.slope_type == SLOPE_UP_45) {
                // 45 degree slope rising north (Z)
                out_chunk.vertices.push_back({{min.x, min.y, min.z}, {0, 0.707f, -0.707f}, {0, 0}, color});
                out_chunk.vertices.push_back({{max.x, min.y, min.z}, {0, 0.707f, -0.707f}, {1, 0}, color});
                out_chunk.vertices.push_back({{max.x, max.y, max.z}, {0, 0.707f, -0.707f}, {1, 1}, color});
                out_chunk.vertices.push_back({{min.x, max.y, max.z}, {0, 0.707f, -0.707f}, {0, 1}, color});
            } else {
                // Fallback flat for other slopes
                out_chunk.vertices.push_back({{min.x, max.y, min.z}, {0, 1, 0}, {0, 0}, color});
                out_chunk.vertices.push_back({{max.x, max.y, min.z}, {0, 1, 0}, {1, 0}, color});
                out_chunk.vertices.push_back({{max.x, max.y, max.z}, {0, 1, 0}, {1, 1}, color});
                out_chunk.vertices.push_back({{min.x, max.y, max.z}, {0, 1, 0}, {0, 1}, color});
            }

            out_chunk.indices.insert(out_chunk.indices.end(), {
                base, static_cast<uint16_t>(base + 1), static_cast<uint16_t>(base + 2),
                base, static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 3)
            });
        }

        // 2. Walls (Left, Right, Top, Bottom)
        if (block.left_tile > 0) {
            uint16_t base = static_cast<uint16_t>(out_chunk.vertices.size());
            uint32_t color = 0xFFAAAAAA;
            out_chunk.vertices.push_back({{min.x, min.y, min.z}, {-1, 0, 0}, {0, 0}, color});
            out_chunk.vertices.push_back({{min.x, min.y, max.z}, {-1, 0, 0}, {1, 0}, color});
            out_chunk.vertices.push_back({{min.x, max.y, max.z}, {-1, 0, 0}, {1, 1}, color});
            out_chunk.vertices.push_back({{min.x, max.y, min.z}, {-1, 0, 0}, {0, 1}, color});
            out_chunk.indices.insert(out_chunk.indices.end(), {
                base, static_cast<uint16_t>(base + 1), static_cast<uint16_t>(base + 2),
                base, static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 3)
            });
        }

        if (block.top_tile > 0) {
            uint16_t base = static_cast<uint16_t>(out_chunk.vertices.size());
            uint32_t color = 0xFF999999;
            out_chunk.vertices.push_back({{min.x, min.y, min.z}, {0, 0, -1}, {0, 0}, color});
            out_chunk.vertices.push_back({{max.x, min.y, min.z}, {0, 0, -1}, {1, 0}, color});
            out_chunk.vertices.push_back({{max.x, max.y, min.z}, {0, 0, -1}, {1, 1}, color});
            out_chunk.vertices.push_back({{min.x, max.y, min.z}, {0, 0, -1}, {0, 1}, color});
            out_chunk.indices.insert(out_chunk.indices.end(), {
                base, static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 1),
                base, static_cast<uint16_t>(base + 3), static_cast<uint16_t>(base + 2)
            });
        }
    }
};

} // namespace gta2ceiling

