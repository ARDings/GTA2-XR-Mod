#include "../src/mod/texture_filter.h"

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace gta2ceiling;
namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<ArgbMipLevel> Chain(const std::vector<uint32_t>& source,
                              uint32_t width, uint32_t height, MipOptions options = {}) {
    std::vector<ArgbMipLevel> result;
    std::string error;
    const bool built = BuildArgbMipChain(source, width, height, result, error, options);
    Require(built, error.c_str());
    Require(error.empty(), "Successful construction left an error");
    return result;
}

void CheckOpaqueAndLevelZero() {
    std::vector<uint32_t> checker(16);
    for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x)
        checker[y * 4 + x] = ((x + y) & 1) ? 0xffffffffu : 0xff000000u;
    const auto source = checker;
    const auto levels = Chain(checker, 4, 4);
    Require(checker == source && levels[0].argb == source, "Source or original level zero changed");
    Require(levels.size() == 3 && levels[1].width == 2 && levels[1].height == 2,
            "Wrong power-of-two mip dimensions");
    for (size_t i = 1; i < levels.size(); ++i)
        for (uint32_t pixel : levels[i].argb)
            Require(pixel == 0xff808080u, "Checkerboard did not become its rounded box average");
    const std::vector<uint32_t> one{0x17654321u};
    const auto singleton = Chain(one, 1, 1);
    Require(singleton.size() == 1 && singleton[0].argb == one, "1x1 original was not preserved");
}

void CheckAlphaEdges() {
    const auto red = Chain({0xffff0000u, 0x000000ffu, 0x000000ffu, 0x000000ffu}, 2, 2);
    Require(red.back().argb[0] == 0x40ff0000u, "Invisible blue leaked into opaque red edge");
    const auto mixed = Chain({0xffff0000u, 0x800000ffu, 0x0000ff00u, 0x0000ff00u}, 2, 2);
    Require(mixed.back().argb[0] == 0x60aa0055u, "Partial alpha was not used as RGB weight");
    const auto empty = Chain({0x00ff0000u, 0x000000ffu, 0x0000ff00u, 0x00ffffffu}, 2, 2);
    Require(empty.back().argb[0] == 0, "Fully transparent output retained coloured fringes");
}

void CheckMipEdgeBleed() {
    std::vector<uint32_t> cutout(16, 0x000000ffu);
    for (uint32_t y = 0; y < 4; ++y) cutout[y * 4] = cutout[y * 4 + 1] = 0xffff0000u;
    const auto plain = Chain(cutout, 4, 4);
    const auto bled = Chain(cutout, 4, 4, {.bleed_transparent_edges = true});
    Require(bled.front().argb == cutout, "Bleeding modified original level zero");
    Require(plain[1].argb[1] == 0 && bled[1].argb[1] == 0x00ff0000u,
            "Transparent lower-mip edge did not receive neighbouring red");
    for (size_t level = 0; level < plain.size(); ++level)
        for (size_t i = 0; i < plain[level].argb.size(); ++i) {
            Require((plain[level].argb[i] >> 24) == (bled[level].argb[i] >> 24),
                    "Edge dilation changed alpha coverage");
            if (plain[level].argb[i] >> 24)
                Require(plain[level].argb[i] == bled[level].argb[i], "Dilation changed visible RGB");
        }
    // A hardware bilinear midpoint between the two mip texels must retain
    // straight red while only alpha fades. Transparent black would halve RGB.
    const uint32_t filteredRed = (((bled[1].argb[0] >> 16) & 255u) +
                                  ((bled[1].argb[1] >> 16) & 255u)) / 2;
    Require(filteredRed == 255, "Bilinear edge still acquires a dark fringe");
    for (bool vertical : {false, true}) {
        const auto thin = Chain({0xffff0000u, 0xffff0000u, 0, 0},
                                vertical ? 1u : 4u, vertical ? 4u : 1u,
                                {.bleed_transparent_edges = true});
        Require(thin[1].argb[1] == 0x00ff0000u, "Thin mip skipped edge dilation");
    }
    const auto empty = Chain(std::vector<uint32_t>(16, 0x0000ff00u), 4, 4,
                             {.bleed_transparent_edges = true});
    Require(empty[1].argb == std::vector<uint32_t>(4, 0), "Empty image fabricated visible edge seeds");
}

