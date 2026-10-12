// The graphics side of the mock OpenXR runtime (xrmock.c): everything that
// depends on the graphics API the session is bound to.  The core never
// includes a platform or graphics header; it asks the table that claims the
// session's graphics binding.
//
// Implemented: xrmock_gles.c (XR_MNDX_egl_enable + XR_KHR_opengl_es_enable,
// swapchain images are textures in the app's current EGL context).
//
// For Vulkan (Lane D2) add xrmock_vk.c with a second MockGfx and list it in
// kGfxTables (xrmock.c):
//   - owns_binding: XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR (the binding is
//     XrGraphicsBindingVulkan2KHR; with XR_KHR_vulkan_enable2 the app gets
//     the instance, physical device and device from xrCreateVulkanInstanceKHR,
//     xrGetVulkanGraphicsDevice2KHR, xrCreateVulkanDeviceKHR).
//   - get_proc: xrGetVulkanGraphicsRequirements2KHR, xrCreateVulkanInstanceKHR,
//     xrGetVulkanGraphicsDevice2KHR, xrCreateVulkanDeviceKHR (they call into
//     the system Vulkan loader; the app's queue family and device come back
//     through the binding).
//   - formats: VkFormat numbers (R8G8B8A8_SRGB, R16G16B16A16_SFLOAT,
//     D24_UNORM_S8_UINT or D32_SFLOAT_S8_UINT) and the fmt_* mapping.
//   - create_images / fill_images: VkImages on the app's VkDevice, filled into
//     XrSwapchainImageVulkan2KHR; the layouts stay the app's business (the
//     runtime would own acquire/release ordering on the app's queue).
//   - the extension list (MOCK_XR_EXTENSIONS) already offers
//     XR_KHR_vulkan_enable2.
#pragma once

#include <openxr/openxr.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { MOCK_FMT_EYE, MOCK_FMT_UI, MOCK_FMT_MOTION, MOCK_FMT_DEPTH } MockFormatKind;

typedef struct MockGfx {
    const char* name;
    // The session's graphics binding (first struct of XrSessionCreateInfo::next
    // that this table claims).  Writes a description for the trace.
    int (*owns_binding)(XrStructureType type);
    int (*describe_binding)(const XrBaseInStructure* binding, char* out, size_t cap);
    // Entry points of the graphics extension (xrGetOpenGLESGraphicsRequirementsKHR ...).
    PFN_xrVoidFunction (*get_proc)(const char* name);
    // The swapchain formats of the API, and the one for each kind.
    int (*formats)(const int64_t** list);
    int64_t (*format_of)(MockFormatKind kind);
    // Images of a swapchain: `count` of them, `mips` levels.
    void* (*create_images)(int64_t format, int width, int height, int mips, int count);
    // Fills `count` XrSwapchainImage* structures (type-checked) with the images.
    XrResult (*fill_images)(void* images, int count, XrSwapchainImageBaseHeader* out);
} MockGfx;

extern const MockGfx kMockGfxGles;
