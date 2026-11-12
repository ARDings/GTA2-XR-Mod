// Environment/transport observations only. This program does not run GTA2,
// create a graphics device, submit stereo frames, or approve a release gate.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "gta2ceiling/xr_transport.h"

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
struct Options {
    double xr_seconds = 0;
    fs::path output_directory;
    std::string xr_version_file;
    bool write_version_file = true;
    bool immersive_projection = true;
};
void Usage() {
    std::cout << "GTA2Ceiling runtime observation probe\n"
              << "  --output-dir PATH       Log and disk-test directory\n"
              << "  --xr-seconds N          Observe XrAPI UDP for 0 < N <= 3600 seconds\n"
              << "  --xr-version-file PATH  Override the Winlator version handshake path\n"
              << "  --xr-no-version-file    Skip handshake file (loopback fixtures only)\n"
              << "  --xr-flat               Diagnostic screen mode (API MODE_VR=2, SBS=1)\n"
              << "  --help                  Show this help\n"
              << "Native Windows defaults to D:\\GTA2Ceiling-work\\runtime-probe.\n"
              << "Wine defaults to probe-logs beside this executable in the private game copy.\n"
              << "Exit: 0 requested observations passed; 1 an observation failed;\n"
              << "      2 invalid arguments/output path; 3 unsupported platform.\n"
              << "Success is not GTA2, GPU, stereo, audio playback, or Quest acceptance.\n";
}
Options Parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--xr-no-version-file") { options.write_version_file = false; continue; }
        if (argument == "--xr-flat") { options.immersive_projection = false; continue; }
        if (argument != "--xr-seconds" && argument != "--output-dir" && argument != "--xr-version-file")
            throw std::invalid_argument("Unknown argument: " + argument);
        if (++i == argc || !*argv[i]) throw std::invalid_argument("Missing value for " + argument);
        const std::string value = argv[i];
        if (argument == "--output-dir") options.output_directory = fs::path(value);
        else if (argument == "--xr-version-file") options.xr_version_file = value;
        else {
            size_t end = 0;
            options.xr_seconds = std::stod(value, &end);
            if (end != value.size() || !std::isfinite(options.xr_seconds) ||
                options.xr_seconds <= 0 || options.xr_seconds > 3600)
                throw std::invalid_argument("--xr-seconds must be finite and between 0 and 3600 (exclusive 0).");
        }
    }
    if (!options.write_version_file && !options.xr_version_file.empty())
        throw std::invalid_argument("--xr-version-file and --xr-no-version-file are mutually exclusive.");
    if (options.xr_seconds == 0 && (!options.write_version_file || !options.xr_version_file.empty() ||
                                   !options.immersive_projection))
        throw std::invalid_argument("XrAPI handshake/mode options require --xr-seconds.");
    return options;
}
struct Log {
    explicit Log(const fs::path& path) : file(path, std::ios::out | std::ios::trunc) {
        if (!file) throw std::runtime_error("Cannot open probe log: " + path.string());
    }
    template<class... Parts> void line(const Parts&... parts) {
        std::ostringstream message;
        (message << ... << parts);
        std::cout << message.str() << '\n';
        file << message.str() << '\n';
        file.flush();
    }
    bool good() { file.flush(); return file.good(); }
    std::ofstream file;
};
#ifdef _WIN32
fs::path ExecutableDirectory() {
    std::vector<wchar_t> path(32768);
    const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) throw std::runtime_error("Cannot resolve executable directory.");
    return fs::path(std::wstring(path.data(), count)).parent_path();
}
bool DriveD(const fs::path& path) {
    const auto drive = path.root_name().wstring();
    return drive == L"D:" || drive == L"d:";
}
#endif
}

int main(int argc, char** argv) {
    if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        Usage(); return 0;
    }
    Options options;
    try { options = Parse(argc, argv); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; Usage(); return 2; }
