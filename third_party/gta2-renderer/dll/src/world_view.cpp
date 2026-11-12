#include "world_view.h"
#include "ceiling_mask.h"
#include "../../../../src/mod/graphics_quality.h"
#include "spatial_batches.h"
#include "../../src/dynamic_draw.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <psapi.h>
#include <unordered_map>
#include <string>
#include <vector>

#include "../../src/gta2_style.h"
#include "../../src/world_mesh.h"
#include "debug_overlay.h"
#include "frame_limiter.h"
#include "game_access.h"
#include "light_bench.h"
#include "log.h"
#include "remix_api.h"
#include "remix_lights.h"
#include "synthetic_lights.h"
#include "texture_store.h"
#include "time_of_day.h"

namespace gta2dx9 {
namespace {

constexpr uint32_t kMaxColumnWords = 1 << 20;
constexpr uint32_t kMaxBlocks = 1 << 18;
constexpr size_t kMatchBlocks = 4000;

// Per-frame easing toward the street height under the camera.
constexpr float kHeightFollowRate = 0.06f;

// ---------------------------------------------------------------------------
// The one patch gta2.exe needs to reason about a wide frame, and the switch
// that takes it back out. The sites keep the bytes they had, so
// spawn_offscreen=0 - or the checkbox in the menu, mid-game - leaves the running
// process byte for byte as it shipped.
// ---------------------------------------------------------------------------

bool g_spawnOffscreen = kDefaultSpawnOffscreen;
bool g_spawnOffscreenActive = false;

// One patched run of bytes and what was there before it.
struct BytePatch {
    uintptr_t at = 0;
    size_t size = 0;
    uint8_t original[8] = {};
    bool applied = false;
};

bool WriteCode(uintptr_t at, const void* bytes, size_t size) {
    void* target = reinterpret_cast<void*>(at);
    DWORD previous = 0;
    if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &previous)) return false;
    memcpy(target, bytes, size);
    VirtualProtect(target, size, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return true;
}

// Saves what is there, then writes. A site that does not hold what it is
// supposed to is left alone rather than half patched - that is the guard
// against a different build of gta2.exe.
bool ApplyPatch(BytePatch* patch, uintptr_t at, const void* expected, const void* bytes,
                size_t size, const char* what) {
    if (patch->applied) return true;
    if (size > sizeof(patch->original)) return false;
    if (memcmp(reinterpret_cast<const void*>(at), expected, size) != 0) {
        Log("%s: %08X does not hold what this expects - leaving gta2.exe alone", what,
            static_cast<unsigned>(at));
        return false;
    }
    memcpy(patch->original, reinterpret_cast<const void*>(at), size);
    if (!WriteCode(at, bytes, size)) {
        Log("%s: could not write %08X", what, static_cast<unsigned>(at));
        return false;
    }
    patch->at = at;
    patch->size = size;
    patch->applied = true;
    return true;
}

void RevertPatch(BytePatch* patch) {
    if (!patch->applied) return;
    WriteCode(patch->at, patch->original, patch->size);
    patch->applied = false;
}

// The two Fixed values FUN_0041E7A0 reads in place of its own 0.5 and 0.75 once
// its operands point here. See game_access.h kViewHalfScaleSite. They hold the
// game's own values until a frame says otherwise, so a patch applied before the
// first frame changes nothing.
int32_t g_viewHalfScale = game::kViewHalfScaleStock;
int32_t g_viewAspect = game::kViewAspectStock;
BytePatch g_viewHalfScalePatch;
BytePatch g_viewAspectPatch;

bool ApplyGamePatches() {
    struct Site {
        BytePatch* patch;
        uintptr_t at;
        uintptr_t expected;
        int32_t* replacement;
        const char* what;
    };
    const Site sites[2] = {
        {&g_viewHalfScalePatch, game::kViewHalfScaleSite, game::kViewHalfScale, &g_viewHalfScale,
         "view width"},
        {&g_viewAspectPatch, game::kViewAspectSite, game::kViewAspect, &g_viewAspect,
         "view aspect"},
    };
    bool ok = true;
    for (const Site& site : sites) {
        if (*reinterpret_cast<const uint8_t*>(site.at - 1) != 0x68) {
            Log("%s: %08X is not a PUSH", site.what, static_cast<unsigned>(site.at));
            ok = false;
            break;
        }
        ok &= ApplyPatch(site.patch, site.at, &site.expected, &site.replacement, sizeof(void*),
                         site.what);
    }
    // Width without height, or the other way round, is a wrong rectangle rather
    // than a stock one - so it is both or neither.
    if (!ok) {
        RevertPatch(&g_viewHalfScalePatch);
        RevertPatch(&g_viewAspectPatch);
    }
    Log("spawn offscreen: %s", ok ? "gta2.exe patched" : "not applied, see above");
    return ok;
}

void RevertGamePatches() {
    RevertPatch(&g_viewHalfScalePatch);
    RevertPatch(&g_viewAspectPatch);
    Log("spawn offscreen: gta2.exe put back the way it shipped");
}

constexpr float kDegreesToRadians = 3.14159265f / 180.0f;

// The desktop, in the physical pixels a window is really sized in.
//
// Three APIs answer "how big is the screen" and on a scaled desktop they
// disagree. Measured here on a 1920x1080 display at 125%:
//
//   EnumDisplaySettings(ENUM_CURRENT_SETTINGS)   1920x1080   the mode in force
//   EnumDisplaySettings(ENUM_REGISTRY_SETTINGS)  3840x2400   a mode from months ago
//   GetSystemMetrics / MONITORINFO               1536x864    the DPI-scaled desktop
//
// CreateWindowEx sizes land on screen as physical pixels, so the current display
// mode is the one to build a present window from. The other two each broke it in
// their own direction:
//
//   - the registry mode is the display's *persistent* mode, not the mode it is
//     in. It was preferred here, on the reasoning that ENUM_CURRENT_SETTINGS
//     reads back GTA2's own 640x480 once its video device has taken the display.
//     It does survive that - but it also survived the desktop being moved from
//     3840x2400 down to 1920x1080, so the present window was built at twice the
//     size of the screen and only its top-left quarter was ever visible. That is
//     what a front end "zoomed in" is.
//   - GetSystemMetrics reports the desktop as a DPI-unaware process may address
//     it, which is 4/5 of the real thing at 125%. Sizing the window from that
//     leaves a fifth of the screen showing the desktop behind it.
//
// The takeover case the old comment worried about is guarded rather than
// assumed: a mode too small to be a desktop is not believed, and the registry
// mode - the one the display will go back to - is used instead. As deployed it
// cannot arise anyway, because configure_game.py asserts dxwrapper's
// EnableWindowMode and GTA2 then never changes the mode at all.
void DesktopSize(int* width, int* height) {
    // Anything smaller than this is GTA2's own screen, not a desktop.
    const DWORD kMinDesktopWidth = 800;
    const DWORD kMinDesktopHeight = 600;

    // DWORD, not int: ENUM_CURRENT_SETTINGS and ENUM_REGISTRY_SETTINGS are
    // (DWORD)-1 and (DWORD)-2, which narrow.
    const DWORD modes[2] = {ENUM_CURRENT_SETTINGS, ENUM_REGISTRY_SETTINGS};
    for (DWORD which : modes) {
        DEVMODEA mode = {};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsA(nullptr, which, &mode) && mode.dmPelsWidth >= kMinDesktopWidth
            && mode.dmPelsHeight >= kMinDesktopHeight) {
            *width = static_cast<int>(mode.dmPelsWidth);
            *height = static_cast<int>(mode.dmPelsHeight);
            return;
        }
    }
    // Last, and the one that is wrong on a scaled desktop - but a window that is
    // too small still shows all of itself, which is the better failure.
    *width = GetSystemMetrics(SM_CXSCREEN);
    *height = GetSystemMetrics(SM_CYSCREEN);
}

std::string GameDirectory() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *slash = '\0';
    return std::string(path) + "\\data";
}

// Reads the game's DMAP arrays straight out of its map object. The in-memory
// layout is identical to the file, so the same parser handles both.
bool ReadLiveMap(const uint8_t* mapObject, gta2::Map* map, std::string* error) {
    const uint8_t* base = mapObject;
    const uint8_t* columns = *reinterpret_cast<const uint8_t* const*>(mapObject + gta2::kMapObjectColumnsPtr);
    const uint8_t* blocks = *reinterpret_cast<const uint8_t* const*>(mapObject + gta2::kMapObjectBlocksPtr);
    if (!columns || !blocks) {
        *error = "map arrays are not populated";
        return false;
    }

    // Neither array stores its length, so the extents come from the indices the
    // base array actually references.
    uint32_t maxWord = 0;
    for (size_t i = 0; i < gta2::kDmapBaseBytes; i += 4) {
        uint32_t word;
        memcpy(&word, base + i, 4);
        if (word > maxWord && word < kMaxColumnWords) maxWord = word;
    }
    const size_t columnBytes = (static_cast<size_t>(maxWord) + 1 + gta2::kMapLevels) * 4;

    uint32_t maxBlock = 0;
    for (size_t i = 0; i < gta2::kDmapBaseBytes; i += 4) {
        uint32_t word;
        memcpy(&word, base + i, 4);
        const size_t header = static_cast<size_t>(word) * 4;
        if (header + 4 > columnBytes) continue;
        const int count = columns[header] - columns[header + 1];
        for (int b = 0; b < count; ++b) {
            uint32_t index;
            memcpy(&index, columns + header + 4 + static_cast<size_t>(b) * 4, 4);
            if (index > maxBlock && index < kMaxBlocks) maxBlock = index;
        }
    }

    return map->LoadFromDmap(base, columns, columnBytes, blocks,
                             (static_cast<size_t>(maxBlock) + 1) * 12, error);
}

// One chunk of a .gmp file, by its four-letter tag; empty if the file or the
// chunk is missing.
std::vector<uint8_t> FileChunk(const std::string& path, const char* tag) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(size > 0 ? static_cast<size_t>(size) : 0);
    const bool ok = !data.empty() && fread(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    if (!ok || data.size() < 6 || memcmp(data.data(), "GBMP", 4) != 0) return {};

    size_t offset = 6;
    while (offset + 8 <= data.size()) {
        uint32_t chunk;
        memcpy(&chunk, &data[offset + 4], 4);
        const size_t body = offset + 8;
        if (body + chunk > data.size()) return {};
        if (memcmp(&data[offset], tag, 4) == 0) {
            return std::vector<uint8_t>(data.begin() + body, data.begin() + body + chunk);
        }
        offset = body + chunk;
    }
    return {};
}

std::vector<uint8_t> FileBlockArray(const std::string& path) {
    const std::vector<uint8_t> dmap = FileChunk(path, "DMAP");
    const size_t pool = gta2::kDmapBaseBytes;
    if (pool + 4 > dmap.size()) return {};
    uint32_t words;
    memcpy(&words, &dmap[pool], 4);
    const size_t countAt = pool + 4 + static_cast<size_t>(words) * 4;
    if (countAt + 4 > dmap.size()) return {};
    uint32_t count;
    memcpy(&count, &dmap[countAt], 4);
    const size_t start = countAt + 4;
    const size_t bytes = (std::min)(static_cast<size_t>(count) * 12, dmap.size() - start);
    return std::vector<uint8_t>(dmap.begin() + start, dmap.begin() + start + bytes);
}

// The map's own lamps (LGHT: 16 bytes each - ARGB, then x, y, z and radius at
// 128 to a tile, then intensity, shape, on and off time), as x, y, z.
std::vector<uint16_t> FileLightPositions(const std::string& path) {
    const std::vector<uint8_t> lght = FileChunk(path, "LGHT");
    std::vector<uint16_t> xyz;
    xyz.reserve(lght.size() / 16 * 3);
    for (size_t i = 0; i + 16 <= lght.size(); i += 16) {
        uint16_t v[3];
        memcpy(v, &lght[i + 4], sizeof(v));
        xyz.insert(xyz.end(), v, v + 3);
    }
    return xyz;
}

// Which style each map is played with, from the game's own level lists: the
// single-player sequence (test1.seq, MainOrBonus / GMPFile / STYFile entries)
// and the multiplayer maps (*.mmp, the same keys under [MapFiles]). A district's
// map shares its style's basename, but its bonus levels do not - lorne2e.gmp is
// played with wil.sty, mike1e.gmp with ste.sty, mike2e.gmp with bil.sty - and
// neither do the multiplayer maps (MP1-comp.gmp is bil.sty). Keys are the
// lowercased map filename.
std::map<std::string, std::string> MapStyles(const std::string& dataDir) {
    std::map<std::string, std::string> styles;
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return s;
    };
    auto trim = [](std::string s) {
        const size_t first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string();
        return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
    };
    for (const char* pattern : {"\\*.seq", "\\*.mmp"}) {
        WIN32_FIND_DATAA find = {};
        HANDLE handle = FindFirstFileA((dataDir + pattern).c_str(), &find);
        if (handle == INVALID_HANDLE_VALUE) continue;
        do {
            FILE* file = fopen((dataDir + "\\" + find.cFileName).c_str(), "rb");
            if (!file) continue;
            std::string map;
            char line[512];
            while (fgets(line, sizeof(line), file)) {
                std::string text = line;
                const size_t equals = text.find('=');
                if (equals == std::string::npos) continue;
                const std::string key = lower(trim(text.substr(0, equals)));
                const std::string value = trim(text.substr(equals + 1));
                // GMPFile comes first in every entry; the STYFile after it is
                // that map's style.
                if (key == "gmpfile") {
                    map = lower(value);
                } else if (key == "styfile" && !map.empty() && !value.empty()) {
                    styles.emplace(map, value);
                    map.clear();
                }
            }
            fclose(file);
        } while (FindNextFileA(handle, &find));
        FindClose(handle);
    }
    return styles;
}

// The game does not keep the style filename anywhere obvious, so the map is
// identified by matching its live blocks against the .gmp files on disk, and
// its style looked up in the game's level lists (MapStyles). A map they do not
// mention is taken to share its style's basename, which every district does.
std::string DetectStylePath(const uint8_t* mapObject, const std::string& dataDir,
                            std::string* mapPath) {
    const uint8_t* blocks = *reinterpret_cast<const uint8_t* const*>(mapObject + gta2::kMapObjectBlocksPtr);
    if (!blocks) return {};

    WIN32_FIND_DATAA find = {};
    HANDLE handle = FindFirstFileA((dataDir + "\\*.gmp").c_str(), &find);
    if (handle == INVALID_HANDLE_VALUE) return {};

    std::string bestName;
    double bestScore = 0.0;
    do {
        const std::vector<uint8_t> fileBlocks = FileBlockArray(dataDir + "\\" + find.cFileName);
        const size_t common = (std::min)(kMatchBlocks * 12, fileBlocks.size());
        if (common < 12) continue;

        size_t same = 0;
        for (size_t i = 0; i + 12 <= common; i += 12) {
            if (memcmp(blocks + i, &fileBlocks[i], 12) == 0) ++same;
        }
        const double score = static_cast<double>(same) / (common / 12);
        if (score > bestScore) {
            bestScore = score;
            bestName = find.cFileName;
        }
    } while (FindNextFileA(handle, &find));
    FindClose(handle);

    if (bestScore < 0.9 || bestName.empty()) return {};
    *mapPath = dataDir + "\\" + bestName;
    std::string lowerName = bestName;
    for (char& c : lowerName) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    const std::map<std::string, std::string> styles = MapStyles(dataDir);
    const auto listed = styles.find(lowerName);
    if (listed != styles.end()) {
        Log("map %s is played with %s (from the game's level lists)", bestName.c_str(),
            listed->second.c_str());
        return dataDir + "\\" + listed->second;
    }
    std::string stem = bestName.substr(0, bestName.find_last_of('.'));
    const size_t dash = stem.find('-');
    if (dash != std::string::npos) stem = stem.substr(0, dash);
    return dataDir + "\\" + stem + ".sty";
}

