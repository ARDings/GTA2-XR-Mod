// Legacy filename retained for discovery. CPU math only: no GPU/Quest/thermal measurement.
#include <cassert>
#include <cmath>
#include <iostream>
#include "gta2ceiling/xr_transport.h"
using namespace gta2ceiling;

int main() {
    // Sweep orientation, position, IPD and aggregate FOV. Host timing cannot
    // establish a headset frame budget, so no elapsed-time assertion is made.
    for (int sample = 0; sample < 64; ++sample) {
        const float sample_value = static_cast<float>(sample);
        const float angle = sample_value * 6.2831853f / 64.0f;
        XrTrackingFrame frame;
        frame.pose = {0, std::sin(angle * 0.5f), 0, std::cos(angle * 0.5f),
                      std::cos(angle), 1.0f + sample_value * 0.01f, std::sin(angle)};
        frame.ipd_meters = 0.055f + sample_value * 0.0003f;
        const float half_fov = 0.55f + sample_value * 0.004f;
        frame.fov_left = frame.fov_right = {-half_fov, half_fov, half_fov, -half_fov};
        Mat4 left, right, left_projection, right_projection;
        XrTransport::compute_eye_transforms(frame, Eye::Left, left, left_projection);
        XrTransport::compute_eye_transforms(frame, Eye::Right, right, right_projection);
        for (int index = 0; index < 16; ++index) {
            assert(std::isfinite(left.m[index]) && std::isfinite(right.m[index]));
            assert(std::isfinite(left_projection.m[index]));
            assert(left_projection.m[index] == right_projection.m[index]);
        }
        for (int column = 0; column < 3; ++column) {
            float length_squared = 0;
            for (int row = 0; row < 3; ++row) {
                const float component = left.m[row * 4 + column];
                length_squared += component * component;
                assert(component == right.m[row * 4 + column]);
            }
            assert(std::fabs(length_squared - 1.0f) < 0.0001f);
            for (int other = column + 1; other < 3; ++other) {
                float dot = 0;
                for (int row = 0; row < 3; ++row)
                    dot += left.m[row * 4 + column] * left.m[row * 4 + other];
                assert(std::fabs(dot) < 0.0001f);
            }
        }
        assert(std::fabs((left.m[12] - right.m[12]) - frame.ipd_meters) < 0.0001f);
        assert(left_projection.m[0] > 0 && left_projection.m[5] > 0);
    }
    std::cout << "CPU transform invariant checks passed (64 synthetic poses).\n"
                 "No GPU performance, thermal headroom or Quest acceptance measured.\n";
}
