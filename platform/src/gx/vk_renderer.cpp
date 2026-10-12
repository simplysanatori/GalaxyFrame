// Vulkan backend of gpu::Renderer: replays prepared frames (gpu_prepare.h)
// with Vulkan.  gl_renderer.cpp is the reference for what every item does;
// this file does the same with dynamic rendering and dynamic state.
//
// Images keep GL's memory layout: the standard Vulkan viewport maps clip y
// -1 to row 0 as GL's viewport does, so the viewport, scissor and copy
// rectangles of gpu_prepare.h apply unchanged (row 0 is the bottom of the
// picture), and only the front face is inverted (Vulkan judges winding with
// y pointing down).
#include "vk_device.h"  // first: Vulkan without prototypes
#include "vk_mem.h"
#include "vk_pipeline.h"
#include "tex_decode.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gpu.h"
#include "gpu_prepare.h"
#include "gpu_renderer.h"
#include "port/heap_routing.h"
#include "port/port.h"

extern "C" void port_headless_write_png(const char* path, const unsigned char* rgba, int w, int h);

namespace gpu {

using namespace vk;

namespace {

// ---------------------------------------------------------------------------
// Images by handle (TexHandle): textures, render targets, EFB copies.
// ---------------------------------------------------------------------------
enum class ImgKind : u8 { None, Texture, Color, Depth, Copy, Dummy, External };

struct Img {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;     // sampled (depth formats: the depth aspect)
    VkImageView attView = VK_NULL_HANDLE;  // attachment (depth formats: depth and stencil)
    Allocation mem;
    VkFormat format = VK_FORMAT_UNDEFINED;
    u32 width = 0, height = 0, levels = 1;
    ImgKind kind = ImgKind::None;
    // Its state as recorded so far (rendering thread).
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stages = 0;
    VkAccessFlags2 access = 0;
    bool depth() const { return format == kDepthFormat; }
};

class ImageTable {
public:
    u32 alloc() {
        std::lock_guard<std::mutex> lock(mLock);
        u32 h;
        if (!mFree.empty()) {
            h = mFree.back();
            mFree.pop_back();
        } else {
            h = mNext++;
            if (h / kChunk >= kChunks) {
                return 0;
            }
            if (!mChunks[h / kChunk].load()) {
                mChunks[h / kChunk].store(new Img[kChunk]);
            }
        }
        mLive++;
        return h;
    }
    void release(u32 h) {
        std::lock_guard<std::mutex> lock(mLock);
        (*this)[h] = Img();
        mFree.push_back(h);
        mLive--;
    }
    Img& operator[](u32 h) { return mChunks[h / kChunk].load()[h % kChunk]; }
    bool valid(u32 h) { return h && h < kChunk * kChunks && mChunks[h / kChunk].load() && (*this)[h].image; }
    size_t live() const { return mLive; }

private:
    static const u32 kChunk = 1024, kChunks = 1024;
    std::atomic<Img*> mChunks[kChunks] = {};
    std::mutex mLock;
    std::vector<u32, PortHostAllocator<u32>> mFree;
    u32 mNext = 1;  // 0 = none
    size_t mLive = 0;
};

ImageTable sImages;
bool sKeepDepth = false;

// ---------------------------------------------------------------------------
// Foveation (VulkanRenderer::setFoveation): a fragment density map for a
// target, attached to the rendering instances of the renders into it.  Each
// target has a ring of maps in linear, host-visible memory written by the
// CPU (Turnip offers density maps in R16G16_SFLOAT and R32G32_SFLOAT, linear
// tiling included),
// so a map changing every frame (the gaze) needs no copy and no wait: a map
// is rewritten only once the GPU has finished the render that used it.
// ---------------------------------------------------------------------------
const VkFormat kFdmFormat = VK_FORMAT_R16G16_SFLOAT;
const int kFdmRing = 4;
struct FdmMap {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    Allocation mem;
    VkSubresourceLayout layout{};
    uint64_t usedBy = 0;  // timeline value of the last render that read it
};
struct Fdm {
    u32 w = 0, h = 0;      // map texels
    u32 tw = 0, th = 0;    // target pixels
    FdmMap ring[kFdmRing];
    int next = 0;
    int current = -1;      // the map the target's renders use now (-1: none)
    bool initialized = false;
};
std::unordered_map<u32, Fdm> sFdm;  // by the target's color handle
bool sFdmChecked = false, sFdmOk = false;

void destroyFdm(Fdm& f) {
    VkDevice dev = device().device;
    for (FdmMap& m : f.ring) {
        if (m.view) vkDestroyImageView(dev, m.view, nullptr);
        if (m.image) vkDestroyImage(dev, m.image, nullptr);
        freeMemory(m.mem);
        m = FdmMap();
    }
}

bool createFdmMap(FdmMap& m, u32 w, u32 h) {
    VkDevice dev = device().device;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = kFdmFormat;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_LINEAR;
    ci.usage = VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT;
    ci.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    if (vkCreateImage(dev, &ci, nullptr, &m.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, m.image, &req);
    if (!allocateMemory(req, MemUse::Upload, MemCat::Misc, true, &m.mem, "density map") || vkBindImageMemory(dev, m.image, m.mem.memory, 0) != VK_SUCCESS) {
        return false;
    }
    VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    vkGetImageSubresourceLayout(dev, m.image, &sub, &m.layout);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = m.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = kFdmFormat;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev, &vci, nullptr, &m.view) != VK_SUCCESS) return false;
    setName(VK_OBJECT_TYPE_IMAGE, (uint64_t)m.image, "density map");
    return true;
}

// Density around the focal point (normalized device coordinates, y up as
// the target is stored): full within `inner`, then the larger of `edge` and
// a half out to 1.6 x `inner`, then `edge` (per axis; Turnip makes
// fragments of 1, 2 or 4 pixels a side from it).  Turnip applies the map per
// tile of its tiled rendering (a tile of an eye is about a third of its
// width), at the tile's lowest density as measured: a full-density region
// smaller than a tile does not show.
uint16_t halfFloat(float v) {
    // v in 1/16..1: a normal half float.
    uint32_t b;
    memcpy(&b, &v, 4);
    return (uint16_t)(((b >> 16) & 0x8000) | ((((b >> 23) & 0xff) - 112) << 10) | ((b >> 13) & 0x3ff));
}

void fillFdm(Fdm& f, FdmMap& m, float fx, float fy, float inner, float edge) {
    uint8_t* base = m.mem.ptr + m.layout.offset;
    float aspect = (float)f.tw / (float)f.th;  // round on the image, not in NDC
    for (u32 y = 0; y < f.h; y++) {
        uint16_t* row = (uint16_t*)(base + y * m.layout.rowPitch);
        float ny = ((y + 0.5f) * f.th / f.h) / f.th * 2.0f - 1.0f;
        for (u32 x = 0; x < f.w; x++) {
            float nx = ((x + 0.5f) * f.tw / f.w) / f.tw * 2.0f - 1.0f;
            float dx = (nx - fx) * aspect, dy = ny - fy;
            float r = sqrtf(dx * dx + dy * dy);
            float v = r < inner ? 1.0f : r < 1.6f * inner ? fmaxf(edge, 0.5f) : edge;
            uint16_t d = halfFloat(fminf(1.0f, fmaxf(0.0625f, v)));
            row[x * 2] = d;
            row[x * 2 + 1] = d;
        }
    }
}
  // store the depth at the end of a render (vk::setKeepDepth)

u32 newImage(u32 w, u32 h, u32 levels, VkFormat format, VkImageUsageFlags usage, ImgKind kind, MemCat cat, const char* name) {
    VkDevice dev = device().device;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w ? w : 1, h ? h : 1, 1};
    ci.mipLevels = levels ? levels : 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    VkResult r = vkCreateImage(dev, &ci, nullptr, &image);
    if (r != VK_SUCCESS) {
        port_log("vk: cannot create a %ux%u %s image (%d)", w, h, name, (int)r);
        return 0;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, image, &req);
    Allocation mem;
    if (!allocateMemory(req, MemUse::Device, cat, false, &mem, name) || vkBindImageMemory(dev, image, mem.memory, 0) != VK_SUCCESS) {
        vkDestroyImage(dev, image, nullptr);
        freeMemory(mem);
        return 0;
    }
    bool depth = format == kDepthFormat;
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {(VkImageAspectFlags)(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, ci.mipLevels, 0, 1};
    VkImageView view = VK_NULL_HANDLE, attView = VK_NULL_HANDLE;
    bool ok = vkCreateImageView(dev, &vci, nullptr, &view) == VK_SUCCESS;
    if (ok && depth) {
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        ok = vkCreateImageView(dev, &vci, nullptr, &attView) == VK_SUCCESS;
    } else {
        attView = view;
    }
    u32 handle = ok ? sImages.alloc() : 0;
    if (!handle) {
        if (view) vkDestroyImageView(dev, view, nullptr);
        if (attView && attView != view) vkDestroyImageView(dev, attView, nullptr);
        vkDestroyImage(dev, image, nullptr);
        freeMemory(mem);
        port_log("vk: cannot make a view or handle for a %s image", name);
        return 0;
    }
    Img& im = sImages[handle];
    im.image = image;
    im.view = view;
    im.attView = attView;
    im.mem = mem;
    im.format = format;
    im.width = ci.extent.width;
    im.height = ci.extent.height;
    im.levels = ci.mipLevels;
    im.kind = kind;
    im.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    im.stages = 0;
    im.access = 0;
    setName(VK_OBJECT_TYPE_IMAGE, (uint64_t)image, name);
    return handle;
}

void destroyImageNow(u32 h) {
    if (!h || !sImages.valid(h)) return;
    Img& im = sImages[h];
    VkDevice dev = device().device;
    if (im.kind != ImgKind::External) {
        if (im.attView && im.attView != im.view) vkDestroyImageView(dev, im.attView, nullptr);
        if (im.view) vkDestroyImageView(dev, im.view, nullptr);
        vkDestroyImage(dev, im.image, nullptr);
        freeMemory(im.mem);
    }
    sImages.release(h);
}

// Once the GPU no longer needs it (rendering thread).
void destroyImageLater(u32 h) {
    if (!h) return;
    deferUntilDone([h] { destroyImageNow(h); });
}

// ---------------------------------------------------------------------------
// Barriers: each image's layout and last access are tracked as commands are
// recorded (in submission order), and a use that conflicts with the last one
// gets a barrier.
// ---------------------------------------------------------------------------
const VkAccessFlags2 kWriteAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT |
                                    VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
const VkPipelineStageFlags2 kStageColor = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
const VkAccessFlags2 kAccessColor = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
const VkPipelineStageFlags2 kStageDepth = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
const VkAccessFlags2 kAccessDepth = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
const VkPipelineStageFlags2 kStageSample = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
const VkAccessFlags2 kAccessSample = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
const VkPipelineStageFlags2 kStageTransfer = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;

struct Barriers {
    VkImageMemoryBarrier2 b[8];
    u32 n = 0;