#ifndef _WIN32
    std::cerr << "Unsupported: this probe requires Windows or Wine. No observations performed.\n";
    return 3;
#else
    try {
        bool is_wine = false;
        std::string wine_version = "not detected";
        using WineVersion = const char* (__cdecl*)();
        if (HMODULE ntdll = GetModuleHandleA("ntdll.dll")) {
            if (auto version = reinterpret_cast<WineVersion>(GetProcAddress(ntdll, "wine_get_version"))) {
                is_wine = true;
                const char* value = version();
                wine_version = value ? value : "unavailable";
            }
        }
        if (options.output_directory.empty()) options.output_directory = is_wine
            ? ExecutableDirectory() / "probe-logs"
            : fs::path(L"D:\\GTA2Ceiling-work\\runtime-probe");
        options.output_directory = fs::absolute(options.output_directory).lexically_normal();
        if (!is_wine && !DriveD(options.output_directory))
            throw std::invalid_argument("Native Windows probe artifacts must be on drive D:.");
        fs::create_directories(options.output_directory);
        const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const std::string run = "runtime-probe-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(epoch_ms);
        const fs::path log_path = options.output_directory / (run + ".log");
        Log log(log_path);
        log.line("GTA2Ceiling environment/transport observations; not a G0 or release acceptance.");
        log.line("Log: ", log_path.string());
        log.line("Process pointer width: ", sizeof(void*) * 8, " bits");
        log.line("Wine detected: ", is_wine ? "yes" : "no", "; wine_get_version: ", wine_version);
        log.line("Host device/Winlator build/GPU driver: not identified by this probe.");
        log.line("Start Unix epoch milliseconds: ", epoch_ms);
        int failures = 0;
        auto check = [&](bool passed, const std::string& label) {
            log.line(passed ? "PASS " : "FAIL ", label);
            if (!passed) ++failures;
            return passed;
        };
        BOOL wow64 = FALSE;
        if (check(IsWow64Process(GetCurrentProcess(), &wow64) != FALSE, "IsWow64Process query"))
            log.line("WoW64 process flag: ", wow64 ? "true" : "false", " (not proof of an ARM translation backend)");
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        if (check(GlobalMemoryStatusEx(&memory) != FALSE, "GlobalMemoryStatusEx query"))
            log.line("OS-reported physical memory: total=", memory.ullTotalPhys / (1024 * 1024),
                     " MiB available=", memory.ullAvailPhys / (1024 * 1024), " MiB");

        const fs::path io_path = options.output_directory / (run + ".io.tmp");
        const std::string token = "GTA2Ceiling disk roundtrip " + run;
        bool written = false;
        {
            std::ofstream output(io_path, std::ios::binary | std::ios::trunc);
            output << token;
            output.flush();
            written = output.good();
            output.close();
            written = written && !output.fail();
        }
        std::string readback;
        bool read = false;
        if (written) {
            std::ifstream input(io_path, std::ios::binary);
            readback.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
            read = input.is_open() && !input.bad() && readback == token;
        }
        check(written && read, "Private-directory disk write/read content roundtrip");
        std::error_code remove_error;
        const bool removed = fs::remove(io_path, remove_error);
        check(!remove_error && (removed || !written), "Disk-test temporary file cleanup");

        for (const char* dll : {"d3d9.dll", "dsound.dll"}) {
            HMODULE module = LoadLibraryA(dll);
            const DWORD error = module ? 0 : GetLastError();
            check(module != nullptr, std::string("LoadLibrary ") + dll + " (library availability only; error=" + std::to_string(error) + ")");
            if (module) FreeLibrary(module);
        }
        log.line("Not tested: D3D device creation, raster output, stereo presentation, sound output, controller, GTA2 gameplay.");

        if (options.xr_seconds > 0) {
            gta2ceiling::XrTransport transport;
            gta2ceiling::XrTransportOptions transport_options;
            transport_options.write_version_file = options.write_version_file;
            transport_options.immersive_projection = options.immersive_projection;
            if (!options.xr_version_file.empty()) transport_options.version_file = options.xr_version_file;
            log.line("XrAPI 0.6 observation requested for ", options.xr_seconds,
                     " seconds; loopback receive=", transport_options.receive_port,
                     " control=", transport_options.control_port);
            log.line("Requested presentation: ", options.immersive_projection ?
                     "projection (API MODE_VR=1, SBS=1)" :
                     "diagnostic screen (API MODE_VR=2, SBS=1; UDP remains active)");
            log.line("Startup phase: MODE_VR=2/SBS=0 for ", transport_options.projection_warmup.count(),
                     " ms before the requested presentation; host import workaround under investigation.");
            log.line("Version handshake file: ", transport_options.write_version_file ? transport_options.version_file : "disabled (fixture mode)");
            std::string error;
            if (check(transport.start(error, transport_options), "XrAPI UDP transport start")) {
                const auto started = Clock::now();
                const auto deadline = started + std::chrono::duration<double>(options.xr_seconds);
                auto next_log = started;
                uint64_t previous_sample = 0, observed_samples = 0;
                gta2ceiling::XrTrackingFrame frame;
                while (Clock::now() < deadline && transport.running()) {
                    if (transport.latest_tracking(frame) && frame.local_sample_id != previous_sample) {
                        previous_sample = frame.local_sample_id;
                        ++observed_samples;
                        const auto now = Clock::now();
                        if (now >= next_log) {
                            log.line("XR fresh sample: elapsed_s=", std::chrono::duration<double>(now-started).count(),
                                     " id=", frame.local_sample_id, " host_sync=", unsigned(frame.hmd_sync),
                                     " recenter=", frame.recenter_count,
                                     " position_m=", frame.pose.position_x, ",", frame.pose.position_y, ",", frame.pose.position_z,
                                     " quaternion=", frame.pose.orientation_x, ",", frame.pose.orientation_y, ",", frame.pose.orientation_z, ",", frame.pose.orientation_w,
                                     " ipd_m=", frame.ipd_meters, " reported_refresh_hz=", frame.refresh_hz,
                                     " host_sbs=", frame.sbs, " host_immersive=", frame.immersive_screen);
                            next_log = now + std::chrono::seconds(1);
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                const bool still_fresh = transport.latest_tracking(frame);
                const bool still_running = transport.running();
                const double elapsed = std::chrono::duration<double>(Clock::now()-started).count();
                const std::string last_error = transport.last_error();
                transport.stop();
                log.line("XR observed distinct fresh samples=", observed_samples,
                         " accepted sample counter=", frame.local_sample_id,
                         " elapsed_s=", elapsed,
                         " observed_samples_per_second=", elapsed > 0 ? observed_samples / elapsed : 0,
                         " (UDP observation rate, not rendered FPS)");
                if (!last_error.empty()) log.line("Last transport diagnostic: ", last_error);
                check(still_running && observed_samples >= 2 && still_fresh,
                      "XrAPI delivered multiple fresh poses and remained fresh at observation end");
                log.line("XrAPI aggregate FOV is a symmetric approximation; no OpenXR session-validity flags or stereo output were verified.");
            } else {
                log.line("Transport failure: ", error);
            }
        } else {
            log.line("XrAPI packet observation: not requested (use --xr-seconds N).");
        }
        log.line("Requested observation failures: ", failures, "; exit_code=", failures ? 1 : 0);
        log.line("No claim of standalone GTA2 gameplay, G0/G1/G2 completion, or Quest compatibility is made.");
        if (!log.good()) { std::cerr << "Probe log write failed.\n"; return 1; }
        return failures ? 1 : 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << "Invalid probe configuration: " << error.what() << '\n'; return 2;
    } catch (const std::exception& error) {
        std::cerr << "Probe failed: " << error.what() << '\n'; return 1;
    }
#endif
}
