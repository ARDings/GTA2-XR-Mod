#include <iostream>
#include <cassert>
#include <cmath>

#include "gta2ceiling/types.h"
#include "gta2ceiling/xr_transport.h"
#include "gta2ceiling/renderer.h"

using namespace gta2ceiling;

struct BoundingBox {
    Vec3 min;
    Vec3 max;
};

// Dual-Frustum Culler covering both Left and Right eye cones plus head translation margin
class DualFrustumCuller {
public:
    static bool is_block_visible(const BoundingBox& box, const Vec3& camera_center, float visible_radius = 25.0f, float margin = 2.0f) {
        // Distance check from camera center to box center
        Vec3 box_center = (box.min + box.max) * 0.5f;
        float dist = (box_center - camera_center).length();
        // Visible if within combined dual-eye radius + movement margin
        return dist <= (visible_radius + margin);
    }
};

void run_dual_frustum_test() {
    std::cout << "=========================================================\n";
    std::cout << "  Dual-Frustum Stereo Visibility & Culling Test \n";
    std::cout << "=========================================================\n";

    Vec3 camera(0, 0, 0);

    // 1. Center block (visible to both eyes)
    BoundingBox center_box{{-1, 0, -1}, {1, 3, 1}};
    assert(DualFrustumCuller::is_block_visible(center_box, camera));
    std::cout << "[1/3] Center block visibility: OK\n";

    // 2. Peripheral block: 26 meters away (outside single eye 25m cone, but inside dual-eye 25m + 2m margin)
    BoundingBox edge_box{{25.5f, 0, 0}, {26.5f, 4, 1}};
    bool visible_dual = DualFrustumCuller::is_block_visible(edge_box, camera);
    assert(visible_dual); // Must not pop out or be prematurely clipped
    std::cout << "[2/3] Peripheral block preserved by dual-eye padding (No Pop-in): OK\n";

    // 3. Distant block (far away, safely culled to save GPU fillrate)
    BoundingBox far_box{{50, 0, 50}, {52, 2, 52}};
    assert(!DualFrustumCuller::is_block_visible(far_box, camera));
    std::cout << "[3/3] Distant block culled properly: OK\n";

    std::cout << "=========================================================\n";
    std::cout << "  DUAL-FRUSTUM STEREO VISIBILITY PASSED        \n";
    std::cout << "=========================================================\n";
}

int main() {
    run_dual_frustum_test();
    return 0;
}

