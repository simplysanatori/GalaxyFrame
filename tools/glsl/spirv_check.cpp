// Checks and the converter for the Vulkan dialect shaders, outside the game:
//   spirv_check convert <shaders.bin> <spirv.bin>   seed spirv.bin from a GLES program cache
//   spirv_check dump <spirv.bin> <dir>              write <n>.vert.spv / <n>.frag.spv (for spirv-val)
//   spirv_check fixed <dir>                         compile the fixed copy shaders (VK dialect), write the .spv
//   spirv_check cutaway <shaders.bin> <dir>         the same for the VR cutaway variant of every uid
//   spirv_check vkglsl <shaders.bin> <dir>          write the VK GLSL text of every uid
//   spirv_check selftest                            cache corruption / stale / pool / pipelines.bin checks
// libgq_glsl.so is looked up beside this executable (PETARI_GQ_GLSL overrides).
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

#include "gl_shadergen.h"
#include "port/heap_routing.h"
#include "port/port.h"
#include "vk_spirv.h"

#include "smoke_vert_spv.h"

// The port layer's logging and host allocation scope, stood in for.
extern "C" {
void port_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
void port_host_alloc_scope_enter(void) {}
void port_host_alloc_scope_leave(void) {}
}

using namespace gpu;

static bool writeFile(const std::string& path, const void* data, size_t n) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static int cmdDump(const char* in, const char* dir) {
    SpirvCache cache;
    size_t loaded = 0, stale = 0;
    cache.open(in, &loaded, &stale);
    int n = 0;
    for (const ShaderUid& uid : cache.uids()) {
        auto p = cache.find(uid);
        writeFile(std::string(dir) + "/" + std::to_string(n) + ".vert.spv", p->vs.data(), p->vs.size() * 4);
        writeFile(std::string(dir) + "/" + std::to_string(n) + ".frag.spv", p->fs.data(), p->fs.size() * 4);
        n++;
    }
    fprintf(stderr, "dumped %d programs (%zu loaded, %zu stale)\n", n, loaded, stale);
    return 0;
}

static int cmdFixed(const char* dir) {
    if (!spirvAvailable()) {
        fprintf(stderr, "%s\n", spirvLoadError());
        return 1;
    }
    int bad = 0;
    for (int i = 0; i < 2; i++) {
        std::string src = i ? genCopyFragmentShader(Dialect::VK) : genCopyVertexShader(Dialect::VK);
        std::vector<uint32_t> w;
        std::string log;
        if (!spirvCompileGlsl(i ? SpirvStage::Fragment : SpirvStage::Vertex, src, &w, &log)) {
            fprintf(stderr, "copy %s failed: %s\n", i ? "fs" : "vs", log.c_str());
            bad++;
            continue;
        }
        writeFile(std::string(dir) + (i ? "/copy.frag.spv" : "/copy.vert.spv"), w.data(), w.size() * 4);
        writeFile(std::string(dir) + (i ? "/copy.frag.glsl" : "/copy.vert.glsl"), src.data(), src.size());
        std::string gles = i ? genCopyFragmentShader(Dialect::GLES) : genCopyVertexShader(Dialect::GLES);
        writeFile(std::string(dir) + (i ? "/copy.frag.gles.glsl" : "/copy.vert.gles.glsl"), gles.data(), gles.size());
        fprintf(stderr, "copy %s: %zu words\n", i ? "fs" : "vs", w.size());
    }
    // The glslc/CMake path (gq_compile_glsl) produced a SPIR-V module too.
    if (sizeof(smoke_vert_spv) < 64 || smoke_vert_spv[0] != 0x07230203u) {
        fprintf(stderr, "smoke.vert from glslc is not SPIR-V\n");
        bad++;
    } else {
        writeFile(std::string(dir) + "/smoke.vert.spv", smoke_vert_spv, sizeof(smoke_vert_spv));
        fprintf(stderr, "smoke.vert (glslc -mfmt=c): %zu words\n", sizeof(smoke_vert_spv) / 4);
    }
    return bad != 0;
}