// A window of our own to present into.
//
// GTA2 sets DirectDraw up on its own window during startup, and on Windows 10
// and 11 DirectDraw is itself implemented on Direct3D 9: it takes the window
// into an exclusive fullscreen device before the renderer is even loaded. A
// second swap chain on that same window is tolerated by Microsoft's D3D9 but
// not by RTX Remix, whose Vulkan presentation never completes - the game hangs
// on its first Present with the Remix runtime spinning. Presenting into a
// window we own leaves DirectDraw in sole possession of the game's.
//
// It has to be a *child* of the game's window rather than a popup over it. A
// popup loses the z-order the moment the game is activated, and DirectDraw's
// empty primary surface is then what you see: black while the game has focus,
// and the path traced picture only when you alt-tab away and the game's window
// drops back down. A child window is always painted above its parent's client
// area, so there is no ordering to lose, and it can never take the focus or the
// input away from the game either.
HWND g_presentGameWindow = nullptr;

// The present window covers the game's, so the mouse is over ours, and a click
// there is ours: the game never hears of it, and a click on another top-level
// window takes the activation away from the game, which then pauses itself.
// The game does not use the mouse, but plugins for it do - a front end driven by
// the mouse reads it off the game's window. So a click is refused the
// activation, and the mouse messages go on to the game's window, at the place
// under the cursor in its own client area: exactly what it would have been sent
// with nothing of ours in the way. Not while the F4 menu has the mouse, which
// reads it for itself.
LRESULT CALLBACK PresentWndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    const bool mouse = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST
                       && message != WM_MOUSEWHEEL && message != WM_MOUSEHWHEEL;
    if (mouse && g_presentGameWindow && !DebugMenuWantsMouse()) {
        POINT p = {static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        ClientToScreen(window, &p);
        ScreenToClient(g_presentGameWindow, &p);
        PostMessageA(g_presentGameWindow, message, wparam,
                     MAKELPARAM(static_cast<short>(p.x), static_cast<short>(p.y)));
        return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

HWND CreatePresentWindow(HWND gameWindow, int width, int height, PresentWindow mode) {
    static bool registered = false;
    HINSTANCE instance = GetModuleHandleA(nullptr);
    if (!registered) {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = PresentWndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        wc.lpszClassName = "gta2dx9_present";
        if (!RegisterClassA(&wc)) return nullptr;
        registered = true;
    }
    if (!gameWindow) return nullptr;
    g_presentGameWindow = gameWindow;

    // Without this the game's window is free to paint its own background over
    // the child.
    SetWindowLongA(gameWindow, GWL_STYLE, GetWindowLongA(gameWindow, GWL_STYLE) | WS_CLIPCHILDREN);

    if (mode == PresentWindow::Child) {
        // Child coordinates are relative to the parent's client area, which is
        // the rectangle gbh_Init measured.
        HWND window = CreateWindowExA(WS_EX_NOACTIVATE, "gta2dx9_present", "GTA2",
                                      WS_CHILD | WS_VISIBLE, 0, 0, width, height, gameWindow,
                                      nullptr, instance, nullptr);
        if (window) {
            SetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        return window;
    }

    // Topmost: a separate top-level window in the topmost band, which sits above
    // the game's window even while the game is the activated one. A child cannot
    // help against a video device that presents past the window manager
    // altogether; this at least competes in the right band.
    // Positioned over the game's window but sized to the render resolution, not
    // to it: the game's window is whatever GTA2's options screen last wrote, and
    // 640x480 of path traced image is not the goal. Centred on the desktop when
    // the render size is larger than the game's window, so a 4K image over a
    // 640x480 game window still lands somewhere sensible.
    RECT gameRect = {0, 0, width, height};
    GetWindowRect(gameWindow, &gameRect);
    int left = gameRect.left;
    int top = gameRect.top;
    // The same measure the render size came from, so the two are comparable. On
    // a scaled desktop GetSystemMetrics answers a different question - see
    // DesktopSize - and mixing the two here put the window half off the screen.
    int screenW = 0, screenH = 0;
    DesktopSize(&screenW, &screenH);
    if (width >= screenW && height >= screenH) {
        left = 0;
        top = 0;
    } else {
        if (left + width > screenW) left = (screenW - width) / 2;
        if (top + height > screenH) top = (screenH - height) / 2;
        if (left < 0) left = 0;
        if (top < 0) top = 0;
    }
    // Created per-monitor DPI-aware whatever the process is, so a size in
    // physical pixels stays one and the window manager never scales the swap
    // chain. See MakeProcessDpiAware in dllmain.cpp for why that matters.
    typedef HANDLE(WINAPI * SetThreadContextFn)(HANDLE);
    static const SetThreadContextFn setThreadContext = reinterpret_cast<SetThreadContextFn>(
        GetProcAddress(GetModuleHandleA("user32.dll"), "SetThreadDpiAwarenessContext"));
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2.
    const HANDLE previous = setThreadContext
        ? setThreadContext(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)))
        : nullptr;
    HWND window = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                  "gta2dx9_present", "GTA2", WS_POPUP, left, top, width, height,
                                  nullptr, nullptr, instance, nullptr);
    if (previous) setThreadContext(previous);
    if (!window) return nullptr;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return window;
}

}  // namespace

bool g_blood = kDefaultBlood;
bool g_bloodForced = false;     // whether we are the ones holding the flag up
uint8_t g_bloodGameValue = 0;   // what the game had before we did

void SetBlood(bool on) { g_blood = on; }
bool Blood() { return g_blood; }

// Written every frame rather than once: a demo replay restores the flag from the
// recording, and the debug cheat toggles it, and either would otherwise turn
// blood off behind the setting's back. Turning the setting off hands back the
// value the game had, so a do_blood the player set in the registry survives.
void WorldView::ApplyBlood() {
    uint8_t* flag = reinterpret_cast<uint8_t*>(game::kDoBloodFlag);
    if (g_blood) {
        if (!g_bloodForced) {
            g_bloodGameValue = *flag;
            g_bloodForced = true;
        }
        *flag = 1;
    } else if (g_bloodForced) {
        *flag = g_bloodGameValue;
        g_bloodForced = false;
    }
}

void SetSpawnOffscreen(bool on) { g_spawnOffscreen = on; }
bool SpawnOffscreen() { return g_spawnOffscreen; }
bool SpawnOffscreenActive() { return g_spawnOffscreenActive; }

// Make GTA2's own view rectangle the shape of the frame we draw.
//
// Cars and pedestrians appeared and vanished inside a 16:9 frame's left and
// right margins because everything in gta2.exe that asks "is this on screen" -
// where traffic may be created, the ring pedestrians are placed on, which grid
// cells are drawn, what the recycler may destroy - measures against one
// rectangle, and FUN_0041E7A0 builds that rectangle 4:3. Earlier attempts moved
// the consumers one at a time (a detoured car test, a widened pedestrian ring, a
// padded grid walk, a padded rectangle), and each one that moved alone fell out
// of step with the rest: a spawn point pushed past what the recycler counts as
// seen is destroyed before it arrives, which is how the streets emptied.
//
// This moves the source instead, so every consumer stays consistent with every
// other exactly as it is in the stock game - only wider. Height is untouched:
// the renderer frames the game's own vertical extent, and the half-width scale
// and the aspect are changed by reciprocal amounts so their product, the
// half-height, stays what it was.
void WorldView::MatchGameViewToFrame() {
    // Follow the switch, in both directions and at any time. Applying reads and
    // keeps the original bytes, so turning it off mid-game is a real revert.
    if (g_spawnOffscreen != g_spawnOffscreenActive) {
        if (g_spawnOffscreen) {
            // A site that does not match is a different gta2.exe: give up for
            // good rather than ask again every frame.
            if (!ApplyGamePatches()) g_spawnOffscreen = false;
        } else {
            RevertGamePatches();
        }
        g_spawnOffscreenActive = g_spawnOffscreen;
    }

    // Never narrower than the game's own: a 4:3 or taller frame is already
    // covered by the stock rectangle.
    float widen = 1.0f;
    if (renderer_.Width() > 0 && renderer_.Height() > 0) {
        const float aspect =
            static_cast<float>(renderer_.Width()) / static_cast<float>(renderer_.Height());
        widen = (std::max)(1.0f, aspect / game::kViewAspectRatioStock);
    }
    // GTA2Ceiling view range: the whole rectangle grows (width through the
    // half-width scale, height with it since the aspect stays), so traffic and
    // pedestrians appear, live and are drawn further from the player - the
    // ceiling shows far more of the map than the game's own screen ever did.
    const float range = (std::clamp)(ceiling_.ViewRange(), 1.0f, 3.0f);
    g_viewHalfScale = static_cast<int32_t>(game::kViewHalfScaleStock * widen * range + 0.5f);
    g_viewAspect = static_cast<int32_t>(game::kViewAspectStock / widen + 0.5f);
}

void WorldView::Configure(float pitchDegrees, float fovDegrees, bool useGameTiles,
                          PresentWindow present) {
    camera_.SetOrientation(0.0f, pitchDegrees * kDegreesToRadians);
    camera_.SetFovY(fovDegrees * kDegreesToRadians);
    useGameTiles_ = useGameTiles;
    present_ = present;
    Log("camera pitch %.1f deg, fov %.1f deg; tile artwork from %s", pitchDegrees, fovDegrees,
        useGameTiles ? "the game" : "the style file");
}

void WorldView::SetRenderSize(int width, int height) {
    requestedWidth_ = width;
    requestedHeight_ = height;
}

bool WorldView::Initialize(HWND window, int width, int height, std::string* error) {
    staleXrFramePresented_=false;lastXrSample_=0;
    dataDir_ = GameDirectory();
    const std::string inputConfig=dataDir_+"\\..\\gta2ceiling.ini";
    gamepad_.Initialize(inputConfig.c_str());
    gamepad_.SetFilter([](const gta2ceiling::GamepadState& state,void* context) {
        return static_cast<WorldView*>(context)->ceiling_.ConsumeController(state);
    },this);
    gamepad_.SetRemap([](gta2ceiling::GamepadState& state,void* context) {
        static_cast<WorldView*>(context)->ModernRemap(state);
    },this);
    if (!gamepad_.LastError().empty()) Log("controller: %s",gamepad_.LastError().c_str());
    // Both live here, so the pointer is good for the process. The sampler stays
    // empty until a map is loaded, and the sprite pass falls back until it is.
    live_.SetGround(&ground_);
    LightsSetGround(&ground_);
    // The first frames the game draws are the front end, which is the same 2D
    // stream the menus use later. Traced once so a broken menu can be compared
    // against a working one.
    overlay_.TraceNextFrames(3);
    gameWindow_ = window;
    window_ = window;
    // Winlator launches the shortcut on "shell", while Wine can retain
    // "Default" as its input desktop. SendInput then returns ACCESS_DENIED,
    // even with GTA2 in the foreground. Select this game's own Wine desktop;
    // never change the interactive Windows desktop on a native PC.
    if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"wine_get_version")) {
        const BOOL switched=SwitchDesktop(GetThreadDesktop(GetCurrentThreadId()));
        Log("Wine game input desktop selected=%d error=%lu",switched,switched?0:GetLastError());
    }

    // What the game thinks its screen is only sets out the HUD; the overlay pass
    // scales that to whatever it is drawn into. So the render size is ours to
    // choose, and defaults to the desktop rather than to GTA2's 640x480.
    int desktopW = 0, desktopH = 0;
    DesktopSize(&desktopW, &desktopH);
    width_ = requestedWidth_ > 0 ? requestedWidth_ : desktopW;
    height_ = requestedHeight_ > 0 ? requestedHeight_ : desktopH;
    if (width_ < 320 || height_ < 240) {
        width_ = width;
        height_ = height;
    }
    if (present_==PresentWindow::GameWindow) {
        // The raster-only video proxy already owns presentation: no second
        // Remix window is needed. WinlatorXR must see one drawable at (0,0),
        // with its sync pixel and keyboard focus belonging to the same window.
        SetWindowLongA(window_,GWL_STYLE,WS_POPUP|WS_VISIBLE);
        SetWindowLongA(window_,GWL_EXSTYLE,WS_EX_APPWINDOW);
        SetWindowPos(window_,HWND_TOP,0,0,width_,height_,SWP_FRAMECHANGED|SWP_SHOWWINDOW);
        SetForegroundWindow(window_);
        RECT rectangle{}; POINT origin{};
        GetWindowRect(window_,&rectangle); ClientToScreen(window_,&origin);
        Log("single render window=%p rect=(%ld,%ld,%ld,%ld) clientOrigin=(%ld,%ld) foreground=%p",
            window_,rectangle.left,rectangle.top,rectangle.right,rectangle.bottom,origin.x,origin.y,GetForegroundWindow());
    }
    // Everything the sizing depends on, in one line: the three answers Windows
    // gives for "how big is the screen" disagree whenever DPI scaling is on, and
    // which one arrived is the first thing to know when the picture is the wrong
    // size.
    {
        typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
        static GetDpiForWindowFn getDpi = reinterpret_cast<GetDpiForWindowFn>(
            GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow"));
        DEVMODEA mode = {};
        mode.dmSize = sizeof(mode);
        EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &mode);
        // The client rect of our own window is what ImGui lays the F4 menu out
        // in and what the mouse is measured against, so a disagreement with the
        // back buffer is a misplaced cursor rather than a wrong-sized picture.
        Log("render size %dx%d (monitor %dx%d, system metrics %dx%d, display mode %ux%u, "
            "game window %u dpi, game screen %dx%d, ini asked for %dx%d)",
            width_, height_, desktopW, desktopH, GetSystemMetrics(SM_CXSCREEN),
            GetSystemMetrics(SM_CYSCREEN), mode.dmPelsWidth, mode.dmPelsHeight,
            getDpi && window ? getDpi(window) : 0, width, height, requestedWidth_,
            requestedHeight_);
    }
    if (desktopW <= 800 && requestedWidth_ <= 0) {
        Log("WARNING: even the registry display mode reads %dx%d. Set render_width and "
            "render_height in gta2ceiling.ini explicitly.", desktopW, desktopH);
    }
    width = width_;
    height = height_;
    if (present_ != PresentWindow::GameWindow) {
        window_ = CreatePresentWindow(window, width, height, present_);
        RECT client = {0, 0, 0, 0};
        if (window_) GetClientRect(window_, &client);
        Log("presenting into our own %s window %p (client %ldx%ld) over the game's %p",
            present_ == PresentWindow::Child ? "child" : "topmost", window_,
            client.right - client.left, client.bottom - client.top, window);
        if (!window_) window_ = window;
    }
    gta2::SetRendererTrace([](const char* line) { Log("d3d: %s", line); });
    if (EnsureDevice()) return true;
    *error = "device not up yet, retrying from the frame loop";
    return false;
}

