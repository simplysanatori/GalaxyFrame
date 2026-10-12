// What the VR presentation draws itself, apart from the game's frames (the
// renderer replays those): the composite of the diorama into the eye image,
// panels, the laser and its reticle, the skip ring, the UI layer images, the
// SpaceWarp motion passes, and the GPU timers that judge the eyes.  The
// layout, the pointer and the other logic of the presentation stay in
// vr_game.cpp, vr_settings.cpp and vr_setup.cpp; everything that calls the
// graphics API is behind this interface.
//
// One implementation per API: vr_draw_gl.cpp and, later, vr_draw_vk.cpp.
// Images are gpu::EfbTargets (the swapchain images from xr_gfx.h, the
// renderer's own targets) and gpu::TexHandles; matrices are column-major 4x4
// as xm::Mat4 keeps them.  All calls are made on the render thread (the XR
// main thread).
//
// Conventions for the final images: premultiplied alpha in the UI layers,
// rows stored bottom up (GL order: the first row of an image in memory is the
// bottom of the picture), clip z in -w..w.
#pragma once

#include <stdint.h>

#include "../gx/gl_renderer.h"
#include "../gx/gx_record.h"
#include "xr_gfx.h"

namespace ui {
struct Canvas;
}

namespace vrdraw {

using gpu::TexHandle;

// How the composite takes the diorama's colour.
enum class CompositeKind {
    Plain,      // bilinear (the scale of the target, if any)
    Cas1,       // AMD FidelityFX CAS, the diorama and the image the same size
    CasScaled,  // CAS, scaling to the image
};

// The composite of the diorama into an eye image (Draw::composite).
struct Composite {
    TexHandle src = 0;      // the diorama's colour
    int srcW = 0, srcH = 0;  // its size
    int usedW = 0, usedH = 0;  // the part of the eye image written (from its lower left corner)
    CompositeKind kind = CompositeKind::Plain;
    float brightness = 1.0f;
    // The comfort vignette: 0 = off .. 1 = strong.  Its shape is in the angle
    // from the view's axis, so it does not depend on how wide the eye image
    // is: clear in the middle (to 55 degrees at 0, to 34 at 1: Mario, 25 to 30
    // degrees below the axis, stays clear), darkening
    // to `vignette` of the brightness (as displayed) 30 degrees further out.
    // vigMap turns a position of the image (-1..1) into the tangent of that
    // angle: tan x = x * vigMap[0] + vigMap[1], tan y = y * vigMap[2] + vigMap[3].
    float vignette = 0.0f;
    float vigMap[4] = {1.0f, 0.0f, 1.0f, 0.0f};
    float casPeak = 0.0f;    // CAS: -1 / mix(8, 5, sharpness)
    float wipe[4] = {};      // closed (0..1), kind (0 fade, 1 ring), ring centre uv
    float wipeColor[4] = {};  // linear colour, w = width / height of the eye
};

// Shapes of Draw::overlayQuad.
enum { kShapeBeam, kShapeReticle, kShapeSkipRing };

class Draw {
public:
    virtual ~Draw() = default;

    // Programs and state objects, on the thread that renders (GLES: with the
    // context current).
    virtual bool init() = 0;

    // --- Passes into an image (an eye image, a UI layer image).
    // Starts drawing into `dst`: the viewport is its lower left
    // `width` x `height` part, nothing is tested, culled or scissored, all
    // channels are written.  `invalidate`: the old contents are not needed.
    virtual void beginTarget(const gpu::EfbTarget& dst, int width, int height, bool invalidate) = 0;
    virtual void clear(float r, float g, float b, float a) = 0;
    // The diorama into the image, with the vignette and the screen wipe.
    virtual void composite(const Composite& c) = 0;
    // A textured unit quad (centred on the origin, facing +Z) through `mvp`:
    // `opaque` ignores the texture's alpha; otherwise it is premultiplied and
    // blended over.  The colour is decoded from sRGB (the texture is
    // premultiplied), then scaled by `brightness` and `alpha`.
    virtual void quad(TexHandle texture, const float* mvp, bool opaque, float alpha, float brightness) = 0;
    // A UI layer's image drawn the way the compositor does, with the part of
    // it used (`uScale`, `vScale` of the image): the headless simulator.
    virtual void layerQuad(TexHandle texture, const float* mvp, float uScale, float vScale) = 0;
    // Beams, reticles and the skip ring: overlayBegin sets `mvp` for the
    // quads that follow; each quad is four corners (a triangle strip,
    // 12 floats) in the space `mvp` maps, with a premultiplied colour
    // (`color[3]` alpha); `ring` (3 floats) is for kShapeSkipRing only.
    virtual void overlayBegin(const float* mvp) = 0;
    virtual void overlayQuad(const float* corners, const float* color, int shape, const float* ring) = 0;
    virtual void overlayEnd() = 0;
    // Ends the pass.
    virtual void endTarget() = 0;

