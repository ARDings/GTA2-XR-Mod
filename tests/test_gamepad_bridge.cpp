#include "../src/mod/gamepad_bridge.h"

#include <cassert>
#include <iostream>
#include <vector>

using namespace gta2ceiling;
namespace {
constexpr uint16_t A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;
constexpr uint16_t Start = 0x0010, Back = 0x0020, LB = 0x0100, RB = 0x0200;
KeyboardBinding Key(const char* name) {
    KeyboardBinding result;
    assert(ParseKeyboardBinding(name, result));
    return result;
}
bool IsHeld(const DesiredKeyboardState& keys, const char* name) {
    const auto key = Key(name);
    return keys[key.scan_code | (key.extended ? 256u : 0u)];
}
struct FakeInput final : GamepadInput {
    bool foreground = true, connected = true, lose_focus_during_read = false;
    int reads = 0;
    GamepadState state;
    bool IsForeground() const override { return foreground; }
    bool Read(GamepadState& result) override {
        assert(foreground); // Production logic must never poll in the background.
        ++reads;
        if (lose_focus_during_read) foreground = false;
        result = state;
        return connected;
    }
};
struct FakeSink final : KeyboardSink {
    bool reject_press = false, reject_release = false;
    std::vector<KeyboardEvent> attempts, accepted;
    bool Send(const KeyboardEvent& event) override {
        attempts.push_back(event);
        if ((event.down && reject_press) || (!event.down && reject_release)) return false;
        accepted.push_back(event);
        return true;
    }
    size_t Count(const char* name, bool down) const {
        size_t count = 0;
        for (const auto& event : accepted) if (event.key == Key(name) && event.down == down) ++count;
        return count;
    }
};

void TestMapping() {
    const auto config = ControllerConfig::Defaults();
    GamepadState sample{};
    sample.buttons = A | B | X | Y | Start | Back | LB | RB;
    sample.left_trigger = sample.right_trigger = 255;
    const auto keys = MapGamepad(sample, config);
    for (const char* name : {"ENTER", "ESC", "SPACE", "TAB", "F6", "Z", "X", "LALT", "LCTRL"}) {
        assert(IsHeld(keys, name));
    }
    assert(Key("UP").extended && !Key("LCTRL").extended);
    assert(Key("RCTRL").extended && Key("RALT").extended);
    KeyboardBinding binding{42, true};
    assert(ParseKeyboardBinding("  none \t", binding) && binding.scan_code == 0);
    assert(ParseKeyboardBinding(" enter ", binding) && binding == Key("ENTER"));
    assert(!ParseKeyboardBinding("typo", binding) && binding == Key("ENTER"));

    sample = {};
    sample.left_x = 7849; sample.left_y = -7849;
    sample.left_trigger = sample.right_trigger = 30;
    const auto threshold = MapGamepad(sample, config);
    for (bool held : threshold) assert(!held);
    sample.left_x = 7850; sample.left_y = -7850;
    sample.left_trigger = sample.right_trigger = 31;
    const auto beyond = MapGamepad(sample, config);
    for (const char* name : {"RIGHT", "DOWN", "LALT", "LCTRL"}) assert(IsHeld(beyond, name));
    sample.left_x = -32768; sample.left_y = 32767;
    const auto extremes = MapGamepad(sample, config);
    assert(IsHeld(extremes, "LEFT") && IsHeld(extremes, "UP"));
    sample.buttons = 0x0002 | 0x0008; // D-pad opposes the left/up stick.
    const auto opposed = MapGamepad(sample, config);
    for (const char* name : {"UP", "DOWN", "LEFT", "RIGHT"}) assert(!IsHeld(opposed, name));

    auto custom = config;
    custom.bindings[static_cast<size_t>(PadControl::A)] = Key("F1");
    custom.bindings[static_cast<size_t>(PadControl::RightTrigger)] = {};
    sample = {}; sample.buttons = A; sample.right_trigger = 255;
    const auto remapped = MapGamepad(sample, custom);
    assert(IsHeld(remapped, "F1") && !IsHeld(remapped, "ENTER") && !IsHeld(remapped, "LCTRL"));
    custom.enabled = false;
    for (bool held : MapGamepad(sample, custom)) assert(!held);
}

void TestTransitionsFocusAndDisconnect() {
    FakeInput input; FakeSink sink;
    GamepadBridge bridge(input, sink);
    input.state.buttons = A | 0x0001;
    bridge.Tick();
    assert(bridge.PendingKeys() == 2 && sink.accepted.size() == 2);
    bridge.Tick(); // Held buttons must not repeat Return on every graphics tick.
    assert(sink.accepted.size() == 2);
    input.foreground = false;
    const int reads = input.reads;
    bridge.Tick(); bridge.Tick();
    assert(input.reads == reads && bridge.PendingKeys() == 0);
    assert(sink.Count("ENTER", false) == 1 && sink.Count("UP", false) == 1);
    input.foreground = true;
    bridge.Tick();
    assert(bridge.PendingKeys() == 2);
    input.connected = false;
    bridge.Tick();
    assert(bridge.PendingKeys() == 0 && sink.Count("ENTER", false) == 2);
    input.connected = true; input.lose_focus_during_read = true;
    const size_t before = sink.accepted.size();
    bridge.Tick();
    assert(bridge.PendingKeys() == 0 && sink.accepted.size() == before);
}

void TestSharedBinding() {
    FakeInput input; FakeSink sink;
    GamepadBridge bridge(input, sink);
    input.state.buttons = B | Start; // Both map to Escape.
    bridge.Tick();
    assert(bridge.PendingKeys() == 1 && sink.Count("ESC", true) == 1);
    input.state.buttons = Start;
    bridge.Tick();
    assert(bridge.PendingKeys() == 1 && sink.Count("ESC", false) == 0);
    input.state.buttons = 0;
    bridge.Tick();
    assert(bridge.PendingKeys() == 0 && sink.Count("ESC", false) == 1);
}

void TestRetryAndShutdown() {
    FakeInput input; FakeSink sink;
    GamepadBridge bridge(input, sink);
    input.state.buttons = A;
    sink.reject_press = true;
    bridge.Tick();
    assert(bridge.PendingKeys() == 0 && sink.accepted.empty());
    sink.reject_press = false;
    bridge.Tick();
    assert(bridge.PendingKeys() == 1 && sink.Count("ENTER", true) == 1);
    sink.reject_release = true;
    input.state.buttons = X;
    bridge.Tick();
    assert(bridge.PendingKeys() == 1 && sink.Count("SPACE", true) == 0);
    input.foreground = false;
    bridge.Tick();
    assert(bridge.PendingKeys() == 1); // Failed release must not lose ownership.
    sink.reject_release = false;
    const int reads = input.reads;
    bridge.Tick();
    assert(input.reads == reads && bridge.PendingKeys() == 0);
    input.foreground = true;
    bridge.Tick();
    assert(bridge.PendingKeys() == 1 && sink.Count("SPACE", true) == 1);
    sink.reject_release = true;
    assert(!bridge.Shutdown() && bridge.PendingKeys() == 1);
    assert(!bridge.Initialize() && bridge.PendingKeys() == 1);
    const int reads_after_shutdown = input.reads;
    bridge.Tick();
    assert(bridge.PendingKeys() == 1 && input.reads == reads_after_shutdown);
    sink.reject_release = false;
    bridge.Tick();
    assert(bridge.PendingKeys() == 0 && input.reads == reads_after_shutdown);
    assert(bridge.Shutdown());
    bridge.Tick();
    assert(sink.Count("SPACE", true) == 1); // No new presses after shutdown.
    assert(bridge.Initialize()); // Reopen after gbh_CloseScreen / a new game screen.
    bridge.Tick();
    assert(bridge.PendingKeys() == 1 && sink.Count("SPACE", true) == 2);
    assert(bridge.Shutdown() && bridge.PendingKeys() == 0);
}

void TestFailedPressReleasedBeforeRetry() {
    FakeInput input; FakeSink sink;
    GamepadBridge bridge(input, sink);
    input.state.buttons = A;
    sink.reject_press = true;
    bridge.Tick();
    input.state.buttons = 0;
    sink.reject_press = false;
    bridge.Tick();
    assert(sink.accepted.empty() && bridge.PendingKeys() == 0);
    assert(bridge.Shutdown()); // Never release a key this bridge did not press.
    assert(sink.accepted.empty());
}
} // namespace

int main() {
    TestMapping();
    TestTransitionsFocusAndDisconnect();
    TestSharedBinding();
    TestRetryAndShutdown();
    TestFailedPressReleasedBeforeRetry();
    std::cout << "Gamepad bridge mapping/transition tests passed (injected state/sink; no controller hardware).\n";
}
