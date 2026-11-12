// Windows x86 regression for the actual renderer lamp-generation function.
// Synthetic game fields only: no GTA2 binary, artwork, D3D device or headset.
// Link with live_geometry.cpp; unexpected calls into other passes fail fast.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "live_geometry.h"
#include "gibs.h"
#include "texture_store.h"
#include "../third_party/gta2-renderer/src/renderer.h"

namespace {
bool Near(float a, float b) { return std::fabs(a - b) < 0.0001f; }
[[noreturn]] void UnexpectedPass() {
    std::fputs("FAIL: lamp test entered an unrelated rendering pass.\n", stderr);
    std::abort();
}
}

namespace gta2 {
Mat4 Camera::ViewMatrix() const { UnexpectedPass(); }
Mat4 Camera::ProjectionMatrix(float) const { UnexpectedPass(); }
AlphaMode GetAlphaMode() { UnexpectedPass(); }
int GetAlphaRef() { UnexpectedPass(); }
}

namespace gta2dx9 {
// The test enters the lamp geometry after game-field collection. Any accidental
// access to game memory or another rendering pass must fail immediately.
const void* GameObjectBeingDrawn() { UnexpectedPass(); }
bool GroundSampler::SurfaceAt(float, float, float, float*) const { UnexpectedPass(); }
GroundPlane GroundSampler::Fit(const float*, const float*, int, float, float, float) const {
    UnexpectedPass();
}
bool GibsHideCurrentSprite() { UnexpectedPass(); }
float GibsCurrentScale() { UnexpectedPass(); }
bool GibsCurrentIsResting() { UnexpectedPass(); }
bool GameObjectIsCorpse() { UnexpectedPass(); }
bool GameObjectIsPowerUp() { UnexpectedPass(); }
void Log(const char*, ...) { UnexpectedPass(); }
IDirect3DTexture9* DeviceTextureFor(IDirect3DDevice9*, const void*, bool*, bool) { UnexpectedPass(); }
void NoteSpriteTexture(const void*) { UnexpectedPass(); }
bool IsGroundDecal(const void*) { UnexpectedPass(); }

struct LiveGeometryLampTestAccess {
    static void Run() {
        LiveGeometry geometry;
        LiveGeometry::Sprite body{};
        body.corners = 4;
        body.texture = reinterpret_cast<const void*>(1);
        for (int i = 0; i < 4; ++i) body.objectToWorld.m[i][i] = 1.0f;
        body.objectToWorld.m[3][0] = 7.0f;
        const float positions[4][4] = {
            {10, 20, 0, 0}, {14, 20, 1, 0},
            {14, 28, 1, 1}, {10, 28, 0, 1}
        };
        for (int i = 0; i < 4; ++i) {
            body.local[i] = {positions[i][0], 2.0f, positions[i][1],
                             0, 1, 0, positions[i][2], positions[i][3]};
        }
        geometry.sprites_.reserve(1);
        geometry.sprites_.push_back(body);
        assert(geometry.sprites_.size() == geometry.sprites_.capacity());
        const auto* previousStorage = geometry.sprites_.data();

        // The first of three lamps forces reallocation. The second and mirrored
        // third must still use the original body's corners and transform.
        constexpr uint32_t lit = (1u << 5) | (1u << 6) | (1u << 22);
        geometry.AddCarLampQuads(geometry.sprites_.front(), lit, 0);
        assert(geometry.sprites_.data() != previousStorage);
        assert(geometry.sprites_.size() == 4);
        assert(geometry.SpriteQuads() == 3);
        const float expected[3][4] = {
            {10.4f, 11.2f, 20.8f, 22.0f},
            {12.0f, 13.2f, 24.8f, 26.4f},
            {12.8f, 13.6f, 20.8f, 22.0f}
        };
        for (int n = 0; n < 3; ++n) {
            const auto& lamp = geometry.sprites_[static_cast<size_t>(n) + 1];
            assert(lamp.corners == 4 && lamp.texture == nullptr && lamp.lamp);
            assert(lamp.lampMirrored == (n == 2));
            assert(std::memcmp(&lamp.objectToWorld, &body.objectToWorld,
                               sizeof(body.objectToWorld)) == 0);
            const float xs[4] = {expected[n][0], expected[n][1], expected[n][1], expected[n][0]};
            const float zs[4] = {expected[n][2], expected[n][2], expected[n][3], expected[n][3]};
            for (int i = 0; i < 4; ++i) {
                assert(Near(lamp.local[i].x, xs[i]));
                assert(Near(lamp.local[i].y, 2.0f + kConformClearance));
                assert(Near(lamp.local[i].z, zs[i]));
            }
        }
    }
};
}

int main() {
    static_assert(sizeof(void*) == 4, "the GTA2 renderer ABI is x86");
    gta2dx9::LampArt first;
    first.x0 = 1; first.y0 = 2; first.width = 2; first.height = 3;
    first.spriteWidth = 10; first.spriteHeight = 20;
    first.argb.assign(6, 0xFFFFFFFF);
    gta2dx9::LampArt second;
    second.x0 = 5; second.y0 = 12; second.width = 3; second.height = 4;
    second.spriteWidth = 10; second.spriteHeight = 20;
    second.argb.assign(12, 0xFFFFFFFF);
    gta2dx9::SetCarLampArt(5, {first});
    gta2dx9::SetCarLampArt(6, {second});
    gta2dx9::LiveGeometryLampTestAccess::Run();
    std::puts("PASS: three lamp quads retain the car snapshot across vector reallocation.");
}
