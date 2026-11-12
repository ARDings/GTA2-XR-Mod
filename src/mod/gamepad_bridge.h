#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace gta2ceiling {

// Physical, set-1 keyboard scan codes. The E0 prefix is stored separately.
// A zero scan code disables a binding. These are independent of keyboard layout.
struct KeyboardBinding {
    uint8_t scan_code = 0;
    bool extended = false;
    bool operator==(const KeyboardBinding&) const = default;
};

enum class PadControl : size_t {
    Up, Down, Left, Right, A, B, X, Y, LeftShoulder, RightShoulder,
    Back, Start, LeftThumb, RightThumb, LeftTrigger, RightTrigger, Count
};

struct GamepadState {
    // Button bits use the documented XInput button-mask values.
    uint16_t buttons = 0;
    int16_t left_x = 0, left_y = 0;
    uint8_t left_trigger = 0, right_trigger = 0;
    int16_t right_x = 0, right_y = 0;
    // Set false by the bridge for focus loss/disconnect/shutdown. Such samples
    // have neutral axes/buttons and let modal controls cancel without saving.
    bool available = true;
};

struct ControllerConfig {
    bool enabled = true;
    int stick_deadzone = 7849;
    int trigger_threshold = 30;
    std::array<KeyboardBinding, static_cast<size_t>(PadControl::Count)> bindings{};
    static ControllerConfig Defaults();
};

struct KeyboardEvent {
    KeyboardBinding key;
    bool down = false;
};

class GamepadInput {
public:
    virtual ~GamepadInput() = default;
    virtual bool IsForeground() const = 0;
    // False means no connected controller. Called only while foreground.
    virtual bool Read(GamepadState& state) = 0;
};

class KeyboardSink {
public:
    virtual ~KeyboardSink() = default;
    // True means this single event was accepted, not just queued by the caller.
    virtual bool Send(const KeyboardEvent& event) = 0;
};

// The pure mapping is also used by the production bridge. Multiple controls bound
// to the same key produce one hold; opposing directional inputs cancel out.
using DesiredKeyboardState = std::array<bool, 512>;
DesiredKeyboardState MapGamepad(const GamepadState&, const ControllerConfig&);
bool ParseKeyboardBinding(const std::string& name, KeyboardBinding& result);

// All methods run on the original game thread, outside DllMain. The default
// implementation loads XInput dynamically and sends keyboard scan-code events.
// The injectable constructor executes exactly the same mapping/transition logic.
class GamepadBridge {
public:
    using InputFilter = bool (*)(const GamepadState&, void* context);
    GamepadBridge();
    GamepadBridge(GamepadInput& input, KeyboardSink& sink,
                  ControllerConfig config = ControllerConfig::Defaults());
    ~GamepadBridge();
    GamepadBridge(const GamepadBridge&) = delete;
    GamepadBridge& operator=(const GamepadBridge&) = delete;

    // May reopen after Shutdown, but only once every pending release succeeds.
    bool Initialize(const char* ini_path = "gta2ceiling.ini");
    void Tick();
    // True consumes the sample and releases mapped game keys. Called on the
    // game thread, including neutral unavailable samples. Context must outlive
    // the bridge (or be detached with SetFilter(nullptr) before destruction).
    void SetFilter(InputFilter filter, void* context = nullptr);
    // Called on the game thread for every sample the filter did not consume,
    // before it is mapped to keys: may rewrite it (see modern_controls.h).
    using InputRemap = void (*)(GamepadState&, void* context);
    void SetRemap(InputRemap remap, void* context = nullptr);
    int StickDeadzone() const;
    int TriggerThreshold() const;
    // Stops new presses. Failed key-up events remain pending; subsequent Tick()
    // or Shutdown() retries them. False means the OS has not accepted all releases.
    bool Shutdown();
    size_t PendingKeys() const;
    const std::string& LastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gta2ceiling
