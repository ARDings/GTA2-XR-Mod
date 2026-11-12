// SPDX-License-Identifier: GPL-3.0-or-later
// Host-side adaptation of GTA2Ceiling's MIT Scene diagnostic query logic.
#pragma once
#include "openxr.h"
#include <memory>
#include <string>

namespace gcc_host {
// All methods belong to the host's owning OpenXR thread. The host retains its
// instance/session; call Shutdown before destroying either. No Java, permissions,
// sockets, recentering or reference-space policy is hidden in this class.
class SceneQuery {
public:
    SceneQuery();
    ~SceneQuery();
    SceneQuery(const SceneQuery&) = delete;
    SceneQuery& operator=(const SceneQuery&) = delete;
    bool Initialize(XrInstance instance, XrSession session);
    void RequestRefresh();
    void OnEvent(const XrEventDataBuffer& event);
    void Tick(XrTime time);
    // Fresh locate calls relative to the exact supplied reference space. The
    // wrapper must attach its negotiated base-space identity and session epoch.
    // Invalid data never receives SUCCESS_CEILING_DATA or usable 3D vertices.
    std::string SnapshotJson(XrSpace base, XrTime time);
    // Heights (y) of the nearest CEILING above and FLOOR below headY in the
    // supplied gravity-aligned space. Bit 0: ceiling valid, bit 1: floor valid.
    int PlaneHeights(XrSpace space, XrTime time, float headY, float* ceilY, float* floorY,
                     float* outlineXZ = nullptr, int maxPoints = 0, int* pointCount = nullptr);
    const std::string& Status() const;
    void Shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