void CheckOddAndThinImages() {
    const std::vector<uint32_t> thin{0xff000000u, 0xff000000u, 0xffff0000u, 0xff000000u, 0xff000000u};
    for (bool vertical : {false, true}) {
        const auto levels = Chain(thin, vertical ? 1u : 5u, vertical ? 5u : 1u);
        Require(levels.size() == 3 && levels[1].width == (vertical ? 1u : 2u) &&
                levels[1].height == (vertical ? 2u : 1u), "1xN or Nx1 chain dimensions wrong");
        Require(levels[1].argb == std::vector<uint32_t>({0xff330000u, 0xff330000u}),
                "Odd middle texel was lost or counted twice");
        Require(levels.back().argb[0] == 0xff330000u, "Thin image final average incorrect");
    }
    std::vector<uint32_t> odd(15, 0xff000000u);
    odd[4] = odd[9] = odd[14] = 0xffff0000u;
    const auto levels = Chain(odd, 5, 3);
    Require(levels[1].width == 2 && levels[1].height == 1, "NPOT dimensions do not match D3D mip rules");
    Require(levels[1].argb == std::vector<uint32_t>({0xff000000u, 0xff660000u}) &&
            levels.back().argb[0] == 0xff330000u, "Last odd column lost its area contribution");
    const auto three = Chain({0xff000000u, 0xff000000u, 0xffffffffu,
                              0xff000000u, 0xff000000u, 0xffffffffu,
                              0xff000000u, 0xff000000u, 0xffffffffu}, 3, 3);
    Require(three.back().argb[0] == 0xff555555u, "3x3->1x1 dropped the final row or column");
}

void CheckLimitsAndFailure() {
    const std::vector<uint32_t> source(32, 0xff123456u);
    const auto capped = Chain(source, 8, 4, {.max_levels = 2});
    Require(capped.size() == 2 && capped.back().width == 4 && capped.back().height == 2,
            "Maximum levels did not include original level zero");
    Require(Chain(source, 8, 4, {.max_levels = 1}).size() == 1, "Base-only cap ignored");
    Require(Chain(source, 8, 4, {.max_levels = 99}).size() == 4, "Large cap repeated the final 1x1 mip");
    const auto maxAxis = Chain(std::vector<uint32_t>(kMaxMipDimension, 0xffabcdefu), 1, kMaxMipDimension);
    Require(maxAxis.size() == 15 && maxAxis.back().width == 1 && maxAxis.back().height == 1 &&
            maxAxis.back().argb[0] == 0xffabcdefu, "Largest supported axis did not finish exactly at 1x1");

    std::vector<ArgbMipLevel> output{{1, 1, {0x11223344u}}};
    std::string error;
    const std::array<std::array<uint32_t, 2>, 5> bad{{
        {0, 1}, {1, 0}, {2, 3}, {kMaxMipDimension + 1, 1},
        {std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max()}
    }};
    for (const auto& dimensions : bad) {
        Require(!BuildArgbMipChain(source, dimensions[0], dimensions[1], output, error),
                "Invalid dimensions/count accepted");
        Require(!error.empty() && output.size() == 1 && output[0].argb[0] == 0x11223344u,
                "Failure did not preserve previous mip chain");
    }
    Require(!BuildArgbMipChain({}, 1, 1, output, error), "Empty source accepted");
    ArgbMipLevel mip{1, 1, {0x11223344u}};
    Require(!DownsampleArgbMip(std::span<const uint32_t>(source).first(1), 1, 1, mip, error) &&
            mip.argb[0] == 0x11223344u, "Terminal downsample changed output");
    Require(!DownsampleArgbMip({}, 2, 2, mip, error) && !error.empty(), "Empty downsample accepted");
#ifdef _WIN32
    IDirect3DTexture9* texture = nullptr;
    Require(CreateManagedArgbMipTexture(nullptr, source, 8, 4, &texture) == D3DERR_INVALIDCALL && !texture,
            "Null upload device did not fail without replacing output");
#endif
}

void CheckExplicitKeyEscape() {
    const std::vector<uint32_t> magenta(4, 0xffff00ffu);
    const auto normal = Chain(magenta, 2, 2);
    Require(normal.front().argb == magenta && normal.back().argb[0] == 0xffff00ffu,
            "Default filtering modified the key colour");
    const auto escaped = Chain(magenta, 2, 2, {.avoid_exact_magenta = true});
    for (const auto& level : escaped) for (uint32_t pixel : level.argb)
        Require(pixel == 0xffff01ffu, "Exact magenta escape changed wrong channel or alpha");
    const std::vector<uint32_t> nearColours{0x80ff00ffu, 0xffff01ffu, 0xfffe00ffu, 0x00ff00ffu};
    const auto nearChain = Chain(nearColours, 2, 2, {.avoid_exact_magenta = true});
    Require(nearChain[0].argb == std::vector<uint32_t>({0x80ff01ffu, 0xffff01ffu, 0xfffe00ffu, 0x00ff01ffu}) &&
            nearColours[0] == 0x80ff00ffu, "Key escape modified source, non-key colour, or alpha");
}
} // namespace

int main() {
    CheckOpaqueAndLevelZero();
    CheckAlphaEdges();
    CheckMipEdgeBleed();
    CheckOddAndThinImages();
    CheckLimitsAndFailure();
    CheckExplicitKeyEscape();
    std::cout << "ARGB mip filtering passed: box averages, alpha edges, odd/thin extents, limits, "
                 "unchanged originals and opt-in key escape. No GPU or headset quality claim.\n";
}
