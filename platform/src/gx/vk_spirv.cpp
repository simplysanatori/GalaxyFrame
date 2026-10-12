// GLSL to SPIR-V for the Vulkan backend: see vk_spirv.h.
#include "vk_spirv.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include "gpu.h"
#include "gq_glsl.h"
#include "port/heap_routing.h"
#include "port/port.h"

namespace gpu {

namespace {

// ---------------------------------------------------------------------------
// libgq_glsl.so
// ---------------------------------------------------------------------------
struct GlslLib {
    int (*compile)(int, const char*, size_t, uint32_t**, size_t*, char**) = nullptr;
    void (*release)(void*) = nullptr;
};

std::mutex sLibLock;
bool sLibTried = false;
GlslLib sLib;
std::string sLibError;

void markerFunction() {}

std::string moduleDir() {
    Dl_info info;
    if (dladdr((void*)&markerFunction, &info) && info.dli_fname) {
        std::string path = info.dli_fname;
        size_t slash = path.rfind('/');
        if (slash != std::string::npos) {
            return path.substr(0, slash);
        }
    }
    return ".";
}

bool loadLib() {
    std::lock_guard<std::mutex> lock(sLibLock);
    if (sLibTried) {
        return sLib.compile != nullptr;
    }
    sLibTried = true;
    std::string path = debugEnv("PETARI_GQ_GLSL") ? debugEnv("PETARI_GQ_GLSL") : moduleDir() + "/libgq_glsl.so";
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* e = dlerror();
        sLibError = path + ": " + (e ? e : "dlopen failed");
        return false;
    }
    auto version = (int (*)(void))dlsym(h, "gq_glsl_version");
    auto compile = (int (*)(int, const char*, size_t, uint32_t**, size_t*, char**))dlsym(h, "gq_glsl_compile");
    auto release = (void (*)(void*))dlsym(h, "gq_glsl_free");
    if (!version || !compile || !release) {
        sLibError = path + ": missing symbols";
        dlclose(h);
        return false;
    }
    if (version() != GQ_GLSL_VERSION) {
        sLibError = path + ": version " + std::to_string(version()) + ", expected " + std::to_string(GQ_GLSL_VERSION);
        dlclose(h);
        return false;
    }
    sLib.compile = compile;
    sLib.release = release;
    return true;
}

uint64_t fnv1a(uint64_t h, const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

uint32_t crc32Update(uint32_t crc, const void* data, size_t n) {
    static uint32_t table[256];
    static bool ready;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;  // benign race: every thread computes the same table
    }
    const uint8_t* p = (const uint8_t*)data;
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

}  // namespace

bool spirvAvailable() {
    return loadLib();
}

const char* spirvLoadError() {
    std::lock_guard<std::mutex> lock(sLibLock);
    return sLibError.c_str();
}

bool spirvCompileGlsl(SpirvStage stage, const std::string& glsl, std::vector<uint32_t>* words, std::string* log) {
    if (!loadLib()) {
        if (log) *log = sLibError;
        return false;
    }
    uint32_t* w = nullptr;
    size_t n = 0;
    char* msg = nullptr;
    int rc = sLib.compile(stage == SpirvStage::Vertex ? GQ_GLSL_VERTEX : GQ_GLSL_FRAGMENT, glsl.data(), glsl.size(), &w, &n, &msg);
    if (log) *log = msg ? msg : "";
    if (msg) sLib.release(msg);
    if (rc != 0) {
        return false;
    }
    words->assign(w, w + n);
    sLib.release(w);
    return true;
}

uint64_t spirvTextHash(const std::string& vs, const std::string& fs) {
    uint64_t h = 1469598103934665603ull;
    h = fnv1a(h, vs.data(), vs.size());
    h = fnv1a(h, fs.data(), fs.size());
    return h;
}

uint64_t spirvSourceHash(const ShaderUid& uid) {
    return spirvTextHash(genVertexShader(uid, Dialect::VK), genFragmentShader(uid, Dialect::VK));
}

bool spirvBuild(const ShaderUid& uid, SpirvProgram* out, uint64_t* srcHash, std::string* log) {
    std::string vs = genVertexShader(uid, Dialect::VK), fs = genFragmentShader(uid, Dialect::VK);
    if (srcHash) *srcHash = spirvTextHash(vs, fs);
    std::string vlog, flog;
    bool ok = spirvCompileGlsl(SpirvStage::Vertex, vs, &out->vs, &vlog) && spirvCompileGlsl(SpirvStage::Fragment, fs, &out->fs, &flog);
    if (log) *log = !vlog.empty() && !ok ? "vertex: " + vlog : flog.empty() ? vlog : "fragment: " + flog;
    return ok;
}

// ---------------------------------------------------------------------------
// spirv.bin
// ---------------------------------------------------------------------------
namespace {

const uint32_t kFileMagic = 0x31565350;  // "PSV1"
const uint32_t kFileVersion = 1;
const uint32_t kRecMagic = 0x43525053;   // "SPRC"
const uint32_t kMaxWords = 8u << 20;

struct FileHeader {
    uint32_t magic, version, uidSize, glslVersion;
};

struct RecHeader {
    uint32_t magic, vsWords, fsWords, zero;
    uint64_t srcHash;
};

}  // namespace

bool SpirvCache::writeRecord(FILE* f, const ShaderUid& uid, uint64_t srcHash, const SpirvProgram& p) {
    RecHeader h = {kRecMagic, (uint32_t)p.vs.size(), (uint32_t)p.fs.size(), 0, srcHash};
    uint32_t crc = crc32Update(0, &h, sizeof(h));
    crc = crc32Update(crc, &uid, sizeof(uid));
    crc = crc32Update(crc, p.vs.data(), p.vs.size() * 4);
    crc = crc32Update(crc, p.fs.data(), p.fs.size() * 4);
    return fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(&uid, sizeof(uid), 1, f) == 1 &&
           (p.vs.empty() || fwrite(p.vs.data(), 4, p.vs.size(), f) == p.vs.size()) &&
           (p.fs.empty() || fwrite(p.fs.data(), 4, p.fs.size(), f) == p.fs.size()) && fwrite(&crc, 4, 1, f) == 1;
}

bool SpirvCache::open(const char* path, size_t* loadedOut, size_t* staleOut) {
    PortHostAllocScope hostAlloc;
    close();
    std::lock_guard<std::mutex> lock(lock_);
    map_.clear();
    stale_.clear();
    size_t loaded = 0, stale = 0;
    if (loadedOut) *loadedOut = 0;
    if (staleOut) *staleOut = 0;
    if (!path || !*path) {
        return true;
    }

    bool headerOk = false, dirty = false;
    long goodEnd = 0;
    size_t dead = 0;
    FILE* in = fopen(path, "rb");
    if (in) {
        FileHeader fh;
        if (fread(&fh, sizeof(fh), 1, in) == 1 && fh.magic == kFileMagic && fh.version == kFileVersion && fh.uidSize == sizeof(ShaderUid) &&
            fh.glslVersion == (uint32_t)GQ_GLSL_VERSION) {
            headerOk = true;
            goodEnd = (long)sizeof(fh);
            for (;;) {
                RecHeader h;
                ShaderUid uid;
                if (fread(&h, sizeof(h), 1, in) != 1) break;
                if (h.magic != kRecMagic || h.vsWords > kMaxWords || h.fsWords > kMaxWords) {
                    dirty = true;
                    break;
                }
                auto prog = std::make_shared<SpirvProgram>();
                prog->vs.resize(h.vsWords);
                prog->fs.resize(h.fsWords);
                uint32_t crc = 0;
                if (fread(&uid, sizeof(uid), 1, in) != 1 || (h.vsWords && fread(prog->vs.data(), 4, h.vsWords, in) != h.vsWords) ||
                    (h.fsWords && fread(prog->fs.data(), 4, h.fsWords, in) != h.fsWords) || fread(&crc, 4, 1, in) != 1) {
                    dirty = true;
                    break;
                }
                uint32_t want = crc32Update(0, &h, sizeof(h));
                want = crc32Update(want, &uid, sizeof(uid));
                want = crc32Update(want, prog->vs.data(), prog->vs.size() * 4);
                want = crc32Update(want, prog->fs.data(), prog->fs.size() * 4);
                if (want != crc) {
                    dirty = true;
                    break;
                }
                goodEnd = ftell(in);
                if (map_.count(uid)) dead++;
                map_[uid] = Entry{h.srcHash, std::move(prog)};
            }
            if (!dirty) {
                long end = (fseek(in, 0, SEEK_END) == 0) ? ftell(in) : goodEnd;
                if (end != goodEnd) dirty = true;  // a partial record at the end
            }
        }
        fclose(in);
    }

    // Entries made by an older generator are not used; they are recompiled.
    for (auto it = map_.begin(); it != map_.end();) {
        if (it->second.srcHash != spirvSourceHash(it->first)) {
            stale++;
            stale_.push_back(it->first);
            dead++;
            it = map_.erase(it);
        } else {
            loaded++;
            ++it;
        }
    }

    if (headerOk && dead == 0) {
        if (dirty && truncate(path, goodEnd) != 0) {
            headerOk = false;  // cannot cut the tail: rewrite below
        }
    }
    if (!headerOk || dead > 0) {
        // Rewrite the file with what is kept.
        std::string tmp = std::string(path) + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        bool ok = f != nullptr;
        if (f) {
            FileHeader fh = {kFileMagic, kFileVersion, (uint32_t)sizeof(ShaderUid), (uint32_t)GQ_GLSL_VERSION};
            ok = fwrite(&fh, sizeof(fh), 1, f) == 1;
            for (auto& kv : map_) {
                ok = ok && writeRecord(f, kv.first, kv.second.srcHash, *kv.second.prog);
            }
            ok = (fclose(f) == 0) && ok;
        }
        if (!ok || rename(tmp.c_str(), path) != 0) {
            unlink(tmp.c_str());
            port_log("vk: cannot rewrite %s", path);
        }
    }
    out_ = fopen(path, "ab");
    if (loadedOut) *loadedOut = loaded;
    if (staleOut) *staleOut = stale;
    return out_ != nullptr;
}

void SpirvCache::close() {
    std::lock_guard<std::mutex> lock(lock_);
    if (out_) {
        fclose(out_);
        out_ = nullptr;
    }
}

std::shared_ptr<const SpirvProgram> SpirvCache::find(const ShaderUid& uid) {
    std::lock_guard<std::mutex> lock(lock_);
    auto it = map_.find(uid);
    return it != map_.end() ? it->second.prog : nullptr;
}

void SpirvCache::put(const ShaderUid& uid, uint64_t srcHash, std::shared_ptr<const SpirvProgram> prog) {
    PortHostAllocScope hostAlloc;
    std::lock_guard<std::mutex> lock(lock_);
    auto it = map_.find(uid);
    if (it != map_.end() && it->second.srcHash == srcHash) {
        return;
    }
    if (out_) {
        writeRecord(out_, uid, srcHash, *prog);
        fflush(out_);
    }
    map_[uid] = Entry{srcHash, std::move(prog)};
}

size_t SpirvCache::size() {
    std::lock_guard<std::mutex> lock(lock_);
    return map_.size();
}

std::vector<ShaderUid> SpirvCache::uids() {
    PortHostAllocScope hostAlloc;
    std::lock_guard<std::mutex> lock(lock_);
    std::vector<ShaderUid> v;
    v.reserve(map_.size());
    for (auto& kv : map_) v.push_back(kv.first);
    return v;
}

std::vector<ShaderUid> SpirvCache::staleUids() {
    std::lock_guard<std::mutex> lock(lock_);
    return stale_;
}

// ---------------------------------------------------------------------------
// Compile pool
// ---------------------------------------------------------------------------
void SpirvCompilePool::start() {
    PortHostAllocScope hostAlloc;
    std::lock_guard<std::mutex> lock(lock_);
    if (!workers_.empty()) {
        return;
    }
    stopping_ = false;
    for (int i = 0; i < threads_; i++) {
        workers_.emplace_back([this] { workerMain(); });
    }
}

void SpirvCompilePool::stop() {
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(lock_);
        stopping_ = true;
        workers.swap(workers_);
    }
    wake_.notify_all();
    for (std::thread& t : workers) t.join();
}

