// SPDX-License-Identifier: GPL-3.0-or-later
// GTA2Ceiling display-rate reprojection ("planar timewarp").
//
// The emulated game delivers about 30 images per second, and each one reaches
// the display a few frames after the head pose it was rendered with. The
// compositor's own reprojection only corrects head rotation, so every head
// movement also translates the eyes, and the city swims against the real room
// until the next game image arrives.
//
// Everything the game shows lies on (or hangs a little off) the scanned
// ceiling or floor plane. So on every display frame each output pixel's ray
// from the CURRENT eye pose is intersected with that plane and looked up in the
// game image through the eye pose it was rendered with. The layer is then
// submitted with the current pose. Head rotation and translation are both
// corrected at display rate; only cars and people still move at the game's 30.
//
// The game stamps the pose slot it rendered with into the image twice: an 8x8
// block plus an 8x8 green marker at the top-left and again at the bottom-right.
// The slot is read from the image itself, so the pose always belongs to the
// drawn content, and an image whose two stamps differ arrived torn (top from
// one game frame, bottom from the next). A torn image is never shown: the last
// complete one is reprojected instead.
#include "gcc_reproject.h"
#include "gcc_room_bridge.h"
#include <GLES3/gl3.h>
#include <android/log.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {
constexpr const char* kTag = "GTA2Reproject";
constexpr GLenum kSrgbDecode = 0x8A48;      // GL_TEXTURE_SRGB_DECODE_EXT
constexpr GLenum kDecode = 0x8A49;          // GL_DECODE_EXT
// The streets lie on the plane; buildings hang toward the viewer by up to a
// few blocks. Warping at about one block in front of the plane splits the
// remaining parallax error between street and rooftops.
constexpr float kContentOffset = 0.10f;

const char* kVertex = R"(#version 300 es
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";

// Shared: decode a stamp (slot index 0..21, or -1) from stored sRGB bytes.
#define GCC_STAMP_GLSL \
    "int stampSlot(vec4 c, vec4 m) {\n" \
    "    m *= 255.0; c *= 255.0;\n" \
    "    if (m.a < 250.0 || m.g < 250.0 || m.r > 5.0 || m.b > 5.0) return -1;\n" \
    "    if (c.a < 250.0 || c.g > 1.5 || c.b > 1.5) return -1;\n" \
    "    float k = floor(c.r / 12.0 + 0.5);\n" \
    "    if (abs(c.r - k * 12.0) > 3.0 || k > 21.0) return -1;\n" \
    "    return int(k);\n" \
    "}\n"

// Pass 1: the swapchain image (sampled through sRGB decode, as the spec says
// texelFetch does, and encoded back: the stored bytes) into the next private
// copy - unless it arrived torn, then the last complete copy is kept.
const char* kCopy = "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D swapImage;\n"
    "uniform sampler2D lastImage;\n"
    "uniform int haveLast;\n"
    "uniform int encodeBack;   // 1: samples arrive decoded, store the bytes\n"
    "uniform ivec4 topAt;      // stamp sample, marker sample (x, y, x, y)\n"
    "uniform ivec4 bottomAt;\n"
    "out vec4 outColor;\n"
    "vec4 bytes(vec4 c) {\n"
    "    if (encodeBack == 0) return c;\n"
    "    vec3 s = mix(c.rgb * 12.92, 1.055 * pow(c.rgb, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c.rgb));\n"
    "    return vec4(s, c.a);\n"
    "}\n"
    GCC_STAMP_GLSL
    "void main() {\n"
    "    int top = stampSlot(bytes(texelFetch(swapImage, topAt.xy, 0)), bytes(texelFetch(swapImage, topAt.zw, 0)));\n"
    "    int bottom = stampSlot(bytes(texelFetch(swapImage, bottomAt.xy, 0)), bytes(texelFetch(swapImage, bottomAt.zw, 0)));\n"
    "    bool torn = top >= 0 && bottom >= 0 && top != bottom;\n"
    "    outColor = (torn && haveLast != 0) ? texelFetch(lastImage, ivec2(gl_FragCoord.xy), 0)\n"
    "                                       : bytes(texelFetch(swapImage, ivec2(gl_FragCoord.xy), 0));\n"
    "}\n";

