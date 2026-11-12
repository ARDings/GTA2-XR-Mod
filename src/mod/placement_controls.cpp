#include "placement_controls.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gta2ceiling {
namespace {
constexpr float Pi = 3.14159265358979323846f;
constexpr uint16_t Chord = 0x0040 | 0x0080;
constexpr uint16_t A = 0x1000, B = 0x2000, LB = 0x0100, RB = 0x0200;
constexpr int Deadzone = 7849;
constexpr const char* Keys[] = {
    "format_version", "distance_millimeters", "width_millimeters",
    "depth_millimeters", "scale_millionths", "yaw_milliradians",
    "offset_x_millimeters", "offset_z_millimeters"
};

bool Finite(const PlacementSettings& s) {
    return std::isfinite(s.distance_m) && std::isfinite(s.width_m) &&
        std::isfinite(s.depth_m) && std::isfinite(s.meters_per_tile) &&
        std::isfinite(s.yaw_radians) && std::isfinite(s.offset_x_m) &&
        std::isfinite(s.offset_z_m);
}
float Bounded(float value, float low, float high, float fallback) {
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}
float Axis(int16_t value) {
    const int magnitude = std::abs(static_cast<int>(value));
    if (magnitude <= Deadzone) return 0;
    const int limit = value < 0 ? 32768 : 32767;
    const float unit = static_cast<float>(magnitude - Deadzone) / (limit - Deadzone);
    return value < 0 ? -unit : unit;
}
bool Neutral(const GamepadState& state) {
    return state.buttons == 0 && Axis(state.left_x) == 0 && Axis(state.left_y) == 0 &&
        Axis(state.right_x) == 0 && Axis(state.right_y) == 0 &&
        state.left_trigger <= 30 && state.right_trigger <= 30;
}
std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
std::string Serialize(const PlacementSettings& original) {
    const auto s = ClampPlacement(original);
    const std::array<long, 8> values{{2, std::lround(s.distance_m * 1000),
        std::lround(s.width_m * 1000), std::lround(s.depth_m * 1000),
        std::lround(s.meters_per_tile * 1000000), std::lround(s.yaw_radians * 1000),
        std::lround(s.offset_x_m * 1000), std::lround(s.offset_z_m * 1000)}};
    std::string result = "; GTA2Ceiling personal placement. Original game files stay unchanged.\n[ceiling]\n";
    for (size_t i = 0; i < values.size(); ++i)
        result += std::string(Keys[i]) + "=" + std::to_string(values[i]) + "\n";
    return result;
}
bool Parse(const std::string& text, PlacementSettings& output, std::string& error) {
    std::array<int, 8> values{};
    std::array<bool, 8> seen{};
    bool section = false;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line == "[ceiling]" && !section) { section = true; continue; }
        const auto equals = line.find('=');
        if (!section || equals == std::string::npos) {
            error = "User placement file has an unexpected section or line."; return false;
        }
        const auto key = Trim(line.substr(0, equals));
        const auto value = Trim(line.substr(equals + 1));
        size_t index = 0;
        while (index < std::size(Keys) && key != Keys[index]) ++index;
        if (index == std::size(Keys) || seen[index] || value.empty()) {
            error = "User placement file has an unknown, duplicate or empty setting."; return false;
        }
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), values[index]);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
            error = "User placement file contains an invalid number."; return false;
        }
        seen[index] = true;
    }
    if (!std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }) || values[0] < 1 || values[0] > 2) {
        error = "User placement file is incomplete or uses an unsupported format."; return false;
    }
    PlacementSettings candidate{values[1] / 1000.0f, values[2] / 1000.0f,
        values[3] / 1000.0f, values[4] / 1000000.0f, values[5] / 1000.0f,
        values[6] / 1000.0f, values[7] / 1000.0f};
    // Format 1 was saved with the older, smaller default city. Keep the
    // person's relative choice but enlarge it once to the new 1.5x baseline.
    if (values[0] == 1) candidate.meters_per_tile *= 1.5f;
    output = ClampPlacement(candidate);
    return true;
}

