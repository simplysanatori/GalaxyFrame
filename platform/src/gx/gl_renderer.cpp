// GLES 3.2 backend of gpu::Renderer: replays prepared frames (gpu_prepare.h)
// with GL.
#include "gl_renderer.h"

#include <EGL/egl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gl_shadergen.h"
#include "gpu.h"
#include "gpu_prepare.h"
#include "port/heap_routing.h"
#include "port/port.h"
#include "tex_decode.h"

extern "C" void port_headless_write_png(const char* path, const unsigned char* rgba, int w, int h);

namespace gpu {

// Deferred GL deletes (texture images die on whichever thread drops them;
// gpu_textures.cpp keeps the queue).
static void flushDeletes() {
    drainTextureDeletes([](const TexHandle* list, size_t count, void*) { glDeleteTextures((GLsizei)count, list); }, nullptr);
}

namespace {

GLuint compileShader(GLenum type, const std::string& src) {
    GLuint s = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        port_log("gl: %s shader compile failed: %s", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        // Dump the source in chunks (logcat lines are limited).
        for (size_t i = 0; i < src.size(); i += 900) {
            port_log("gl: src: %s", src.substr(i, 900).c_str());
        }
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint linkProgram(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glProgramParameteri(p, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        port_log("gl: program link failed: %s", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

// The neutral render state of gpu_prepare.h in GL terms.
GLenum toGlPrim(Prim p) {
    static const GLenum m[] = {GL_TRIANGLES, GL_LINES, GL_POINTS};
    return m[(int)p];
}

GLenum toGlBlendFactor(BlendFactor f) {
    static const GLenum m[] = {GL_ZERO,      GL_ONE,           GL_SRC_COLOR,          GL_ONE_MINUS_SRC_COLOR,
                               GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                               GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_CONSTANT_ALPHA};
    return m[(int)f];
}

GLenum toGlBlendOp(BlendOp op) { return op == BlendOp::ReverseSubtract ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD; }

GLenum toGlCompareFunc(CompareFunc f) {
    static const GLenum m[] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    return m[(int)f];
}

GLenum toGlCullFace(CullFace f) {
    static const GLenum m[] = {GL_FRONT, GL_BACK, GL_FRONT_AND_BACK};
    return m[(int)f];
}

struct CopyTex {
    GLuint tex = 0, fbo = 0;
    int w = 0, h = 0;
    u64 frame = 0;  // frame whose replay last wrote it
};

class GlesRenderer final : public Renderer {
public:
    Api api() const override { return Api::Gles; }
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

    // The last EFB->XFB copy (written by GlImpl::doCopy).
    GLuint mXfbTex = 0;
    int mXfbWidth = 0, mXfbHeight = 0;
};

}  // namespace

struct GlImpl {
    bool ready = false;
    // Programs by shader key (0: failed to build), shared with the compile
    // thread; guarded by progLock.
    std::mutex progLock;
    std::unordered_map<ShaderUid, GLuint, ShaderUidHash> programs;
    std::unordered_set<ShaderUid, ShaderUidHash> pendingPrograms;
    std::deque<ShaderUid> compileQueue;
    std::condition_variable compileCv;
    std::thread compiler;
    FILE* cacheOut = nullptr;
    // Cached programs whose code the generator now writes differently: the
    // compile thread rebuilds them as soon as it starts (during the title
    // screen) rather than when a scene first needs them.
    std::vector<ShaderUid> staleUids;
    std::unordered_map<u32, GLuint> vaos;
    std::unordered_map<u64, GLuint> samplers;
    std::unordered_map<u64, CopyTex> copies;  // by copyKey()
    u32 copySlot = 0;                          // of the render in progress
    // Buffers of the current frame (one of frameBufs).
    GLuint vbo = 0, recVbo = 0, ibo = 0, eyeUbo = 0;
    // The per-render camera block rotates through a few buffers, so writing
    // it never waits for the GPU to finish the renders before.
    GLuint eyeUbos[8] = {};
    int eyeUboNext = 0;
    GLuint rowBuf = 0, recBuf = 0, stateBuf = 0;
    // Each frame's data goes into the next of three buffer sets, which the
    // GPU finished drawing from frames ago: refilling buffers still in use
    // made the driver wait for the GPU (up to 15 ms per frame at 120 Hz).
    struct FrameBuffers {
        GLuint buf[6] = {};    // vbo, recVbo, ibo, rowBuf, recBuf, stateBuf
        size_t cap[6] = {};    // bytes allocated
    };
    FrameBuffers frameBufs[3];
    int frameBufNext = 0;
    GLuint copyProgram = 0, copyVao = 0;
    GLint copyRectLoc = -1, copyModeLoc = -1, copyDepthLoc = -1;

    // The frame being replayed.
    std::unique_ptr<Prepared> cur;
    Builder syncBuilder;  // setFrame()

    // Background preparation (update()).
    std::thread worker;
    std::mutex workLock;
    std::condition_variable workCv;
    std::shared_ptr<const Frame> wanted;  // newest frame to build
    std::unique_ptr<Prepared> built;      // finished, not yet uploaded
    std::unique_ptr<Prepared> spare;      // recycled storage
    u64 lastRequested = 0;
    Builder workerBuilder;

    void init();
    // The program for `uid`: built now if `sync`, otherwise queued for the
    // compile thread (returns 0 until it is ready).
    GLuint program(const ShaderUid& uid, bool sync);
    GLuint buildProgram(const ShaderUid& uid);
    static void generateSources(const ShaderUid& uid, std::string* vs, std::string* fs);
    static u64 sourceHash(const ShaderUid& uid);
    void loadShaderCache();
    void storeBinary(const ShaderUid& uid, GLuint prog);
    void startCompiler();
    std::unordered_map<GLuint, GLint> ordinalLocs;
    GLint firstOrdinalLoc(GLuint prog) {
        auto it = ordinalLocs.find(prog);
        if (it != ordinalLocs.end()) return it->second;
        GLint loc = glGetUniformLocation(prog, "u_firstOrdinal");
        ordinalLocs[prog] = loc;
        return loc;
    }
    GLuint vao(u32 flags);
    GLuint sampler(u32 mode0, u32 mode1, u32 levels);
    void upload(Prepared& p, bool syncPrograms);
    void workerMain();
    void requestLatest();
    void execute(Target& efb, const EyeView* eye, Target* hud, HudMode hudMode, GlesRenderer& r);
    void doCopy(const Item& it, Target& src, GlesRenderer& r, bool vr);
    CopyTex& copyTex(u64 key, int w, int h);
    void evictCopies();
    bool snapshotTaken = false;    // the last execute() filled eye->depthSnapshot
    int hudDraws = 0;              // draws the last execute() sent to the HUD target
    bool dumpingCopies = false;    // PETARI_COPYDUMP, this replay
    u64 copyDumpFrame = ~0ull;     // last frame dumped
};

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void GlImpl::init() {
    for (FrameBuffers& fb : frameBufs) {
        glGenBuffers(6, fb.buf);
    }
    glGenBuffers(8, eyeUbos);
    for (GLuint b : eyeUbos) {
        glBindBuffer(GL_UNIFORM_BUFFER, b);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(EyeBlock), nullptr, GL_DYNAMIC_DRAW);
    }

    GLint attribs = 0, vsBlocks = 0, fsBlocks = 0, blockSize = 0;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attribs);
    glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &vsBlocks);
    glGetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, &fsBlocks);
    glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &blockSize);
    port_log("gl: %d vertex attribs, %d/%d storage blocks (vs/fs), %d MB max block", attribs, vsBlocks, fsBlocks, blockSize >> 20);
    GLint numExt = 0, numBinFmt = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &numExt);
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &numBinFmt);
    std::string exts;
    for (GLint i = 0; i < numExt; i++) {
        const char* e = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (strstr(e, "parallel") || strstr(e, "binary") || strstr(e, "multiview") || strstr(e, "buffer_storage") || strstr(e, "QCOM") || strstr(e, "foveat")) {
            exts += e;
            exts += ' ';
        }
    }
    port_log("gl: %d program binary formats; extensions: %s", numBinFmt, exts.c_str());
    if (vsBlocks < 4 || fsBlocks < 1) {
        port_log("gl: this GPU lacks the vertex attributes or storage blocks the renderer needs");
    }

    GLuint vs = compileShader(GL_VERTEX_SHADER, std::string(kCopyVsHeaderGles) + kCopyVsBody);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, std::string(kCopyFsHeaderGles) + kCopyFsBody);
    copyProgram = linkProgram(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    copyRectLoc = glGetUniformLocation(copyProgram, "u_rect");
    copyModeLoc = glGetUniformLocation(copyProgram, "u_mode");
    glGenVertexArrays(1, &copyVao);
    loadShaderCache();
    ready = true;
}