    void use(Img& im, VkImageLayout layout, VkPipelineStageFlags2 stages, VkAccessFlags2 access, bool discard = false) {
        bool change = im.layout != layout || discard;
        if (!change && !(im.access & kWriteAccess) && !(access & kWriteAccess)) {
            // Reads after reads in the same layout: no barrier, but a later
            // write waits for all of them.
            im.stages |= stages;
            im.access |= access;
            return;
        }
        if (n == 8) flush();
        VkImageMemoryBarrier2& m = b[n++];
        m = VkImageMemoryBarrier2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        m.srcStageMask = im.stages ? im.stages : VK_PIPELINE_STAGE_2_NONE;
        m.srcAccessMask = im.access & kWriteAccess;
        m.dstStageMask = stages;
        m.dstAccessMask = access;
        m.oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : im.layout;
        m.newLayout = layout;
        m.srcQueueFamilyIndex = m.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        m.image = im.image;
        m.subresourceRange = {(VkImageAspectFlags)(im.depth() ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0,
                              VK_REMAINING_MIP_LEVELS, 0, 1};
        im.layout = layout;
        im.stages = stages;
        im.access = access;
    }
    void flush() {
        if (!n) return;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = n;
        di.pImageMemoryBarriers = b;
        vkCmdPipelineBarrier2KHR(commandBuffer(), &di);
        n = 0;
    }
};

// ---------------------------------------------------------------------------
// The neutral render state of gpu_prepare.h in Vulkan terms
// ---------------------------------------------------------------------------
VkPrimitiveTopology toVkPrim(Prim p) {
    static const VkPrimitiveTopology m[] = {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
    return m[(int)p];
}

VkBlendFactor toVkBlendFactor(BlendFactor f) {
    static const VkBlendFactor m[] = {VK_BLEND_FACTOR_ZERO,      VK_BLEND_FACTOR_ONE,           VK_BLEND_FACTOR_SRC_COLOR,
                                      VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
                                      VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
                                      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_CONSTANT_ALPHA};
    return m[(int)f];
}

VkBlendOp toVkBlendOp(BlendOp op) { return op == BlendOp::ReverseSubtract ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD; }

VkCompareOp toVkCompare(CompareFunc f) {
    static const VkCompareOp m[] = {VK_COMPARE_OP_NEVER,   VK_COMPARE_OP_LESS,      VK_COMPARE_OP_EQUAL,         VK_COMPARE_OP_LESS_OR_EQUAL,
                                    VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    return m[(int)f];
}

VkCullModeFlags toVkCull(CullFace f) {
    static const VkCullModeFlags m[] = {VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_BACK_BIT, VK_CULL_MODE_FRONT_AND_BACK};
    return m[(int)f];
}

// ---------------------------------------------------------------------------
// Per-frame GPU data: the recording's vertices and the shader storage
// buffers of a prepared frame, in one host-visible buffer, and the
// descriptor set that points at them.
// ---------------------------------------------------------------------------
enum { kVerts, kIndices, kRows, kRecs, kStates, kVtxRec, kRegions };

struct FrameData {
    Buffer buf;
    VkDeviceSize off[kRegions] = {}, size[kRegions] = {};
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint64_t retire = 0;  // the GPU is done with it once the timeline reaches this
    bool inUse = false;   // filled for, or by, a frame not yet retired
};

struct VkPrepared : Prepared {
    FrameData* data = nullptr;
};

struct CopyTex {
    u32 handle = 0;
    int w = 0, h = 0;
    u64 frame = 0;  // frame whose replay last wrote it
};

class VulkanRenderer;

const u32 kEyeEntries = 1024;   // EyeBlock ring entries
const u32 kEyeStride = 256;     // >= sizeof(EyeBlock), a multiple of minUniformBufferOffsetAlignment
const u32 kMaxFrameData = 12;
const u32 kAutoSubmitRenders = 24;  // renders in one command buffer before it is submitted anyway

// Render state recorded so far in the command buffer: only changes are
// recorded (a frame has thousands of batches).  All bits set = unknown.
struct DynState {
    VkPipeline pipe;
    float vp[4];
    VkRect2D sc;
    u32 colorMask;
    u32 blendOn;
    VkColorBlendEquationEXT eq;
    float blendAlpha;
    u32 depthTest, depthWrite;
    VkCompareOp depthOp;
    VkCullModeFlags cull;
    VkPrimitiveTopology topo;
    u32 stencilTest, stencilWrite, stencilRef;
    VkImageView views[kVkNumSamplers];
    VkSampler samps[kVkNumSamplers];
    VkDeviceSize vbOff;
    u32 firstOrdinal;
    u32 fixedSet;  // front face, stencil op, compare mask, line width
    void forget() { memset(this, 0xFF, sizeof(*this)); }
};

}  // namespace

// ---------------------------------------------------------------------------
// The renderer
// ---------------------------------------------------------------------------
namespace {

class VulkanRenderer final : public Renderer {
public:
    Api api() const override { return Api::Vulkan; }
    bool init() override;
    Target createTarget(int width, int height) override;
    Target createDepthTarget(int width, int height) override;
    void destroyTarget(Target& t) override;
    bool setFoveation(Target& t, float focalX, float focalY, float gain, float foveaArea) override;
    void setFrame(std::shared_ptr<const Frame> frame) override;
    bool update() override;
    bool hasFrame() const override;
    uint64_t frameNumber() const override;
    void render(Target& efb, const EyeView* eye, Target* hud, HudMode hudMode = HudMode::Inline) override;
    bool snapshotTaken() const override;
    int hudDrawCount() const override;
    const CameraInfo& camera() const override;
    TexHandle xfbTexture() const override { return mXfbTex; }
    int xfbWidth() const override { return mXfbWidth; }
    int xfbHeight() const override { return mXfbHeight; }
    void finish() override;
    bool readPixels(const Target& t, uint8_t* rgba) override;

    // The last EFB->XFB copy (written by VkImpl::doCopy).
    TexHandle mXfbTex = 0;
    int mXfbWidth = 0, mXfbHeight = 0;
};

}  // namespace

struct VkImpl {
    bool ready = false;
    Pipelines pipes;
    VkDescriptorPool setPool = VK_NULL_HANDLE;
    Buffer eyeRing;
    u32 eyeNext = 0;
    uint64_t eyeUsed[kEyeEntries] = {};
    u32 dummy = 0;  // 1x1 black texture for units without one
    std::unordered_map<u64, VkSampler> samplers;
    VkSampler copyColorSampler = VK_NULL_HANDLE, copyDepthSampler = VK_NULL_HANDLE;
    float aniso = 0.0f;
    std::unordered_map<u64, CopyTex> copies;  // by copyKey()
    u32 copySlot = 0;                          // of the render in progress
    Buffer readback;

    // The frame being replayed.
    std::unique_ptr<VkPrepared> cur;
    Builder syncBuilder;  // setFrame()

    // Background preparation (update()).
    std::thread worker;
    std::mutex workLock;
    std::condition_variable workCv;
    std::shared_ptr<const Frame> wanted;
    std::unique_ptr<VkPrepared> built, spare;
    u64 lastRequested = 0;
    Builder workerBuilder;
    std::atomic<bool> async{false};  // the worker runs: programs compile in the background

    std::mutex fdLock;
    std::vector<std::unique_ptr<FrameData>> frameData;

    // Texture uploads (worker or rendering thread), and those left for the
    // rendering thread's command buffer when host image copy is missing.
    std::mutex texLock;
    struct PendingUpload {
        std::shared_ptr<TexImage> img;
        u32 handle;
    };
    std::vector<PendingUpload> pendingUploads;

    // Per execute().
    Barriers bar;
    DynState ds;
    Target* passTarget = nullptr;  // target of the open rendering instance
    Target* efbTarget = nullptr;
    Target* hudTarget = nullptr;
    bool efbNeedsClear = false, hudNeedsClear = false;
    VkClearValue efbClear[2], hudClear[2];
    int lastBreak = -1;     // index of the last item that ends a rendering instance early
    bool snapshotTaken = false;
    int hudDraws = 0;
    bool dumpingCopies = false;
    u64 copyDumpFrame = ~0ull;
    u32 rendersInCmd = 0;
    uint64_t lastCmdValue = 0;  // timeline value of the command buffer rendersInCmd counts in

    // Statistics for the log.
    struct Stats {
        int renders = 0, passes = 0, breaks = 0, maxBreaks = 0, copies = 0, skipped = 0;
        double gpuMs = 0.0, gpuMaxMs = 0.0, cpuMs = 0.0, cpuMaxMs = 0.0;
        int gpuCount = 0;
    } st;
    int curPasses = 0, curCopies = 0;
    // Which paths the renders took, since start (PETARI_VKSTATS, at finish).
    struct Coverage {
        long zCopies = 0, maskedClears = 0, attachmentClears = 0, xfbBlits = 0, snapshots = 0, lineBatches = 0, pointBatches = 0, emptyViewports = 0,
             hudSwitches = 0;
    } cov;
    double finishGpuMs = 0.0;  // renders timed since the last finish() (PETARI_GPUTIME)
    int finishGpuCount = 0;
    int64_t lastLogNs = 0, lastMemLogNs = 0;
    u64 renderSerial = 0;

    bool init();
    void workerMain();
    void requestLatest();
    void prepareGpu(VkPrepared& p, bool syncPrograms, bool renderThread);
    FrameData* acquireFrameData(bool renderThread);
    void releaseFrameData(VkPrepared& p, uint64_t retire);
    void uploadTextures(const Frame& f);
    void drainPendingUploads();
    void flushDeletes();
    VkSampler sampler(u32 mode0, u32 mode1, u32 levels);
    CopyTex& copyTex(u64 key, int w, int h);
    void evictCopies();
    u32 allocEye(const EyeBlock& eb);
    void execute(Target& efb, const EyeView* eye, Target* hud, HudMode hudMode, VulkanRenderer& r);
    void ensurePass(Target& t, size_t index);
    void endPass();
    bool needsClear(const Target& t) const { return (&t == efbTarget && efbNeedsClear) || (&t == hudTarget && hudNeedsClear); }
    void setFixedState(VkCommandBuffer cmd);
    void doCopy(const Item& it, Target& src, VulkanRenderer& r, bool vr, size_t index);
    void maskedClear(Target& t, const CopyOp& op, int x0, int y0, int x1, int y1);
    void snapshot(Target& efb, Target& snap, size_t index);
    void dumpCopy(const CopyOp& op, u32 handle, int w, int h);
    void logStats(bool force);
};

namespace {

VkImpl* sImpl;

// The GPU times of the last renders, for vk::renderGpuMs.
struct TimedRender {
    int serial = 0;
    double ms = 0.0;
};
TimedRender sTimed[64];
int sLastSerial = 0;

void onTimer(int tag, double ms) {
    sTimed[tag & 63] = TimedRender{tag, ms};
    if (!sImpl) return;
    VkImpl::Stats& s = sImpl->st;
    s.gpuMs += ms;
    s.gpuMaxMs = ms > s.gpuMaxMs ? ms : s.gpuMaxMs;
    s.gpuCount++;
    sImpl->finishGpuMs += ms;
    sImpl->finishGpuCount++;
    static const bool perRender = debugEnv("PETARI_VKSTATS") != nullptr;
    if (perRender) port_log("vk: render %d GPU %.3f ms", tag, ms);
}

std::string siblingPath(const char* path, const char* name) {
    std::string p = path ? path : "";
    if (p.empty()) return std::string();
    size_t slash = p.rfind('/');
    return (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) + name;
}

}  // namespace

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
bool VkImpl::init() {
    PortHostAllocScope hostAlloc;
    VkDevice dev = device().device;
    std::string base = shaderCachePath();
    // Beside the GLES cache: spirv.bin and pipelines.bin.
    if (!pipes.init(siblingPath(base.c_str(), "spirv.bin"), siblingPath(base.c_str(), "pipelines.bin"))) {
        return false;
    }
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * kMaxFrameData}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kMaxFrameData}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = kMaxFrameData;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &pci, nullptr, &setPool) != VK_SUCCESS) {
        port_log("vk: cannot create the descriptor pool");
        return false;
    }
    if (!createBuffer((VkDeviceSize)kEyeEntries * kEyeStride, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, MemUse::Upload, &eyeRing, MemCat::Misc, "eye blocks")) {
        return false;
    }
    static_assert(sizeof(EyeBlock) <= kEyeStride, "EyeBlock outgrew its ring stride");

    // Units without a texture read black (GL's texture 0 reads 0,0,0,1).
    dummy = newImage(1, 1, 1, kColorFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, ImgKind::Dummy, MemCat::Misc, "dummy texture");
    if (!dummy) return false;
    {
        Img& im = sImages[dummy];
        bar.use(im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kStageTransfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
        bar.flush();
        VkClearColorValue black = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(commandBuffer(), im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        bar.use(im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kStageSample, kAccessSample);
        bar.flush();
    }
    // The copy program samples the EFB like the GLES renderer's targets:
    // color linear, depth nearest, GL's default repeat.
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 0.25f;
    vkCreateSampler(dev, &sci, nullptr, &copyColorSampler);
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    vkCreateSampler(dev, &sci, nullptr, &copyDepthSampler);

    // Anisotropic filtering on mipmapped textures, as the GLES renderer
    // (PETARI_ANISO=<n>, default 8).
    {
        const char* want = debugEnv("PETARI_ANISO");
        float a = want ? (float)atof(want) : 8.0f;
        float max = device().maxAnisotropy;
        aniso = a > max ? max : a;
        port_log("vk: anisotropic filtering %.0fx (GPU allows %.0fx)", aniso, max);
    }
    setTimerSink(onTimer);
    if (!debugEnv("PETARI_VKNOPREWARM")) {
        pipes.prewarm();
    }
    finishAll();
    checkReservedWindow("renderer ready");
    logMemory("renderer ready");
    lastLogNs = lastMemLogNs = port_host_time_ns();
    ready = true;
    return true;
}

// ---------------------------------------------------------------------------
// Samplers (rendering thread), as the GLES renderer's, the way zink turns GL
// samplers into Vulkan ones: without a mip filter, level 0 only (LOD clamped
// to 0.25), and the anisotropy of mipmapped textures.
// ---------------------------------------------------------------------------
VkSampler VkImpl::sampler(u32 mode0, u32 mode1, u32 levels) {
    u64 key = ((u64)(mode0 & 0xFFFFFF) << 32) | ((mode1 & 0xFFFF) << 8) | (levels > 1 ? 1 : 0);
    auto it = samplers.find(key);
    if (it != samplers.end()) {
        return it->second;
    }
    static const VkSamplerAddressMode wrap[4] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                                 VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT};
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.addressModeU = wrap[mode0 & 3];
    sci.addressModeV = wrap[(mode0 >> 2) & 3];
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.magFilter = ((mode0 >> 4) & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    u32 minf = (mode0 >> 5) & 7;
    bool mips = levels > 1;
    bool mipFilter = false;
    VkSamplerMipmapMode mipMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    switch (minf) {
    case 0:
        sci.minFilter = VK_FILTER_NEAREST;
        break;
    case 1:
        sci.minFilter = VK_FILTER_NEAREST;
        mipFilter = mips;
        break;
    case 2:
        sci.minFilter = VK_FILTER_NEAREST;
        mipFilter = mips;
        mipMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        break;
    case 5:
        sci.minFilter = VK_FILTER_LINEAR;
        mipFilter = mips;
        break;
    case 6:
        sci.minFilter = VK_FILTER_LINEAR;
        mipFilter = mips;
        mipMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        break;
    default:
        sci.minFilter = VK_FILTER_LINEAR;
        break;
    }
    // GL's MIN_LOD / MAX_LOD (Mesa: min at least 0, swapped if reversed).
    float minLod = (mode1 & 0xFF) / 16.0f, maxLod = ((mode1 >> 8) & 0xFF) / 16.0f;
    if (maxLod < minLod) {
        float t = maxLod;
        maxLod = minLod;
        minLod = t;
    }
    if (mipFilter) {
        sci.mipmapMode = mipMode;
        sci.minLod = minLod;
        sci.maxLod = maxLod;
    } else {
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.minLod = minLod < 0.25f ? minLod : 0.25f;
        sci.maxLod = maxLod < 0.25f ? maxLod : 0.25f;
    }
    if (mips && aniso > 1.0f) {
        sci.anisotropyEnable = VK_TRUE;
        sci.maxAnisotropy = aniso;
    }
    VkSampler s = VK_NULL_HANDLE;
    if (vkCreateSampler(device().device, &sci, nullptr, &s) != VK_SUCCESS) {
        port_log("vk: cannot create a sampler");
        s = copyColorSampler;
    }
    samplers[key] = s;
    return s;
}

// ---------------------------------------------------------------------------
// EFB copy textures
// ---------------------------------------------------------------------------
CopyTex& VkImpl::copyTex(u64 key, int w, int h) {
    CopyTex& c = copies[key];
    if (c.handle && (c.w != w || c.h != h)) {
        destroyImageLater(c.handle);
        c.handle = 0;
    }
    if (!c.handle) {
        c.handle = newImage((u32)w, (u32)h, 1, kColorFormat,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            ImgKind::Copy, MemCat::Copy, key == kXfbCopyKey ? "xfb copy" : "efb copy");
        c.w = w;
        c.h = h;
    }
    c.frame = cur->number;
    return c;
}

// Copy targets of addresses (or views) no longer copied to (kCopyKeepFrames).
void VkImpl::evictCopies() {
    for (auto it = copies.begin(); it != copies.end();) {
        if (it->first != kXfbCopyKey && it->second.frame + kCopyKeepFrames < cur->number) {
            destroyImageLater(it->second.handle);
            it = copies.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// Frame data and uploads
// ---------------------------------------------------------------------------
FrameData* VkImpl::acquireFrameData(bool renderThread) {
    for (int attempt = 0;; attempt++) {
        uint64_t wait = 0;
        {
            std::lock_guard<std::mutex> lock(fdLock);
            uint64_t done = completedValue();
            FrameData* oldest = nullptr;
            for (auto& fd : frameData) {
                if (fd->inUse) continue;
                if (fd->retire <= done) {
                    fd->inUse = true;
                    return fd.get();
                }
                if (!oldest || fd->retire < oldest->retire) oldest = fd.get();
            }
            if (frameData.size() < kMaxFrameData) {
                std::unique_ptr<FrameData> fd(new FrameData());
                VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                ai.descriptorPool = setPool;
                ai.descriptorSetCount = 1;
                ai.pSetLayouts = &pipes.bufferSetLayout;
                if (vkAllocateDescriptorSets(device().device, &ai, &fd->set) != VK_SUCCESS) {
                    port_log("vk: cannot allocate a frame descriptor set");
                    return nullptr;
                }
                fd->inUse = true;
                frameData.push_back(std::move(fd));
                return frameData.back().get();
            }
            if (oldest) wait = oldest->retire;
        }
        if (wait) {
            // Only the rendering thread may submit what is still being recorded.
            if (renderThread && commandBufferOpen() && wait >= pendingValue()) submit();
            if (!waitValue(wait, 2000000000ull) && attempt % 5 == 4) {
                port_log("vk: waiting for frame data (timeline %llu, done %llu)", (unsigned long long)wait, (unsigned long long)completedValue());
            }
        } else {
            port_log("vk: all %u frame data sets in use", kMaxFrameData);
            port_host_sleep_ns(1000000);
        }
    }
}

void VkImpl::releaseFrameData(VkPrepared& p, uint64_t retire) {
    if (!p.data) return;
    std::lock_guard<std::mutex> lock(fdLock);
    p.data->retire = retire;
    p.data->inUse = false;
    p.data = nullptr;
}

void VkImpl::uploadTextures(const Frame& f) {
    // Images never change once recorded, so the pixels are not needed again
    // (except by the PETARI_DUMP texture dump and PETARI_FRAMESAVE).
    static const bool keepPixels = debugEnv("PETARI_DUMP") != nullptr || debugEnv("PETARI_FRAMESAVE") != nullptr;
    const bool hostCopy = device().hostImageCopy;
    std::lock_guard<std::mutex> lock(texLock);
    for (const std::shared_ptr<TexImage>& img : f.textures) {
        if (img->gpuTex && img->gpuVersion == img->version) {
            continue;
        }
        if (img->gpuTex) {
            queueTextureDelete(img->gpuTex);  // new pixels: a new image (the old one may be in use)
            img->gpuTex = 0;
        }
        ensureDecoded(*img);  // normally done while the frame was prepared
        size_t texels = 0;
        for (u32 l = 0, w = img->width, h = img->height; l < img->levels; l++, w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1) texels += (size_t)w * h;
        if (img->rgba.size() < texels) {
            continue;  // no pixels (released before): leave it unbound
        }
        u32 h = newImage(img->width, img->height, img->levels, kColorFormat,
                         VK_IMAGE_USAGE_SAMPLED_BIT | (hostCopy ? VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT : VK_IMAGE_USAGE_TRANSFER_DST_BIT), ImgKind::Texture,
                         MemCat::Texture, "texture");
        if (!h) continue;
        Img& im = sImages[h];
        if (hostCopy) {
            VkHostImageLayoutTransitionInfoEXT ti{VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO_EXT};
            ti.image = im.image;
            ti.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ti.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ti.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, img->levels, 0, 1};
            vkTransitionImageLayoutEXT(device().device, 1, &ti);
            VkMemoryToImageCopyEXT regions[16];
            const u32* px = img->rgba.data();
            u32 w = img->width, hh = img->height;
            u32 n = img->levels < 16 ? img->levels : 16;
            for (u32 l = 0; l < n; l++) {
                regions[l] = VkMemoryToImageCopyEXT{VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY_EXT};
                regions[l].pHostPointer = px;
                regions[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
                regions[l].imageExtent = {w, hh, 1};
                px += (size_t)w * hh;
                w = w > 1 ? w / 2 : 1;
                hh = hh > 1 ? hh / 2 : 1;
            }
            VkCopyMemoryToImageInfoEXT ci{VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO_EXT};
            ci.dstImage = im.image;
            ci.dstImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ci.regionCount = n;
            ci.pRegions = regions;
            VkResult r = vkCopyMemoryToImageEXT(device().device, &ci);
            if (r != VK_SUCCESS) {
                port_log("vk: vkCopyMemoryToImageEXT failed (%d)", (int)r);
            }
            // Its state from here on, as recorded commands see it.
            im.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            im.stages = kStageSample;
            im.access = kAccessSample;
            if (!keepPixels) {
                std::vector<uint32_t>().swap(img->rgba);
            }
        } else {
            pendingUploads.push_back(PendingUpload{img, h});
        }
        img->gpuTex = h;
        img->gpuVersion = img->version;
    }
}

// Rendering thread: the uploads host image copy could not do, recorded
// into the command buffer before any draw that samples them.
void VkImpl::drainPendingUploads() {
    static const bool keepPixels = debugEnv("PETARI_DUMP") != nullptr || debugEnv("PETARI_FRAMESAVE") != nullptr;
    std::vector<PendingUpload> list;
    {
        std::lock_guard<std::mutex> lock(texLock);
        list.swap(pendingUploads);
    }
    for (PendingUpload& u : list) {
        TexImage& img = *u.img;
        if (img.gpuTex != u.handle || !sImages.valid(u.handle)) continue;
        Img& im = sImages[u.handle];
        size_t texels = 0;
        for (u32 l = 0, w = img.width, h = img.height; l < img.levels; l++, w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1) texels += (size_t)w * h;
        if (img.rgba.size() < texels) continue;
        ScratchAlloc s = scratch(texels * 4, 16);
        if (!s.ptr) continue;
        memcpy(s.ptr, img.rgba.data(), texels * 4);
        VkBufferImageCopy regions[16];
        VkDeviceSize off = s.offset;
        u32 n = img.levels < 16 ? img.levels : 16;
        for (u32 l = 0, w = img.width, h = img.height; l < n; l++, w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1) {
            regions[l] = VkBufferImageCopy{};
            regions[l].bufferOffset = off;
            regions[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
            regions[l].imageExtent = {w, h, 1};
            off += (VkDeviceSize)w * h * 4;
        }
        bar.use(im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kStageTransfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
        bar.flush();
        vkCmdCopyBufferToImage(commandBuffer(), s.buffer, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, regions);
        bar.use(im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kStageSample, kAccessSample);
        bar.flush();
        if (!keepPixels) {
            std::vector<uint32_t>().swap(img.rgba);
        }
    }
}

// Uploads a built frame's data and textures and resolves its pipelines
// (worker thread, or the rendering thread for setFrame()).
void VkImpl::prepareGpu(VkPrepared& p, bool syncPrograms, bool renderThread) {
    const Frame& f = *p.frame;
    FrameData* fd = acquireFrameData(renderThread);
    if (fd) {
        const void* src[kRegions] = {f.verts.data(), p.indices.data(), p.rows.data(), p.recs.data(), p.states.data(), p.vtxRec.data()};
        VkDeviceSize bytes[kRegions] = {f.verts.size() * 4, p.indices.size() * 4, p.rows.size() * 4,
                                        p.recs.size() * 4,  p.states.size() * 4,  p.vtxRec.size() * 4};
        VkDeviceSize total = 0;
        for (int i = 0; i < kRegions; i++) {
            fd->off[i] = total;
            fd->size[i] = bytes[i] ? bytes[i] : 16;
            total += (fd->size[i] + 255) & ~(VkDeviceSize)255;
        }
        if (total > fd->buf.size) {
            // Free: the GPU is done with it.
            destroyBuffer(fd->buf);
            createBuffer(total + total / 4 + 65536,
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemUse::Upload,
                         &fd->buf, MemCat::FrameData, "frame data");
        }
        if (fd->buf.ptr) {
            for (int i = 0; i < kRegions; i++) {
                if (bytes[i]) memcpy(fd->buf.ptr + fd->off[i], src[i], bytes[i]);
            }
            VkDescriptorBufferInfo bi[5] = {{fd->buf.buffer, fd->off[kRows], fd->size[kRows]},
                                            {fd->buf.buffer, fd->off[kRecs], fd->size[kRecs]},
                                            {fd->buf.buffer, fd->off[kStates], fd->size[kStates]},
                                            {fd->buf.buffer, fd->off[kVtxRec], fd->size[kVtxRec]},
                                            {eyeRing.buffer, 0, sizeof(EyeBlock)}};
            VkWriteDescriptorSet w[5];
            for (u32 i = 0; i < 5; i++) {
                w[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w[i].dstSet = fd->set;
                w[i].dstBinding = i;
                w[i].descriptorCount = 1;
                w[i].descriptorType = i == kVkBindEye ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w[i].pBufferInfo = &bi[i];
            }
            vkUpdateDescriptorSets(device().device, 5, w, 0, nullptr);
        }
    }
    p.data = fd;
    uploadTextures(f);
    // Draw-time requests first (GLES: ahead of background rebuilds).
    std::vector<u32> progs(p.uids.size());
    for (size_t i = 0; i < p.uids.size(); i++) {
        progs[i] = pipes.request(p.uids[i], syncPrograms, 10);
    }
    for (Item& it : p.items) {
        if (it.type == IT_DRAW) {
            it.program = it.uid < progs.size() ? progs[it.uid] : 0;
        }
    }
    p.cutProgs.assign(p.uids.size(), 0);
}

// Worker thread: builds the newest requested frame and uploads it.
void VkImpl::workerMain() {
    for (;;) {
        std::shared_ptr<const Frame> frame;
        std::unique_ptr<VkPrepared> out;
        {
            std::unique_lock<std::mutex> lock(workLock);
            workCv.wait(lock, [&] { return wanted != nullptr; });
            frame = std::move(wanted);
            wanted.reset();
            out = spare ? std::move(spare) : std::unique_ptr<VkPrepared>(new VkPrepared());
        }
        int64_t t0 = port_host_time_ns();
        workerBuilder.build(std::move(frame), *out);
        prepareGpu(*out, false, false);
        port_perf_prepare(port_host_time_ns() - t0);
        static const bool perfLog = debugEnv("PETARI_PERFLOG") != nullptr;
        if (perfLog && out->number % 600 == 0) {
            size_t batches = 0, copiesN = 0;
            for (const Item& it : out->items) {
                batches += it.type == IT_DRAW;
                copiesN += it.type == IT_COPY;
            }
            port_log("perf: prepared frame %llu: %zu batches, %zu EFB copies, %zu shader keys, %zu indices", (unsigned long long)out->number, batches,
                     copiesN, out->uids.size(), out->indices.size());
        }
        std::lock_guard<std::mutex> lock(workLock);
        if (built) {
            // Superseded before the rendering thread took it: its data was
            // never drawn from.
            releaseFrameData(*built, 0);
            spare = std::move(built);
        }
        built = std::move(out);
    }
}

void VkImpl::requestLatest() {
    std::lock_guard<std::mutex> lock(workLock);
    auto latest = frameQueue().latest();
    if (latest && latest->number != lastRequested) {
        lastRequested = latest->number;
        wanted = std::move(latest);
        workCv.notify_one();
    }
}

void VkImpl::flushDeletes() {
    drainTextureDeletes([](const TexHandle* list, size_t count, void*) {
        for (size_t i = 0; i < count; i++) destroyImageLater(list[i]);
    }, nullptr);
}

u32 VkImpl::allocEye(const EyeBlock& eb) {
    u32 i = eyeNext;
    eyeNext = (eyeNext + 1) % kEyeEntries;
    uint64_t used = eyeUsed[i];
    if (used && completedValue() < used) {
        if (commandBufferOpen() && used >= pendingValue()) submit();
        waitValue(used);
    }
    memcpy(eyeRing.ptr + (size_t)i * kEyeStride, &eb, sizeof(eb));
    commandBuffer();
    eyeUsed[i] = pendingValue();
    return i * kEyeStride;
}

// ---------------------------------------------------------------------------
// Execute
// ---------------------------------------------------------------------------
// Opens a rendering instance on `t` (clearing it if this render has not yet)
// unless one is open there.  Item `index` is the first it draws.
void VkImpl::ensurePass(Target& t, size_t index) {
    if (passTarget == &t) return;
    endPass();
    VkCommandBuffer cmd = commandBuffer();
    bool clear = needsClear(t);
    Img& c = sImages[t.color];
    Img& d = sImages[t.depth];
    bar.use(c, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kStageColor, kAccessColor, clear);
    bar.use(d, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, kStageDepth, kAccessDepth, clear);
    bar.flush();
    const VkClearValue* cv = &t == hudTarget ? hudClear : efbClear;
    VkRenderingAttachmentInfoKHR ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    ca.imageView = c.attView;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue = cv[0];
    // Nothing reads the depth after the last rendering instance of a render
    // (the next one starts by clearing it; SpaceWarp's snapshot is taken on
    // the way): the tiler need not write it back (GLES: glInvalidateFramebuffer).
    bool finalPass = (int)index > lastBreak && !sKeepDepth;
    VkRenderingAttachmentInfoKHR da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    da.imageView = d.attView;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    da.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    da.storeOp = finalPass ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue = cv[1];
    VkRenderingInfoKHR ri{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
    ri.renderArea = {{0, 0}, {(u32)t.width, (u32)t.height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    ri.pStencilAttachment = &da;
    // A foveated eye render (setFoveation): the density map of this render.
    VkRenderingFragmentDensityMapAttachmentInfoEXT fdm{VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT};
    if (&t == efbTarget && !sFdm.empty()) {
        auto it = sFdm.find(t.color);
        if (it != sFdm.end() && it->second.current >= 0) {
            FdmMap& m = it->second.ring[it->second.current];
            fdm.imageView = m.view;
            fdm.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            m.usedBy = pendingValue();
            ri.pNext = &fdm;
        }
    }
    vkCmdBeginRenderingKHR(cmd, &ri);
    passTarget = &t;
    curPasses++;
    if (&t == efbTarget) efbNeedsClear = false;
    if (&t == hudTarget) hudNeedsClear = false;
}

void VkImpl::endPass() {
    if (!passTarget) return;
    vkCmdEndRenderingKHR(commandBuffer());
    passTarget = nullptr;
}

// Dynamic state no batch changes; must be set in every command buffer.
void VkImpl::setFixedState(VkCommandBuffer cmd) {
    if (ds.fixedSet == 1) return;
    // GL's front faces are counter-clockwise with y up; Vulkan's y points
    // down in the same memory layout.
    vkCmdSetFrontFaceEXT(cmd, VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetStencilOpEXT(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetLineWidth(cmd, 1.0f);
    float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    vkCmdSetBlendConstants(cmd, zero);
    ds.blendAlpha = 0.0f;
    ds.fixedSet = 1;
}

void VkImpl::snapshot(Target& efb, Target& snap, size_t index) {
    if (needsClear(efb)) ensurePass(efb, index);
    endPass();
    if (!snap.depth || !sImages.valid(snap.depth)) return;
    VkCommandBuffer cmd = commandBuffer();
    Img& s = sImages[efb.depth];
    Img& d = sImages[snap.depth];
    bar.use(s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kStageTransfer, VK_ACCESS_2_TRANSFER_READ_BIT);
    bar.use(d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kStageTransfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
    bar.flush();
    VkImageAspectFlags aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (device().depthStencilBlit) {
        // The scene's depth and the player tags, at the snapshot's size
        // (nearest sample).
        VkImageBlit b{};
        b.srcSubresource = {aspects, 0, 0, 1};
        b.srcOffsets[1] = {efb.width, efb.height, 1};
        b.dstSubresource = {aspects, 0, 0, 1};
        b.dstOffsets[1] = {snap.width, snap.height, 1};
        vkCmdBlitImage(cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    } else if (snap.width == efb.width && snap.height == efb.height) {
        VkImageCopy c{};
        c.srcSubresource = {aspects, 0, 0, 1};
        c.dstSubresource = {aspects, 0, 0, 1};
        c.extent = {(u32)efb.width, (u32)efb.height, 1};
        vkCmdCopyImage(cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    } else {
        static bool logged;
        if (!logged) {
            logged = true;
            port_log("vk: the GPU cannot blit D24S8 and the depth snapshot is %dx%d, not %dx%d: not taken", snap.width, snap.height, efb.width,
                     efb.height);
        }
    }
    bar.use(d, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kStageSample, kAccessSample);
    bar.flush();
}

// The draws of a clear with a partial color mask: vkCmdClearAttachments
// ignores the write mask that GL's glClear honours.
void VkImpl::maskedClear(Target& t, const CopyOp& op, int x0, int y0, int x1, int y1) {
    VkCommandBuffer cmd = commandBuffer();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes.clearPipeline);
    setFixedState(cmd);
    VkViewport vp{0.0f, 0.0f, (float)t.width, (float)t.height, 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{{x0, y0}, {(u32)(x1 - x0), (u32)(y1 - y0)}};
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdSetPrimitiveTopologyEXT(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
    vkCmdSetCullModeEXT(cmd, VK_CULL_MODE_NONE);
    VkBool32 off = VK_FALSE;
    vkCmdSetColorBlendEnableEXT(cmd, 0, 1, &off);
    VkColorBlendEquationEXT eq{VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};
    vkCmdSetColorBlendEquationEXT(cmd, 0, 1, &eq);
    VkColorComponentFlags mask = (op.colorUpd ? 7u : 0u) | (op.alphaUpd ? 8u : 0u);
    vkCmdSetColorWriteMaskEXT(cmd, 0, 1, &mask);
    vkCmdSetDepthTestEnableEXT(cmd, op.zUpd ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthWriteEnableEXT(cmd, op.zUpd ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthCompareOpEXT(cmd, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilTestEnableEXT(cmd, VK_FALSE);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    struct {
        u32 firstOrdinal;
        int mode;
        u32 depthBits;
        u32 pad;
        float color[4];
    } pc = {0, 0, 0, 0, {op.clearColor[0], op.clearColor[1], op.clearColor[2], op.clearColor[3]}};
    memcpy(&pc.depthBits, &op.clearDepth, 4);
    vkCmdPushConstants(cmd, pipes.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 4, 1, 0, 0);
    ds.forget();
}

void VkImpl::dumpCopy(const CopyOp& op, u32 handle, int dw, int dh) {
    // PETARI_COPYDUMP=<dir>: the copies of each frame's first flat replay
    // as <dir>/copy_<frame>_<id>_fmt<f>_<w>x<h>.png (rows bottom-up, so the
    // image appears upside down), written once the GPU has made them.
    Img& im = sImages[handle];
    ScratchAlloc s = scratch((VkDeviceSize)dw * dh * 4, 16);
    if (!s.ptr) return;
    bar.use(im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kStageTransfer, VK_ACCESS_2_TRANSFER_READ_BIT);
    bar.flush();
    VkBufferImageCopy region{};
    region.bufferOffset = s.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(u32)dw, (u32)dh, 1};
    vkCmdCopyImageToBuffer(commandBuffer(), im.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buffer, 1, &region);
    bar.use(im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kStageSample, kAccessSample);
    bar.flush();
    char path[512];
    snprintf(path, sizeof(path), "%s/copy_%llu_%x_fmt%x_%dx%d.png", debugEnv("PETARI_COPYDUMP"), (unsigned long long)cur->number, op.id, op.fmt, dw, dh);
    std::string p = path;
    uint8_t* src = (uint8_t*)s.ptr;
    deferUntilDone([p, src, dw, dh] {
        std::vector<u8> px(src, src + (size_t)dw * dh * 4);
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        port_headless_write_png(p.c_str(), px.data(), dw, dh);
    });
}

void VkImpl::doCopy(const Item& it, Target& src, VulkanRenderer& r, bool vr, size_t index) {
    CopyOp op;
    decodeCopy(it, src.width, src.height, cur->nativeW, cur->nativeH, copySlot, op);
    // Tests of paths the recorded scenes rarely take: PETARI_VKFORCECLEAR=1
    // clears after every copy (color, alpha and depth), PETARI_VKFORCEZCOPY=1
    // makes every texture copy a Z copy.
    static const bool sForceClear = debugEnv("PETARI_VKFORCECLEAR") != nullptr;
    static const bool sForceZ = debugEnv("PETARI_VKFORCEZCOPY") != nullptr;
    if (sForceClear) {
        op.clear = true;
        op.colorUpd = op.alphaUpd = op.zUpd = true;
    }
    if (sForceZ && !op.toXfb) {
        op.mode = 11;
    }
    int sx0 = op.sx0, sy0 = op.sy0, sx1 = op.sx1, sy1 = op.sy1;
    bool copies = !op.toXfb || !vr;
    if (copies) {
        // The copy reads what this render cleared.
        if (needsClear(src)) ensurePass(src, index);
        endPass();
        VkCommandBuffer cmd = commandBuffer();
        Img& color = sImages[src.color];
        if (op.toXfb) {
            int dw = op.dw, dh = op.dh;
            CopyTex& xfb = copyTex(kXfbCopyKey, dw, dh);
            if (xfb.handle) {
                Img& dst = sImages[xfb.handle];
                bar.use(color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kStageTransfer, VK_ACCESS_2_TRANSFER_READ_BIT);
                bar.use(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kStageTransfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
                bar.flush();
                VkImageBlit b{};
                b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                b.srcOffsets[0] = {sx0, sy0, 0};
                b.srcOffsets[1] = {sx1, sy1, 1};
                b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                b.dstOffsets[1] = {dw, dh, 1};
                vkCmdBlitImage(cmd, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
                bar.use(dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kStageSample, kAccessSample);
                bar.flush();
                r.mXfbTex = xfb.handle;
                cov.xfbBlits++;
                r.mXfbWidth = dw;
                r.mXfbHeight = dh;
            }
        } else {
            int dw = op.dw, dh = op.dh;
            CopyTex& dstTex = copyTex(op.key, dw, dh);
            if (dstTex.handle) {
                Img& dst = sImages[dstTex.handle];
                bool z = op.mode == 11;
                cov.zCopies += z;
                bar.use(color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kStageSample, kAccessSample);
                if (z) bar.use(sImages[src.depth], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kStageSample, kAccessSample);
                // The copy covers it.
                bar.use(dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kStageColor, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, true);
                bar.flush();
                VkRenderingAttachmentInfoKHR ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
                ca.imageView = dst.attView;
                ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingInfoKHR ri{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
                ri.renderArea = {{0, 0}, {(u32)dw, (u32)dh}};
                ri.layerCount = 1;
                ri.colorAttachmentCount = 1;
                ri.pColorAttachments = &ca;
                vkCmdBeginRenderingKHR(cmd, &ri);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes.copyPipeline);
                setFixedState(cmd);
                VkViewport vp{0.0f, 0.0f, (float)dw, (float)dh, 0.0f, 1.0f};
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{{0, 0}, {(u32)dw, (u32)dh}};
                vkCmdSetScissor(cmd, 0, 1, &sc);
                vkCmdSetPrimitiveTopologyEXT(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
                vkCmdSetCullModeEXT(cmd, VK_CULL_MODE_NONE);
                VkBool32 off = VK_FALSE;
                vkCmdSetColorBlendEnableEXT(cmd, 0, 1, &off);
                VkColorBlendEquationEXT eq{VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};
                vkCmdSetColorBlendEquationEXT(cmd, 0, 1, &eq);
                VkColorComponentFlags mask = 0xF;
                vkCmdSetColorWriteMaskEXT(cmd, 0, 1, &mask);
                vkCmdSetDepthTestEnableEXT(cmd, VK_FALSE);
                vkCmdSetDepthWriteEnableEXT(cmd, VK_FALSE);
                vkCmdSetDepthCompareOpEXT(cmd, VK_COMPARE_OP_ALWAYS);
                vkCmdSetStencilTestEnableEXT(cmd, VK_FALSE);
                vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
                vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
                struct {
                    u32 firstOrdinal;
                    int mode;
                    u32 pad[2];
                    float rect[4];
                } pc = {0, op.mode, {0, 0}, {sx0 / (float)src.width, sy0 / (float)src.height, sx1 / (float)src.width, sy1 / (float)src.height}};
                vkCmdPushConstants(cmd, pipes.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
                VkDescriptorImageInfo ii[kVkNumSamplers];
                VkWriteDescriptorSet w[kVkNumSamplers];
                Img& dm = sImages[dummy];
                for (u32 u = 0; u < kVkNumSamplers; u++) {
                    ii[u] = {copyColorSampler, dm.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                }
                ii[0] = {copyColorSampler, color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                if (z) ii[1] = {copyDepthSampler, sImages[src.depth].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                for (u32 u = 0; u < kVkNumSamplers; u++) {
                    w[u] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    w[u].dstBinding = u;
                    w[u].descriptorCount = 1;
                    w[u].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    w[u].pImageInfo = &ii[u];
                }
                vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes.layout, kVkSetTextures, kVkNumSamplers, w);
                vkCmdDraw(cmd, 4, 1, 0, 0);
                vkCmdEndRenderingKHR(cmd);
                bar.use(dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kStageSample, kAccessSample);
                bar.flush();
                ds.forget();
                curCopies++;
                if (dumpingCopies) dumpCopy(op, dstTex.handle, dw, dh);
            }
        }
    }

    if (op.clear) {
        int x0 = sx0 < 0 ? 0 : sx0, y0 = sy0 < 0 ? 0 : sy0;
        int x1 = sx1 > src.width ? src.width : sx1, y1 = sy1 > src.height ? src.height : sy1;
        bool color = op.colorUpd || op.alphaUpd;
        if ((color || op.zUpd) && x1 > x0 && y1 > y0) {
            ensurePass(src, index + 1);
            VkCommandBuffer cmd = commandBuffer();
            // PETARI_VKMASKEDCLEAR=1: every clear as the draw a partial color
            // mask needs (tests that path against vkCmdClearAttachments).
            static const bool sForceDraw = debugEnv("PETARI_VKMASKEDCLEAR") != nullptr;
            if ((color && op.colorUpd != op.alphaUpd) || sForceDraw) {
                maskedClear(src, op, x0, y0, x1, y1);
                cov.maskedClears++;
            } else {
                VkClearAttachment ca[2];
                u32 n = 0;
                if (color) {
                    ca[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    ca[n].colorAttachment = 0;
                    ca[n].clearValue.color = {{op.clearColor[0], op.clearColor[1], op.clearColor[2], op.clearColor[3]}};
                    n++;
                }
                if (op.zUpd) {
                    ca[n].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                    ca[n].colorAttachment = 0;
                    ca[n].clearValue.depthStencil = {op.clearDepth, 0};
                    n++;
                }
                VkClearRect rect{{{x0, y0}, {(u32)(x1 - x0), (u32)(y1 - y0)}}, 0, 1};
                vkCmdClearAttachments(cmd, n, ca, 1, &rect);
                cov.attachmentClears++;
            }
        }
    }
}

void VkImpl::execute(Target& efb, const EyeView* eye, Target* hud, HudMode hudMode, VulkanRenderer& r) {
    int64_t cpuStart = port_host_time_ns();
    VkPrepared& P = *cur;
    const std::vector<Item>& items = P.items;
    const std::vector<std::shared_ptr<TexImage>>& textures = P.frame->textures;
    u32 nativeW = P.nativeW, nativeH = P.nativeH;
    u32 clearAR = P.clearAR, clearGB = P.clearGB, clearZ = P.clearZ;
    if (hudMode == HudMode::Target && !hud) {
        hudMode = HudMode::Inline;
    }
    if (!sImages.valid(efb.color) || !sImages.valid(efb.depth) || !P.data || !P.data->buf.buffer ||
        (hudMode == HudMode::Target && (!sImages.valid(hud->color) || !sImages.valid(hud->depth)))) {
        snapshotTaken = false;
        hudDraws = 0;
        return;
    }
    // Per-eye block.
    EyeBlock eb;
    fillEyeBlock(eye, eb);
    // A VR eye, as opposed to a flat replay or one of its stereo pair.
    bool vrEye = eye && !eye->flatStereo;
    u32 eyeOffset = allocEye(eb);
    // With the cutaway on, 3D draws use their cutaway variants, compiled in
    // the background on first use (a draw keeps its plain pipeline until
    // then).  The variants are only used while needed: their discard costs
    // the GPU its early depth rejection.
    bool cutActive = vrEye && eye->focus[3] > 0.0f;
    std::vector<u32>& cutProgs = P.cutProgs;
    copySlot = copySlotFor(eye);
    // PETARI_COPYDUMP_EYE=1: the left eye's copies instead of the flat replay's.
    static const bool sDumpEye = debugEnv("PETARI_COPYDUMP_EYE") != nullptr;
    dumpingCopies = (sDumpEye ? (eye && eye->index == 0) : !eye) && debugEnv("PETARI_COPYDUMP") && copyDumpFrame != P.number;
    if (dumpingCopies) {
        copyDumpFrame = P.number;
    }
    if (cutActive) {
        const std::vector<ShaderUid>& frameUids = P.uids;
        for (size_t i = 0; i < frameUids.size() && i < cutProgs.size(); i++) {
            if (!cutProgs[i] && !frameUids[i].hud) {
                ShaderUid u = frameUids[i];
                u.cutaway = 1;
                cutProgs[i] = pipes.request(u, !async.load(), 5);
            }
        }
    }

    VkCommandBuffer cmd = commandBuffer();
    if (pendingValue() != lastCmdValue) {
        lastCmdValue = pendingValue();  // a new command buffer
        rendersInCmd = 0;
    }
    rendersInCmd++;
    int serial = (int)(++renderSerial & 0x7FFFFFFF);
    sLastSerial = serial;
    beginTimer(serial);
    ds.forget();
    curPasses = curCopies = 0;
    int skipped = 0;

    // Start of frame: the EFB is cleared (the previous frame's display copy
    // cleared it on the console); the HUD target becomes transparent.
    // Cleared when first drawn to (or copied from), as the tiler wants.
    efbTarget = &efb;
    hudTarget = hudMode == HudMode::Target ? hud : nullptr;
    efbNeedsClear = true;
    hudNeedsClear = hudTarget != nullptr;
    efbClear[0].color = {{(clearAR & 255) / 255.0f, ((clearGB >> 8) & 255) / 255.0f, (clearGB & 255) / 255.0f, ((clearAR >> 8) & 255) / 255.0f}};
    efbClear[1].depthStencil = {clearZ / 16777215.0f, 0};
    hudClear[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    hudClear[1].depthStencil = {clearZ / 16777215.0f, 0};
    // SpaceWarp: the player's draws tag the stencil buffer (see
    // EyeView::depthSnapshot); the stencil test itself always passes.
    bool tagPlayer = eye && eye->depthSnapshot;
    snapshotTaken = false;
    hudDraws = 0;
    static const bool noCopies = debugEnv("PETARI_NOCOPY") != nullptr;  // perf experiments

    // The last item that ends a rendering instance before the end of the
    // render: after it, the depth need not be stored.
    lastBreak = -1;
    {
        bool hudSec = false;
        for (size_t i = 0; i < items.size(); i++) {
            const Item& it = items[i];
            if (it.type == IT_MARKER) {
                if (it.marker == MARK_SCENE_DEPTH && tagPlayer && !(hudSec && hudMode == HudMode::Target)) lastBreak = (int)i;
                if (it.marker == MARK_HUD_BEGIN || it.marker == MARK_HUD_END) {
                    hudSec = it.marker == MARK_HUD_BEGIN;
                    if (hudMode == HudMode::Target) lastBreak = (int)i;
                }
                continue;
            }
            if (hudSec && hudMode == HudMode::Skip) continue;
            if (it.type == IT_COPY) {
                bool toXfb = (it.copy[9] >> 14) & 1;
                if (noCopies && !toXfb) continue;
                if (!toXfb || !eye) lastBreak = (int)i;
            }
        }
    }

    struct UnitMemo {
        u32 tr, mode0, mode1;
        VkImageView view;
        VkSampler samp;
    } memo[kVkNumSamplers];
    for (UnitMemo& m : memo) m.tr = ~0u;
    FrameData& fd = *P.data;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes.layout, kVkSetBuffers, 1, &fd.set, 1, &eyeOffset);
    vkCmdBindIndexBuffer(cmd, fd.buf.buffer, fd.off[kIndices], VK_INDEX_TYPE_UINT32);
    Img& dummyImg = sImages[dummy];

    Target* curT = &efb;
    bool inHudSection = false;
    int stopAfter = gDebugStopAfterDraws;
    for (size_t idx = 0; idx < items.size(); idx++) {
        const Item& it = items[idx];
        if (stopAfter >= 0 && it.type == IT_DRAW) {
            if (stopAfter < (int)it.draws) {
                break;
            }
            stopAfter -= (int)it.draws;
        }
        if (it.type == IT_MARKER) {
            if (it.marker == MARK_SCENE_DEPTH && tagPlayer && curT == &efb) {
                snapshot(efb, *eye->depthSnapshot, idx);
                cov.snapshots++;
                ds.forget();
                snapshotTaken = true;
                continue;
            }
            if (it.marker != MARK_HUD_BEGIN && it.marker != MARK_HUD_END) {
                continue;
            }
            inHudSection = it.marker == MARK_HUD_BEGIN;
            if (hudMode == HudMode::Target) {
                endPass();
                curT = inHudSection ? hud : &efb;
                cov.hudSwitches++;
            }
            continue;
        }
        if (inHudSection && hudMode == HudMode::Skip) {
            continue;
        }
        bool toHudTarget = inHudSection && hudMode == HudMode::Target;
        if (it.type == IT_COPY) {
            if (noCopies && !((it.copy[9] >> 14) & 1)) continue;
            doCopy(it, *curT, r, eye != nullptr, idx);
            ds.forget();
            for (UnitMemo& m : memo) m.tr = ~0u;
            continue;
        }
        if (!it.program || !it.idxCount) {
            continue;
        }
        u32 progId = it.program;
        VkPipeline pipe = VK_NULL_HANDLE;
        if (cutActive && it.useVr && it.uid < cutProgs.size() && cutProgs[it.uid]) {
            pipe = pipes.get(cutProgs[it.uid]);
        }
        if (!pipe) pipe = pipes.get(progId);
        if (!pipe) {
            skipped++;  // still compiling (GLES: program 0)
            continue;
        }
        Target& t = *curT;
        DrawRect rect;
        drawRect(it, t.width, t.height, nativeW, nativeH, eye, vrEye, rect);
        if (toHudTarget) {
            hudDraws++;
        }
        if (rect.vp[2] <= 0 || rect.vp[3] <= 0) {
            cov.emptyViewports++;
            continue;  // an empty viewport draws nothing (Vulkan does not take one)
        }
        cov.lineBatches += it.prim == Prim::Lines;
        cov.pointBatches += it.prim == Prim::Points;
        ensurePass(t, idx);
        setFixedState(cmd);
        if (ds.pipe != pipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            ds.pipe = pipe;
        }
        if (ds.vp[0] != (float)rect.vp[0] || ds.vp[1] != (float)rect.vp[1] || ds.vp[2] != (float)rect.vp[2] || ds.vp[3] != (float)rect.vp[3]) {
            VkViewport vp{(float)rect.vp[0], (float)rect.vp[1], (float)rect.vp[2], (float)rect.vp[3], 0.0f, 1.0f};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            ds.vp[0] = vp.x, ds.vp[1] = vp.y, ds.vp[2] = vp.width, ds.vp[3] = vp.height;
        }
        VkRect2D sc;
        if (rect.scissor) {
            int x0 = rect.sc[0] < 0 ? 0 : rect.sc[0], y0 = rect.sc[1] < 0 ? 0 : rect.sc[1];
            int x1 = rect.sc[0] + rect.sc[2], y1 = rect.sc[1] + rect.sc[3];
            x1 = x1 > t.width ? t.width : x1;
            y1 = y1 > t.height ? t.height : y1;
            sc = {{x0, y0}, {(u32)(x1 > x0 ? x1 - x0 : 0), (u32)(y1 > y0 ? y1 - y0 : 0)}};
        } else {
            sc = {{0, 0}, {(u32)t.width, (u32)t.height}};
        }
        if (memcmp(&ds.sc, &sc, sizeof(sc)) != 0) {
            vkCmdSetScissor(cmd, 0, 1, &sc);
            ds.sc = sc;
        }

        BlendState bs;
        blendState(it, toHudTarget, bs);
        if (ds.colorMask != (u32)bs.colorMask) {
            VkColorComponentFlags m = (VkColorComponentFlags)bs.colorMask;  // bit 0 R .. bit 3 A, as Vulkan's
            vkCmdSetColorWriteMaskEXT(cmd, 0, 1, &m);
            ds.colorMask = (u32)bs.colorMask;
        }
        if (ds.blendOn != (u32)bs.on) {
            VkBool32 on = bs.on ? VK_TRUE : VK_FALSE;
            vkCmdSetColorBlendEnableEXT(cmd, 0, 1, &on);
            ds.blendOn = (u32)bs.on;
        }
        if (bs.on) {
            VkColorBlendEquationEXT eq{toVkBlendFactor(bs.srcRGB), toVkBlendFactor(bs.dstRGB), toVkBlendOp(bs.eqRGB),
                                       toVkBlendFactor(bs.srcA),   toVkBlendFactor(bs.dstA),   toVkBlendOp(bs.eqA)};
            if (memcmp(&ds.eq, &eq, sizeof(eq)) != 0) {
                vkCmdSetColorBlendEquationEXT(cmd, 0, 1, &eq);
                ds.eq = eq;
            }
            bool usesConst = bs.srcA == BlendFactor::ConstantAlpha || bs.dstA == BlendFactor::ConstantAlpha;
            if (usesConst && !(ds.blendAlpha == bs.constAlpha)) {
                float c[4] = {0.0f, 0.0f, 0.0f, bs.constAlpha};
                vkCmdSetBlendConstants(cmd, c);
                ds.blendAlpha = bs.constAlpha;
            }
        } else if (ds.eq.srcColorBlendFactor == (VkBlendFactor)0xFFFFFFFF) {
            // The equation must be set once even while blending is off.
            VkColorBlendEquationEXT eq{VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};
            vkCmdSetColorBlendEquationEXT(cmd, 0, 1, &eq);
            ds.eq = eq;
        }
        u32 depthOn = it.depthTest ? 1 : 0, depthWrite = it.depthTest && it.depthWrite ? 1 : 0;
        if (ds.depthTest != depthOn) {
            vkCmdSetDepthTestEnableEXT(cmd, depthOn);
            ds.depthTest = depthOn;
        }
        VkCompareOp zop = toVkCompare(it.depthFunc);
        if ((depthOn || ds.depthOp == (VkCompareOp)0xFFFFFFFF) && ds.depthOp != zop) {
            vkCmdSetDepthCompareOpEXT(cmd, zop);
            ds.depthOp = zop;
        }
        if (ds.depthWrite != depthWrite) {
            vkCmdSetDepthWriteEnableEXT(cmd, depthWrite);
            ds.depthWrite = depthWrite;
        }
        VkCullModeFlags cull = it.cull ? toVkCull(it.cullFace) : VK_CULL_MODE_NONE;
        if (ds.cull != cull) {
            vkCmdSetCullModeEXT(cmd, cull);
            ds.cull = cull;
        }
        // Where a draw writes depth, the stencil gets `ref` (1 for the
        // player's draws); elsewhere it keeps what is there.
        u32 stencilOn = tagPlayer ? 1 : 0;
        if (ds.stencilTest != stencilOn) {
            vkCmdSetStencilTestEnableEXT(cmd, stencilOn);
            ds.stencilTest = stencilOn;
        }
        bool stencilWrite = tagPlayer && !toHudTarget && it.depthTest && it.depthWrite;
        u32 wmask = stencilWrite ? 0xFF : 0;
        if (ds.stencilWrite != wmask) {
            vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, wmask);
            ds.stencilWrite = wmask;
        }
        u32 ref = stencilWrite && it.player ? 1 : 0;
        if (ds.stencilRef != ref && (stencilWrite || ds.stencilRef == 0xFFFFFFFF)) {
            vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, ref);
            ds.stencilRef = ref;
        }
        VkPrimitiveTopology topo = toVkPrim(it.prim);
        if (ds.topo != topo) {
            vkCmdSetPrimitiveTopologyEXT(cmd, topo);
            ds.topo = topo;
        }

        // Textures: units the program samples, the rest a black dummy.
        // Consecutive batches mostly bind the same: each unit remembers what
        // its last lookup found (cleared by copies, which can replace a copy
        // texture).
        VkImageView views[kVkNumSamplers];
        VkSampler samps[kVkNumSamplers];
        bool texChanged = false;
        for (u32 u = 0; u < kVkNumSamplers; u++) {
            VkImageView v = dummyImg.view;
            VkSampler s = copyColorSampler;
            u32 tr = it.tex[u];
            if ((it.texMask & (1u << u)) && tr != ~0u) {
                UnitMemo& m = memo[u];
                if (m.tr == tr && m.mode0 == it.texMode0[u] && m.mode1 == it.texMode1[u]) {
                    v = m.view;
                    s = m.samp;
                } else {
                    if (tr & 0x80000000u) {
                        auto cit = copies.find(copyKey(copySlot, (tr & 0x7FFFFFFF) << 5));
                        if (cit != copies.end() && cit->second.handle) v = sImages[cit->second.handle].view;
                        s = sampler(it.texMode0[u], it.texMode1[u], 1);
                    } else if (tr < textures.size()) {
                        TexHandle th = textures[tr]->gpuTex;
                        if (th && sImages.valid(th)) {
                            v = sImages[th].view;
                        }
                        s = sampler(it.texMode0[u], it.texMode1[u], textures[tr]->levels);
                    }
                    m = UnitMemo{tr, it.texMode0[u], it.texMode1[u], v, s};
                }
            }
            views[u] = v;
            samps[u] = s;
            texChanged |= ds.views[u] != v || ds.samps[u] != s;
        }
        if (texChanged) {
            VkDescriptorImageInfo ii[kVkNumSamplers];
            VkWriteDescriptorSet w[kVkNumSamplers];
            for (u32 u = 0; u < kVkNumSamplers; u++) {
                ii[u] = {samps[u], views[u], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                w[u] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w[u].dstBinding = u;
                w[u].descriptorCount = 1;
                w[u].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[u].pImageInfo = &ii[u];
                ds.views[u] = views[u];
                ds.samps[u] = samps[u];
            }
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes.layout, kVkSetTextures, kVkNumSamplers, w);
        }

        // The batch: its vertices from its first word, indices from 0.
        VkDeviceSize vbOff = fd.off[kVerts] + (VkDeviceSize)it.firstWord * 4;
        if (ds.vbOff != vbOff) {
            vkCmdBindVertexBuffers(cmd, 0, 1, &fd.buf.buffer, &vbOff);
            ds.vbOff = vbOff;
        }
        if (ds.firstOrdinal != it.firstOrdinal) {
            vkCmdPushConstants(cmd, pipes.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, kVkPushFirstOrdinal, 4, &it.firstOrdinal);
            ds.firstOrdinal = it.firstOrdinal;
        }
        vkCmdDrawIndexed(cmd, it.idxCount, 1, it.idxOffset, 0, 0);
    }
    endPass();
    // Targets nothing was drawn to still get their clear.
    if (efbNeedsClear) {
        ensurePass(efb, items.size());
        endPass();
    }
    if (hudTarget && hudNeedsClear) {
        ensurePass(*hud, items.size());
        endPass();
    }
    // Ready to be sampled (the presentation) or read back.
    bar.use(sImages[efb.color], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kStageSample, kAccessSample);
    if (hudTarget) bar.use(sImages[hud->color], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kStageSample, kAccessSample);
    bar.flush();
    endTimer(serial);
    efbTarget = hudTarget = nullptr;

    int breaks = curPasses > 1 ? curPasses - 1 : 0;
    double cpuMs = (port_host_time_ns() - cpuStart) / 1e6;
    st.cpuMs += cpuMs;
    st.cpuMaxMs = cpuMs > st.cpuMaxMs ? cpuMs : st.cpuMaxMs;
    st.renders++;
    st.passes += curPasses;
    st.breaks += breaks;
    st.maxBreaks = breaks > st.maxBreaks ? breaks : st.maxBreaks;
    st.copies += curCopies;
    st.skipped += skipped;
    static const bool perRender = debugEnv("PETARI_VKSTATS") != nullptr;
    if (perRender) {
        port_log("vk: render %d of frame %llu (%s): %d rendering instances (%d breaks), %d copy passes, %d batches skipped, CPU %.2f ms", serial,
                 (unsigned long long)P.number, !eye ? "flat" : eye->flatStereo ? "stereo" : eye->index ? "right eye" : "left eye", curPasses, breaks,
                 curCopies, skipped, cpuMs);
    }
    if (rendersInCmd >= kAutoSubmitRenders) {
        submit();  // a presentation that never submits must not grow one command buffer forever
    }
    logStats(false);
}

void VkImpl::logStats(bool force) {
    int64_t now = port_host_time_ns();
    if (!force && now - lastLogNs < 10000000000ll) return;
    double secs = (now - lastLogNs) / 1e9;
    lastLogNs = now;
    if (st.renders) {
        port_log("vk: %d renders in %.0f s: GPU %.2f ms avg %.2f max a render (%d timed), CPU %.2f ms avg %.2f max; rendering instances %.1f a render "
                 "(breaks %.1f avg, %d max), %.1f copy passes; %d batches waited for pipelines; %zu images",
                 st.renders, secs, st.gpuCount ? st.gpuMs / st.gpuCount : 0.0, st.gpuMaxMs, st.gpuCount, st.cpuMs / st.renders, st.cpuMaxMs,
                 st.passes / (double)st.renders, st.breaks / (double)st.renders, st.maxBreaks, st.copies / (double)st.renders, st.skipped, sImages.live());
    }
    st = Stats();
    if (force || now - lastMemLogNs >= 30000000000ll) {
        lastMemLogNs = now;
        logMemory("periodic");
        pipes.logStats("periodic");
        if (device().validation) {
            port_log("vk: validation so far: %d errors, %d warnings", validationErrors(), validationWarnings());
        }
    }
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------
namespace {

bool VulkanRenderer::init() {
    if (!sImpl) {
        // The device the XR presentation made through the runtime
        // (vk::adoptDevice), else one of our own (headless).
        if (!deviceReady() && !createStandalone()) {
            port_log("vk: no Vulkan device; nothing will be drawn");
            return false;
        }
        checkReservedWindow("device ready");
        sImpl = new VkImpl();
        if (!sImpl->init()) {
            port_log("vk: the renderer could not be set up");
        }
    }
    return sImpl->ready;
}

// Foveation of the target's renders around (focalX, focalY) (normalized
// device coordinates).  Under Vulkan `gain` is the density at the fringes
// (0..1 of full per axis; <= 0 turns foveation off) and `foveaArea` the
// radius of the full-density region (NDC, on the image's own aspect).
bool VulkanRenderer::setFoveation(Target& t, float focalX, float focalY, float gain, float foveaArea) {
    if (!deviceReady() || !device().fragmentDensityMap || !sImages.valid(t.color)) return false;
    if (!sFdmChecked) {
        sFdmChecked = true;
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(device().phys, kFdmFormat, &fp);
        sFdmOk = (fp.linearTilingFeatures & VK_FORMAT_FEATURE_FRAGMENT_DENSITY_MAP_BIT_EXT) != 0;
        port_log("vk: foveation %s (density map texels %ux%u to %ux%u px)", sFdmOk ? "available" : "unavailable (no linear density map format)",
                 device().fdmTexelMin.width, device().fdmTexelMin.height, device().fdmTexelMax.width, device().fdmTexelMax.height);
    }
    if (!sFdmOk) return false;
    auto it = sFdm.find(t.color);
    if (gain <= 0.0f) {
        if (it != sFdm.end()) it->second.current = -1;
        return true;
    }
    Fdm& f = sFdm[t.color];
    u32 tw = (u32)t.width, th = (u32)t.height;
    if (f.tw != tw || f.th != th) {
        // A new target (or one of a new size): a new ring, the old one once
        // the GPU is done with it.
        if (f.ring[0].image) {
            Fdm old = f;
            deferUntilDone([old]() mutable { destroyFdm(old); });
        }
        f = Fdm();
        f.tw = tw;
        f.th = th;
        u32 texW = device().fdmTexelMin.width ? device().fdmTexelMin.width : 32, texH = device().fdmTexelMin.height ? device().fdmTexelMin.height : 32;
        f.w = (tw + texW - 1) / texW;
        f.h = (th + texH - 1) / texH;
        for (FdmMap& m : f.ring) {
            if (!createFdmMap(m, f.w, f.h)) {
                port_log("vk: cannot create a density map");
                destroyFdm(f);
                sFdm.erase(t.color);
                return false;
            }
        }
    }
    FdmMap& m = f.ring[f.next];
    if (m.usedBy && completedValue() < m.usedBy) waitValue(m.usedBy);
    fillFdm(f, m, focalX, focalY, foveaArea, fminf(1.0f, gain));
    f.current = f.next;
    f.next = (f.next + 1) % kFdmRing;
    if (!f.initialized) {
        // PREINITIALIZED -> GENERAL keeps what the CPU wrote; the maps stay
        // in GENERAL (host writes before a submission are visible to it).
        f.initialized = true;
        VkImageMemoryBarrier2 b[kFdmRing];
        for (int i = 0; i < kFdmRing; i++) {
            b[i] = VkImageMemoryBarrier2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b[i].srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            b[i].srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
            b[i].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_DENSITY_PROCESS_BIT_EXT;
            b[i].dstAccessMask = VK_ACCESS_2_FRAGMENT_DENSITY_MAP_READ_BIT_EXT;
            b[i].oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
            b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b[i].image = f.ring[i].image;
            b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = kFdmRing;
        di.pImageMemoryBarriers = b;
        vkCmdPipelineBarrier2KHR(commandBuffer(), &di);
    }
    return true;
}

Target VulkanRenderer::createTarget(int width, int height) {
    Target t;
    if (!deviceReady()) {
        port_log("vk: createTarget before the device exists (init first)");
        return t;
    }
    t.width = width;
    t.height = height;
    t.color = newImage((u32)width, (u32)height, 1, kColorFormat,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       ImgKind::Color, MemCat::Target, "target color");
    // Depth with stencil: the stencil tags the player's pixels for SpaceWarp.
    t.depth = newImage((u32)width, (u32)height, 1, kDepthFormat,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       ImgKind::Depth, MemCat::Target, "target depth");
    t.fbo = t.color;
    if (!t.color || !t.depth) {
        port_log("vk: EFB target %dx%d not created", width, height);
    }
    return t;
}

Target VulkanRenderer::createDepthTarget(int width, int height) {
    Target t;
    if (!deviceReady()) {
        port_log("vk: createDepthTarget before the device exists (init first)");
        return t;
    }
    t.width = width;
    t.height = height;
    t.depth = newImage((u32)width, (u32)height, 1, kDepthFormat,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       ImgKind::Depth, MemCat::Target, "depth target");
    t.fbo = t.depth;
    if (!t.depth) {
        port_log("vk: depth target %dx%d not created", width, height);
    }
    return t;
}

void VulkanRenderer::destroyTarget(Target& t) {
    auto fdm = sFdm.find(t.color);
    if (fdm != sFdm.end()) {
        Fdm old = fdm->second;
        sFdm.erase(fdm);
        deferUntilDone([old]() mutable { destroyFdm(old); });
    }
    if (deviceReady()) {
        destroyImageLater(t.color);
        destroyImageLater(t.depth);
    }
    t = Target();
}

void VulkanRenderer::setFrame(std::shared_ptr<const Frame> frame) {
    if (!sImpl || !sImpl->ready) return;
    VkImpl& im = *sImpl;
    im.flushDeletes();
    if (!frame) {
        return;
    }
    if (im.cur && im.cur->number == frame->number) {
        return;
    }
    std::unique_ptr<VkPrepared> p;
    {
        std::lock_guard<std::mutex> lock(im.workLock);  // the worker recycles spare too
        p = im.spare ? std::move(im.spare) : std::unique_ptr<VkPrepared>(new VkPrepared());
    }
    im.syncBuilder.build(std::move(frame), *p);
    im.prepareGpu(*p, !im.async.load(), true);
    im.drainPendingUploads();
    std::unique_ptr<VkPrepared> old = std::move(im.cur);
    im.cur = std::move(p);
    if (old) {
        im.releaseFrameData(*old, pendingValue());
        std::lock_guard<std::mutex> lock(im.workLock);
        im.spare = std::move(old);
    }
    im.evictCopies();
    im.pipes.poll();
}

// Game thread, as each frame is presented: preparation starts right away,
// so the frame is ready by the renderer's next refresh.
void onFramePublished() {
    if (sImpl) {
        sImpl->requestLatest();
    }
}

bool VulkanRenderer::update() {
    if (!sImpl || !sImpl->ready) return false;
    VkImpl& im = *sImpl;
    im.flushDeletes();
    collectFinished();
    if (!im.worker.joinable()) {
        im.async.store(true);
        PortHostAllocScope hostAlloc;
        im.worker = std::thread([&im] {
            PortHostAllocScope scope;
            im.workerMain();
        });
        frameQueue().setListener(onFramePublished);
    }
    im.requestLatest();
    im.pipes.poll();
    std::unique_ptr<VkPrepared> done;
    {
        std::lock_guard<std::mutex> lock(im.workLock);
        done = std::move(im.built);
    }
    if (!done) {
        return false;
    }
    im.drainPendingUploads();
    std::unique_ptr<VkPrepared> old = std::move(im.cur);
    im.cur = std::move(done);
    im.evictCopies();
    if (old) {
        im.releaseFrameData(*old, pendingValue());
        old->frame.reset();  // let the recording's memory go
        std::lock_guard<std::mutex> lock(im.workLock);
        if (!im.spare) im.spare = std::move(old);
    }
    return true;
}

bool VulkanRenderer::hasFrame() const { return sImpl && sImpl->cur; }

uint64_t VulkanRenderer::frameNumber() const { return hasFrame() ? sImpl->cur->number : 0; }

const CameraInfo& VulkanRenderer::camera() const {
    static const CameraInfo none{};
    return hasFrame() ? sImpl->cur->cam : none;
}

void VulkanRenderer::render(Target& efb, const EyeView* eye, Target* hud, HudMode hudMode) {
    if (!hasFrame() || !sImpl->ready) {
        if (sImpl) {
            sImpl->snapshotTaken = false;
            sImpl->hudDraws = 0;
        }
        return;
    }
    sImpl->execute(efb, eye, hud, hudMode, *this);
}

bool VulkanRenderer::snapshotTaken() const { return sImpl && sImpl->snapshotTaken; }
int VulkanRenderer::hudDrawCount() const { return sImpl ? sImpl->hudDraws : 0; }

void VulkanRenderer::finish() {
    if (!sImpl || !sImpl->ready) return;
    finishAll();
    sImpl->pipes.poll();
    // PETARI_GPUTIME=1: the GPU time of the renders this finish waited for
    // (headless: after its timed replays, as the GLES timer query).
    static const bool stats = debugEnv("PETARI_VKSTATS") != nullptr;
    if (stats) {
        sImpl->pipes.logStats("so far");
        const VkImpl::Coverage& c = sImpl->cov;
        port_log("vk: paths so far: %ld Z copies, %ld masked clears, %ld attachment clears, %ld XFB blits, %ld depth snapshots, %ld HUD target switches, "
                 "%ld line batches, %ld point batches, %ld empty viewports",
                 c.zCopies, c.maskedClears, c.attachmentClears, c.xfbBlits, c.snapshots, c.hudSwitches, c.lineBatches, c.pointBatches, c.emptyViewports);
    }
    static const bool gpuTime = debugEnv("PETARI_GPUTIME") != nullptr;
    if (gpuTime && sImpl->finishGpuCount) {
        port_log("headless: replay GPU %.2f ms a render (Vulkan timestamps, %d renders)", sImpl->finishGpuMs / sImpl->finishGpuCount,
                 sImpl->finishGpuCount);
    }
    sImpl->finishGpuMs = 0.0;
    sImpl->finishGpuCount = 0;
}

bool VulkanRenderer::readPixels(const Target& t, uint8_t* rgba) {
    if (!sImpl || !sImpl->ready || !sImages.valid(t.color)) return false;
    VkImpl& im = *sImpl;
    VkDeviceSize bytes = (VkDeviceSize)t.width * t.height * 4;
    if (im.readback.size < bytes) {
        finishAll();  // the old buffer may be in flight
        destroyBuffer(im.readback);
        if (!createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemUse::Readback, &im.readback, MemCat::Readback, "readback")) return false;
    }
    VkCommandBuffer cmd = commandBuffer();
    Img& c = sImages[t.color];
    im.bar.use(c, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kStageTransfer, VK_ACCESS_2_TRANSFER_READ_BIT);
    im.bar.flush();
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(u32)t.width, (u32)t.height, 1};
    vkCmdCopyImageToBuffer(cmd, c.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im.readback.buffer, 1, &region);
    im.bar.use(c, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kStageSample, kAccessSample);
    im.bar.flush();
    finishAll();
    im.finishGpuMs = 0.0;  // a readback is not one of the timed replays
    im.finishGpuCount = 0;
    // Rows as stored: the first is the bottom of the picture (GL order).
    memcpy(rgba, im.readback.ptr, (size_t)bytes);
    return true;
}

}  // namespace

// The device itself comes later (VulkanRenderer::init): the XR presentation
// makes it through the runtime once the session's system is known.
Renderer* createVulkanRenderer() {
    if (!spirvAvailable()) {
        port_log("vk: %s; no Vulkan renderer", spirvLoadError());
        return nullptr;
    }
    if (!deviceReady() && !probeDevice()) {
        return nullptr;
    }
    return new VulkanRenderer();
}

}  // namespace gpu

// ---------------------------------------------------------------------------
// The renderer's images for the XR presentation (vk_device.h)
// ---------------------------------------------------------------------------
namespace gpu {
namespace vk {

bool imageInfo(uint32_t handle, ImageInfo* out) {
    if (!sImages.valid(handle)) return false;
    Img& im = sImages[handle];
    out->image = im.image;
    out->view = im.view;
    out->attView = im.attView;
    out->format = im.format;
    out->width = im.width;
    out->height = im.height;
    out->layout = im.layout;
    return true;
}

void transitionImage(uint32_t handle, VkImageLayout layout, VkPipelineStageFlags2 stages, VkAccessFlags2 access) {
    if (!sImages.valid(handle)) return;
    Barriers b;
    b.use(sImages[handle], layout, stages, access);
    b.flush();
}

void assumeImageLayout(uint32_t handle, VkImageLayout layout) {
    if (!sImages.valid(handle)) return;
    Img& im = sImages[handle];
    im.layout = layout;
    im.stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    im.access = VK_ACCESS_2_MEMORY_WRITE_BIT;
}

uint32_t registerImage(VkImage image, VkImageView view, VkImageView attView, VkFormat format, uint32_t width, uint32_t height, VkImageLayout layout) {
    u32 h = sImages.alloc();
    if (!h) return 0;
    Img& im = sImages[h];
    im.image = image;
    im.view = view;
    im.attView = attView ? attView : view;
    im.format = format;
    im.width = width;
    im.height = height;
    im.levels = 1;
    im.kind = ImgKind::External;
    im.layout = layout;
    im.stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    im.access = VK_ACCESS_2_MEMORY_WRITE_BIT;
    return h;
}

void unregisterImage(uint32_t handle) {
    if (sImages.valid(handle) && sImages[handle].kind == ImgKind::External) sImages.release(handle);
}

void setKeepDepth(bool keep) { sKeepDepth = keep; }

int lastRenderSerial() { return sLastSerial; }

bool renderGpuMs(int serial, double* ms) {
    const TimedRender& t = sTimed[serial & 63];
    if (!serial || t.serial != serial) return false;
    *ms = t.ms;
    return true;
}

}  // namespace vk
}  // namespace gpu
