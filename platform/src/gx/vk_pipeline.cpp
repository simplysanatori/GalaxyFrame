// Pipelines of the Vulkan renderer (vk_pipeline.h).
#include "vk_pipeline.h"

#include <stdio.h>
#include <string.h>

#include <thread>

#include "gpu.h"
#include "gx_record.h"
#include "port/heap_routing.h"
#include "port/port.h"

namespace gpu {
namespace vk {

namespace {

int64_t nowNs() { return port_host_time_ns(); }

void atomicMax(std::atomic<int64_t>& a, int64_t v) {
    int64_t cur = a.load();
    while (v > cur && !a.compare_exchange_weak(cur, v)) {
    }
}

VkShaderModule makeModule(const std::vector<uint32_t>& words) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = words.size() * 4;
    ci.pCode = words.data();
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device().device, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

const VkDynamicState kDynamic[] = {
    VK_DYNAMIC_STATE_VIEWPORT,
    VK_DYNAMIC_STATE_SCISSOR,
    VK_DYNAMIC_STATE_LINE_WIDTH,
    VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
    VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
    VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_CULL_MODE_EXT,
    VK_DYNAMIC_STATE_FRONT_FACE_EXT,
    VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY_EXT,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT,
    VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT,
    VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT,
    VK_DYNAMIC_STATE_STENCIL_OP_EXT,
    VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,
    VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
    VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT,
};

// Everything a pipeline needs but its shaders and vertex input.
struct FixedState {
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportDepthClipControlCreateInfoEXT clip{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationLineStateCreateInfoEXT line{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att{};
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkFormat colorFormat = kColorFormat;
    VkPipelineRenderingCreateInfoKHR rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};

    explicit FixedState(bool depth) {
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        // GL clip space: z from -w to w (the generated shaders and xmath.h).
        clip.negativeOneToOne = VK_TRUE;
        vp.pNext = &clip;
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rs.lineWidth = 1.0f;
        if (device().lineBresenham) {
            // GL's lines, as zink draws them.
            line.lineRasterizationMode = VK_LINE_RASTERIZATION_MODE_BRESENHAM_EXT;
            rs.pNext = &line;
        }
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        ds.front.compareOp = ds.back.compareOp = VK_COMPARE_OP_ALWAYS;
        ds.front.compareMask = ds.back.compareMask = 0xFF;
        ds.front.writeMask = ds.back.writeMask = 0xFF;
        ds.front.failOp = ds.back.failOp = VK_STENCIL_OP_KEEP;
        ds.front.passOp = ds.back.passOp = VK_STENCIL_OP_REPLACE;
        ds.front.depthFailOp = ds.back.depthFailOp = VK_STENCIL_OP_KEEP;
        att.colorWriteMask = 0xF;
        cb.attachmentCount = 1;
        cb.pAttachments = &att;
        dyn.dynamicStateCount = (uint32_t)(sizeof(kDynamic) / sizeof(kDynamic[0]));
        dyn.pDynamicStates = kDynamic;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colorFormat;
        rendering.depthAttachmentFormat = depth ? kDepthFormat : VK_FORMAT_UNDEFINED;
        rendering.stencilAttachmentFormat = depth ? kDepthFormat : VK_FORMAT_UNDEFINED;
    }

    void fill(VkGraphicsPipelineCreateInfo& ci) {
        ci.pNext = &rendering;
        // Every pipeline may draw in a foveated eye render (a fragment
        // density map attachment, VulkanRenderer::setFoveation).
        if (device().fragmentDensityMap) ci.flags |= VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dyn;
    }
};

// Clears with a partial color mask (GL's glColorMask applies to glClear;
// vkCmdClearAttachments ignores the write mask): a full-target quad of the
// clear color (u_copyRect) at the clear depth (u_pad0, as float bits).
const char* const kClearVs = R"(#version 450
layout(push_constant, std430) uniform PushBlock {
    uint u_firstOrdinal;
    int u_copyMode;
    uint u_pad0;
    uint u_pad1;
    vec4 u_copyRect;
};
void main() {
    vec2 p = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* const kClearFs = R"(#version 450
layout(push_constant, std430) uniform PushBlock {
    uint u_firstOrdinal;
    int u_copyMode;
    uint u_pad0;
    uint u_pad1;
    vec4 u_copyRect;
};
layout(location = 0) out vec4 o;
void main() {
    o = u_copyRect;
    gl_FragDepth = uintBitsToFloat(u_pad0);
}
)";

}  // namespace

bool Pipelines::init(const std::string& spirvPath, const std::string& cachePath) {
    PortHostAllocScope hostAlloc;
    VkDevice dev = device().device;
    mEntries.reset(new Entry[kMaxPipelines]);

    // Layouts (gl_shadergen.h, kVk*).
    VkDescriptorSetLayoutBinding b0[5] = {};
    for (uint32_t i = 0; i < 5; i++) {
        b0[i].binding = i;
        b0[i].descriptorType = i == kVkBindEye ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b0[i].descriptorCount = 1;
        b0[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo l0{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    l0.bindingCount = 5;
    l0.pBindings = b0;
    VkDescriptorSetLayoutBinding b1[kVkNumSamplers] = {};
    for (uint32_t i = 0; i < kVkNumSamplers; i++) {
        b1[i].binding = i;
        b1[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b1[i].descriptorCount = 1;
        b1[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo l1{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    l1.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    l1.bindingCount = kVkNumSamplers;
    l1.pBindings = b1;
    if (vkCreateDescriptorSetLayout(dev, &l0, nullptr, &bufferSetLayout) != VK_SUCCESS ||
        vkCreateDescriptorSetLayout(dev, &l1, nullptr, &textureSetLayout) != VK_SUCCESS) {
        port_log("vk: cannot create the descriptor set layouts");
        return false;
    }
    VkDescriptorSetLayout sets[2] = {bufferSetLayout, textureSetLayout};
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kVkPushSize};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 2;
    lci.pSetLayouts = sets;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(dev, &lci, nullptr, &layout) != VK_SUCCESS) {
        port_log("vk: cannot create the pipeline layout");
        return false;
    }

    // pipelines.bin, for this device and driver only.
    mCachePath = cachePath;
    VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &idp;
    vkGetPhysicalDeviceProperties2(device().phys, &p2);
    mCacheId.vendorId = p2.properties.vendorID;
    mCacheId.deviceId = p2.properties.deviceID;
    mCacheId.driverVersion = p2.properties.driverVersion;
    mCacheId.apiVersion = p2.properties.apiVersion;
    memcpy(mCacheId.deviceUUID, idp.deviceUUID, 16);
    memcpy(mCacheId.cacheUUID, p2.properties.pipelineCacheUUID, 16);
    std::vector<uint8_t> blob;
    bool loaded = !cachePath.empty() && pipelineCacheLoad(cachePath.c_str(), mCacheId, &blob);
    VkPipelineCacheCreateInfo pcci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    pcci.initialDataSize = blob.size();
    pcci.pInitialData = blob.empty() ? nullptr : blob.data();
    if (vkCreatePipelineCache(dev, &pcci, nullptr, &mCache) != VK_SUCCESS) {
        pcci.initialDataSize = 0;
        pcci.pInitialData = nullptr;
        loaded = false;
        if (vkCreatePipelineCache(dev, &pcci, nullptr, &mCache) != VK_SUCCESS) mCache = VK_NULL_HANDLE;
    }

    size_t spirvLoaded = 0, spirvStale = 0;
    mSpirv.open(spirvPath.c_str(), &spirvLoaded, &spirvStale);
    port_log("vk: pipeline cache %s: %s (%zu KB); SPIR-V cache %s: %zu programs, %zu stale", cachePath.c_str(), loaded ? "loaded" : "new",
             blob.size() >> 10, spirvPath.c_str(), spirvLoaded, spirvStale);

    // The fixed programs.
    std::vector<uint32_t> cvs, cfs, kvs, kfs;
    std::string log;
    if (!spirvCompileGlsl(SpirvStage::Vertex, genCopyVertexShader(Dialect::VK), &cvs, &log) ||
        !spirvCompileGlsl(SpirvStage::Fragment, genCopyFragmentShader(Dialect::VK), &cfs, &log) ||
        !spirvCompileGlsl(SpirvStage::Vertex, kClearVs, &kvs, &log) || !spirvCompileGlsl(SpirvStage::Fragment, kClearFs, &kfs, &log)) {
        port_log("vk: fixed shaders do not compile: %s", log.c_str());
        return false;
    }
    copyPipeline = createFixed(cvs, cfs, false, "gq copy");
    clearPipeline = createFixed(kvs, kfs, true, "gq clear");
    if (!copyPipeline || !clearPipeline) {
        port_log("vk: cannot create the fixed pipelines");
        return false;
    }

    mPool.reset(new SpirvCompilePool(&mSpirv, 2));
    mPool->setCallback([this](const SpirvResult& r) { onCompiled(r); });
    mPool->start();
    // Programs an older generator made: rebuilt now rather than when a scene
    // first needs them.
    for (const ShaderUid& uid : mSpirv.staleUids()) {
        request(uid, false, -2);
    }
    return true;
}

VkPipeline Pipelines::createFixed(const std::vector<uint32_t>& vs, const std::vector<uint32_t>& fs, bool depth, const char* name) {
    VkShaderModule mv = makeModule(vs), mf = makeModule(fs);
    VkPipeline p = VK_NULL_HANDLE;
    if (mv && mf) {
        VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = mv;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = mf;
        st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        FixedState fx(depth);
        fx.ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        fx.fill(ci);
        ci.stageCount = 2;
        ci.pStages = st;
        ci.pVertexInputState = &vi;
        ci.layout = layout;
        if (vkCreateGraphicsPipelines(device().device, mCache, 1, &ci, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
        setName(VK_OBJECT_TYPE_PIPELINE, (uint64_t)p, name);
    }
    if (mv) vkDestroyShaderModule(device().device, mv, nullptr);
    if (mf) vkDestroyShaderModule(device().device, mf, nullptr);
    return p;
}

VkPipeline Pipelines::create(const ShaderUid& uid, const SpirvProgram& prog) {
    int64_t t0 = nowNs();
    VkShaderModule mv = makeModule(prog.vs), mf = makeModule(prog.fs);
    VkPipeline p = VK_NULL_HANDLE;
    if (mv && mf) {
        VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = mv;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = mf;
        st[1].pName = "main";
        // The vertex layout of the uid's flags, as the GLES renderer's VAOs
        // (gl_renderer.cpp vao()): one buffer, whole words per attribute.
        uint32_t flags = uid.vtxFlags;
        VkVertexInputAttributeDescription attrs[16];
        uint32_t n = 0, off = 0;
        auto attr = [&](uint32_t loc, VkFormat fmt, uint32_t bytes) {
            attrs[n].location = loc;
            attrs[n].binding = 0;
            attrs[n].format = fmt;
            attrs[n].offset = off;
            n++;
            off += bytes;
        };
        attr(0, VK_FORMAT_R32G32B32_SFLOAT, 12);
        if (flags & VF_POSMTX) attr(1, VK_FORMAT_R32_UINT, 4);
        if (flags & VF_NRM) attr(2, VK_FORMAT_R32G32B32_SFLOAT, 12);
        if (flags & VF_NBT) {
            attr(3, VK_FORMAT_R32G32B32_SFLOAT, 12);
            attr(4, VK_FORMAT_R32G32B32_SFLOAT, 12);
        }
        if (flags & VF_CLR0) attr(5, VK_FORMAT_R8G8B8A8_UNORM, 4);
        if (flags & VF_CLR1) attr(6, VK_FORMAT_R8G8B8A8_UNORM, 4);
        for (uint32_t i = 0; i < 8; i++) {
            if (flags & (VF_TEX0 << i)) attr(7 + i, VK_FORMAT_R32G32_SFLOAT, 8);
        }
        if (flags & VF_TEXMTX) attr(15, VK_FORMAT_R32G32_UINT, 8);
        VkVertexInputBindingDescription bind{0, vtxWords(flags) * 4, VK_VERTEX_INPUT_RATE_VERTEX};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &bind;
        vi.vertexAttributeDescriptionCount = n;
        vi.pVertexAttributeDescriptions = attrs;
        FixedState fx(true);
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        fx.fill(ci);
        ci.stageCount = 2;
        ci.pStages = st;
        ci.pVertexInputState = &vi;
        ci.layout = layout;
        VkResult r = vkCreateGraphicsPipelines(device().device, mCache, 1, &ci, nullptr, &p);
        if (r != VK_SUCCESS) {
            port_log("vk: vkCreateGraphicsPipelines failed (%d) for vertex flags %05x", (int)r, flags);
            p = VK_NULL_HANDLE;
        }
    }
    if (mv) vkDestroyShaderModule(device().device, mv, nullptr);
    if (mf) vkDestroyShaderModule(device().device, mf, nullptr);
    int64_t dt = nowNs() - t0;
    mCreateNs += dt;
    atomicMax(mCreateMaxNs, dt);
    return p;
}

void Pipelines::publish(uint32_t id, VkPipeline p) {
    Entry& e = mEntries[id];
    if (!p) {
        int expected = e.state.load();
        if (expected != 2) {
            e.state.store(3);
            mFailed++;
        }
        return;
    }
    VkPipeline expected = VK_NULL_HANDLE;
    if (e.pipe.compare_exchange_strong(expected, p, std::memory_order_acq_rel)) {
        e.state.store(2);
        mReady++;
    } else {
        vkDestroyPipeline(device().device, p, nullptr);  // built twice (sync and background)
    }
}

uint32_t Pipelines::request(const ShaderUid& uid, bool sync, int priority) {
    uint32_t id;
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lock(mLock);
        auto it = mIds.find(uid);
        if (it != mIds.end()) {
            id = it->second;
        } else {
            if (mCount + 1 >= kMaxPipelines) {
                return 0;
            }
            id = ++mCount;
            mIds.emplace(uid, id);
            fresh = true;
        }
    }
    Entry& e = mEntries[id];
    int state = e.state.load();
    if (state >= 2) {
        return id;
    }
    if (sync) {
        std::shared_ptr<const SpirvProgram> prog = mSpirv.find(uid);
        if (prog) {
            mFromCache++;
        } else {
            auto built = std::make_shared<SpirvProgram>();
            uint64_t hash = 0;
            std::string log;
            int64_t t0 = nowNs();
            bool ok = spirvBuild(uid, built.get(), &hash, &log);
            int64_t dt = nowNs() - t0;
            mSpirvNs += dt;
            atomicMax(mSpirvMaxNs, dt);
            mSpirvTimed++;
            if (!ok) {
                port_log("vk: program does not compile: %.600s", log.c_str());
                publish(id, VK_NULL_HANDLE);
                return id;
            }
            mCompiled++;
            mSpirv.put(uid, hash, built);
            prog = built;
        }
        publish(id, create(uid, *prog));
        return id;
    }
    if (fresh || priority > 0) {
        if (fresh) {
            e.state.store(1);
            mInFlight++;
        }
        mPool->enqueue(uid, priority);
    }
    return id;
}

void Pipelines::onCompiled(const SpirvResult& r) {
    uint32_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mLock);
        auto it = mIds.find(r.uid);
        if (it != mIds.end()) id = it->second;
    }
    if (id) {
        if (!r.ok) {
            port_log("vk: program does not compile: %.600s", r.log.c_str());
            publish(id, VK_NULL_HANDLE);
        } else if (!mEntries[id].pipe.load()) {
            r.cached ? mFromCache++ : mCompiled++;
            publish(id, create(r.uid, *r.prog));
        }
    }
    mInFlight--;
}

void Pipelines::prewarm() {
    std::vector<ShaderUid> uids = mSpirv.uids();
    mPrewarmStartNs = port_host_time_ns();
    for (const ShaderUid& uid : uids) {
        request(uid, false, -1);
    }
    mPrewarming = true;
    port_log("vk: creating the %zu pipelines of the SPIR-V cache in the background", uids.size());
}

void Pipelines::savePipelineCache() {
    std::lock_guard<std::mutex> lock(mSaveLock);
    if (mCachePath.empty() || !mCache) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(device().device, mCache, &size, nullptr) != VK_SUCCESS || !size) return;
    std::vector<uint8_t> data(size);
    if (vkGetPipelineCacheData(device().device, mCache, &size, data.data()) != VK_SUCCESS) return;
    if (pipelineCacheSave(mCachePath.c_str(), mCacheId, data.data(), size)) {
        port_log("vk: pipeline cache saved: %zu KB, %zu pipelines", size >> 10, mReady.load());
    }
}

void Pipelines::poll() {
    SpirvResult r;
    while (mPool && mPool->poll(&r)) {
    }
    if (mPrewarming && mInFlight.load() == 0) {
        mPrewarming = false;
        char why[64];
        snprintf(why, sizeof(why), "background creation done after %.1f s", (port_host_time_ns() - mPrewarmStartNs) / 1e9);
        logStats(why);
    }
    // pipelines.bin once the compiles have settled (on a thread of its own:
    // the cache can be megabytes).
    size_t ready = mReady.load();
    if (ready != mSavedAt.load() && mInFlight.load() == 0) {
        static std::atomic<bool> saving{false};
        bool expected = false;
        if (saving.compare_exchange_strong(expected, true)) {
            mSavedAt.store(ready);
            PortHostAllocScope hostAlloc;
            std::thread([this] {
                PortHostAllocScope scope;
                savePipelineCache();
                saving.store(false);
            }).detach();
        }
    }
}

void Pipelines::logStats(const char* why) {
    size_t created = mReady.load() + mFailed.load();
    size_t timed = mSpirvTimed.load();
    port_log("vk: pipelines (%s): %zu ready, %zu failed, %zu queued; %zu from spirv.bin, %zu compiled from GLSL (%.1f ms avg, %.1f max); "
             "vkCreateGraphicsPipelines %.2f ms avg, %.1f max",
             why, mReady.load(), mFailed.load(), mInFlight.load(), mFromCache.load(), mCompiled.load(), timed ? mSpirvNs.load() / 1e6 / timed : 0.0,
             mSpirvMaxNs.load() / 1e6, created ? mCreateNs.load() / 1e6 / created : 0.0, mCreateMaxNs.load() / 1e6);
}

}  // namespace vk
}  // namespace gpu
