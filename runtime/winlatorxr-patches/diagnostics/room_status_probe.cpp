// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only live metadata probe. Run as the debuggable GTA2Ceiling host UID,
// with its private data directory as cwd. Never accepts a token as an argument.
#include "room_status_summary.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
using gcc_diagnostic::Json;
struct Fd { int value = -1; ~Fd() { if (value >= 0) ::close(value); } };
[[noreturn]] void Fail(const char* reason) { throw std::runtime_error(reason); }
std::string ReadPrivateToken() {
    Fd fd{::open("files/imagefs/tmp/xr/gta2ceiling_room.token", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (fd.value < 0) Fail("PRIVATE_TOKEN_UNAVAILABLE");
    struct stat info{};
    if (::fstat(fd.value, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
        (info.st_mode & 0077) != 0 || info.st_size != 32) Fail("PRIVATE_TOKEN_UNSAFE");
    char bytes[33]{};
    if (::read(fd.value, bytes, sizeof bytes) != 32) Fail("PRIVATE_TOKEN_READ_FAILED");
    std::string token(bytes, 32);
    if (!std::all_of(token.begin(), token.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        Fail("PRIVATE_TOKEN_INVALID");
    return token;
}
bool Identifier(const std::string& text) {
    return !text.empty() && text.size() <= 32 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}
Json Exchange(int fd, const std::string& request, const std::string& nonce, const std::string& tail) {
    const std::string packet = "GCCROOM1 " + request + " " + nonce + tail;
    if (::send(fd, packet.data(), packet.size(), 0) != static_cast<ssize_t>(packet.size())) Fail("UDP_SEND_FAILED");
    char buffer[60001];
    const ssize_t count = ::recv(fd, buffer, sizeof buffer, 0);
    if (count <= 0) Fail("UDP_NO_REPLY");
    if (count > 60000) Fail("UDP_REPLY_TOO_LARGE");
    const Json value = Json::parse(buffer, buffer + count);
    if (value.at("protocol") != "GCCROOM1" || value.at("request") != request || value.at("nonce") != nonce ||
        value.at("upstreamCommit") != "39b270c6401a69e7c28ea6ffd61de3dbac9e9b1e") Fail("INVALID_REPLY_IDENTITY");
    return value;
}
}

int main(int argc, char**) {
    if (argc != 1) { std::puts("{\"probeError\":\"NO_ARGUMENTS_ACCEPTED\"}"); return 2; }
    try {
        const auto token = ReadPrivateToken();
        Fd socket{::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)};
        if (socket.value < 0) Fail("UDP_SOCKET_FAILED");
        sockaddr_in peer{}; peer.sin_family = AF_INET; peer.sin_port = htons(7279); peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        // Connected UDP also rejects packets from other IPs/ports.
        if (::connect(socket.value, reinterpret_cast<sockaddr*>(&peer), sizeof peer) != 0) Fail("UDP_CONNECT_FAILED");
        const timeval timeout{1, 0};
        if (::setsockopt(socket.value, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) != 0) Fail("UDP_TIMEOUT_SETUP_FAILED");
        const auto start = std::chrono::steady_clock::now();
        const auto seed = std::chrono::duration_cast<std::chrono::microseconds>(start.time_since_epoch()).count();
        for (int attempt = 0; attempt < 3; ++attempt) {
            const std::string nonce = "diag" + std::to_string(seed) + "_" + std::to_string(attempt);
            const Json caps = Exchange(socket.value, "CAPS", nonce, "");
            if (caps.at("status") != "OK" || !caps.at("tokenReady").get<bool>()) Fail("ROOM_HOST_NOT_READY");
            const auto session = caps.at("sessionId").get<std::string>();
            if (!Identifier(session)) Fail("INVALID_SESSION_ID");
            const auto epoch = caps.at("referenceEpoch").get<uint64_t>();
            const auto getStart = std::chrono::steady_clock::now();
            const Json reply = Exchange(socket.value, "GET_ROOM", nonce + "r", " 1 " + session + " " + std::to_string(epoch) + " " + token);
            const auto status = reply.at("status").get<std::string>();
            if (status == "STALE_SESSION" || status == "STALE_REFERENCE_EPOCH") continue;
            Json summary = gcc_diagnostic::Summary(reply);
            summary["getRoundTripMilliseconds"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - getStart).count();
            std::puts(summary.dump(2).c_str());
            return summary.value("sceneStatus", std::string()) == "SUCCESS_CEILING_DATA" ? 0 : 3;
        }
        std::puts("{\"probeError\":\"REFERENCE_CHANGED_DURING_HANDSHAKE\"}"); return 3;
    } catch (...) {
        // Never print exception.what(): JSON exceptions may include private data.
        std::puts("{\"probeError\":\"METADATA_PROBE_FAILED_NO_PRIVATE_DETAILS_LOGGED\"}"); return 1;
    }
}
