// SPDX-License-Identifier: GPL-3.0-or-later
// GTA2Ceiling real-world occlusion for the host's projection layer.
//
// The game image carries no depth, but everything it shows lies on (or hangs a
// little off) the scanned ceiling or floor plane. For each pixel of the game's
// projection-layer image the ray from the eye pose the game rendered with is
// intersected with that plane; the Quest environment depth (XR_META_environment_
// depth) then says whether something real - a hand, a person, furniture - is in
// front of that point. Covered pixels are faded out so passthrough shows through.
#include "gcc_occlusion.h"
#include "gcc_room_bridge.h"
#include "openxr.h"
#include <GLES3/gl3.h>
#include <android/log.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
constexpr const char* kTag = "GTA2Occlusion";
bool g_extension = false;
int g_loggedWanted = -1;

struct State {
    XrInstance instance = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    PFN_xrCreateEnvironmentDepthProviderMETA createProvider = nullptr;
    PFN_xrDestroyEnvironmentDepthProviderMETA destroyProvider = nullptr;
    PFN_xrStartEnvironmentDepthProviderMETA startProvider = nullptr;
    PFN_xrStopEnvironmentDepthProviderMETA stopProvider = nullptr;
    PFN_xrCreateEnvironmentDepthSwapchainMETA createSwapchain = nullptr;
    PFN_xrDestroyEnvironmentDepthSwapchainMETA destroySwapchain = nullptr;
    PFN_xrEnumerateEnvironmentDepthSwapchainImagesMETA enumerateImages = nullptr;
    PFN_xrAcquireEnvironmentDepthImageMETA acquire = nullptr;
    XrEnvironmentDepthProviderMETA provider = XR_NULL_HANDLE;
    XrEnvironmentDepthSwapchainMETA swapchain = XR_NULL_HANDLE;
    std::vector<GLuint> images;
    bool running = false, failed = false;
    std::chrono::steady_clock::time_point retryAt{};
    GLuint program = 0, vao = 0;
    // Diagnostics (logcat tag GTA2Occlusion).
    int loggedPlanes = -1, loggedOutline = -1;
    unsigned applied = 0, acquireFailures = 0, noPlanes = 0;
    std::chrono::steady_clock::time_point statsAt{};
    GLint uRect = -1, uTan = -1, uQ = -1, uP = -1, uDepthVP = -1, uPlanes = -1, uHeights = -1,
          uEye = -1, uDepthTex = -1, uNearFar = -1, uOutline = -1, uOutlineCount = -1;
};
State* g_state = nullptr;

template <class T> bool Load(XrInstance instance, const char* name, T& out) {
    PFN_xrVoidFunction function = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(instance, name, &function)) || !function) return false;
    out = reinterpret_cast<T>(function);
    return true;
}

const char* kVertex = R"(#version 300 es
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";

const char* kFragment = R"(#version 300 es
precision highp float;
precision highp sampler2DArray;
uniform sampler2DArray depthTex;
uniform float eye;
uniform vec4 rect;      // pixel rectangle of this eye in the swapchain image
uniform vec4 tanFov;    // left, right, down, up tangents the game rendered with
uniform vec4 renderQ;   // eye pose the game rendered with (space of the layer)
uniform vec3 renderP;
uniform mat4 depthVP;   // layer space -> environment depth clip space
uniform vec2 nearFar;   // environment depth near/far (far <= 0: infinite)
uniform vec2 heights;   // ceiling y, floor y
uniform ivec2 planes;   // ceiling valid, floor valid
uniform vec2 outline[64]; // room walls (x,z), closed polygon
uniform int outlineCount;
out vec4 outColor;
vec3 rotate(vec4 q, vec3 v) { vec3 t = 2.0 * cross(q.xyz, v); return v + q.w * t + cross(q.xyz, t); }
float linearDepth(float d) {
    float ndc = d * 2.0 - 1.0;
    float n = nearFar.x, f = nearFar.y;
    if (f <= 0.0) return 2.0 * n / max(1.0 - ndc, 1e-6);
    return 2.0 * f * n / max((f + n) - ndc * (f - n), 1e-6);
}
void main() {
    vec2 uv = (gl_FragCoord.xy - rect.xy) / rect.zw;
    vec3 dir = rotate(renderQ, normalize(vec3(mix(tanFov.x, tanFov.y, uv.x), mix(tanFov.z, tanFov.w, uv.y), -1.0)));
    // Where the empty room would be seen: the first hit on ceiling, floor or
    // a wall. Everything the game draws lies on or inside that shell, so the
    // depth sensor seeing something clearly nearer means a real occluder.
    float t = 1e9;
    if (dir.y > 1e-4 && planes.x != 0) t = (heights.x - renderP.y) / dir.y;
    else if (dir.y < -1e-4 && planes.y != 0) t = (heights.y - renderP.y) / dir.y;
    if (t <= 0.0) t = 1e9;
    for (int i = 0; i < 64; ++i) {
        if (i >= outlineCount) break;
        vec2 a = outline[i], b = outline[(i + 1 == outlineCount) ? 0 : i + 1];
        vec2 e = b - a, d = dir.xz;
        float den = d.x * e.y - d.y * e.x;
        if (abs(den) < 1e-6) continue;
        vec2 ao = a - renderP.xz;
        float tw = (ao.x * e.y - ao.y * e.x) / den;
        float u = (ao.x * d.y - ao.y * d.x) / den;
        if (tw > 0.0 && u >= 0.0 && u <= 1.0) t = min(t, tw);
    }
    if (t >= 1e8) discard;
    // A little in front of the shell: hanging buildings are nearer than it.
    vec3 p = renderP + dir * max(t - 0.05, 0.05);
    vec4 clip = depthVP * vec4(p, 1.0);
    if (clip.w <= 0.0) discard;
    vec2 duv = clip.xy / clip.w * 0.5 + 0.5;
    if (any(lessThan(duv, vec2(0.0))) || any(greaterThan(duv, vec2(1.0)))) discard;
    float env = texture(depthTex, vec3(duv, eye)).r;
    if (env >= 0.9999) discard;
    float covered = clamp((clip.w - linearDepth(env) - 0.03) / 0.06, 0.0, 1.0);
    if (covered <= 0.0) discard;
    outColor = vec4(0.0, 0.0, 0.0, covered);
})";

