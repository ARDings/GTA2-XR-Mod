#include <iostream>
#include <cassert>

#include "gta2ceiling/types.h"
#include "gta2ceiling/xr_transport.h"

using namespace gta2ceiling;

// Test-local policy: no passthrough layer, anchor API, scan, GPU clear or storage.
class PassthroughAnchorPolicyModel {
public:
    PassthroughAnchorPolicyModel() = default;

    void enable_passthrough(bool enable) {
        m_passthrough_enabled = enable;
    }

    uint32_t get_clear_color() const {
        // Return proposed color values; no framebuffer is cleared.
        return m_passthrough_enabled ? 0x00000000 : 0xFF111115;
    }

    bool is_anchor_valid() const { return m_anchor_valid; }
    void set_anchor_valid(bool valid) { m_anchor_valid = valid; }

    Vec3 get_anchor_origin() const {
        if (!m_anchor_valid) {
            // Proposed fallback coordinate; safety/comfort is not measured.
            return Vec3(0.0f, 2.1f, 0.0f);
        }
        return m_anchor_origin;
    }

private:
    bool m_passthrough_enabled = false;
    bool m_anchor_valid = true;
    Vec3 m_anchor_origin{0.0f, 2.2f, 0.1f};
};

void run_passthrough_test() {
    std::cout << "=========================================================\n";
    std::cout << "  CPU-only color and anchor-position policy model      \n";
    std::cout << "=========================================================\n";

    PassthroughAnchorPolicyModel mgr;

    // 1. Virtual Room Mode
    assert(mgr.get_clear_color() == 0xFF111115);
    std::cout << "[1/3] Model returns the opaque default color value.\n";

    // 2. Passthrough Mode (Alpha blend)
    mgr.enable_passthrough(true);
    assert(mgr.get_clear_color() == 0x00000000);
    std::cout << "[2/3] Model returns a zero-alpha color value.\n";

    // 3. Anchor Loss Fallback
    mgr.set_anchor_valid(false);
    Vec3 fallback = mgr.get_anchor_origin();
    assert(fallback.y == 2.1f);
    std::cout << "[3/3] Injected invalid-anchor flag selects the fallback coordinate.\n";

    std::cout << "=========================================================\n";
    std::cout << "  Color/position policy-model assertions passed.        \n";
    std::cout << "  No passthrough, real anchor or Quest acceptance.      \n";
    std::cout << "=========================================================\n";
}

int main() {
    run_passthrough_test();
    return 0;
}