// ---------------------------------------------------------------------------
// Programs: disk cache of linked binaries + background compilation
// ---------------------------------------------------------------------------
static const u32 kCacheMagic = 0x32434750;  // "PGC2"

void GlImpl::loadShaderCache() {
    const std::string sShaderCachePath = shaderCachePath();
    if (sShaderCachePath.empty()) {
        return;
    }
    FILE* in = fopen(sShaderCachePath.c_str(), "rb");
    size_t loaded = 0, stale = 0;
    if (in) {
        u32 magic = 0;
        if (fread(&magic, 4, 1, in) == 1 && magic == kCacheMagic) {
            for (;;) {
                u32 hdr[3];  // key size, binary format, binary length
                u64 srcHash = 0;
                if (fread(hdr, 4, 3, in) != 3 || hdr[0] != sizeof(ShaderUid) || hdr[2] > (64u << 20)) break;
                ShaderUid uid;
                std::vector<u8> bin(hdr[2]);
                if (fread(&srcHash, 8, 1, in) != 1 || fread(&uid, sizeof(uid), 1, in) != 1 || fread(bin.data(), 1, bin.size(), in) != bin.size()) break;
                if (srcHash != sourceHash(uid)) {
                    stale++;
                    staleUids.push_back(uid);
                    continue;
                }
                GLuint prog = glCreateProgram();
                glProgramBinary(prog, hdr[1], bin.data(), (GLsizei)bin.size());
                GLint ok = 0;
                glGetProgramiv(prog, GL_LINK_STATUS, &ok);
                if (ok) {
                    programs[uid] = prog;
                    loaded++;
                } else {
                    glDeleteProgram(prog);
                    stale++;
                    staleUids.push_back(uid);
                }
            }
        }
        fclose(in);
    }
    // Rewrite the cache with the entries that loaded; new programs are
    // appended as they are built.
    cacheOut = fopen(sShaderCachePath.c_str(), "wb");
    if (cacheOut) {
        fwrite(&kCacheMagic, 4, 1, cacheOut);
        for (auto& kv : programs) {
            storeBinary(kv.first, kv.second);
        }
    }
    port_log("gl: shader cache %s: %zu programs loaded, %zu stale", sShaderCachePath.c_str(), loaded, stale);
}

void GlImpl::storeBinary(const ShaderUid& uid, GLuint prog) {
    if (!cacheOut || !prog) {
        return;
    }
    GLint len = 0;
    glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &len);
    if (len <= 0) {
        return;
    }
    std::vector<u8> bin((size_t)len);
    GLenum format = 0;
    GLsizei got = 0;
    glGetProgramBinary(prog, len, &got, &format, bin.data());
    u32 hdr[3] = {(u32)sizeof(ShaderUid), (u32)format, (u32)got};
    u64 srcHash = sourceHash(uid);
    fwrite(hdr, 4, 3, cacheOut);
    fwrite(&srcHash, 8, 1, cacheOut);
    fwrite(&uid, sizeof(uid), 1, cacheOut);
    fwrite(bin.data(), 1, (size_t)got, cacheOut);
    fflush(cacheOut);
}

