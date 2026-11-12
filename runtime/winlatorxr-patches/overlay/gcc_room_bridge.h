// SPDX-License-Identifier: GPL-3.0-or-later
// Optional GTA2Ceiling host extension; stock XrAPI reference types are unchanged.
#pragma once
#include <jni.h>
#include <openxr/openxr.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef XrSpace (*GccResolveSpace)(int32_t index);
void GccRoomBridgeSetSceneExtensionsEnabled(int enabled);
void GccRoomBridgeInit(XrInstance instance, XrSession session, JNIEnv* env,
                       jobject activity, GccResolveSpace resolve);
void GccRoomBridgeFrame(JNIEnv* env, XrTime time, uint32_t hmd_sync, uint32_t recenter_count);
// XR-thread only, before ReferenceCounter::Publish and GccRoomBridgeFrame. Does
// not consume events: the matching Frame call advances the reference epoch.
int GccRoomBridgeReferenceChangeDue(XrTime time);
void GccRoomBridgeEvent(const XrEventDataBuffer* event);
void GccRoomBridgeInvalidate(void);
void GccRoomBridgeShutdown(JNIEnv* env);
// Nearest CEILING above / FLOOR below head_y in a gravity-aligned space.
// Returns bit 0 = ceiling valid, bit 1 = floor valid.
// outline_xz receives the room outline (walls) as x,z pairs, at most max_points.
int GccRoomBridgePlaneHeights(XrSpace space, XrTime time, float head_y, float* ceiling_y, float* floor_y,
                              float* outline_xz, int max_points, int* point_count);
// The game's real-world occlusion setting (default on).
int GccRoomBridgeOcclusionWanted(void);
// The game's round play field (circle/table) in the given space: centre x, y,
// centre z, radius. Returns 0 when the whole room surface is used.
int GccRoomBridgeField(XrSpace space, XrTime time, float* out4);
#ifdef __cplusplus
}
#endif