// Pass 2: per-pixel ray/plane reprojection back into the swapchain image.
const char* kWarp = "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D src;\n"
    "uniform vec2 texSize;\n"
    "uniform vec4 rect;      // this eye's pixel rectangle\n"
    "uniform vec4 tanFov;    // left, right, down, up tangents (game and layer)\n"
    "uniform vec4 nowQ;      // current eye pose (layer space)\n"
    "uniform vec3 nowP;\n"
    "uniform vec4 slotQ[22]; // this eye's pose for every frame-sync slot\n"
    "uniform vec3 slotP[22];\n"
    "uniform int fallbackSlot;\n"
    "uniform ivec4 topAt;\n"
    "uniform vec2 heights;   // content plane above / below the head\n"
    "uniform ivec2 planes;   // which of them exist\n"
    "uniform int encode;     // framebuffer encodes sRGB on write: decode here\n"
    "out vec4 outColor;\n"
    "vec3 toLinear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }\n"
    "vec3 rotate(vec4 q, vec3 v) { vec3 t = 2.0 * cross(q.xyz, v); return v + q.w * t + cross(q.xyz, t); }\n"
    GCC_STAMP_GLSL
    "void main() {\n"
    "    int slot = stampSlot(texelFetch(src, topAt.xy, 0), texelFetch(src, topAt.zw, 0));\n"
    "    if (slot < 0) slot = fallbackSlot;\n"
    "    vec4 renQ = slotQ[slot];\n"
    "    vec3 renP = slotP[slot];\n"
    "    vec2 uv = (gl_FragCoord.xy - rect.xy) / rect.zw;\n"
    "    vec3 d = rotate(nowQ, vec3(mix(tanFov.x, tanFov.y, uv.x), mix(tanFov.z, tanFov.w, uv.y), -1.0));\n"
    "    float t = -1.0;\n"
    "    if (d.y > 1e-4 && planes.x != 0) t = (heights.x - nowP.y) / d.y;\n"
    "    else if (d.y < -1e-4 && planes.y != 0) t = (heights.y - nowP.y) / d.y;\n"
    "    vec4 inv = vec4(-renQ.xyz, renQ.w);\n"
    "    // No plane on this ray: content at infinity, rotation only. This is also\n"
    "    // the limit of the plane case toward the horizon, so there is no seam.\n"
    "    vec3 v = t > 0.0 ? rotate(inv, nowP + d * t - renP) : rotate(inv, d);\n"
    "    if (v.z > -1e-6) { outColor = vec4(0.0); return; }\n"
    "    vec2 tanXY = v.xy / -v.z;\n"
    "    vec2 s = vec2((tanXY.x - tanFov.x) / (tanFov.y - tanFov.x), (tanXY.y - tanFov.z) / (tanFov.w - tanFov.z));\n"
    "    if (any(lessThan(s, vec2(0.0))) || any(greaterThan(s, vec2(1.0)))) { outColor = vec4(0.0); return; }\n"
    "    // Premultiplied image: bilinear filtering is exact at the keyed edges.\n"
    "    vec2 px = rect.xy + clamp(s * rect.zw, vec2(0.5), rect.zw - vec2(0.5));\n"
    "    vec4 c = texture(src, px / texSize);\n"
    "    outColor = encode != 0 ? vec4(toLinear(c.rgb), c.a) : c;\n"
    "}\n";