void GlImpl::startCompiler() {
    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext share = eglGetCurrentContext();
    EGLint cfgId = 0, n = 0;
    eglQueryContext(dpy, share, EGL_CONFIG_ID, &cfgId);
    const EGLint cfgAttr[] = {EGL_CONFIG_ID, cfgId, EGL_NONE};
    EGLConfig cfg = nullptr;
    eglChooseConfig(dpy, cfgAttr, &cfg, 1, &n);
    const EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, share, ctxAttr);
    const EGLint pb[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE) {
        port_log("gl: no shared context for background compiles (0x%x); compiling inline", eglGetError());
        return;
    }
    PortHostAllocScope hostAlloc;
    compiler = std::thread([this, dpy, ctx, surf] {
        PortHostAllocScope scope;
        eglMakeCurrent(dpy, surf, surf, ctx);
        for (;;) {
            ShaderUid uid;
            {
                std::unique_lock<std::mutex> lock(progLock);
                compileCv.wait(lock, [&] { return !compileQueue.empty(); });
                uid = compileQueue.front();
                compileQueue.pop_front();
            }
            GLuint prog = buildProgram(uid);
            glFinish();  // complete before the render context uses it
            std::lock_guard<std::mutex> lock(progLock);
            programs[uid] = prog;
            pendingPrograms.erase(uid);
            storeBinary(uid, prog);
        }
    });
    std::lock_guard<std::mutex> lock(progLock);
    for (const ShaderUid& uid : staleUids) {
        if (!programs.count(uid) && pendingPrograms.insert(uid).second) {
            compileQueue.push_back(uid);
        }
    }
    if (!staleUids.empty()) {
        port_log("gl: rebuilding %zu cached programs in the background", staleUids.size());
        compileCv.notify_one();
    }
    staleUids.clear();
}

GLuint GlImpl::program(const ShaderUid& uid, bool sync) {
    {
        std::lock_guard<std::mutex> lock(progLock);
        auto it = programs.find(uid);
        if (it != programs.end()) {
            return it->second;
        }
        // Queued for the compile thread; without one it is built right here.
        if (!sync && compiler.joinable()) {
            if (pendingPrograms.insert(uid).second) {
                compileQueue.push_front(uid);  // ahead of any background rebuilds
                compileCv.notify_one();
            }
            return 0;
        }
    }
    GLuint prog = buildProgram(uid);
    std::lock_guard<std::mutex> lock(progLock);
    programs[uid] = prog;
    pendingPrograms.erase(uid);
    storeBinary(uid, prog);
    return prog;
}

void GlImpl::generateSources(const ShaderUid& uid, std::string* vs, std::string* fs) {
    static std::mutex genLock;  // the generator keeps scratch buffers
    std::lock_guard<std::mutex> lock(genLock);
    *vs = genVertexShader(uid);
    *fs = genFragmentShader(uid);
}

// Identifies the generated code, so cached binaries from an older generator
// are not reused.
u64 GlImpl::sourceHash(const ShaderUid& uid) {
    std::string vs, fs;
    generateSources(uid, &vs, &fs);
    u64 h = 1469598103934665603ull;
    for (char ch : vs) h = (h ^ (u8)ch) * 1099511628211ull;
    for (char ch : fs) h = (h ^ (u8)ch) * 1099511628211ull;
    return h;
}

GLuint GlImpl::buildProgram(const ShaderUid& uid) {
    GLuint prog = 0;
    std::string vsrc, fsrc;
    generateSources(uid, &vsrc, &fsrc);
    if (debugEnv("PETARI_GLSRC")) {
        std::string vbody = vsrc.substr(vsrc.find("layout(location"));
        for (char& ch : vbody) {
            if (ch == 10) ch = 124;
        }
        for (size_t i = 0; i < vbody.size(); i += 900) {
            port_log("glvs: %s", vbody.substr(i, 900).c_str());
        }
        size_t start = fsrc.find("void main()");
        std::string body = fsrc.substr(start == std::string::npos ? 0 : start);
        for (char& ch : body) {
            if (ch == 10) ch = 124;
        }
        for (size_t i = 0; i < body.size(); i += 900) {
            port_log("glsrc: %s", body.substr(i, 900).c_str());
        }
    }
    GLuint vs = compileShader(GL_VERTEX_SHADER, vsrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsrc);
    if (vs && fs) {
        prog = linkProgram(vs, fs);
    }
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    port_perf_programs(0, 1);
    static std::atomic<int> built{0};
    if ((++built % 50) == 0) {
        port_log("gl: %d shader programs built", built.load());
    }
    return prog;
}

GLuint GlImpl::vao(u32 flags) {
    auto it = vaos.find(flags);
    if (it != vaos.end()) {
        return it->second;
    }
    GLuint v;
    glGenVertexArrays(1, &v);
    glBindVertexArray(v);
    u32 off = 0;
    auto attr = [&](GLuint loc, GLint size, GLenum type, bool normalized, bool integer, u32 bytes) {
        glEnableVertexAttribArray(loc);
        if (integer) {
            glVertexAttribIFormat(loc, size, type, off);
        } else {
            glVertexAttribFormat(loc, size, type, normalized, off);
        }
        glVertexAttribBinding(loc, 0);
        off += bytes;
    };
    attr(0, 3, GL_FLOAT, false, false, 12);
    if (flags & VF_POSMTX) attr(1, 1, GL_UNSIGNED_INT, false, true, 4);
    if (flags & VF_NRM) attr(2, 3, GL_FLOAT, false, false, 12);
    if (flags & VF_NBT) {
        attr(3, 3, GL_FLOAT, false, false, 12);
        attr(4, 3, GL_FLOAT, false, false, 12);
    }
    if (flags & VF_CLR0) attr(5, 4, GL_UNSIGNED_BYTE, true, false, 4);
    if (flags & VF_CLR1) attr(6, 4, GL_UNSIGNED_BYTE, true, false, 4);
    for (int i = 0; i < 8; i++) {
        if (flags & (VF_TEX0 << i)) attr(7 + i, 2, GL_FLOAT, false, false, 8);
    }
    if (flags & VF_TEXMTX) attr(15, 2, GL_UNSIGNED_INT, false, true, 8);
    glBindVertexArray(0);
    vaos[flags] = v;
    return v;
}

