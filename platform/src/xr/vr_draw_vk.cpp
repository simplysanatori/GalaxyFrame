// vr_draw.h on Vulkan: the pipelines behind the VR presentation's own drawing
// (composite, panels, laser, UI layer images), the panel textures, mip
// levels, read-back and GPU timers.  Recorded into the renderer's open
// command buffer (gx/vk_device.h), after the renders of the frame; image
// layouts go through the renderer's tracking (transitionImage).
//
// Orientation: the renderer's targets and the panel textures keep GL's (the
// first row in memory is the bottom of the picture); the images drawn here
// (XR swapchain images, and xr_sim.cpp's stand-ins) are read by the runtime
// with the first row at the top.  So every pass here flips the picture once
// (clip y negated in the vertex shaders) and works out GL's window
// coordinates where a shader needs them (CAS).  readPixels gives rows bottom
// up, as the GLES side does.
//
// Shaders: GLSL compiled at init through libgq_glsl.so (gx/vk_spirv.h), which
// the Vulkan renderer needs anyway.  SpaceWarp's motion passes are not
// implemented on Vulkan (SpaceWarp is off there).
#include "../gx/vk_device.h"  // first: Vulkan without prototypes
#include "../gx/vk_mem.h"
#include "../gx/vk_spirv.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "port/port.h"
#include "ui_canvas.h"
#include "vr_draw.h"

namespace xrgfx {
// xr_gfx_vk.cpp: a swapchain image is released once the work recorded on it
// has finished.
void noteVulkanImageUse(uint32_t handle);
}  // namespace xrgfx

namespace vrdraw {
namespace {

using namespace gpu::vk;

// sRGB-encoded game colors -> linear, so the sRGB swapchain stores them as is.
const char* kCommon = R"(
layout(set = 0, binding = 0) uniform sampler2D uTex;
vec3 toLinear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }
)";

// Fullscreen triangle; the picture is flipped into the image (see the top).
const char* kBlitVs = R"(#version 450
layout(location = 0) out vec2 vUv;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vUv = p;
    gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
}
)";

// The composite (vr_draw_gl.cpp's kBlitFs).  FRAG_GL: GL's window
// coordinates of the pixel (origin at the bottom of the part drawn).
const char* kBlitFsHead = R"(#version 450
layout(push_constant) uniform PC {
    vec4 params;     // brightness, vignette, CAS peak, height of the part drawn
    vec4 wipe;       // closed (0..1), kind (0 fade, 1 ring), ring centre uv
    vec4 wipeColor;  // linear colour, w = width / height of the eye
    vec4 casScale;   // xy: diorama pixels per eye image pixel
    vec4 vigMap;     // image position to the tangent of the view angle
} pc;
#define uBrightness pc.params.x
#define uVignette pc.params.y
#define uCasPeak pc.params.z
#define uWipe pc.wipe
#define uWipeColor pc.wipeColor
#define uCasScale pc.casScale.xy
#define uVigMap pc.vigMap
#define FRAG_GL vec2(gl_FragCoord.x, pc.params.w - gl_FragCoord.y)
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 oColor;
)";

const char* kBlitFsMain = R"(
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
// The same filter as vr_draw_gl.cpp's (see there), with GL's window
// coordinates from FRAG_GL.
const char* kBlitCas1 = R"(
vec3 casLoad(ivec2 p) { return texelFetch(uTex, clamp(p, ivec2(0), textureSize(uTex, 0) - 1), 0).rgb; }
vec3 sourceColor() {
    ivec2 sp = ivec2(FRAG_GL);
    vec3 a = casLoad(sp + ivec2(-1, -1));
    vec3 b = casLoad(sp + ivec2(0, -1));
    vec3 c = casLoad(sp + ivec2(1, -1));
    vec3 d = casLoad(sp + ivec2(-1, 0));
    vec3 e = casLoad(sp);
    vec3 f = casLoad(sp + ivec2(1, 0));
    vec3 g = casLoad(sp + ivec2(-1, 1));
    vec3 h = casLoad(sp + ivec2(0, 1));
    vec3 i = casLoad(sp + ivec2(1, 1));
    float mn = min(min(min(d.g, e.g), min(f.g, b.g)), h.g);
    float mx = max(max(max(d.g, e.g), max(f.g, b.g)), h.g);
    mn += min(min(min(mn, a.g), min(c.g, g.g)), i.g);
    mx += max(max(max(mx, a.g), max(c.g, g.g)), i.g);
    float amp = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, 1e-4), 0.0, 1.0));
    float w = amp * uCasPeak;
    return clamp((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w), 0.0, 1.0);
}
)";

