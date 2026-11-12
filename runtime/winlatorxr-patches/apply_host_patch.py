#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Apply the reviewed thin-host delta to one clean, pinned WinlatorXR checkout.

No assets, game data, binaries or dependencies are downloaded by this program.
Previous outputs are hash-checked before an idempotent refresh; unrelated edits fail closed.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess

COMMIT = "39b270c6401a69e7c28ea6ffd61de3dbac9e9b1e"
APP_ID = "com.gta2ceiling.winlator"
ROOT = Path(__file__).resolve().parent


def under_work_root(path):
    """Builds stay outside the source tree: under GTA2CEILING_WORK_ROOT when set, else on D:."""
    root = os.environ.get("GTA2CEILING_WORK_ROOT")
    if root:
        root = Path(root).resolve()
        return path == root or root in path.parents
    return path.drive.lower() == "d:"


def digest(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Expected one pinned source anchor: {old[:100]!r}")
    return text.replace(old, new, 1)


def apply(fork):
    fork = fork.resolve(strict=True)
    if not under_work_root(fork):
        raise ValueError("Build working copies must be under the work root (GTA2CEILING_WORK_ROOT or D:)")
    # This sparse source checkout intentionally omits large donor assets.
    # Missing source objects must fail instead of silently filling the disk with assets.
    os.environ["GIT_NO_LAZY_FETCH"] = "1"
    actual = subprocess.check_output(["git", "-C", str(fork), "rev-parse", "HEAD"], text=True).strip()
    if actual != COMMIT:
        raise ValueError(f"Wrong upstream commit: {actual}")
    state_path = fork / ".gcc-patch-state.json"
    previous = json.loads(state_path.read_text(encoding="utf-8")) if state_path.exists() else {}
    changes = {}

    def upstream(path):
        return subprocess.check_output(["git", "-C", str(fork), "show", f"{COMMIT}:{path}"]).decode("utf-8").replace("\r\n", "\n")

    def put(path, value, original=None):
        target = fork / path
        if target.exists():
            current = target.read_text(encoding="utf-8")
            if current != value and (original is None or current != original) and digest(current) != previous.get(path):
                raise ValueError(f"Refusing to overwrite an unrecognized edit: {target}")
        changes[path] = value

    def edit(path, fn):
        original = upstream(path)
        put(path, fn(original), original)

    def engine(s):
        s = replace_once(s, '#include "engine.h"', '#include "engine.h"\n#include "gcc_room_bridge.h"\n#include "gcc_occlusion.h"')
        s = replace_once(s, '    uint32_t available_count = 0;', '    GccRoomBridgeSetSceneExtensionsEnabled(0);\n    GccOcclusionSetExtensionEnabled(0);\n    uint32_t available_count = 0;')
        anchor = '            if (xrEnumerateInstanceExtensionProperties(NULL, available_count, &available_count, available) == XR_SUCCESS) {'
        addition = '''
                const char* scene_extensions[] = {
                    XR_FB_SPATIAL_ENTITY_EXTENSION_NAME,
                    XR_FB_SPATIAL_ENTITY_QUERY_EXTENSION_NAME,
                    XR_FB_SCENE_EXTENSION_NAME
                };
                bool scene_available = true;
                for (unsigned i = 0; i < 3; ++i)
                    scene_available = scene_available && XrEngineHasExtension(available, available_count, scene_extensions[i]);
                if (scene_available) {
                    for (unsigned i = 0; i < 3; ++i) extensions[count++] = scene_extensions[i];
                    GccRoomBridgeSetSceneExtensionsEnabled(1);
                }
                // Real-world occlusion of the game layer (Quest 3 depth sensor).
                if (XrEngineHasExtension(available, available_count, XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME)) {
                    extensions[count++] = XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME;
                    GccOcclusionSetExtensionEnabled(1);
                }
'''
        return replace_once(s, anchor, anchor + addition)
    edit("app/src/main/cpp/xr/engine.c", engine)

    def main(s):
        s = '#include "gcc_room_bridge.h"\n#include "gcc_occlusion.h"\n#include "gcc_reference_counter.h"\nstatic gcc_host::ReferenceCounter gcc_reference_counter;\n' + s
        anchor = '// The headset recenter only moves LOCAL'
        s = replace_once(s, anchor, 'static XrSpace gcc_room_space(int32_t id) { return direct_space(static_cast<uint64_t>(id)); }\n\n' + anchor)
        s = replace_once(s, '    XrDirectStart();', '    XrDirectStart();\n    GccRoomBridgeInit(xr_module_engine.Instance, xr_module_engine.Session, env, obj, gcc_room_space);')
        s = replace_once(s, '    XrRendererInit(&xr_module_engine, &xr_module_renderer);', '    XrRendererInit(&xr_module_engine, &xr_module_renderer);\n    gcc_reference_counter.Reset(xr_module_renderer.RecenterCount);')
        s = replace_once(s, '            recenter_stage_spaces();', '            recenter_stage_spaces();\n            GccRoomBridgeInvalidate();\n            gcc_reference_counter.MarkCoordinatesChanged();')
        s = replace_once(s, '    xr_spaces.clear();\n    XrRendererDestroy', '    GccOcclusionShutdown();\n    GccRoomBridgeShutdown(env);\n    xr_spaces.clear();\n    XrRendererDestroy')
        s = replace_once(s, '        XrRendererLockFrame(&xr_module_engine, &xr_module_renderer);', '        XrRendererLockFrame(&xr_module_engine, &xr_module_renderer);\n        if (GccRoomBridgeReferenceChangeDue(xr_module_engine.PredictedDisplayTime))\n            gcc_reference_counter.MarkCoordinatesChanged();\n        gcc_reference_counter.Publish(xr_module_renderer.RecenterCount);\n        GccRoomBridgeFrame(env, xr_module_engine.PredictedDisplayTime, xr_module_renderer.FrameSync, gcc_reference_counter.Value());')
        s = replace_once(s, '    data[count++] = xr_module_renderer.RecenterCount; //HMD_RECENTER', '    data[count++] = gcc_reference_counter.Value(); //HMD_RECENTER: same published generation as room replies')
        s = replace_once(s, '    double yaw = -ToRadians(xr_module_renderer.ConfigFloat[CONFIG_MENU_YAW]);', '    GccRoomBridgeInvalidate();\n    gcc_reference_counter.MarkCoordinatesChanged();\n    double yaw = -ToRadians(xr_module_renderer.ConfigFloat[CONFIG_MENU_YAW]);')
        s = replace_once(s, '    xr_colour_key = mode < 0 || mode > 4 ? 0 : mode;', '    xr_colour_key = mode < 0 || mode > 5 ? 0 : mode;')
        # The head pose the game renders with must be exactly the eye poses the
        # host stores for that frame's sync slot (what the reprojection and the
        # compositor reproject from). A second xrLocateViews a moment later
        # returns a refined prediction for the same display time - a small,
        # random rotation error on every game image, visible as judder of the
        # whole city whenever the head turns. Express the frame's own views in
        # the requested space through the (static) space-to-space transform.
        s = replace_once(s, """            OXR(xrLocateViews(xr_module_engine.Session, &projection_info, &view_state,
                              projection_capacity, &projection_count, projections));

            XrPosef pose;""", """            XrSpaceLocation gcc_rel = {XR_TYPE_SPACE_LOCATION};
            const XrSpaceLocationFlags gcc_need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
            if (XR_SUCCEEDED(xrLocateSpace(xr_module_engine.CurrentSpace, xr_spaces[space.second],
                                           xr_module_engine.PredictedDisplayTime, &gcc_rel)) &&
                (gcc_rel.locationFlags & gcc_need) == gcc_need) {
                const XrQuaternionf a = gcc_rel.pose.orientation;
                for (int eye = 0; eye < XrMaxNumEyes; ++eye) {
                    const XrPosef b = xr_module_renderer.Projections[eye].pose;
                    XrQuaternionf q;
                    q.w = a.w * b.orientation.w - a.x * b.orientation.x - a.y * b.orientation.y - a.z * b.orientation.z;
                    q.x = a.w * b.orientation.x + a.x * b.orientation.w + a.y * b.orientation.z - a.z * b.orientation.y;
                    q.y = a.w * b.orientation.y - a.x * b.orientation.z + a.y * b.orientation.w + a.z * b.orientation.x;
                    q.z = a.w * b.orientation.z + a.x * b.orientation.y - a.y * b.orientation.x + a.z * b.orientation.w;
                    const XrVector3f v = b.position;
                    const float tx = 2 * (a.y * v.z - a.z * v.y), ty = 2 * (a.z * v.x - a.x * v.z), tz = 2 * (a.x * v.y - a.y * v.x);
                    projections[eye].pose.orientation = q;
                    projections[eye].pose.position.x = gcc_rel.pose.position.x + v.x + a.w * tx + (a.y * tz - a.z * ty);
                    projections[eye].pose.position.y = gcc_rel.pose.position.y + v.y + a.w * ty + (a.z * tx - a.x * tz);
                    projections[eye].pose.position.z = gcc_rel.pose.position.z + v.z + a.w * tz + (a.x * ty - a.y * tx);
                    projections[eye].fov = xr_module_renderer.Projections[eye].fov;
                }
            } else {
                OXR(xrLocateViews(xr_module_engine.Session, &projection_info, &view_state,
                                  projection_capacity, &projection_count, projections));
            }

            XrPosef pose;""")
        # Eye buffers in the X screen's own aspect: GTA2Ceiling's 3072x1536 SBS
        # window lands 1:1 in two 1536x1536 eyes. The stock square buffer
        # stretched every eye 2x vertically (blurrier, and twice the pixels for
        # every pass that follows, reprojection and occlusion included).
        s = replace_once(s, '    xr_module_renderer.ConfigInt[CONFIG_VIEWPORT_HEIGHT] = width; //Use square resolution',
                         '    xr_module_renderer.ConfigInt[CONFIG_VIEWPORT_HEIGHT] = height; // GTA2Ceiling: 1:1 SBS eyes')
        # Quality sharpening by the compositor: the eye images are below the
        # panel resolution, and the menu text profits most.
        s = replace_once(s, '        xr_module_renderer.ConfigInt[CONFIG_SHARPENING] = xr_sharpening;',
                         '        xr_module_renderer.ConfigInt[CONFIG_SHARPENING] = 2; (void)xr_sharpening;')
        return s
    edit("app/src/main/cpp/xr/main.cpp", main)
    def renderer_native(s):
        s = replace_once('#include "gcc_room_bridge.h"\n#include "gcc_occlusion.h"\n#include "gcc_reproject.h"\n#include <time.h>\n'
                         'static float gcc_refresh_hz = 72.0f;  /* GTA2Ceiling: chosen display rate */\n'
                         '#include <dirent.h>\n#include <pthread.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <unistd.h>\n#include <sys/stat.h>\n' + s, '        switch (base_event_handler->type)',
                         '        GccRoomBridgeEvent(&event_data_bufer);\n        switch (base_event_handler->type)')
        # GTA2Ceiling: the emulated x86 game and the stereo renderer are CPU and
        # GPU bound; always request the boost performance level (measured
        # before: CPU level 3, GPU level 2 at 492 MHz).
        s = replace_once(s, '            int cpuLevel = renderer->ConfigInt[CONFIG_LEVEL_CPU];\n            int gpuLevel = renderer->ConfigInt[CONFIG_LEVEL_GPU];',
                         '            int cpuLevel = XR_PERF_SETTINGS_LEVEL_BOOST_EXT;\n            int gpuLevel = XR_PERF_SETTINGS_LEVEL_BOOST_EXT;')
        # The Quest resets the app's performance levels when a system overlay
        # (recording, the quick menu) takes focus: measured CPU level 8 before
        # a recording, 3 after it, and the game stayed juddery until restart.
        # Request the levels and the refresh rate again whenever focus returns.
        s = replace_once(s, """                    case XR_SESSION_STATE_FOCUSED:
                        renderer->SessionFocused = true;
                        break;""", """                    case XR_SESSION_STATE_FOCUSED:
                        renderer->SessionFocused = true;
                        if (renderer->SessionActive && engine->PlatformFlag[PLATFORM_EXTENSION_PERFORMANCE]) {
                            PFN_xrPerfSettingsSetPerformanceLevelEXT gccSetLevel = NULL;
                            xrGetInstanceProcAddr(engine->Instance, "xrPerfSettingsSetPerformanceLevelEXT",
                                                  (PFN_xrVoidFunction*)(&gccSetLevel));
                            if (gccSetLevel) {
                                gccSetLevel(engine->Session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT);
                                gccSetLevel(engine->Session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT);
                            }
                            if (pfnRequestDisplayRefreshRate) pfnRequestDisplayRefreshRate(engine->Session, gcc_refresh_hz);
                            __android_log_print(ANDROID_LOG_INFO, "GTA2Pacing", "focus back: performance levels and %.0f Hz requested again", gcc_refresh_hz);
                        }
                        break;""")
        # Real-world occlusion: after the game window reached the screen
        # swapchain, before it is released to the compositor.
        occlusion = '''extern struct XrEngine xr_module_engine;
/* GTA2Ceiling host diagnostics also go to a file next to the game's own log,
 * because the Quest system floods logcat and rotates them out within minutes. */
#include <stdarg.h>
static void GccHostLog(const char* tag, const char* format, ...)
{
    static FILE* file = NULL;
    static int opened = 0;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    __android_log_print(ANDROID_LOG_INFO, tag, "%s", text);
    pthread_mutex_lock(&lock);
    if (!opened) { opened = 1; file = fopen("/sdcard/Download/GTA2Ceiling/gta2/host_perf.log", "w"); }
    if (file) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        struct tm local;
        localtime_r(&now.tv_sec, &local);
        fprintf(file, "%02d:%02d:%02d.%03ld %s: %s\\n", local.tm_hour, local.tm_min, local.tm_sec, now.tv_nsec / 1000000, tag, text);
        fflush(file);
    }
    pthread_mutex_unlock(&lock);
}
/* GTA2Ceiling CPU diagnostics (logcat tag GTA2Cpu, every 5 s): CPU use of every
 * thread of this app and of its Wine/Box64 processes (same uid), the busiest
 * threads by name, and the available memory. 100% = one full core. */
struct GccTask { int tid; unsigned long long ticks; char name[24]; char proc[24]; };
/* Memory of one of our processes: resident, swapped out, and the dmabufs it
 * holds open (graphics buffers - not in RSS). Which one grows is the leak. */
static long GccDmabufKb(const char* pid)
{
    char path[96], target[160], line[96];
    snprintf(path, sizeof path, "/proc/%s/fd", pid);
    DIR* fds = opendir(path);
    if (!fds) return -1;
    long kb = 0;
    int guard = 0;
    struct dirent* d;
    while ((d = readdir(fds)) && guard++ < 8192) {
        if (d->d_name[0] < '0' || d->d_name[0] > '9') continue;
        snprintf(path, sizeof path, "/proc/%s/fd/%s", pid, d->d_name);
        const ssize_t n = readlink(path, target, sizeof target - 1);
        if (n <= 0) continue;
        target[n] = 0;
        if (!strstr(target, "dmabuf")) continue;
        snprintf(path, sizeof path, "/proc/%s/fdinfo/%s", pid, d->d_name);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        while (fgets(line, sizeof line, f)) {
            unsigned long size = 0;
            if (sscanf(line, "size: %lu", &size) == 1) { kb += (long)(size / 1024); break; }
        }
        fclose(f);
    }
    closedir(fds);
    return kb;
}
static long GccReadLong(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    long value = -1;
    if (fscanf(f, "%ld", &value) != 1) value = -1;
    fclose(f);
    return value;
}
static void GccCpuReport(double seconds)
{
    enum { kMax = 1536 };
    static struct GccTask prev[kMax], cur[kMax];
    static int prevCount = 0;
    int count = 0;
    char rssName[3][24] = {"", "", ""};
    long rssMb[3] = {0, 0, 0}, swapMb[3] = {0, 0, 0}, bufMb[3] = {0, 0, 0}, totalMb[3] = {0, 0, 0};
    const uid_t me = getuid();
    DIR* procs = opendir("/proc");
    if (!procs) return;
    struct dirent* p;
    while ((p = readdir(procs)) && count < kMax) {
        if (p->d_name[0] < '0' || p->d_name[0] > '9') continue;
        char path[96], proc[24] = "?", buf[512];
        struct stat st;
        snprintf(path, sizeof path, "/proc/%s", p->d_name);
        if (stat(path, &st) || st.st_uid != me) continue;
        snprintf(path, sizeof path, "/proc/%s/stat", p->d_name);
        FILE* f = fopen(path, "r");
        if (f) {
            const size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
            char* l = strchr(buf, '('); char* r = strrchr(buf, ')');
            if (l && r && r > l) { size_t len = (size_t)(r - l - 1); if (len > 23) len = 23; memcpy(proc, l + 1, len); proc[len] = 0; }
        }
        snprintf(path, sizeof path, "/proc/%s/statm", p->d_name);
        f = fopen(path, "r");
        if (f) {
            long size = 0, resident = 0;
            if (fscanf(f, "%ld %ld", &size, &resident) == 2) {
                const long mb = resident * (sysconf(_SC_PAGESIZE) / 1024) / 1024;
                long swapKb = 0;
                char statusPath[96], line[128];
                snprintf(statusPath, sizeof statusPath, "/proc/%s/status", p->d_name);
                FILE* status = fopen(statusPath, "r");
                if (status) {
                    while (fgets(line, sizeof line, status)) if (sscanf(line, "VmSwap: %ld kB", &swapKb) == 1) break;
                    fclose(status);
                }
                const long buf = mb > 20 || swapKb > 20 * 1024 ? GccDmabufKb(p->d_name) : 0;
                const long total = mb + swapKb / 1024 + (buf > 0 ? buf / 1024 : 0);
                for (int r = 0; r < 3; ++r) if (total > totalMb[r]) {
                    for (int m = 2; m > r; --m) {
                        rssMb[m] = rssMb[m - 1]; swapMb[m] = swapMb[m - 1]; bufMb[m] = bufMb[m - 1];
                        totalMb[m] = totalMb[m - 1]; memcpy(rssName[m], rssName[m - 1], 24);
                    }
                    rssMb[r] = mb; swapMb[r] = swapKb / 1024; bufMb[r] = buf / 1024; totalMb[r] = total;
                    memcpy(rssName[r], proc, 24);
                    break;
                }
            }
            fclose(f);
        }
        snprintf(path, sizeof path, "/proc/%s/task", p->d_name);
        DIR* tasks = opendir(path);
        if (!tasks) continue;
        struct dirent* t;
        while ((t = readdir(tasks)) && count < kMax) {
            if (t->d_name[0] < '0' || t->d_name[0] > '9') continue;
            snprintf(path, sizeof path, "/proc/%s/task/%s/stat", p->d_name, t->d_name);
            FILE* s = fopen(path, "r");
            if (!s) continue;
            const size_t n = fread(buf, 1, sizeof buf - 1, s); fclose(s); buf[n] = 0;
            char* l = strchr(buf, '('); char* r = strrchr(buf, ')');
            if (!l || !r || r <= l) continue;
            struct GccTask* k = &cur[count];
            size_t len = (size_t)(r - l - 1); if (len > 23) len = 23;
            memcpy(k->name, l + 1, len); k->name[len] = 0;
            memcpy(k->proc, proc, sizeof k->proc);
            unsigned long ut = 0, sy = 0;
            if (sscanf(r + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &sy) != 2) continue;
            k->tid = atoi(t->d_name);
            k->ticks = (unsigned long long)ut + sy;
            ++count;
        }
        closedir(tasks);
    }
    closedir(procs);
    const double hz = (double)sysconf(_SC_CLK_TCK);
    int top[8]; double topPct[8]; int topCount = 0;
    double total = 0;
    for (int i = 0; i < count; ++i) {
        unsigned long long before = 0;  /* a new thread counts from its start */
        for (int j = 0; j < prevCount; ++j) if (prev[j].tid == cur[i].tid) { before = prev[j].ticks; break; }
        const double pct = cur[i].ticks >= before ? (double)(cur[i].ticks - before) / (hz * seconds) * 100.0 : 0.0;
        total += pct;
        if (topCount == 8 && pct <= topPct[7]) continue;
        int at = topCount < 8 ? topCount++ : 7;
        while (at > 0 && topPct[at - 1] < pct) { top[at] = top[at - 1]; topPct[at] = topPct[at - 1]; --at; }
        top[at] = i; topPct[at] = pct;
    }
    if (prevCount) {
        long availableKb = -1, anonKb = -1, shmemKb = -1, swapTotalKb = -1, swapFreeKb = -1;
        FILE* m = fopen("/proc/meminfo", "r");
        if (m) {
            char line[128];
            while (fgets(line, sizeof line, m)) {
                sscanf(line, "MemAvailable: %ld kB", &availableKb);
                sscanf(line, "AnonPages: %ld kB", &anonKb);
                sscanf(line, "Shmem: %ld kB", &shmemKb);
                sscanf(line, "SwapTotal: %ld kB", &swapTotalKb);
                sscanf(line, "SwapFree: %ld kB", &swapFreeKb);
            }
            fclose(m);
        }
        const long gpuBytes = GccReadLong("/sys/class/kgsl/kgsl/page_alloc");
        char text[1024]; int used = snprintf(text, sizeof text,
            "%.0fs: total %.0f%% in %d threads, %ld MB available (anon %ld, shmem %ld, swap used %ld, gpu %ld MB) "
            "(rss/swap/dmabuf: %s %ld/%ld/%ld MB, %s %ld/%ld/%ld MB, %s %ld/%ld/%ld MB) |",
            seconds, total, count, availableKb / 1024, anonKb / 1024, shmemKb / 1024, (swapTotalKb - swapFreeKb) / 1024,
            gpuBytes >= 0 ? gpuBytes / (1024 * 1024) : -1,
            rssName[0], rssMb[0], swapMb[0], bufMb[0], rssName[1], rssMb[1], swapMb[1], bufMb[1],
            rssName[2], rssMb[2], swapMb[2], bufMb[2]);
        for (int i = 0; i < topCount && used < (int)sizeof text - 64; ++i)
            used += snprintf(text + used, sizeof text - used, " %.0f%% %s/%s", topPct[i], cur[top[i]].proc, cur[top[i]].name);
        GccHostLog("GTA2Cpu", "%s", text);
    }
    memcpy(prev, cur, sizeof(struct GccTask) * (size_t)count);
    prevCount = count;
}
/* Own low-priority thread: reading /proc must never cost the XR thread a frame. */
static void* GccCpuThread(void* unused)
{
    (void)unused;
    struct timespec last, now;
    clock_gettime(CLOCK_MONOTONIC, &last);
    for (;;) {
        sleep(5);
        clock_gettime(CLOCK_MONOTONIC, &now);
        GccCpuReport((now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9);
        last = now;
    }
    return NULL;
}
static void GccCpuStart(void)
{
    static int started = 0;
    if (started) return;
    started = 1;
    pthread_t thread;
    if (pthread_create(&thread, NULL, GccCpuThread, NULL) == 0) pthread_detach(thread);
}
/* GTA2Ceiling pacing diagnostics (logcat tag GTA2Pacing, once per second):
 * how old the shown game image is, in display frames, and how often a new
 * game image arrives. Age jumping 1-2-1-3 means visible judder. */
static void GccPacingStats(struct XrRenderer* renderer)
{
    static int sync = -1, lastSync = -1, frames = 0, newImages = 0, ageHist[8] = {0}, repeats = 0, maxRepeat = 0;
    static struct timespec start = {0, 0};
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (start.tv_sec == 0) start = now;
    if (renderer->ConfigInt[CONFIG_FRAMESYNC_G] < 1 && renderer->ConfigInt[CONFIG_FRAMESYNC_A] > 0)
        sync = renderer->ConfigInt[CONFIG_FRAMESYNC_R];
    ++frames;
    if (sync >= 0) {
        if (sync != lastSync) { ++newImages; repeats = 0; lastSync = sync; }
        else if (++repeats > maxRepeat) maxRepeat = repeats;
        const int span = (XrMaxFrameSync + XrFrameSyncStep - 1) / XrFrameSyncStep * XrFrameSyncStep; /* 22 slots: 0..252 */
        const int age = ((renderer->FrameSync - sync) % span + span) % span / XrFrameSyncStep;
        ageHist[age > 7 ? 7 : age]++;

    }
    const double elapsed = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
    if (elapsed >= 1.0) {
        int r[GCC_REPROJECT_STAT_COUNT];
        GccReprojectStats(r);
        GccCpuStart();
        GccHostLog("GTA2Pacing",
            "1s: %d display frames, %d new game images, longest repeat %d, image age 0:%d 1:%d 2:%d 3:%d 4:%d 5:%d 6:%d 7+:%d, reprojected %d, not %d | arriving images intact %d torn %d unreadable %d | sync reader same %d other %d",
            frames, newImages, maxRepeat, ageHist[0], ageHist[1], ageHist[2], ageHist[3], ageHist[4],
            ageHist[5], ageHist[6], ageHist[7], r[GCC_REPROJECT_WARPED], r[GCC_REPROJECT_SKIPPED],
            r[GCC_REPROJECT_INTACT], r[GCC_REPROJECT_TORN], r[GCC_REPROJECT_STAMP_UNREADABLE],
            r[GCC_REPROJECT_SYNC_SAME], r[GCC_REPROJECT_SYNC_OTHER]);
        frames = newImages = maxRepeat = 0;
        for (int i = 0; i < 8; ++i) ageHist[i] = 0;
        start = now;
    }
}
/* GTA2Ceiling display-rate reprojection: 1 when this frame's game image was
 * re-rendered for the current pose (the layer is then submitted with it). */
static int gcc_warped = 0;
static int GccShownSlot(struct XrRenderer* renderer)
{
    static int sync = 0;
    if (renderer->ConfigInt[CONFIG_FRAMESYNC_G] < 1 && renderer->ConfigInt[CONFIG_FRAMESYNC_A] > 0)
        sync = renderer->ConfigInt[CONFIG_FRAMESYNC_R];
    return renderer->ConfigInt[CONFIG_FRAMESYNC] ? sync : renderer->FrameSync;
}
static void GccRendererWarp(struct XrRenderer* renderer)
{
    gcc_warped = 0;
    if (!renderer->ConfigInt[CONFIG_VR] || !renderer->ConfigInt[CONFIG_FRAMESYNC] || !renderer->ConfigInt[CONFIG_SBS]) return;
    if (renderer->ConfigInt[CONFIG_AER]) return;
    struct XrFramebuffer* framebuffer = &renderer->Framebuffer[0];
    if (!framebuffer->Acquired) return;
    const int frame = GccShownSlot(renderer);
    if (frame < 0 || frame > XrMaxFrameSync) return;
    const int now = renderer->FrameSync;
    static XrPosef slots[2][22];
    for (int eye = 0; eye < 2; ++eye)
        for (int k = 0; k < 22; ++k) slots[eye][k] = renderer->InvertedViewPose[eye][k * XrFrameSyncStep];
    XrPosef current[2] = {renderer->InvertedViewPose[0][now], renderer->InvertedViewPose[1][now]};
    const float fovx = ToRadians(renderer->ConfigFloat[CONFIG_VIEWPORT_FOVX]);
    const float fovy = ToRadians(renderer->ConfigFloat[CONFIG_VIEWPORT_FOVY]);
    XrFovf fov;
    fov.angleLeft = -fovx / 2.0f; fov.angleRight = fovx / 2.0f;
    fov.angleUp = fovy / 2.0f; fov.angleDown = -fovy / 2.0f;
    const int w = renderer->ConfigInt[CONFIG_VIEWPORT_WIDTH] / 2;
    const int h = renderer->ConfigInt[CONFIG_VIEWPORT_HEIGHT];
    const int rect[2][4] = {{0, 0, w, h}, {w, 0, w, h}};
    XrFramebufferSetCurrent(framebuffer);
    const GLuint texture = ((XrSwapchainImageOpenGLESKHR*)framebuffer->SwapchainImage)[framebuffer->SwapchainIndex].image;
    gcc_warped = GccReprojectApply(texture, framebuffer->Width, framebuffer->Height, xr_module_engine.CurrentSpace,
                                   xr_module_engine.PredictedDisplayTime, (const XrPosef(*)[22])slots,
                                   frame / XrFrameSyncStep, current, fov, rect);
}
static void GccRendererOcclusion(struct XrRenderer* renderer)
{
    if (!renderer->ConfigInt[CONFIG_VR] || !renderer->ConfigInt[CONFIG_PASSTHROUGH]) return;
    struct XrFramebuffer* framebuffer = &renderer->Framebuffer[0];
    if (!framebuffer->Acquired) return;
    // Same pose the projection layer will use: the current one when the image
    // was reprojected, otherwise the pose the game rendered with.
    const int frame = gcc_warped ? renderer->FrameSync : GccShownSlot(renderer);
    if (frame < 0 || frame > XrMaxFrameSync) return;
    XrPosef poses[2] = {renderer->InvertedViewPose[0][frame], renderer->InvertedViewPose[1][frame]};
    const float fovx = ToRadians(renderer->ConfigFloat[CONFIG_VIEWPORT_FOVX]);
    const float fovy = ToRadians(renderer->ConfigFloat[CONFIG_VIEWPORT_FOVY]);
    XrFovf fov;
    fov.angleLeft = -fovx / 2.0f; fov.angleRight = fovx / 2.0f;
    fov.angleUp = fovy / 2.0f; fov.angleDown = -fovy / 2.0f;
    int w = renderer->ConfigInt[CONFIG_VIEWPORT_WIDTH];
    const int h = renderer->ConfigInt[CONFIG_VIEWPORT_HEIGHT];
    const int sbs = renderer->ConfigInt[CONFIG_SBS];
    if (sbs) w /= 2;
    const int rect[2][4] = {{0, 0, w, h}, {sbs ? w : 0, 0, w, h}};
    XrFramebufferSetCurrent(framebuffer);
    GccOcclusionApply(xr_module_engine.Instance, xr_module_engine.Session, xr_module_engine.CurrentSpace,
                      xr_module_engine.PredictedDisplayTime, poses, fov, rect);
}

void XrRendererEndFrame(struct XrRenderer* renderer)
{
    int fbo_index = renderer->ConfigInt[CONFIG_CURRENT_FBO];
    if (fbo_index == 0) GccPacingStats(renderer);
    if (fbo_index == 0) GccRendererWarp(renderer);
    if (fbo_index == 0) GccRendererOcclusion(renderer);'''
        s = replace_once(s, '''void XrRendererEndFrame(struct XrRenderer* renderer)
{
    int fbo_index = renderer->ConfigInt[CONFIG_CURRENT_FBO];''', occlusion)
        # Display rate: the host reprojects every display frame, so the rate only
        # has to be one the CPU sustains. 90 Hz was measured at 52-59 fps (CPU at
        # 100%, stale frames), so 72 Hz; 60 Hz (exactly 2 per game frame) when
        # the runtime offers it.
        s = replace_once(s, '            OXR(pfnRequestDisplayRefreshRate(engine->Session, (float)refresh));',
                         """            (void)refresh;
            {
                float chosen = 72.0f;
                PFN_xrEnumerateDisplayRefreshRatesFB pfnEnumerate = NULL;
                uint32_t rateCount = 0;
                float rates[32];
                xrGetInstanceProcAddr(engine->Instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction*)(&pfnEnumerate));
                if (pfnEnumerate && XR_SUCCEEDED(pfnEnumerate(engine->Session, 0, &rateCount, NULL)) && rateCount <= 32 &&
                    XR_SUCCEEDED(pfnEnumerate(engine->Session, rateCount, &rateCount, rates))) {
                    for (uint32_t i = 0; i < rateCount; ++i) if (fabsf(rates[i] - 60.0f) < 0.5f) chosen = rates[i];
                }
                __android_log_print(ANDROID_LOG_INFO, "GTA2Pacing", "display refresh %.0f Hz (%u rates offered)", chosen, rateCount);
                gcc_refresh_hz = chosen;
                OXR(pfnRequestDisplayRefreshRate(engine->Session, chosen));
            }""")
        # Each eye texture was rendered from its own physical eye position.
        # The compositor must reproject it from that same eye's saved pose.
        # A reprojected image belongs to the current pose.
        return replace_once(s, 'XrPosef pose = renderer->InvertedViewPose[0][frame];',
                            'XrPosef pose = renderer->InvertedViewPose[eye][gcc_warped ? renderer->FrameSync : frame];')
    edit("app/src/main/cpp/xr/renderer.c", renderer_native)
    exact_key_glsl = 'if (keyMode == 5) return max(abs(c.r - 1.0), max(abs(c.g), abs(c.b - 1.0))) < (0.5 / 255.0) ? 0.0 : 1.0;'
    def direct_key(s):
        s = replace_once(s, '    "    if (keyMode == 0) return 1.0;\\n" \\\n', '    "    if (keyMode == 0) return 1.0;\\n" \\\n    "    ' + exact_key_glsl + '\\n" \\\n')
        return replace_once(s, '    "    color = vec4(c, keyAlpha(c));\\n"',
                            '    "    float a = keyAlpha(c);\\n"\n    "    color = vec4(keyMode == 5 ? c * a : c, a);\\n"')
    edit("app/src/main/cpp/xr/direct.c", direct_key)

    def window_key(s):
        s = replace_once(s, 'public class WindowMaterial extends ShaderMaterial {',
                         'public class WindowMaterial extends ShaderMaterial {\n' +
                         (ROOT / "overlay/WindowMaterial.methods.java").read_text(encoding="utf-8"))
        s = replace_once(s, '"keyMode", "keyThreshold");', '"keyMode", "keyThreshold", "gccTextureSize");')
        s = replace_once(s, '    public void setColourKey(int mode, float threshold) {',
                         '    public void setColourKey(int mode, float threshold) {\n        gccExactKey = mode == 5;')
        s = replace_once(s, '"precision mediump float;\\n"', '"precision highp float;\\n"')
        s = replace_once(s, '"uniform sampler2D texture;\\n" +',
                         '"uniform sampler2D texture;\\n" +\n            "uniform vec2 gccTextureSize;\\n" +')
        s = replace_once(s, '                "if (keyMode == 0) return 1.0;\\n" +',
                         '                "if (keyMode == 0) return 1.0;\\n" +\n                "' + exact_key_glsl + '\\n" +')
        shader = (ROOT / "overlay/gcc_exact_key.glsl").read_text(encoding="utf-8")
        s = replace_once(s, '            "void main() {\\n" +\n                "vec3 color = texture2D(texture, vUV).rgb;\\n" +',
                         '            ' + json.dumps(shader) + ' +\n' +
                         '            "void main() {\\n" +\n' +
                         '                "if (keyMode == 5) { gl_FragColor = gccExactKeyBilinear(vUV); return; }\\n" +\n' +
                         '                "vec3 color = texture2D(texture, vUV).rgb;\\n" +')
        return s
    edit("app/src/main/java/com/winlator/cmod/renderer/material/WindowMaterial.java", window_key)

    def renderer_key(s):
        s = replace_once(s, '            renderTexture(texture, material);',
                         '            if (material instanceof WindowMaterial) ((WindowMaterial) material).setSourceSize(drawable.width, drawable.height);\n            renderTexture(texture, material);')
        old = '''        GLES20.glUniform1fv(material.getUniformLocation("xform"), tmpXForm1.length, tmpXForm1, 0);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, quadVertices.count());
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, 0);'''
        new = '''        GLES20.glUniform1fv(material.getUniformLocation("xform"), tmpXForm1.length, tmpXForm1, 0);
        WindowMaterial gccWindow = material instanceof WindowMaterial ? (WindowMaterial) material : null;
        if (gccWindow != null) gccWindow.beginExactKeySampling();
        try {
            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, quadVertices.count());
        } finally {
            if (gccWindow != null) gccWindow.endExactKeySampling();
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, 0);
        }'''
        return replace_once(s, old, new)
    edit("app/src/main/java/com/winlator/cmod/renderer/GLRenderer.java", renderer_key)

    for filename in ("gcc_room_bridge.h", "gcc_room_bridge.cpp", "gcc_scene_query.h", "gcc_scene_query.cpp", "gcc_reference_counter.h", "gcc_private_token.h", "gcc_occlusion.h", "gcc_occlusion.cpp", "gcc_reproject.h", "gcc_reproject.cpp"):
        put("app/src/main/cpp/xr/" + filename, (ROOT / "overlay" / filename).read_text(encoding="utf-8"))

    def evshim(s):
        s = replace_once(s, '/data/data/com.winlator.cmod/files/imagefs/tmp/gamepad%s.mem',
                         '/data/data/' + APP_ID + '/files/imagefs/tmp/gamepad%s.mem')
        # A regular shared-memory file is a snapshot, not a stream. read() would
        # advance to EOF after the initial state and lose subsequent input.
        return replace_once(s, 'ssize_t n = read(fd, &cur, sizeof cur);',
                            'ssize_t n = pread(fd, &cur, sizeof cur, 0);')
    edit("app/src/main/cpp/winlator/evshim.c", evshim)

    def bionic_launcher(s):
        s = replace_once(s, '        String evshimPath = imageFs.getLibDir() + "/libevshim.so";', '''        // ARM64EC uses our source-built shim with this app's private memory path.
        // Never preload the donor ImageFS shim, which targets the donor package.
        String evshimPath = wineInfo.isArm64EC()
                ? context.getApplicationInfo().nativeLibraryDir + "/libevshim.so" : "";
        if (evshimPath.isEmpty()) Log.w("GTA2Ceiling", "Gamepad shim requires the supported ARM64EC runtime");''')
        return replace_once(s, '        ld_preload += ":" + evshimPath;',
                            '        if (!evshimPath.isEmpty()) ld_preload += ":" + evshimPath;')
    edit("app/src/main/java/com/winlator/cmod/xenvironment/components/BionicProgramLauncherComponent.java", bionic_launcher)

    def framesync(s):
        # GTA2 stamps the host slot as an exact 8-bit red value (G=B=0). Upstream
        # learns a colour mapping from the first 22 distinct red values it sees;
        # GTA2's untracked frames are transparent magenta (R=255), which entered
        # that mapping, shifted slots and repeatedly reset it. Each reset froze the
        # reprojection slot for ~300 ms: smooth when still, judder on head motion.
        s = replace_once(s, '    private int lastFrameSync = 0;\n',
                         '    private int lastFrameSync = 0;\n'
                         '    private Boolean gccExactSync = null;\n')
        anchor = '''        int a = buffer.get(3) & 0xFF;
'''
        exact = '''        int a = buffer.get(3) & 0xFF;

        if (gccExactSync == null) {
            gccExactSync = com.winlator.cmod.core.SessionSettings.getBoolean(context, "gcc_exact_colour_key", false);
            if (gccExactSync) android.util.Log.i("GTA2Ceiling", "exact frame sync decoding active");
        }
        if (gccExactSync) {
            // Only an exact slot value is a pose reference. Anything else (keyed
            // background, menus, a torn read) keeps the previous slot unchanged.
            aerShouldUpdate = false;
            if (g != 0 || b != 0 || a == 0 || r > 252 || r % 12 != 0) return;
            if (lastFrameSync != r) currentFrames++;
            long now = System.currentTimeMillis();
            if (now > lastTimestamp + 1000) {
                lastTimestamp = now;
                lastFPS = currentFrames;
                currentFrames = 0;
            }
            callback.setFramesync(r, 0, 0, a);
            aerShouldUpdate = lastFrameSync != r;
            aerTargetFBO = 0;
            lastFrameSync = r;
            return;
        }
'''
        return replace_once(s, anchor, exact)
    edit("app/src/main/java/com/winlator/xr/api/XrFramesync.java", framesync)

    def xr_renderer(s):
        # Read the sync pixel only from the window the game presents to (the
        # topmost renderable one, as postWindows/vrWindowOnTop use it), never from
        # a desktop or helper window drawn earlier in the same frame.
        old = '        if (XrActivity.isEnabled(null) && XrActivity.isVR && vrWindowOnTop && xrFrameReady && drawable.id != 0) {'
        s = replace_once(s, old, old[:-3] + ' && gccIsSyncSource(drawable)) {')
        # Same 1:1 eye size as the native side (see main.cpp).
        s = replace_once(s, '            height = width; ////Use square resolution\n',
                         '            // GTA2Ceiling: keep the X screen height (1:1 SBS eyes, see main.cpp)\n')
        anchor = '    @Override\n    protected boolean preDrawable(ShaderMaterial material, Drawable drawable) {'
        method = '''    private boolean gccIsSyncSource(Drawable drawable) {
        int top = renderableWindows.size() - 1;
        while (top > 0 && renderableWindows.get(top).content.width <= 1 && renderableWindows.get(top).content.height <= 1) top--;
        return top < 0 || renderableWindows.get(top).content == drawable;
    }

'''
        return replace_once(s, anchor, method + anchor)
    edit("app/src/main/java/com/winlator/xr/io/XrRenderer.java", xr_renderer)

    java_methods = (ROOT / "overlay" / "XrActivity.methods.java").read_text(encoding="utf-8")
    def activity(s):
        s = replace_once(s, 'public class XrActivity extends XServerDisplayActivity {', 'public class XrActivity extends XServerDisplayActivity {\n' + java_methods)
        return replace_once(s, '        colourKeyTolerance =', '        if (SessionSettings.getBoolean(this, "gcc_exact_colour_key", false)) colourKey = 5;\n        colourKeyTolerance =')
    edit("app/src/main/java/com/winlator/xr/XrActivity.java", activity)
    def controller(s):
        anchor = '            }\n        } catch (Exception e) {\n            e.printStackTrace();\n        }\n    }'
        automatic = '''            }
            // GTA2Ceiling: player 1 gets the connected controller by itself when it
            // has none - nothing assigned yet, or the assigned one is not connected
            // (another controller than last time). An Xbox controller first (by
            // vendor or name, third-party ones included), otherwise the only
            // controller there is. Several candidates need the user's choice; a
            // disabled slot stays disabled.
            if (enabledSlots[0] && getAssignedDeviceForSlot(0) == null) {
                InputDevice xbox = null, only = null;
                int xboxes = 0, free = 0;
                for (InputDevice candidate : detectedDevices) {
                    if (getSlotForDevice(candidate.getId()) >= 0) continue;
                    String name = candidate.getName() == null ? "" : candidate.getName().toLowerCase(java.util.Locale.ROOT);
                    ++free; only = candidate;
                    if (candidate.getVendorId() == 0x045e || name.contains("xbox")) { ++xboxes; xbox = candidate; }
                }
                InputDevice pick = xboxes == 1 ? xbox : (xboxes == 0 && free == 1 ? only : null);
                if (pick != null) {
                    android.util.Log.i("GTA2Ceiling", "player 1: " + pick.getName());
                    assignDeviceToSlot(0, pick);
                }
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
    }'''
        return replace_once(s, anchor, automatic)
    edit("app/src/main/java/com/winlator/cmod/inputcontrols/ControllerManager.java", controller)

    def touch_pad_loop(s):
        # GTA2Ceiling: the Touch controllers are always a pad as well.
        return replace_once(s, '                xrAPI.consumeInputs(xServer);\n                if (XrActivity.gamepadEmulation) {',
                            '                xrAPI.consumeInputs(xServer);\n                XrActivity.getInstance().gccUpdateTouchPad();\n'
                            '                if (XrActivity.gamepadEmulation) {')
    edit("app/src/main/java/com/winlator/xr/io/XrInput.java", touch_pad_loop)

    def touch_pad_menu(s):
        # The WinlatorXR menu hold moves to the left stick (see gccMenuHold).
        old = '            if (buttons[primaryPress.ordinal()]) {\n'
        if s.count(old) != 2:
            raise ValueError("Expected the two WinlatorXR menu-hold checks")
        return s.replace(old, '            if (XrActivity.gccMenuHold(buttons, primaryPress)) {\n')
    edit("app/src/main/java/com/winlator/xr/io/XrController.java", touch_pad_menu)

    def touch_pad_merge(s):
        # GTA2Ceiling: Touch controllers (virtual pad) and a physical controller
        # both drive player 1: buttons and d-pad combined, each stick from
        # whichever pad moves it further, each trigger the stronger.
        s = replace_once(s, '    /** Virtual gamepad writes directly to P1 buffer, caches last state for gyro-only updates. */\n',
                         '''    private long gccEnsureAt = 0;

    private GamepadState gccMerged(GamepadState virtual) {
        if (!XrActivity.isActive() || !XrActivity.gamepadEmulation || virtual == null) return virtual;
        long now = System.currentTimeMillis();
        if (currentController == null && now >= gccEnsureAt) { gccEnsureAt = now + 1000; ensureP1Controller(); }
        if (currentController == null) return virtual;
        GamepadState physical = currentController.state, merged = new GamepadState();
        merged.copy(virtual);
        merged.buttons |= physical.buttons;
        for (int i = 0; i < 4; i++) merged.dpad[i] |= physical.dpad[i];
        if (physical.thumbLX * physical.thumbLX + physical.thumbLY * physical.thumbLY >
                merged.thumbLX * merged.thumbLX + merged.thumbLY * merged.thumbLY) {
            merged.thumbLX = physical.thumbLX; merged.thumbLY = physical.thumbLY;
        }
        if (physical.thumbRX * physical.thumbRX + physical.thumbRY * physical.thumbRY >
                merged.thumbRX * merged.thumbRX + merged.thumbRY * merged.thumbRY) {
            merged.thumbRX = physical.thumbRX; merged.thumbRY = physical.thumbRY;
        }
        merged.triggerL = Math.max(merged.triggerL, physical.triggerL);
        merged.triggerR = Math.max(merged.triggerR, physical.triggerR);
        return merged;
    }

    /** Virtual gamepad writes directly to P1 buffer, caches last state for gyro-only updates. */
''')
        s = replace_once(s, 'writeStateToMappedBuffer(state, gamepadBuffer, true, /*slot*/0);',
                         'writeStateToMappedBuffer(gccMerged(state), gamepadBuffer, true, /*slot*/0);')
        s = replace_once(s, '''        if (XrActivity.isActive() && XrActivity.gamepadEmulation && hasVirtualState && lastVirtualState != null) {
            writeStateToMappedBuffer(lastVirtualState, gamepadBuffer, true, 0);''',
                         '''        if (XrActivity.isActive() && XrActivity.gamepadEmulation && hasVirtualState && lastVirtualState != null) {
            writeStateToMappedBuffer(gccMerged(lastVirtualState), gamepadBuffer, true, 0);''')
        return replace_once(s, '                    GamepadState state = useVirtual ? profile.getGamepadState()\n                            : currentController.state;',
                            '                    GamepadState state = useVirtual ? profile.getGamepadState()\n'
                            '                            : (XrActivity.gamepadEmulation && hasVirtualState ? gccMerged(lastVirtualState) : currentController.state);')
    edit("app/src/main/java/com/winlator/cmod/winhandler/WinHandler.java", touch_pad_merge)

    put("app/src/main/java/com/winlator/cmod/core/GccAssets.java", (ROOT / "overlay" / "GccAssets.java").read_text(encoding="utf-8"))

    def manifest(s):
        s = replace_once(s, '    <application', '''    <uses-permission android:name="com.oculus.permission.USE_ANCHOR_API" />
    <uses-permission android:name="com.oculus.permission.USE_SCENE" />
    <queries><package android:name="com.winlator.cmod" /></queries>
    <application''')
        s = s.replace('android:label="@string/app_name"', 'android:label="GTA2Ceiling Host (experimental)"')
        s = s.replace('android:authorities="com.winlator.cmod.', 'android:authorities="${applicationId}.')
        return s
    edit("app/src/main/AndroidManifest.xml", manifest)

    # Only assets/data-path literals change; Java packages/JNI symbols stay upstream-compatible.
    java_root = fork / "app/src/main/java"
    for file in java_root.rglob("*"):
        if file.suffix not in (".java", ".kt") or file.name == "GccAssets.java":
            continue
        path = file.relative_to(fork).as_posix()
        if path not in changes and path not in previous:
            candidate = file.read_text(encoding="utf-8")
            if not any(needle in candidate for needle in ('getAssets()', '/com.winlator.cmod/', 'com.winlator.cmod/com.winlator.')):
                continue
        if path in changes:
            source = changes[path]
            original = upstream(path)
        else:
            original = upstream(path)
            source = original
        updated = source.replace('/data/data/com.winlator.cmod/', '/data/data/' + APP_ID + '/')
        updated = updated.replace('/com.winlator.cmod/storage', '/' + APP_ID + '/storage')
        updated = updated.replace('com.winlator.cmod/com.winlator.cmod.XServerDisplayActivity', APP_ID + '/com.winlator.cmod.XServerDisplayActivity')
        for context in ("mContext", "context", "ctx"):
            updated = updated.replace(context + '.getResources().getAssets()', 'com.winlator.cmod.core.GccAssets.get(' + context + ')')
            updated = updated.replace(context + '.getAssets()', 'com.winlator.cmod.core.GccAssets.get(' + context + ')')
        updated = updated.replace('getAssets().', 'com.winlator.cmod.core.GccAssets.get(this).')
        # A file patched last time but no longer is goes back to upstream too.
        if updated != source or path in changes or path in previous:
            put(path, updated, original)

    edit("app/build.gradle", lambda s: (ROOT / "overlay" / "app.build.gradle").read_text(encoding="utf-8"))
    edit("build.gradle", lambda s: "plugins {\n    id 'com.android.application' version '8.10.0' apply false\n    id 'org.jetbrains.kotlin.android' version '2.2.20' apply false\n}\n")
    edit("gradle.properties", lambda s: "org.gradle.jvmargs=-Xmx3072m -Dfile.encoding=UTF-8\nandroid.useAndroidX=true\nandroid.nonTransitiveRClass=true\nandroid.nonFinalResIds=false\norg.gradle.configuration-cache=false\norg.gradle.workers.max=2\n")
    for path, value in changes.items():
        target = fork / path
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists() or target.read_text(encoding="utf-8") != value:
            target.write_text(value, encoding="utf-8", newline="\n")
    state_path.write_text(json.dumps({path: digest(value) for path, value in changes.items()}, indent=2) + "\n", encoding="utf-8")
    print(f"Applied/verified {len(changes)} pinned thin-host source files; no binary build or device action.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fork", type=Path)
    apply(parser.parse_args().fork)