GLuint GlImpl::sampler(u32 mode0, u32 mode1, u32 levels) {
    u64 key = ((u64)(mode0 & 0xFFFFFF) << 32) | ((mode1 & 0xFFFF) << 8) | (levels > 1 ? 1 : 0);
    auto it = samplers.find(key);
    if (it != samplers.end()) {
        return it->second;
    }
    GLuint s;
    glGenSamplers(1, &s);
    static const GLenum wrap[4] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_REPEAT};
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, wrap[mode0 & 3]);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, wrap[(mode0 >> 2) & 3]);
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, ((mode0 >> 4) & 1) ? GL_LINEAR : GL_NEAREST);
    u32 minf = (mode0 >> 5) & 7;
    GLenum min;
    bool mips = levels > 1;
    switch (minf) {
    case 0:
        min = GL_NEAREST;
        break;
    case 1:
        min = mips ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;
        break;
    case 2:
        min = mips ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST;
        break;
    case 5:
        min = mips ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR;
        break;
    case 6:
        min = mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
        break;
    default:
        min = GL_LINEAR;
        break;
    }
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, min);
    glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, (mode1 & 0xFF) / 16.0f);
    glSamplerParameterf(s, GL_TEXTURE_MAX_LOD, ((mode1 >> 8) & 0xFF) / 16.0f);
    // Anisotropic filtering on mipmapped textures (GL_EXT_texture_filter_anisotropic),
    // where the GPU has it: the Wii's mip selection by distance alone blurs
    // floors and walls seen at an angle, far more visibly at an eye's
    // resolution than on a TV.  PETARI_ANISO=<n> sets the degree (0 off).
    static const float maxAniso = [] {
        GLint n = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        bool has = false;
        for (GLint i = 0; i < n && !has; i++) {
            const char* e = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
            has = e && strcmp(e, "GL_EXT_texture_filter_anisotropic") == 0;
        }
        if (!has) return 0.0f;
        GLfloat max = 1.0f;
        glGetFloatv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT */, &max);
        const char* want = debugEnv("PETARI_ANISO");
        float aniso = want ? (float)atof(want) : 8.0f;
        aniso = aniso > max ? max : aniso;
        port_log("gl: anisotropic filtering %.0fx (GPU allows %.0fx)", aniso, max);
        return aniso;
    }();
    if (mips && maxAniso > 1.0f) {
        glSamplerParameterf(s, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY_EXT */, maxAniso);
    }
    samplers[key] = s;
    return s;
}