// The device does not necessarily come up on the first ask. Under RTX Remix the
// runtime backing D3D9 is a separate 64-bit process that is still starting when
// the game asks its renderer to initialise, and it only finishes once the game
// has run its message loop for a moment - which first happens between frames.
// Retrying from the frame loop is what gives it that moment; failing outright
// here is what used to leave the game sitting at a black menu.
bool WorldView::EnsureDevice() {
    if (ready_) {
        const HRESULT state=renderer_.DeviceStatus();
        if (SUCCEEDED(state)) return true;
        if (state==D3DERR_DEVICENOTRESET) {
            // Managed geometry/artwork survives Reset. The default-pool UI and
            // ImGui objects must be released first, on the owning game thread.
            DebugMenuShutdown();
            if (SUCCEEDED(renderer_.ResetDevice())) {
                staleXrFramePresented_=false;lastXrSample_=0;
                InvalidateRect(gameWindow_,nullptr,TRUE);
                Log("D3D device restored; UI requested to repaint");
                return true;
            }
        }
        return false;
    }
    if (!window_ || deviceAttempts_ >= kMaxDeviceAttempts) return false;

    ++deviceAttempts_;
    std::string error;
    if (!renderer_.Initialize(window_, width_, height_, &error)) {
        // One line per attempt would be hundreds of them, so only the first and
        // the last are worth keeping.
        if (deviceAttempts_ == 1 || deviceAttempts_ == kMaxDeviceAttempts) {
            Log("device not up on attempt %d: %s", deviceAttempts_, error.c_str());
        }
        return false;
    }
    ready_ = true;
    staleXrFramePresented_=false;lastXrSample_=0;
    Log("world view up on hwnd %p at %dx%d after %d attempt(s)", window_, width_, height_,
        deviceAttempts_);
    return true;
}

void WorldView::Shutdown() {
    staleXrFramePresented_=false;lastXrSample_=0;
    if (!gamepad_.Shutdown()) Log("controller: pending key releases at shutdown");
    ceiling_.Shutdown();
    if (!ready_ && !loadedMapObject_) return;
    ready_ = false;
    loadedMapObject_ = nullptr;
    const HWND ours = window_ != gameWindow_ ? window_ : nullptr;
    window_ = nullptr;  // stops the frame loop bringing the device back up
    // ImGui and the Remix lights both hold device-side or bridge-side objects, so
    // they go first: the menu's font texture belongs to the device we are about
    // to release, and a light handle outliving the bridge is a leak on the far
    // side of it.
    DebugMenuShutdown();
    LightsShutdown();
    RemixApiShutdown();
    // Device-owned textures have to go before the device does.
    overlay_.ReleaseResources();
    live_.ReleaseResources();
    // Before the frames go: the report is the only record of what this session
    // actually built and drew.
    WriteTextureReport();
    ReleaseDeviceTextures();
    renderer_.Shutdown();
    if (ours) DestroyWindow(ours);
    Log("world view shut down, device released");
}

void WorldView::SetVisibleTileBounds(float minX, float minY, float maxX, float maxY) {
    minX_ = minX;
    minY_ = minY;
    maxX_ = maxX;
    maxY_ = maxY;
    haveBounds_ = maxX > minX && maxY > minY;
}

bool WorldView::EnsureWorldLoaded() {
    const uint8_t* mapObject = game::MapObject();
    if (!mapObject) {
        // Back at the menu. The game has freed its map, so the level's geometry
        // has to go with it - otherwise the last frame of the city stays in the
        // scene, drawn behind the menu and still path traced by Remix, and the
        // menu never appears to come back.
        if (worldUploaded_) {
            Log("level ended: releasing the world mesh");
            renderer_.ReleaseWorld();
            worldUploaded_ = false;
            // The sprite pass must not conform to a map that is no longer there,
            // and the next level's camera should not ease down from this one's
            // rooftops.
            ground_.Clear();
            tileSources_.clear();
            heightSettled_ = false;
            // Back to the menu's own scale immediately, rather than keeping the
            // level's until the game next calls gbh_SetWindow.
            overlay_.RevertToWindowSize();
            // Back at the menu is exactly where the 2D stream wants looking at.
            overlay_.TraceNextFrames(3);
        }
        // The map memory goes with the level: never write to it again.
        DropWalkOut("level ended");
        ceiling_.ReleaseWorld();
        worldWasLocked_ = false;
        releaseStartMs_ = 0;
        loadedMapObject_ = nullptr;
        return false;
    }
    if (mapObject == loadedMapObject_) return true;

    Log("level load: begin map=%p thread=%lu", mapObject, GetCurrentThreadId());
    DropWalkOut("new level");
    follow_.clear();
    RecallFollow();
    followRounds_ = 0;
    ceiling_.ReleaseWorld();
    worldWasLocked_ = false;
    releaseStartMs_ = 0;
    std::string error;
    // The old ground describes the old map. Cleared here rather than after the
    // rebuild, so a level that fails to load leaves sprites on the fixed lift
    // instead of on a floor from the district before it.
    ground_.Clear();
    Log("level load: reading live map");
    if (!ReadLiveMap(mapObject, &map_, &error)) {
        Log("live map read failed: %s", error.c_str());
        loadedMapObject_ = mapObject;  // Do not retry every frame on a bad map.
        return false;
    }

    Log("level load: matching style");
    std::string mapPath;
    const std::string stylePath = DetectStylePath(mapObject, dataDir_, &mapPath);
    gta2::Style style;
    if (stylePath.empty()) {
        Log("could not identify the district for map object %p", mapObject);
        loadedMapObject_ = mapObject;
        return false;
    }
    Log("level load: reading style %s",stylePath.c_str());
    if (!style.Load(stylePath, &error)) {
        Log("style load failed (%s): %s", stylePath.c_str(), error.c_str());
        loadedMapObject_ = mapObject;
        return false;
    }

    // The district's name salts the per-light override keys, so a lamp standing
    // in the same spot in two districts does not share one entry.
    {
        const size_t slash = stylePath.find_last_of('\\');
        std::string district = slash == std::string::npos ? stylePath : stylePath.substr(slash + 1);
        const size_t dot = district.find_last_of('.');
        if (dot != std::string::npos) district = district.substr(0, dot);
        LightsSetScene(district.c_str());
    }

    // What lies on the road rather than standing on it, so the sprite pass can
    // lay it flat instead of lifting it like a car. By code_obj number, which
    // every shipped district shares:
    //   497-502  a blood pool, growing in six steps
    //   515      a pair of drops
    //   516-527  trail strips, 4 wide at three lengths (24, 36, 16), each
    //            fading from solid to sparse - skid marks in grey (palette 257
    //            in game) and blood trails in red, the same artwork either way
    //   528-531  the dots at the end of a trail
    ClearGroundDecalArtwork();
    const int kDecalRanges[][2] = {{497, 502}, {515, 531}};
    for (const auto& range : kDecalRanges) {
        for (int sprite = range[0]; sprite <= range[1]; ++sprite) {
            gta2::SpriteIndices art;
            if (style.SpriteArtwork(gta2::SpriteBase::CodeObj, sprite, &art)) {
                AddGroundDecalArtwork(art.width, art.height, art.indices.data(), art.width);
            }
        }
    }
    Log("ground decals: %d skid, trail and blood sprites registered", GroundDecalArtworkCount());

    // Where each car's lamps are, from the deltas the game lights for them. A
    // car sprite faces down the page - its headlight delta is on the bottom row -
    // so the heading runs along +y and the lamp's offset from the middle, at 64
    // pixels a tile, is where the light goes relative to the car.
    {
        const int cars = style.SpriteCount(gta2::SpriteBase::Car);
        for (const int delta : game::kCarLampDeltas) {
            std::vector<CarLamp> lamps(cars > 0 ? cars : 0);
            int found = 0;
            for (int sprite = 0; sprite < cars; ++sprite) {
                gta2::SpriteIndices art;
                float x = 0.0f, y = 0.0f;
                if (!style.SpriteArtwork(gta2::SpriteBase::Car, sprite, &art) ||
                    !style.SpriteDeltaCentre(gta2::SpriteBase::Car, sprite, delta, &x, &y)) {
                    continue;
                }
                lamps[sprite].valid = true;
                lamps[sprite].along = (y - art.height * 0.5f) / 64.0f;
                lamps[sprite].side = (x - art.width * 0.5f) / 64.0f;
                style.SpriteDeltaColour(gta2::SpriteBase::Car, sprite, delta, lamps[sprite].rgb);
                ++found;
            }
            SyntheticSetCarLamps(delta, std::move(lamps));

            // And the lamp's own pixels, for our own lamp quads.
            std::vector<LampArt> arts(cars > 0 ? cars : 0);
            for (int sprite = 0; sprite < cars; ++sprite) {
                gta2::SpriteIndices art;
                LampArt& a = arts[sprite];
                if (style.SpriteArtwork(gta2::SpriteBase::Car, sprite, &art) &&
                    style.SpriteDeltaImage(gta2::SpriteBase::Car, sprite, delta, &a.x0, &a.y0,
                                           &a.width, &a.height, &a.argb)) {
                    a.spriteWidth = art.width;
                    a.spriteHeight = art.height;
                }
            }
            SetCarLampArt(delta, std::move(arts));
            Log("car lamps: delta %d placed from the artwork for %d of %d car sprites", delta,
                found, cars);
        }
    }

    // Prefer the game's own artwork over our parse of the style file: the game
    // resolves tile numbers through a remap table, so indexing the file directly
    // yields the right set of textures in the wrong order.
    int overridden = 0;
    tileSources_.assign(style.TileCount(), TileSource{});
    if (useGameTiles_) {
        std::vector<uint32_t> pixels(gta2::kTilePixels);
        for (int tile = 1; tile < style.TileCount(); ++tile) {
            if (ResolveTileImage(tile, pixels.data())) {
                style.OverrideTile(tile, pixels.data());
                const void* record = game::TextureForTile(tile);
                tileSources_[tile].record = record;
                tileSources_[tile].revision =
                    record ? static_cast<const TextureRecord*>(record)->revision : 0;
                ++overridden;
            }
        }
    }
    Log("tile artwork from the game: %d/%d (palettes captured: %d)", overridden,
        style.TileCount(), PaletteCount());

    // Ramp shapes come from the game's own slope table rather than a derived
    // formula: it is the only statement of which way each type climbs and over
    // how many blocks, and it exists only in the running process.
    std::array<gta2::SlopeInfo, gta2::kSlopeTypeCount> slopes = {};
    {
        uint8_t directions[gta2::kSlopeTypeCount] = {};
        uint8_t steps[gta2::kSlopeTypeCount] = {};
        uint8_t stepIndices[gta2::kSlopeTypeCount] = {};
        game::ReadSlopeTable(directions, steps, stepIndices, gta2::kSlopeTypeCount);
        int ramps = 0;
        for (int type = 0; type < gta2::kSlopeTypeCount; ++type) {
            slopes[type].direction = directions[type];
            slopes[type].steps = steps[type];
            slopes[type].step = stepIndices[type];
            if (directions[type] >= 1 && directions[type] <= 4 && steps[type]) ++ramps;
        }
        Log("slope table: %d ramp types read from the game", ramps);
    }

    // The sprite pass stands sprites on the lid this same table shapes. Resolved
    // once, here, and handed to both consumers: two readings of a ramp would be
    // two different floors, and a sprite conformed to the wrong one is exactly
    // the bug conforming exists to remove.
    std::array<gta2::SlopeInfo, gta2::kSlopeTypeCount> resolvedSlopes = {};
    gta2::ResolveSlopeTable(slopes.data(), resolvedSlopes.data());
    ground_.Reset(&map_, resolvedSlopes.data());
    // Which of the lights the game lists are the map's own lamps, the only ones
    // lifted onto these floors.
    {
        const std::vector<uint16_t> mapLights = FileLightPositions(mapPath);
        LightsSetMapLights(mapLights);
        Log("map lamps: %u in %s", static_cast<unsigned>(mapLights.size() / 3), mapPath.c_str());
    }

    // Where a partial or corner block is cut, likewise straight from the game
    // rather than assumed. The two outer constants are a sanity check: they must
    // read as an empty and a whole cell, or the addresses are wrong.
    gta2::PartialCuts cuts;
    {
        const float zero = game::CellFraction(game::kCellZeroPtr);
        const float one = game::CellFraction(game::kCellOnePtr);
        const float low = game::CellFraction(game::kCellLowPtr);
        const float high = game::CellFraction(game::kCellHighPtr);
        const bool sane = std::abs(zero) < 0.01f && std::abs(one - 1.0f) < 0.01f && low > 0.0f &&
                          low < high && high < 1.0f;
        if (sane) {
            cuts.low = low;
            cuts.high = high;
        }
        Log("partial block cuts: %.4f / %.4f (bounds %.4f..%.4f) %s", low, high, zero, one,
            sane ? "accepted" : "REJECTED, using defaults");
    }

    gta2::WorldMesh mesh;
    gta2::BuildWorldMesh(map_, style, slopes.data(), cuts, &mesh);
    const size_t oldBatches=mesh.batches.size();
    if (!gta2ceiling::PartitionRasterWorld(mesh)) {
        Log("level load: invalid mesh, raster partition refused"); return false;
    }
    Log("raster spatial batches: %zu -> %zu (geometry/materials unchanged)",oldBatches,mesh.batches.size());
    if (!renderer_.UploadWorld(mesh, style, &error)) {
        Log("world upload failed: %s", error.c_str());
        loadedMapObject_ = mapObject;
        return false;
    }

    if (game::ViewRotation() != 0xFF) {
        Log("WARNING: view rotation is %u, not the 0xFF the tile orientation assumes",
            game::ViewRotation());
    }
    Log("world loaded: %s, %zu blocks, %zu triangles, %zu batches, floor %s", stylePath.c_str(),
        map_.BlockCount(), mesh.indices.size() / 3, mesh.batches.size(),
        gta2::WorldSeal() ? "sealed" : "OPEN (the sky shows through any gap)");
    loadedMapObject_ = mapObject;
    worldUploaded_ = true;
    return true;
}

// GTA2 animates ceiling fans, screens and water by changing the artwork behind
// a tile number, not by moving anything: either the style's remap sends the tile
// at a different texture record, or the record's own pixels are rewritten
// between a lock and an unlock. Both show up here, so the static world mesh
// keeps its geometry and only the affected textures are re-uploaded.
void WorldView::RefreshAnimatedTiles() {
    if (!useGameTiles_ || tileSources_.empty()) return;

    std::vector<uint32_t> pixels;
    int updated = 0;
    for (int tile = 1; tile < static_cast<int>(tileSources_.size()); ++tile) {
        if (!renderer_.HasTileTexture(tile)) continue;  // not used by any face
        const void* record = game::TextureForTile(tile);
        if (!record) continue;
        const uint16_t revision = static_cast<const TextureRecord*>(record)->revision;
        TileSource& source = tileSources_[tile];
        if (record == source.record && revision == source.revision) continue;

        if (pixels.empty()) pixels.resize(gta2::kTilePixels);
        if (!ResolveTileImage(tile, pixels.data())) continue;
        renderer_.UpdateTileTexture(tile, pixels.data());
        source.record = record;
        source.revision = revision;
        // A handful of tiles animate; a cap keeps a pathological frame from
        // stalling on hundreds of texture uploads.
        if (++updated >= 64) break;
    }
}

// Street level under a point. Deliberately the *bottom* of the column rather
// than the top: the topmost block is a building roof, so following it makes the
// camera leap whenever it crosses a building.
float WorldView::GroundHeightAt(float tileX, float tileY) const {
    const int x = (std::clamp)(static_cast<int>(tileX), 0, gta2::kMapWidth - 1);
    const int y = (std::clamp)(static_cast<int>(tileY), 0, gta2::kMapHeight - 1);
    const gta2::Column& column = map_.ColumnAt(x, y);
    return column.blocks.empty() ? 0.0f : static_cast<float>(column.offset + 1);
}

