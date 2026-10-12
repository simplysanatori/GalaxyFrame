// Pipelines of the Vulkan renderer: one VkPipeline per ShaderUid (its vertex
// flags fix the vertex layout; everything else is dynamic state), built from
// the generated VK-dialect GLSL through spirv.bin (vk_spirv.h), on two
// compile threads or synchronously, with a VkPipelineCache kept in
// pipelines.bin.  Also the layouts every pipeline shares and the fixed EFB
// copy and clear pipelines.
#pragma once

#include <stdint.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gl_shadergen.h"
#include "vk_device.h"
#include "vk_spirv.h"

namespace gpu {
namespace vk {

// Attachment formats of every render target (dynamic rendering).
const VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
const VkFormat kDepthFormat = VK_FORMAT_D24_UNORM_S8_UINT;

class Pipelines {
public:
    // spirvPath / cachePath: spirv.bin and pipelines.bin (may be empty).
    bool init(const std::string& spirvPath, const std::string& cachePath);

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout bufferSetLayout = VK_NULL_HANDLE;   // set 0: SSBOs 0-3, EyeBlock UBO 4 (dynamic offset)
    VkDescriptorSetLayout textureSetLayout = VK_NULL_HANDLE;  // set 1: 8 combined image samplers, push descriptors
    VkPipeline copyPipeline = VK_NULL_HANDLE;   // EFB copy (12 modes) into a color-only target
    VkPipeline clearPipeline = VK_NULL_HANDLE;  // clears with a partial color mask, EFB targets

    // The id of the pipeline for `uid` (never 0).  With `sync` it is built
    // before this returns; otherwise it is queued for the compile threads
    // (priority: higher first) and get() returns null until it is ready.
    uint32_t request(const ShaderUid& uid, bool sync, int priority);
    VkPipeline get(uint32_t id) const { return id && id < kMaxPipelines ? mEntries[id].pipe.load(std::memory_order_acquire) : VK_NULL_HANDLE; }
    // Every program of spirv.bin, compiled in the background at low priority.
    void prewarm();
    // Rendering thread, now and then: statistics, pipelines.bin.
    void poll();
    void logStats(const char* why);
    size_t pipelineCount() const { return mReady.load(); }

private:
    static const uint32_t kMaxPipelines = 1u << 16;
    struct Entry {
        std::atomic<VkPipeline> pipe{VK_NULL_HANDLE};
        std::atomic<int> state{0};  // 0 none, 1 queued, 2 ready, 3 failed
    };
    VkPipeline create(const ShaderUid& uid, const SpirvProgram& prog);
    VkPipeline createFixed(const std::vector<uint32_t>& vs, const std::vector<uint32_t>& fs, bool depth, const char* name);
    void publish(uint32_t id, VkPipeline p);
    void onCompiled(const SpirvResult& r);
    void savePipelineCache();

    std::unique_ptr<Entry[]> mEntries;
    std::mutex mLock;  // mIds, mCount
    std::unordered_map<ShaderUid, uint32_t, ShaderUidHash> mIds;
    uint32_t mCount = 0;
    VkPipelineCache mCache = VK_NULL_HANDLE;
    std::string mCachePath;
    PipelineCacheId mCacheId;
    SpirvCache mSpirv;
    std::unique_ptr<SpirvCompilePool> mPool;
    std::mutex mSaveLock;

    // Statistics.
    std::atomic<size_t> mReady{0}, mFailed{0}, mFromCache{0}, mCompiled{0};
    std::atomic<int64_t> mCreateNs{0}, mCreateMaxNs{0}, mSpirvNs{0}, mSpirvMaxNs{0};
    std::atomic<size_t> mSpirvTimed{0};
    std::atomic<size_t> mInFlight{0}, mSavedAt{0};
};

}  // namespace vk
}  // namespace gpu
