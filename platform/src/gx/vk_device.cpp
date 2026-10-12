// Vulkan device, function table and submission (vk_device.h).
#include "vk_device.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "gpu.h"
#include "port/heap_routing.h"
#include "port/port.h"
#include "vk_mem.h"

namespace gpu {
namespace vk {

#define GQ_VK_DEFINE(name) PFN_##name name = nullptr;
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
GQ_VK_GLOBAL_FUNCS(GQ_VK_DEFINE)
GQ_VK_INSTANCE_FUNCS(GQ_VK_DEFINE)
GQ_VK_INSTANCE_OPT_FUNCS(GQ_VK_DEFINE)
GQ_VK_DEVICE_FUNCS(GQ_VK_DEFINE)
GQ_VK_DEVICE_OPT_FUNCS(GQ_VK_DEFINE)
#undef GQ_VK_DEFINE

namespace {

Device sDev;
void* sLoader = nullptr;
bool sLoaderTried = false;
bool sDeviceReady = false;
VkDebugUtilsMessengerEXT sMessenger = VK_NULL_HANDLE;
std::atomic<int> sValidationErrors{0}, sValidationWarnings{0};

int validateLevel() {
    const char* v = debugEnv("PETARI_VKVALIDATE");
    return v ? atoi(v) : 0;
}

bool hasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const VkExtensionProperties& e : list) {
        if (!strcmp(e.extensionName, name)) return true;
    }
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    bool error = severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    int n = error ? ++sValidationErrors : ++sValidationWarnings;
    // Every distinct message once in a while is enough: the same error per
    // draw would drown the log.
    if (n <= 200 || (n % 1000) == 0) {
        const char* text = data && data->pMessage ? data->pMessage : "";
        port_log("vk %s #%d: %s: %.1500s", error ? "VALIDATION ERROR" : "validation warning", n,
                 data && data->pMessageIdName ? data->pMessageIdName : "-", text);
    }
    return VK_FALSE;
}

// ---------------------------------------------------------------------------
// Submission state (rendering thread only, but for the timeline queries)
// ---------------------------------------------------------------------------
const uint32_t kMaxTimers = 32;  // begin/end pairs per command buffer

struct ScratchBlock {
    Buffer buf;
    VkDeviceSize used = 0;
};

struct Slot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint64_t value = 0;  // timeline value of its last submission
    std::vector<ScratchBlock> scratch;
    VkQueryPool queries = VK_NULL_HANDLE;
    uint32_t timers = 0;      // pairs written
    int tags[kMaxTimers];
    bool timerOpen = false;
    bool queriesReset = false;
};

Slot sSlots[kFramesInFlight];
int sNextSlot = 0;
int sOpen = -1;             // slot whose command buffer is being recorded
uint64_t sNextValue = 1;    // value the next submission signals
VkSemaphore sTimeline = VK_NULL_HANDLE;
std::deque<std::pair<uint64_t, std::function<void()>>> sDeferred;
void (*sTimerSink)(int, double) = nullptr;
double sTickMs = 0.0;
bool sTimersOk = false;