void WorldView::UpdateShadowLevels() {
    // The ground levels that cover a real share of the visible city. Each one
    // receives the sun shadows of what stands above it (see DrawWorldShadows).
    if (map_.Empty()) { shadowLevelCount_ = 0; return; }
    const int cx = static_cast<int>(cityCenter_.x);
    const int cy = static_cast<int>(static_cast<float>(gta2::kMapHeight) - cityCenter_.z);
    const int radius = (std::clamp)(static_cast<int>(ceiling_.HalfWidthTiles()) + 2, 8, 48);
    if (cx == shadowLevelsX_ && cy == shadowLevelsY_ && radius == shadowLevelsRadius_) return;
    shadowLevelsX_ = cx; shadowLevelsY_ = cy; shadowLevelsRadius_ = radius;
    int histogram[16]{}, total = 0;
    for (int y = cy - radius; y <= cy + radius; y += 2)
        for (int x = cx - radius; x <= cx + radius; x += 2) {
            if (x < 0 || y < 0 || x >= gta2::kMapWidth || y >= gta2::kMapHeight) continue;
            const gta2::Column& column = map_.ColumnAt(x, y);
            if (column.blocks.empty()) continue;
            ++histogram[(std::min)(static_cast<int>(column.offset) + 1, 15)];
            ++total;
        }
    const int previousCount = shadowLevelCount_;
    float previous[kMaxShadowLevels];
    std::memcpy(previous, shadowLevels_, sizeof(previous));
    shadowLevelCount_ = 0;
    for (int pick = 0; pick < kMaxShadowLevels && total; ++pick) {
        int best = -1;
        for (int level = 0; level < 16; ++level)
            if (histogram[level] && (best < 0 || histogram[level] > histogram[best])) best = level;
        if (best < 0 || histogram[best] * 100 < total * 6) break;  // at least 6 % of the view
        shadowLevels_[shadowLevelCount_++] = static_cast<float>(best);
        histogram[best] = 0;
    }
    std::sort(shadowLevels_, shadowLevels_ + shadowLevelCount_);  // low to high (see DrawWorldShadows)
    if (previousCount == shadowLevelCount_ && !std::memcmp(previous, shadowLevels_, sizeof(previous))) return;
    Log("shadow level at (%d,%d) r%d: %d [%.0f]", cx, cy, radius, shadowLevelCount_,
        shadowLevelCount_ > 0 ? shadowLevels_[0] : -1.0f);
}

float WorldView::StreetLevel(float tileX, float tileY) {
    // The lowest ground that really is ground around the camera, over the whole
    // play field: everything below the level chosen here lies behind the real
    // floor or ceiling and is not drawn. Taking the most common height made a
    // raised plaza the reference while the player stood on it, and the street
    // below it then vanished into the room's surface (a dark patch where the
    // street should be) until the player walked down the stairs. The lowest
    // level with a real share of the cells (not a single pit or tunnel mouth)
    // keeps every street of the field above the surface; raised parts simply
    // stand on it, as buildings do.
    if (map_.Empty()) return -1.0f;
    int histogram[16]{};
    const int cx = static_cast<int>(tileX), cy = static_cast<int>(tileY);
    const int radius = (std::clamp)(static_cast<int>(std::ceil(ceiling_.FieldRadiusTiles())), 4, 16);
    int total = 0;
    for (int y = cy - radius; y <= cy + radius; ++y)
        for (int x = cx - radius; x <= cx + radius; ++x) {
            if (x < 0 || y < 0 || x >= gta2::kMapWidth || y >= gta2::kMapHeight) continue;
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > radius * radius) continue;
            const gta2::Column& column = map_.ColumnAt(x, y);
            if (column.blocks.empty()) continue;
            ++histogram[(std::min)(static_cast<int>(column.offset) + 1, 15)];
            ++total;
        }
    if (!total) return -1.0f;
    const int share = (std::max)(3, total / 10);
    for (int level = 1; level < 16; ++level) if (histogram[level] >= share) return static_cast<float>(level);
    int best = 0;
    for (int level = 1; level < 16; ++level) if (histogram[level] > histogram[best]) best = level;
    return static_cast<float>(best);
}

void WorldView::UpdateCamera() {
    float tileX = 0.0f, tileY = 0.0f;
    if (!game::CameraPosition(&tileX, &tileY)) return;

    // Ground height is a per-tile step function, so following it directly makes
    // the camera bob every time it crosses a tile edge. Ease toward it instead.
    const float street = StreetLevel(tileX, tileY);
    if (street != loggedStreet_) {
        Log("street level %.0f at tile (%.1f,%.1f)", street, tileX, tileY);
        loggedStreet_ = street;
    }
    const float desired = street >= 0.0f ? street : GroundHeightAt(tileX, tileY);
    if (!heightSettled_) {
        smoothedHeight_ = desired;
        heightSettled_ = true;
    } else {
        smoothedHeight_ += (desired - smoothedHeight_) * kHeightFollowRate;
    }

    // Map rows run north to south, so the row is mirrored exactly the way the
    // mesh builder mirrors it.
    const gta2::Vec3 target{tileX, smoothedHeight_,
                            static_cast<float>(gta2::kMapHeight) - tileY};
    if (ceiling_.WorldLocked()) {
        // Frozen: the city stays where it is, the game goes on and its player
        // walks off the disc into the room (see walk_out.h).
        if (!worldWasLocked_) {
            worldWasLocked_ = true;
            releaseStartMs_ = 0;
            Log("city frozen at (%.1f,%.1f) level %.2f, disc radius %.1f tiles", cityCenter_.x, cityCenter_.z,
                cityCenter_.y, ceiling_.FieldRadiusTiles());
        }
    } else {
        if (worldWasLocked_) {
            // Released: nothing jumps. The city eases from where it stood back
            // onto the player, starting at rest and arriving at the player's
            // speed. A respawn far away is simply followed.
            worldWasLocked_ = false;
            const float gap = std::hypot(target.x - cityCenter_.x, target.z - cityCenter_.z);
            releaseFrom_ = cityCenter_;
            releaseStartMs_ = gap < 40.0f ? (std::max)(GetTickCount64(), uint64_t{1}) : 0;
            Log("city follows the player again: %.1f tiles away, %s", gap, releaseStartMs_ ? "easing back" : "jumping");
        }
        if (releaseStartMs_) {
            const float t = static_cast<float>(GetTickCount64() - releaseStartMs_) / (kReleaseSeconds * 1000.0f);
            if (t >= 1.0f) {
                releaseStartMs_ = 0;
                cityCenter_ = target;
            } else {
                const float s = t * t * (3.0f - 2.0f * t);
                cityCenter_ = {releaseFrom_.x + (target.x - releaseFrom_.x) * s,
                               releaseFrom_.y + (target.y - releaseFrom_.y) * s,
                               releaseFrom_.z + (target.z - releaseFrom_.z) * s};
            }
        } else {
            cityCenter_ = target;
        }
    }
    UpdateWalkOut();
    // Who the player is: learnt all through play, so it is known by the time
    // the city is frozen.
    if (worldUploaded_) ProbeFollow(tileX, tileY);
    // Which sprite the player is, asked before this frame's sprite pass.
    if (WalkOutFree() && worldUploaded_) {
        float px = 0, py = 0;
        bool inCar = false;
        const bool exact = FollowedPlayer(&px, &py, &inCar);
        if (exact != walkExact_) {
            walkExact_ = exact;
            Log("walk out: the player is %s", exact ? "known exactly (the game's own object)" : "followed by its sprite");
        }
        if (exact)
            live_.RequestFree(true, target.x, target.z, true, px, static_cast<float>(gta2::kMapHeight) - py, -1.0f, 0.0f);
        else
            live_.RequestFree(true, target.x, target.z, walkTrack_.have, walkTrack_.x + walkTrack_.vx,
                              walkTrack_.z + walkTrack_.vz, walkTrack_.area, std::hypot(walkTrack_.vx, walkTrack_.vz));
    } else {
        live_.RequestFree(false, 0.0f, 0.0f, false, 0.0f, 0.0f, 0.0f, 0.0f);
        walkTrack_ = WalkTrack{};
    }

    // Zoom comes from the camera struct's continuous visible extent. The tile
    // rectangle handed to gbh_SetCamera is the same window rounded out to whole
    // tiles, so driving zoom from it makes the view step as the camera crosses
    // tile boundaries even when the game is not zooming at all.
    game::ViewExtent extent;
    if (game::VisibleExtent(&extent)) {
        visibleTiles_ = extent.Height();
    }
    if (visibleTiles_ <= 0.01f) return;

    const float distance = (visibleTiles_ * 0.5f) / std::tan(camera_.FovY() * 0.5f);
    camera_.FrameTarget(target, distance);
}

// The game's zoom has to come from the camera struct, not from the visible tile
// rectangle: that rectangle is a whole number of tiles, so using it as the zoom
// source can only ever change in steps. This dumps the struct so the continuous
// field driving zoom can be identified from real gameplay rather than guessed.
void WorldView::DumpCameraStruct() const {
    const uint8_t* camera = game::CameraStruct();
    if (!camera) return;

    char line[512];
    int used = snprintf(line, sizeof(line), "cam[%d] bounds=%.1f,%.1f..%.1f,%.1f |", frameCount_,
                        minX_, minY_, maxX_, maxY_);
    for (uintptr_t offset = 0x60; offset <= 0xC0 && used > 0 && used < (int)sizeof(line) - 32;
         offset += 4) {
        const int32_t raw = *reinterpret_cast<const int32_t*>(camera + offset);
        const float asFloat = *reinterpret_cast<const float*>(camera + offset);
        // Report both readings; the struct mixes 16.14 fixed point with floats.
        used += snprintf(line + used, sizeof(line) - used, " %02X:%.3f/%.3f", (unsigned)offset,
                         raw * game::kFixedScale, asFloat);
    }
    Log("%s", line);
}

void WorldView::CheckDrawnTile(const void* textureRecord) {
    if (map_.Empty() || tileChecks_ >= 24) return;

    float cellX = 0.0f, cellY = 0.0f;
    int level = 0;
    if (!game::CurrentCell(&cellX, &cellY, &level)) return;

    const int x = static_cast<int>(cellX + 0.5f);
    const int y = static_cast<int>(cellY + 0.5f);
    if (x < 0 || x >= gta2::kMapWidth || y < 0 || y >= gta2::kMapHeight) return;

    const int drawn = game::TileForTexture(textureRecord);
    if (drawn <= 0) return;

    // What our own walk of the map has at that cell and level.
    const gta2::Column& column = map_.ColumnAt(x, y);
    const int slot = level - column.offset;
    int ours[5] = {-1, -1, -1, -1, -1};
    if (slot >= 0 && slot < static_cast<int>(column.blocks.size())) {
        const gta2::Block& block = map_.BlockAt(column.blocks[slot]);
        ours[0] = block.left.Tile();
        ours[1] = block.right.Tile();
        ours[2] = block.top.Tile();
        ours[3] = block.bottom.Tile();
        ours[4] = block.lid.Tile();
    }

    const bool match = drawn == ours[0] || drawn == ours[1] || drawn == ours[2] ||
                       drawn == ours[3] || drawn == ours[4];
    Log("tilecheck cell=(%d,%d) lvl=%d game=%d ours[l/r/t/b/lid]=%d/%d/%d/%d/%d %s", x, y, level,
        drawn, ours[0], ours[1], ours[2], ours[3], ours[4], match ? "MATCH" : "MISMATCH");
    ++tileChecks_;
}

// The game writes each projected vertex's absolute world position four slots
// further along the vertex array, so the shape it actually draws for a block can
// be read straight off. Positions are in the game's own frame: x east, y south,
// z the level.
void WorldView::CheckDrawnShape(int corners) {
    const int slopeType = game::CurrentBlockSlopeType();
    if (slopeType < 45 || slopeType > 61) return;
    if (shapeChecks_ >= 40) return;

    float cellX = 0.0f, cellY = 0.0f;
    int level = 0;
    if (!game::CurrentCell(&cellX, &cellY, &level)) return;

    char line[320];
    int used = snprintf(line, sizeof(line), "shape type=%d cell=(%.0f,%.0f) lvl=%d %s", slopeType,
                        cellX, cellY, level, corners == 3 ? "tri" : "quad");
    for (int i = 0; i < corners && used > 0 && used < static_cast<int>(sizeof(line)) - 40; ++i) {
        const float* world = reinterpret_cast<const float*>(
            game::kTileVertexArray +
            static_cast<uintptr_t>(i + game::kWorldShadowSlots) * game::kVertexStride);
        used += snprintf(line + used, sizeof(line) - used, "  (%.3f %.3f %.3f)", world[0], world[1],
                         world[2]);
    }
    Log("%s", line);
    ++shapeChecks_;
}

void WorldView::NotePacing(bool rendered, bool captureUi, bool skippedStill, bool sameSample, bool tracked) {
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&frequency);
    const double nowMs = frequency.QuadPart ? 1000.0 * now.QuadPart / frequency.QuadPart : 0;
    auto& p = pacing_;
    if (!p.windowStart) p.windowStart = GetTickCount64();
    if (!tracked) ++p.untracked;
    else if (skippedStill) ++p.skippedStill;
    else if (sameSample) ++p.skippedSameSample;
    if (rendered) {
        (captureUi ? p.gameFrames : p.poseRefreshes)++;
        p.renderSum += xrRenderSeconds_ * 1000.0;
        p.renderMax = (std::max)(p.renderMax, xrRenderSeconds_ * 1000.0);
        const double age = ceiling_.PoseAgeMs();
        p.ageSum += age; p.ageMax = (std::max)(p.ageMax, age);
        if (p.lastPresentMs > 0) p.gapMax = (std::max)(p.gapMax, nowMs - p.lastPresentMs);
        p.lastPresentMs = nowMs;
        float pitch = 0;
        const float yaw = ceiling_.HeadYawPitch(pitch);
        if (p.haveAngles) {
            float dy = yaw - p.lastYaw;
            if (dy > 180) dy -= 360;
            if (dy < -180) dy += 360;
            p.turnDegrees += std::sqrt(dy * dy + (pitch - p.lastPitch) * (pitch - p.lastPitch));
        }
        p.lastYaw = yaw; p.lastPitch = pitch; p.haveAngles = true;
    }
    if (GetTickCount64() - p.windowStart >= 1000) {
        const int pairs = p.gameFrames + p.poseRefreshes;
        Log("pacing 1s: pairs %d (game %d, head %d), skipped still %d, same-sample %d, untracked %d | "
            "render avg %.1f max %.1f ms | pose age avg %.1f max %.1f ms | max gap %.1f ms | head turn %.0f deg/s",
            pairs, p.gameFrames, p.poseRefreshes, p.skippedStill, p.skippedSameSample, p.untracked,
            pairs ? p.renderSum / pairs : 0.0, p.renderMax, pairs ? p.ageSum / pairs : 0.0, p.ageMax, p.gapMax,
            p.turnDegrees);
        const double last = p.lastPresentMs;
        const bool have = p.haveAngles; const float ly = p.lastYaw, lp = p.lastPitch;
        p = Pacing{};
        p.windowStart = GetTickCount64(); p.lastPresentMs = last;
        p.haveAngles = have; p.lastYaw = ly; p.lastPitch = lp;
    }
}

