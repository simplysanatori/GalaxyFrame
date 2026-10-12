// Vulkan device of the renderer (vk_renderer.cpp) and of the XR presentation:
// the loader (libvulkan.so.1, opened with dlopen), the instance, the device
// with its one queue, the function table, and command buffer submission.
//
// Include this header before anything that includes <vulkan/vulkan.h>
// (openxr_platform.h with XR_USE_GRAPHICS_API_VULKAN): the Vulkan functions
// are the pointers below (namespace gpu::vk), not prototypes, so the game
// library does not link libvulkan and a missing loader only means "no
// Vulkan" (the renderer falls back to GLES).
//
// Bring-up for the XR presentation (XR_KHR_vulkan_enable2):
//   gpu::vk::loadLoader();
//   InstanceSetup is;  fillInstanceSetup(&is);           // apiVersion 1.2, validation with PETARI_VKVALIDATE
//   xrCreateVulkanInstanceKHR(..., pfnGetInstanceProcAddr = gpu::vk::instanceProcAddr(),
//                              vulkanCreateInfo = &is.info) -> instance
//   adoptInstance(instance, false);
//   xrGetVulkanGraphicsDevice2KHR(...) -> phys
//   DeviceSetup ds;  fillDeviceSetup(phys, &ds);         // false: the GPU lacks something (logged)
//   (append the presentation's own device extensions to ds.extensions[] / ds.info if needed)
//   xrCreateVulkanDeviceKHR(..., vulkanCreateInfo = &ds.info) -> device
//   adoptDevice(phys, device, ds.queueFamily, 0, false, &ds.info);
//   ... then gpu::renderer().init() (PETARI_GFX=vk) uses that device.  The
//   renderer object itself may be made earlier (gpu::renderer().api() picks
//   the binding): making it only checks the GPU (probeDevice()).
// Without an adopted device at init() (headless), the renderer creates its
// own (createStandalone()).
//
// SteamVR's OpenXR API layer XR_APILAYER_VALVE_fdm_injection puts the Vulkan
// layer VK_LAYER_VALVE_fdm_injection into instances made through
// XR_KHR_vulkan_enable2.  It injects a fragment density map (foveation) and a
// depth copy into render passes whose targets have an XR swapchain's size.
// Tried headless (the layer forced on, FDM_DEBUG=enable, a fake swapchain
// size): vkCreateImage, vkCreateImageView, vkCreateGraphicsPipelines and
// vkCmdEndRenderingKHR resolve into the layer, vkCmdBeginRenderingKHR does
// not; the renderer's pictures stayed within the tolerance (HUD targets of the
// swapchain size changed slightly) and the validation layer, below it, lost
// track of our rendering instances' ends.  adoptDevice() logs which library
// each key entry point resolves into; DISABLE_VULKAN_FDM_INJECTION_LAYER=1 in
// the environment keeps the layer out (an A/B switch, nothing else changes).
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <stddef.h>
#include <stdint.h>

#include <functional>

