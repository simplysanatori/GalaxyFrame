// xr_gfx.h on Vulkan (XR_KHR_vulkan_enable2): the instance and device made
// through the runtime and handed to the renderer (gx/vk_device.h), the
// swapchain images wrapped as handles of the renderer's image table, and
// submission through the renderer's command buffers and timeline.
//
// SteamVR on the Frame shows a released swapchain image at once, finished or
// not, for Vulkan as for GLES (H1b): an image is released only once the GPU
// has finished the work recorded on it.  The frame loop (xr_app.cpp) waits
// for the frame's fence before it releases; release() checks it again and
// waits itself if needed (counted in the log).
//
// Images keep Vulkan's orientation (the first row in memory is the top of the
// picture, as the runtime reads it); the passes that write them flip the
// picture (vr_draw_vk.cpp).
#include "../gx/vk_device.h"  // first: Vulkan without prototypes
#include "../gx/vk_mem.h"

#include <stdio.h>
#include <stdlib.h>

#include <unordered_map>
#include <string.h>

#include "xr_gfx.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>

#include "port/port.h"

#define XR_CHECK(call)                                                                                                                               \
    do {                                                                                                                                             \
        XrResult _r = (call);                                                                                                                        \
        if (XR_FAILED(_r)) {                                                                                                                         \
            port_fatal("%s failed: %d (%s:%d)", #call, (int)_r, __FILE__, __LINE__);                                                                 \
        }                                                                                                                                            \
    } while (0)

namespace xrgfx {
namespace {

using namespace gpu::vk;

}  // namespace
void noteVulkanImageUse(uint32_t handle);
namespace {

// The timeline value of the command buffer that last recorded work on each
// swapchain image (vr_draw_vk.cpp notes its passes): the image is released
// once that has finished.
std::unordered_map<uint32_t, uint64_t> sUse;

bool hasExtension(const std::vector<XrExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts) {
        if (strcmp(e.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

const VkPipelineStageFlags2 kColorStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
const VkAccessFlags2 kColorAccess = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;

class VulkanGfx : public Gfx {
public:
    Api api() const override { return Api::Vulkan; }

    void createContext() override {
        if (!loadLoader()) {
            port_fatal("vk: no Vulkan loader (libvulkan.so.1)");
        }
    }

    void instanceExtensions(const std::vector<XrExtensionProperties>& offered, std::vector<const char*>* enable) override {
        if (!hasExtension(offered, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) {
            port_fatal("the OpenXR runtime has no Vulkan binding (XR_KHR_vulkan_enable2)");
        }
        // The same list as the GLES side enables (the binding is decided at
        // session creation).
        if (hasExtension(offered, "XR_MNDX_egl_enable") && hasExtension(offered, "XR_KHR_opengl_es_enable")) {
            enable->push_back("XR_KHR_opengl_es_enable");
            enable->push_back("XR_MNDX_egl_enable");
        }
        enable->push_back(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    }

    void attach(XrInstance instance, XrSystemId system) override {
        PFN_xrGetVulkanGraphicsRequirements2KHR getReq = nullptr;
        PFN_xrCreateVulkanInstanceKHR createInstance = nullptr;
        PFN_xrGetVulkanGraphicsDevice2KHR getDevice = nullptr;
        PFN_xrCreateVulkanDeviceKHR createDevice = nullptr;
        XR_CHECK(xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction*)&getReq));
        XR_CHECK(xrGetInstanceProcAddr(instance, "xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction*)&createInstance));
        XR_CHECK(xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction*)&getDevice));
        XR_CHECK(xrGetInstanceProcAddr(instance, "xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction*)&createDevice));
        XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
        XR_CHECK(getReq(instance, system, &req));
        port_log("xr vk: the runtime takes Vulkan %d.%d to %d.%d", (int)XR_VERSION_MAJOR(req.minApiVersionSupported),
                 (int)XR_VERSION_MINOR(req.minApiVersionSupported), (int)XR_VERSION_MAJOR(req.maxApiVersionSupported),
                 (int)XR_VERSION_MINOR(req.maxApiVersionSupported));

        InstanceSetup is;
        if (!fillInstanceSetup(&is)) {
            port_fatal("vk: no Vulkan 1.2 instance possible (see above)");
        }
        XrVulkanInstanceCreateInfoKHR ici{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
        ici.systemId = system;
        ici.pfnGetInstanceProcAddr = instanceProcAddr();
        ici.vulkanCreateInfo = &is.info;
        VkInstance vkInstance = VK_NULL_HANDLE;
        VkResult vr = VK_SUCCESS;
        XR_CHECK(createInstance(instance, &ici, &vkInstance, &vr));
        if (vr != VK_SUCCESS || !adoptInstance(vkInstance, false)) {
            port_fatal("vk: the runtime could not make the Vulkan instance (%d)", (int)vr);
        }

        XrVulkanGraphicsDeviceGetInfoKHR gi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
        gi.systemId = system;
        gi.vulkanInstance = vkInstance;
        VkPhysicalDevice phys = VK_NULL_HANDLE;
        XR_CHECK(getDevice(instance, &gi, &phys));
        // The renderer was chosen after a probe of this GPU
        // (gpu::createVulkanRenderer): a GPU that fails here is not the one
        // probed, and there is no going back to GLES at this point.
        DeviceSetup ds;
        if (!fillDeviceSetup(phys, &ds)) {
            port_fatal("vk: the runtime's GPU cannot run the Vulkan renderer (see above); start with renderer = gles");
        }
        XrVulkanDeviceCreateInfoKHR dci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
        dci.systemId = system;
        dci.pfnGetInstanceProcAddr = instanceProcAddr();
        dci.vulkanPhysicalDevice = phys;
        dci.vulkanCreateInfo = &ds.info;
        VkDevice device = VK_NULL_HANDLE;
        XR_CHECK(createDevice(instance, &dci, &device, &vr));
        if (vr != VK_SUCCESS || !adoptDevice(phys, device, ds.queueFamily, 0, false, &ds.info)) {
            port_fatal("vk: the runtime could not make the Vulkan device (%d)", (int)vr);
        }
        mBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
        mBinding.instance = vkInstance;
        mBinding.physicalDevice = phys;
        mBinding.device = device;
        mBinding.queueFamilyIndex = ds.queueFamily;
        mBinding.queueIndex = 0;
    }

    const void* sessionBinding() override { return &mBinding; }

    void sessionCreated(XrSession session) override {
        uint32_t fmtCount = 0;
        XR_CHECK(xrEnumerateSwapchainFormats(session, 0, &fmtCount, nullptr));
        mFormats.resize(fmtCount);
        XR_CHECK(xrEnumerateSwapchainFormats(session, fmtCount, &fmtCount, mFormats.data()));
        std::string list;
        for (int64_t f : mFormats) {
            char b[16];
            snprintf(b, sizeof(b), " %lld", (long long)f);
            list += b;
        }
        port_log("xr vk: swapchain formats (VkFormat):%s", list.c_str());
    }

    bool hasFormat(ImageKind kind) const override {
        switch (kind) {
        case ImageKind::Eye:
            return !mFormats.empty();
        case ImageKind::Ui:
            return offers(VK_FORMAT_R8G8B8A8_SRGB);
        case ImageKind::Motion:
        case ImageKind::Depth:
            // SpaceWarp stays with GLES for now (xr_app.cpp turns it off).
            return false;
        }
        return false;
    }

    bool createChain(XrSession session, ImageKind kind, int width, int height, uint32_t mips, Chain* out) override {
        if (kind != ImageKind::Eye && kind != ImageKind::Ui) {
            port_log("xr vk: no motion vector or depth swapchains on Vulkan");
            return false;
        }
        // SteamVR on the Frame accepts mip levels for a swapchain but allocates
        // the image's memory for level 0 only (H2: a 2048x1152 swapchain of 12
        // levels got 9.47 MB, one level is 9.44 MB): blitting the levels below
        // wrote past the image, into the other images' memory, and the screen
        // showed blocks of garbage.  One level on Vulkan (the compositor
        // filters level 0, as it did for the GLES swapchains).
        if (mips > 1) {
            static bool logged = false;
            if (!logged) port_log("xr vk: UI layer swapchains get one mip level (SteamVR allocates no more), %u asked", mips);
            logged = true;
            mips = 1;
        }
        Chain& sc = *out;
        sc.width = width;
        sc.height = height;
        sc.mips = mips;
        int64_t format = VK_FORMAT_R8G8B8A8_SRGB;
        if (kind == ImageKind::Eye && !offers(format)) {
            format = mFormats[0];
            port_log("xr vk: no R8G8B8A8_SRGB eye swapchains; using format %lld", (long long)format);
        }
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        // Transfer source and destination: the mip levels are blitted; the
        // debug shots (PETARI_VRSHOT_MS) read the images back.
        static const bool shots = getenv("PETARI_VRSHOT_MS") != nullptr;
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        if (shots || mips > 1) ci.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
        if (mips > 1) ci.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = format;
        ci.sampleCount = 1;
        ci.width = (uint32_t)width;
        ci.height = (uint32_t)height;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = mips;
        sc.format = format;
        XrResult r = xrCreateSwapchain(session, &ci, &sc.handle);
        if (XR_FAILED(r) && ci.usageFlags != (XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT)) {
            // Without the transfer usages (the runtime refused them): one
            // level, no read-back.
            port_log("xr vk: a %dx%d swapchain with transfer usage refused (%d); made without, %u mip levels -> 1", width, height, (int)r, mips);
            ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            ci.mipCount = 1;
            sc.mips = mips = 1;
            r = xrCreateSwapchain(session, &ci, &sc.handle);
        }
        if (XR_FAILED(r)) port_fatal("xrCreateSwapchain failed: %d", (int)r);
        uint32_t n = 0;
        XR_CHECK(xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr));
        std::vector<XrSwapchainImageVulkan2KHR> images(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        XR_CHECK(xrEnumerateSwapchainImages(sc.handle, n, &n, (XrSwapchainImageBaseHeader*)images.data()));
        sc.images.assign(n, gpu::EfbTarget());
        for (uint32_t i = 0; i < n; i++) {
            uint32_t h = wrap(images[i].image, (VkFormat)format, width, height, mips, "xr swapchain image");
            if (!h) port_fatal("xr vk: cannot wrap a swapchain image");
            gpu::EfbTarget& t = sc.images[i];
            t.fbo = t.color = h;
            t.width = width;
            t.height = height;
        }
        return true;
    }

    uint32_t acquire(Chain& chain) override {
        uint32_t idx = acquireImage(chain.handle);
        Acquired a;
        a.swapchain = chain.handle;
        a.handle = idx < chain.images.size() ? chain.images[idx].color : 0;
        mAcquired.push_back(a);
        return idx;
    }

    void wait(Chain& chain) override {
        waitImage(chain.handle);
        // XR_KHR_vulkan_enable2: after the wait a color image is in
        // COLOR_ATTACHMENT_OPTIMAL, and goes back in that layout.
        for (Acquired& a : mAcquired) {
            if (a.swapchain == chain.handle && !a.waited) {
                a.waited = true;
                sUse[a.handle] = 0;
                assumeImageLayout(a.handle, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                break;
            }
        }
    }

    void release(Chain& chain) override {
        // The oldest image acquired from the swapchain goes back.
        size_t i = 0;
        while (i < mAcquired.size() && mAcquired[i].swapchain != chain.handle) i++;
        if (i < mAcquired.size()) {
            Acquired a = mAcquired[i];
            mAcquired.erase(mAcquired.begin() + (long)i);
            a.value = sUse[a.handle];
            ImageInfo info;
            bool unsubmitted = commandBufferOpen() && a.value >= pendingValue();
            if (!a.value || unsubmitted || (imageInfo(a.handle, &info) && info.layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)) {
                // Nothing recorded on it, or recorded and not yet submitted:
                // back to the layout the runtime takes, then submitted.
                transitionImage(a.handle, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kColorStage, kColorAccess);
                a.value = gpu::vk::submit();
            }
            if (completedValue() < a.value) {
                // The frame loop waits before it releases; this is the safety net.
                int64_t t0 = port_host_time_ns();
                waitValue(a.value);
                mLateReleases++;
                mLateWaitNs += port_host_time_ns() - t0;
            }
        }
        releaseImage(chain.handle);
        int64_t now = port_host_time_ns();
        if (mLateReleases && now - mLateLogAt > 10000000000ll) {
            port_log("xr vk: %d swapchain images released after a wait in the release itself (%.2f ms avg): the frame loop did not wait for them",
                     mLateReleases, mLateWaitNs / 1e6 / mLateReleases);
            mLateReleases = 0;
            mLateWaitNs = 0;
            mLateLogAt = now;
        }
    }

    Fence submit() override {
        // Every image acquired goes back to the runtime's layout at the end of
        // the work recorded on it, and is released once that work is done.
        for (Acquired& a : mAcquired) {
            ImageInfo info;
            if (imageInfo(a.handle, &info) && info.layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
                transitionImage(a.handle, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kColorStage, kColorAccess);
                noteVulkanImageUse(a.handle);
            }
        }
        return (Fence)gpu::vk::submit();
    }

    bool waitFence(Fence fence, int64_t timeoutNs) override {
        if (!fence) return true;
        if (timeoutNs <= 0) return completedValue() >= (uint64_t)fence;
        return waitValue((uint64_t)fence, (uint64_t)timeoutNs);
    }

    void destroyFence(Fence) override {}

    void finish() override { finishAll(); }

    gpu::EfbTarget createImage(ImageKind kind, int width, int height, uint32_t mips) override {
        gpu::EfbTarget t;
        if (kind != ImageKind::Eye && kind != ImageKind::Ui) {
            static bool logged = false;
            if (!logged) port_log("xr vk: no motion vector or depth images on Vulkan (SpaceWarp is off)");
            logged = true;
            return t;
        }
        if (!deviceReady()) {
            port_log("xr vk: createImage before the device exists");
            return t;
        }
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R8G8B8A8_SRGB;  // like the swapchains
        ci.extent = {(uint32_t)width, (uint32_t)height, 1};
        ci.mipLevels = mips ? mips : 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage image = VK_NULL_HANDLE;
        VkDevice dev = device().device;
        if (vkCreateImage(dev, &ci, nullptr, &image) != VK_SUCCESS) {
            port_log("xr vk: cannot create a %dx%d image", width, height);
            return t;
        }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, image, &req);
        Allocation mem;
        if (!allocateMemory(req, MemUse::Device, MemCat::Target, false, &mem, "xr sim image") || vkBindImageMemory(dev, image, mem.memory, 0) != VK_SUCCESS) {
            vkDestroyImage(dev, image, nullptr);
            port_log("xr vk: no memory for a %dx%d image", width, height);
            return t;
        }
        uint32_t h = wrap(image, ci.format, width, height, ci.mipLevels, "xr sim image");
        t.fbo = t.color = h;
        t.width = width;
        t.height = height;
        return t;
    }

private:
    struct Acquired {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        uint32_t handle = 0;
        bool waited = false;
        uint64_t value = 0;  // timeline value of the last submission after it was acquired
    };

    // A handle of the renderer's image table for `image`: a view of every
    // mip level for sampling, one of level 0 to draw into.
    uint32_t wrap(VkImage image, VkFormat format, int width, int height, uint32_t mips, const char* name) {
        VkDevice dev = device().device;
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips ? mips : 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE, attView = VK_NULL_HANDLE;
        if (vkCreateImageView(dev, &vci, nullptr, &view) != VK_SUCCESS) return 0;
        if (mips > 1) {
            vci.subresourceRange.levelCount = 1;
            if (vkCreateImageView(dev, &vci, nullptr, &attView) != VK_SUCCESS) return 0;
        } else {
            attView = view;
        }
        setName(VK_OBJECT_TYPE_IMAGE, (uint64_t)image, name);
        return registerImage(image, view, attView, format, (uint32_t)width, (uint32_t)height, VK_IMAGE_LAYOUT_UNDEFINED);
    }

    bool offers(int64_t format) const {
        for (int64_t f : mFormats) {
            if (f == format) {
                return true;
            }
        }
        return false;
    }

    XrGraphicsBindingVulkan2KHR mBinding{};
    std::vector<int64_t> mFormats;
    std::vector<Acquired> mAcquired;  // in the order acquired
    int mLateReleases = 0;
    int64_t mLateWaitNs = 0, mLateLogAt = 0;
};

}  // namespace

void noteVulkanImageUse(uint32_t handle) {
    commandBuffer();  // the value the recording goes out with
    sUse[handle] = pendingValue();
}

Gfx* createVulkanGfx() { return new VulkanGfx(); }

}  // namespace xrgfx
