#include <iostream>
#include <cassert>

#include "gta2ceiling/types.h"
#include "gta2ceiling/xr_transport.h"
#include "gta2ceiling/snapshot.h"

using namespace gta2ceiling;

// Test-local policy: no production pause/audio hooks or OpenXR events exercised.
// XrAPI 0.6 does not provide authoritative focus flags.
class LifecyclePolicyModel {
public:
    LifecyclePolicyModel() = default;

    void update_session(XrSessionState state, bool tracking_valid) {
        m_state = state;
        m_tracking_valid = tracking_valid;

        // Set model flags for the proposed unfocused policy.
        if (m_state != XrSessionState::Focused) {
            m_game_paused = true;
            m_audio_muted = true;
        } else {
            // Audio flag clears; pause remains until explicit unpause().
            m_audio_muted = false;
        }

        // Set model flags for an injected tracking loss.
        if (!m_tracking_valid) {
            m_camera_frozen = true;
            m_fade_to_dim = true;
        } else {
            m_camera_frozen = false;
            m_fade_to_dim = false;
        }
    }

    bool is_game_paused() const { return m_game_paused; }
    bool is_audio_muted() const { return m_audio_muted; }
    bool is_camera_frozen() const { return m_camera_frozen; }
    bool is_dimmed() const { return m_fade_to_dim; }

    void unpause() { m_game_paused = false; }

private:
    XrSessionState m_state = XrSessionState::Focused;
    bool m_tracking_valid = true;
    bool m_game_paused = false;
    bool m_audio_muted = false;
    bool m_camera_frozen = false;
    bool m_fade_to_dim = false;
};

void run_lifecycle_test() {
    std::cout << "=========================================================\n";
    std::cout << "  CPU-only lifecycle policy model                     \n";
    std::cout << "=========================================================\n";

    LifecyclePolicyModel lm;

    // 1. Normal Running State
    std::cout << "[1/4] Injected focused/valid model state...\n";
    lm.update_session(XrSessionState::Focused, true);
    assert(!lm.is_game_paused() && !lm.is_audio_muted() && !lm.is_camera_frozen());
    std::cout << "      -> Model flags are clear.\n";

    // 2. Tracking Loss (Headset obscured or dark room)
    std::cout << "[2/4] Injected invalid tracking flag...\n";
    lm.update_session(XrSessionState::Focused, false);
    // Checks booleans, not a displayed fade or a comfort outcome.
    assert(lm.is_camera_frozen() && lm.is_dimmed());
    std::cout << "      -> Model freeze/dim flags set.\n";

    // 3. System Overlay / Oculus Dashboard (Visible but not Focused)
    std::cout << "[3/4] Injected visible/unfocused model state...\n";
    lm.update_session(XrSessionState::Visible, true);
    // No game is paused and no audio device is accessed by this test.
    assert(lm.is_game_paused() && lm.is_audio_muted());
    std::cout << "      -> Model pause/mute flags set.\n";

    // 4. Headset Sleep / Standby (Stopping state) & Resume
    std::cout << "[4/4] Injected stopping/focused model sequence...\n";
    lm.update_session(XrSessionState::Stopping, false);
    assert(lm.is_game_paused() && lm.is_audio_muted() && lm.is_camera_frozen());

    // Wakeup & resume
    lm.update_session(XrSessionState::Focused, true);
    lm.unpause();
    assert(!lm.is_game_paused() && !lm.is_audio_muted() && !lm.is_camera_frozen());
    std::cout << "      -> Model reset after injected focus and explicit unpause.\n";

    std::cout << "=========================================================\n";
    std::cout << "  Lifecycle policy-model assertions passed.            \n";
    std::cout << "  No headset lifecycle or production hook verified.    \n";
    std::cout << "=========================================================\n";
}

int main() {
    run_lifecycle_test();
    return 0;
}