bool createSubmission() {
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = 0;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &type;
    if (vkCreateSemaphore(sDev.device, &sci, nullptr, &sTimeline) != VK_SUCCESS) {
        port_log("vk: cannot create the timeline semaphore");
        return false;
    }
    setName(VK_OBJECT_TYPE_SEMAPHORE, (uint64_t)sTimeline, "gq timeline");
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(sDev.phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(sDev.phys, &qn, qf.data());
    sTimersOk = sDev.queueFamily < qn && qf[sDev.queueFamily].timestampValidBits > 0 && sDev.props.limits.timestampPeriod > 0.0f;
    sTickMs = sDev.props.limits.timestampPeriod / 1e6;
    for (int i = 0; i < kFramesInFlight; i++) {
        Slot& s = sSlots[i];
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = sDev.queueFamily;
        if (vkCreateCommandPool(sDev.device, &pci, nullptr, &s.pool) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = s.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(sDev.device, &ai, &s.cmd) != VK_SUCCESS) return false;
        if (sTimersOk) {
            VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qci.queryCount = kMaxTimers * 2;
            if (vkCreateQueryPool(sDev.device, &qci, nullptr, &s.queries) != VK_SUCCESS) {
                sTimersOk = false;
            }
        }
    }
    return true;
}

// Results of the timers of a slot whose submission finished.
void readTimers(Slot& s) {
    if (!sTimersOk || !s.queries || s.timerOpen) return;
    if (s.timers && s.queriesReset) {
        uint64_t ticks[kMaxTimers * 2];
        uint32_t n = s.timers * 2;
        if (vkGetQueryPoolResults(sDev.device, s.queries, 0, n, sizeof(ticks), ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            for (uint32_t i = 0; i < s.timers; i++) {
                double ms = (double)(ticks[i * 2 + 1] - ticks[i * 2]) * sTickMs;
                if (sTimerSink && ticks[i * 2 + 1] >= ticks[i * 2]) sTimerSink(s.tags[i], ms);
            }
        }
    }
    vkResetQueryPool(sDev.device, s.queries, 0, kMaxTimers * 2);
    s.queriesReset = true;
    s.timers = 0;
    s.timerOpen = false;
}

}  // namespace

const Device& device() { return sDev; }

PFN_vkGetInstanceProcAddr instanceProcAddr() { return vkGetInstanceProcAddr; }

bool loadLoader() {
    if (sLoaderTried) {
        return sLoader != nullptr;
    }
    sLoaderTried = true;
    sLoader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!sLoader) {
        sLoader = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!sLoader) {
        port_log("vk: no Vulkan loader (libvulkan.so.1): %s", dlerror());
        return false;
    }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(sLoader, "vkGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr) {
        port_log("vk: the Vulkan loader has no vkGetInstanceProcAddr");
        dlclose(sLoader);
        sLoader = nullptr;
        return false;
    }
#define GQ_VK_LOAD_GLOBAL(name) name = (PFN_##name)vkGetInstanceProcAddr(VK_NULL_HANDLE, #name);
    GQ_VK_GLOBAL_FUNCS(GQ_VK_LOAD_GLOBAL)
#undef GQ_VK_LOAD_GLOBAL
    if (!vkCreateInstance || !vkEnumerateInstanceExtensionProperties || !vkEnumerateInstanceLayerProperties) {
        port_log("vk: the Vulkan loader lacks its global functions");
        return false;
    }
    return true;
}