GLuint Compile(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        __android_log_print(ANDROID_LOG_ERROR, kTag, "shader: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool BuildProgram(State& s) {
    if (s.program) return true;
    const GLuint vs = Compile(GL_VERTEX_SHADER, kVertex), fs = Compile(GL_FRAGMENT_SHADER, kFragment);
    if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); return false; }
    s.program = glCreateProgram();
    glAttachShader(s.program, vs);
    glAttachShader(s.program, fs);
    glLinkProgram(s.program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(s.program, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(s.program); s.program = 0; return false; }
    s.uRect = glGetUniformLocation(s.program, "rect");
    s.uTan = glGetUniformLocation(s.program, "tanFov");
    s.uQ = glGetUniformLocation(s.program, "renderQ");
    s.uP = glGetUniformLocation(s.program, "renderP");
    s.uDepthVP = glGetUniformLocation(s.program, "depthVP");
    s.uPlanes = glGetUniformLocation(s.program, "planes");
    s.uHeights = glGetUniformLocation(s.program, "heights");
    s.uEye = glGetUniformLocation(s.program, "eye");
    s.uDepthTex = glGetUniformLocation(s.program, "depthTex");
    s.uNearFar = glGetUniformLocation(s.program, "nearFar");
    s.uOutline = glGetUniformLocation(s.program, "outline");
    s.uOutlineCount = glGetUniformLocation(s.program, "outlineCount");
    glGenVertexArrays(1, &s.vao);
    return true;
}

void DestroyProvider(State& s) {
    if (s.running && s.stopProvider) s.stopProvider(s.provider);
    s.running = false;
    if (s.swapchain && s.destroySwapchain) s.destroySwapchain(s.swapchain);
    s.swapchain = XR_NULL_HANDLE;
    if (s.provider && s.destroyProvider) s.destroyProvider(s.provider);
    s.provider = XR_NULL_HANDLE;
    s.images.clear();
}

bool EnsureProvider(State& s) {
    if (s.running) return true;
    const auto now = std::chrono::steady_clock::now();
    if (now < s.retryAt) return false;
    s.retryAt = now + std::chrono::seconds(2);
    DestroyProvider(s);
    XrEnvironmentDepthProviderCreateInfoMETA info{XR_TYPE_ENVIRONMENT_DEPTH_PROVIDER_CREATE_INFO_META};
    const XrResult created = s.createProvider(s.session, &info, &s.provider);
    if (XR_FAILED(created)) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "environment depth provider not available yet (XrResult %d)", static_cast<int>(created));
        s.provider = XR_NULL_HANDLE;
        return false;
    }
    XrEnvironmentDepthSwapchainCreateInfoMETA chainInfo{XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_CREATE_INFO_META};
    if (XR_FAILED(s.createSwapchain(s.provider, &chainInfo, &s.swapchain))) { DestroyProvider(s); return false; }
    uint32_t count = 0;
    if (XR_FAILED(s.enumerateImages(s.swapchain, 0, &count, nullptr)) || !count || count > 16) { DestroyProvider(s); return false; }
    std::vector<XrSwapchainImageOpenGLESKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    if (XR_FAILED(s.enumerateImages(s.swapchain, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())))) { DestroyProvider(s); return false; }
    GLint previous = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &previous);
    for (const auto& image : images) {
        s.images.push_back(image.image);
        glBindTexture(GL_TEXTURE_2D_ARRAY, image.image);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    }
    glBindTexture(GL_TEXTURE_2D_ARRAY, static_cast<GLuint>(previous));
    if (XR_FAILED(s.startProvider(s.provider))) { DestroyProvider(s); return false; }
    s.running = true;
    __android_log_print(ANDROID_LOG_INFO, kTag, "environment depth running (%u images)", count);
    return true;
}