#ifdef _WIN32
bool CheckedDirectory(const std::filesystem::path& input, std::filesystem::path& output, std::string& error) {
    if (input.empty()) { error = "A game directory is required for placement settings."; return false; }
    for (const auto& part : input)
        if (part == L"..") { error = "Parent traversal is not a placement directory."; return false; }
    const auto directory = std::filesystem::absolute(input).lexically_normal();
    for (auto part = directory; !part.empty(); part = part.parent_path()) {
        const DWORD attributes = GetFileAttributesW(part.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            error = "Placement directory is missing, is not a directory, or redirects through a link.";
            return false;
        }
        if (part == part.parent_path()) break;
    }
    output = directory;
    return true;
}
bool CheckedTarget(const std::filesystem::path& path, bool& exists, std::string& error) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND) { exists = false; return true; }
        error = "Cannot inspect user placement file (Windows error " + std::to_string(code) + ").";
        return false;
    }
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        error = "User placement target must be a regular file, not a directory or link.";
        return false;
    }
    exists = true;
    return true;
}
struct StagedFile {
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::filesystem::path path;
    ~StagedFile() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        if (!path.empty()) DeleteFileW(path.c_str());
    }
};
#endif
} // namespace

PlacementSettings ClampPlacement(PlacementSettings s) noexcept {
    const PlacementSettings defaults;
    s.distance_m = Bounded(s.distance_m, 0.7f, 4.0f, defaults.distance_m);
    s.width_m = Bounded(s.width_m, 0.5f, 10.0f, defaults.width_m);
    s.depth_m = Bounded(s.depth_m, 0.5f, 10.0f, defaults.depth_m);
    s.meters_per_tile = Bounded(s.meters_per_tile, 0.01f, 0.3f, defaults.meters_per_tile);
    s.offset_x_m = Bounded(s.offset_x_m, -5.0f, 5.0f, 0);
    s.offset_z_m = Bounded(s.offset_z_m, -5.0f, 5.0f, 0);
    s.yaw_radians = std::isfinite(s.yaw_radians) ? std::remainder(s.yaw_radians, 2 * Pi) : 0;
    return s;
}