CopyTex& GlImpl::copyTex(u64 key, int w, int h) {
    CopyTex& c = copies[key];
    if (c.tex && (c.w != w || c.h != h)) {
        glDeleteTextures(1, &c.tex);
        glDeleteFramebuffers(1, &c.fbo);
        c.tex = c.fbo = 0;
    }
    if (!c.tex) {
        glGenTextures(1, &c.tex);
        glBindTexture(GL_TEXTURE_2D, c.tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
        glGenFramebuffers(1, &c.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, c.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c.tex, 0);
        c.w = w;
        c.h = h;
    }
    c.frame = cur->number;
    return c;
}

// Copy targets of addresses (or views) no longer copied to (kCopyKeepFrames).
void GlImpl::evictCopies() {
    for (auto it = copies.begin(); it != copies.end();) {
        if (it->first != kXfbCopyKey && it->second.frame + kCopyKeepFrames < cur->number) {
            glDeleteTextures(1, &it->second.tex);
            glDeleteFramebuffers(1, &it->second.fbo);
            it = copies.erase(it);
        } else {
            ++it;
        }
    }
}

// Render thread: uploads a built frame and resolves its programs.
void GlImpl::upload(Prepared& p, bool syncPrograms) {
    const Frame& f = *p.frame;
    FrameBuffers& fb = frameBufs[frameBufNext];
    frameBufNext = (frameBufNext + 1) % 3;
    glBindVertexArray(0);  // the element buffer binding belongs to the bound VAO
    auto put = [&fb](int i, GLenum target, const void* data, size_t bytes) {
        glBindBuffer(target, fb.buf[i]);
        if (bytes > fb.cap[i]) {
            fb.cap[i] = bytes + bytes / 4 + 4096;
            glBufferData(target, (GLsizeiptr)fb.cap[i], nullptr, GL_DYNAMIC_DRAW);
        }
        if (bytes) {
            glBufferSubData(target, 0, (GLsizeiptr)bytes, data);
        }
    };
    put(0, GL_ARRAY_BUFFER, f.verts.data(), f.verts.size() * 4);
    put(1, GL_SHADER_STORAGE_BUFFER, p.vtxRec.data(), p.vtxRec.size() * 4);
    put(2, GL_ELEMENT_ARRAY_BUFFER, p.indices.data(), p.indices.size() * 4);
    put(3, GL_SHADER_STORAGE_BUFFER, p.rows.data(), p.rows.size() * 4);
    put(4, GL_SHADER_STORAGE_BUFFER, p.recs.data(), p.recs.size() * 4);
    put(5, GL_SHADER_STORAGE_BUFFER, p.states.data(), p.states.size() * 4);
    vbo = fb.buf[0];
    recVbo = fb.buf[1];
    ibo = fb.buf[2];
    rowBuf = fb.buf[3];
    recBuf = fb.buf[4];
    stateBuf = fb.buf[5];

    // New textures.
    int64_t upStart = port_host_time_ns();
    unsigned upCount = 0;
    u64 upBytes = 0;
    for (auto& img : f.textures) {
        if (img->gpuTex && img->gpuVersion == img->version) {
            continue;
        }
        if (!img->gpuTex) {
            glGenTextures(1, &img->gpuTex);
        }
        glBindTexture(GL_TEXTURE_2D, img->gpuTex);
        ensureDecoded(*img);  // normally done while the frame was prepared
        const u32* px = img->rgba.data();
        u32 w = img->width, h = img->height;
        for (u32 l = 0; l < img->levels; l++) {
            glTexImage2D(GL_TEXTURE_2D, (GLint)l, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            px += (size_t)w * h;
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)img->levels - 1);
        img->gpuVersion = img->version;
        upCount++;
        upBytes += img->rgba.size() * 4;
        // Images never change once recorded, so the pixels are not needed
        // again (except by the PETARI_DUMP texture dump and PETARI_FRAMESAVE).
        static const bool keepPixels = debugEnv("PETARI_DUMP") != nullptr || debugEnv("PETARI_FRAMESAVE") != nullptr;
        if (!keepPixels) {
            std::vector<uint32_t>().swap(img->rgba);
        }
    }

    if (upCount) {
        port_perf_tex_upload(port_host_time_ns() - upStart, upCount, upBytes);
    }

    std::vector<GLuint> progs(p.uids.size());
    unsigned missed = 0;
    for (size_t i = 0; i < p.uids.size(); i++) {
        progs[i] = program(p.uids[i], syncPrograms);
        missed += progs[i] == 0;
    }
    if (missed) {
        port_perf_programs(missed, 0);
    }
    for (Item& it : p.items) {
        if (it.type == IT_DRAW) {
            it.program = progs[it.uid];
        }
    }
    p.cutProgs.assign(p.uids.size(), 0);
}

// Worker thread: builds the newest requested frame.
void GlImpl::workerMain() {
    port_boost_thread();  // the frame shown next waits for it
    for (;;) {
        std::shared_ptr<const Frame> frame;
        std::unique_ptr<Prepared> out;
        {
            std::unique_lock<std::mutex> lock(workLock);
            workCv.wait(lock, [&] { return wanted != nullptr; });
            frame = std::move(wanted);
            wanted.reset();
            out = spare ? std::move(spare) : std::unique_ptr<Prepared>(new Prepared());
        }
        int64_t t0 = port_host_time_ns();
        workerBuilder.build(std::move(frame), *out);
        port_perf_prepare(port_host_time_ns() - t0);
        static const bool perfLog = debugEnv("PETARI_PERFLOG") != nullptr;
        if (perfLog && out->number % 600 == 0) {
            size_t batches = 0, copies = 0;
            for (const Item& it : out->items) {
                batches += it.type == IT_DRAW;
                copies += it.type == IT_COPY;
            }
            port_log("perf: prepared frame %llu: %zu batches, %zu EFB copies, %zu shader keys, %zu indices", (unsigned long long)out->number, batches,
                     copies, out->uids.size(), out->indices.size());
        }
        std::lock_guard<std::mutex> lock(workLock);
        if (built) {
            spare = std::move(built);  // superseded before the render thread took it
        }
        built = std::move(out);
    }
}

// ---------------------------------------------------------------------------
// Execute
// ---------------------------------------------------------------------------
void GlImpl::doCopy(const Item& it, Target& src, GlesRenderer& r, bool vr) {
    CopyOp op;
    decodeCopy(it, src.width, src.height, cur->nativeW, cur->nativeH, copySlot, op);
    int sx0 = op.sx0, sy0 = op.sy0, sx1 = op.sx1, sy1 = op.sy1;

    if (op.toXfb) {
        if (!vr) {
            int dw = op.dw, dh = op.dh;
            CopyTex& xfb = copyTex(kXfbCopyKey, dw, dh);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, src.fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, xfb.fbo);
            glDisable(GL_SCISSOR_TEST);
            glBlitFramebuffer(sx0, sy0, sx1, sy1, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            r.mXfbTex = xfb.tex;
            r.mXfbWidth = dw;
            r.mXfbHeight = dh;
        }
    } else {
        int dw = op.dw, dh = op.dh;
        CopyTex& dst = copyTex(op.key, dw, dh);
        glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
        const GLenum color[1] = {GL_COLOR_ATTACHMENT0};
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, color);  // the copy covers it
        glViewport(0, 0, dw, dh);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glUseProgram(copyProgram);
        glUniform4f(copyRectLoc, sx0 / (float)src.width, sy0 / (float)src.height, sx1 / (float)src.width, sy1 / (float)src.height);
        glUniform1i(copyModeLoc, op.mode);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src.color);
        glBindSampler(0, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, src.depth);
        glBindSampler(1, 0);
        glBindVertexArray(copyVao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (dumpingCopies) {
            // PETARI_COPYDUMP=<dir>: the copies of each frame's first flat
            // replay as <dir>/copy_<frame>_<id>_fmt<f>_<w>x<h>.png (rows
            // bottom-up, so the image appears upside down).
            std::vector<u8> px((size_t)dw * dh * 4);
            glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
            char path[512];
            snprintf(path, sizeof(path), "%s/copy_%llu_%x_fmt%x_%dx%d.png", debugEnv("PETARI_COPYDUMP"), (unsigned long long)cur->number, op.id,
                     op.fmt, dw, dh);
            port_headless_write_png(path, px.data(), dw, dh);
        }
    }

    if (op.clear) {
        glBindFramebuffer(GL_FRAMEBUFFER, src.fbo);
        glEnable(GL_SCISSOR_TEST);
        glScissor(sx0, sy0, sx1 - sx0, sy1 - sy0);
        GLbitfield mask = 0;
        if (op.colorUpd || op.alphaUpd) {
            glColorMask(op.colorUpd, op.colorUpd, op.colorUpd, op.alphaUpd);
            glClearColor(op.clearColor[0], op.clearColor[1], op.clearColor[2], op.clearColor[3]);
            mask |= GL_COLOR_BUFFER_BIT;
        }
        if (op.zUpd) {
            glDepthMask(GL_TRUE);
            glClearDepthf(op.clearDepth);
            mask |= GL_DEPTH_BUFFER_BIT;
        }
        if (mask) {
            glClear(mask);
        }
    }
}