static int cmdCutaway(const char* in, const char* dir) {
    std::vector<ShaderUid> uids;
    if (!spirvReadGlesCacheUids(in, &uids) || !spirvAvailable()) return 1;
    int bad = 0;
    for (size_t i = 0; i < uids.size(); i++) {
        ShaderUid u = uids[i];
        u.cutaway = 1;
        SpirvProgram p;
        std::string log;
        if (!spirvBuild(u, &p, nullptr, &log)) {
            fprintf(stderr, "cutaway %zu: %s\n", i, log.c_str());
            bad++;
            continue;
        }
        writeFile(std::string(dir) + "/" + std::to_string(i) + ".vert.spv", p.vs.data(), p.vs.size() * 4);
        writeFile(std::string(dir) + "/" + std::to_string(i) + ".frag.spv", p.fs.data(), p.fs.size() * 4);
    }
    fprintf(stderr, "%zu cutaway variants, %d failed\n", uids.size(), bad);
    return bad != 0;
}

static int cmdVkGlsl(const char* in, const char* dir) {
    std::vector<ShaderUid> uids;
    if (!spirvReadGlesCacheUids(in, &uids)) return 1;
    for (size_t i = 0; i < uids.size(); i++) {
        std::string vs = genVertexShader(uids[i], Dialect::VK), fs = genFragmentShader(uids[i], Dialect::VK);
        writeFile(std::string(dir) + "/" + std::to_string(i) + ".vert", vs.data(), vs.size());
        writeFile(std::string(dir) + "/" + std::to_string(i) + ".frag", fs.data(), fs.size());
    }
    fprintf(stderr, "%zu uids\n", uids.size());
    return 0;
}