bool SpirvCompilePool::enqueue(const ShaderUid& uid, int priority) {
    PortHostAllocScope hostAlloc;
    std::lock_guard<std::mutex> lock(lock_);
    if (!known_.insert(uid).second) {
        for (Job& j : queue_) {
            if (j.uid == uid && j.priority < priority) j.priority = priority;
        }
        return false;
    }
    queue_.push_back(Job{uid, priority, seq_++});
    wake_.notify_one();
    return true;
}

bool SpirvCompilePool::poll(SpirvResult* out) {
    std::lock_guard<std::mutex> lock(lock_);
    if (results_.empty()) {
        return false;
    }
    *out = std::move(results_.front());
    results_.erase(results_.begin());
    if (out->ok) {
        known_.erase(out->uid);  // delivered: the owner has it now
    }
    return true;
}

size_t SpirvCompilePool::pending() {
    std::lock_guard<std::mutex> lock(lock_);
    return queue_.size() + running_;
}

void SpirvCompilePool::waitIdle() {
    std::unique_lock<std::mutex> lock(lock_);
    idle_.wait(lock, [&] { return queue_.empty() && running_ == 0; });
}

void SpirvCompilePool::workerMain() {
    PortHostAllocScope hostAlloc;
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(lock_);
            wake_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                return;
            }
            size_t best = 0;
            for (size_t i = 1; i < queue_.size(); i++) {
                if (queue_[i].priority > queue_[best].priority || (queue_[i].priority == queue_[best].priority && queue_[i].seq < queue_[best].seq)) {
                    best = i;
                }
            }
            job = queue_[best];
            queue_.erase(queue_.begin() + (long)best);
            running_++;
        }

        SpirvResult r;
        r.uid = job.uid;
        if (cache_) {
            if (auto hit = cache_->find(job.uid)) {
                r.ok = r.cached = true;
                r.prog = std::move(hit);
            }
        }
        if (!r.ok) {
            auto prog = std::make_shared<SpirvProgram>();
            uint64_t hash = 0;
            r.ok = spirvBuild(job.uid, prog.get(), &hash, &r.log);
            if (r.ok) {
                r.prog = prog;
                if (cache_) cache_->put(job.uid, hash, prog);
            }
        }
        if (callback_) {
            callback_(r);
        }
        {
            std::lock_guard<std::mutex> lock(lock_);
            results_.push_back(std::move(r));
            running_--;
            if (queue_.empty() && running_ == 0) {
                idle_.notify_all();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// pipelines.bin
// ---------------------------------------------------------------------------
namespace {

const uint32_t kPipeMagic = 0x31435050;  // "PPC1"
const uint32_t kPipeVersion = 1;

struct PipeHeader {
    uint32_t magic, version, headerSize, blobSize;
    uint32_t blobCrc, vendorId, deviceId, driverVersion;
    uint32_t apiVersion, pad[3];
    uint8_t deviceUUID[16];
    uint8_t cacheUUID[16];
};

void fillHeader(PipeHeader* h, const PipelineCacheId& id, size_t size, uint32_t crc) {
    memset(h, 0, sizeof(*h));
    h->magic = kPipeMagic;
    h->version = kPipeVersion;
    h->headerSize = sizeof(PipeHeader);
    h->blobSize = (uint32_t)size;
    h->blobCrc = crc;
    h->vendorId = id.vendorId;
    h->deviceId = id.deviceId;
    h->driverVersion = id.driverVersion;
    h->apiVersion = id.apiVersion;
    memcpy(h->deviceUUID, id.deviceUUID, 16);
    memcpy(h->cacheUUID, id.cacheUUID, 16);
}

}  // namespace

bool pipelineCacheLoad(const char* path, const PipelineCacheId& id, std::vector<uint8_t>* blob) {
    PortHostAllocScope hostAlloc;
    blob->clear();
    FILE* f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    PipeHeader h;
    PipeHeader want;
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.magic == kPipeMagic && h.version == kPipeVersion && h.headerSize == sizeof(h) && h.blobSize > 0 &&
              h.blobSize <= (256u << 20);
    if (ok) {
        fillHeader(&want, id, h.blobSize, h.blobCrc);
        ok = memcmp(&h, &want, sizeof(h)) == 0;  // same device, driver and cache UUID
    }
    if (ok) {
        blob->resize(h.blobSize);
        ok = fread(blob->data(), 1, h.blobSize, f) == h.blobSize && crc32Update(0, blob->data(), blob->size()) == h.blobCrc;
    }
    fclose(f);
    if (!ok) {
        blob->clear();
    }
    return ok;
}

bool pipelineCacheSave(const char* path, const PipelineCacheId& id, const void* data, size_t size) {
    if (!data || !size || size > (256u << 20)) {
        return false;
    }
    PipeHeader h;
    fillHeader(&h, id, size, crc32Update(0, data, size));
    std::string tmp = std::string(path) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(data, 1, size, f) == size;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp.c_str(), path) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Converter
// ---------------------------------------------------------------------------
bool spirvReadGlesCacheUids(const char* path, std::vector<ShaderUid>* uids) {
    PortHostAllocScope hostAlloc;
    uids->clear();
    FILE* in = fopen(path, "rb");
    if (!in) {
        return false;
    }
    // "PGC2": u32 magic, then per program { u32 sizeof(ShaderUid), u32 binary
    // format, u32 binary length, u64 source hash, ShaderUid, binary }.
    std::unordered_set<ShaderUid, ShaderUidHash> seen;
    uint32_t magic = 0;
    if (fread(&magic, 4, 1, in) != 1 || magic != 0x32434750) {
        fclose(in);
        return false;
    }
    for (;;) {
        uint32_t hdr[3];
        uint64_t srcHash;
        ShaderUid uid;
        if (fread(hdr, 4, 3, in) != 3 || hdr[0] != sizeof(ShaderUid) || hdr[2] > (64u << 20)) break;
        if (fread(&srcHash, 8, 1, in) != 1 || fread(&uid, sizeof(uid), 1, in) != 1 || fseek(in, (long)hdr[2], SEEK_CUR) != 0) break;
        if (seen.insert(uid).second) uids->push_back(uid);
    }
    fclose(in);
    return true;
}

bool spirvConvertFromGlesCache(const char* glesCachePath, const char* outPath, SpirvConvertStats* statsOut, int threads) {
    PortHostAllocScope hostAlloc;
    SpirvConvertStats st;
    if (statsOut) *statsOut = st;
    if (!spirvAvailable()) {
        port_log("vk: spirv convert: %s", spirvLoadError());
        return false;
    }
    std::vector<ShaderUid> uids;
    if (!spirvReadGlesCacheUids(glesCachePath, &uids)) {
        port_log("vk: spirv convert: %s is not a readable GLES program cache", glesCachePath);
        return false;
    }
    st.uids = uids.size();

    SpirvCache cache;
    cache.open(outPath);
    for (const ShaderUid& uid : uids) {
        if (cache.find(uid)) st.reused++;
    }
    SpirvCompilePool pool(&cache, threads);
    pool.start();
    for (const ShaderUid& uid : uids) {
        pool.enqueue(uid);
    }
    pool.waitIdle();
    SpirvResult r;
    while (pool.poll(&r)) {
        if (r.cached) continue;
        if (r.ok) {
            st.compiled++;
        } else {
            st.failed++;
            port_log("vk: spirv convert: uid failed: %s", r.log.substr(0, 400).c_str());
        }
    }
    pool.stop();
    port_log("vk: spirv convert: %zu programs, %zu already in %s, %zu compiled, %zu failed", st.uids, st.reused, outPath, st.compiled, st.failed);
    if (statsOut) *statsOut = st;
    return st.failed == 0;
}

}  // namespace gpu

// For platform/android/headless.c (which only knows libgame.so by dlsym):
//   petari_headless --spirv-from-gles-cache <shaders.bin> <spirv.bin>
// Returns 0 when every program compiled.
extern "C" int port_spirv_from_gles_cache(const char* glesCache, const char* spirvOut) {
    return gpu::spirvConvertFromGlesCache(glesCache, spirvOut) ? 0 : 1;
}
