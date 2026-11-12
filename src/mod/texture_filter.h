#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#ifdef _WIN32
#include <d3d9.h>
#endif

namespace gta2ceiling {

// Packed, straight-alpha 0xAARRGGBB pixels (D3DFMT_A8R8G8B8 on Windows).
// Filtering averages alpha coverage and weights RGB by that coverage, so RGB
// stored in invisible texels cannot produce fringes around sprites.
struct ArgbMipLevel {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint32_t> argb;
};

struct MipOptions {
    // Zero builds the complete D3D mip chain down to 1x1. A positive value
    // caps the number of levels, including level zero.
    uint32_t max_levels = 0;
    // Explicit opt-in: preserve alpha but move exact RGB(255,0,255) to
    // RGB(255,1,255), including level zero. This does not defeat the host's
    // colour-key tolerance; it only removes exact key-colour collisions.
    bool avoid_exact_magenta = false;
    // For straight-alpha cutouts only: give fully transparent texels touching
    // visible texels their neighbours' RGB after generating EACH lower level.
    // Alpha and original level zero remain unchanged. Do not enable for
    // additive SRCBLEND=ONE artwork, where invisible RGB would add a halo.
    bool bleed_transparent_edges = false;
};

inline constexpr uint32_t kMaxMipDimension = 16384;
inline constexpr uint64_t kMaxMipPixels = 64ull * 1024 * 1024;

// NPOT extents use area weights, retaining the entire source image rather
// than dropping the final odd row/column. On failure, output is unchanged.
// Downsampling an already 1x1 image returns false.
bool DownsampleArgbMip(std::span<const uint32_t> pixels, uint32_t width,
                       uint32_t height, ArgbMipLevel& output,
                       std::string& error, bool avoidExactMagenta = false,
                       bool bleedTransparentEdges = false);
bool BuildArgbMipChain(std::span<const uint32_t> pixels, uint32_t width,
                       uint32_t height, std::vector<ArgbMipLevel>& output,
                       std::string& error, MipOptions options = {});

#ifdef _WIN32
// Uploads every CPU-generated level to one managed D3D9 texture. No GPU
// autogeneration is required. *output must be null and remains null on failure.
HRESULT CreateManagedArgbMipTexture(IDirect3DDevice9* device,
                                    std::span<const uint32_t> pixels,
                                    uint32_t width, uint32_t height,
                                    IDirect3DTexture9** output,
                                    MipOptions options = {});
#endif

} // namespace gta2ceiling