namespace gpu {
namespace vk {

// ---------------------------------------------------------------------------
// Function table
// ---------------------------------------------------------------------------
#define GQ_VK_GLOBAL_FUNCS(X)                  \
    X(vkCreateInstance)                        \
    X(vkEnumerateInstanceExtensionProperties)  \
    X(vkEnumerateInstanceLayerProperties)      \
    X(vkEnumerateInstanceVersion)

#define GQ_VK_INSTANCE_FUNCS(X)                     \
    X(vkDestroyInstance)                            \
    X(vkEnumeratePhysicalDevices)                   \
    X(vkGetPhysicalDeviceProperties)                \
    X(vkGetPhysicalDeviceProperties2)               \
    X(vkGetPhysicalDeviceFeatures2)                 \
    X(vkGetPhysicalDeviceMemoryProperties)          \
    X(vkGetPhysicalDeviceMemoryProperties2)         \
    X(vkGetPhysicalDeviceQueueFamilyProperties)     \
    X(vkGetPhysicalDeviceFormatProperties)          \
    X(vkGetPhysicalDeviceImageFormatProperties2)    \
    X(vkEnumerateDeviceExtensionProperties)         \
    X(vkCreateDevice)                               \
    X(vkGetDeviceProcAddr)

// Instance functions that may be missing (debug utils without the extension).
#define GQ_VK_INSTANCE_OPT_FUNCS(X)      \
    X(vkCreateDebugUtilsMessengerEXT)    \
    X(vkDestroyDebugUtilsMessengerEXT)   \
    X(vkSetDebugUtilsObjectNameEXT)

#define GQ_VK_DEVICE_FUNCS(X)              \
    X(vkDestroyDevice)                     \
    X(vkGetDeviceQueue)                    \
    X(vkDeviceWaitIdle)                    \
    X(vkQueueSubmit)                       \
    X(vkQueueWaitIdle)                     \
    X(vkAllocateMemory)                    \
    X(vkFreeMemory)                        \
    X(vkMapMemory)                         \
    X(vkUnmapMemory)                       \
    X(vkCreateBuffer)                      \
    X(vkDestroyBuffer)                     \
    X(vkGetBufferMemoryRequirements)       \
    X(vkBindBufferMemory)                  \
    X(vkCreateImage)                       \
    X(vkDestroyImage)                      \
    X(vkGetImageMemoryRequirements)        \
    X(vkGetImageSubresourceLayout)         \
    X(vkBindImageMemory)                   \
    X(vkCreateImageView)                   \
    X(vkDestroyImageView)                  \
    X(vkCreateSampler)                     \
    X(vkDestroySampler)                    \
    X(vkCreateShaderModule)                \
    X(vkDestroyShaderModule)               \
    X(vkCreatePipelineCache)               \
    X(vkDestroyPipelineCache)              \
    X(vkGetPipelineCacheData)              \
    X(vkCreateGraphicsPipelines)           \
    X(vkDestroyPipeline)                   \
    X(vkCreatePipelineLayout)              \
    X(vkDestroyPipelineLayout)             \
    X(vkCreateDescriptorSetLayout)         \
    X(vkDestroyDescriptorSetLayout)        \
    X(vkCreateDescriptorPool)              \
    X(vkDestroyDescriptorPool)             \
    X(vkAllocateDescriptorSets)            \
    X(vkUpdateDescriptorSets)              \
    X(vkCreateCommandPool)                 \
    X(vkDestroyCommandPool)                \
    X(vkResetCommandPool)                  \
    X(vkAllocateCommandBuffers)            \
    X(vkBeginCommandBuffer)                \
    X(vkEndCommandBuffer)                  \
    X(vkCreateFence)                       \
    X(vkDestroyFence)                      \
    X(vkCreateSemaphore)                   \
    X(vkDestroySemaphore)                  \
    X(vkWaitSemaphores)                    \
    X(vkGetSemaphoreCounterValue)          \
    X(vkCreateQueryPool)                   \
    X(vkDestroyQueryPool)                  \
    X(vkGetQueryPoolResults)               \
    X(vkResetQueryPool)                    \
    X(vkCmdBeginQuery)                     \
    X(vkCmdEndQuery)                       \
    X(vkCmdBindPipeline)                   \
    X(vkCmdSetViewport)                    \
    X(vkCmdSetScissor)                     \
    X(vkCmdSetLineWidth)                   \
    X(vkCmdSetBlendConstants)              \
    X(vkCmdSetStencilCompareMask)          \
    X(vkCmdSetStencilWriteMask)            \
    X(vkCmdSetStencilReference)            \
    X(vkCmdBindDescriptorSets)             \
    X(vkCmdBindIndexBuffer)                \
    X(vkCmdBindVertexBuffers)              \
    X(vkCmdDraw)                           \
    X(vkCmdDrawIndexed)                    \
    X(vkCmdCopyImage)                      \
    X(vkCmdBlitImage)                      \
    X(vkCmdCopyBufferToImage)              \
    X(vkCmdCopyImageToBuffer)              \
    X(vkCmdClearColorImage)                \
    X(vkCmdClearAttachments)               \
    X(vkCmdPushConstants)                  \
    X(vkCmdWriteTimestamp)                 \
    X(vkCmdResetQueryPool)                 \
    X(vkCmdBeginRenderingKHR)              \
    X(vkCmdEndRenderingKHR)                \
    X(vkCmdPipelineBarrier2KHR)            \
    X(vkCmdPushDescriptorSetKHR)           \
    X(vkCmdSetCullModeEXT)                 \
    X(vkCmdSetFrontFaceEXT)                \
    X(vkCmdSetPrimitiveTopologyEXT)        \
    X(vkCmdSetDepthTestEnableEXT)          \
    X(vkCmdSetDepthWriteEnableEXT)         \
    X(vkCmdSetDepthCompareOpEXT)           \
    X(vkCmdSetStencilTestEnableEXT)        \
    X(vkCmdSetStencilOpEXT)                \
    X(vkCmdSetColorBlendEnableEXT)         \
    X(vkCmdSetColorBlendEquationEXT)       \
    X(vkCmdSetColorWriteMaskEXT)

// Device functions of optional extensions (null when not enabled).
#define GQ_VK_DEVICE_OPT_FUNCS(X)          \
    X(vkCopyMemoryToImageEXT)              \
    X(vkTransitionImageLayoutEXT)

#define GQ_VK_DECLARE(name) extern PFN_##name name;
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
GQ_VK_GLOBAL_FUNCS(GQ_VK_DECLARE)
GQ_VK_INSTANCE_FUNCS(GQ_VK_DECLARE)
GQ_VK_INSTANCE_OPT_FUNCS(GQ_VK_DECLARE)
GQ_VK_DEVICE_FUNCS(GQ_VK_DECLARE)
GQ_VK_DEVICE_OPT_FUNCS(GQ_VK_DECLARE)
#undef GQ_VK_DECLARE

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------
struct Device {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memProps{};
    bool hostImageCopy = false;        // VK_EXT_host_image_copy: textures are written from the CPU, no queue
    bool depthStencilBlit = false;     // D24S8 can be blitted (the SpaceWarp depth snapshot)
    bool topologyUnrestricted = false; // any topology with any pipeline (EDS3 property)
    bool lineBresenham = false;        // VK_EXT_line_rasterization Bresenham lines (as zink draws GL lines)
    bool memoryBudget = false;         // VK_EXT_memory_budget
    bool fragmentDensityMap = false;   // VK_EXT_fragment_density_map (foveation; PETARI_VKFDM=0 turns it off)
    VkExtent2D fdmTexelMin{}, fdmTexelMax{};
    bool debugUtils = false;           // VK_EXT_debug_utils on the instance
    bool validation = false;           // VK_LAYER_KHRONOS_validation on the instance
    bool ownsInstance = false, ownsDevice = false;
    float maxAnisotropy = 1.0f;
};
const Device& device();

// The loader, opened once.  False if libvulkan.so.1 is missing.
bool loadLoader();
// For XR_KHR_vulkan_enable2 (XrVulkan*CreateInfoKHR::pfnGetInstanceProcAddr).
PFN_vkGetInstanceProcAddr instanceProcAddr();

// Instance create info for an instance the renderer can use: apiVersion 1.2
// (SteamVR accepts 1.0 to 1.2), VK_EXT_debug_utils and the validation layer
// with PETARI_VKVALIDATE=1 (=2 adds synchronization validation).  `info`
// points into the struct: do not copy it after filling.
struct InstanceSetup {
    VkApplicationInfo app{};
    VkInstanceCreateInfo info{};
    VkValidationFeaturesEXT features{};
    VkValidationFeatureEnableEXT enables[2]{};
    const char* extensions[4]{};
    const char* layers[2]{};
};
bool fillInstanceSetup(InstanceSetup* s);
// Loads the instance functions (and the debug messenger with validation).
bool adoptInstance(VkInstance instance, bool owned);

// Device create info for `phys`: one queue (graphics, compute and transfer)
// and every extension and feature the renderer needs.  False (with a log
// line naming what is missing) if the GPU cannot run the renderer.  `info`
// points into the struct: do not copy it after filling.  More extensions may
// be appended to extensions[] (update info.enabledExtensionCount).
struct DeviceSetup {
    VkDeviceCreateInfo info{};
    VkDeviceQueueCreateInfo queue{};
    float priority = 1.0f;
    uint32_t queueFamily = 0;
    const char* extensions[32]{};
    VkPhysicalDeviceFeatures2 f2{};
    VkPhysicalDeviceVulkan11Features f11{};
    VkPhysicalDeviceVulkan12Features f12{};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynRendering{};
    VkPhysicalDeviceSynchronization2FeaturesKHR sync2{};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds1{};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2{};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{};
    VkPhysicalDeviceDepthClipControlFeaturesEXT clipControl{};
    VkPhysicalDeviceHostImageCopyFeaturesEXT hostCopy{};
    VkPhysicalDeviceLineRasterizationFeaturesEXT lines{};
    VkPhysicalDeviceFragmentDensityMapFeaturesEXT fdm{};
    // What fillDeviceSetup found (copied into device() by adoptDevice).
    bool hostImageCopy = false, lineBresenham = false, memoryBudget = false, topologyUnrestricted = false, fragmentDensityMap = false;
};
bool fillDeviceSetup(VkPhysicalDevice phys, DeviceSetup* s);
// Takes a device made from such a create info: loads the device functions,
// gets the queue, creates the submission objects.  `usedInfo`: the create
// info the device was made with, for the log (layers and extensions in play,
// and which library each key entry point resolves into).
bool adoptDevice(VkPhysicalDevice phys, VkDevice device, uint32_t queueFamily, uint32_t queueIndex, bool owned,
                 const VkDeviceCreateInfo* usedInfo = nullptr);
// No XR: instance and device of our own (headless).  Logs why on failure.
bool createStandalone();
bool deviceReady();
// Whether the GPU can run the renderer (with a short-lived instance of its
// own, before any device exists): createVulkanRenderer() asks, so the
// choice can fall back to GLES before the XR session is made.
bool probeDevice();

// ---------------------------------------------------------------------------
// Command buffers and submission.  Only the thread that renders (the XR main
// thread; headless: the snapshot thread) records and submits: the SteamVR
// client library submits on the same queue inside the process.
//
// A refresh of the XR frame loop on Vulkan:
//   r.update();                                  // the newest prepared frame
//   r.render(eye0, ...); r.render(eye1, ...);    // recorded into commandBuffer()
//   record the composite and UI draws into commandBuffer(), sampling the
//   targets' color (SHADER_READ_ONLY_OPTIMAL after render(), see below)
//   uint64_t v = submit(&extra);                 // before xrReleaseSwapchainImage
//   ... waitValue(v of an earlier refresh) or extra.fence to pace the loop.
// The renderer submits on its own only after 24 renders without a submit
// (a safety net) and in finish() / readPixels() (headless).
// ---------------------------------------------------------------------------
// Eight: the XR frame loop submits up to three times a refresh (left eye,
// right eye and the rest, UI layers) with two refreshes' sets in flight;
// with three, opening a command buffer waited for the last set's GPU work in
// the middle of recording the next (H3 at resolution 1.0: "both eyes"
// taking 8-12 ms of CPU, refreshes missed).
const int kFramesInFlight = 8;

// The open command buffer, begun on first use after a submit (which may wait
// for the GPU to finish the submission made kFramesInFlight submits ago).
// Such a wait is counted (commandBufferWaits) for the frame loop's pacing.
VkCommandBuffer commandBuffer();
bool commandBufferOpen();
// Time commandBuffer() spent waiting for the GPU since the last call (ns).
int64_t takeCommandBufferWaitNs();
// Semaphores and fence for a submission from the XR presentation.
struct SubmitExtra {
    uint32_t waitCount = 0;
    const VkSemaphore* waits = nullptr;
    const VkPipelineStageFlags* waitStages = nullptr;
    uint32_t signalCount = 0;
    const VkSemaphore* signals = nullptr;
    VkFence fence = VK_NULL_HANDLE;
};
// Ends and submits the open command buffer (an empty one if none is open);
// returns the timeline value the submission signals.
uint64_t submit(const SubmitExtra* extra = nullptr);
// The timeline value the open (or next) command buffer will signal: work
// recorded so far is finished once completedValue() reaches it.
uint64_t pendingValue();
uint64_t completedValue();
bool waitValue(uint64_t value, uint64_t timeoutNs = UINT64_MAX);
// Submits what is open and waits for the GPU (headless finish, readback).
void finishAll();
// Runs `fn` (on the rendering thread) once the GPU has finished everything
// recorded so far.  For destroying objects the GPU may still read.
void deferUntilDone(std::function<void()> fn);
// Runs deferred work whose submissions finished (also done by
// commandBuffer() and submit()).
void collectFinished();
// The timeline semaphore (for a presentation that waits on our work).
VkSemaphore timelineSemaphore();

// Per-submission scratch memory (host visible, coherent): valid until the
// open command buffer has finished on the GPU.
struct ScratchAlloc {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    void* ptr = nullptr;
};
ScratchAlloc scratch(VkDeviceSize size, VkDeviceSize align);

// GPU timestamps: one pair per call of beginTimer/endTimer in the open
// command buffer; the results arrive (in ms) through `sink` when the
// submission finishes.
void setTimerSink(void (*sink)(int tag, double ms));
void beginTimer(int tag);
void endTimer(int tag);

// GPU profile (PETARI_VKPROF=1): a timestamp at each mark; the GPU time from
// a mark to the next one in the same command buffer is added to the first
// one's label (a string that lives on).  Timestamps wait for the GPU to go
// idle (Turnip), so a profiled render is a little slower than a plain one,
// and only direct rendering (TU_DEBUG=sysmem) gives times inside a rendering
// instance.  profReport logs the totals since the last report divided by
// `per` (renders) and starts over.
bool profEnabled();
int profMark(const char* label);  // the mark's index in the open command buffer, -1 if none
void profRelabel(int mark, const char* label);
void profReport(const char* why, int per);
// GPU counters (headless, PETARI_VKPERFC=1 with TU_DEBUG=perfc: Turnip's
// VK_KHR_performance_query): perfBegin / perfEnd around commands in one
// command buffer, perfReport (after the GPU finished them) logs every
// counter.  No-ops without them.
void perfBegin();
void perfEnd();
void perfReport(const char* why);

// Names a Vulkan object for the validation layer's messages (no-op without
// debug utils).
void setName(VkObjectType type, uint64_t handle, const char* name);

// ---------------------------------------------------------------------------
// The renderer's images (vk_renderer.cpp), for the XR presentation.
// gpu::Target::color / depth and Renderer::xfbTexture() are handles of this
// table.  The renderer records which layout every image is in as it records
// commands; work recorded by others on these images goes through
// transitionImage() (or assumeImageLayout() after an outside transition, such
// as an XR runtime handing back a swapchain image).
//
// After render(): the target's color (and the HUD target's) is in
// SHADER_READ_ONLY_OPTIMAL, written by color attachment output and visible to
// fragment shader reads recorded after it; its depth is undefined unless
// setKeepDepth(true); EyeView::depthSnapshot targets are SHADER_READ_ONLY_OPTIMAL.
// ---------------------------------------------------------------------------
struct ImageInfo {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;     // sampled (depth formats: the depth aspect)
    VkImageView attView = VK_NULL_HANDLE;  // attachment (depth formats: depth and stencil)
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;  // as recorded so far
};
bool imageInfo(uint32_t handle, ImageInfo* out);
// Records a barrier into the open command buffer: from the image's last
// recorded use to `layout`, for `stages` / `access` (sync2 flags).
void transitionImage(uint32_t handle, VkImageLayout layout, VkPipelineStageFlags2 stages, VkAccessFlags2 access);
// The image is now in `layout` (changed outside the renderer's commands).
void assumeImageLayout(uint32_t handle, VkImageLayout layout);
// An image made elsewhere (an XR swapchain image) as a handle: a Target's
// color must be kColorFormat (R8G8B8A8_UNORM), its depth D24_UNORM_S8_UINT.
// The renderer never destroys it; unregister before destroying it.
uint32_t registerImage(VkImage image, VkImageView view, VkImageView attView, VkFormat format, uint32_t width, uint32_t height, VkImageLayout layout);
void unregisterImage(uint32_t handle);
// Keeps each render's depth (and stencil) in its target, for a depth layer
// or frame synthesis (default: not stored, which saves the tiler the
// write-back).
void setKeepDepth(bool keep);
// GPU time of render() calls (timestamps around each), for the
// presentation's dynamic resolution: the serial number of the last render()
// and, once its submission has finished (the renderer reads the results when
// a command buffer slot comes round again, or at finish), its time in ms.
int lastRenderSerial();
bool renderGpuMs(int serial, double* ms);

}  // namespace vk
}  // namespace gpu