// Column-major 4x4 for GL.
struct M4 { float m[16]; };
M4 Multiply(const M4& a, const M4& b) {
    M4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float v = 0;
            for (int k = 0; k < 4; ++k) v += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = v;
        }
    return r;
}
M4 InverseRigid(const XrPosef& pose) {
    const auto& q = pose.orientation;
    // Rotation matrix of q (column-major), then transpose for the inverse.
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z, wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    const float r[3][3] = {{1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy)},
                           {2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx)},
                           {2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)}};  // r[row][col]
    M4 inv{};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) inv.m[col * 4 + row] = r[col][row];
    const float p[3] = {pose.position.x, pose.position.y, pose.position.z};
    for (int row = 0; row < 3; ++row)
        inv.m[12 + row] = -(r[0][row] * p[0] + r[1][row] * p[1] + r[2][row] * p[2]);
    inv.m[15] = 1;
    return inv;
}
M4 Projection(const XrFovf& fov, float nearZ, float farZ) {
    const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
    const float d = std::tan(fov.angleDown), u = std::tan(fov.angleUp);
    M4 p{};
    p.m[0] = 2 / (r - l);
    p.m[5] = 2 / (u - d);
    p.m[8] = (r + l) / (r - l);
    p.m[9] = (u + d) / (u - d);
    p.m[11] = -1;
    if (!std::isfinite(farZ) || farZ <= nearZ) {
        p.m[10] = -1;
        p.m[14] = -2 * nearZ;
    } else {
        p.m[10] = -(farZ + nearZ) / (farZ - nearZ);
        p.m[14] = -2 * farZ * nearZ / (farZ - nearZ);
    }
    return p;
}
}  // namespace

extern "C" void GccOcclusionSetExtensionEnabled(int enabled) { g_extension = enabled != 0; }

