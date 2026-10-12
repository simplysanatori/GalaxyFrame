// Vulkan backend shader front end: GLSL (generated, VK dialect) to SPIR-V
// through libgq_glsl.so, the on-disk SPIR-V cache (spirv.bin), a small compile
// pool, the pipeline cache file (pipelines.bin) and the converter that seeds
// spirv.bin from a GLES program cache (shaders.bin).
//
// libgq_glsl.so (tools/glsl) is loaded with dlopen from the directory of the
// library this code is in.  When it is missing spirvAvailable() is false and
// the Vulkan backend falls back to GLES.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gl_shadergen.h"

namespace gpu {

struct SpirvProgram {
    std::vector<uint32_t> vs, fs;
};

enum class SpirvStage { Vertex, Fragment };

// ---------------------------------------------------------------------------
// Compiler front end
// ---------------------------------------------------------------------------

// Loads libgq_glsl.so once; false if it is missing or does not match.
// spirvLoadError() says why.  PETARI_GQ_GLSL=<path> overrides the location.
bool spirvAvailable();
const char* spirvLoadError();

// Compiles GLSL (with its own #version) for Vulkan 1.2.  Thread safe.
bool spirvCompileGlsl(SpirvStage stage, const std::string& glsl, std::vector<uint32_t>* words, std::string* log);

// FNV-1a of the generated VK GLSL text (vertex then fragment) of `uid`: what
// identifies a cached entry.  Pass the texts to avoid generating them twice.
uint64_t spirvSourceHash(const ShaderUid& uid);
uint64_t spirvTextHash(const std::string& vs, const std::string& fs);

// Generates the VK dialect of `uid` and compiles both stages.
bool spirvBuild(const ShaderUid& uid, SpirvProgram* out, uint64_t* srcHash, std::string* log);

// ---------------------------------------------------------------------------
// spirv.bin
//
//   header  u32 magic "PSV1", u32 format version, u32 sizeof(ShaderUid), u32 gq_glsl version
//   record  u32 magic "SPRC", u32 vs words, u32 fs words, u32 0,
//           u64 FNV-1a of the VK GLSL text, ShaderUid, vs words, fs words, u32 crc32
//
// Append only while running.  open() drops what no longer matches the
// generator (stale) and what repeats, and cuts a corrupt tail.
// ---------------------------------------------------------------------------
class SpirvCache {
public:
    ~SpirvCache() { close(); }
    // Without `path` nothing is read or written.  `loaded` / `stale` receive
    // the number of entries kept and dropped.
    bool open(const char* path, size_t* loaded = nullptr, size_t* stale = nullptr);
    void close();

    std::shared_ptr<const SpirvProgram> find(const ShaderUid& uid);
    void put(const ShaderUid& uid, uint64_t srcHash, std::shared_ptr<const SpirvProgram> prog);
    size_t size();
    std::vector<ShaderUid> uids();
    // Entries that were dropped as stale at open(): to be recompiled.
    std::vector<ShaderUid> staleUids();

private:
    struct Entry {
        uint64_t srcHash;
        std::shared_ptr<const SpirvProgram> prog;
    };
    bool writeRecord(FILE* f, const ShaderUid& uid, uint64_t srcHash, const SpirvProgram& p);
    std::mutex lock_;
    std::unordered_map<ShaderUid, Entry, ShaderUidHash> map_;
    std::vector<ShaderUid> stale_;
    FILE* out_ = nullptr;
};

// ---------------------------------------------------------------------------
// Compile pool: worker threads (2 by default) that turn ShaderUids into SPIR-V,
// through the cache when there is one.  Higher priority first, then first in.
// ---------------------------------------------------------------------------
struct SpirvResult {
    ShaderUid uid;
    bool ok = false;
    bool cached = false;  // came from the cache, not compiled
    std::shared_ptr<const SpirvProgram> prog;
    std::string log;      // compiler messages when !ok
};

class SpirvCompilePool {
public:
    explicit SpirvCompilePool(SpirvCache* cache, int threads = 2) : cache_(cache), threads_(threads) {}
    ~SpirvCompilePool() { stop(); }
    // Called on a worker thread for every finished uid, before it is queued for poll().
    void setCallback(std::function<void(const SpirvResult&)> cb) { callback_ = std::move(cb); }
    void start();
    void stop();
    // False if the uid is already queued, being compiled, waiting in poll(), or failed before
    // (a queued uid has its priority raised).
    bool enqueue(const ShaderUid& uid, int priority = 0);
    bool poll(SpirvResult* out);
    size_t pending();      // queued + being compiled
    void waitIdle();       // until nothing is queued or being compiled

private:
    struct Job {
        ShaderUid uid;
        int priority;
        uint64_t seq;
    };
    void workerMain();
    SpirvCache* cache_;
    int threads_;
    std::function<void(const SpirvResult&)> callback_;
    std::mutex lock_;
    std::condition_variable wake_, idle_;
    std::vector<Job> queue_;
    std::unordered_set<ShaderUid, ShaderUidHash> known_;  // queued, running, unpolled or failed
    std::vector<SpirvResult> results_;
    std::vector<std::thread> workers_;
    size_t running_ = 0;
    uint64_t seq_ = 0;
    bool stopping_ = false;
};

// ---------------------------------------------------------------------------
// pipelines.bin: a VkPipelineCache blob behind a header that says which
// device and driver made it.  The Vulkan device code fills the id from
// VkPhysicalDeviceProperties; a blob from another device or driver is not
// returned (the cache then starts empty).
// ---------------------------------------------------------------------------
struct PipelineCacheId {
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint32_t driverVersion = 0;
    uint32_t apiVersion = 0;
    uint8_t deviceUUID[16] = {};  // VkPhysicalDeviceIDProperties::deviceUUID
    uint8_t cacheUUID[16] = {};   // VkPhysicalDeviceProperties::pipelineCacheUUID
};

bool pipelineCacheLoad(const char* path, const PipelineCacheId& id, std::vector<uint8_t>* blob);
bool pipelineCacheSave(const char* path, const PipelineCacheId& id, const void* data, size_t size);

// ---------------------------------------------------------------------------
// Converter: reads the ShaderUids out of a GLES program cache (shaders.bin,
// "PGC2") and writes the SPIR-V of each to `outPath` (kept entries are reused).
// ---------------------------------------------------------------------------
bool spirvReadGlesCacheUids(const char* glesCachePath, std::vector<ShaderUid>* uids);

struct SpirvConvertStats {
    size_t uids = 0;      // distinct uids in the input
    size_t reused = 0;    // already in the output
    size_t compiled = 0;
    size_t failed = 0;
};
bool spirvConvertFromGlesCache(const char* glesCachePath, const char* outPath, SpirvConvertStats* stats = nullptr, int threads = 4);

}  // namespace gpu