struct State {
    bool failed = false;
    GLuint copy = 0, warp = 0, vao = 0, texture[2]{}, fbo[2]{};
    int current = 0;          // copy written last frame (the last complete image)
    bool haveLast = false;
    int width = 0, height = 0;
    bool encode = false;      // the swapchain encodes sRGB on write
    int encodeBack = 1;       // pass 1 samples arrive decoded (verified below)
    GLint cSwap = -1, cLast = -1, cHaveLast = -1, cEncodeBack = -1, cTop = -1, cBottom = -1;
    GLint wSrc = -1, wTexSize = -1, wRect = -1, wTan = -1, wNowQ = -1, wNowP = -1, wSlotQ = -1, wSlotP = -1,
          wFallback = -1, wTop = -1, wHeights = -1, wPlanes = -1, wEncode = -1;
    int loggedPlanes = -1;
    // Diagnostics, read back asynchronously 3 frames later (never stalls):
    // both stamps of the arriving image, and the top stamp of the stored copy.
    GLuint pbo[3]{};
    int pboSlot[3]{};
    bool pboUsed[3]{};
    unsigned frame = 0;
    int counts[GCC_REPROJECT_STAT_COUNT]{};
    int logs = 0;
    int copyChecks = 0, copyBytes = 0, copyBright = 0;
};
State g;

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

GLuint Link(const char* fragment) {
    const GLuint vs = Compile(GL_VERTEX_SHADER, kVertex), fs = Compile(GL_FRAGMENT_SHADER, fragment);
    if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); return 0; }
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(program); return 0; }
    return program;
}

