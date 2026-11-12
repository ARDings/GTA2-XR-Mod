// GTA2Ceiling "walk out": with the city frozen on the round play field, the
// player drives or walks off the disc onto the real floor and back in again.
//
// Nothing is faked for that. The game's own player keeps moving in the game's
// own world, with its own physics, speed and sounds; the city just stops
// following it. Two things make that work:
//
//  * Around the player, and only outside the disc, the game's map is made
//    flat ground at the player's level (only in memory, only a few tiles
//    around it, and only while frozen). Every cell gets its own column back
//    as soon as the player has moved on, so outside the disc there is nothing
//    to run into unseen, while inside it everything is exactly as drawn.
//    Coming back in anywhere is then simply driving back into the real city
//    at that very spot.
//  * Only the player's own sprites are drawn without the disc's clip (see
//    LiveGeometry::RequestFree), everything else stays inside it.
//
// The render mesh is built once per level from the map as loaded, so the
// in-memory change never reaches what is drawn. Pure functions over the
// game's DMAP layout, kept free of Windows so they can be tested on their own.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace gta2dx9 {
namespace walkout {

constexpr int kMapSize = 256;
constexpr int kCells = kMapSize * kMapSize;
constexpr int kMaxLevels = 8;
// Side face flags of a block record (left, right, top, bottom).
constexpr uint16_t kFaceWall = 0x0400;
constexpr uint16_t kFaceBulletWall = 0x0800;

// The game's in-memory DMAP: base[y][x] is a word index into the column pool;
// a column is {height, offset, pad, pad, block indices bottom up}; a block is
// 12 bytes: left, right, top, bottom, lid (u16 each), arrows, slope type.
struct MapView {
    uint32_t* base = nullptr;
    const uint8_t* columns = nullptr;
    size_t columnBytes = 0;  // readable part of the column pool
    const uint8_t* blocks = nullptr;
    size_t blockCount = 0;   // readable block records
};

struct ColumnInfo {
    int height = 0;  // top level + 1
    int offset = 0;  // lowest level
    const uint8_t* indices = nullptr;
};

inline bool ReadColumn(const MapView& map, uint32_t word, ColumnInfo* out) {
    const size_t at = static_cast<size_t>(word) * 4;
    if (!map.columns || at + 4 > map.columnBytes) return false;
    const uint8_t* p = map.columns + at;
    out->height = p[0];
    out->offset = p[1];
    const int count = out->height - out->offset;
    if (out->height > kMaxLevels || count < 0) return false;
    if (at + 4 + static_cast<size_t>(count) * 4 > map.columnBytes) return false;
    out->indices = p + 4;
    return true;
}

inline const uint8_t* BlockRecord(const MapView& map, const ColumnInfo& column, int level) {
    if (level < column.offset || level >= column.height || !map.blocks) return nullptr;
    uint32_t index = 0;
    std::memcpy(&index, column.indices + static_cast<size_t>(level - column.offset) * 4, 4);
    return index < map.blockCount ? map.blocks + static_cast<size_t>(index) * 12 : nullptr;
}

inline uint16_t Face(const uint8_t* block, int face) {  // 0 left .. 3 bottom, 4 lid
    uint16_t raw = 0;
    std::memcpy(&raw, block + face * 2, 2);
    return raw;
}

// How good a column is as the flat ground outside the disc, for ground whose
// surface is `level` (the lid of a block at level - 1). 0: unusable - it must
// end right there (nothing above, so nothing to run into), and its top block
// must be a full, flat, solid lid.
inline int FlatColumnScore(const MapView& map, uint32_t word, int level) {
    ColumnInfo column;
    if (!ReadColumn(map, word, &column)) return 0;
    if (column.height != level || column.height - column.offset < 1) return 0;
    const uint8_t* top = BlockRecord(map, column, level - 1);
    if (!top) return 0;
    if ((Face(top, 4) & 0x03FF) == 0) return 0;  // no lid
    const uint8_t arrows = top[10], slope = top[11];
    if ((slope >> 2) != 0) return 0;             // a ramp or a partial block
    const int ground = slope & 3;                // 0 air, 1 road, 2 pavement, 3 field
    if (ground == 0) return 0;
    // Nothing should be spawned out there that the player could run into
    // unseen: pedestrians are made on pavement, traffic on lanes (arrows).
    int score = 2;
    if (ground == 1 && arrows == 0) score = 10;
    else if (ground == 3 && arrows == 0) score = 8;
    else if (ground == 1) score = 6;
    else if (arrows == 0) score = 4;
    bool walls = false;
    for (int f = 0; f < 4; ++f)
        if (Face(top, f) & (kFaceWall | kFaceBulletWall)) walls = true;
    if (!walls) score += 1;
    return score;
}

struct FlatChoice {
    bool found = false;
    uint32_t word = 0;
    int cellX = 0, cellY = 0, score = 0;
};

// The best flat column of the whole map, the nearest one to (nearX, nearY)
// (cells, x east, y south) among equally good ones.
inline FlatChoice FindFlatColumn(const MapView& map, int level, float nearX, float nearY) {
    FlatChoice best;
    float bestDistance = 0.0f;
    for (int y = 0; y < kMapSize; ++y)
        for (int x = 0; x < kMapSize; ++x) {
            const uint32_t word = map.base[y * kMapSize + x];
            const int score = FlatColumnScore(map, word, level);
            if (score <= 0) continue;
            const float dx = x + 0.5f - nearX, dy = y + 0.5f - nearY;
            const float distance = dx * dx + dy * dy;
            if (best.found && (score < best.score || (score == best.score && distance >= bestDistance)))
                continue;
            best = {true, word, x, y, score};
            bestDistance = distance;
        }
    return best;
}

struct Flattening {
    bool active = false;
    uint32_t flatWord = 0;
    int level = 0;
    std::vector<uint32_t> original;  // a cell's column word from before we flattened it
    std::vector<uint8_t> mask;       // 1: flattened by us right now
    std::vector<int> cells;          // the same, as a list (only ever a few dozen)
};

inline bool Near(int x, int y, float px, float py, float radius) {
    if (radius <= 0.0f) return false;
    const float dx = x + 0.5f - px, dy = y + 0.5f - py;
    return dx * dx + dy * dy <= radius * radius;
}

// Outside the disc (centre in renderer coordinates: x east, z north =
// kMapSize - row; discRadius < 0: everywhere).
inline bool OutsideDisc(int x, int y, float centreX, float centreZ, float discRadius) {
    if (discRadius < 0.0f) return true;
    const float dx = x + 0.5f - centreX;
    const float dz = static_cast<float>(kMapSize) - (y + 0.5f) - centreZ;
    return dx * dx + dz * dz > discRadius * discRadius;
}

inline void Restore(MapView& map, Flattening& f, int i) {
    if (map.base[i] == f.flatWord) map.base[i] = f.original[i];  // else the game changed it itself
    f.mask[i] = 0;
}

struct Point { float x, y; };  // cell coordinates: x east, y south

inline bool NearAny(int x, int y, const std::vector<Point>& points, float radius) {
    for (const Point& p : points)
        if (Near(x, y, p.x, p.y, radius)) return true;
    return false;
}

// Only small windows of the map are ever flat: cells outside the disc within
// `nearRadius` of a point in `around` (the player, and the camera trailing it)
// become flat ground; flattened cells beyond `farRadius` of all of them get
// their own column back - unless something stands on them (`hold`: the
// game's cars), which would be shut inside a building. The rest of the map
// stays exactly as the game made it - flattening all of it at once left the
// game nowhere to put new cars and people, and it hung. A cell is only
// written back while it still holds what we left there, so a change the game
// made itself is never undone. Returns cells changed.
inline int Track(MapView& map, Flattening& f, float centreX, float centreZ, float discRadius,
                 const std::vector<Point>& around, float nearRadius, float farRadius,
                 const std::vector<Point>& hold, float holdRadius) {
    if (!f.active) {
        f.original.assign(kCells, 0);
        f.mask.assign(kCells, 0);
        f.cells.clear();
        f.active = true;
    }
    int changed = 0;
    for (size_t k = 0; k < f.cells.size();) {
        const int i = f.cells[k], x = i % kMapSize, y = i / kMapSize;
        const bool outside = OutsideDisc(x, y, centreX, centreZ, discRadius);
        const bool wanted = NearAny(x, y, around, outside ? farRadius : nearRadius);
        if (wanted || NearAny(x, y, hold, holdRadius)) { ++k; continue; }
        Restore(map, f, i);
        f.cells[k] = f.cells.back();
        f.cells.pop_back();
        ++changed;
    }
    for (const Point& p : around) {
        const int x0 = static_cast<int>(std::floor(p.x - nearRadius)), x1 = static_cast<int>(std::floor(p.x + nearRadius));
        const int y0 = static_cast<int>(std::floor(p.y - nearRadius)), y1 = static_cast<int>(std::floor(p.y + nearRadius));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                if (x < 1 || y < 1 || x >= kMapSize - 1 || y >= kMapSize - 1) continue;  // the border stays
                const int i = y * kMapSize + x;
                if (f.mask[i] || !Near(x, y, p.x, p.y, nearRadius) || !OutsideDisc(x, y, centreX, centreZ, discRadius))
                    continue;
                f.original[i] = map.base[i];
                map.base[i] = f.flatWord;
                f.mask[i] = 1;
                f.cells.push_back(i);
                ++changed;
            }
    }
    return changed;
}

