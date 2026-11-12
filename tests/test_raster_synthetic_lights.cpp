// Production raster paths must not inspect a running GTA2 process. Deliberately
// runs without mapping any game globals; a raw-memory walk would fail here.
#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "synthetic_lights.h"
#include "remix_lights.h"

#ifndef GTA2CEILING_RASTER_ONLY
#error This regression must compile the production raster configuration.
#endif

namespace {
[[noreturn]] void UnexpectedLightingWork() {
    std::fputs("FAIL: standalone raster entered Remix lighting work.\n", stderr);
    std::abort();
}
}

namespace gta2dx9 {
void Log(const char*, ...) { UnexpectedLightingWork(); }
float DaylightGateFactor(const DaylightGate&) { UnexpectedLightingWork(); }
void LightsBeginExtra() { UnexpectedLightingWork(); }
void LightsSubmitExtra(const RemixLightDesc&) { UnexpectedLightingWork(); }
bool RemixApiAvailable() { UnexpectedLightingWork(); }
}

int main() {
    auto& settings = gta2dx9::SyntheticLightsSettings();
    settings.enabled = true;
    for (auto& category : settings.category) category.enabled = true;

    // Pass the old 400-frame automatic-probe threshold several times. Even a
    // manual probe request must remain inert in the raster executable.
    for (int i = 0; i < 1200; ++i) gta2dx9::SyntheticLightsUpdate();
    gta2dx9::SyntheticProbe("synthetic regression request");
    assert(!gta2dx9::SyntheticProbeHasRun());
    assert(!gta2dx9::SyntheticCarLampsReplaceQuads());
    assert(gta2dx9::SyntheticParticleTypes().empty());
    assert(gta2dx9::SyntheticVehicleModels().empty());
    const auto& stats = gta2dx9::SyntheticLightsStats();
    assert(stats.particlesWalked == 0 && stats.vehiclesWalked == 0);
    assert(!stats.particleListFound && !stats.vehicleListFound);
    for (int emitted : stats.emitted) assert(emitted == 0);
    std::puts("PASS: raster updates and explicit probe perform no game-memory or Remix work.");
}