const char* kBlitCasScaled = R"(
vec3 casLoad(ivec2 p) { return texelFetch(uTex, clamp(p, ivec2(0), textureSize(uTex, 0) - 1), 0).rgb; }
vec3 sourceColor() {
    vec2 pp = FRAG_GL * uCasScale - 0.5;
    vec2 fp = floor(pp);
    pp -= fp;
    ivec2 sp = ivec2(fp);
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

// A unit quad through the matrix (vr_draw_gl.cpp's kQuadVs), flipped.
const char* kQuadVs = R"(#version 450
layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 params;
} pc;
layout(location = 0) out vec2 vUv;
void main() {
    vec2 p = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    vUv = p;
    gl_Position = pc.mvp * vec4(p - 0.5, 0.0, 1.0);
    gl_Position.y = -gl_Position.y;
}
)";

// params: opaque, brightness, alpha.
const char* kQuadFs = R"(#version 450
layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 params;
} pc;
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 oColor;
%s
void main() {
    vec4 c = texture(uTex, vUv);
    float a = mix(c.a, 1.0, pc.params.x);
    vec3 straight = a > 0.0 ? c.rgb / max(a, 1e-4) : vec3(0.0);
    a *= pc.params.z;
    oColor = vec4(toLinear(clamp(straight, 0.0, 1.0)) * a * pc.params.y, a);
}
)";

// A UI layer's image (stored top row first, like a swapchain's) composited
// the way the runtime does; params.xy: the part of the image used.
const char* kLayerFs = R"(#version 450
layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 params;
} pc;
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 oColor;
%s
void main() { oColor = texture(uTex, vec2(vUv.x * pc.params.x, (1.0 - vUv.y) * pc.params.y)); }
)";

// Beams, reticles and the skip ring: the corners come in clip space.
const char* kOverlayVs = R"(#version 450
layout(push_constant) uniform PC {
    vec4 corner[4];
    vec4 color;
    vec4 ring;  // xyz: the skip ring's, w: the shape
} pc;
layout(location = 0) out vec2 vUv;
void main() {
    vUv = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    gl_Position = pc.corner[gl_VertexIndex];
    gl_Position.y = -gl_Position.y;
}
)";