inline int RestoreAll(MapView& map, Flattening& f) {
    int changed = 0;
    if (f.active)
        for (int i : f.cells) { Restore(map, f, i); ++changed; }
    f = Flattening{};
    return changed;
}

// Whether a cell's original column has anything at or above `level` - a
// building, a wall, a deck - that a player standing there would be inside of.
inline bool OriginalBlocksPlayer(const MapView& map, uint32_t word, int level) {
    ColumnInfo column;
    if (!ReadColumn(map, word, &column)) return false;
    for (int z = (level > column.offset ? level : column.offset); z < column.height; ++z) {
        const uint8_t* block = BlockRecord(map, column, z);
        if (!block) continue;
        if (Face(block, 4) & 0x03FF) return true;
        for (int face = 0; face < 4; ++face)
            if (Face(block, face) & (kFaceWall | kFaceBulletWall)) return true;
        if ((block[11] & 3) != 0) return true;
    }
    return false;
}

// Whether putting the original map back now would leave the player (at cell
// coordinates px, py) inside something: any flattened cell within radius.
inline bool PlayerInsideOriginal(const MapView& map, const Flattening& f, float px, float py, float radius) {
    if (!f.active) return false;
    const int x0 = static_cast<int>(std::floor(px - radius)), x1 = static_cast<int>(std::floor(px + radius));
    const int y0 = static_cast<int>(std::floor(py - radius)), y1 = static_cast<int>(std::floor(py + radius));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            if (x < 0 || y < 0 || x >= kMapSize || y >= kMapSize) continue;
            const int i = y * kMapSize + x;
            if (!f.mask[i] || !Near(x, y, px, py, radius)) continue;
            if (OriginalBlocksPlayer(map, f.original[i], f.level)) return true;
        }
    return false;
}

}  // namespace walkout
}  // namespace gta2dx9