void GlImpl::execute(Target& efb, const EyeView* eye, Target* hud, HudMode hudMode, GlesRenderer& r) {
    const std::vector<Item>& items = cur->items;
    const std::vector<std::shared_ptr<TexImage>>& textures = cur->frame->textures;
    u32 nativeW = cur->nativeW, nativeH = cur->nativeH;
    u32 clearAR = cur->clearAR, clearGB = cur->clearGB, clearZ = cur->clearZ;
    if (hudMode == HudMode::Target && !hud) {
        hudMode = HudMode::Inline;
    }
    // Per-eye block.
    EyeBlock eb;
    fillEyeBlock(eye, eb);
    // A VR eye, as opposed to a flat replay or one of its stereo pair.
    bool vrEye = eye && !eye->flatStereo;
    eyeUbo = eyeUbos[eyeUboNext];
    eyeUboNext = (eyeUboNext + 1) % 8;
    glBindBuffer(GL_UNIFORM_BUFFER, eyeUbo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(eb), &eb);
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, eyeUbo);
    // With the cutaway on, 3D draws use their cutaway variants, compiled in
    // the background on first use (a draw keeps its plain program until
    // then).  The variants are only used while needed: their discard costs
    // the GPU its early depth rejection.
    bool cutActive = vrEye && eye->focus[3] > 0.0f;
    std::vector<u32>& cutProgs = this->cur->cutProgs;
    copySlot = copySlotFor(eye);
    // PETARI_COPYDUMP_EYE=1: the left eye's copies instead of the flat replay's.
    static const bool sDumpEye = debugEnv("PETARI_COPYDUMP_EYE") != nullptr;
    dumpingCopies = (sDumpEye ? (eye && eye->index == 0) : !eye) && debugEnv("PETARI_COPYDUMP") && copyDumpFrame != this->cur->number;
    if (dumpingCopies) {
        copyDumpFrame = this->cur->number;
    }
    if (cutActive) {
        const std::vector<ShaderUid>& frameUids = this->cur->uids;
        for (size_t i = 0; i < frameUids.size() && i < cutProgs.size(); i++) {
            if (!cutProgs[i] && !frameUids[i].hud) {
                ShaderUid u = frameUids[i];
                u.cutaway = 1;
                cutProgs[i] = program(u, false);
            }
        }
    }

    // Start of frame: the EFB is cleared (the previous frame's display copy
    // cleared it on the console).
    auto clearTarget = [&](Target& t, bool transparent) {
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        if (transparent) {
            glClearColor(0, 0, 0, 0);
        } else {
            glClearColor((clearAR & 255) / 255.0f, ((clearGB >> 8) & 255) / 255.0f, (clearGB & 255) / 255.0f, ((clearAR >> 8) & 255) / 255.0f);
        }
        glClearDepthf(clearZ / 16777215.0f);
        glStencilMask(0xFF);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    };
    clearTarget(efb, false);
    if (hudMode == HudMode::Target) {
        clearTarget(*hud, true);
    }
    bool inHudSection = false;
    // SpaceWarp: the player's draws tag the stencil buffer (see
    // EyeView::depthSnapshot); the stencil test itself always passes.
    bool tagPlayer = eye && eye->depthSnapshot;
    snapshotTaken = false;
    hudDraws = 0;
    if (tagPlayer) {
        glEnable(GL_STENCIL_TEST);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    }

    Target* cur = &efb;
    glBindFramebuffer(GL_FRAMEBUFFER, cur->fbo);
    GLuint lastProgram = ~0u, lastVao = ~0u;
    GLint ordinalLoc = -1;
    // GL state the batches so far left: only changes go to the driver (a
    // frame has thousands of batches, most with the state of the one
    // before).  All bits set = not known.
    struct {
        int vp[4];
        int scissorOn;
        int sc[4];
        int colorMask;
        int blendOn;
        GLenum blend[6];  // source / destination RGB and alpha, equations
        float blendAlpha;
        int depthOn, depthMask;
        GLenum depthFunc;
        int cullOn;
        GLenum cullFace;
        int stencilMask, stencilRef;
        GLuint tex[8], samp[8];
        int unit;
        void forget() { memset(this, 0xFF, sizeof(*this)); }
    } gs;
    gs.forget();
    auto setViewport = [&gs](int x, int y, int w, int h) {
        if (gs.vp[0] != x || gs.vp[1] != y || gs.vp[2] != w || gs.vp[3] != h) {
            glViewport(x, y, w, h);
            gs.vp[0] = x, gs.vp[1] = y, gs.vp[2] = w, gs.vp[3] = h;
        }
    };
    auto setScissor = [&gs](bool on, int x, int y, int w, int h) {
        if (gs.scissorOn != (int)on) {
            on ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
            gs.scissorOn = on;
        }
        if (on && (gs.sc[0] != x || gs.sc[1] != y || gs.sc[2] != w || gs.sc[3] != h)) {
            glScissor(x, y, w, h);
            gs.sc[0] = x, gs.sc[1] = y, gs.sc[2] = w, gs.sc[3] = h;
        }
    };
    auto setColorMask = [&gs](int m) {
        if (gs.colorMask != m) {
            glColorMask(m & 1, (m >> 1) & 1, (m >> 2) & 1, (m >> 3) & 1);
            gs.colorMask = m;
        }
    };
    auto setBlend = [&gs](bool on, GLenum srcRGB, GLenum dstRGB, GLenum srcA, GLenum dstA, GLenum eqRGB, GLenum eqA, float constAlpha) {
        if (gs.blendOn != (int)on) {
            on ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
            gs.blendOn = on;
        }
        if (!on) {
            return;
        }
        if (gs.blend[0] != srcRGB || gs.blend[1] != dstRGB || gs.blend[2] != srcA || gs.blend[3] != dstA) {
            glBlendFuncSeparate(srcRGB, dstRGB, srcA, dstA);
            gs.blend[0] = srcRGB, gs.blend[1] = dstRGB, gs.blend[2] = srcA, gs.blend[3] = dstA;
        }
        if (gs.blend[4] != eqRGB || gs.blend[5] != eqA) {
            glBlendEquationSeparate(eqRGB, eqA);
            gs.blend[4] = eqRGB, gs.blend[5] = eqA;
        }
        bool usesConst = srcA == GL_CONSTANT_ALPHA || dstA == GL_CONSTANT_ALPHA;
        if (usesConst && !(gs.blendAlpha == constAlpha)) {
            glBlendColor(0, 0, 0, constAlpha);
            gs.blendAlpha = constAlpha;
        }
    };
    auto setDepth = [&gs](bool on, GLenum func, bool write) {
        if (gs.depthOn != (int)on) {
            on ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
            gs.depthOn = on;
        }
        if (on && gs.depthFunc != func) {
            glDepthFunc(func);
            gs.depthFunc = func;
        }
        if (gs.depthMask != (int)write) {
            glDepthMask(write);
            gs.depthMask = write;
        }
    };
    auto setCull = [&gs](bool on, GLenum face) {
        if (gs.cullOn != (int)on) {
            on ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
            gs.cullOn = on;
        }
        if (on && gs.cullFace != face) {
            glCullFace(face);
            gs.cullFace = face;
        }
    };
    auto bindTexture = [&gs](int u, GLuint tex, GLuint samp) {
        if (gs.tex[u] != tex) {
            if (gs.unit != u) {
                glActiveTexture(GL_TEXTURE0 + u);
                gs.unit = u;
            }
            glBindTexture(GL_TEXTURE_2D, tex);
            gs.tex[u] = tex;
        }
        if (gs.samp[u] != samp) {
            glBindSampler(u, samp);
            gs.samp[u] = samp;
        }
    };
    // Where a draw writes depth, the stencil gets `ref` (1 for the player's
    // draws); elsewhere it keeps what is there.
    auto setStencil = [&gs](bool write, int ref) {
        int mask = write ? 0xFF : 0;
        if (gs.stencilMask != mask) {
            glStencilMask(mask);
            gs.stencilMask = mask;
        }
        if (write && gs.stencilRef != ref) {
            glStencilFunc(GL_ALWAYS, ref, 0xFF);
            gs.stencilRef = ref;
        }
    };
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, eyeUbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, rowBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, recBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, stateBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, recVbo);

    int stopAfter = gDebugStopAfterDraws;
    for (const Item& it : items) {
        if (stopAfter >= 0 && it.type == IT_DRAW) {
            if (stopAfter < (int)it.draws) {
                break;
            }
            stopAfter -= (int)it.draws;
        }
        if (it.type == IT_MARKER) {
            if (it.marker == MARK_SCENE_DEPTH && tagPlayer && cur == &efb) {
                // The scene's depth and the player tags, at the snapshot's
                // size (nearest sample).
                Target& snap = *eye->depthSnapshot;
                glBindFramebuffer(GL_READ_FRAMEBUFFER, efb.fbo);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, snap.fbo);
                glDisable(GL_SCISSOR_TEST);
                glBlitFramebuffer(0, 0, efb.width, efb.height, 0, 0, snap.width, snap.height, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                                  GL_NEAREST);
                glBindFramebuffer(GL_FRAMEBUFFER, cur->fbo);
                gs.forget();
                snapshotTaken = true;
                continue;
            }
            if (it.marker != MARK_HUD_BEGIN && it.marker != MARK_HUD_END) {
                continue;
            }
            inHudSection = it.marker == MARK_HUD_BEGIN;
            if (hudMode == HudMode::Target) {
                cur = inHudSection ? hud : &efb;
                glBindFramebuffer(GL_FRAMEBUFFER, cur->fbo);
            }
            continue;
        }
        if (inHudSection && hudMode == HudMode::Skip) {
            continue;
        }
        bool toHudTarget = inHudSection && hudMode == HudMode::Target;
        if (it.type == IT_COPY) {
            static const bool noCopies = debugEnv("PETARI_NOCOPY") != nullptr;  // perf experiments
            if (noCopies && !((it.copy[9] >> 14) & 1)) continue;
            doCopy(it, *cur, r, eye != nullptr);
            glBindFramebuffer(GL_FRAMEBUFFER, cur->fbo);
            lastProgram = ~0u;
            lastVao = ~0u;
            gs.forget();
            continue;
        }
        if (!it.program || !it.idxCount) {
            continue;
        }
        Target& t = *cur;
        DrawRect rect;
        drawRect(it, t.width, t.height, nativeW, nativeH, eye, vrEye, rect);
        setViewport(rect.vp[0], rect.vp[1], rect.vp[2], rect.vp[3]);
        setScissor(rect.scissor, rect.sc[0], rect.sc[1], rect.sc[2], rect.sc[3]);

        if (toHudTarget) {
            hudDraws++;
        }
        BlendState bs;
        blendState(it, toHudTarget, bs);
        setColorMask(bs.colorMask);
        if (bs.on) {
            setBlend(true, toGlBlendFactor(bs.srcRGB), toGlBlendFactor(bs.dstRGB), toGlBlendFactor(bs.srcA), toGlBlendFactor(bs.dstA),
                     toGlBlendOp(bs.eqRGB), toGlBlendOp(bs.eqA), bs.constAlpha);
        } else {
            setBlend(false, 0, 0, 0, 0, 0, 0, 0.0f);
        }
        setDepth(it.depthTest, toGlCompareFunc(it.depthFunc), it.depthTest && it.depthWrite);
        setCull(it.cull, toGlCullFace(it.cullFace));
        if (tagPlayer) {
            setStencil(!toHudTarget && it.depthTest && it.depthWrite, it.player ? 1 : 0);
        }

        GLuint prog = it.program;
        if (cutActive && it.useVr && it.uid < cutProgs.size() && cutProgs[it.uid]) {
            prog = cutProgs[it.uid];
        }
        if (prog != lastProgram) {
            glUseProgram(prog);
            lastProgram = prog;
            ordinalLoc = firstOrdinalLoc(prog);
        }
        glUniform1ui(ordinalLoc, it.firstOrdinal);
        for (int u = 0; u < 8; u++) {
            u32 tr = it.tex[u];
            if (!(it.texMask & (1u << u)) || tr == ~0u) {
                continue;
            }
            if (tr & 0x80000000u) {
                auto cit = copies.find(copyKey(copySlot, (tr & 0x7FFFFFFF) << 5));
                bindTexture(u, cit != copies.end() ? cit->second.tex : 0, sampler(it.texMode0[u], it.texMode1[u], 1));
            } else if (tr < textures.size()) {
                bindTexture(u, textures[tr]->gpuTex, sampler(it.texMode0[u], it.texMode1[u], textures[tr]->levels));
            }
        }

        GLuint v = vao(it.flags);
        if (v != lastVao) {
            glBindVertexArray(v);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
            lastVao = v;
        }
        glBindVertexBuffer(0, vbo, (GLintptr)it.firstWord * 4, (GLsizei)(vtxWords(it.flags) * 4));
        glDrawElements(toGlPrim(it.prim), (GLsizei)it.idxCount, GL_UNSIGNED_INT, (const void*)((uintptr_t)it.idxOffset * 4));
    }
    glBindVertexArray(0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    if (tagPlayer) {
        glDisable(GL_STENCIL_TEST);
    }
    glStencilMask(0xFF);
    // Nothing reads the depth buffers after the frame (the next frame starts
    // by clearing them; SpaceWarp's snapshot was taken on the way): the
    // tiler need not write them back to memory.
    const GLenum depthOnly[2] = {GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
    glBindFramebuffer(GL_FRAMEBUFFER, efb.fbo);
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, depthOnly);
    if (hudMode == HudMode::Target) {
        glBindFramebuffer(GL_FRAMEBUFFER, hud->fbo);
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, depthOnly);
    }
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------
static GlImpl* sImpl;