// vr_draw_gl.cpp's kLaserFs.
const char* kOverlayFs = R"(#version 450
layout(push_constant) uniform PC {
    vec4 corner[4];
    vec4 color;
    vec4 ring;
} pc;
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 oColor;
%s
float triangle(vec2 p, float w, float h) {
    vec2 n = vec2(h, 2.0 * w) / length(vec2(h, 2.0 * w));
    return max(-w - p.x, dot(vec2(p.x - w, abs(p.y)), n));
}
void main() {
    vec4 uColor = pc.color;
    vec3 uRing = pc.ring.xyz;
    int uShape = int(pc.ring.w + 0.5);
    if (uShape == 2) {
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
            lit = max(0.0, 1.0 - fract(uRing.y - turn) / 0.45);
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

enum Program { kCompPlain, kCompCas1, kCompCasScaled, kQuadOpaque, kQuadBlend, kLayer, kOverlay, kPrograms };

struct BlitPush {
    float params[4];
    float wipe[4];
    float wipeColor[4];
    float casScale[4];
    float vigMap[4];
};
struct QuadPush {
    float mvp[16];
    float params[4];
};
struct OverlayPush {
    float corner[4][4];
    float color[4];
    float ring[4];
};

const VkPipelineStageFlags2 kColorStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
const VkAccessFlags2 kColorAccess = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;

class VulkanDraw : public Draw {
public:
    bool init() override {
        // Headless (xr_sim.cpp) there is no device before the renderer's
        // init: the same one it will take.
        if (!deviceReady() && !createStandalone()) {
            port_log("vr vk: no Vulkan device");
            return false;
        }
        VkDevice dev = device().device;
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(dev, &si, nullptr, &mSampler) != VK_SUCCESS) return false;
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        dl.bindingCount = 1;
        dl.pBindings = &b;
        if (vkCreateDescriptorSetLayout(dev, &dl, nullptr, &mSetLayout) != VK_SUCCESS) return false;
        VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &mSetLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(dev, &pl, nullptr, &mLayout) != VK_SUCCESS) return false;

        std::string blitFs = std::string(kBlitFsHead) + kCommon;
        mVs[kCompPlain] = mVs[kCompCas1] = mVs[kCompCasScaled] = module(gpu::SpirvStage::Vertex, kBlitVs, "blit vs");
        mFs[kCompPlain] = module(gpu::SpirvStage::Fragment, blitFs + kBlitPlain + kBlitFsMain, "composite fs");
        mFs[kCompCas1] = module(gpu::SpirvStage::Fragment, blitFs + kBlitCas1 + kBlitFsMain, "composite CAS fs");
        mFs[kCompCasScaled] = module(gpu::SpirvStage::Fragment, blitFs + kBlitCasScaled + kBlitFsMain, "composite scaling CAS fs");
        mVs[kQuadOpaque] = mVs[kQuadBlend] = mVs[kLayer] = module(gpu::SpirvStage::Vertex, kQuadVs, "quad vs");
        mFs[kQuadOpaque] = mFs[kQuadBlend] = module(gpu::SpirvStage::Fragment, withCommon(kQuadFs), "quad fs");
        mFs[kLayer] = module(gpu::SpirvStage::Fragment, withCommon(kLayerFs), "layer fs");
        mVs[kOverlay] = module(gpu::SpirvStage::Vertex, kOverlayVs, "overlay vs");
        mFs[kOverlay] = module(gpu::SpirvStage::Fragment, withCommon(kOverlayFs), "overlay fs");
        for (int p = 0; p < kPrograms; p++) {
            if (!mVs[p] || !mFs[p]) {
                port_log("vr vk: presentation shaders failed to compile");
                return false;
            }
        }
        mReady = true;
        // A 1x1 texture for the passes that sample nothing (the pipeline
        // layout has a sampler binding).
        ui::Canvas c;
        c.w = c.h = 1;
        c.px.assign(1, 0);
        uploadCanvas(mDummy, c, false);
        return mDummy != 0;
    }

    // --- Passes into an image.
    void beginTarget(const gpu::EfbTarget& dst, int width, int height, bool invalidate) override {
        suspend();
        mDst = dst.color;
        mW = width;
        mH = height;
        mLoad = invalidate ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
        ImageInfo info;
        mFormat = imageInfo(mDst, &info) ? info.format : VK_FORMAT_UNDEFINED;
        mAttView = info.attView;
        mInPass = mFormat != VK_FORMAT_UNDEFINED && mReady;
    }

    void clear(float r, float g, float b, float a) override {
        if (!rendering()) return;
        VkClearAttachment ca{};
        ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ca.colorAttachment = 0;
        ca.clearValue.color = {{r, g, b, a}};
        VkClearRect rect{{{0, 0}, {(uint32_t)mW, (uint32_t)mH}}, 0, 1};
        vkCmdClearAttachments(commandBuffer(), 1, &ca, 1, &rect);
    }

    void composite(const Composite& c) override {
        int prog = c.kind == CompositeKind::Plain ? kCompPlain : c.kind == CompositeKind::Cas1 ? kCompCas1 : kCompCasScaled;
        if (!sampled(c.src) || !rendering()) return;
        BlitPush pc{};
        pc.params[0] = c.brightness;
        pc.params[1] = c.vignette;
        pc.params[2] = c.casPeak;
        pc.params[3] = (float)mH;
        memcpy(pc.vigMap, c.vigMap, sizeof(pc.vigMap));
        memcpy(pc.wipe, c.wipe, sizeof(pc.wipe));
        memcpy(pc.wipeColor, c.wipeColor, sizeof(pc.wipeColor));
        if (c.kind != CompositeKind::Plain) {
            pc.casScale[0] = (float)c.srcW / c.usedW;
            pc.casScale[1] = (float)c.srcH / c.usedH;
        }
        draw(prog, c.src, &pc, sizeof(pc), 3);
    }

    void quad(TexHandle texture, const float* mvp, bool opaque, float alpha, float brightness) override {
        if (!sampled(texture) || !rendering()) return;
        QuadPush pc{};
        memcpy(pc.mvp, mvp, sizeof(pc.mvp));
        pc.params[0] = opaque ? 1.0f : 0.0f;
        pc.params[1] = brightness;
        pc.params[2] = alpha;
        draw(opaque ? kQuadOpaque : kQuadBlend, texture, &pc, sizeof(pc), 4);
    }

    void layerQuad(TexHandle texture, const float* mvp, float uScale, float vScale) override {
        if (!sampled(texture) || !rendering()) return;
        QuadPush pc{};
        memcpy(pc.mvp, mvp, sizeof(pc.mvp));
        pc.params[0] = uScale;
        pc.params[1] = vScale;
        draw(kLayer, texture, &pc, sizeof(pc), 4);
    }

    void overlayBegin(const float* mvp) override { memcpy(mOverlayMvp, mvp, sizeof(mOverlayMvp)); }

    void overlayQuad(const float* corners, const float* color, int shape, const float* ring) override {
        if (!sampled(mDummy) || !rendering()) return;
        OverlayPush pc{};
        // The corners into clip space here: the push constants hold no matrix.
        const float* m = mOverlayMvp;
        for (int i = 0; i < 4; i++) {
            const float* p = corners + i * 3;
            for (int r = 0; r < 4; r++) {
                pc.corner[i][r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
            }
        }
        memcpy(pc.color, color, sizeof(pc.color));
        if (ring) {
            mRing[0] = ring[0];
            mRing[1] = ring[1];
            mRing[2] = ring[2];
        }
        pc.ring[0] = mRing[0];
        pc.ring[1] = mRing[1];
        pc.ring[2] = mRing[2];
        pc.ring[3] = (float)shape;
        draw(kOverlay, mDummy, &pc, sizeof(pc), 4);
    }

    void overlayEnd() override {}

    void endTarget() override {
        suspend();
        mInPass = false;
    }

    // --- Images.
    void uploadCanvas(TexHandle& texture, const ui::Canvas& canvas, bool mips) override {
        const int w = canvas.w, h = canvas.h;
        if (!mReady || w <= 0 || h <= 0) return;
        suspend();
        uint32_t levels = mips ? 1 + (uint32_t)floorf(log2f((float)w)) : 1;
        if (!texture) {
            texture = createTexture(w, h, levels, "vr panel texture");
            if (!texture) return;
        }
        ImageInfo info;
        if (!imageInfo(texture, &info)) return;
        // Rows bottom up, as the GLES side stores them.
        ScratchAlloc s = scratch((VkDeviceSize)w * h * 4, 16);
        if (!s.ptr) return;
        uint8_t* dst = (uint8_t*)s.ptr;
        for (int y = 0; y < h; y++) {
            memcpy(dst + (size_t)y * w * 4, &canvas.px[(size_t)(h - 1 - y) * w], (size_t)w * 4);
        }
        transitionImage(texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.bufferOffset = s.offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
        vkCmdCopyBufferToImage(commandBuffer(), s.buffer, info.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        if (levels > 1) {
            blitMips(texture, info, levels);
        }
        transitionImage(texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    void generateMips(TexHandle texture) override {
        suspend();
        ImageInfo info;
        if (!imageInfo(texture, &info)) return;
        uint32_t levels = 1;
        while ((std::max(info.width, info.height) >> levels) > 0) levels++;
        auto it = mLevels.find(texture);
        if (it != mLevels.end()) levels = it->second;
        transitionImage(texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        blitMips(texture, info, levels);
        xrgfx::noteVulkanImageUse(texture);
    }

    void readPixels(const gpu::EfbTarget& image, int width, int height, uint8_t* rgba) override {
        suspend();
        // The renderer's read-back copies the first `height` rows as stored
        // (here the top of the picture first): turned over for rows bottom up.
        gpu::Target t;
        t.fbo = t.color = image.color;
        t.width = width;
        t.height = height;
        mRead.resize((size_t)width * height * 4);
        if (!gpu::renderer().readPixels(t, mRead.data())) {
            memset(rgba, 0, (size_t)width * height * 4);
            return;
        }
        for (int y = 0; y < height; y++) {
            memcpy(rgba + (size_t)y * width * 4, &mRead[(size_t)(height - 1 - y) * width * 4], (size_t)width * 4);
        }
    }

    void readMotion(TexHandle, int width, int height, float* rgba) override { memset(rgba, 0, (size_t)width * height * 4 * sizeof(float)); }

    // --- SpaceWarp: not on Vulkan (xr_app.cpp keeps it off).
    void motionBegin(TexHandle, TexHandle, int, int, float) override {}
    void motionScene(const gpu::EfbTarget&, int, int, const float*, const float*) override {}
    void motionFill(const float*, float, float) override {}
    void motionQuad(TexHandle, const float*, float, const float*, const float*, float) override {}
    void motionEnd() override {}

    // --- GPU timers: a pair of timestamps per slot, read once the
    // submission that holds them has finished.
    bool timersInit() override {
        if (!mReady) return false;
        const Device& d = device();
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d.phys, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d.phys, &qn, qf.data());
        if (d.queueFamily >= qn || qf[d.queueFamily].timestampValidBits == 0 || d.props.limits.timestampPeriod <= 0.0f) return false;
        mTickMs = d.props.limits.timestampPeriod / 1e6;
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = kTimerSlots * 2;
        if (vkCreateQueryPool(d.device, &qci, nullptr, &mQueries) != VK_SUCCESS) return false;
        vkResetQueryPool(d.device, mQueries, 0, kTimerSlots * 2);
        return true;
    }

    void timerBegin(int slot) override {
        if (!mQueries) return;
        suspend();
        VkCommandBuffer cmd = commandBuffer();
        vkCmdResetQueryPool(cmd, mQueries, (uint32_t)slot * 2, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mQueries, (uint32_t)slot * 2);
        mTimerSlot = slot;
    }

    void timerEnd() override {
        if (!mQueries || mTimerSlot < 0) return;
        vkCmdWriteTimestamp(commandBuffer(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, mQueries, (uint32_t)mTimerSlot * 2 + 1);
        mTimerValue[mTimerSlot] = pendingValue();
        mTimerSlot = -1;
    }

    uint32_t timerPoll(uint32_t pending, float* ms, uint32_t* done) override {
        *done = 0;
        uint32_t valid = 0;
        if (!mQueries) return 0;
        uint64_t completed = completedValue();
        for (int i = 0; i < kTimerSlots; i++) {
            // Only once the submission that holds the pair has finished: before
            // that the pool may still show the slot's last use.
            if (!(pending & (1u << i)) || mTimerValue[i] > completed) continue;
            uint64_t t[2];
            VkResult r = vkGetQueryPoolResults(device().device, mQueries, (uint32_t)i * 2, 2, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
            *done |= 1u << i;
            if (r == VK_SUCCESS && t[1] >= t[0]) {
                ms[i] = (float)((double)(t[1] - t[0]) * mTickMs);
                valid |= 1u << i;
            }
        }
        return valid;
    }

private:
    std::string withCommon(const char* fmt) {
        char src[8192];
        snprintf(src, sizeof(src), fmt, kCommon);
        return src;
    }

    VkShaderModule module(gpu::SpirvStage stage, const std::string& glsl, const char* what) {
        std::vector<uint32_t> words;
        std::string log;
        if (!gpu::spirvCompileGlsl(stage, glsl, &words, &log)) {
            port_log("vr vk: %s: %s", what, log.c_str());
            return VK_NULL_HANDLE;
        }
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = words.size() * 4;
        ci.pCode = words.data();
        VkShaderModule m = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device().device, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
        return m;
    }

    // The pipeline of a program for an attachment format, made on first use.
    VkPipeline pipeline(int prog, VkFormat format) {
        uint64_t key = (uint64_t)prog << 32 | (uint32_t)format;
        auto it = mPipes.find(key);
        if (it != mPipes.end()) return it->second;
        VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = mVs[prog];
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = mFs[prog];
        st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = prog <= kCompCasScaled ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (prog == kQuadBlend || prog == kLayer || prog == kOverlay) {
            // Premultiplied: glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            ba.colorBlendOp = ba.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &ba;
        const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfoKHR ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
        ri.colorAttachmentCount = 1;
        ri.pColorAttachmentFormats = &format;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pi.pNext = &ri;
        pi.stageCount = 2;
        pi.pStages = st;
        pi.pVertexInputState = &vi;
        pi.pInputAssemblyState = &ia;
        pi.pViewportState = &vp;
        pi.pRasterizationState = &rs;
        pi.pMultisampleState = &ms;
        pi.pColorBlendState = &cb;
        pi.pDynamicState = &ds;
        pi.layout = mLayout;
        VkPipeline p = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(device().device, VK_NULL_HANDLE, 1, &pi, nullptr, &p) != VK_SUCCESS) {
            port_log("vr vk: pipeline %d for format %d failed", prog, (int)format);
        }
        mPipes[key] = p;
        return p;
    }

    // A texture to sample in the pass: moved to the sampling layout first
    // (outside the rendering instance), unless it is there already.
    bool sampled(TexHandle tex) {
        if (!mInPass) return false;
        ImageInfo info;
        if (!imageInfo(tex, &info)) return false;
        if (info.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            suspend();
            transitionImage(tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        return true;
    }

    // The rendering instance into the pass's image, begun on the first draw
    // (and again after a suspend, loading what was drawn).
    bool rendering() {
        if (!mInPass) return false;
        if (mRendering) return true;
        transitionImage(mDst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kColorStage, kColorAccess);
        VkRenderingAttachmentInfoKHR att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
        att.imageView = mAttView;
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = mLoad;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfoKHR ri{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
        ri.renderArea = {{0, 0}, {(uint32_t)mW, (uint32_t)mH}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        VkCommandBuffer cmd = commandBuffer();
        xrgfx::noteVulkanImageUse(mDst);
        vkCmdBeginRenderingKHR(cmd, &ri);
        VkViewport vp{0.0f, 0.0f, (float)mW, (float)mH, 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {(uint32_t)mW, (uint32_t)mH}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        mLoad = VK_ATTACHMENT_LOAD_OP_LOAD;  // a resumed instance keeps what was drawn
        mRendering = true;
        return true;
    }

    void suspend() {
        if (!mRendering) return;
        vkCmdEndRenderingKHR(commandBuffer());
        mRendering = false;
    }

    void draw(int prog, TexHandle tex, const void* push, uint32_t size, uint32_t vertices) {
        VkPipeline p = pipeline(prog, mFormat);
        ImageInfo info;
        if (!p || !imageInfo(tex, &info)) return;
        VkCommandBuffer cmd = commandBuffer();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        VkDescriptorImageInfo ii{mSampler, info.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 0, 1, &w);
        vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size, push);
        vkCmdDraw(cmd, vertices, 1, 0, 0);
    }

    TexHandle createTexture(int w, int h, uint32_t levels, const char* name) {
        VkDevice dev = device().device;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent = {(uint32_t)w, (uint32_t)h, 1};
        ci.mipLevels = levels;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage image = VK_NULL_HANDLE;
        if (vkCreateImage(dev, &ci, nullptr, &image) != VK_SUCCESS) return 0;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, image, &req);
        Allocation mem;
        if (!allocateMemory(req, MemUse::Device, MemCat::Texture, false, &mem, name) || vkBindImageMemory(dev, image, mem.memory, 0) != VK_SUCCESS) {
            vkDestroyImage(dev, image, nullptr);
            return 0;
        }
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = ci.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(dev, &vci, nullptr, &view) != VK_SUCCESS) return 0;
        setName(VK_OBJECT_TYPE_IMAGE, (uint64_t)image, name);
        TexHandle h0 = registerImage(image, view, view, ci.format, (uint32_t)w, (uint32_t)h, VK_IMAGE_LAYOUT_UNDEFINED);
        mLevels[h0] = levels;
        return h0;
    }

    // Levels 1.. from level 0 (in TRANSFER_DST with the others), each by a
    // linear blit from the one above; all levels end in TRANSFER_SRC.
    void blitMips(TexHandle tex, const ImageInfo& info, uint32_t levels) {
        VkCommandBuffer cmd = commandBuffer();
        auto barrier = [&](uint32_t level, VkImageLayout from, VkImageLayout to, VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess) {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            b.srcAccessMask = srcAccess;
            b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            b.dstAccessMask = dstAccess;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = info.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            di.imageMemoryBarrierCount = 1;
            di.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2KHR(cmd, &di);
        };
        barrier(0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        int32_t w = (int32_t)info.width, h = (int32_t)info.height;
        for (uint32_t i = 1; i < levels; i++) {
            int32_t nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
            blit.srcOffsets[1] = {w, h, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
            blit.dstOffsets[1] = {nw, nh, 1};
            vkCmdBlitImage(cmd, info.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, info.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            barrier(i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            w = nw;
            h = nh;
        }
        // The renderer's tracking: every level in TRANSFER_SRC, written last.
        assumeImageLayout(tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }

    bool mReady = false;
    VkSampler mSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout mLayout = VK_NULL_HANDLE;
    VkShaderModule mVs[kPrograms] = {}, mFs[kPrograms] = {};
    std::unordered_map<uint64_t, VkPipeline> mPipes;
    std::unordered_map<TexHandle, uint32_t> mLevels;  // mip levels of the textures made here
    TexHandle mDummy = 0;

    // The pass (beginTarget .. endTarget).
    bool mInPass = false, mRendering = false;
    uint32_t mDst = 0;
    int mW = 0, mH = 0;
    VkFormat mFormat = VK_FORMAT_UNDEFINED;
    VkImageView mAttView = VK_NULL_HANDLE;
    VkAttachmentLoadOp mLoad = VK_ATTACHMENT_LOAD_OP_LOAD;
    float mOverlayMvp[16] = {};
    float mRing[3] = {};

    std::vector<uint8_t> mRead;

    VkQueryPool mQueries = VK_NULL_HANDLE;
    double mTickMs = 0.0;
    int mTimerSlot = -1;
    uint64_t mTimerValue[kTimerSlots] = {};
};

}  // namespace

Draw* createVulkanDraw() { return new VulkanDraw(); }

}  // namespace vrdraw
