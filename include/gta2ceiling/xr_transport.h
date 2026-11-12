#pragma once
#include "gta2ceiling/types.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace gta2ceiling {
// Local value types, NOT packed network structures. XrAPI 0.6 is ASCII UDP.
// Poses use OpenXR right-handed metres, with -Z forward.
struct XrApiPose {
    float orientation_x = 0, orientation_y = 0, orientation_z = 0, orientation_w = 1;
    float position_x = 0, position_y = 0, position_z = 0;
};
struct XrApiFov {
    float angle_left = -0.78539816f, angle_right = 0.78539816f;
    float angle_up = 0.78539816f, angle_down = -0.78539816f;
};
// API 0.6 reports no session-state or tracking-validity flags. This enum remains
// for a caller with a separate real lifecycle source; default is Unknown.
enum class XrSessionState : uint8_t {
    Unknown = 0, Idle, Ready, Synchronized, Visible, Focused, Stopping, LossPending, Exiting
};
struct XrTrackingFrame {
    XrApiPose pose;
    XrApiFov fov_left, fov_right;
    float ipd_meters = 0.063f;
    float refresh_hz = 0; // zero if absent from an older 0.6 text packet
    uint8_t hmd_sync = 0; // received host value, never a local frame counter
    uint32_t recenter_count = 0;
    std::array<float, 8> controller_axes{};
    std::array<bool, 19> controller_buttons{};
    bool pose_valid = false; // numeric validation, NOT an OpenXR validity flag
    bool fov_is_symmetric_approximation = true;
    bool immersive_screen = false, sbs = false;
    uint64_t local_sample_id = 0;
    std::chrono::steady_clock::time_point received_at{}; // local, not predictedDisplayTime
};
struct XrTransportOptions {
    uint16_t receive_port = 7872, control_port = 7278;
    std::string version_file = "Z:\\tmp\\xr\\version";
    bool write_version_file = true;
    // False is a diagnostic screen path: MODE_VR=2 keeps UDP poses/SBS active.
    // MODE_VR=0 would stop host pose delivery and trip the stale guard.
    bool immersive_projection = true;
    // Startup workaround under investigation: let the host import the first
    // game surface before SBS can create its startup-dialog overlay. A sticky
    // GL error in that path may otherwise poison GPUImage's first bind check.
    // Begins in MODE_VR=2/SBS=0; then selects the requested mode with SBS=1.
    std::chrono::milliseconds projection_warmup{3000};
    // Below the 22-frame sync-ring wrap even at 120 Hz (~183 ms).
    std::chrono::milliseconds stale_after{100};
};
class XrTransport {
public:
    static constexpr int LOCAL_REFERENCE_SPACE_ID = 1;
    XrTransport();
    ~XrTransport();
    XrTransport(const XrTransport&) = delete;
    XrTransport& operator=(const XrTransport&) = delete;

    // Call outside DllMain. Owns a loopback receiver thread, no OpenXR session.
    bool start(std::string& error, const XrTransportOptions& options = {});
    void stop();
    bool running() const;
    std::string last_error() const;
    // Copy ONCE per stereo frame; use the same copy for both eyes and sync pixel.
    bool latest_tracking(XrTrackingFrame& frame) const;
    bool has_valid_pose() const;
    bool consume_recenter();
    static bool parse_packet(std::string_view message, XrTrackingFrame& frame, std::string& error);
    static std::string enable_command(bool immersive_projection = true, bool sbs = true);
    static std::string disable_command();
    // Row-major/row-vector matrices: RH world -> LH camera -> D3D depth [0,1].
    // Parallel eyes, offsets rotated by head quaternion. No toe-in.
    static void compute_eye_transforms(const XrTrackingFrame& frame, Eye eye, Mat4& view, Mat4& projection);
    void compute_eye_transforms(Eye eye, const CeilingConfig& config, Mat4& view, Mat4& projection) const;
    // D3DCOLOR ARGB at pixel (0,0): exact sRGB red byte, G=B=0, A=255.
    // Draw LAST with sRGB-write, blending, dithering and filtering disabled.
    static uint32_t encode_sync_pixel(uint8_t hmd_sync);
    // Source compatibility only. These booleans are NOT encoded on the wire.
    static uint32_t encode_sync_pixel(uint8_t hmd_sync, bool, bool) { return encode_sync_pixel(hmd_sync); }
    // Injection seam for synthetic tests/separately validated pose sources.
    void update_hmd_pose(const XrApiPose& pose, float ipd, const XrApiFov& left, const XrApiFov& right);
    void set_session_state(XrSessionState state);
    XrSessionState session_state() const;
    bool is_focused() const;
private:
    struct NetworkState;
    void receive_loop();
    void accept_tracking(XrTrackingFrame frame);
    void record_error(const std::string& error);
    std::unique_ptr<NetworkState> network_;
    mutable std::mutex mutex_;
    XrTrackingFrame latest_;
    XrSessionState session_state_ = XrSessionState::Unknown;
    std::chrono::milliseconds stale_after_{100};
    bool have_packet_ = false, recenter_pending_ = false;
    std::string last_error_;
};
} // namespace gta2ceiling
