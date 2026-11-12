#include "texture_filter.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace gta2ceiling {
namespace {
bool ValidImage(std::span<const uint32_t> pixels, uint32_t width,
                uint32_t height, std::string& error) {
    const uint64_t count = static_cast<uint64_t>(width) * height;
    if (!width || !height || width > kMaxMipDimension || height > kMaxMipDimension ||
        count > kMaxMipPixels || count != pixels.size() || !pixels.data()) {
        error = "Invalid ARGB image dimensions or pixel count.";
        return false;
    }
    return true;
}

uint32_t EscapeMagenta(uint32_t pixel, bool enabled) {
    return enabled && (pixel & 0x00ffffffu) == 0x00ff00ffu ? pixel | 0x00000100u : pixel;
}

void BleedTransparentRgb(ArgbMipLevel& level) {
    // One neighbour ring covers the bilinear footprint at this mip level.
    // Read from a snapshot so scan direction cannot propagate colours farther
    // on one side. Thin 1xN/Nx1 mip levels are included.
    bool visible = false, transparent = false;
    for (uint32_t pixel : level.argb) {
        visible |= (pixel >> 24) != 0;
        transparent |= (pixel >> 24) == 0;
    }
    if (!visible || !transparent) return;
    const auto source = level.argb;
    for (uint32_t y = 0; y < level.height; ++y) for (uint32_t x = 0; x < level.width; ++x) {
        const size_t here = static_cast<size_t>(y) * level.width + x;
        if (source[here] >> 24) continue;
        uint32_t alpha = 0, red = 0, green = 0, blue = 0;
        const uint32_t firstY = y ? y - 1 : 0, endY = std::min(level.height, y + 2);
        const uint32_t firstX = x ? x - 1 : 0, endX = std::min(level.width, x + 2);
        for (uint32_t sy = firstY; sy < endY; ++sy) for (uint32_t sx = firstX; sx < endX; ++sx) {
            const uint32_t pixel = source[static_cast<size_t>(sy) * level.width + sx];
            const uint32_t a = pixel >> 24;
            alpha += a;
            red += a * ((pixel >> 16) & 255u);
            green += a * ((pixel >> 8) & 255u);
            blue += a * (pixel & 255u);
        }
        if (alpha) level.argb[here] = (((red + alpha / 2) / alpha) << 16) |
                                       (((green + alpha / 2) / alpha) << 8) |
                                       ((blue + alpha / 2) / alpha);
    }
}

ArgbMipLevel Downsample(std::span<const uint32_t> pixels, uint32_t width,
                        uint32_t height, bool avoidExactMagenta, bool bleedTransparentEdges) {
    ArgbMipLevel result;
    result.width = std::max(1u, width / 2);
    result.height = std::max(1u, height / 2);
    result.argb.resize(static_cast<size_t>(result.width) * result.height);
    for (uint32_t y = 0; y < result.height; ++y) {
        // Coordinates use integer units of 1/destinationExtent source texels.
        // This produces exact box-overlap weights even for 3->1 and 5->2.
        const uint64_t top = static_cast<uint64_t>(y) * height;
        const uint64_t bottom = static_cast<uint64_t>(y + 1) * height;
        const auto firstY = static_cast<uint32_t>(top / result.height);
        const auto endY = static_cast<uint32_t>((bottom + result.height - 1) / result.height);
        for (uint32_t x = 0; x < result.width; ++x) {
            const uint64_t left = static_cast<uint64_t>(x) * width;
            const uint64_t right = static_cast<uint64_t>(x + 1) * width;
            const auto firstX = static_cast<uint32_t>(left / result.width);
            const auto endX = static_cast<uint32_t>((right + result.width - 1) / result.width);
            uint64_t total = 0, alpha = 0, red = 0, green = 0, blue = 0;
            for (uint32_t sy = firstY; sy < endY; ++sy) {
                const uint64_t wy = std::min(bottom, static_cast<uint64_t>(sy + 1) * result.height) -
                                    std::max(top, static_cast<uint64_t>(sy) * result.height);
                for (uint32_t sx = firstX; sx < endX; ++sx) {
                    const uint64_t wx = std::min(right, static_cast<uint64_t>(sx + 1) * result.width) -
                                        std::max(left, static_cast<uint64_t>(sx) * result.width);
                    const uint64_t weight = wx * wy;
                    const uint32_t pixel = pixels[static_cast<size_t>(sy) * width + sx];
                    const uint64_t covered = weight * (pixel >> 24);
                    total += weight;
                    alpha += covered;
                    red += covered * ((pixel >> 16) & 255u);
                    green += covered * ((pixel >> 8) & 255u);
                    blue += covered * (pixel & 255u);
                }
            }
            const uint32_t a = static_cast<uint32_t>((alpha + total / 2) / total);
            uint32_t filtered = 0;
            if (a && alpha) {
                const auto r = static_cast<uint32_t>((red + alpha / 2) / alpha);
                const auto g = static_cast<uint32_t>((green + alpha / 2) / alpha);
                const auto b = static_cast<uint32_t>((blue + alpha / 2) / alpha);
                filtered = (a << 24) | (r << 16) | (g << 8) | b;
            }
            result.argb[static_cast<size_t>(y) * result.width + x] =
                EscapeMagenta(filtered, avoidExactMagenta);
        }
    }
    if (bleedTransparentEdges) {
        BleedTransparentRgb(result);
        if (avoidExactMagenta)
            for (auto& pixel : result.argb) pixel = EscapeMagenta(pixel, true);
    }
    return result;
}
} // namespace