#define CHECK(c)                                          \
    do {                                                  \
        if (!(c)) {                                       \
            fprintf(stderr, "selftest: FAILED %s (line %d)\n", #c, __LINE__); \
            return 1;                                     \
        }                                                 \
    } while (0)

static ShaderUid testUid(uint32_t seed) {
    ShaderUid u;
    memset(&u, 0, sizeof(u));
    u.vtxFlags = 1;
    u.numTexGens = 1;
    u.numChans = 1;
    u.numTevStages = 1;
    u.tevColor[0] = 0x0AF0A0 + seed;  // d = rasterized color, last stage
    u.tevAlpha[0] = 0x0AF0A0;
    u.alphaFunc = 0x3F | seed;
    return u;
}

static int cmdSelfTest() {
    if (!spirvAvailable()) {
        fprintf(stderr, "selftest: %s\n", spirvLoadError());
        return 1;
    }
    char dirTemplate[] = "/tmp/spirv_check_XXXXXX";
    CHECK(mkdtemp(dirTemplate));
    std::string dir = dirTemplate, path = dir + "/spirv.bin";

    // Pool + cache: compile, reload.
    {
        SpirvCache cache;
        CHECK(cache.open(path.c_str()));
        SpirvCompilePool pool(&cache, 2);
        pool.start();
        int calls = 0;
        std::mutex m;
        pool.setCallback([&](const SpirvResult&) { std::lock_guard<std::mutex> l(m); calls++; });
        for (uint32_t i = 0; i < 6; i++) CHECK(pool.enqueue(testUid(i), (int)i));
        CHECK(!pool.enqueue(testUid(0), 99));  // already known
        pool.waitIdle();
        int got = 0;
        SpirvResult r;
        while (pool.poll(&r)) {
            if (!r.ok) fprintf(stderr, "compile log: %s\n", r.log.c_str());
            CHECK(r.ok && !r.cached && r.prog && r.prog->vs.size() > 5 && r.prog->fs.size() > 5);
            got++;
        }
        CHECK(got == 6 && calls == 6 && cache.size() == 6);
        // A second round comes from the cache.
        CHECK(pool.enqueue(testUid(1)));
        pool.waitIdle();
        CHECK(pool.poll(&r) && r.ok && r.cached);
        pool.stop();
    }
    {
        SpirvCache cache;
        size_t loaded = 0, stale = 0;
        CHECK(cache.open(path.c_str(), &loaded, &stale));
        CHECK(loaded == 6 && stale == 0);
    }
    // Corrupt tail: a partial record, then a flipped byte inside the last record.
    long size = 0;
    {
        FILE* f = fopen(path.c_str(), "rb");
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fclose(f);
    }
    {
        FILE* f = fopen(path.c_str(), "ab");
        const char junk[] = "SPRCjunkjunkjunk";
        fwrite(junk, 1, sizeof(junk), f);
        fclose(f);
        SpirvCache cache;
        size_t loaded = 0;
        CHECK(cache.open(path.c_str(), &loaded));
        CHECK(loaded == 6);
        cache.close();
        f = fopen(path.c_str(), "rb");
        fseek(f, 0, SEEK_END);
        CHECK(ftell(f) == size);  // the tail was cut
        fclose(f);
    }
    {
        FILE* f = fopen(path.c_str(), "r+b");
        fseek(f, size - 100, SEEK_SET);
        int c = fgetc(f);
        fseek(f, size - 100, SEEK_SET);
        fputc(c ^ 0xFF, f);
        fclose(f);
        SpirvCache cache;
        size_t loaded = 0;
        CHECK(cache.open(path.c_str(), &loaded));
        CHECK(loaded == 5);  // the damaged last record is dropped
    }
    // A stale entry (generator changed): fake it by writing a record with a wrong hash.
    {
        SpirvCache cache;
        size_t loaded = 0, stale = 0;
        CHECK(cache.open(path.c_str(), &loaded, &stale));
        auto p = std::make_shared<SpirvProgram>();
        p->vs = {0x07230203, 1, 2, 3, 4};
        p->fs = {0x07230203, 1, 2, 3, 4};
        cache.put(testUid(40), 12345, p);
        cache.close();
        SpirvCache again;
        CHECK(again.open(path.c_str(), &loaded, &stale));
        CHECK(stale == 1 && loaded == 5 && again.staleUids().size() == 1);
        CHECK(!again.find(testUid(40)));
        SpirvCache third;
        CHECK(third.open(path.c_str(), &loaded, &stale));
        CHECK(stale == 0 && loaded == 5);  // the stale record was compacted away
    }
    // pipelines.bin
    {
        PipelineCacheId id;
        id.vendorId = 0x5143;
        id.driverVersion = 7;
        memset(id.deviceUUID, 0xAB, 16);
        std::vector<uint8_t> blob(1000), back;
        for (size_t i = 0; i < blob.size(); i++) blob[i] = (uint8_t)(i * 7);
        std::string pp = dir + "/pipelines.bin";
        CHECK(!pipelineCacheLoad(pp.c_str(), id, &back));
        CHECK(pipelineCacheSave(pp.c_str(), id, blob.data(), blob.size()));
        CHECK(pipelineCacheLoad(pp.c_str(), id, &back) && back == blob);
        PipelineCacheId other = id;
        other.driverVersion = 8;
        CHECK(!pipelineCacheLoad(pp.c_str(), other, &back));
        other = id;
        other.deviceUUID[3] ^= 1;
        CHECK(!pipelineCacheLoad(pp.c_str(), other, &back));
        FILE* f = fopen(pp.c_str(), "r+b");
        fseek(f, -10, SEEK_END);
        fputc(0x55, f);
        fclose(f);
        CHECK(!pipelineCacheLoad(pp.c_str(), id, &back));
    }
    std::string rm = "rm -rf " + dir;
    int rc = system(rm.c_str());
    (void)rc;
    fprintf(stderr, "selftest: ok\n");
    return 0;
}

int main(int argc, char** argv) {
    std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "convert" && argc == 4) {
        SpirvConvertStats st;
        bool ok = spirvConvertFromGlesCache(argv[2], argv[3], &st);
        return ok ? 0 : 1;
    }
    if (cmd == "dump" && argc == 4) return cmdDump(argv[2], argv[3]);
    if (cmd == "fixed" && argc == 3) return cmdFixed(argv[2]);
    if (cmd == "cutaway" && argc == 4) return cmdCutaway(argv[2], argv[3]);
    if (cmd == "vkglsl" && argc == 4) return cmdVkGlsl(argv[2], argv[3]);
    if (cmd == "selftest") return cmdSelfTest();
    fprintf(stderr, "usage: spirv_check convert <shaders.bin> <spirv.bin> | dump <spirv.bin> <dir> | fixed <dir> | cutaway <shaders.bin> <dir> | vkglsl <shaders.bin> <dir> | selftest\n");
    return 2;
}
