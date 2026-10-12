// Choice of the graphics API behind the OpenXR session (xr_gfx.h), and the
// swapchain calls every API shares.
#include "xr_gfx.h"

#include <stdlib.h>
#include <strings.h>

#include "port/port.h"

namespace xrgfx {

// Without the Vulkan implementation in the build there is none
// (xr_gfx_vk.cpp defines the real one).
__attribute__((weak)) Gfx* createVulkanGfx() { return nullptr; }

namespace {

Gfx* sGfx = nullptr;

}  // namespace

Api wantedApi() {
    const char* want = getenv("PETARI_GFX");
    if (want && (!strcasecmp(want, "vk") || !strcasecmp(want, "vulkan"))) {
        return Api::Vulkan;
    }
    return Api::Gles;
}

void select(Api wanted) {
    if (sGfx) {
        return;
    }
    if (wanted == Api::Vulkan) {
        sGfx = createVulkanGfx();
        if (!sGfx) {
            port_log("xr: no Vulkan graphics in this build; using GLES");
        }
    }
    if (!sGfx) {
        sGfx = createGlesGfx();
    }
}

Gfx& gfx() {
    if (!sGfx) {
        select(Api::Gles);
    }
    return *sGfx;
}

uint32_t acquireImage(XrSwapchain swapchain) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult r = xrAcquireSwapchainImage(swapchain, &ai, &idx);
    if (XR_FAILED(r)) {
        port_fatal("xrAcquireSwapchainImage failed: %d", (int)r);
    }
    return idx;
}

void waitImage(XrSwapchain swapchain) {
    XrSwapchainImageWaitInfo swi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    swi.timeout = XR_INFINITE_DURATION;
    XrResult r = xrWaitSwapchainImage(swapchain, &swi);
    if (XR_FAILED(r)) {
        port_fatal("xrWaitSwapchainImage failed: %d", (int)r);
    }
}

void releaseImage(XrSwapchain swapchain) {
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XrResult r = xrReleaseSwapchainImage(swapchain, &ri);
    if (XR_FAILED(r)) {
        port_fatal("xrReleaseSwapchainImage failed: %d", (int)r);
    }
}

}  // namespace xrgfx