bool DownsampleArgbMip(std::span<const uint32_t> pixels, uint32_t width,
                       uint32_t height, ArgbMipLevel& output,
                       std::string& error, bool avoidExactMagenta, bool bleedTransparentEdges) {
    error.clear();
    if (!ValidImage(pixels, width, height, error)) return false;
    if (width == 1 && height == 1) {
        error = "A 1x1 image has no smaller mip level.";
        return false;
    }
    try {
        auto result = Downsample(pixels, width, height, avoidExactMagenta, bleedTransparentEdges);
        output = std::move(result);
        return true;
    } catch (const std::bad_alloc&) {
        error = "Insufficient memory for mip level.";
    } catch (const std::length_error&) {
        error = "Mip level exceeds container limits.";
    }
    return false;
}

bool BuildArgbMipChain(std::span<const uint32_t> pixels, uint32_t width,
                       uint32_t height, std::vector<ArgbMipLevel>& output,
                       std::string& error, MipOptions options) {
    error.clear();
    if (!ValidImage(pixels, width, height, error)) return false;
    try {
        std::vector<ArgbMipLevel> chain;
        // uint32_t extents need at most 33 entries; supported extents need 15.
        chain.reserve(16);
        chain.push_back({width, height, std::vector<uint32_t>(pixels.begin(), pixels.end())});
        if (options.avoid_exact_magenta)
            for (auto& pixel : chain.front().argb) pixel = EscapeMagenta(pixel, true);
        while ((chain.back().width > 1 || chain.back().height > 1) &&
               (!options.max_levels || chain.size() < options.max_levels)) {
            const auto& level = chain.back();
            auto next = Downsample(level.argb, level.width, level.height,
                                   options.avoid_exact_magenta, options.bleed_transparent_edges);
            chain.push_back(std::move(next));
        }
        output = std::move(chain);
        return true;
    } catch (const std::bad_alloc&) {
        error = "Insufficient memory for mip chain.";
    } catch (const std::length_error&) {
        error = "Mip chain exceeds container limits.";
    }
    return false;
}

#ifdef _WIN32
HRESULT CreateManagedArgbMipTexture(IDirect3DDevice9* device,
                                    std::span<const uint32_t> pixels,
                                    uint32_t width, uint32_t height,
                                    IDirect3DTexture9** output,
                                    MipOptions options) {
    if (!device || !output || *output) return D3DERR_INVALIDCALL;
    std::vector<ArgbMipLevel> chain;
    std::string error;
    if (!BuildArgbMipChain(pixels, width, height, chain, error, options)) return D3DERR_INVALIDCALL;
    IDirect3DTexture9* texture = nullptr;
    HRESULT result = device->CreateTexture(width, height, static_cast<UINT>(chain.size()), 0,
                                           D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr);
    if (FAILED(result)) return result;
    if (!texture) return E_FAIL;
    for (UINT i = 0; i < static_cast<UINT>(chain.size()); ++i) {
        D3DLOCKED_RECT lock{};
        result = texture->LockRect(i, &lock, nullptr, 0);
        if (FAILED(result)) { texture->Release(); return result; }
        const auto& mip = chain[i];
        const size_t rowBytes = static_cast<size_t>(mip.width) * sizeof(uint32_t);
        if (!lock.pBits || lock.Pitch < 0 || static_cast<size_t>(lock.Pitch) < rowBytes) {
            texture->UnlockRect(i);
            texture->Release();
            return D3DERR_INVALIDCALL;
        }
        for (uint32_t y = 0; y < mip.height; ++y)
            std::memcpy(static_cast<unsigned char*>(lock.pBits) + static_cast<size_t>(y) * lock.Pitch,
                        mip.argb.data() + static_cast<size_t>(y) * mip.width, rowBytes);
        result = texture->UnlockRect(i);
        if (FAILED(result)) { texture->Release(); return result; }
    }
    *output = texture;
    return S_OK;
}
#endif

} // namespace gta2ceiling
