// SPDX-License-Identifier: GPL-3.0-or-later
// GTA2Ceiling real-world occlusion (XR_META_environment_depth) for the game layer.
#pragma once
#include <openxr/openxr.h>
#ifdef __cplusplus
extern "C" {
#endif
void GccOcclusionSetExtensionEnabled(int enabled);
// XR thread, GL context current, the game's swapchain image bound and drawn.
// renderPose: the eye poses the shown game frame was rendered with (layer space).
// rect: per-eye {x, y, width, height} of the projection-layer sub-images.
void GccOcclusionApply(XrInstance instance, XrSession session, XrSpace space, XrTime time,
                       const XrPosef renderPose[2], XrFovf fov, const int rect[2][4]);
void GccOcclusionShutdown(void);
#ifdef __cplusplus
}
#endif
