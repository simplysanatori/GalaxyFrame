// vr_draw.h on OpenGL ES: the programs and GL state behind the VR
// presentation's own drawing (composite, panels, laser, motion passes), the
// GPU timers and the readbacks.
#include <GLES3/gl32.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "port/port.h"
#include "ui_canvas.h"
#include "vr_draw.h"

namespace vrdraw {
namespace {

// sRGB-encoded game colors -> linear, so the sRGB swapchain stores them as is.
const char* kCommon = R"(
vec3 toLinear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }
)";

const char* kBlitVs = R"(#version 320 es
out vec2 vUv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// The composite of the diorama into the eye image.  Its first %s is the
// common code, the second defines sourceColor(): the diorama's colour at the
// pixel (kBlitPlain, kBlitCas1, kBlitCasScaled).
const char* kBlitFs = R"(#version 320 es
precision mediump float;
uniform sampler2D uTex;
uniform float uBrightness;
uniform float uVignette;  // 0 = off .. 1 = strong
uniform vec4 uVigMap;     // image position to the tangent of the view angle
uniform vec4 uWipe;       // closed (0..1), kind (0 fade, 1 ring), ring centre uv
uniform vec4 uWipeColor;  // linear colour, w = width / height of the eye
in vec2 vUv;
out vec4 oColor;
%s
%s
void main() {
    vec2 d = vUv * 2.0 - 1.0;
    // The comfort vignette, by the angle from the view's axis (see vr_draw.h).
    float ang = atan(length(vec2(d.x * uVigMap.x + uVigMap.y, d.y * uVigMap.z + uVigMap.w)));
    float clearTo = 0.96 - 0.36 * uVignette;
    float v = 1.0 - uVignette * smoothstep(clearTo, clearTo + 0.52, ang);
    vec3 c = toLinear(sourceColor()) * (uVignette > 0.0 ? pow(v, 2.2) : 1.0);
    if (uWipe.y < 0.5) {
        c = mix(c, uWipeColor.rgb, uWipe.x);
    } else {
        // A ring closing on its centre (radius in units of the eye's height;
        // 1.6 clears the view from any centre on it).
        float r = (1.0 - uWipe.x) * 1.6;
        float inside = 1.0 - smoothstep(r - 0.015, r, length((vUv - uWipe.zw) * vec2(uWipeColor.w, 1.0)));
        c = mix(uWipeColor.rgb, c, inside);
    }
    oColor = vec4(c * uBrightness, 1.0);
}
)";

const char* kBlitPlain = R"(
vec3 sourceColor() { return texture(uTex, vUv).rgb; }
)";

// AMD FidelityFX CAS (contrast adaptive sharpening), after CasFilter in
// ffx_cas.h (https://github.com/GPUOpen-Effects/FidelityFX-CAS):
//   Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All rights reserved.
//   Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software and associated documentation files (the
//   "Software"), to deal in the Software without restriction, including
//   without limitation the rights to use, copy, modify, merge, publish,
//   distribute, sublicense, and/or sell copies of the Software, and to
//   permit persons to whom the Software is furnished to do so, subject to
//   the following conditions: The above copyright notice and this
//   permission notice shall be included in all copies or substantial
//   portions of the Software.  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT
//   WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
//   THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
//   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
//   LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
//   CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH
//   THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// A cross-shaped sharpening filter whose strength follows the local
// contrast, so flat areas and strong edges are sharpened less (no
// ringing); the weights come from the green channel, as ffx_cas.h's fast
// path uses them for all three.  Runs on the game's sRGB-encoded colours.
//   uCasPeak: -1 / mix(8, 5, sharpness)
//   uCasScale: diorama pixels per eye image pixel (the scaled variant)
// kBlitCas1: diorama and eye image the same size (Super Resolution).
const char* kBlitCas1 = R"(
uniform float uCasPeak;
vec3 casLoad(ivec2 p) { return texelFetch(uTex, clamp(p, ivec2(0), textureSize(uTex, 0) - 1), 0).rgb; }
vec3 sourceColor() {
    ivec2 sp = ivec2(gl_FragCoord.xy);
    vec3 a = casLoad(sp + ivec2(-1, -1));
    vec3 b = casLoad(sp + ivec2(0, -1));
    vec3 c = casLoad(sp + ivec2(1, -1));
    vec3 d = casLoad(sp + ivec2(-1, 0));
    vec3 e = casLoad(sp);
    vec3 f = casLoad(sp + ivec2(1, 0));
    vec3 g = casLoad(sp + ivec2(-1, 1));
    vec3 h = casLoad(sp + ivec2(0, 1));
    vec3 i = casLoad(sp + ivec2(1, 1));
    // Soft minimum and maximum: the cross plus the whole 3x3 (twice the size).
    float mn = min(min(min(d.g, e.g), min(f.g, b.g)), h.g);
    float mx = max(max(max(d.g, e.g), max(f.g, b.g)), h.g);
    mn += min(min(min(mn, a.g), min(c.g, g.g)), i.g);
    mx += max(max(max(mx, a.g), max(c.g, g.g)), i.g);
    // Distance to the signal's limits over the maximum, shaped.
    float amp = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, 1e-4), 0.0, 1.0));
    float w = amp * uCasPeak;
    return clamp((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w), 0.0, 1.0);
}
)";