bool Prepare(int width, int height) {
    if (g.failed) return false;
    if (!g.copy) {
        const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        g.copy = Link(kCopy);
        g.warp = Link(kWarp);
        if (!extensions || !g.copy || !g.warp) { g.failed = true; return false; }
        g.cSwap = glGetUniformLocation(g.copy, "swapImage");
        g.cLast = glGetUniformLocation(g.copy, "lastImage");
        g.cHaveLast = glGetUniformLocation(g.copy, "haveLast");
        g.cEncodeBack = glGetUniformLocation(g.copy, "encodeBack");
        g.cTop = glGetUniformLocation(g.copy, "topAt");
        g.cBottom = glGetUniformLocation(g.copy, "bottomAt");
        g.wSrc = glGetUniformLocation(g.warp, "src");
        g.wTexSize = glGetUniformLocation(g.warp, "texSize");
        g.wRect = glGetUniformLocation(g.warp, "rect");
        g.wTan = glGetUniformLocation(g.warp, "tanFov");
        g.wNowQ = glGetUniformLocation(g.warp, "nowQ");
        g.wNowP = glGetUniformLocation(g.warp, "nowP");
        g.wSlotQ = glGetUniformLocation(g.warp, "slotQ");
        g.wSlotP = glGetUniformLocation(g.warp, "slotP");
        g.wFallback = glGetUniformLocation(g.warp, "fallbackSlot");
        g.wTop = glGetUniformLocation(g.warp, "topAt");
        g.wHeights = glGetUniformLocation(g.warp, "heights");
        g.wPlanes = glGetUniformLocation(g.warp, "planes");
        g.wEncode = glGetUniformLocation(g.warp, "encode");
        // Does writing to the sRGB swapchain encode? The host turns that off
        // (EXT_sRGB_write_control); without the extension it is always on.
        g.encode = !std::strstr(extensions, "GL_EXT_sRGB_write_control") || glIsEnabled(0x8DB9 /* GL_FRAMEBUFFER_SRGB_EXT */);
        __android_log_print(ANDROID_LOG_INFO, kTag, "swapchain writes %s", g.encode ? "sRGB encoded" : "raw");
        glGenBuffers(3, g.pbo);
        for (GLuint buffer : g.pbo) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
            glBufferData(GL_PIXEL_PACK_BUFFER, 3 * 36, nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glGenVertexArrays(1, &g.vao);
    }
    if (g.texture[0] && g.width == width && g.height == height) return true;
    for (int i = 0; i < 2; ++i) {
        if (g.fbo[i]) glDeleteFramebuffers(1, &g.fbo[i]);
        if (g.texture[i]) glDeleteTextures(1, &g.texture[i]);
        g.fbo[i] = g.texture[i] = 0;
    }
    for (int i = 0; i < 2; ++i) {
        glGenTextures(1, &g.texture[i]);
        glBindTexture(GL_TEXTURE_2D, g.texture[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &g.fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, g.fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.texture[i], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "copy target incomplete; reprojection off");
            g.failed = true;
            return false;
        }
    }
    g.haveLast = false;
    g.width = width; g.height = height;
    __android_log_print(ANDROID_LOG_INFO, kTag, "display-rate reprojection ready (%dx%d)", width, height);
    return true;
}

// CPU side of the stamp decode (bytes as stored in the swapchain).
int StampSlot(const unsigned char* c, const unsigned char* m) {
    if (m[3] < 250 || m[1] < 250 || m[0] > 5 || m[2] > 5) return -1;
    if (c[3] < 250 || c[1] > 1 || c[2] > 1) return -1;
    const int k = (c[0] + 6) / 12;
    if (std::abs(c[0] - 12 * k) > 3 || k > 21) return -1;
    return k;
}
float SrgbByte(int k) {
    const float l = 12.0f * k / 255.0f;
    return 255.0f * (l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f);
}

void ReadBack(unsigned i) {
    const auto* px = static_cast<const unsigned char*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 3 * 36, GL_MAP_READ_BIT));
    if (!px) return;
    // Rows of 9 pixels: [0] stamp sample, [8] marker sample.
    const int top = StampSlot(px, px + 32), bottom = StampSlot(px + 36, px + 68);
    const unsigned char* stored = px + 72;
    const int java = g.pboSlot[i];
    int* n = g.counts;
    if (top < 0) ++n[GCC_REPROJECT_STAMP_UNREADABLE];
    else if (bottom >= 0 && bottom != top) ++n[GCC_REPROJECT_TORN];
    else ++n[GCC_REPROJECT_INTACT];
    if (top >= 0) ++n[top == java ? GCC_REPROJECT_SYNC_SAME : GCC_REPROJECT_SYNC_OTHER];
    // Self-check of pass 1: the stored copy must hold the swapchain's bytes.
    if (top >= 0 && bottom == top && g.copyChecks < 60) {
        ++g.copyChecks;
        if (std::abs(stored[0] - px[0]) <= 2) ++g.copyBytes;
        else if (std::fabs(stored[0] - SrgbByte(top)) <= 3.0f && top >= 2) ++g.copyBright;
        if (g.copyChecks == 60) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "copy check: %d of 60 exact, %d too bright", g.copyBytes, g.copyBright);
            if (g.copyBright > 40) {
                g.encodeBack = 0;  // this driver does not decode on texelFetch
                __android_log_print(ANDROID_LOG_WARN, kTag, "copy: samples arrive undecoded, storing them as they are");
                g.copyChecks = 0; g.copyBytes = g.copyBright = 0;
            }
        }
    }
    if (g.logs < 6 || ((top < 0 || (bottom >= 0 && bottom != top)) && g.logs < 16)) {
        ++g.logs;
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "stamps: top %d %d %d %d (marker %d %d %d) -> %d, bottom %d %d %d %d (marker %d %d %d) -> %d, stored %d, sync reader %d",
                            px[0], px[1], px[2], px[3], px[32], px[33], px[34], top,
                            px[36], px[37], px[38], px[39], px[68], px[69], px[70], bottom, stored[0], java);
    }
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
}
}  // namespace

extern "C" void GccReprojectStats(int out[GCC_REPROJECT_STAT_COUNT]) {
    for (int i = 0; i < GCC_REPROJECT_STAT_COUNT; ++i) { out[i] = g.counts[i]; g.counts[i] = 0; }
}

