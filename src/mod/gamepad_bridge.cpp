#include "gamepad_bridge.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <utility>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <Xinput.h>
#endif

namespace gta2ceiling {
namespace {
constexpr size_t Index(PadControl control) { return static_cast<size_t>(control); }
constexpr size_t KeyIndex(KeyboardBinding key) {
    return key.scan_code | (key.extended ? 256u : 0u);
}
KeyboardBinding KeyAt(size_t index) {
    return {static_cast<uint8_t>(index & 255), (index & 256) != 0};
}
const char* const binding_names[] = {
    "up", "down", "left", "right", "a", "b", "x", "y", "lb", "rb",
    "back", "start", "left_thumb", "right_thumb", "lt", "rt"
};
static_assert(std::size(binding_names) == Index(PadControl::Count));

ControllerConfig Normalize(ControllerConfig config) {
    config.stick_deadzone = std::clamp(config.stick_deadzone, 0, 32766);
    config.trigger_threshold = std::clamp(config.trigger_threshold, 0, 254);
    return config;
}

#ifdef _WIN32
class WindowsGamepad final : public GamepadInput {
public:
    ~WindowsGamepad() override { if (module_) FreeLibrary(module_); }
    bool Load() {
        if (get_state_) return true;
        const auto now = std::chrono::steady_clock::now();
        if (now < retry_at_) return false;
        retry_at_ = now + std::chrono::seconds(2);
        // Load only the runtime's system DLLs, never an unrelated game-folder DLL.
        const wchar_t* libraries[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
        wchar_t system_path[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(system_path, MAX_PATH);
        if (!length || length >= MAX_PATH) return false;
        for (const auto* library : libraries) {
            std::wstring path(system_path);
            path += L"\\";
            path += library;
            HMODULE candidate = LoadLibraryW(path.c_str());
            if (!candidate) continue;
            auto function = reinterpret_cast<GetStateFn>(GetProcAddress(candidate, "XInputGetState"));
            if (function) {
                module_ = candidate;
                get_state_ = function;
                return true;
            }
            FreeLibrary(candidate);
        }
        return false;
    }
    bool IsForeground() const override {
        DWORD process = 0;
        const HWND foreground = GetForegroundWindow();
        return foreground && GetWindowThreadProcessId(foreground, &process) != 0 &&
               process == GetCurrentProcessId();
    }
    bool Read(GamepadState& state) override {
        if (!Load()) return false;
        // Keep one device until it disconnects, then search for the first device.
        XINPUT_STATE native{};
        if (selected_ < XUSER_MAX_COUNT && get_state_(selected_, &native) == ERROR_SUCCESS) {
            Copy(native.Gamepad, state);
            return true;
        }
        for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user) {
            if (user == selected_) continue;
            if (get_state_(user, &native) == ERROR_SUCCESS) {
                selected_ = user;
                Copy(native.Gamepad, state);
                return true;
            }
        }
        selected_ = XUSER_MAX_COUNT;
        return false;
    }
private:
    static void Copy(const XINPUT_GAMEPAD& native, GamepadState& state) {
        state = {native.wButtons, native.sThumbLX, native.sThumbLY,
                 native.bLeftTrigger, native.bRightTrigger,
                 native.sThumbRX, native.sThumbRY, true};
    }
    using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    HMODULE module_ = nullptr;
    GetStateFn get_state_ = nullptr;
    DWORD selected_ = XUSER_MAX_COUNT;
    std::chrono::steady_clock::time_point retry_at_{};
};

class WindowsKeyboard final : public KeyboardSink {
public:
    bool Send(const KeyboardEvent& event) override {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = event.key.scan_code;
        input.ki.dwFlags = KEYEVENTF_SCANCODE |
            (event.key.extended ? KEYEVENTF_EXTENDEDKEY : 0) |
            (event.down ? 0 : KEYEVENTF_KEYUP);
        return SendInput(1, &input, sizeof(input)) == 1;
    }
};

ControllerConfig ReadConfig(const char* ini_path, std::string& error) {
    auto config = ControllerConfig::Defaults();
    char absolute[MAX_PATH]{};
    const DWORD size = GetFullPathNameA(ini_path, MAX_PATH, absolute, nullptr);
    if (!size || size >= MAX_PATH) {
        error = "Controller INI path is invalid; using default bindings.";
        return config;
    }
    config.enabled = GetPrivateProfileIntA("controller", "enabled", 1, absolute) != 0;
    config.stick_deadzone = static_cast<int>(GetPrivateProfileIntA("controller", "stick_deadzone", 7849, absolute));
    config.trigger_threshold = static_cast<int>(GetPrivateProfileIntA("controller", "trigger_threshold", 30, absolute));
    for (size_t i = 0; i < Index(PadControl::Count); ++i) {
        char value[64]{};
        GetPrivateProfileStringA("controller", binding_names[i], "", value, sizeof(value), absolute);
        if (!value[0]) continue;
        KeyboardBinding binding;
        if (ParseKeyboardBinding(value, binding)) config.bindings[i] = binding;
        else {
            if (!error.empty()) error += " ";
            error += std::string("Unknown controller binding '") + binding_names[i] + "'; using its default.";
        }
    }
    return Normalize(config);
}
#endif
} // namespace

ControllerConfig ControllerConfig::Defaults() {
    ControllerConfig result;
    // Original Rockstar GTA2 PC manual: arrows, Return, Ctrl, Space, Tab, X/Z,
    // F6 pause; Esc abandons/exits play. LAlt is an explicit optional key binding.
    // https://gtamp.com/GTA2/gta2manual.pdf (Controls, pages 4-5).
    result.bindings = {{{0x48, true}, {0x50, true}, {0x4b, true}, {0x4d, true},
                        {0x1c, false}, {0x01, false}, {0x39, false}, {0x0f, false},
                        {0x2c, false}, {0x2d, false}, {0x40, false}, {0x01, false},
                        {}, {}, {0x38, false}, {0x1d, false}}};
    return result;
}

bool ParseKeyboardBinding(const std::string& name, KeyboardBinding& result) {
    const auto first = name.find_first_not_of(" \t\r\n");
    const auto last = name.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) return false;
    std::string token = name.substr(first, last - first + 1);
    for (auto& c : token) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    struct NamedKey { const char* name; uint8_t scan; bool extended; };
    const NamedKey keys[] = {
        {"NONE", 0, false}, {"DISABLED", 0, false},
        {"UP", 0x48, true}, {"DOWN", 0x50, true}, {"LEFT", 0x4b, true}, {"RIGHT", 0x4d, true},
        {"ENTER", 0x1c, false}, {"RETURN", 0x1c, false}, {"ESC", 1, false}, {"ESCAPE", 1, false},
        {"SPACE", 0x39, false}, {"TAB", 0x0f, false},
        {"LCTRL", 0x1d, false}, {"CTRL", 0x1d, false}, {"RCTRL", 0x1d, true},
        {"LALT", 0x38, false}, {"ALT", 0x38, false}, {"RALT", 0x38, true},
        {"LSHIFT", 0x2a, false}, {"SHIFT", 0x2a, false}, {"RSHIFT", 0x36, false},
        {"A", 0x1e, false}, {"B", 0x30, false}, {"C", 0x2e, false}, {"D", 0x20, false},
        {"E", 0x12, false}, {"F", 0x21, false}, {"G", 0x22, false}, {"H", 0x23, false},
        {"I", 0x17, false}, {"J", 0x24, false}, {"K", 0x25, false}, {"L", 0x26, false},
        {"M", 0x32, false}, {"N", 0x31, false}, {"O", 0x18, false}, {"P", 0x19, false},
        {"Q", 0x10, false}, {"R", 0x13, false}, {"S", 0x1f, false}, {"T", 0x14, false},
        {"U", 0x16, false}, {"V", 0x2f, false}, {"W", 0x11, false}, {"X", 0x2d, false},
        {"Y", 0x15, false}, {"Z", 0x2c, false},
        {"1", 0x02, false}, {"2", 0x03, false}, {"3", 0x04, false}, {"4", 0x05, false},
        {"5", 0x06, false}, {"6", 0x07, false}, {"7", 0x08, false}, {"8", 0x09, false},
        {"9", 0x0a, false}, {"0", 0x0b, false},
        {"F1", 0x3b, false}, {"F2", 0x3c, false}, {"F3", 0x3d, false}, {"F4", 0x3e, false},
        {"F5", 0x3f, false}, {"F6", 0x40, false}, {"F7", 0x41, false}, {"F8", 0x42, false},
        {"F9", 0x43, false}, {"F10", 0x44, false}, {"F11", 0x57, false}, {"F12", 0x58, false}
    };
    for (const auto& key : keys) {
        if (token == key.name) { result = {key.scan, key.extended}; return true; }
    }
    return false;
}

DesiredKeyboardState MapGamepad(const GamepadState& state, const ControllerConfig& settings) {
    DesiredKeyboardState desired{};
    const auto config = Normalize(settings);
    if (!config.enabled) return desired;
    const bool up = (state.buttons & 0x0001) || state.left_y > config.stick_deadzone;
    const bool down = (state.buttons & 0x0002) || state.left_y < -config.stick_deadzone;
    const bool left = (state.buttons & 0x0004) || state.left_x < -config.stick_deadzone;
    const bool right = (state.buttons & 0x0008) || state.left_x > config.stick_deadzone;
    const auto hold = [&](PadControl control, bool pressed) {
        const auto binding = config.bindings[Index(control)];
        if (pressed && binding.scan_code) desired[KeyIndex(binding)] = true;
    };
    hold(PadControl::Up, up && !down); hold(PadControl::Down, down && !up);
    hold(PadControl::Left, left && !right); hold(PadControl::Right, right && !left);
    struct ButtonMask { PadControl control; uint16_t mask; };
    const ButtonMask buttons[] = {
        {PadControl::A, 0x1000}, {PadControl::B, 0x2000}, {PadControl::X, 0x4000}, {PadControl::Y, 0x8000},
        {PadControl::LeftShoulder, 0x0100}, {PadControl::RightShoulder, 0x0200},
        {PadControl::Back, 0x0020}, {PadControl::Start, 0x0010},
        {PadControl::LeftThumb, 0x0040}, {PadControl::RightThumb, 0x0080}
    };
    for (const auto& button : buttons) hold(button.control, (state.buttons & button.mask) != 0);
    hold(PadControl::LeftTrigger, state.left_trigger > config.trigger_threshold);
    hold(PadControl::RightTrigger, state.right_trigger > config.trigger_threshold);
    return desired;
}

struct GamepadBridge::Impl {
    ControllerConfig config = ControllerConfig::Defaults();
    GamepadInput* input = nullptr;
    KeyboardSink* sink = nullptr;
    std::unique_ptr<GamepadInput> owned_input;
    std::unique_ptr<KeyboardSink> owned_sink;
    DesiredKeyboardState held{};
    bool initialized = false, stopping = false, injected = false;
    GamepadBridge::InputFilter filter = nullptr;
    void* filter_context = nullptr;
    GamepadBridge::InputRemap remap = nullptr;
    void* remap_context = nullptr;
    std::string error;