    // --- Images.
    // Creates the texture of a panel's picture on first use (full mip
    // chain with `mips`, linear filtering, clamped) and uploads the canvas:
    // `canvas` has its first row at the top, the texture is stored bottom up.
    virtual void uploadCanvas(TexHandle& texture, const ui::Canvas& canvas, bool mips) = 0;
    // Fills in the mip levels below level 0.
    virtual void generateMips(TexHandle texture) = 0;
    // Reads the lower left `width` x `height` of an image's colour as RGBA8,
    // rows bottom up.
    virtual void readPixels(const gpu::EfbTarget& image, int width, int height, uint8_t* rgba) = 0;
    // Reads a motion vector image (RGBA16F) as floats, rows bottom up.
    virtual void readMotion(TexHandle motion, int width, int height, float* rgba) = 0;

    // --- SpaceWarp.  Starts the motion pass into the two images (the
    // motion vectors, RGBA16F, and the depth with stencil).
    // No depth is written beyond `maxDepth`.
    virtual void motionBegin(TexHandle motion, TexHandle depth, int width, int height, float maxDepth) = 0;
    // The scene: its depth and the player tags in `snapshot` (the same size)
    // copied as the depth, then the motion of what the tags mark as the world
    // (`world`) and as the player (`player`): matrices from this frame's NDC
    // to the last frame's clip space.
    virtual void motionScene(const gpu::EfbTarget& snapshot, int width, int height, const float* world, const float* player) = 0;
    // No scene: clear, then one fullscreen pass at a fixed depth.
    virtual void motionFill(const float* curToPrev, float depth, float tag) = 0;
    // A panel's motion vectors where its texture has alpha of `alphaMin` or
    // more (< 0: everywhere); `invSize` is 1 / the motion image size.
    virtual void motionQuad(TexHandle texture, const float* mvp, float alphaMin, const float* curToPrev, const float* invSize, float tag) = 0;
    virtual void motionEnd() = 0;

    // --- GPU timers for the eyes: `slots` timers, each timing the work
    // between timerBegin(slot) and timerEnd().
    static const int kTimerSlots = 8;
    // False if the GPU cannot time its work.
    virtual bool timersInit() = 0;
    virtual void timerBegin(int slot) = 0;
    virtual void timerEnd() = 0;
    // Looks at the timers in `pending` (a bit for each slot).  `*done` gets
    // the slots whose result was ready (and is not asked for again); the
    // return value has those among them with a usable time, in `ms[slot]`
    // (a GPU that was interrupted since the last call gives none).
    virtual uint32_t timerPoll(uint32_t pending, float* ms, uint32_t* done) = 0;
    // A GPU profile mark (Vulkan with PETARI_VKPROF; see vk::profMark): the
    // GPU time from here to the next mark goes to `label`.
    virtual void profileMark(const char* label) { (void)label; }
    // Logs the profile since the last report, per `per` frames (vk::profReport).
    virtual void profileReport(const char* why, int per) { (void)why, (void)per; }
};

// The implementation for the API xrgfx selected (see xrgfx::select).
Draw& draw();

// The implementations (null if the build has none).
Draw* createGlesDraw();
Draw* createVulkanDraw();

}  // namespace vrdraw
