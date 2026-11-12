// Walk out (frozen city): the game's map is flattened outside the disc and put
// back exactly as it was. See third_party/gta2-renderer/dll/src/walk_out.h.
#include "../third_party/gta2-renderer/dll/src/walk_out.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace gta2dx9::walkout;
namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Block { uint16_t faces[5]; uint8_t arrows, slope; };
static_assert(sizeof(Block) == 12, "a DMAP block record is 12 bytes");

struct TestMap {
    std::vector<Block> blocks;
    std::vector<uint32_t> columns;
    std::vector<uint32_t> base;
    uint32_t Column(int height, int offset, std::vector<uint32_t> indices) {
        const uint32_t word = static_cast<uint32_t>(columns.size());
        columns.push_back(static_cast<uint32_t>(height) | (static_cast<uint32_t>(offset) << 8));
        columns.insert(columns.end(), indices.begin(), indices.end());
        return word;
    }
    MapView View() {
        MapView view;
        view.base = base.data();
        view.columns = reinterpret_cast<const uint8_t*>(columns.data());
        view.columnBytes = columns.size() * 4;
        view.blocks = reinterpret_cast<const uint8_t*>(blocks.data());
        view.blockCount = blocks.size();
        return view;
    }
};
}  // namespace

int main() {
    try {
        TestMap map;
        map.blocks = {
            {{0, 0, 0, 0, 0}, 0, 0},                      // 0 empty
            {{0, 0, 0, 0, 5}, 0x10, 1},                   // 1 road with a lane
            {{0, 0, 0, 0, 7}, 0, 1},                      // 2 road, no lane
            {{0x405, 0x405, 0x405, 0x405, 0}, 0, 0},      // 3 building walls
            {{0, 0, 0, 0, 9}, 0, 2},                      // 4 roof (pavement)
            {{0, 0, 0, 0, 7}, 0, static_cast<uint8_t>(1 | (1 << 2))},  // 5 ramp
        };
        const uint32_t lane = map.Column(2, 1, {1});
        const uint32_t road = map.Column(2, 1, {2});
        const uint32_t building = map.Column(5, 1, {1, 3, 3, 4});
        const uint32_t ramp = map.Column(2, 1, {5});
        map.base.assign(kCells, lane);
        for (int y = 100; y < 110; ++y)
            for (int x = 100; x < 110; ++x) map.base[y * kMapSize + x] = building;
        map.base[50 * kMapSize + 60] = road;
        map.base[200 * kMapSize + 200] = road;
        map.base[10 * kMapSize + 10] = ramp;
        MapView view = map.View();

        Require(FlatColumnScore(view, road, 2) > FlatColumnScore(view, lane, 2), "a road without lanes is preferred");
        Require(FlatColumnScore(view, lane, 2) > 0, "a road with lanes still works");
        Require(FlatColumnScore(view, building, 2) == 0, "a building is never flat ground");
        Require(FlatColumnScore(view, ramp, 2) == 0, "a ramp is never flat ground");
        Require(FlatColumnScore(view, road, 3) == 0, "flat ground must end at the player's level");
        FlatChoice choice = FindFlatColumn(view, 2, 190, 190);
        Require(choice.found && choice.word == road && choice.cellX == 200, "nearest best column");

        // Disc around cell (105,105): renderer x 105, z 256 - 105, radius 8.
        const std::vector<uint32_t> before = map.base;
        Flattening flat;
        flat.flatWord = road;
        flat.level = 2;
        // Player at the edge of the disc, cell (112,105): only outside cells near it.
        Track(view, flat, 105, 151, 8, {{112.5f, 105.5f}}, 3.5f, 5.0f, {}, 0);
        Require(!flat.cells.empty() && flat.cells.size() < 50, "a small window only");
        Require(map.base[105 * kMapSize + 115] == road, "outside next to the player is flat");
        Require(map.base[105 * kMapSize + 109] == building, "inside the disc stays");
        Require(map.base[105 * kMapSize + 135] == lane, "far away stays");
        // The game changes one of the flattened cells itself meanwhile.
        map.base[105 * kMapSize + 114] = ramp;
        // The player drives on: what is behind it goes back.
        for (float x = 112.5f; x < 160.0f; x += 0.3f) Track(view, flat, 105, 151, 8, {{x, 105.5f}}, 3.5f, 5.0f, {{130.5f, 105.5f}}, 1.2f);
        Require(map.base[105 * kMapSize + 130] == road, "a cell a car stands on stays flat");
        Track(view, flat, 105, 151, 8, {{159.5f, 105.5f}}, 3.5f, 5.0f, {}, 0);
        Require(map.base[105 * kMapSize + 115] == lane, "behind the player it is back");
        Require(map.base[105 * kMapSize + 159] == road, "around the player it is flat");
        Require(!PlayerInsideOriginal(view, flat, 159.5f, 105.5f, 1.5f), "open street outside");
        RestoreAll(view, flat);
        Require(!flat.active, "restored");
        for (int i = 0; i < kCells; ++i) {
            if (i == 105 * kMapSize + 114) Require(map.base[i] == ramp, "the game's own change is kept");
            else Require(map.base[i] == before[i], "everything else is exactly as before");
        }
        // Released while standing where the original map has a building.
        Flattening other;
        other.flatWord = road;
        other.level = 2;
        Track(view, other, 0, 0, -1, {{105, 105}}, 3.5f, 5.0f, {}, 0);
        Require(map.base[105 * kMapSize + 105] == road, "everywhere around the player while released");
        Require(PlayerInsideOriginal(view, other, 105, 105, 1.5f), "inside a building");
        RestoreAll(view, other);
        std::cout << "test_walk_out passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