    void Apply(const DesiredKeyboardState& desired) {
        bool released = true;
        // Track only accepted events. Failed key-ups retain ownership for retry,
        // even if focus/controller availability changes before the next tick.
        for (size_t i = 1; i < held.size(); ++i) {
            if (held[i] && !desired[i]) {
                if (sink && sink->Send({KeyAt(i), false})) held[i] = false;
                else released = false;
            }
        }
        if (!released) { error = "Keyboard release rejected; retrying on the next game tick."; return; }
        for (size_t i = 1; i < held.size(); ++i) {
            if (desired[i] && !held[i]) {
                // Check again immediately before presses: focus may have changed
                // while reading XInput. Releases are allowed after focus loss.
                if (!input || !input->IsForeground()) return;
                if (sink && sink->Send({KeyAt(i), true})) held[i] = true;
                else error = "Keyboard press rejected; retrying while the control remains held.";
            }
        }
    }
};

GamepadBridge::GamepadBridge() : impl_(std::make_unique<Impl>()) {}
GamepadBridge::GamepadBridge(GamepadInput& input, KeyboardSink& sink, ControllerConfig config)
    : impl_(std::make_unique<Impl>()) {
    impl_->input = &input; impl_->sink = &sink; impl_->config = Normalize(config);
    impl_->initialized = true; impl_->injected = true;
}
GamepadBridge::~GamepadBridge() { Shutdown(); }

bool GamepadBridge::Initialize(const char* ini_path) {
    // Reinitialization cannot discard outstanding key releases.
    if (impl_->initialized && !impl_->stopping) return true;
    impl_->Apply({});
    if (PendingKeys() != 0) return false;
    impl_->stopping = false;
    impl_->error.clear();
    if (impl_->injected) { impl_->initialized = true; return true; }
#ifdef _WIN32
    impl_->config = ReadConfig(ini_path ? ini_path : "gta2ceiling.ini", impl_->error);
    auto input = std::make_unique<WindowsGamepad>();
    const bool loaded = !impl_->config.enabled || input->Load();
    impl_->input = input.get(); impl_->owned_input = std::move(input);
    impl_->owned_sink = std::make_unique<WindowsKeyboard>(); impl_->sink = impl_->owned_sink.get();
    impl_->initialized = true;
    if (!loaded) impl_->error = "XInput runtime unavailable; retrying while the game has focus.";
    return loaded;
#else
    (void)ini_path;
    impl_->error = "The production gamepad bridge requires Windows/Wine.";
    return false;
#endif
}

void GamepadBridge::Tick() {
    if (!impl_->initialized) return;
    DesiredKeyboardState desired{};
    GamepadState state{};
    state.available = false;
    if (!impl_->stopping && impl_->config.enabled && impl_->input && impl_->input->IsForeground()) {
        GamepadState read{};
        if (impl_->input->Read(read) && impl_->input->IsForeground()) {
            state = read;
            state.available = true;
        }
    }
    const bool consumed = impl_->filter && impl_->filter(state, impl_->filter_context);
    if (state.available && !consumed) {
        if (impl_->remap) impl_->remap(state, impl_->remap_context);
        desired = MapGamepad(state, impl_->config);
    }
    impl_->Apply(desired);
}

void GamepadBridge::SetRemap(InputRemap remap, void* context) {
    impl_->remap = remap;
    impl_->remap_context = context;
}
int GamepadBridge::StickDeadzone() const { return impl_->config.stick_deadzone; }
int GamepadBridge::TriggerThreshold() const { return impl_->config.trigger_threshold; }

void GamepadBridge::SetFilter(InputFilter filter, void* context) {
    impl_->filter = filter;
    impl_->filter_context = context;
}

bool GamepadBridge::Shutdown() {
    impl_->stopping = true;
    if (impl_->filter) {
        GamepadState state{};
        state.available = false;
        impl_->filter(state, impl_->filter_context);
    }
    impl_->Apply({});
    return PendingKeys() == 0;
}
size_t GamepadBridge::PendingKeys() const {
    return static_cast<size_t>(std::count(impl_->held.begin(), impl_->held.end(), true));
}
const std::string& GamepadBridge::LastError() const { return impl_->error; }
} // namespace gta2ceiling