// The scaled variant: the sharpened results at the four diorama pixels
// around the eye pixel, blended bilinearly with less weight where the local
// contrast is high (keeps edges thin).
const char* kBlitCasScaled = R"(
uniform float uCasPeak;
uniform highp vec2 uCasScale;
vec3 casLoad(ivec2 p) { return texelFetch(uTex, clamp(p, ivec2(0), textureSize(uTex, 0) - 1), 0).rgb; }
vec3 sourceColor() {
    highp vec2 pp = gl_FragCoord.xy * uCasScale - 0.5;
    highp vec2 fp = floor(pp);
    pp -= fp;
    ivec2 sp = ivec2(fp);
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    vec3 b = casLoad(sp + ivec2(0, -1));
    vec3 c = casLoad(sp + ivec2(1, -1));
    vec3 e = casLoad(sp + ivec2(-1, 0));
    vec3 f = casLoad(sp);
    vec3 g = casLoad(sp + ivec2(1, 0));
    vec3 h = casLoad(sp + ivec2(2, 0));
    vec3 i = casLoad(sp + ivec2(-1, 1));
    vec3 j = casLoad(sp + ivec2(0, 1));
    vec3 k = casLoad(sp + ivec2(1, 1));
    vec3 l = casLoad(sp + ivec2(2, 1));
    vec3 n = casLoad(sp + ivec2(0, 2));
    vec3 o = casLoad(sp + ivec2(1, 2));
    float mnf = min(min(min(b.g, e.g), min(f.g, g.g)), j.g);
    float mxf = max(max(max(b.g, e.g), max(f.g, g.g)), j.g);
    float mng = min(min(min(c.g, f.g), min(g.g, h.g)), k.g);
    float mxg = max(max(max(c.g, f.g), max(g.g, h.g)), k.g);
    float mnj = min(min(min(f.g, i.g), min(j.g, k.g)), n.g);
    float mxj = max(max(max(f.g, i.g), max(j.g, k.g)), n.g);
    float mnk = min(min(min(g.g, j.g), min(k.g, l.g)), o.g);
    float mxk = max(max(max(g.g, j.g), max(k.g, l.g)), o.g);
    float wf = sqrt(clamp(min(mnf, 1.0 - mxf) / max(mxf, 1e-4), 0.0, 1.0)) * uCasPeak;
    float wg = sqrt(clamp(min(mng, 1.0 - mxg) / max(mxg, 1e-4), 0.0, 1.0)) * uCasPeak;
    float wj = sqrt(clamp(min(mnj, 1.0 - mxj) / max(mxj, 1e-4), 0.0, 1.0)) * uCasPeak;
    float wk = sqrt(clamp(min(mnk, 1.0 - mxk) / max(mxk, 1e-4), 0.0, 1.0)) * uCasPeak;
    float s = (1.0 - pp.x) * (1.0 - pp.y) / (1.0 / 32.0 + mxf - mnf);
    float t = pp.x * (1.0 - pp.y) / (1.0 / 32.0 + mxg - mng);
    float u = (1.0 - pp.x) * pp.y / (1.0 / 32.0 + mxj - mnj);
    float v = pp.x * pp.y / (1.0 / 32.0 + mxk - mnk);
    float qbe = wf * s, qch = wg * t, qin = wj * u, qlo = wk * v;
    float qf = wg * t + wj * u + s;
    float qg = wf * s + wk * v + t;
    float qj = wf * s + wk * v + u;
    float qk = wg * t + wj * u + v;
    vec3 sum = (b + e) * qbe + (c + h) * qch + (i + n) * qin + (l + o) * qlo + f * qf + g * qg + j * qj + k * qk;
    return clamp(sum / (2.0 * (qbe + qch + qin + qlo) + qf + qg + qj + qk), 0.0, 1.0);
}
)";