PlacementFileStore::PlacementFileStore(std::filesystem::path directory) : directory_(std::move(directory)) {}
bool PlacementFileStore::Load(PlacementSettings& settings, std::string& error) {
    error.clear();
#ifdef _WIN32
    try {
        std::filesystem::path directory;
        if (!CheckedDirectory(directory_, directory, error)) return false;
        const auto target = directory / L"gta2ceiling_user.ini";
        bool exists = false;
        if (!CheckedTarget(target, exists, error) || !exists) return false;
        if (std::filesystem::file_size(target) > 4096) { error = "User placement file is too large."; return false; }
        std::ifstream input(target, std::ios::binary);
        if (!input) { error = "Cannot read user placement file."; return false; }
        std::string text(std::istreambuf_iterator<char>(input), {});
        if (input.bad() || text.size() > 4096 || text.find('\0') != std::string::npos) {
            error = "User placement file is unreadable or invalid."; return false;
        }
        return Parse(text, settings, error);
    } catch (const std::exception& failure) { error = failure.what(); return false; }
#else
    (void)settings;
    error = "Placement files require Windows/Wine."; return false;
#endif
}
bool PlacementFileStore::Save(const PlacementSettings& settings, std::string& error) {
    error.clear();
    if (!Finite(settings)) { error = "Placement contains a non-finite value."; return false; }
#ifdef _WIN32
    try {
        std::filesystem::path directory;
        if (!CheckedDirectory(directory_, directory, error)) return false;
        const auto target = directory / L"gta2ceiling_user.ini";
        bool exists = false;
        if (!CheckedTarget(target, exists, error)) return false;
        static std::atomic<unsigned long> sequence{0};
        StagedFile staged;
        const auto temporary = directory / (L"gta2ceiling_user.ini.tmp." + std::to_wstring(GetCurrentProcessId()) +
            L"." + std::to_wstring(++sequence));
        staged.handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (staged.handle == INVALID_HANDLE_VALUE) { error = "Cannot create exclusive user-placement temporary file."; return false; }
        staged.path = temporary; // Only an actually created file belongs to us.
        const auto text = Serialize(settings);
        DWORD written = 0;
        if (!WriteFile(staged.handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
            written != text.size() || !FlushFileBuffers(staged.handle)) {
            error = "Cannot write/flush user-placement temporary file."; return false;
        }
        if (!CloseHandle(staged.handle)) { error = "Cannot close user-placement temporary file."; return false; }
        staged.handle = INVALID_HANDLE_VALUE;
        if (!CheckedTarget(target, exists, error)) return false;
        if (!MoveFileExW(staged.path.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = "Cannot replace user placement; previous settings remain (Windows error " +
                std::to_string(GetLastError()) + ")."; return false;
        }
        staged.path.clear();
        return true;
    } catch (const std::exception& failure) { error = failure.what(); return false; }
#else
    error = "Atomic placement replacement requires Windows/Wine."; return false;
#endif
}

PlacementControls::PlacementControls(PlacementSettings initial, PlacementPersistence& persistence)
    : persistence_(persistence), committed_(ClampPlacement(initial)), preview_(committed_) {}
bool PlacementControls::LoadSaved() {
    if (active_) { error_ = "Finish or cancel placement before loading settings."; return false; }
    PlacementSettings candidate = committed_;
    if (!persistence_.Load(candidate, error_)) return false;
    committed_ = preview_ = ClampPlacement(candidate);
    return true;
}
void PlacementControls::Cancel() {
    if (!active_) return;
    active_ = false;
    chord_hold_ = false;
    preview_ = committed_;
    wait_neutral_ = true;
    error_.clear();
    event_ = PlacementEvent::Cancelled;
}
bool PlacementControls::SetOffsets(float offset_x_m, float offset_z_m) {
    if (active_) { error_ = "Finish placement before recentring."; return false; }
    PlacementSettings candidate = committed_;
    candidate.offset_x_m = offset_x_m;
    candidate.offset_z_m = offset_z_m;
    candidate = ClampPlacement(candidate);
    if (!persistence_.Save(candidate, error_)) return false;
    committed_ = preview_ = candidate;
    return true;
}
PlacementEvent PlacementControls::TakeEvent() noexcept {
    const auto event = event_;
    event_ = PlacementEvent::None;
    return event;
}
bool PlacementControls::ConsumeInput(const GamepadState& state, double delta_seconds) {
    if (!state.available) {
        Cancel();
        wait_neutral_ = true;
        chord_armed_ = false;
        previous_buttons_ = 0;
        return true;
    }
    const uint16_t pressed = state.buttons & static_cast<uint16_t>(~previous_buttons_);
    previous_buttons_ = state.buttons;
    if (!(state.buttons & Chord)) chord_armed_ = true;
    if (wait_neutral_) {
        if (Neutral(state)) { wait_neutral_ = false; previous_buttons_ = 0; chord_armed_ = true; }
        return true; // Even the release sample belongs to this modal interaction.
    }
    if (!active_) {
        if ((state.buttons & Chord) == Chord && chord_armed_) {
            chord_armed_ = false;
            active_ = true;
            preview_ = committed_;
            error_.clear();
            event_ = PlacementEvent::Opened;
            chord_hold_ = true;
            chord_hold_seconds_ = 0;
            return true; // Held A/B on opening cannot confirm/cancel accidentally.
        }
        return false;
    }
    if (chord_hold_) {
        // Still holding the opening chord: nothing else counts until the
        // sticks are released (holding them longer no longer does anything).
        if ((state.buttons & Chord) == Chord) return true;
        chord_hold_ = false;
    }
    if (pressed & B) { Cancel(); return true; } // Cancel wins if both are pressed.
    if (pressed & A) {
        if (persistence_.Save(preview_, error_)) {
            committed_ = preview_;
            active_ = false;
            wait_neutral_ = true;
            event_ = PlacementEvent::Committed;
        } else event_ = PlacementEvent::SaveFailed;
        return true;
    }
    const float seconds = std::isfinite(delta_seconds) ? static_cast<float>(std::clamp(delta_seconds, 0.0, 0.25)) : 0;
    float x = Axis(state.left_x), z = -Axis(state.left_y);
    const float length = std::sqrt(x*x + z*z);
    if (length > 1) { x /= length; z /= length; }
    preview_.offset_x_m += x * 0.5f * seconds;
    preview_.offset_z_m += z * 0.5f * seconds;
    preview_.distance_m += Axis(state.right_y) * 0.35f * seconds;
    preview_.yaw_radians -= Axis(state.right_x) * (Pi / 4) * seconds;
    const int width = ((pressed & 0x0008) && !(state.buttons & 0x0004)) -
        ((pressed & 0x0004) && !(state.buttons & 0x0008));
    const int depth = ((pressed & 0x0001) && !(state.buttons & 0x0002)) -
        ((pressed & 0x0002) && !(state.buttons & 0x0001));
    const int scale = ((pressed & RB) && !(state.buttons & LB)) -
        ((pressed & LB) && !(state.buttons & RB));
    preview_.width_m += width * 0.1f;
    preview_.depth_m += depth * 0.1f;
    preview_.meters_per_tile += scale * 0.005f;
    preview_ = ClampPlacement(preview_);
    return true;
}
const char* PlacementControls::GuideText() noexcept {
    return "Align the ceiling\nLeft stick: move\nRight stick: height / rotate\n"
        "D-pad: width / depth\nLB / RB: city size\nA: save   B: discard";
}
} // namespace gta2ceiling