extern "C" int GccReprojectApply(unsigned int swapTexture, int width, int height, XrSpace space, XrTime time,
                                 const XrPosef slotPoses[2][22], int fallbackSlot, const XrPosef nowPose[2],
                                 XrFovf fov, const int rect[2][4]) {
    if (fallbackSlot < 0 || fallbackSlot > 21) fallbackSlot = 0;
    if (!swapTexture || width <= 0 || height <= 0 || !space || time <= 0) { ++g.counts[GCC_REPROJECT_SKIPPED]; return 0; }

    // The content plane: the round play field when there is one, otherwise
    // the scanned ceiling (rays upward) and floor (rays downward).
    const float headY = 0.5f * (nowPose[0].position.y + nowPose[1].position.y);
    float ceilY = 0, floorY = 0, outline[128]{};
    int outlineCount = 0;
    int planes = GccRoomBridgePlaneHeights(space, time, headY, &ceilY, &floorY, outline, 64, &outlineCount);
    float field[4]{};
    if (GccRoomBridgeField(space, time, field)) {
        if (field[1] > headY) { ceilY = field[1]; planes = 1; }
        else { floorY = field[1]; planes = 2; }
    }
    if (planes != g.loggedPlanes) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "content planes: ceiling %d (%.2f m) floor %d (%.2f m), head %.2f m",
                            planes & 1, ceilY, (planes >> 1) & 1, floorY, headY);
        g.loggedPlanes = planes;
    }
    // Never leave the frame to the compositor: after standby or a recenter the
    // room planes can be unavailable for a while. Keep the last known ones
    // (as long as they still lie above / below the head), else correct
    // rotation only (content at infinity) - never fall back to judder.
    static int lastPlanes = 0;
    static float lastCeil = 0, lastFloor = 0;
    if (planes) { lastPlanes = planes; lastCeil = ceilY; lastFloor = floorY; }
    else {
        planes = lastPlanes;
        ceilY = lastCeil; floorY = lastFloor;
        if ((planes & 1) && ceilY <= headY + 0.2f) planes &= ~1;
        if ((planes & 2) && floorY >= headY - 0.2f) planes &= ~2;
    }
    ceilY -= kContentOffset;
    floorY += kContentOffset;

    // Save the GL state this pass touches; the host continues drawing after it.
    GLint program = 0, vao = 0, viewport[4]{}, active = 0, bound6 = 0, bound7 = 0, drawFbo = 0, readFbo = 0,
          decode = kDecode, minFilter = GL_LINEAR, magFilter = GL_LINEAR, packBuffer = 0;
    GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST),
              scissor = glIsEnabled(GL_SCISSOR_TEST), cull = glIsEnabled(GL_CULL_FACE),
              stencil = glIsEnabled(GL_STENCIL_TEST);
    GLboolean mask[4]{};
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
    glActiveTexture(GL_TEXTURE7);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound7);
    glActiveTexture(GL_TEXTURE6);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound6);

    int result = 0;
    if (Prepare(width, height)) {
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_STENCIL_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBindVertexArray(g.vao);

        // Stamp samples a few pixels inside the 8x8 blocks, where scaling into
        // the eye buffer (here 1x horizontally, 2x vertically) keeps the values
        // exact. Top-left of eye 0, bottom-right of eye 1.
        const int topX = rect[0][0] + 3, topY = rect[0][1] + rect[0][3] - 7, topMarkerX = rect[0][0] + 11;
        const int bottomX = rect[1][0] + rect[1][2] - 13, bottomY = rect[1][1] + 6,
                  bottomMarkerX = rect[1][0] + rect[1][2] - 5;
        const int next = 1 - g.current;  // ping-pong: never sample the image being written

        // 1) swapchain image -> next private copy (or keep the last complete one).
        glBindTexture(GL_TEXTURE_2D, swapTexture);  // unit 6
        glGetTexParameteriv(GL_TEXTURE_2D, kSrgbDecode, &decode);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minFilter);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magFilter);
        glTexParameteri(GL_TEXTURE_2D, kSrgbDecode, kDecode);
        // One level only: a mipmapped min filter would make it incomplete.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, g.texture[g.current]);
        glActiveTexture(GL_TEXTURE6);
        glBindFramebuffer(GL_FRAMEBUFFER, g.fbo[next]);
        glViewport(0, 0, width, height);
        glUseProgram(g.copy);
        glUniform1i(g.cSwap, 6);
        glUniform1i(g.cLast, 7);
        glUniform1i(g.cHaveLast, g.haveLast ? 1 : 0);
        glUniform1i(g.cEncodeBack, g.encodeBack);
        glUniform4i(g.cTop, topX, topY, topMarkerX, topY);
        glUniform4i(g.cBottom, bottomX, bottomY, bottomMarkerX, bottomY);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glTexParameteri(GL_TEXTURE_2D, kSrgbDecode, decode);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
        g.current = next;
        g.haveLast = true;

        // Diagnostics: map the read-back of 3 frames ago, then queue this one.
        {
            const unsigned i = g.frame % 3;
            glBindBuffer(GL_PIXEL_PACK_BUFFER, g.pbo[i]);
            if (g.pboUsed[i]) ReadBack(i);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
            glReadPixels(topX, topY, 9, 1, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glReadPixels(bottomX, bottomY, 9, 1, GL_RGBA, GL_UNSIGNED_BYTE, reinterpret_cast<void*>(36));
            glBindFramebuffer(GL_READ_FRAMEBUFFER, g.fbo[g.current]);
            glReadPixels(topX, topY, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, reinterpret_cast<void*>(72));
            g.pboSlot[i] = fallbackSlot;
            g.pboUsed[i] = true;
            ++g.frame;
            glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(packBuffer));
        }

        // 2) private copy -> swapchain image, reprojected per eye.
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
        glBindTexture(GL_TEXTURE_2D, g.texture[g.current]);
        glUseProgram(g.warp);
        glUniform1i(g.wSrc, 6);
        glUniform1i(g.wEncode, g.encode ? 1 : 0);
        glUniform2f(g.wTexSize, static_cast<float>(width), static_cast<float>(height));
        glUniform4f(g.wTan, std::tan(fov.angleLeft), std::tan(fov.angleRight), std::tan(fov.angleDown),
                    std::tan(fov.angleUp));
        glUniform2f(g.wHeights, ceilY, floorY);
        glUniform2i(g.wPlanes, planes & 1, (planes >> 1) & 1);
        glUniform1i(g.wFallback, fallbackSlot);
        glUniform4i(g.wTop, topX, topY, topMarkerX, topY);
        for (int eye = 0; eye < 2; ++eye) {
            glViewport(rect[eye][0], rect[eye][1], rect[eye][2], rect[eye][3]);
            glUniform4f(g.wRect, static_cast<float>(rect[eye][0]), static_cast<float>(rect[eye][1]),
                        static_cast<float>(rect[eye][2]), static_cast<float>(rect[eye][3]));
            const auto& nq = nowPose[eye].orientation; const auto& np = nowPose[eye].position;
            glUniform4f(g.wNowQ, nq.x, nq.y, nq.z, nq.w);
            glUniform3f(g.wNowP, np.x, np.y, np.z);
            float q[22 * 4], p[22 * 3];
            for (int k = 0; k < 22; ++k) {
                const auto& o = slotPoses[eye][k].orientation; const auto& t = slotPoses[eye][k].position;
                q[k * 4] = o.x; q[k * 4 + 1] = o.y; q[k * 4 + 2] = o.z; q[k * 4 + 3] = o.w;
                p[k * 3] = t.x; p[k * 3 + 1] = t.y; p[k * 3 + 2] = t.z;
            }
            glUniform4fv(g.wSlotQ, 22, q);
            glUniform3fv(g.wSlotP, 22, p);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        result = 1;
        ++g.counts[GCC_REPROJECT_WARPED];
    } else {
        ++g.counts[GCC_REPROJECT_SKIPPED];
    }

    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound6));
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound7));
    glActiveTexture(static_cast<GLenum>(active));
    glBindVertexArray(static_cast<GLuint>(vao));
    glUseProgram(static_cast<GLuint>(program));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo));
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    if (blend) glEnable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (stencil) glEnable(GL_STENCIL_TEST);
    return result;
}