const char* kQuadVs = R"(#version 320 es
uniform mat4 uMvp;
out vec2 vUv;
void main() {
    vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    vUv = vec2(p.x, p.y);
    gl_Position = uMvp * vec4(p - 0.5, 0.0, 1.0);
}
)";

// Premultiplied texture: color is scaled by coverage, so decode after
// un-premultiplying.
const char* kQuadFs = R"(#version 320 es
precision mediump float;
uniform sampler2D uTex;
uniform float uOpaque;
uniform float uBrightness;
uniform float uAlpha;
in vec2 vUv;
out vec4 oColor;
%s
void main() {
    vec4 c = texture(uTex, vUv);
    float a = mix(c.a, 1.0, uOpaque);
    vec3 straight = a > 0.0 ? c.rgb / max(a, 1e-4) : vec3(0.0);
    a *= uAlpha;
    oColor = vec4(toLinear(clamp(straight, 0.0, 1.0)) * a * uBrightness, a);
}
)";

// A UI layer's image (sRGB texture: sampling gives linear, premultiplied
// colour) composited the way the runtime does, for the headless simulator.
const char* kLayerFs = R"(#version 320 es
precision mediump float;
uniform sampler2D uTex;
uniform vec2 uUvScale;  // the part of the image used
in vec2 vUv;
out vec4 oColor;
%s
void main() { oColor = texture(uTex, vUv * uUvScale); }
)";

const char* kLaserVs = R"(#version 320 es
uniform mat4 uMvp;
uniform vec3 uCorners[4];
out vec2 vUv;
void main() {
    vUv = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    gl_Position = uMvp * vec4(uCorners[gl_VertexID], 1.0);
}
)";

// uShape 0: beam (fades along its length), 1: round reticle, 2: the skip
// indicator (uRing: fill 0..1, spinner head in turns or < 0 for none, flash
// 0..1).
const char* kLaserFs = R"(#version 320 es
precision mediump float;
uniform vec4 uColor;
uniform int uShape;
uniform vec3 uRing;
in vec2 vUv;
out vec4 oColor;
%s
// Right-pointing triangle: back edge at x = -w, apex at x = w, half-height h.
float triangle(vec2 p, float w, float h) {
    vec2 n = vec2(h, 2.0 * w) / length(vec2(h, 2.0 * w));
    return max(-w - p.x, dot(vec2(p.x - w, abs(p.y)), n));
}
void main() {
    if (uShape == 2) {
        // Premultiplied layers: a dark disc, the ring (a dim track, filled
        // clockwise from the top), a fast-forward glyph.
        vec2 q = vUv * 2.0 - 1.0;
        float r = length(q);
        float px = fwidth(r);
        vec4 c = vec4(0.0, 0.0, 0.0, 0.5 * (1.0 - smoothstep(1.0 - px, 1.0, r)));
        float band = smoothstep(0.7 - px, 0.7, r) * (1.0 - smoothstep(0.9 - px, 0.9, r));
        float turn = fract(atan(q.x, q.y) / 6.2831853 + 1.0);
        float lit;
        if (uRing.y < 0.0) {
            lit = uRing.x >= 1.0 ? 1.0 : uRing.x <= 0.0 ? 0.0 : smoothstep(turn - 0.008, turn + 0.008, uRing.x);
        } else {
            lit = max(0.0, 1.0 - fract(uRing.y - turn) / 0.45);  // a head and its fading tail
        }
        vec3 ringColor = mix(mix(vec3(0.85), vec3(1.0, 0.82, 0.25), lit), vec3(1.0), uRing.z);
        float ringA = band * mix(0.3, 1.0, max(lit, uRing.z));
        c = vec4(ringColor * ringA, ringA) + c * (1.0 - ringA);
        float g = min(triangle(q - vec2(-0.16, 0.0), 0.17, 0.32), triangle(q - vec2(0.16, 0.0), 0.17, 0.32));
        float glyphA = 0.95 * (1.0 - smoothstep(-px, px, g));
        c = vec4(vec3(glyphA), glyphA) + c * (1.0 - glyphA);
        c *= uColor.a;
        oColor = vec4(toLinear(c.rgb / max(c.a, 1e-4)) * c.a, c.a);
        return;
    }
    float a;
    if (uShape == 0) {
        a = (1.0 - abs(vUv.x * 2.0 - 1.0)) * mix(0.9, 0.15, vUv.y);
    } else {
        float d = length(vUv * 2.0 - 1.0);
        a = smoothstep(1.0, 0.7, d) * (0.55 + 0.45 * smoothstep(0.5, 0.8, d));
    }
    a *= uColor.a;
    oColor = vec4(toLinear(uColor.rgb) * a, a);
}
)";

