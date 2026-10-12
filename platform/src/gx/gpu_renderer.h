// Render-thread half of the renderer, independent of the graphics API:
// turns recorded frames (gx_record.h) into draws.  A frame is prepared once
// (uploads, programs, per-draw state) and can then be executed any number
// of times: per eye, per display refresh, or once for a flat view.
//
// Two backends implement gpu::Renderer: GLES 3.2 (gl_renderer.cpp) and
// Vulkan (vk_renderer.cpp, when built).  Both share the preparation of a
// frame (gpu_prepare.h).  Nothing here includes a graphics API header.
#pragma once

#include <stdint.h>

#include <memory>

#include "gx_record.h"

namespace gpu {

// A texture of the active backend: a GL texture name under GLES, an image
// slot index under Vulkan.  0 = none.  (Declared in gx_record.h, which
// TexImage needs it.)
//   using TexHandle = uint32_t;

// A render target: color + depth-stencil the game's draws render into (an
// "EFB"), or depth-stencil only (createDepthTarget).  Under GLES `fbo` is
// the framebuffer name and `color` / `depth` texture names; under Vulkan
// `fbo` is the target's slot index and `color` / `depth` image slot indices.
// 0 = none.
struct Target {
    uint32_t fbo = 0;
    TexHandle color = 0, depth = 0;
    int width = 0, height = 0;
};
using EfbTarget = Target;  // the name before the backend split

// The frame's main 3D camera as recorded by the game (CMD_CAMERA).
struct CameraInfo {
    bool valid = false;
    float view[12];   // world -> view, 3x4 rows
    float pos[3];     // camera position (world)
    float watch[3];   // point the camera looks at (usually the player)
    float up[3];      // camera up vector
    float watchUp[3]; // up (anti-gravity) at the watch point
    float fovy;       // degrees
    uint32_t flags;   // PORT_GX_CAMERA_* hints
    float aspect;     // width / height of the game's projection
    float player[3];  // player centre (world), with PORT_GX_CAMERA_PLAYER
    float playerUp[3];  // against the player's gravity (world); zero while it has none
};

// How draws between the HUD markers are handled.
enum class HudMode {
    Inline,  // drawn into the EFB like everything else
    Target,  // drawn into the separate HUD target (premultiplied alpha)
    Skip,    // not drawn (the HUD was rendered for another eye)
};

// VR camera for perspective 3D draws (row-major 4x4 matrices).
struct EyeView {
    float view[16];  // game view space -> eye space (head tracking, IPD, world scale)
    float proj[16];  // eye space -> GL clip space (z in -w..w on both backends)
    // Cutaway: 3D geometry inside a cone from the eye to `focus` (eye space,
    // metres; w > 0 enables it) and inside a sphere around the eye is
    // dithered out, so walls never hide the player.  cut = radius at the eye,
    // radius at the focus, distance before the focus where the cut ends,
    // radius of the sphere around the eye.
    float focus[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float cut[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // The eye's position in game view space: lights the game puts on its
    // camera (view space origin) are lit from here instead.
    float eyePos[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    int index = 0;  // 0 = left, 1 = right: each eye keeps its own EFB copies
    // One picture of a stereo pair of the flat view instead (view, proj and
    // the cutaway are not used): the frame is replayed with the game's own
    // camera, viewports and projection, and the 3D draws a VR eye would take
    // are shifted sideways in clip space by
    //   stereo[0] * min(stereo[1] - w, stereo[2] * w)
    // with w the depth in front of the camera (game units).  stereo[0]: the
    // shift of the farthest things, in half screen widths, positive for the
    // left picture and negative for the right one; stereo[1]: the depth
    // that lands where it does without the shift (the screen's own depth to
    // the two eyes); stereo[2]: how many times farther than the farthest
    // things the nearest ones may shift the other way; stereo[3]: the shift
    // of the draws between the HUD markers, in half screen widths (positive
    // for the left picture: the HUD floats before the screen).
    // pointer[0]: the shift of the pointer's 2D cursor (the draws between
    // the pointer markers) while it points into the scene: that of the point
    // under it; pointer[1]: nonzero while it points at menus instead, and
    // goes with the HUD.
    bool flatStereo = false;
    float stereo[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float pointer[2] = {0.0f, 0.0f};
    // SpaceWarp: with a target here, draws between the player markers leave
    // 1 in the stencil buffer where they write depth (other draws 0), and at
    // the scene depth marker the depth and stencil are copied into the
    // target (a depth-stencil target of any size, see createDepthTarget).
    Target* depthSnapshot = nullptr;
};

// The graphics API behind the renderer.
enum class Api {
    Gles,
    Vulkan,
};

class Renderer {
public:
    virtual ~Renderer() = default;

    virtual Api api() const = 0;
    // Creates the backend's objects (GLES: on the thread with the current
    // context).  Returns false if the GPU cannot run the renderer.
    virtual bool init() = 0;

    virtual Target createTarget(int width, int height) = 0;
    // Depth-stencil only (no color), sampled with nearest filtering.
    virtual Target createDepthTarget(int width, int height) = 0;
    virtual void destroyTarget(Target& t) = 0;
    // Fixed foveated rendering for a target (GLES: GL_QCOM_texture_foveated):
    // resolution falls off away from the focal point (normalized device
    // coordinates) at the given gain.  Returns false if unsupported.
    virtual bool setFoveation(Target& t, float focalX, float focalY, float gain, float foveaArea) = 0;

    // Makes `frame` the frame to render, preparing it on this thread if it
    // is new.
    virtual void setFrame(std::shared_ptr<const Frame> frame) = 0;
    // Asks the preparation thread for the newest recorded frame and switches
    // to the most recently prepared one.  Returns true if the frame changed.
    virtual bool update() = 0;
    virtual bool hasFrame() const = 0;
    virtual uint64_t frameNumber() const = 0;

    // Replays the current frame into `efb`.  With `eye`, the perspective 3D
    // draws after the frame's camera use the VR camera (or, with
    // eye->flatStereo, the game's camera shifted for a stereo pair).
    virtual void render(Target& efb, const EyeView* eye, Target* hud, HudMode hudMode = HudMode::Inline) = 0;
    // Whether the last render() reached the scene depth marker and filled
    // eye->depthSnapshot.
    virtual bool snapshotTaken() const = 0;
    // Draws the last render() sent to the HUD target (0: it stayed clear).
    virtual int hudDrawCount() const = 0;

    // Camera recorded in the current frame (valid = false if none).
    virtual const CameraInfo& camera() const = 0;

    // Texture holding the last EFB->XFB copy of the most recent render().
    virtual TexHandle xfbTexture() const = 0;
    virtual int xfbWidth() const = 0;
    virtual int xfbHeight() const = 0;

    // Waits until the GPU has finished all work submitted so far (headless
    // timing and readback).
    virtual void finish() = 0;
    // Reads a target's color as RGBA8, width * height * 4 bytes, rows
    // bottom-up (GL order: the first row is the bottom of the picture).
    virtual bool readPixels(const Target& t, uint8_t* rgba) = 0;
};

// The renderer in use, created on first use: the backend chosen by
// selectRenderer(), else by PETARI_GFX, else GLES.
Renderer& renderer();

// Chooses the backend before the first renderer() call.  PETARI_GFX
// (vk | vulkan | gles) wins over `renderer = ...` in the ini file at
// `iniPath` (may be null).  Without a Vulkan backend in the build, or if it
// cannot be created, GLES is used and a log line says so.
void selectRenderer(const char* iniPath);

// The backend PETARI_GFX or the ini file asks for (renderer() may still
// fall back to GLES), without creating the renderer.
Api requestedApi();
// Makes renderer() use GLES whatever was asked, logging `why` (a string that
// lives on) when Vulkan was asked for: the XR presentation cannot run on
// Vulkan.  Before the first renderer() call.
void forceGles(const char* why);

// Backend factories (null if the backend is not available).
Renderer* createGlesRenderer();
Renderer* createVulkanRenderer();

// File for the backend's shader / pipeline cache (set before
// Renderer::init).  GLES: linked program binaries.
void setShaderCachePath(const char* path);
const char* shaderCachePath();

// Debug: stop replaying after this many draws (-1 = all).  Initialized from
// PETARI_GLSTOP.  Both backends honour it.
extern int gDebugStopAfterDraws;

}  // namespace gpu