bool WorldView::RenderStereo(bool captureUi) {
    LARGE_INTEGER started{}, finished{}, frequency{};
    QueryPerformanceCounter(&started);
    gta2::Camera eyes[2];
    const bool tracked = ceiling_.Prepare(cityCenter_, eyes);
    if (!ceiling_.Active()) return false;
    // Present one transparent/blank target when tracking expires. The idle
    // pacer calls us about every millisecond, so do not keep clearing/presenting
    // that same blank surface until tracking returns or the device is reset.
    if(!tracked && staleXrFramePresented_) return true;
    if(tracked) staleXrFramePresented_=false;
    renderer_.SetWorldClip(cityCenter_.x-ceiling_.HalfWidthTiles(),cityCenter_.z-ceiling_.HalfDepthTiles(),
                           cityCenter_.x+ceiling_.HalfWidthTiles(),cityCenter_.z+ceiling_.HalfDepthTiles());
    // Never send a mono image while the host interprets the surface as SBS.
    // Capture game UI once; repeat only immutable geometry and this texture.
    // A missing/stale tracking packet must reach the clear/present path below.
    // Returning here would leave the previous stereo city on the host surface
    // indefinitely while the simulation is paused or no new UI is captured.
    ceiling_.SetGraphicsEffective(renderer_.RenderScalePercent(), renderer_.MsaaSamples());
    SetSpriteDepth(ceiling_.SpriteBodies());
    if (ceiling_.HighEnd() != appliedHighEnd_) {
        appliedHighEnd_ = ceiling_.HighEnd();
        gta2ceiling::SetHighEndTextureFiltering(appliedHighEnd_);
        SetSpriteDepthLayers(appliedHighEnd_ ? 12 : 6);
        Log("graphics profile applied: %s (anisotropy %lu, mip bias %.2f, body layers %d)",
            appliedHighEnd_ ? "high-end" : "standard",
            static_cast<unsigned long>(gta2ceiling::g_graphicsQuality.anisotropy),
            gta2ceiling::g_graphicsQuality.lod_bias, appliedHighEnd_ ? 12 : 6);
    }
    {
        int scale=100; unsigned samples=0;
        if (ceiling_.TakeGraphicsRequest(scale,samples)) {
            const bool ok=renderer_.ApplyQuality(scale,samples);
            Log("graphics applied=%d: scene %dx%d, render scale %d%%, MSAA %u",ok?1:0,
                renderer_.Width(),renderer_.Height(),renderer_.RenderScalePercent(),renderer_.MsaaSamples());
            ceiling_.SetGraphicsEffective(renderer_.RenderScalePercent(), renderer_.MsaaSamples());
            lastXrSample_=0; // force a fresh pair at the new size
        }
    }
    if (!captureUi && tracked && ceiling_.SampleId() == lastXrSample_) return true;
    // Performance: skip pose-only refreshes the compositor can reproject.
    // The placement menu still follows the head (the simulation is frozen).
    if (!captureUi && tracked && ((!ceiling_.PoseRefreshEnabled() && !ceiling_.Placing()) ||
                                  !ceiling_.PoseRefreshWorthwhile())) {
        NotePacing(false, false, true, false, true);
        lastXrSample_ = ceiling_.SampleId();  // count each still sample once
        return true;
    }
    IDirect3DDevice9* d = renderer_.Device();
    const int w=renderer_.Width(), h=renderer_.Height();
    const D3DVIEWPORT9 full{0,0,static_cast<DWORD>(w),static_cast<DWORD>(h),0,1};
    d->SetViewport(&full);
    if (!renderer_.BeginFrame(ceiling_.MixedReality())) return true;
    if (ceiling_.MixedReality()) gta2ceiling::ClearCeilingBackground(d);
    // Phase timings for the slow-frame log (where do 50-120 ms spikes go?).
    LARGE_INTEGER phase[5]{};
    QueryPerformanceCounter(&phase[0]);
    if (captureUi && renderer_.BeginUiLayer(worldUploaded_)) {
        overlay_.Flush(d,w,h);
        renderer_.EndUiLayer(false);
    }
    QueryPerformanceCounter(&phase[1]);
    double worldMs=0, spriteMs=0;
    if (tracked) {
        for (int i=0;i<2;++i) {
            const D3DVIEWPORT9 viewport{static_cast<DWORD>(i*w/2),0,
                static_cast<DWORD>(w/2),static_cast<DWORD>(h),0,1};
            d->SetViewport(&viewport);
            const auto eye=i==0?gta2ceiling::Eye::Left:gta2ceiling::Eye::Right;
            // Walk out: the player's own sprites go first, before the field's
            // mask and wall band, so the band - which keeps the buildings
            // whole up to their roofs - cannot hide the player behind the
            // disc. They mark their pixels as city themselves (stencil 0x81).
            const std::function<void()> underlay=[&]() {
                d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
                d->SetRenderState(D3DRS_STENCILENABLE, TRUE);
                d->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
                d->SetRenderState(D3DRS_STENCILREF, 0x81);
                d->SetRenderState(D3DRS_STENCILMASK, 0xFF);
                d->SetRenderState(D3DRS_STENCILWRITEMASK, 0xFF);
                d->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
                d->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
                d->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
                d->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED|D3DCOLORWRITEENABLE_GREEN|
                                                         D3DCOLORWRITEENABLE_BLUE);
                live_.DrawFree(d,eyes[i],w/2,h,captureUi && i==0);
            };
            const bool walkOut=worldUploaded_ && WalkOutFree();
            const bool baseReady=(worldUploaded_ || ceiling_.Placing()) &&
                ceiling_.DrawBaseAndClip(d,eye,cityCenter_,walkOut?&underlay:nullptr);
            if (baseReady && worldUploaded_) {
                gta2ceiling::PreserveCeilingCoverage(d);
                QueryPerformanceCounter(&phase[2]);
                renderer_.SetCircleMask(ceiling_.HoloCuts() && ceiling_.RoundField(), cityCenter_.x,
                                        cityCenter_.z, HoloCutRadius());
                renderer_.DrawWorld(eyes[i]);
                renderer_.SetCircleMask(false);
                if (i == 0) {  // how the map lies in view, for the modern controls' stick
                    stickView_ = eyes[0].ViewMatrix();
                    stickProjection_ = eyes[0].ProjectionMatrix(static_cast<float>(w / 2) / static_cast<float>(h));
                    stickViewValid_ = true;
                }
                // Optional (X in the menu): buildings cut open by the field's
                // edge show a glowing holographic inside instead of a hole.
                if (ceiling_.HoloCuts()) DrawHoloCut(d, eyes[i], static_cast<float>(w / 2) / static_cast<float>(h));
                if (ceiling_.Shadows()) {
                    if (i == 0) UpdateShadowLevels();
                    renderer_.DrawWorldShadows(eyes[i], shadowLevels_, shadowLevelCount_);
                }
                SetSpriteContactShadows(ceiling_.Shadows());
                QueryPerformanceCounter(&phase[3]);
                live_.Draw(d,eyes[i],w/2,h,captureUi && i==0);
                if (i == 0 && captureUi) UpdateWalkTrack();
                QueryPerformanceCounter(&phase[4]);
                LARGE_INTEGER f{}; QueryPerformanceFrequency(&f);
                if (f.QuadPart) {
                    worldMs+=1000.0*(phase[3].QuadPart-phase[2].QuadPart)/f.QuadPart;
                    spriteMs+=1000.0*(phase[4].QuadPart-phase[3].QuadPart)/f.QuadPart;
                }
                gta2ceiling::FillCeilingCoverage(d);
            }
            gta2ceiling::ClearClip(d);
            // A failed stencil/base pass also excludes the in-world HUD. The
            // front menu has its own intentional head-relative panel.
            if(!worldUploaded_ || baseReady)
                ceiling_.DrawHud(d,renderer_.UiTexture(),eye,worldUploaded_);
            ceiling_.DrawPlacementGuide(d,eye);
        }
        ceiling_.StampSync(d,w,h);
        ceiling_.SetOcclusionContext(worldUploaded_);
        renderer_.SetPresentStamp(true,ceiling_.SyncColour());
        lastXrSample_=ceiling_.SampleId();
    }
    d->SetViewport(&full);
    renderer_.EndFrame();
    if(!tracked) staleXrFramePresented_=true;
    QueryPerformanceCounter(&finished);
    QueryPerformanceFrequency(&frequency);
    if (frequency.QuadPart) xrRenderSeconds_=static_cast<double>(finished.QuadPart-started.QuadPart)/frequency.QuadPart;
    if (xrRenderSeconds_>0.040) {
        static uint64_t lastSlowLog=0;
        if (GetTickCount64()-lastSlowLog>1000) {
            lastSlowLog=GetTickCount64();
            const double uiMs=frequency.QuadPart?1000.0*(phase[1].QuadPart-phase[0].QuadPart)/frequency.QuadPart:0;
            Log("slow stereo frame %.1f ms (captureUi=%d, world=%d, sprites=%d): ui %.1f, world+shadows %.1f, sprites %.1f, rest %.1f",
                xrRenderSeconds_*1000.0,captureUi?1:0,worldUploaded_?1:0,live_.SpriteQuads(),
                uiMs,worldMs,spriteMs,xrRenderSeconds_*1000.0-uiMs-worldMs-spriteMs);
        }
    }
    if (tracked) ceiling_.RecordPresent(xrRenderSeconds_);
    NotePacing(tracked, captureUi, false, false, tracked);
    return true;
}

namespace {
// ---------------------------------------------------------------------------
// GTA2Ceiling walk out (see walk_out.h): the game's own map, flattened outside
// the frozen disc and put back afterwards.
// ---------------------------------------------------------------------------
// Memory checks through VirtualQuery only: probing by access violation
// crashed the game under Box64. Regions are cached.
bool CheckRegion(const void* p, size_t n, bool write) {
    const uintptr_t at = reinterpret_cast<uintptr_t>(p);
    if (!p || at < 0x10000 || at > 0x7FFF0000 || n == 0) return false;
    static uintptr_t cachedBase = 0, cachedEnd = 0;
    static DWORD cachedProtect = 0;
    if (!(at >= cachedBase && at + n <= cachedEnd)) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(p, &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT) return false;
        cachedBase = reinterpret_cast<uintptr_t>(info.BaseAddress);
        cachedEnd = cachedBase + info.RegionSize;
        cachedProtect = info.Protect;
        if (at + n > cachedEnd) return false;
    }
    const DWORD protect = cachedProtect & 0xFF;
    if ((cachedProtect & PAGE_GUARD) || protect == PAGE_NOACCESS || protect == 0) return false;
    if (!write) return true;
    return protect == PAGE_READWRITE || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_WRITECOPY ||
           protect == PAGE_EXECUTE_WRITECOPY;
}
bool ReadableBlock(const void* p, size_t n) { return CheckRegion(p, n, false); }
bool WritableBlock(void* p, size_t n) { return CheckRegion(p, n, true); }

// [ceiling] walk_out_flat_outside in gta2ceiling.ini, on unless set to 0.
bool FlatOutsideEnabled() {
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = std::strrchr(path, '\\');
    if (!slash) return true;
    std::strcpy(slash + 1, "gta2ceiling.ini");
    return GetPrivateProfileIntA("ceiling", "walk_out_flat_outside", 1, path) != 0;
}

// The running game's DMAP (the same object ReadLiveMap reads at level load),
// with the base array checked writable and both pools checked readable as
// far as the base array - and the originals kept of flattened cells -
// reference them.
bool OpenGameMap(uint8_t* mapObject, size_t blockCount, walkout::MapView* view,
                 const walkout::Flattening* flat = nullptr) {
    if (!mapObject || !WritableBlock(mapObject, gta2::kDmapBaseBytes) ||
        !ReadableBlock(mapObject + gta2::kMapObjectColumnsPtr, 8)) return false;
    view->base = reinterpret_cast<uint32_t*>(mapObject);
    view->columns = *reinterpret_cast<const uint8_t* const*>(mapObject + gta2::kMapObjectColumnsPtr);
    view->blocks = *reinterpret_cast<const uint8_t* const*>(mapObject + gta2::kMapObjectBlocksPtr);
    uint32_t maxWord = 0;
    for (int i = 0; i < walkout::kCells; ++i) {
        uint32_t word = view->base[i];
        if (flat && flat->active && flat->mask[i]) word = flat->original[i];  // ours is not in the pool
        if (word > maxWord && word < kMaxColumnWords) maxWord = word;
    }
    // The last column's own length decides where the pool ends.
    if (!ReadableBlock(view->columns, (static_cast<size_t>(maxWord) + 1) * 4)) return false;
    const uint8_t* last = view->columns + static_cast<size_t>(maxWord) * 4;
    const int count = (std::max)(0, static_cast<int>(last[0]) - static_cast<int>(last[1]));
    view->columnBytes = (static_cast<size_t>(maxWord) + 1 + static_cast<size_t>((std::min)(count, gta2::kMapLevels))) * 4;
    view->blockCount = blockCount;
    return blockCount > 0 && ReadableBlock(view->columns, view->columnBytes) &&
           ReadableBlock(view->blocks, blockCount * 12);
}

// Our own flat ground: one column (just a block at level - 1) and one block -
// a plain road lid without lanes or walls, which pedestrians keep off and no
// traffic is routed over. Placed in memory above both of the game's pools, so
// the game reaches them through its own pointers with ordinary indices
// (column word = distance / 4, block index = distance / 12).
void* MakeOwnFlat(const walkout::MapView& view, int level, uint16_t lid, uint32_t* word) {
    const uintptr_t columns = reinterpret_cast<uintptr_t>(view.columns);
    const uintptr_t blocks = reinterpret_cast<uintptr_t>(view.blocks);
    const uintptr_t above = ((columns > blocks ? columns : blocks) + 0x01000000u) & ~uintptr_t{0xFFFF};
    void* memory = nullptr;
    for (uintptr_t hint = above; hint < 0x7F000000u && !memory; hint += 0x00400000u)
        memory = VirtualAlloc(reinterpret_cast<void*>(hint), 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory) {
        memory = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        const uintptr_t at = reinterpret_cast<uintptr_t>(memory);
        if (memory && (at <= columns || at <= blocks)) { VirtualFree(memory, 0, MEM_RELEASE); return nullptr; }
    }
    if (!memory) return nullptr;
    uint8_t* bytes = static_cast<uint8_t*>(memory);
    const uintptr_t start = reinterpret_cast<uintptr_t>(bytes);
    uint8_t* block = bytes + (12 - (start - blocks) % 12) % 12;
    const uint16_t faces[5] = {0, 0, 0, 0, static_cast<uint16_t>(lid ? lid : 1)};
    std::memcpy(block, faces, sizeof(faces));
    block[10] = 0;  // no lanes
    block[11] = 1;  // flat road
    uint8_t* column = bytes + 64;
    column[0] = static_cast<uint8_t>(level);
    column[1] = static_cast<uint8_t>(level - 1);
    column[2] = column[3] = 0;
    const uint32_t blockIndex = static_cast<uint32_t>((reinterpret_cast<uintptr_t>(block) - blocks) / 12);
    std::memcpy(column + 4, &blockIndex, 4);
    *word = static_cast<uint32_t>((reinterpret_cast<uintptr_t>(column) - columns) / 4);
    return memory;
}

// Where the game's cars are (cell coordinates): a cell one stands on is not
// given its building back, that would shut the car inside it.
std::vector<walkout::Point> CarCells() {
    std::vector<walkout::Point> cars;
    int guard = 0;
    for (const uint8_t* car = game::VehicleListHead(); car && guard < 400; car = game::NextInList(car, game::kVehicleNext), ++guard) {
        if (!ReadableBlock(car, game::kVehiclePlacementPtr + 4)) break;
        float x = 0, y = 0, z = 0;
        const uint8_t* place = game::Placement(car, game::kVehiclePlacementPtr);
        if (!place || !ReadableBlock(place, game::kPlaceZ + 4)) continue;
        if (game::ReadPlacement(car, game::kVehiclePlacementPtr, &x, &y, &z) && x > 0 && y > 0 && x < 256 && y < 256)
            cars.push_back({x, y});
    }
    return cars;
}

