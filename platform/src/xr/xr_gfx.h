// The graphics half of the OpenXR session: what depends on the graphics API
// the session is bound to.  xr_app.cpp (the frame loop) and xr_sim.cpp (the
// headless stand-in) work through this interface and never touch the API
// themselves; drawing into the images is vr_draw.h.
//
// One implementation per API: xr_gfx_gl.cpp (GLES through EGL, or Android's
// OpenGL ES binding) and, later, xr_gfx_vk.cpp (Vulkan through
// XR_KHR_vulkan_enable2).  The images of a swapchain are handed out as
// gpu::EfbTargets, as the renderer's own targets are: under GLES `fbo` is a
// framebuffer with the image as its colour attachment and `color` the
// texture name (for a depth swapchain `depth` is the texture name); under
// Vulkan the slots of the renderer's image table.
//
// Threads: the XR main thread (the one running the frame loop) owns the
// session, the swapchain calls and, under Vulkan, the queue: acquire,
// release and submit are called only from it.
#pragma once

#include <openxr/openxr.h>
#include <stdint.h>

#include <vector>

#include "../gx/gl_renderer.h"

namespace xrgfx {

enum class Api {
    Gles,
    Vulkan,
};

// What a swapchain holds (each has one format per API).
enum class ImageKind {
    Eye,     // sRGB RGBA8, drawn into by the composite
    Ui,      // sRGB RGBA8 with mip levels: a panel's picture for a quad layer
    Motion,  // SpaceWarp motion vectors, RGBA16F
    Depth,   // SpaceWarp depth, D24S8
};

// A swapchain and its images.
struct Chain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int32_t width = 0, height = 0;
    uint32_t mips = 1;
    int64_t format = 0;                 // the API's own format number, for the log
    std::vector<gpu::EfbTarget> images;  // in the order the runtime lists them
};

// A mark in the GPU's work: signalled when everything submitted before it
// has finished.  0 = none.
using Fence = uintptr_t;

class Gfx {
public:
    virtual ~Gfx() = default;
    virtual Api api() const = 0;

    // --- Setup, in this order.
    // 1. The graphics context, before the OpenXR instance (GLES: the EGL
    //    display and context).
    virtual void createContext() = 0;
    // 2. The runtime's extensions this API needs, added to `enable`;
    //    `offered` is what the runtime lists.  Stops the app if it lacks
    //    them.  Both APIs' binding extensions the runtime offers are enabled,
    //    so the API in use is decided at session creation.
    virtual void instanceExtensions(const std::vector<XrExtensionProperties>& offered, std::vector<const char*>* enable) = 0;
    // 3. After xrGetSystem: the runtime's requirements on the API
    //    (Vulkan: the device is created here, through the runtime).
    virtual void attach(XrInstance instance, XrSystemId system) = 0;
    // 4. The graphics binding for XrSessionCreateInfo::next (valid until
    //    xrCreateSession returns).
    virtual const void* sessionBinding() = 0;
    // 5. After xrCreateSession: the formats the runtime offers.
    virtual void sessionCreated(XrSession session) = 0;
    // Whether swapchains of that kind can be made.
    virtual bool hasFormat(ImageKind kind) const = 0;

    // --- Swapchains.  createChain makes the swapchain and wraps its images.
    virtual bool createChain(XrSession session, ImageKind kind, int width, int height, uint32_t mips, Chain* out) = 0;
    // xrAcquireSwapchainImage: the index of the next image to draw into.
    virtual uint32_t acquire(Chain& chain) = 0;
    // xrWaitSwapchainImage: until the compositor is done with that image
    // (Vulkan: the image is then moved to the layout drawing wants).
    virtual void wait(Chain& chain) = 0;
    // xrReleaseSwapchainImage.  The image's drawing has been submitted (a
    // GLES runtime reads it as soon as it is released, finished or not).
    virtual void release(Chain& chain) = 0;

    // --- The GPU's work.
    // Ends the work recorded so far and returns a fence for it (GLES:
    // glFenceSync + glFlush; Vulkan: vkQueueSubmit with a fence).
    virtual Fence submit() = 0;
    // Waits up to `timeoutNs` (0: just looks) for the fence; true once
    // signalled.
    virtual bool waitFence(Fence fence, int64_t timeoutNs) = 0;
    virtual void destroyFence(Fence fence) = 0;
    // Submits and waits until the GPU has finished everything (glFinish).
    virtual void finish() = 0;

    // --- Without OpenXR (xr_sim.cpp): an image like a swapchain's, of a
    // kind's format; Ui images sample with linear filtering.
    virtual gpu::EfbTarget createImage(ImageKind kind, int width, int height, uint32_t mips) = 0;
};

// The API to use: PETARI_GFX (vk | vulkan | gles | gl), else GLES.  (Once the
// renderer backend is chosen from the ini file too, it decides.)
Api wantedApi();
// Creates the implementation for `wanted` (Vulkan without an implementation
// in the build falls back to GLES, logged).  Call once, before gfx().
void select(Api wanted);
// The selected implementation (GLES if select was never called).
Gfx& gfx();

// The implementations (null if the build has none).
Gfx* createGlesGfx();
Gfx* createVulkanGfx();

// The OpenXR calls behind Gfx::acquire, wait and release, for the
// implementations to build on.
uint32_t acquireImage(XrSwapchain swapchain);
void waitImage(XrSwapchain swapchain);
void releaseImage(XrSwapchain swapchain);

}  // namespace xrgfx