// SpaceWarp motion vectors (see renderMotion): this frame's normalized
// device coordinates minus the last frame's, for the point seen at each
// pixel.  Full-screen: the point comes from the scene's depth (or a fixed
// depth); w carries a tag for debug views (0 world, 1 player, 0.5 fixed in
// the room).
const char* kMotionFs = R"(#version 320 es
precision highp float;
uniform highp sampler2D uDepth;
uniform mat4 uCurToPrev;       // this frame's NDC -> the last frame's clip space
uniform float uDepthOverride;  // >= 0: this depth everywhere
uniform float uMaxDepth;
uniform float uTag;
in vec2 vUv;
layout(location = 0) out vec4 oMotion;
void main() {
    float d = uDepthOverride >= 0.0 ? uDepthOverride : texture(uDepth, vUv).r;
    d = min(d, uMaxDepth);
    vec4 cur = vec4(vUv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    vec4 prev = uCurToPrev * cur;
    oMotion = vec4(prev.w > 1e-6 ? cur.xyz - prev.xyz / prev.w : vec3(0.0), uTag);
    gl_FragDepth = d;
}
)";

// A panel's motion vectors (with kQuadVs), where its texture is opaque
// enough (uAlphaMin < 0: everywhere); depth from the panel itself.
const char* kMotionQuadFs = R"(#version 320 es
precision highp float;
uniform sampler2D uTex;
uniform float uAlphaMin;
uniform mat4 uCurToPrev;
uniform vec2 uInvSize;  // 1 / motion image size
uniform float uTag;
in vec2 vUv;
layout(location = 0) out vec4 oMotion;
void main() {
    if (uAlphaMin >= 0.0 && texture(uTex, vUv).a < uAlphaMin) discard;
    vec4 cur = vec4(gl_FragCoord.xy * uInvSize * 2.0 - 1.0, gl_FragCoord.z * 2.0 - 1.0, 1.0);
    vec4 prev = uCurToPrev * cur;
    oMotion = vec4(prev.w > 1e-6 ? cur.xyz - prev.xyz / prev.w : vec3(0.0), uTag);
}
)";

// Composite programs: plain, CAS at 1:1, CAS scaling.
struct BlitProgram {
    GLuint program = 0;
    GLint tex = 0, brightness = 0, vignette = 0, vigMap = 0, wipe = 0, wipeColor = 0, casPeak = 0, casScale = 0;
};

GLuint compile(GLenum type, const char* fmt, bool withCommon) {
    char src[8192];
    if (withCommon) {
        snprintf(src, sizeof(src), fmt, kCommon);
    } else {
        snprintf(src, sizeof(src), "%s", fmt);
    }
    GLuint s = glCreateShader(type);
    const char* p = src;
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        port_fatal("vr shader compile failed: %s", log);
    }
    return s;
}

GLuint linkProgram(const char* vs, const char* fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs, false));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs, true));
    glLinkProgram(p);
    return p;
}

// kBlitFs with `source` as its sourceColor() (the common code goes in when
// compile() formats it).
BlitProgram linkBlit(const char* source) {
    char fs[8192];
    snprintf(fs, sizeof(fs), kBlitFs, "%s", source);
    BlitProgram b;
    b.program = linkProgram(kBlitVs, fs);
    b.tex = glGetUniformLocation(b.program, "uTex");
    b.brightness = glGetUniformLocation(b.program, "uBrightness");
    b.vignette = glGetUniformLocation(b.program, "uVignette");
    b.vigMap = glGetUniformLocation(b.program, "uVigMap");
    b.wipe = glGetUniformLocation(b.program, "uWipe");
    b.wipeColor = glGetUniformLocation(b.program, "uWipeColor");
    b.casPeak = glGetUniformLocation(b.program, "uCasPeak");
    b.casScale = glGetUniformLocation(b.program, "uCasScale");
    return b;
}

