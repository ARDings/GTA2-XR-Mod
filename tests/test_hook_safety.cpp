#include <iostream>
#include <cassert>
#include <vector>
#include <sstream>

#include "gta2ceiling/adapter.h"

using namespace gta2ceiling;

struct DiagnosticReport {
    std::string mod_version = "0.1.0";
    std::string game_version;
    std::string error_class;
    std::string status_message;
    bool is_clean_abort = false;
};

DiagnosticReport attempt_hook_initialization(const uint8_t* image, size_t size, const char* expected_hash, const char* actual_hash) {
    DiagnosticReport report;
    std::string err;

    // 1. PE safety verification
    if (!GameAdapter::verify_module_safety(image, size, err)) {
        report.error_class = "PE_STRUCTURE_ERROR";
        report.status_message = err;
        report.is_clean_abort = true;
        return report;
    }

    // 2. Hash whitelist check
    if (std::string(actual_hash) != std::string(expected_hash)) {
        report.error_class = "VERSION_MISMATCH_BLOCKED";
        report.status_message = "Target executable hash not in whitelist. Expected v9.6.0.0. Hooking safely aborted.";
        report.is_clean_abort = true;
        return report;
    }

    // 3. Signature scan for main loop tick hook
    const char pattern[] = "\x55\x8B\xEC";
    const char mask[] = "xxx";
    const uint8_t* hook_target = GameAdapter::scan_pattern(image, size, pattern, mask);
    if (!hook_target) {
        report.error_class = "SIGNATURE_NOT_FOUND";
        report.status_message = "Critical game hook signature missing. Aborting hook install.";
        report.is_clean_abort = true;
        return report;
    }

    report.error_class = "NONE";
    report.game_version = "9.6.0.0";
    report.status_message = "Hooks verified and installed safely.";
    report.is_clean_abort = false;
    return report;
}

void test_version_safe_hooks() {
    std::cout << "=========================================================\n";
    std::cout << "  Version-Safe Hooks & Startup Diagnostics Test \n";
    std::cout << "=========================================================\n";

    // Scenario 1: Malformed PE header (Corrupt executable)
    std::vector<uint8_t> corrupt_pe(1024, 0);
    DiagnosticReport r1 = attempt_hook_initialization(corrupt_pe.data(), corrupt_pe.size(),
        GameAdapter::SUPPORTED_SHA256, GameAdapter::SUPPORTED_SHA256);
    assert(r1.is_clean_abort);
    assert(r1.error_class == "PE_STRUCTURE_ERROR");
    std::cout << "[1/3] Corrupt PE properly caught: " << r1.status_message << "\n";

    // Scenario 2: Valid PE but mismatched hash (e.g. unknown v11.44 or pirated build)
    std::vector<uint8_t> valid_pe(4096, 0);
    valid_pe[0] = 'M'; valid_pe[1] = 'Z';
    *reinterpret_cast<uint32_t*>(&valid_pe[0x3C]) = 0x80;
    valid_pe[0x80] = 'P'; valid_pe[0x81] = 'E';

    DiagnosticReport r2 = attempt_hook_initialization(valid_pe.data(), valid_pe.size(),
        GameAdapter::SUPPORTED_SHA256, "c1945badbeef0000000000000000000000000000000000000000000000000000");
    assert(r2.is_clean_abort);
    assert(r2.error_class == "VERSION_MISMATCH_BLOCKED");
    std::cout << "[2/3] Version mismatch properly blocked: " << r2.status_message << "\n";

    // Scenario 3: Valid PE + Verified v9.6 hash + matching signature
    valid_pe[0x200] = 0x55; valid_pe[0x201] = 0x8B; valid_pe[0x202] = 0xEC; // pattern
    DiagnosticReport r3 = attempt_hook_initialization(valid_pe.data(), valid_pe.size(),
        GameAdapter::SUPPORTED_SHA256, GameAdapter::SUPPORTED_SHA256);
    assert(!r3.is_clean_abort);
    assert(r3.error_class == "NONE");
    assert(r3.game_version == "9.6.0.0");
    std::cout << "[3/3] Verified v9.6 hook installed successfully: " << r3.status_message << "\n";

    std::cout << "=========================================================\n";
    std::cout << "  HOOK SAFETY & DIAGNOSTICS PASSED   \n";
    std::cout << "=========================================================\n";
}

int main() {
    test_version_safe_hooks();
    return 0;
}