// ---------------------------------------------------------------------------
// The game's own player object (walk out): who to draw outside the disc.
// ---------------------------------------------------------------------------
// Readability by page, remembered for one probe: the search reads a lot of
// arbitrary pointers and must never fault.
class PageCache {
public:
    bool Readable(const void* p, size_t n) {
        const uintptr_t at = reinterpret_cast<uintptr_t>(p);
        if (!p || at < 0x10000 || at + n > 0x7FFF0000 || n == 0) return false;
        for (uintptr_t page = at & ~uintptr_t{0xFFF}; page < at + n; page += 0x1000) {
            auto found = pages_.find(page);
            bool ok;
            if (found == pages_.end()) {
                MEMORY_BASIC_INFORMATION info{};
                const DWORD protect = VirtualQuery(reinterpret_cast<void*>(page), &info, sizeof(info)) == sizeof(info)
                                          ? info.Protect : 0;
                ok = info.State == MEM_COMMIT && !(protect & PAGE_GUARD) && (protect & 0xFF) != PAGE_NOACCESS &&
                     (protect & 0xFF) != 0;
                pages_[page] = ok;
            } else {
                ok = found->second;
            }
            if (!ok) return false;
        }
        return true;
    }
private:
    std::unordered_map<uintptr_t, bool> pages_;
};

constexpr int kFollowSprite = 0, kFollowPed = 1;
constexpr int kFollowFrom = -0x300, kFollowTo = 0x600, kFollowSpan = 0x400;

template <typename T> T ReadAt(const uint8_t* p, int offset) {
    T value;
    std::memcpy(&value, p + offset, sizeof(T));
    return value;
}

// Where an object is (cells, x east, y south), read as `kind`.
bool ObjectCell(const uint8_t* p, int kind, PageCache& pages, float* x, float* y) {
    const int at = kind == kFollowPed ? static_cast<int>(game::kPedX) : static_cast<int>(game::kPlaceX);
    if (!pages.Readable(p + at, 8)) return false;
    *x = ReadAt<int32_t>(p, at) * game::kFixedScale;
    *y = ReadAt<int32_t>(p, at + 4) * game::kFixedScale;
    return *x > 0.5f && *y > 0.5f && *x < 255.5f && *y < 255.5f;
}

// The ped behind an object: itself, or a sprite's owner (physics -> ped).
const uint8_t* PedOf(const uint8_t* p, int kind, PageCache& pages) {
    if (kind == kFollowPed) return pages.Readable(p, game::kPedY + 4) ? p : nullptr;
    if (!pages.Readable(p + game::kSpriteObjectOwner, 4)) return nullptr;
    const uint8_t* physics = ReadAt<const uint8_t*>(p, static_cast<int>(game::kSpriteObjectOwner));
    if (!game::PlausiblePointer(physics) || !pages.Readable(physics, game::kPhysicsSprite + 4) ||
        ReadAt<const uint8_t*>(physics, static_cast<int>(game::kPhysicsSprite)) != p) return nullptr;
    const uint8_t* ped = ReadAt<const uint8_t*>(physics, static_cast<int>(game::kPhysicsPed));
    if (!game::PlausiblePointer(ped) || !pages.Readable(ped, game::kPedY + 4) ||
        ReadAt<const uint8_t*>(ped, static_cast<int>(game::kPedPhysics)) != physics) return nullptr;
    return ped;
}

// Every car with someone at the wheel: driver and where the car is.
struct DrivenCar { const void* driver; float x, y; };
std::vector<DrivenCar> DrivenCars(PageCache& pages) {
    std::vector<DrivenCar> cars;
    int guard = 0;
    for (const uint8_t* car = game::VehicleListHead(); car && guard < 400;
         car = pages.Readable(car, game::kVehicleNext + 4) ? game::NextInList(car, game::kVehicleNext) : nullptr, ++guard) {
        if (!pages.Readable(car, game::kVehicleDriver + 4)) break;
        const void* driver = ReadAt<const void*>(car, static_cast<int>(game::kVehicleDriver));
        if (!game::PlausiblePointer(driver)) continue;
        const uint8_t* place = game::Placement(car, game::kVehiclePlacementPtr);
        float x = 0, y = 0;
        if (place && ObjectCell(place, kFollowSprite, pages, &x, &y)) cars.push_back({driver, x, y});
    }
    return cars;
}

// The object camera + a (-> + b) leads to.
const uint8_t* FollowObject(const uint8_t* camera, int a, int b, PageCache& pages) {
    if (!pages.Readable(camera + a, 4)) return nullptr;
    const uint8_t* p = ReadAt<const uint8_t*>(camera, a);
    if (!game::PlausiblePointer(p)) return nullptr;
    if (b < 0) return p;
    if (!pages.Readable(p + b, 4)) return nullptr;
    const uint8_t* q = ReadAt<const uint8_t*>(p, b);
    return game::PlausiblePointer(q) ? q : nullptr;
}

// Where the player is if that object is it: in its car, if it drives one.
bool FollowCell(const uint8_t* p, int kind, const std::vector<DrivenCar>& cars, PageCache& pages, float* x,
                float* y, bool* inCar) {
    if (const uint8_t* ped = PedOf(p, kind, pages)) {
        for (const DrivenCar& car : cars)
            if (car.driver == ped) { *x = car.x; *y = car.y; if (inCar) *inCar = true; return true; }
    }
    if (inCar) *inCar = false;
    return ObjectCell(p, kind, pages, x, y);
}

// The lid of a column's top block, to dress our own flat block the same way.
uint16_t TopLid(const walkout::MapView& view, uint32_t word) {
    walkout::ColumnInfo column;
    if (!walkout::ReadColumn(view, word, &column) || column.height <= column.offset) return 0;
    const uint8_t* block = walkout::BlockRecord(view, column, column.height - 1);
    return block ? walkout::Face(block, 4) : 0;
}
}  // namespace

void WorldView::ProbeFollow(float cameraX, float cameraY) {
    // Every 250 ms (every 2 s once something is trusted): score what is known,
    // look for more. A candidate is trusted after it sat on a drawn sprite
    // within 4.5 tiles of the camera nearly every time for 5 s while moving
    // 6 tiles - which over a drive or a walk only the player itself does.
    const uint64_t now = GetTickCount64();
    if (now < followProbeMs_) return;
    bool trusted = false;
    for (const FollowCandidate& c : follow_) trusted = trusted || c.trusted;
    followProbeMs_ = now + 250;
    uint8_t* camera = game::CameraStruct();
    if (!game::PlausiblePointer(camera)) return;
    PageCache pages;
    const std::vector<DrivenCar> cars = DrivenCars(pages);
    auto onSprite = [&](float x, float y) {
        return live_.SpriteNear(x, static_cast<float>(gta2::kMapHeight) - y, 0.4f);
    };
    for (FollowCandidate& c : follow_) {
        float x = 0, y = 0;
        bool sample = false;
        const uint8_t* p = FollowObject(camera, c.a, c.b, pages);
        if (p && FollowCell(p, c.kind, cars, pages, &x, &y, nullptr)) {
            sample = std::hypot(x - cameraX, y - cameraY) <= 4.5f && onSprite(x, y);
            if (c.haveLast) c.moved += (std::min)(std::hypot(x - c.lastX, y - c.lastY), 2.0f);
            c.lastX = x; c.lastY = y; c.haveLast = true;
        }
        c.score = 0.9f * c.score + (sample ? 0.1f : 0.0f);
        if (sample) ++c.hits;
        // Once trusted it stays a candidate (a ped's own sprite is not drawn
        // while it sits in a car); each frame still has to find it on a sprite.
        const bool good = c.trusted ? c.score >= 0.5f : (c.hits >= 20 && c.moved >= 6.0f && c.score >= 0.85f);
        if (good && !c.trusted) {
            c.trusted = true;
            char path[48];
            const char* sign = c.a < 0 ? "-" : "+";
            const int offset = c.a < 0 ? -c.a : c.a;
            if (c.b >= 0) std::snprintf(path, sizeof(path), "camera%s%#x -> +%#x", sign, offset, c.b);
            else std::snprintf(path, sizeof(path), "camera%s%#x", sign, offset);
            Log("walk out: the player object is %s (%s), score %.2f after %d hits, moved %.0f tiles", path,
                c.kind == kFollowPed ? "ped" : "sprite", c.score, c.hits, c.moved);
            RememberFollow(c);
        }
        c.good = good;
    }
    follow_.erase(std::remove_if(follow_.begin(), follow_.end(),
                                 [](const FollowCandidate& c) { return !c.trusted && c.score < 0.3f; }),
                  follow_.end());
    // Looking for more: often while nothing is trusted, now and then after.
    if (trusted && (++followRounds_ % 8) != 0) return;
    LARGE_INTEGER t0{}, t1{}, f{};
    QueryPerformanceCounter(&t0);
    auto known = [&](int a, int b, int kind) {
        for (const FollowCandidate& c : follow_)
            if (c.a == a && c.b == b && c.kind == kind) return true;
        return false;
    };
    auto consider = [&](const uint8_t* p, int a, int b) {
        if (!game::PlausiblePointer(p) || follow_.size() >= 300) return;
        for (int kind = kFollowSprite; kind <= kFollowPed; ++kind) {
            float x = 0, y = 0;
            if (!FollowCell(p, kind, cars, pages, &x, &y, nullptr)) continue;
            if (std::hypot(x - cameraX, y - cameraY) > 4.5f || !onSprite(x, y) || known(a, b, kind)) continue;
            FollowCandidate c;
            c.a = a; c.b = b; c.kind = kind; c.lastX = x; c.lastY = y; c.haveLast = true;
            follow_.push_back(c);
        }
    };
    for (int a = kFollowFrom; a < kFollowTo; a += 4) {
        if (!pages.Readable(camera + a, 4)) continue;
        const uint8_t* p = ReadAt<const uint8_t*>(camera, a);
        if (!game::PlausiblePointer(p)) continue;
        consider(p, a, -1);
        if (!pages.Readable(p, kFollowSpan)) continue;
        for (int b = 0; b < kFollowSpan; b += 4) consider(ReadAt<const uint8_t*>(p, b), a, b);
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    static int searchLogs = 0;
    if (searchLogs < 3 && f.QuadPart) {
        ++searchLogs;
        Log("walk out: player search %.1f ms, %zu candidates", 1000.0 * (t1.QuadPart - t0.QuadPart) / f.QuadPart,
            follow_.size());
    }
}

// Where the game keeps the player is the same every run, so what the search
// found once is kept in the user's options file and trusted from the start
// (each frame still has to find it on a drawn sprite).
void WorldView::RememberFollow(const FollowCandidate& found) {
    const std::string& path = ceiling_.UserOptionsPath();
    if (path.empty()) return;
    char text[64];
    std::snprintf(text, sizeof(text), "%d,%d,%d", found.a, found.b, found.kind);
    WritePrivateProfileStringA("player", "object", text, path.c_str());
}

void WorldView::RecallFollow() {
    const std::string& path = ceiling_.UserOptionsPath();
    if (path.empty()) return;
    char text[64]{};
    GetPrivateProfileStringA("player", "object", "", text, sizeof(text), path.c_str());
    FollowCandidate c;
    if (std::sscanf(text, "%d,%d,%d", &c.a, &c.b, &c.kind) != 3) return;
    if (c.a < kFollowFrom || c.a >= kFollowTo || c.b < -1 || c.b >= kFollowSpan ||
        (c.kind != kFollowSprite && c.kind != kFollowPed)) return;
    c.trusted = true;
    c.score = 0.6f;
    follow_.push_back(c);
}

void WorldView::ModernRemap(gta2ceiling::GamepadState& state) {
    if (!ceiling_.ModernControls() || !worldUploaded_ || !state.available) return;
    const int deadzone = gamepad_.StickDeadzone(), threshold = gamepad_.TriggerThreshold();
    // Only look the player up when the sample could be changed at all.
    if (std::hypot(static_cast<float>(state.left_x), static_cast<float>(state.left_y)) <= static_cast<float>(deadzone) &&
        state.left_trigger <= threshold && state.right_trigger <= threshold && !(state.buttons & 0x2000)) return;
    gta2ceiling::ModernPlayer player;
    float face[2] = {0, 0};
    player.known = FollowedPlayer(&player.x, &player.y, &player.inCar, face);
    player.faceX = face[0];
    player.faceY = face[1];
    if (player.known && !player.inCar && face[0] == 0 && face[1] == 0) player.known = false;
    gta2ceiling::StickToMap view;
    if (player.known && !player.inCar && stickViewValid_) {
        // Where the player and the cells east and north of it appear to the eye.
        auto seen = [&](float x, float z, float out[2]) {
            const float v[4] = {x, cityCenter_.y, z, 1.0f};
            float e[4], c[4];
            for (int k = 0; k < 4; ++k)
                e[k] = v[0] * stickView_.m[0][k] + v[1] * stickView_.m[1][k] + v[2] * stickView_.m[2][k] + v[3] * stickView_.m[3][k];
            for (int k = 0; k < 4; ++k)
                c[k] = e[0] * stickProjection_.m[0][k] + e[1] * stickProjection_.m[1][k] + e[2] * stickProjection_.m[2][k] +
                       e[3] * stickProjection_.m[3][k];
            if (!(c[3] > 1e-4f)) return false;
            out[0] = c[0] / c[3];
            out[1] = c[1] / c[3];
            return true;
        };
        const float px = player.x, pz = static_cast<float>(gta2::kMapHeight) - player.y;
        float origin[2], east[2], north[2];
        if (seen(px, pz, origin) && seen(px + 0.5f, pz, east) && seen(px, pz + 0.5f, north))
            view = gta2ceiling::SolveStickToMap(origin, east, north);
    }
    const bool applied = modern_.Apply(state, player, view, deadzone, threshold);
    if (applied != modernApplied_) {
        modernApplied_ = applied;
        Log("controls: %s", applied ? (player.inCar ? "modern (car)" : "modern (on foot)") : "classic for now (player or view unknown)");
    }
}

bool WorldView::FollowedPlayer(float* x, float* y, bool* inCar, float* face) {
    uint8_t* camera = game::CameraStruct();
    if (!game::PlausiblePointer(camera)) return false;
    PageCache pages;
    std::vector<DrivenCar> cars;
    bool haveCars = false;
    const FollowCandidate* best = nullptr;
    for (const FollowCandidate& c : follow_) {
        if (!c.trusted || (best && best->score >= c.score)) continue;
        const uint8_t* p = FollowObject(camera, c.a, c.b, pages);
        if (!p) continue;
        if (!haveCars) { cars = DrivenCars(pages); haveCars = true; }
        float cx = 0, cy = 0;
        bool car = false;
        if (!FollowCell(p, c.kind, cars, pages, &cx, &cy, &car)) continue;
        if (!live_.SpriteNear(cx, static_cast<float>(gta2::kMapHeight) - cy, 0.4f)) continue;
        best = &c;
        *x = cx; *y = cy; *inCar = car;
        if (face) {
            face[0] = face[1] = 0;
            // The sprite the body is drawn with carries the heading.
            const uint8_t* sprite = p;
            if (c.kind == kFollowPed) {
                sprite = nullptr;
                if (pages.Readable(p + game::kPedPhysics, 4)) {
                    const uint8_t* physics = ReadAt<const uint8_t*>(p, static_cast<int>(game::kPedPhysics));
                    if (game::PlausiblePointer(physics) && pages.Readable(physics + game::kPhysicsSprite, 4))
                        sprite = ReadAt<const uint8_t*>(physics, static_cast<int>(game::kPhysicsSprite));
                }
            }
            if (!car && game::PlausiblePointer(sprite) && pages.Readable(sprite + game::kPlaceHeading, 2)) {
                const uint16_t heading = ReadAt<uint16_t>(sprite, static_cast<int>(game::kPlaceHeading));
                if (heading < game::kTrigTableEntries) {
                    face[0] = *reinterpret_cast<const int32_t*>(game::kSinTablePtr + heading * 4u) * game::kFixedScale;
                    face[1] = *reinterpret_cast<const int32_t*>(game::kCosTablePtr + heading * 4u) * game::kFixedScale;
                }
            }
        }
    }
    return best != nullptr;
}

namespace {
// A general 4x4 inverse (row vectors, as gta2::Mat4 is used): the eye's view
// matrix carries the city's scale and mirror, so it is not a rigid one.
bool Invert(const gta2::Mat4& in, gta2::Mat4* out) {
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? in.m[r][c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
        if (std::fabs(a[pivot][c]) < 1e-12) return false;
        if (pivot != c) for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[pivot][k]);
        const double scale = 1.0 / a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] *= scale;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double factor = a[r][c];
            if (factor != 0.0) for (int k = 0; k < 8; ++k) a[r][k] -= factor * a[c][k];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out->m[r][c] = static_cast<float>(a[r][c + 4]);
    return true;
}

// The cut's surface: dark teal glass with scanlines and a faint grid.
IDirect3DTexture9* MakeHoloTexture(IDirect3DDevice9* device) {
    IDirect3DTexture9* texture = nullptr;
    constexpr int kSize = 64;
    if (FAILED(device->CreateTexture(kSize, kSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)))
        return nullptr;
    D3DLOCKED_RECT lock{};
    if (SUCCEEDED(texture->LockRect(0, &lock, nullptr, 0))) {
        for (int y = 0; y < kSize; ++y) {
            uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lock.pBits) + y * lock.Pitch);
            for (int x = 0; x < kSize; ++x) {
                uint32_t colour = D3DCOLOR_ARGB(255, 16, 78, 100);                      // glass
                if ((y & 3) == 0) colour = D3DCOLOR_ARGB(255, 38, 150, 182);            // scanline
                if (y % 32 == 0 || x % 32 == 0) colour = D3DCOLOR_ARGB(255, 120, 236, 255);  // grid
                row[x] = colour;
            }
        }
        texture->UnlockRect(0);
    }
    return texture;
}
}  // namespace

