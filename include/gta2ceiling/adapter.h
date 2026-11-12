#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gta2ceiling {

struct GameVersionInfo {
    std::string exe_sha256;
    uint32_t file_size = 0;
    std::string version_string;
    bool is_supported = false;
};

class GameAdapter {
public:
    static constexpr const char* SUPPORTED_SHA256 = "420af867036b1a66ef94c54b8db2e7e654da359031987c6602bcfa74972e089f";
    static constexpr uint32_t SUPPORTED_FILE_SIZE = 1667072;

    static bool verify_module_safety(const uint8_t* module_base, size_t module_size, std::string& out_error_message) {
        if (!module_base || module_size < 0x1000) {
            out_error_message = "Invalid module base address or size too small.";
            return false;
        }

        // Check DOS MZ Header
        if (module_base[0] != 'M' || module_base[1] != 'Z') {
            out_error_message = "Corrupt executable: Missing MZ signature.";
            return false;
        }

        // Read e_lfanew
        uint32_t pe_offset = *reinterpret_cast<const uint32_t*>(module_base + 0x3C);
        if (pe_offset + 4 > module_size) {
            out_error_message = "Corrupt executable: PE header offset outside image boundaries.";
            return false;
        }

        // Check PE signature
        const uint8_t* pe_sig = module_base + pe_offset;
        if (pe_sig[0] != 'P' || pe_sig[1] != 'E' || pe_sig[2] != 0 || pe_sig[3] != 0) {
            out_error_message = "Corrupt executable: Invalid PE signature.";
            return false;
        }

        return true;
    }

    // Pattern scanner helper for safe hook installation
    static const uint8_t* scan_pattern(const uint8_t* base, size_t size, const char* pattern, const char* mask) {
        size_t pattern_len = std::string(mask).length();
        if (pattern_len == 0 || size < pattern_len) return nullptr;

        for (size_t i = 0; i <= size - pattern_len; ++i) {
            bool match = true;
            for (size_t j = 0; j < pattern_len; ++j) {
                if (mask[j] == 'x' && base[i + j] != static_cast<uint8_t>(pattern[j])) {
                    match = false;
                    break;
                }
            }
            if (match) return base + i;
        }
        return nullptr;
    }
};

} // namespace gta2ceiling

