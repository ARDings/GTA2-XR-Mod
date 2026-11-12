// Original implementation of the public WinlatorXR XrAPI 0.6 text protocol.
// Protocol references: https://winlatorxr.github.io/xrapi.html and the 0.6
// encoder at WinlatorXR commit 39b270c6401a69e7c28ea6ffd61de3dbac9e9b1e.
// No WinlatorXR source is incorporated into this implementation.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif
#include "gta2ceiling/xr_transport.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <locale>
#include <sstream>
#include <thread>
#include <vector>

namespace gta2ceiling {
namespace {
constexpr float PI = 3.14159265358979323846f;
bool number(const std::string& token, float& result) {
    std::istringstream in(token);
    in.imbue(std::locale::classic());
    in >> result;
    return in && in.peek() == std::char_traits<char>::eof() && std::isfinite(result);
}
bool integer(const std::string& token, uint32_t& value, uint32_t maximum) {
    if (token.empty() || token.find_first_not_of("0123456789") != std::string::npos) return false;
    uint64_t parsed = 0;
    for (char c : token) {
        parsed = parsed * 10 + static_cast<unsigned>(c - '0');
        if (parsed > maximum) return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}
bool bools(const std::string& token, size_t count) {
    return token.size() == count && token.find_first_not_of("TF") == std::string::npos;
}
bool normalize_pose(XrApiPose& p) {
    const float values[] = {p.position_x, p.position_y, p.position_z,
        p.orientation_x, p.orientation_y, p.orientation_z, p.orientation_w};
    for (float value : values) if (!std::isfinite(value)) return false;
    const float norm2 = p.orientation_x*p.orientation_x + p.orientation_y*p.orientation_y +
                        p.orientation_z*p.orientation_z + p.orientation_w*p.orientation_w;
    if (norm2 < 0.5f || norm2 > 1.5f) return false;
    const float inv = 1.0f / std::sqrt(norm2);
    p.orientation_x *= inv; p.orientation_y *= inv;
    p.orientation_z *= inv; p.orientation_w *= inv;
    return true;
}
bool valid_fov(const XrApiFov& f) {
    return std::isfinite(f.angle_left) && std::isfinite(f.angle_right) &&
           std::isfinite(f.angle_up) && std::isfinite(f.angle_down) &&
           f.angle_left > -PI/2 && f.angle_left < 0 &&
           f.angle_right > 0 && f.angle_right < PI/2 &&
           f.angle_down > -PI/2 && f.angle_down < 0 &&
           f.angle_up > 0 && f.angle_up < PI/2;
}
Vec3 rotate(const XrApiPose& p, const Vec3& v) {
    const Vec3 q(p.orientation_x, p.orientation_y, p.orientation_z);
    const Vec3 t = Vec3::cross(q, v) * 2.0f;
    return v + t * p.orientation_w + Vec3::cross(q, t);
}
}
struct XrTransport::NetworkState {
    std::atomic<bool> active{false};
    std::thread receiver;
    XrTransportOptions options;
#ifdef _WIN32
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in destination{};
    bool winsock = false;
#endif
};
XrTransport::XrTransport() : network_(std::make_unique<NetworkState>()) {}
XrTransport::~XrTransport() { stop(); }

std::string XrTransport::enable_command(bool immersive_projection, bool sbs) {
    // haptics(2), VR, 3D, FOV overrides(2), ref count, id/type/pose(9),
    // action count, locate count, headset id(0), reference id(1).
    // XrInput.updateXrApp sends poses only for MODE_VR>0. Mode 2 leaves the
    // normal screen active while keeping UDP and SBS, unlike mode 0 (disabled).
    return std::string(immersive_projection ? "0 0 1 " : "0 0 2 ") +
        (sbs ? "1 " : "0 ") + "0 0 1 1 2 0 0 0 0 0 0 1 0 1 0 1";
}
std::string XrTransport::disable_command() { return "0 0 0 0 0 0"; }

bool XrTransport::parse_packet(std::string_view message, XrTrackingFrame& output, std::string& error) {
    error.clear();
    auto fail = [&](const char* why) { error = why; return false; };
    if (message.empty() || message.size() > 8192 || message.find('\0') != std::string_view::npos)
        return fail("Invalid XrAPI packet length or embedded NUL.");
    std::istringstream input{std::string(message)};
    input.imbue(std::locale::classic());
    std::vector<std::string> tokens;
    for (std::string token; input >> token;) tokens.push_back(std::move(token));
    size_t i = 0;
    if (tokens.empty()) return fail("Empty XrAPI packet.");
    // The pinned implementation prefixes client0/client1; the public table omits it.
    if (tokens[0].rfind("client", 0) == 0) {
        if (tokens[0] != "client0" && tokens[0] != "client1") return fail("Unknown XrAPI client prefix.");
        ++i;
    }
    const size_t scalar_start = i;
    while (i < tokens.size() && !bools(tokens[i], 19)) ++i;
    const size_t scalar_count = i - scalar_start;
    if (i == tokens.size() || (scalar_count != 13 && scalar_count != 14))
        return fail("Expected XrAPI 0.6 scalar fields followed by 19 controller buttons.");
    XrTrackingFrame frame;
    float scalars[14]{};
    for (size_t j = 0; j < scalar_count; ++j)
        if (!number(tokens[scalar_start+j], scalars[j])) return fail("Non-finite or malformed tracking scalar.");
    for (size_t j = 0; j < 8; ++j) frame.controller_axes[j] = scalars[j];
    frame.ipd_meters = scalars[8];
    if (frame.ipd_meters < 0.03f || frame.ipd_meters > 0.09f ||
        scalars[9] < 1 || scalars[9] >= 179 || scalars[10] < 1 || scalars[10] >= 179)
        return fail("IPD or aggregate FOV is outside supported bounds.");
    // 0.6 supplies aggregate angles; these are explicitly symmetric approximations.
    const float half_x = scalars[9] * PI / 360.0f;
    const float half_y = scalars[10] * PI / 360.0f;
    frame.fov_left = frame.fov_right = {-half_x, half_x, half_y, -half_y};
    const size_t sync_index = scalar_count == 14 ? 12 : 11;
    if (scalar_count == 14) {
        frame.refresh_hz = scalars[11];
        if (frame.refresh_hz < 1 || frame.refresh_hz > 240) return fail("Invalid display refresh rate.");
    }
    uint32_t sync = 0;
    if (!integer(tokens[scalar_start+sync_index], sync, 255) ||
        !integer(tokens[scalar_start+sync_index+1], frame.recenter_count, UINT32_MAX))
        return fail("Invalid host sync or recenter counter.");
    frame.hmd_sync = static_cast<uint8_t>(sync);
    for (size_t j = 0; j < 19; ++j) frame.controller_buttons[j] = tokens[i][j] == 'T';
    ++i;
    uint32_t count = 0;
    if (i >= tokens.size() || !integer(tokens[i++], count, 32)) return fail("Invalid locate-space count.");
    bool found_head = false;
    for (uint32_t s = 0; s < count; ++s) {
        if (tokens.size()-i < 16) return fail("Truncated locate-space record.");
        uint32_t space = 0, base = 0;
        if (!integer(tokens[i++], space, UINT32_MAX) || !integer(tokens[i++], base, UINT32_MAX))
            return fail("Invalid space identifier.");
        float values[14]{};
        for (size_t j = 0; j < 14; ++j)
            if (!number(tokens[i++], values[j])) return fail("Malformed pose or velocity record.");
        if (space == 0 && base == LOCAL_REFERENCE_SPACE_ID) {
            if (found_head) return fail("Duplicate headset pose.");
            found_head = true;
            frame.pose = {values[3], values[4], values[5], values[6], values[0], values[1], values[2]};
            frame.pose_valid = normalize_pose(frame.pose);
        }
    }
    if (!found_head) return fail("No headset pose in the requested LOCAL reference space.");
    if (i < tokens.size()) {
        if (i+1 != tokens.size() || !bools(tokens[i], 2)) return fail("Unexpected trailing XrAPI fields.");
        frame.immersive_screen = tokens[i][0] == 'T';
        frame.sbs = tokens[i][1] == 'T';
    }
    output = frame;
    return true;
}

bool XrTransport::start(std::string& error, const XrTransportOptions& options) {
    stop();
    error.clear();
    if (options.stale_after.count() <= 0 || options.stale_after.count() > 150) {
        error = "Tracking timeout must be between 1 and 150 milliseconds.";
        record_error(error); return false;
    }
    if (options.projection_warmup.count() < 0 || options.projection_warmup.count() > 60000) {
        error = "Projection startup warmup must be between 0 and 60000 milliseconds.";
        record_error(error); return false;
    }
#ifndef _WIN32
    error = "The WinlatorXR UDP client requires the Windows/Wine build.";
    record_error(error); return false;
#else
    network_->options = options;
    WSADATA data{};
    int result = WSAStartup(MAKEWORD(2,2), &data);
    if (result != 0) { error = "WSAStartup failed: " + std::to_string(result); record_error(error); return false; }
    network_->winsock = true;
    network_->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (network_->socket == INVALID_SOCKET) {
        error = "Cannot create XrAPI UDP socket: " + std::to_string(WSAGetLastError());
        stop(); record_error(error); return false;
    }
    BOOL exclusive = TRUE;
    setsockopt(network_->socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(options.receive_port);
    if (::bind(network_->socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        error = "Cannot bind loopback XrAPI receiver (another app may own the port): " + std::to_string(WSAGetLastError());
        stop(); record_error(error); return false;
    }
    u_long nonblocking = 1;
    if (ioctlsocket(network_->socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        error = "Cannot make XrAPI socket nonblocking."; stop(); record_error(error); return false;
    }
    network_->destination.sin_family = AF_INET;
    network_->destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    network_->destination.sin_port = htons(options.control_port);
    if (options.write_version_file) {
        const std::filesystem::path path(options.version_file);
        std::error_code ec;
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) { error = "Cannot create XrAPI version directory: " + ec.message(); stop(); record_error(error); return false; }
        std::ofstream version(path, std::ios::binary | std::ios::trunc);
        version << "0.6\n";
        version.close();
        if (!version) { error = "Cannot write XrAPI version file: " + options.version_file; stop(); record_error(error); return false; }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stale_after_ = options.stale_after; have_packet_ = false; recenter_pending_ = false;
        session_state_ = XrSessionState::Unknown; last_error_.clear();
    }
    network_->active.store(true);
    try { network_->receiver = std::thread(&XrTransport::receive_loop, this); }
    catch (const std::exception& e) {
        error = std::string("Cannot start XrAPI receiver: ") + e.what(); stop(); record_error(error); return false;
    }
    return true;
#endif
}
void XrTransport::stop() {
    const bool was_active = network_->active.exchange(false);
    if (network_->receiver.joinable()) network_->receiver.join();
#ifdef _WIN32
    if (network_->socket != INVALID_SOCKET) {
        if (was_active) {
            const auto message = disable_command();
            sendto(network_->socket, message.data(), static_cast<int>(message.size()), 0,
                   reinterpret_cast<const sockaddr*>(&network_->destination), sizeof(network_->destination));
        }
        closesocket(network_->socket); network_->socket = INVALID_SOCKET;
    }
    if (network_->winsock) { WSACleanup(); network_->winsock = false; }
#else
    (void)was_active;
#endif
    std::lock_guard<std::mutex> lock(mutex_);
    have_packet_ = false; latest_.pose_valid = false; session_state_ = XrSessionState::Unknown;
}
bool XrTransport::running() const { return network_->active.load(); }
void XrTransport::record_error(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_); last_error_ = error;
}
std::string XrTransport::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_); return last_error_;
}
void XrTransport::receive_loop() {
#ifdef _WIN32
    const std::string target_command = enable_command(network_->options.immersive_projection);
    const std::string warmup_command = enable_command(false, false);
    const auto warmup_until = std::chrono::steady_clock::now() + network_->options.projection_warmup;
    auto last_command = std::chrono::steady_clock::time_point{};
    bool sent_command = false, last_warmup = false;
    while (network_->active.load()) {
        const auto now = std::chrono::steady_clock::now();
        const bool warming_up = now < warmup_until;
        // Send the phase transition promptly, independently of the 500 ms
        // command heartbeat. Both phases keep the same LOCAL pose subscription.
        if (!sent_command || warming_up != last_warmup ||
            now-last_command >= std::chrono::milliseconds(500)) {
            const std::string& command = warming_up ? warmup_command : target_command;
            if (sendto(network_->socket, command.data(), static_cast<int>(command.size()), 0,
                       reinterpret_cast<const sockaddr*>(&network_->destination), sizeof(network_->destination)) == SOCKET_ERROR)
                record_error("XrAPI control send failed: " + std::to_string(WSAGetLastError()));
            last_command = now;
            last_warmup = warming_up;
            sent_command = true;
        }
        fd_set read_set; FD_ZERO(&read_set); FD_SET(network_->socket, &read_set);
        timeval timeout{0, 10000};
        const int ready = select(0, &read_set, nullptr, nullptr, &timeout);
        if (ready == SOCKET_ERROR) { record_error("XrAPI select failed."); break; }
        if (ready == 0) continue;
        for (int packet = 0; packet < 64; ++packet) {
            char buffer[8193];
            sockaddr_in source{}; int source_size = sizeof(source);
            const int bytes = recvfrom(network_->socket, buffer, sizeof(buffer), 0,
                reinterpret_cast<sockaddr*>(&source), &source_size);
            if (bytes == SOCKET_ERROR) {
                const int code = WSAGetLastError();
                if (code != WSAEWOULDBLOCK && code != WSAEMSGSIZE)
                    record_error("XrAPI receive failed: " + std::to_string(code));
                break;
            }
            if (source.sin_family != AF_INET || source.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) continue;
            XrTrackingFrame frame; std::string error;
            if (parse_packet(std::string_view(buffer, static_cast<size_t>(bytes)), frame, error))
                accept_tracking(frame);
            else record_error(error);
        }
    }
    network_->active.store(false);
#endif
}
void XrTransport::accept_tracking(XrTrackingFrame frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Repeated host-sync values must not refresh a stalled frame's deadline.
    if (have_packet_ && frame.hmd_sync == latest_.hmd_sync &&
        frame.recenter_count == latest_.recenter_count && frame.pose_valid == latest_.pose_valid) return;
    if (have_packet_ && frame.recenter_count != latest_.recenter_count) recenter_pending_ = true;
    frame.local_sample_id = latest_.local_sample_id + 1;
    frame.received_at = std::chrono::steady_clock::now();
    latest_ = frame; have_packet_ = true;
}
bool XrTransport::latest_tracking(XrTrackingFrame& frame) const {
    std::lock_guard<std::mutex> lock(mutex_);
    frame = latest_;
    return have_packet_ && frame.pose_valid &&
           std::chrono::steady_clock::now()-frame.received_at <= stale_after_;
}
bool XrTransport::has_valid_pose() const { XrTrackingFrame frame; return latest_tracking(frame); }
bool XrTransport::consume_recenter() {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool value = recenter_pending_; recenter_pending_ = false; return value;
}
void XrTransport::update_hmd_pose(const XrApiPose& pose, float ipd, const XrApiFov& left, const XrApiFov& right) {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_.pose = pose; latest_.ipd_meters = ipd;
    latest_.fov_left = left; latest_.fov_right = right;
    latest_.pose_valid = normalize_pose(latest_.pose) && std::isfinite(ipd) && ipd >= 0.03f && ipd <= 0.09f &&
                         valid_fov(left) && valid_fov(right);
    latest_.fov_is_symmetric_approximation = false;
    latest_.received_at = std::chrono::steady_clock::now();
    ++latest_.local_sample_id; have_packet_ = true;
}
void XrTransport::set_session_state(XrSessionState state) {
    std::lock_guard<std::mutex> lock(mutex_); session_state_ = state;
}
XrSessionState XrTransport::session_state() const {
    std::lock_guard<std::mutex> lock(mutex_); return session_state_;
}
bool XrTransport::is_focused() const { return session_state() == XrSessionState::Focused; }
void XrTransport::compute_eye_transforms(const XrTrackingFrame& frame, Eye eye, Mat4& view, Mat4& projection) {
    XrApiPose pose = frame.pose;
    if (!normalize_pose(pose)) { view = Mat4::identity(); projection = Mat4::identity(); return; }
    const Vec3 right = rotate(pose, {1,0,0}), up = rotate(pose, {0,1,0}), forward = rotate(pose, {0,0,-1});
    const float eye_offset = frame.ipd_meters * (eye == Eye::Left ? -0.5f : 0.5f);
    const Vec3 eye_pos = Vec3(pose.position_x, pose.position_y, pose.position_z) + right * eye_offset;
    view = Mat4::identity();
    view.m[0] = right.x; view.m[4] = right.y; view.m[8] = right.z;
    view.m[1] = up.x; view.m[5] = up.y; view.m[9] = up.z;
    view.m[2] = forward.x; view.m[6] = forward.y; view.m[10] = forward.z;
    view.m[12] = -Vec3::dot(right, eye_pos);
    view.m[13] = -Vec3::dot(up, eye_pos);
    view.m[14] = -Vec3::dot(forward, eye_pos);
    const XrApiFov& fov = eye == Eye::Left ? frame.fov_left : frame.fov_right;
    if (!valid_fov(fov)) { projection = Mat4::identity(); return; }
    const float left = std::tan(fov.angle_left), right_tan = std::tan(fov.angle_right);
    const float bottom = std::tan(fov.angle_down), top = std::tan(fov.angle_up);
    const float near_z = 0.05f, far_z = 100.0f;
    std::fill(std::begin(projection.m), std::end(projection.m), 0.0f);
    projection.m[0] = 2.0f / (right_tan-left); projection.m[5] = 2.0f / (top-bottom);
    projection.m[8] = -(right_tan+left) / (right_tan-left);
    projection.m[9] = -(top+bottom) / (top-bottom);
    projection.m[10] = far_z / (far_z-near_z); projection.m[11] = 1.0f;
    projection.m[14] = -near_z*far_z / (far_z-near_z);
}
void XrTransport::compute_eye_transforms(Eye eye, const CeilingConfig&, Mat4& view, Mat4& projection) const {
    XrTrackingFrame frame;
    { std::lock_guard<std::mutex> lock(mutex_); frame = latest_; }
    compute_eye_transforms(frame, eye, view, projection);
}
uint32_t XrTransport::encode_sync_pixel(uint8_t hmd_sync) {
    return 0xFF000000u | (static_cast<uint32_t>(hmd_sync) << 16);
}
} // namespace gta2ceiling