// The building standing in a map column, for the cut: from the ground it
// stands on up to its roof. Only what rises above the ground counts - a street
// has nothing, and neither does a sign gantry or a fence, whose faces are
// see-through panels (flat). False: no building here.
static bool BuildingSpan(const gta2::Map& map, int cellX, int cellY, int* bottom, int* top) {
    if (cellX < 0 || cellY < 0 || cellX >= gta2::kMapWidth || cellY >= gta2::kMapHeight) return false;
    const gta2::Column& column = map.ColumnAt(cellX, cellY);
    auto solid = [](const gta2::Face& f) { return static_cast<bool>(f) && !f.IsFlat(); };
    int highest = -1, firstWall = -1, firstLid = -1;
    for (size_t i = 0; i < column.blocks.size(); ++i) {
        const uint32_t index = column.blocks[i];
        if (index >= map.BlockCount()) continue;
        const gta2::Block& block = map.BlockAt(index);
        const int level = column.offset + static_cast<int>(i);
        const bool wall = solid(block.left) || solid(block.right) || solid(block.top) || solid(block.bottom);
        const bool lid = solid(block.lid);
        if (wall && firstWall < 0) firstWall = level;
        if (lid && firstLid < 0) firstLid = level;
        if (wall || lid) highest = level;
    }
    if (highest < 0) return false;
    const int ground = firstLid >= 0 ? firstLid + 1 : column.offset;  // the surface it stands on
    *top = highest + 1;
    if (*top <= ground) return false;  // nothing above the ground: a street, a pavement
    *bottom = firstWall >= 0 ? (std::min)(firstWall, ground) : ground;
    return true;
}

void WorldView::DrawHoloCut(IDirect3DDevice9* d, const gta2::Camera& eye, float aspect) {
    // With the holo cut on, the world mesh is cut exactly at the field's circle
    // (Renderer::SetCircleMask), and through the cut of a building one looks
    // into a hollow shell and its floors. The cut is closed with a surface of
    // its own, on that circle, shaped from the map: along the part of the
    // circle that faces the eye, every building there becomes a holographic
    // face from the ground it stands on to its roof. It is depth tested and
    // written like any wall, so everything behind it - floors, inner walls -
    // is hidden, at the right depth for both eyes.
    if (!d || !ceiling_.RoundField() || map_.Empty()) return;
    DWORD clip = 0;
    d->GetRenderState(D3DRS_CLIPPLANEENABLE, &clip);
    if (!clip) return;  // nothing is cut
    gta2::Mat4 toWorld;
    if (!Invert(eye.ViewMatrix(), &toWorld)) return;
    const float eyeX = toWorld.m[3][0], eyeZ = toWorld.m[3][2];
    // Exactly where the world mesh is cut (Renderer::SetCircleMask).
    const float radius = HoloCutRadius();
    const float scroll = static_cast<float>(GetTickCount64() % 20000) / 20000.0f * 4.0f;
    using CapVertex = HoloVertex;
    std::vector<CapVertex>& vertices = holoVertices_;
    vertices.clear();
    // Around the circle in steps of an eighth of a tile; only the part whose
    // outside faces the eye can be looked through.
    const int steps = (std::max)(24, static_cast<int>(std::ceil(6.2831853f * radius / 0.125f)));
    const float step = 6.2831853f / static_cast<float>(steps);
    for (int k = 0; k < steps; ++k) {
        const float a0 = step * static_cast<float>(k), a1 = a0 + step, am = a0 + 0.5f * step;
        const float nx = std::cos(am), nz = std::sin(am);
        const float mx = cityCenter_.x + radius * nx, mz = cityCenter_.z + radius * nz;
        if (nx * (eyeX - mx) + nz * (eyeZ - mz) <= 0.0f) continue;  // faces away
        // The cell just inside the edge: what the cut goes through.
        const float px = mx - nx * 0.02f, pz = mz - nz * 0.02f;
        int bottom = 0, top = 0;
        if (!BuildingSpan(map_, static_cast<int>(std::floor(px)),
                          static_cast<int>(std::floor(static_cast<float>(gta2::kMapHeight) - pz)), &bottom, &top))
            continue;
        const float x0 = cityCenter_.x + radius * std::cos(a0), z0 = cityCenter_.z + radius * std::sin(a0);
        const float x1 = cityCenter_.x + radius * std::cos(a1), z1 = cityCenter_.z + radius * std::sin(a1);
        const float s0 = radius * a0 * 0.5f, s1 = radius * a1 * 0.5f;
        const float y0 = static_cast<float>(bottom), y1 = static_cast<float>(top);
        const CapVertex q[4] = {{x0, y0, z0, s0, y0 * 0.5f + scroll}, {x1, y0, z1, s1, y0 * 0.5f + scroll},
                                {x1, y1, z1, s1, y1 * 0.5f + scroll}, {x0, y1, z0, s0, y1 * 0.5f + scroll}};
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (int i : order) vertices.push_back(q[i]);
    }
    if (vertices.empty()) return;
    if (holoDevice_ != d) {
        if (holoTexture_) holoTexture_->Release();
        holoTexture_ = MakeHoloTexture(d);
        holoDevice_ = d;
    }
    D3DMATRIX matrix;
    const gta2::Mat4 identity = gta2::Identity();
    std::memcpy(&matrix, identity.m, sizeof(matrix));
    d->SetTransform(D3DTS_WORLD, &matrix);
    const gta2::Mat4 view = eye.ViewMatrix();
    std::memcpy(&matrix, view.m, sizeof(matrix));
    d->SetTransform(D3DTS_VIEW, &matrix);
    const gta2::Mat4 projection = eye.ProjectionMatrix(aspect);
    std::memcpy(&matrix, projection.m, sizeof(matrix));
    d->SetTransform(D3DTS_PROJECTION, &matrix);
    // Same stencil as the city (inside the field, marks city coverage); no clip
    // planes, it lies on them.
    d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    d->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetTexture(0, holoTexture_);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    d->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    // In pieces the ring takes comfortably (DynamicDraw caps one draw's size).
    constexpr size_t kChunk = 6 * 2000;
    for (size_t at = 0; at < vertices.size(); at += kChunk) {
        const size_t n = (std::min)(kChunk, vertices.size() - at);
        gta2::DynamicDraw::Instance().Draw(d, D3DPT_TRIANGLELIST, static_cast<UINT>(n / 3), &vertices[at],
                                          sizeof(CapVertex));
    }
    d->SetTexture(0, nullptr);
    d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetRenderState(D3DRS_CLIPPLANEENABLE, clip);
}

void WorldView::FreeOwnFlat() {
    if (flatMemory_) VirtualFree(flatMemory_, 0, MEM_RELEASE);
    flatMemory_ = nullptr;
}

void WorldView::DropWalkOut(const char* why) {
    // Without writing anything: the map this was made in is gone.
    const bool had = flat_.active;
    ground_.SetFlat(nullptr, 0.0f);
    flat_ = walkout::Flattening{};
    FreeOwnFlat();
    flatMapObject_ = nullptr;
    flatRadius_ = 0.0f;
    flatFailed_ = flatRestorePending_ = false;
    flatWaitFrames_ = 0;
    walkTrack_ = WalkTrack{};
    if (had) Log("walk out: flat ground dropped (%s)", why);
}

void WorldView::UpdateWalkOut() {
    // Only a window of a few tiles around the player is ever flat (see
    // walk_out.h): flattening everything outside the disc at once hung the game.
    constexpr float kFlatNear = 3.5f, kFlatFar = 5.0f;
    uint8_t* mapObject = game::MapObject();
    if (flat_.active && (mapObject != flatMapObject_ || !worldUploaded_)) DropWalkOut("the map changed");
    const bool locked = ceiling_.WorldLocked() && worldUploaded_ && mapObject && mapObject == loadedMapObject_;
    if (!locked && !flat_.active) {
        flatWaitFrames_ = 0;
        flatFailed_ = false;
        return;
    }
    float tileX = 0.0f, tileY = 0.0f;
    const bool haveCamera = game::CameraPosition(&tileX, &tileY);
    // The player's cell position: its own sprite, else the camera following it.
    const float px = walkTrack_.have ? walkTrack_.x : tileX;
    const float py = walkTrack_.have ? static_cast<float>(gta2::kMapHeight) - walkTrack_.z : tileY;
    const bool havePlayer = walkTrack_.have || haveCamera;
    // Flat around the player and around the camera, which trails a fast car.
    std::vector<walkout::Point> around;
    if (walkTrack_.have) around.push_back({px, py});
    if (haveCamera) around.push_back({tileX, tileY});
    walkout::MapView view;
    if (locked) {
        flatRestorePending_ = false;
        if (flatFailed_ || !havePlayer) return;
        if (!flat_.active) {
            // The ground the player stands on is the level of the flat floor
            // outside; a few frames for its sprite to be found first.
            if (!walkTrack_.have && ++flatWaitFrames_ < 20) return;
            static const bool enabled = FlatOutsideEnabled();
            if (!enabled) {
                flatFailed_ = true;
                Log("walk out: walk_out_flat_outside=0, the map outside the disc stays as it is");
                return;
            }
            if (!OpenGameMap(mapObject, map_.BlockCount(), &view)) {
                flatFailed_ = true;
                Log("walk out: the game's map is not readable here; it stays as it is");
                return;
            }
            float surface = cityCenter_.y;
            if (walkTrack_.have) {
                float under = 0.0f;
                surface = ground_.SurfaceAt(walkTrack_.x, walkTrack_.z,
                                            walkTrack_.y + GroundSampler::kCeilingHeadroom, &under)
                              ? under : walkTrack_.y;
            }
            const int level = (std::clamp)(static_cast<int>(std::lround(surface)), 1, walkout::kMaxLevels);
            const walkout::FlatChoice choice = walkout::FindFlatColumn(view, level, px, py);
            if (!choice.found) {
                flatFailed_ = true;
                Log("walk out: no flat ground at level %d in this map; it stays as it is", level);
                return;
            }
            flat_.flatWord = choice.word;
            flat_.level = level;
            flatMapObject_ = mapObject;
            uint32_t ownWord = 0;
            FreeOwnFlat();
            flatMemory_ = MakeOwnFlat(view, level, TopLid(view, choice.word), &ownWord);
            if (flatMemory_) flat_.flatWord = ownWord;
            Log("walk out: flat ground at level %d (surface %.2f): %s (map's best: cell %d,%d, score %d)",
                level, surface, flatMemory_ ? "our own road without lanes" : "the map's column",
                choice.cellX, choice.cellY, choice.score);
        } else if (!OpenGameMap(mapObject, map_.BlockCount(), &view, &flat_)) {
            return;
        }
        const size_t before = flat_.cells.size();
        walkout::Track(view, flat_, cityCenter_.x, cityCenter_.z, ceiling_.FieldRadiusTiles(), around, kFlatNear, kFlatFar,
                       CarCells(), 1.3f);
        ground_.SetFlat(flat_.mask.data(), static_cast<float>(flat_.level));
        if ((before == 0) != flat_.cells.empty())
            Log("walk out: %s (%zu flat cells around the player at %.1f,%.1f)",
                flat_.cells.empty() ? "back inside the disc" : "outside the disc", flat_.cells.size(), px, py);
        return;
    }
    // Released: the map goes back exactly as it was - but not while that would
    // shut the player inside a building; until then the flat window follows it.
    if (!OpenGameMap(mapObject, map_.BlockCount(), &view, &flat_)) {
        // Tried again next frame: dropping the originals would leave it flat.
        if (!flatRestorePending_) { flatRestorePending_ = true; Log("walk out: the game's map is not readable right now"); }
        return;
    }
    if (havePlayer && walkout::PlayerInsideOriginal(view, flat_, px, py, 1.5f)) {
        if (!flatRestorePending_) {
            flatRestorePending_ = true;
            Log("walk out: the player is where a building stands; the map goes back once it is clear of it");
        }
        walkout::Track(view, flat_, 0.0f, 0.0f, -1.0f, around, kFlatNear, kFlatFar, CarCells(), 1.3f);
        return;
    }
    // Everything back, except under a car that still stands on flat ground.
    const std::vector<walkout::Point> none;
    walkout::Track(view, flat_, 0.0f, 0.0f, -1.0f, none, kFlatNear, kFlatFar, CarCells(), 1.3f);
    if (!flat_.cells.empty()) {
        if (!flatRestorePending_) {
            flatRestorePending_ = true;
            Log("walk out: %zu cells wait for cars standing on them", flat_.cells.size());
        }
        return;
    }
    const int restored = walkout::RestoreAll(view, flat_);
    ground_.SetFlat(nullptr, 0.0f);
    FreeOwnFlat();
    flatMapObject_ = nullptr;
    flatRadius_ = 0.0f;
    flatRestorePending_ = flatFailed_ = false;
    flatWaitFrames_ = 0;
    Log("walk out: the game's map is back as it was (%d cells)", restored);
}

