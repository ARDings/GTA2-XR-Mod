#pragma once

#include "gamepad_bridge.h"
#include <filesystem>
#include <string>

namespace gta2ceiling {

// LOCAL-space placement relative to the fixed reference used for the ceiling.
// The renderer owns that reference; controller edits never follow head motion.
struct PlacementSettings {
    float distance_m = 1.6f;
    float width_m = 2.4f;
    float depth_m = 2.4f;
    float meters_per_tile = 0.12f;
    float yaw_radians = 0;
    float offset_x_m = 0;
    float offset_z_m = 0;
};

PlacementSettings ClampPlacement(PlacementSettings settings) noexcept;

class PlacementPersistence {
public:
    virtual ~PlacementPersistence() = default;
    // Failure leaves both the output settings and any existing file unchanged.
    // A missing file is false with an empty error; use the supplied defaults.
    virtual bool Load(PlacementSettings& settings, std::string& error) = 0;
    virtual bool Save(const PlacementSettings& settings, std::string& error) = 0;
};

// Only gta2ceiling_user.ini and an exclusive same-directory temporary file are
// writable. No game files, original configuration, registry or player saves.
class PlacementFileStore final : public PlacementPersistence {
public:
    explicit PlacementFileStore(std::filesystem::path game_directory);
    bool Load(PlacementSettings& settings, std::string& error) override;
    bool Save(const PlacementSettings& settings, std::string& error) override;
private:
    std::filesystem::path directory_;
};

enum class PlacementEvent { None, Opened, Committed, Cancelled, SaveFailed, RecenterRequested };

// Holding both sticks this long recentres the city on the player instead of
// leaving placement open.
inline constexpr float kRecenterHoldSeconds = 1.0f;

class PlacementControls {
public:
    PlacementControls(PlacementSettings initial, PlacementPersistence& persistence);
    bool LoadSaved(); // Call outside active editing; failure preserves defaults.
    // True means consume the entire gamepad sample. Pass measured elapsed
    // seconds, not a presumed frame rate. Motion caps one stalled tick at 0.25s.
    // Discrete actions require a fresh press after release; no held-button repeat.
    bool ConsumeInput(const GamepadState& state, double delta_seconds);
    void Cancel();
    bool Active() const noexcept { return active_; }
    const PlacementSettings& GetSettings() const noexcept { return active_ ? preview_ : committed_; }
    const PlacementSettings& CommittedSettings() const noexcept { return committed_; }
    PlacementEvent TakeEvent() noexcept;
    // Commits and saves a new city offset outside editing (long-press recentre).
    bool SetOffsets(float offset_x_m, float offset_z_m);
    const std::string& LastError() const noexcept { return error_; }
    // Friendly guide text for a head-locked overlay, supplied as UTF-8.
    static const char* GuideText() noexcept;
private:
    PlacementPersistence& persistence_;
    PlacementSettings committed_, preview_;
    bool active_ = false, wait_neutral_ = false, chord_armed_ = true, chord_hold_ = false;
    float chord_hold_seconds_ = 0;
    uint16_t previous_buttons_ = 0;
    PlacementEvent event_ = PlacementEvent::None;
    std::string error_;
};

} // namespace gta2ceiling
