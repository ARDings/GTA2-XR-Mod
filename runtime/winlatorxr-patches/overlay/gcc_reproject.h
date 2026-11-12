// SPDX-License-Identifier: GPL-3.0-or-later
// GTA2Ceiling display-rate reprojection of the game image.
#pragma once
#include <openxr/openxr.h>
#ifdef __cplusplus
extern "C" {
#endif
// XR thread, GL context current, the game's swapchain image bound and drawn.
// Re-renders the shown game image (rendered from its slot pose) for the current
// display pose nowPose, treating the city as lying on the scanned ceiling or
// floor plane (or the round play field). This corrects head rotation AND head
// translation every display frame, however old the game image is.
// swapTexture: GL texture of the bound swapchain image (width x height).
// slotPoses: the host's eye poses of all 22 frame-sync slots (slot k = sync
// value 12k); the slot of the shown image is read from its own stamp,
// fallbackSlot (the host's sync reader) only when the stamp is unreadable.
// Returns 1 when the image now belongs to nowPose (submit the layer with it).
int GccReprojectApply(unsigned int swapTexture, int width, int height, XrSpace space, XrTime time,
                      const XrPosef slotPoses[2][22], int fallbackSlot, const XrPosef nowPose[2], XrFovf fov,
                      const int rect[2][4]);
// Counters since the last call, for the pacing log.
enum {
    GCC_REPROJECT_WARPED, GCC_REPROJECT_SKIPPED,
    GCC_REPROJECT_INTACT, GCC_REPROJECT_TORN, GCC_REPROJECT_STAMP_UNREADABLE,
    GCC_REPROJECT_SYNC_SAME, GCC_REPROJECT_SYNC_OTHER,
    GCC_REPROJECT_STAT_COUNT
};
void GccReprojectStats(int out[GCC_REPROJECT_STAT_COUNT]);
#ifdef __cplusplus
}
#endif