extern "C" void GccOcclusionApply(XrInstance instance, XrSession session, XrSpace space, XrTime time,
                                  const XrPosef renderPose[2], XrFovf fov, const int rect[2][4]) {
    if (!g_extension || !instance || !session || !space || time <= 0) return;
    const int wanted = GccRoomBridgeOcclusionWanted();
    if (wanted != g_loggedWanted) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "occlusion requested by game: %d (extension %d)", wanted, g_extension ? 1 : 0);
        g_loggedWanted = wanted;
    }
    if (!wanted) {
        if (g_state && g_state->running) DestroyProvider(*g_state);  // save power while off
        return;
    }
    if (!g_state || g_state->instance != instance || g_state->session != session) {
        GccOcclusionShutdown();
        g_state = new State;
        g_state->instance = instance;
        g_state->session = session;
        State& s = *g_state;
        s.failed = !(Load(instance, "xrCreateEnvironmentDepthProviderMETA", s.createProvider) &&
                     Load(instance, "xrDestroyEnvironmentDepthProviderMETA", s.destroyProvider) &&
                     Load(instance, "xrStartEnvironmentDepthProviderMETA", s.startProvider) &&
                     Load(instance, "xrStopEnvironmentDepthProviderMETA", s.stopProvider) &&
                     Load(instance, "xrCreateEnvironmentDepthSwapchainMETA", s.createSwapchain) &&
                     Load(instance, "xrDestroyEnvironmentDepthSwapchainMETA", s.destroySwapchain) &&
                     Load(instance, "xrEnumerateEnvironmentDepthSwapchainImagesMETA", s.enumerateImages) &&
                     Load(instance, "xrAcquireEnvironmentDepthImageMETA", s.acquire));
        if (s.failed) __android_log_print(ANDROID_LOG_WARN, kTag, "environment depth functions unavailable");
    }
    State& s = *g_state;
    if (s.failed || !EnsureProvider(s)) return;
    if (!BuildProgram(s)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "occlusion shader unavailable; disabling");
        s.failed = true;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - s.statsAt >= std::chrono::seconds(5)) {
        if (s.statsAt.time_since_epoch().count())
            __android_log_print(ANDROID_LOG_INFO, kTag, "5s: applied %u frames, depth not ready %u, no room planes %u",
                                s.applied, s.acquireFailures, s.noPlanes);
        s.statsAt = now; s.applied = s.acquireFailures = s.noPlanes = 0;
    }

    const float headY = 0.5f * (renderPose[0].position.y + renderPose[1].position.y);
    float ceilY = 0, floorY = 0, outline[128]{};
    int outlineCount = 0;
    const int planes = GccRoomBridgePlaneHeights(space, time, headY, &ceilY, &floorY, outline, 64, &outlineCount);
    if (planes != s.loggedPlanes || outlineCount != s.loggedOutline) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "room shell: ceiling %d y=%.2f, floor %d y=%.2f, %d wall corners (head y %.2f)",
                            planes & 1, ceilY, (planes >> 1) & 1, floorY, outlineCount, headY);
        s.loggedPlanes = planes; s.loggedOutline = outlineCount;
    }
    if (!planes) { ++s.noPlanes; return; }

    XrEnvironmentDepthImageAcquireInfoMETA acquireInfo{XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_ACQUIRE_INFO_META};
    acquireInfo.space = space;
    acquireInfo.displayTime = time;
    XrEnvironmentDepthImageMETA image{XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_META};
    image.views[0].type = image.views[1].type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_VIEW_META;
    if (XR_FAILED(s.acquire(s.provider, &acquireInfo, &image)) || image.swapchainIndex >= s.images.size()) {
        ++s.acquireFailures;
        return;
    }
    ++s.applied;

    // Save the GL state this pass touches; the host continues drawing after it.
    GLint program = 0, vao = 0, viewport[4]{}, active = 0, boundArray = 0;
    GLint srcRgb = 0, dstRgb = 0, srcAlpha = 0, dstAlpha = 0, eqRgb = 0, eqAlpha = 0;
    GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST),
              scissor = glIsEnabled(GL_SCISSOR_TEST), cull = glIsEnabled(GL_CULL_FACE);
    GLboolean mask[4]{};
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glGetIntegerv(GL_BLEND_SRC_RGB, &srcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dstAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &eqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eqAlpha);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glActiveTexture(GL_TEXTURE7);
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &boundArray);

    glUseProgram(s.program);
    glBindVertexArray(s.vao);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s.images[image.swapchainIndex]);
    glUniform1i(s.uDepthTex, 7);
    glEnable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    // The layer is premultiplied: scaling RGBA by (1 - covered) reveals passthrough.
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
    glUniform4f(s.uTan, std::tan(fov.angleLeft), std::tan(fov.angleRight), std::tan(fov.angleDown), std::tan(fov.angleUp));
    glUniform2f(s.uHeights, ceilY, floorY);
    glUniform2i(s.uPlanes, planes & 1, (planes >> 1) & 1);
    glUniform2fv(s.uOutline, 64, outline);
    glUniform1i(s.uOutlineCount, outlineCount);
    glUniform2f(s.uNearFar, image.nearZ, std::isfinite(image.farZ) && image.farZ > image.nearZ ? image.farZ : 0.0f);
    for (int eye = 0; eye < 2; ++eye) {
        const M4 depthVP = Multiply(Projection(image.views[eye].fov, image.nearZ, image.farZ),
                                    InverseRigid(image.views[eye].pose));
        glViewport(rect[eye][0], rect[eye][1], rect[eye][2], rect[eye][3]);
        glUniform4f(s.uRect, static_cast<float>(rect[eye][0]), static_cast<float>(rect[eye][1]),
                    static_cast<float>(rect[eye][2]), static_cast<float>(rect[eye][3]));
        const auto& q = renderPose[eye].orientation;
        const auto& p = renderPose[eye].position;
        glUniform4f(s.uQ, q.x, q.y, q.z, q.w);
        glUniform3f(s.uP, p.x, p.y, p.z);
        glUniformMatrix4fv(s.uDepthVP, 1, GL_FALSE, depthVP.m);
        glUniform1f(s.uEye, static_cast<float>(eye));
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    glBindTexture(GL_TEXTURE_2D_ARRAY, static_cast<GLuint>(boundArray));
    glActiveTexture(static_cast<GLenum>(active));
    glBindVertexArray(static_cast<GLuint>(vao));
    glUseProgram(static_cast<GLuint>(program));
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glBlendEquationSeparate(static_cast<GLenum>(eqRgb), static_cast<GLenum>(eqAlpha));
    glBlendFuncSeparate(static_cast<GLenum>(srcRgb), static_cast<GLenum>(dstRgb),
                        static_cast<GLenum>(srcAlpha), static_cast<GLenum>(dstAlpha));
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (cull) glEnable(GL_CULL_FACE);
}

extern "C" void GccOcclusionShutdown(void) {
    if (!g_state) return;
    DestroyProvider(*g_state);
    // GL objects die with the context; the host calls this on session teardown.
    delete g_state;
    g_state = nullptr;
}