void WorldView::UpdateWalkTrack() {
    if (!WalkOutFree()) { walkTrack_ = WalkTrack{}; return; }
    float tileX = 0.0f, tileY = 0.0f;
    if (!game::CameraPosition(&tileX, &tileY)) return;
    const float camX = tileX, camZ = static_cast<float>(gta2::kMapHeight) - tileY;
    LiveGeometry::FocusInfo focus;
    float fromCamera = -1.0f;
    if (live_.FreeFocus(&focus)) {
        const bool switched = live_.FreeSwitched();
        const bool continuing = walkTrack_.have && !switched;
        if (continuing && (focus.area > 0.15f) != (walkTrack_.area > 0.15f))
            Log("walk out: the player is %s now at (%.1f,%.1f)", focus.area > 0.15f ? "in a car" : "on foot", focus.x, focus.z);
        if (switched)
            Log("walk out: the player got %s at (%.1f,%.1f)", focus.area > 0.15f ? "into a car" : "out of the car",
                focus.x, focus.z);
        walkTrack_.vx = continuing ? 0.5f * walkTrack_.vx + 0.5f * (focus.x - walkTrack_.x) : 0.0f;
        walkTrack_.vz = continuing ? 0.5f * walkTrack_.vz + 0.5f * (focus.z - walkTrack_.z) : 0.0f;
        walkTrack_.x = focus.x;
        walkTrack_.y = focus.y;
        walkTrack_.z = focus.z;
        walkTrack_.area = focus.area;
        walkTrack_.have = true;
        walkTrack_.misses = 0;
        fromCamera = std::hypot(focus.x - camX, focus.z - camZ);
        walkCameraDistanceMax_ = (std::max)(walkCameraDistanceMax_, fromCamera);
        if (!continuing)
            Log("walk out: the player is the %s at (%.1f,%.1f), %.2f tiles from the camera (%d sprites)",
                focus.area > 0.15f ? "car" : "person", focus.x, focus.z, fromCamera, live_.FreeSprites());
        // The camera trails a fast car by three tiles and more; only far beyond
        // that is this not what it follows.
        if (fromCamera > 6.0f) {
            Log("walk out: that sprite is %.1f tiles from the camera, looking again", fromCamera);
            walkTrack_.have = false;
        }
    } else if (walkTrack_.have) {
        // Not seen this frame: keep coasting along, give up after half a second.
        walkTrack_.x += walkTrack_.vx;
        walkTrack_.z += walkTrack_.vz;
        if (++walkTrack_.misses <= 15) return;
        Log("walk out: the player's sprite is gone, looking near the camera again");
        walkTrack_.have = false;
    }
    const uint64_t now = GetTickCount64();
    if (now - walkLogMs_ >= 2000) {
        walkLogMs_ = now;
        const float fromCentre = walkTrack_.have ? std::hypot(walkTrack_.x - cityCenter_.x, walkTrack_.z - cityCenter_.z) : -1.0f;
        Log("walk out: %s, player %.1f tiles from the disc centre (radius %.1f), %.2f from the camera (max %.2f), "
            "%d sprites free, %s",
            ceiling_.WorldLocked() ? "frozen" : "easing back", fromCentre, ceiling_.FieldRadiusTiles(), fromCamera,
            walkCameraDistanceMax_, live_.FreeSprites(),
            flat_.active ? (flatRestorePending_ ? "map waits to go back" : "outside is flat") : "map as it is");
        walkCameraDistanceMax_ = 0.0f;
    }
}

void WorldView::WaitForPlacement() {
    // Stay on the game's graphics thread with its complete snapshot frozen.
    // No GTA simulation boundary is crossed while the user edits the ceiling.
    while (ceiling_.Placing() && window_ && IsWindow(window_)) {
        MSG message;
        while (PeekMessageA(&message,nullptr,0,0,PM_REMOVE)) {
            if (message.message==WM_QUIT) {
                gta2ceiling::GamepadState disconnected{}; disconnected.available=false;
                ceiling_.ConsumeController(disconnected);
                PostQuitMessage(static_cast<int>(message.wParam)); return;
            }
            TranslateMessage(&message); DispatchMessageA(&message);
        }
        gamepad_.Tick();
        // At most the game's own rate (the host reprojects the world-locked
        // menu for every head pose). Rendering every head sample here doubled
        // the GPU load while the menu was open, and the Quest did not recover.
        const uint64_t now = GetTickCount64();
        if (now - lastPlacementRenderMs_ >= 33 && EnsureDevice()) {
            lastPlacementRenderMs_ = now;
            RenderStereo(false);
        }
        Sleep(2);
    }
}

void WorldView::RenderFrame() {
    gamepad_.Tick();
    ceiling_.RecordGameBoundary();
    if (!EnsureDevice()) { FrameLimitWait(); return; }

    if (window_ != gameWindow_) {
        // The game pumps its own window, not ours, and an unpumped window is a
        // hung one as far as Windows is concerned.
        MSG message;
        while (PeekMessageA(&message, window_, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        // The game is free to reorder or resize its own children, so where
        // ours belongs is checked now and then - but only re-stated when it
        // actually moved: under Wine every SetWindowPos is a round trip to the
        // X server, which restacks and reconfigures the window it presents to.
        if ((frameCount_ & 0x3F) == 0) {
            RECT client{};
            GetClientRect(window_, &client);
            bool misplaced = !IsWindowVisible(window_) || client.right - client.left != width_ ||
                             client.bottom - client.top != height_;
            if (present_ == PresentWindow::Topmost) {
                misplaced = misplaced || !(GetWindowLongA(window_, GWL_EXSTYLE) & WS_EX_TOPMOST);
            } else if (HWND parent = GetParent(window_)) {
                misplaced = misplaced || GetWindow(parent, GW_CHILD) != window_;
            }
            if (misplaced) {
                static int restated = 0;
                if (restated++ < 20) Log("present window moved or covered: putting it back");
                SetWindowPos(window_, present_ == PresentWindow::Topmost ? HWND_TOPMOST : HWND_TOP, 0, 0,
                             width_, height_,
                             SWP_NOACTIVATE | SWP_SHOWWINDOW |
                                 (present_ == PresentWindow::Topmost ? SWP_NOMOVE : 0));
            }
        }
    }

    EnsureWorldLoaded();
    RefreshAnimatedTiles();
    UpdateCamera();

    // The game's own screen size is what its HUD coordinates are expressed in,
    // and it is not our back buffer size. Prefer the camera struct, which holds
    // it as plain pixel integers; gbh_SetWindow covers the menus, where no
    // camera exists yet.
    // Only while a world is loaded. The camera struct pointer outlives the level
    // it described, so at the menu this reads a freed allocation - which laid the
    // front end out to the wrong scale, and made it jitter as the allocator
    // handed that memory to something else. gbh_SetWindow is the authority there,
    // which is what its own comment in overlay.cpp has always said.
    if (worldUploaded_) {
        if (const uint8_t* camera = game::CameraStruct()) {
            const int32_t screenW =
                *reinterpret_cast<const int32_t*>(camera + game::kScreenWidthOffset);
            const int32_t screenH =
                *reinterpret_cast<const int32_t*>(camera + game::kScreenHeightOffset);
            if (screenW >= 320 && screenW <= 8192 && screenH >= 200 && screenH <= 8192) {
                overlay_.SetGameScreenSize(screenW, screenH);
            }
        }
    }

    // The bridge server is a separate process that is still starting while the
    // game asks its renderer to initialise, so this is retried from the frame
    // loop until it takes, exactly like the device is.
    RemixApiInit();
    // Not conditional on that: when Remix is absent the menu is the thing that
    // says so, which it cannot do if it only exists once Remix is there.
    DebugMenuInit(gameWindow_, window_, renderer_.Device());
    DebugMenuPoll();

    // The sky. Runs whether or not Remix answered - the clock is ours and the
    // menu shows it either way - and pushes the sun when it can.
    TimeOfDayUpdate();

    // Lights for the effects the game draws but never lights, read straight off
    // its particle and vehicle lists. Before the reconcile, so they are matched
    // and handled in the same pass as the game's own.
    SyntheticLightsUpdate();
    // The light benchmark's own lights, when one is running. See light_bench.h.
    LightBenchSubmit();

    // The game lists this frame's lights during its own world pass, which runs
    // between gbh_BeginScene and the gbh_EndScene that brought us here - so by
    // now the list is complete and can be matched against what Remix already has.
    // Timed, for the benchmark: this is where every CreateLight crosses the bridge.
    LARGE_INTEGER lightTimer[4] = {};
    QueryPerformanceCounter(&lightTimer[0]);
    LightsReconcile();
    QueryPerformanceCounter(&lightTimer[1]);

    {
        // Memory, every 10 s: whether this process (or its D3D9 resources)
        // grows over a session. gta2dx9.log "memory".
        static uint64_t lastMemoryLog = 0;
        const uint64_t nowMs = GetTickCount64();
        if (nowMs - lastMemoryLog >= 10000) {
            lastMemoryLog = nowMs;
            PROCESS_MEMORY_COUNTERS_EX counters{};
            counters.cb = sizeof(counters);
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                 sizeof(counters));
            IDirect3DDevice9* device = renderer_.Device();
            int spriteShapes = 0, spriteTextures = 0;
            SpriteShapeCounts(&spriteShapes, &spriteTextures);
            Log("memory: private %u MB, working set %u MB, D3D9 texture memory free %u MB, sprite shapes %d (%d textures)",
                static_cast<unsigned>(counters.PrivateUsage >> 20), static_cast<unsigned>(counters.WorkingSetSize >> 20),
                device ? device->GetAvailableTextureMem() >> 20 : 0u, spriteShapes, spriteTextures);
        }
    }
    if (!RenderStereo(true) && renderer_.BeginFrame()) {
        renderer_.ClearWorldClip();
        renderer_.DrawWorld(camera_);
        // Sprites and the captured block shapes are real world geometry, so they
        // are depth tested against the static mesh rather than painted over it.
        live_.Draw(renderer_.Device(), camera_, renderer_.Width(), renderer_.Height());
        // The 2D stream goes into a layer that is kept between frames, because
        // GTA2's menu only redraws what changed and clears the screen just when
        // it wants a repaint. In a level that reasoning does not apply - the HUD
        // is redrawn every frame and the world under it moves - so the layer is
        // cleared every frame there, which is what it always did.
        if (renderer_.BeginUiLayer(worldUploaded_)) {
            overlay_.Flush(renderer_.Device(), renderer_.Width(), renderer_.Height());
            renderer_.EndUiLayer();
        } else {
            // Straight onto the back buffer, so these draws are the frame's UI
            // themselves. See Renderer::MarkDrawsAsScreenUi.
            renderer_.MarkDrawsAsScreenUi();
            overlay_.Flush(renderer_.Device(), renderer_.Width(), renderer_.Height());
        }
        // ImGui issues ordinary D3D9 draws, so it belongs inside the scene.
        DebugMenuRender();
        // Remix clears its per-frame light list every frame, so a light is in the
        // scene exactly when it is drawn here. Last thing before the flip.
        QueryPerformanceCounter(&lightTimer[2]);
        LightsDraw();
        QueryPerformanceCounter(&lightTimer[3]);
        renderer_.EndFrame();
    }

    ++frameCount_;
    if ((frameCount_ & 0xFF) == 0) {
        Log("raster pair @%d: %d draws, %d triangles",frameCount_,renderer_.Stats().drawCalls,renderer_.Stats().triangles);
        // What conforming actually did, which is also the answer to the one
        // question the design could not settle by reading the game: whether GTA2
        // gives a sprite the true height of the ramp under it or the top of the
        // block. A street full of traffic with maxgap near zero says it gives the
        // true height and the conform is doing its job; a persistent gap the size
        // of a ramp step says it does not, and the tilt fade is quietly switching
        // itself off on every slope in the city.
        const LiveGeometry::Conform& c = live_.ConformCounts();
        Log("frame %d: %d sprite quads | conform grounded=%d airborne=%d noground=%d capped=%d "
            "straddled=%d maxgap=%.3f maxresidual=%.3f",
            frameCount_, live_.SpriteQuads(), c.grounded, c.airborne, c.noGround, c.capped,
            c.straddled, c.maxGap, c.maxResidual);
    }
    // Anything here is a sprite the player should have seen and did not, or a
    // texture built from artwork that was not ready.
    if ((frameCount_ & 0x3F) == 0) {
        const LiveGeometry::Drops& drops = live_.DropCounts();
        // accepted != drawn is the flicker itself: a sprite the game asked for,
        // that we took, that never reached the device.
        if (!drops.Quiet() || !g_trouble.Quiet() || drops.accepted != drops.drawn) {
            Log("sprite trouble @%d: accepted=%d drawn=%d | dropped expand=%d wrongarray=%d "
                "nonfinite=%d outside=%d degenerate=%d notexture=%d | textures built=%d "
                "nopalette=%d blackpalette=%d pixelsmoved=%d whilelocked=%d lockfail=%d",
                frameCount_, drops.accepted, drops.drawn, drops.expandFlag, drops.notTheSpriteArray,
                drops.notFinite, drops.outOfWorld, drops.degenerate, drops.noTexture,
                g_trouble.built, g_trouble.paletteMissing, g_trouble.paletteAllBlack,
                g_trouble.pixelsMovedSilently, g_trouble.builtWhileLocked, g_trouble.lockFailed);
            live_.ClearDropCounts();
            g_trouble = TextureTrouble{};
        }
    }
    // The game's own view rectangle against the frame we draw. With the view
    // patch in, the two widths agree; if cars ever pop in the margins again
    // this line says at once whether gta2.exe is still reasoning 4:3.
    if ((frameCount_ & 0xFF) == 0) {
        game::ViewExtent view;
        if (game::VisibleExtent(&view) && renderer_.Height() > 0) {
            const float aspect =
                static_cast<float>(renderer_.Width()) / static_cast<float>(renderer_.Height());
            Log("view @%d: game %.2f x %.2f tiles | frame %.2f x %.2f | %s", frameCount_,
                view.Width(), view.Height(), visibleTiles_ * aspect, visibleTiles_,
                SpawnOffscreenActive() ? "gta2.exe patched" : "gta2.exe as shipped");
        }
    }

    if ((frameCount_ & 0x3F) == 0) DumpCameraStruct();

    // Last thing in the frame, after the flip: GTA2's own pacer is a checkbox
    // between 30 fps and none at all, so the number lives here instead. See
    // frame_limiter.h for what a cap above 30 does to the game's speed.
    // Pose refreshes reuse this complete game frame while the 30 Hz simulation
    // waits. All D3D calls stay on its owning thread, and no game state advances.
    WaitForPlacement();
    FrameLimitWait([](void* context, double remaining) {
        WorldView* self=static_cast<WorldView*>(context);
        self->gamepad_.Tick();
        self->WaitForPlacement();
        // Between game frames a new pair is only wanted while the placement
        // menu follows the head, or when pose_refresh is enabled; the host
        // reprojects every display frame itself. Otherwise do not wake up
        // every millisecond (all of it emulated x86): poll input every ~4 ms.
        const bool pairs = self->ceiling_.Active() && self->ceiling_.PoseRefreshEnabled();
        if (pairs && remaining > self->xrRenderSeconds_*1.25+0.001)
            self->RenderStereo(false);
        else if (!pairs && remaining > 0.006)
            Sleep(3);
    },this);

    if (LightBenchRunning()) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        const double toMs = 1000.0 / static_cast<double>(frequency.QuadPart);
        LightBenchRecord(static_cast<double>(lightTimer[1].QuadPart - lightTimer[0].QuadPart) * toMs,
                         static_cast<double>(lightTimer[3].QuadPart - lightTimer[2].QuadPart) * toMs);
    }
}

}  // namespace gta2dx9