bool fillInstanceSetup(InstanceSetup* s) {
    PortHostAllocScope hostAlloc;
    if (!loadLoader()) return false;
    *s = InstanceSetup();
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&loaderVersion);
    if (loaderVersion < VK_API_VERSION_1_2) {
        port_log("vk: the Vulkan loader is version %u.%u, 1.2 is needed", VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion));
        return false;
    }
    s->app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    s->app.pApplicationName = "GalaxyQuest";
    s->app.applicationVersion = 1;
    s->app.pEngineName = "petari";
    s->app.engineVersion = 1;
    // SteamVR accepts instances of Vulkan 1.0 to 1.2: newer features are
    // device extensions.
    s->app.apiVersion = VK_API_VERSION_1_2;
    s->info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    s->info.pApplicationInfo = &s->app;

    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data());
    uint32_t extCount = 0, layerCount = 0;
    int level = validateLevel();
    bool debugUtils = false;
    if (level > 0) {
        uint32_t ln = 0;
        vkEnumerateInstanceLayerProperties(&ln, nullptr);
        std::vector<VkLayerProperties> layers(ln);
        vkEnumerateInstanceLayerProperties(&ln, layers.data());
        bool found = false;
        for (const VkLayerProperties& l : layers) {
            found |= !strcmp(l.layerName, "VK_LAYER_KHRONOS_validation");
        }
        if (found) {
            s->layers[layerCount++] = "VK_LAYER_KHRONOS_validation";
            // The layer's own extensions (validation features).
            uint32_t le = 0;
            vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &le, nullptr);
            std::vector<VkExtensionProperties> layerExts(le);
            vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &le, layerExts.data());
            if (level >= 2 && hasExtension(layerExts, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) {
                s->extensions[extCount++] = VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME;
                s->enables[0] = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
                s->features.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
                s->features.enabledValidationFeatureCount = 1;
                s->features.pEnabledValidationFeatures = s->enables;
                s->info.pNext = &s->features;
            }
            if (hasExtension(layerExts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
                debugUtils = true;
            }
        } else {
            port_log("vk: PETARI_VKVALIDATE is set but VK_LAYER_KHRONOS_validation is not installed");
        }
        if (debugUtils || hasExtension(exts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            s->extensions[extCount++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        }
    }
    s->info.enabledExtensionCount = extCount;
    s->info.ppEnabledExtensionNames = extCount ? s->extensions : nullptr;
    s->info.enabledLayerCount = layerCount;
    s->info.ppEnabledLayerNames = layerCount ? s->layers : nullptr;
    sDev.validation = layerCount > 0;
    sDev.debugUtils = false;
    for (uint32_t i = 0; i < extCount; i++) {
        sDev.debugUtils |= !strcmp(s->extensions[i], VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    return true;
}

bool adoptInstance(VkInstance instance, bool owned) {
    if (!loadLoader() || !instance) return false;
    sDev.instance = instance;
    sDev.ownsInstance = owned;
    bool ok = true;
#define GQ_VK_LOAD_INSTANCE(name)                                                 \
    name = (PFN_##name)vkGetInstanceProcAddr(instance, #name);                    \
    if (!name) {                                                                  \
        port_log("vk: instance function %s missing", #name);                     \
        ok = false;                                                               \
    }
    GQ_VK_INSTANCE_FUNCS(GQ_VK_LOAD_INSTANCE)
#undef GQ_VK_LOAD_INSTANCE
#define GQ_VK_LOAD_INSTANCE_OPT(name) name = (PFN_##name)vkGetInstanceProcAddr(instance, #name);
    GQ_VK_INSTANCE_OPT_FUNCS(GQ_VK_LOAD_INSTANCE_OPT)
#undef GQ_VK_LOAD_INSTANCE_OPT
    if (!ok) return false;
    if (sDev.debugUtils && vkCreateDebugUtilsMessengerEXT && !sMessenger) {
        VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mci.pfnUserCallback = debugCallback;
        if (vkCreateDebugUtilsMessengerEXT(instance, &mci, nullptr, &sMessenger) == VK_SUCCESS) {
            port_log("vk: validation layer %s, messages go to this log", sDev.validation ? "on" : "off (debug utils only)");
        }
    }
    return true;
}

bool fillDeviceSetup(VkPhysicalDevice phys, DeviceSetup* s) {
    PortHostAllocScope hostAlloc;
    *s = DeviceSetup();
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    if (props.apiVersion < VK_API_VERSION_1_2) {
        port_log("vk: %s supports Vulkan %u.%u, 1.2 is needed", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
                 VK_API_VERSION_MINOR(props.apiVersion));
        return false;
    }
    // One queue that does everything.
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qf.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < qn && family == UINT32_MAX; i++) {
        if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) family = i;
    }
    if (family == UINT32_MAX) {
        port_log("vk: %s has no graphics queue", props.deviceName);
        return false;
    }

    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, exts.data());
    static const char* const kRequired[] = {
        VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,          VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,            VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME,   VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
        VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME,
    };
    uint32_t count = 0;
    std::string missing;
    for (const char* e : kRequired) {
        if (hasExtension(exts, e)) {
            s->extensions[count++] = e;
        } else {
            missing += missing.empty() ? e : std::string(", ") + e;
        }
    }
    bool hostCopyExt = hasExtension(exts, VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME) && hasExtension(exts, VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME) &&
                       hasExtension(exts, VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME);
    if (debugEnv("PETARI_VKNOHIC")) hostCopyExt = false;  // test the staging upload path
    bool lineExt = hasExtension(exts, VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME);
    bool budgetExt = hasExtension(exts, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

    // Features.
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    VkPhysicalDeviceSynchronization2FeaturesKHR s2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT e1{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT e3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT dc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    VkPhysicalDeviceHostImageCopyFeaturesEXT hc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT};
    VkPhysicalDeviceLineRasterizationFeaturesEXT lr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT};
    f2.pNext = &f12;
    f12.pNext = &dr;
    dr.pNext = &s2;
    s2.pNext = &e1;
    e1.pNext = &e2;
    e2.pNext = &e3;
    e3.pNext = &dc;
    void** tail = &dc.pNext;
    if (hostCopyExt) {
        *tail = &hc;
        tail = &hc.pNext;
    }
    if (lineExt) {
        *tail = &lr;
        tail = &lr.pNext;
    }
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    struct {
        bool ok;
        const char* name;
    } need[] = {
        {f12.timelineSemaphore == VK_TRUE, "timelineSemaphore"},
        {f12.hostQueryReset == VK_TRUE, "hostQueryReset"},
        {dr.dynamicRendering == VK_TRUE, "dynamicRendering"},
        {s2.synchronization2 == VK_TRUE, "synchronization2"},
        {e1.extendedDynamicState == VK_TRUE, "extendedDynamicState"},
        {e2.extendedDynamicState2 == VK_TRUE, "extendedDynamicState2"},
        {e3.extendedDynamicState3ColorBlendEnable == VK_TRUE, "extendedDynamicState3ColorBlendEnable"},
        {e3.extendedDynamicState3ColorBlendEquation == VK_TRUE, "extendedDynamicState3ColorBlendEquation"},
        {e3.extendedDynamicState3ColorWriteMask == VK_TRUE, "extendedDynamicState3ColorWriteMask"},
        {dc.depthClipControl == VK_TRUE, "depthClipControl"},
    };
    if (!missing.empty()) {
        // Features of missing extensions read as false; name the extensions only.
        port_log("vk: %s lacks %s; the Vulkan renderer cannot run", props.deviceName, missing.c_str());
        return false;
    }
    for (auto& f : need) {
        if (!f.ok) missing += missing.empty() ? f.name : std::string(", ") + f.name;
    }
    if (!missing.empty()) {
        port_log("vk: %s lacks the features %s; the Vulkan renderer cannot run", props.deviceName, missing.c_str());
        return false;
    }
    bool hostCopy = hostCopyExt && hc.hostImageCopy == VK_TRUE;
    bool bresenham = lineExt && lr.bresenhamLines == VK_TRUE;

    VkPhysicalDeviceExtendedDynamicState3PropertiesEXT e3p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &e3p;
    vkGetPhysicalDeviceProperties2(phys, &p2);

    if (hostCopy) {
        s->extensions[count++] = VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME;
        s->extensions[count++] = VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME;
        s->extensions[count++] = VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME;
    }
    if (bresenham) s->extensions[count++] = VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME;
    if (budgetExt) s->extensions[count++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;

    // What is enabled: only the features the renderer uses.
    s->f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    s->f2.features.samplerAnisotropy = f2.features.samplerAnisotropy;
    s->f2.features.fullDrawIndexUint32 = f2.features.fullDrawIndexUint32;
    s->f2.features.wideLines = VK_FALSE;
    s->f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    s->f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    s->f12.timelineSemaphore = VK_TRUE;
    s->f12.hostQueryReset = VK_TRUE;
    s->dynRendering.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
    s->dynRendering.dynamicRendering = VK_TRUE;
    s->sync2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR;
    s->sync2.synchronization2 = VK_TRUE;
    s->eds1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;
    s->eds1.extendedDynamicState = VK_TRUE;
    s->eds2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT;
    s->eds2.extendedDynamicState2 = VK_TRUE;
    s->eds3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
    s->eds3.extendedDynamicState3ColorBlendEnable = VK_TRUE;
    s->eds3.extendedDynamicState3ColorBlendEquation = VK_TRUE;
    s->eds3.extendedDynamicState3ColorWriteMask = VK_TRUE;
    s->clipControl.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT;
    s->clipControl.depthClipControl = VK_TRUE;
    s->f2.pNext = &s->f11;
    s->f11.pNext = &s->f12;
    s->f12.pNext = &s->dynRendering;
    s->dynRendering.pNext = &s->sync2;
    s->sync2.pNext = &s->eds1;
    s->eds1.pNext = &s->eds2;
    s->eds2.pNext = &s->eds3;
    s->eds3.pNext = &s->clipControl;
    void** next = &s->clipControl.pNext;
    if (hostCopy) {
        s->hostCopy.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT;
        s->hostCopy.hostImageCopy = VK_TRUE;
        *next = &s->hostCopy;
        next = &s->hostCopy.pNext;
    }
    if (bresenham) {
        s->lines.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT;
        s->lines.bresenhamLines = VK_TRUE;
        *next = &s->lines;
        next = &s->lines.pNext;
    }

    s->queueFamily = family;
    s->queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    s->queue.queueFamilyIndex = family;
    s->queue.queueCount = 1;
    s->queue.pQueuePriorities = &s->priority;
    s->info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    s->info.pNext = &s->f2;
    s->info.queueCreateInfoCount = 1;
    s->info.pQueueCreateInfos = &s->queue;
    s->info.enabledExtensionCount = count;
    s->info.ppEnabledExtensionNames = s->extensions;
    s->hostImageCopy = hostCopy;
    s->lineBresenham = bresenham;
    s->memoryBudget = budgetExt;
    s->topologyUnrestricted = e3p.dynamicPrimitiveTopologyUnrestricted == VK_TRUE;
    return true;
}

namespace {
// What fillDeviceSetup found for the device being adopted (createStandalone
// passes it on; a device made elsewhere is checked again).
DeviceSetup* sAdoptSetup = nullptr;
}  // namespace

bool adoptDevice(VkPhysicalDevice phys, VkDevice device, uint32_t queueFamily, uint32_t queueIndex, bool owned) {
    PortHostAllocScope hostAlloc;
    if (!sDev.instance || !device) return false;
    DeviceSetup check;
    DeviceSetup* setup = sAdoptSetup;
    if (!setup) {
        if (!fillDeviceSetup(phys, &check)) return false;
        setup = &check;
    }
    sDev.phys = phys;
    sDev.device = device;
    sDev.queueFamily = queueFamily;
    sDev.ownsDevice = owned;
    bool ok = true;
#define GQ_VK_LOAD_DEVICE(name)                                         \
    name = (PFN_##name)vkGetDeviceProcAddr(device, #name);              \
    if (!name) {                                                        \
        port_log("vk: device function %s missing", #name);             \
        ok = false;                                                     \
    }
    GQ_VK_DEVICE_FUNCS(GQ_VK_LOAD_DEVICE)
#undef GQ_VK_LOAD_DEVICE
#define GQ_VK_LOAD_DEVICE_OPT(name) name = (PFN_##name)vkGetDeviceProcAddr(device, #name);
    GQ_VK_DEVICE_OPT_FUNCS(GQ_VK_LOAD_DEVICE_OPT)
#undef GQ_VK_LOAD_DEVICE_OPT
    if (!ok) return false;
    vkGetDeviceQueue(device, queueFamily, queueIndex, &sDev.queue);
    vkGetPhysicalDeviceProperties(phys, &sDev.props);
    vkGetPhysicalDeviceMemoryProperties(phys, &sDev.memProps);
    sDev.hostImageCopy = setup->hostImageCopy && vkCopyMemoryToImageEXT && vkTransitionImageLayoutEXT;
    sDev.lineBresenham = setup->lineBresenham;
    sDev.memoryBudget = setup->memoryBudget;
    sDev.topologyUnrestricted = setup->topologyUnrestricted;
    sDev.maxAnisotropy = sDev.props.limits.maxSamplerAnisotropy;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    sDev.depthStencilBlit = (fp.optimalTilingFeatures & blit) == blit;
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        port_log("vk: D24S8 depth-stencil attachments unsupported");
        return false;
    }
    if (!memInit()) return false;
    if (!createSubmission()) return false;
    sDeviceReady = true;
    port_log("vk: %s, driver %u.%u.%u, Vulkan %u.%u (instance 1.2), queue family %u; host image copy %s, D24S8 blit %s, "
             "unrestricted topology %s, Bresenham lines %s",
             sDev.props.deviceName, VK_API_VERSION_MAJOR(sDev.props.driverVersion), VK_API_VERSION_MINOR(sDev.props.driverVersion),
             VK_API_VERSION_PATCH(sDev.props.driverVersion), VK_API_VERSION_MAJOR(sDev.props.apiVersion), VK_API_VERSION_MINOR(sDev.props.apiVersion),
             queueFamily, sDev.hostImageCopy ? "yes" : "no", sDev.depthStencilBlit ? "yes" : "no", sDev.topologyUnrestricted ? "yes" : "no",
             sDev.lineBresenham ? "yes" : "no");
    return true;
}

bool createStandalone() {
    PortHostAllocScope hostAlloc;
    if (sDeviceReady) return true;
    InstanceSetup is;
    if (!fillInstanceSetup(&is)) return false;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&is.info, nullptr, &instance);
    if (r != VK_SUCCESS) {
        port_log("vk: vkCreateInstance failed (%d)", (int)r);
        return false;
    }
    if (!adoptInstance(instance, true)) return false;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(instance, &n, devs.data());
    for (VkPhysicalDevice phys : devs) {
        DeviceSetup ds;
        if (!fillDeviceSetup(phys, &ds)) continue;
        VkDevice device = VK_NULL_HANDLE;
        r = vkCreateDevice(phys, &ds.info, nullptr, &device);
        if (r != VK_SUCCESS) {
            port_log("vk: vkCreateDevice failed (%d)", (int)r);
            continue;
        }
        sAdoptSetup = &ds;
        bool ok = adoptDevice(phys, device, ds.queueFamily, 0, true);
        sAdoptSetup = nullptr;
        if (ok) return true;
    }
    port_log("vk: no usable Vulkan device (%u found)", n);
    return false;
}

bool deviceReady() { return sDeviceReady; }

// ---------------------------------------------------------------------------
// Submission
// ---------------------------------------------------------------------------
bool commandBufferOpen() { return sOpen >= 0; }

VkCommandBuffer commandBuffer() {
    if (sOpen >= 0) {
        return sSlots[sOpen].cmd;
    }
    Slot& s = sSlots[sNextSlot];
    if (s.value) {
        waitValue(s.value);
    }
    collectFinished();
    readTimers(s);
    vkResetCommandPool(sDev.device, s.pool, 0);
    for (ScratchBlock& b : s.scratch) b.used = 0;
    // Keep the scratch memory one submission needed, in one block.
    if (s.scratch.size() > 1) {
        VkDeviceSize total = 0;
        for (ScratchBlock& b : s.scratch) total += b.buf.size;
        for (ScratchBlock& b : s.scratch) destroyBuffer(b.buf);
        s.scratch.clear();
        ScratchBlock b;
        if (createBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                         MemUse::Upload, &b.buf, MemCat::Scratch, "scratch")) {
            s.scratch.push_back(b);
        }
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.cmd, &bi);
    sOpen = sNextSlot;
    sNextSlot = (sNextSlot + 1) % kFramesInFlight;
    return s.cmd;
}

uint64_t submit(const SubmitExtra* extra) {
    if (sOpen < 0) {
        commandBuffer();
    }
    Slot& s = sSlots[sOpen];
    if (s.timerOpen) {
        endTimer(s.tags[s.timers]);
    }
    vkEndCommandBuffer(s.cmd);
    uint64_t value = sNextValue;
    uint32_t waitCount = extra ? extra->waitCount : 0;
    uint32_t signalCount = (extra ? extra->signalCount : 0) + 1;
    std::vector<VkSemaphore> signals(signalCount);
    std::vector<uint64_t> signalValues(signalCount, 0);
    std::vector<uint64_t> waitValues(waitCount, 0);
    for (uint32_t i = 0; i + 1 < signalCount; i++) signals[i] = extra->signals[i];
    signals[signalCount - 1] = sTimeline;
    signalValues[signalCount - 1] = value;
    VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ti.waitSemaphoreValueCount = waitCount;
    ti.pWaitSemaphoreValues = waitCount ? waitValues.data() : nullptr;
    ti.signalSemaphoreValueCount = signalCount;
    ti.pSignalSemaphoreValues = signalValues.data();
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ti;
    si.waitSemaphoreCount = waitCount;
    si.pWaitSemaphores = waitCount ? extra->waits : nullptr;
    si.pWaitDstStageMask = waitCount ? extra->waitStages : nullptr;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.cmd;
    si.signalSemaphoreCount = signalCount;
    si.pSignalSemaphores = signals.data();
    VkResult r = vkQueueSubmit(sDev.queue, 1, &si, extra ? extra->fence : VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        port_log("vk: vkQueueSubmit failed (%d)", (int)r);
    }
    s.value = value;
    sNextValue++;
    sOpen = -1;
    collectFinished();
    return value;
}

uint64_t pendingValue() { return sOpen >= 0 ? sNextValue : sNextValue - 1; }

uint64_t completedValue() {
    uint64_t v = 0;
    if (sTimeline) vkGetSemaphoreCounterValue(sDev.device, sTimeline, &v);
    return v;
}

bool waitValue(uint64_t value, uint64_t timeoutNs) {
    if (!value || !sTimeline) return true;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &sTimeline;
    wi.pValues = &value;
    VkResult r = vkWaitSemaphores(sDev.device, &wi, timeoutNs);
    if (r != VK_SUCCESS && r != VK_TIMEOUT) {
        port_log("vk: vkWaitSemaphores failed (%d)", (int)r);
    }
    return r == VK_SUCCESS;
}

void finishAll() {
    if (sOpen >= 0) submit();
    waitValue(sNextValue - 1);
    collectFinished();
    for (int i = 0; i < kFramesInFlight; i++) {
        if (i != sOpen && sSlots[i].timers) readTimers(sSlots[i]);
    }
}

void deferUntilDone(std::function<void()> fn) {
    uint64_t v = pendingValue();
    if (v == 0 || completedValue() >= v) {
        fn();
        return;
    }
    sDeferred.emplace_back(v, std::move(fn));
}

void collectFinished() {
    if (sDeferred.empty()) return;
    uint64_t done = completedValue();
    while (!sDeferred.empty() && sDeferred.front().first <= done) {
        std::function<void()> fn = std::move(sDeferred.front().second);
        sDeferred.pop_front();
        fn();
    }
}

VkSemaphore timelineSemaphore() { return sTimeline; }

ScratchAlloc scratch(VkDeviceSize size, VkDeviceSize align) {
    commandBuffer();
    Slot& s = sSlots[sOpen];
    if (align < 16) align = 16;
    for (ScratchBlock& b : s.scratch) {
        VkDeviceSize at = (b.used + align - 1) / align * align;
        if (at + size <= b.buf.size) {
            b.used = at + size;
            return ScratchAlloc{b.buf.buffer, at, b.buf.ptr + at};
        }
    }
    ScratchBlock b;
    VkDeviceSize cap = size > (4u << 20) ? size : (4u << 20);
    if (!createBuffer(cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      MemUse::Upload, &b.buf, MemCat::Scratch, "scratch")) {
        return ScratchAlloc{};
    }
    b.used = size;
    s.scratch.push_back(b);
    return ScratchAlloc{b.buf.buffer, 0, b.buf.ptr};
}

void setTimerSink(void (*sink)(int, double)) { sTimerSink = sink; }

void beginTimer(int tag) {
    if (!sTimersOk) return;
    VkCommandBuffer cmd = commandBuffer();
    Slot& s = sSlots[sOpen];
    if (!s.queriesReset) {
        vkResetQueryPool(sDev.device, s.queries, 0, kMaxTimers * 2);
        s.queriesReset = true;
    }
    if (s.timerOpen || s.timers >= kMaxTimers) return;
    s.tags[s.timers] = tag;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.queries, s.timers * 2);
    s.timerOpen = true;
}

void endTimer(int tag) {
    if (!sTimersOk || sOpen < 0) return;
    Slot& s = sSlots[sOpen];
    if (!s.timerOpen || s.tags[s.timers] != tag) return;
    vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.queries, s.timers * 2 + 1);
    s.timers++;
    s.timerOpen = false;
}

void setName(VkObjectType type, uint64_t handle, const char* name) {
    if (!vkSetDebugUtilsObjectNameEXT || !sDev.debugUtils || !sDev.device || !handle) return;
    VkDebugUtilsObjectNameInfoEXT ni{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    ni.objectType = type;
    ni.objectHandle = handle;
    ni.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT(sDev.device, &ni);
}

int validationErrors() { return sValidationErrors.load(); }
int validationWarnings() { return sValidationWarnings.load(); }

}  // namespace vk
}  // namespace gpu