const GLenum kTimeElapsed = 0x88BF;  // GL_TIME_ELAPSED_EXT
const GLenum kGpuDisjoint = 0x8FBB;  // GL_GPU_DISJOINT_EXT

class GlesDraw : public Draw {
public:
    bool init() override {
        mBlit[(int)CompositeKind::Plain] = linkBlit(kBlitPlain);
        mBlit[(int)CompositeKind::Cas1] = linkBlit(kBlitCas1);
        mBlit[(int)CompositeKind::CasScaled] = linkBlit(kBlitCasScaled);
        mQuadProgram = linkProgram(kQuadVs, kQuadFs);
        mQuadTex = glGetUniformLocation(mQuadProgram, "uTex");
        mQuadMvp = glGetUniformLocation(mQuadProgram, "uMvp");
        mQuadOpaque = glGetUniformLocation(mQuadProgram, "uOpaque");
        mQuadBrightness = glGetUniformLocation(mQuadProgram, "uBrightness");
        mQuadAlpha = glGetUniformLocation(mQuadProgram, "uAlpha");
        mLayerProgram = linkProgram(kQuadVs, kLayerFs);
        mLayerTex = glGetUniformLocation(mLayerProgram, "uTex");
        mLayerMvp = glGetUniformLocation(mLayerProgram, "uMvp");
        mLayerUvScale = glGetUniformLocation(mLayerProgram, "uUvScale");
        mLaserProgram = linkProgram(kLaserVs, kLaserFs);
        mLaserMvp = glGetUniformLocation(mLaserProgram, "uMvp");
        mLaserCorners = glGetUniformLocation(mLaserProgram, "uCorners");
        mLaserColor = glGetUniformLocation(mLaserProgram, "uColor");
        mLaserShape = glGetUniformLocation(mLaserProgram, "uShape");
        mLaserRing = glGetUniformLocation(mLaserProgram, "uRing");
        mMotionProgram = linkProgram(kBlitVs, kMotionFs);
        mMotionDepth = glGetUniformLocation(mMotionProgram, "uDepth");
        mMotionMatrix = glGetUniformLocation(mMotionProgram, "uCurToPrev");
        mMotionDepthOverride = glGetUniformLocation(mMotionProgram, "uDepthOverride");
        mMotionMaxDepth = glGetUniformLocation(mMotionProgram, "uMaxDepth");
        mMotionTag = glGetUniformLocation(mMotionProgram, "uTag");
        mMotionQuadProgram = linkProgram(kQuadVs, kMotionQuadFs);
        mMotionQuadTex = glGetUniformLocation(mMotionQuadProgram, "uTex");
        mMotionQuadMvp = glGetUniformLocation(mMotionQuadProgram, "uMvp");
        mMotionQuadAlphaMin = glGetUniformLocation(mMotionQuadProgram, "uAlphaMin");
        mMotionQuadMatrix = glGetUniformLocation(mMotionQuadProgram, "uCurToPrev");
        mMotionQuadInvSize = glGetUniformLocation(mMotionQuadProgram, "uInvSize");
        mMotionQuadTag = glGetUniformLocation(mMotionQuadProgram, "uTag");
        glGenFramebuffers(1, &mMotionFbo);
        glGenVertexArrays(1, &mVao);
        return true;
    }