bool GlesRenderer::init() {
    if (!sImpl) {
        sImpl = new GlImpl();
        sImpl->init();
    }
    return sImpl->ready;
}

Target GlesRenderer::createTarget(int width, int height) {
    Target t;
    t.width = width;
    t.height = height;
    glGenTextures(1, &t.color);
    glBindTexture(GL_TEXTURE_2D, t.color);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Depth with stencil: the stencil tags the player's pixels for SpaceWarp.
    glGenTextures(1, &t.depth);
    glBindTexture(GL_TEXTURE_2D, t.depth);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, width, height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, t.depth, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        port_log("gl: EFB target %dx%d incomplete (0x%x)", width, height, st);
    }
    return t;
}

Target GlesRenderer::createDepthTarget(int width, int height) {
    Target t;
    t.width = width;
    t.height = height;
    glGenTextures(1, &t.depth);
    glBindTexture(GL_TEXTURE_2D, t.depth);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, width, height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, t.depth, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        port_log("gl: depth target %dx%d incomplete (0x%x)", width, height, st);
    }
    return t;
}

bool GlesRenderer::setFoveation(Target& t, float focalX, float focalY, float gain, float foveaArea) {
    typedef void (*FoveationParams)(GLuint, GLuint, GLuint, GLfloat, GLfloat, GLfloat, GLfloat, GLfloat);
    static FoveationParams params = (FoveationParams)eglGetProcAddress("glTextureFoveationParametersQCOM");
    if (!params || !t.color) {
        return false;
    }
    const GLenum kFeatureBits = 0x8BFB;   // GL_TEXTURE_FOVEATED_FEATURE_BITS_QCOM
    const GLint kEnableBits = 0x1 | 0x2;  // FOVEATION_ENABLE | FOVEATION_SCALED_BIN_METHOD
    glBindTexture(GL_TEXTURE_2D, t.color);
    glTexParameteri(GL_TEXTURE_2D, kFeatureBits, kEnableBits);
    params(t.color, 0, 0, focalX, focalY, gain, gain, foveaArea);
    return glGetError() == GL_NO_ERROR;
}