    // --- Passes into an image.
    void beginTarget(const gpu::EfbTarget& dst, int width, int height, bool invalidate) override {
        glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
        if (invalidate) {
            // The composite covers the whole image: no need to load its old
            // contents into the tiles.
            const GLenum color[1] = {GL_COLOR_ATTACHMENT0};
            glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, color);
        }
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }

    void clear(float r, float g, float b, float a) override {
        glClearColor(r, g, b, a);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    void composite(const Composite& c) override {
        const BlitProgram& blit = mBlit[(int)c.kind];
        glUseProgram(blit.program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, c.src);
        glBindSampler(0, 0);
        glUniform1i(blit.tex, 0);
        glUniform1f(blit.brightness, c.brightness);
        glUniform1f(blit.vignette, c.vignette);
        glUniform4f(blit.vigMap, c.vigMap[0], c.vigMap[1], c.vigMap[2], c.vigMap[3]);
        if (c.kind != CompositeKind::Plain) {
            glUniform1f(blit.casPeak, c.casPeak);
            glUniform2f(blit.casScale, (float)c.srcW / c.usedW, (float)c.srcH / c.usedH);
        }
        glUniform4f(blit.wipe, c.wipe[0], c.wipe[1], c.wipe[2], c.wipe[3]);
        glUniform4f(blit.wipeColor, c.wipeColor[0], c.wipeColor[1], c.wipeColor[2], c.wipeColor[3]);
        glBindVertexArray(mVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    void quad(TexHandle texture, const float* mvp, bool opaque, float alpha, float brightness) override {
        glUseProgram(mQuadProgram);
        glUniformMatrix4fv(mQuadMvp, 1, GL_FALSE, mvp);
        glUniform1f(mQuadOpaque, opaque ? 1.0f : 0.0f);
        glUniform1f(mQuadBrightness, brightness);
        glUniform1f(mQuadAlpha, alpha);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glBindSampler(0, 0);
        glUniform1i(mQuadTex, 0);
        if (opaque) {
            glDisable(GL_BLEND);
        } else {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        glBindVertexArray(mVao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisable(GL_BLEND);
    }

    void layerQuad(TexHandle texture, const float* mvp, float uScale, float vScale) override {
        glUseProgram(mLayerProgram);
        glUniformMatrix4fv(mLayerMvp, 1, GL_FALSE, mvp);
        glUniform2f(mLayerUvScale, uScale, vScale);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glBindSampler(0, 0);
        glUniform1i(mLayerTex, 0);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glBindVertexArray(mVao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisable(GL_BLEND);
    }

    void overlayBegin(const float* mvp) override {
        glUseProgram(mLaserProgram);
        glUniformMatrix4fv(mLaserMvp, 1, GL_FALSE, mvp);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_DEPTH_TEST);
        glBindVertexArray(mVao);
    }

    void overlayQuad(const float* corners, const float* color, int shape, const float* ring) override {
        glUniform3fv(mLaserCorners, 4, corners);
        glUniform4f(mLaserColor, color[0], color[1], color[2], color[3]);
        glUniform1i(mLaserShape, shape);
        if (ring) {
            glUniform3f(mLaserRing, ring[0], ring[1], ring[2]);
        }
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    void overlayEnd() override { glDisable(GL_BLEND); }

    void endTarget() override {
        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // --- Images.
    void uploadCanvas(TexHandle& texture, const ui::Canvas& canvas, bool mips) override {
        const int w = canvas.w, h = canvas.h;
        if (!texture) {
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            int levels = mips ? 1 + (int)floorf(log2f((float)w)) : 1;
            glTexStorage2D(GL_TEXTURE_2D, levels, GL_RGBA8, w, h);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        // Rows bottom up for GL.
        mUpload.resize((size_t)w * h);
        for (int y = 0; y < h; y++) {
            memcpy(&mUpload[(size_t)y * w], &canvas.px[(size_t)(h - 1 - y) * w], (size_t)w * 4);
        }
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, mUpload.data());
        if (mips) {
            glGenerateMipmap(GL_TEXTURE_2D);
        }
    }

    void generateMips(TexHandle texture) override {
        glBindTexture(GL_TEXTURE_2D, texture);
        glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void readPixels(const gpu::EfbTarget& image, int width, int height, uint8_t* rgba) override {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, image.fbo);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    }

    void readMotion(TexHandle motion, int width, int height, float* rgba) override {
        GLuint fbo;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, motion, 0);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_FLOAT, rgba);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
    }

    // --- SpaceWarp.
    void motionBegin(TexHandle motion, TexHandle depth, int width, int height, float maxDepth) override {
        glBindFramebuffer(GL_FRAMEBUFFER, mMotionFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, motion, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glStencilMask(0xFF);
        glBindVertexArray(mVao);

        glUseProgram(mMotionProgram);
        glUniform1f(mMotionMaxDepth, maxDepth);
        glUniform1i(mMotionDepth, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindSampler(0, 0);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
    }

    void motionScene(const gpu::EfbTarget& snapshot, int width, int height, const float* world, const float* player) override {
        // The scene's depth and player tags (the same size: an exact copy).
        glBindFramebuffer(GL_READ_FRAMEBUFFER, snapshot.fbo);
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, mMotionFbo);
        glBindTexture(GL_TEXTURE_2D, snapshot.depth);
        glEnable(GL_STENCIL_TEST);
        glStencilMask(0);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glStencilFunc(GL_EQUAL, 0, 0xFF);
        motionPass(world, -1.0f, 0.0f);
        glStencilFunc(GL_EQUAL, 1, 0xFF);
        motionPass(player, -1.0f, 1.0f);
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0xFF);
    }

    void motionFill(const float* curToPrev, float depth, float tag) override {
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClearDepthf(1.0f);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glBindTexture(GL_TEXTURE_2D, 0);
        motionPass(curToPrev, depth, tag);
    }

    void motionQuad(TexHandle texture, const float* mvp, float alphaMin, const float* curToPrev, const float* invSize, float tag) override {
        glUseProgram(mMotionQuadProgram);
        glUniformMatrix4fv(mMotionQuadMvp, 1, GL_FALSE, mvp);
        glUniformMatrix4fv(mMotionQuadMatrix, 1, GL_FALSE, curToPrev);
        glUniform1f(mMotionQuadAlphaMin, alphaMin);
        glUniform2f(mMotionQuadInvSize, invSize[0], invSize[1]);
        glUniform1f(mMotionQuadTag, tag);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glBindSampler(0, 0);
        glUniform1i(mMotionQuadTex, 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    void motionEnd() override {
        glDisable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // --- GPU timers.
    bool timersInit() override {
        const char* ext = (const char*)glGetString(GL_EXTENSIONS);
        mTimersOk = ext && strstr(ext, "GL_EXT_disjoint_timer_query");
        if (mTimersOk) {
            for (GLuint& q : mQueries) glGenQueries(1, &q);
        }
        return mTimersOk;
    }

    void timerBegin(int slot) override { glBeginQuery(kTimeElapsed, mQueries[slot]); }

    void timerEnd() override { glEndQuery(kTimeElapsed); }

    uint32_t timerPoll(uint32_t pending, float* ms, uint32_t* done) override {
        GLint disjoint = 0;
        glGetIntegerv(kGpuDisjoint, &disjoint);  // reading it also clears it
        uint32_t valid = 0;
        *done = 0;
        for (int i = 0; i < kTimerSlots; i++) {
            if (!(pending & (1u << i))) continue;
            GLuint available = 0;
            glGetQueryObjectuiv(mQueries[i], GL_QUERY_RESULT_AVAILABLE, &available);
            if (!available) continue;
            GLuint ns = 0;
            glGetQueryObjectuiv(mQueries[i], GL_QUERY_RESULT, &ns);
            *done |= 1u << i;
            if (!disjoint) {
                ms[i] = ns / 1e6f;
                valid |= 1u << i;
            }
        }
        return valid;
    }

private:
    // One pass of the motion program over the whole image.
    void motionPass(const float* curToPrev, float depthOverride, float tag) {
        glUniformMatrix4fv(mMotionMatrix, 1, GL_FALSE, curToPrev);
        glUniform1f(mMotionDepthOverride, depthOverride);
        glUniform1f(mMotionTag, tag);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    BlitProgram mBlit[3];
    GLuint mQuadProgram = 0, mLaserProgram = 0, mLayerProgram = 0, mVao = 0;
    GLint mQuadTex = 0, mQuadMvp = 0, mQuadOpaque = 0, mQuadBrightness = 0, mQuadAlpha = 0;
    GLint mLayerTex = 0, mLayerMvp = 0, mLayerUvScale = 0;
    GLint mLaserMvp = 0, mLaserCorners = 0, mLaserColor = 0, mLaserShape = 0, mLaserRing = 0;
    GLuint mMotionProgram = 0, mMotionQuadProgram = 0, mMotionFbo = 0;
    GLint mMotionDepth = 0, mMotionMatrix = 0, mMotionDepthOverride = 0, mMotionMaxDepth = 0, mMotionTag = 0;
    GLint mMotionQuadTex = 0, mMotionQuadMvp = 0, mMotionQuadAlphaMin = 0, mMotionQuadMatrix = 0, mMotionQuadInvSize = 0, mMotionQuadTag = 0;
    bool mTimersOk = false;
    GLuint mQueries[kTimerSlots] = {};
    std::vector<uint32_t> mUpload;  // a canvas flipped for GL
};

}  // namespace

Draw* createGlesDraw() { return new GlesDraw(); }

}  // namespace vrdraw