void GlesRenderer::destroyTarget(Target& t) {
    glDeleteFramebuffers(1, &t.fbo);
    glDeleteTextures(1, &t.color);
    glDeleteTextures(1, &t.depth);
    t = Target();
}

void GlesRenderer::setFrame(std::shared_ptr<const Frame> frame) {
    flushDeletes();
    if (!frame || !sImpl) {
        return;
    }
    GlImpl& im = *sImpl;
    if (im.cur && im.cur->number == frame->number) {
        return;
    }
    std::unique_ptr<Prepared> p = im.spare ? std::move(im.spare) : std::unique_ptr<Prepared>(new Prepared());
    im.syncBuilder.build(std::move(frame), *p);
    im.upload(*p, true);
    im.spare = std::move(im.cur);
    im.cur = std::move(p);
    im.evictCopies();
}

// Hands the newest recorded frame to the worker if it has not had it yet.
void GlImpl::requestLatest() {
    std::lock_guard<std::mutex> lock(workLock);
    auto latest = frameQueue().latest();
    if (latest && latest->number != lastRequested) {
        lastRequested = latest->number;
        wanted = std::move(latest);
        workCv.notify_one();
    }
}

// Game thread, as each frame is presented: preparation starts right away,
// so the frame is ready by the renderer's next refresh.
static void onFramePublished() {
    if (sImpl) {
        sImpl->requestLatest();
    }
}

bool GlesRenderer::update() {
    flushDeletes();
    if (!sImpl) {
        return false;
    }
    GlImpl& im = *sImpl;
    if (!im.worker.joinable()) {
        im.startCompiler();
        PortHostAllocScope hostAlloc;
        im.worker = std::thread([&im] {
            PortHostAllocScope scope;
            im.workerMain();
        });
        frameQueue().setListener(onFramePublished);
    }
    im.requestLatest();
    std::unique_ptr<Prepared> done;
    {
        std::lock_guard<std::mutex> lock(im.workLock);
        done = std::move(im.built);
    }
    if (!done) {
        return false;
    }
    im.upload(*done, false);
    std::unique_ptr<Prepared> old = std::move(im.cur);
    im.cur = std::move(done);
    im.evictCopies();
    if (old) {
        old->frame.reset();  // let the recording's memory go
        std::lock_guard<std::mutex> lock(im.workLock);
        if (!im.spare) im.spare = std::move(old);
    }
    return true;
}

bool GlesRenderer::hasFrame() const { return sImpl && sImpl->cur; }

uint64_t GlesRenderer::frameNumber() const { return hasFrame() ? sImpl->cur->number : 0; }

const CameraInfo& GlesRenderer::camera() const {
    static const CameraInfo none{};
    return hasFrame() ? sImpl->cur->cam : none;
}

void GlesRenderer::render(Target& efb, const EyeView* eye, Target* hud, HudMode hudMode) {
    if (!hasFrame()) {
        if (sImpl) {
            sImpl->snapshotTaken = false;
            sImpl->hudDraws = 0;
        }
        return;
    }
    sImpl->execute(efb, eye, hud, hudMode, *this);
}

bool GlesRenderer::snapshotTaken() const { return sImpl && sImpl->snapshotTaken; }
int GlesRenderer::hudDrawCount() const { return sImpl ? sImpl->hudDraws : 0; }

void GlesRenderer::finish() { glFinish(); }

bool GlesRenderer::readPixels(const Target& t, uint8_t* rgba) {
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glReadPixels(0, 0, t.width, t.height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return true;
}

Renderer* createGlesRenderer() { return new GlesRenderer(); }

}  // namespace gpu
