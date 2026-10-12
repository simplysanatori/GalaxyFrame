// Offscreen rendering for bring-up: renders the latest recorded frame with
// the renderer in use (GLES: in a pbuffer-backed context) and writes it as a
// PNG.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "gpu_renderer.h"
#include "../xr/vr_rig.h"
#include "gpu.h"
#include "port/port.h"

namespace {

EGLDisplay sDisplay = EGL_NO_DISPLAY;
EGLContext sContext = EGL_NO_CONTEXT;
EGLSurface sSurface = EGL_NO_SURFACE;
gpu::Target sTarget;
bool sStarted = false;  // a backend without a context of ours (Vulkan) was initialized

// A GLES 3.2 context for the GLES backend.
bool initEgl() {
    sDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(sDisplay, nullptr, nullptr)) {
        port_log("headless: eglInitialize failed");
        return false;
    }
    const EGLint cfgAttr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                              EGL_BLUE_SIZE,       8,                  EGL_ALPHA_SIZE,   8,               EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(sDisplay, cfgAttr, &cfg, 1, &n) || n == 0) {
        port_log("headless: no EGL config");
        return false;
    }
    const EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_CONTEXT_OPENGL_DEBUG, EGL_TRUE, EGL_NONE};
    sContext = eglCreateContext(sDisplay, cfg, EGL_NO_CONTEXT, ctxAttr);
    const EGLint pbAttr[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    sSurface = eglCreatePbufferSurface(sDisplay, cfg, pbAttr);
    if (sContext == EGL_NO_CONTEXT || !eglMakeCurrent(sDisplay, sSurface, sSurface, sContext)) {
        port_log("headless: cannot create/make current a GLES 3.2 context (0x%x)", eglGetError());
        return false;
    }
    port_log("headless: GL %s / %s", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(
        [](GLenum, GLenum type, GLuint, GLenum severity, GLsizei, const GLchar* msg, const void*) {
            if (severity != GL_DEBUG_SEVERITY_NOTIFICATION) {
                static int count;
                if (count++ < 50) port_log("gl debug (type 0x%x sev 0x%x): %s", type, severity, msg);
            }
        },
        nullptr);
    return true;
}

bool initContext() {
    gpu::Renderer& r = gpu::renderer();
    if (r.api() == gpu::Api::Gles) {
        if (sContext != EGL_NO_CONTEXT) {
            return true;
        }
        if (!initEgl()) {
            return false;
        }
    } else {
        if (sStarted) {
            return true;
        }
        sStarted = true;
    }
    gpu::setShaderCachePath(getenv("PETARI_SHADER_CACHE") ? getenv("PETARI_SHADER_CACHE") : "/data/local/tmp/petari/shaders.bin");
    return r.init();
}

// Minimal PNG writer (stored deflate blocks).
unsigned crcTable[256];
void initCrc() {
    for (unsigned n = 0; n < 256; n++) {
        unsigned c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crcTable[n] = c;
    }
}
unsigned crc(const unsigned char* p, size_t n, unsigned c = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; i++) c = crcTable[(c ^ p[i]) & 255] ^ (c >> 8);
    return c;
}
void put32(std::vector<unsigned char>& v, unsigned x) {
    v.push_back(x >> 24);
    v.push_back(x >> 16);
    v.push_back(x >> 8);
    v.push_back(x);
}
void chunk(FILE* f, const char* type, const std::vector<unsigned char>& data) {
    std::vector<unsigned char> buf;
    put32(buf, (unsigned)data.size());
    buf.insert(buf.end(), type, type + 4);
    buf.insert(buf.end(), data.begin(), data.end());
    unsigned c = crc(buf.data() + 4, buf.size() - 4) ^ 0xFFFFFFFFu;
    put32(buf, c);
    fwrite(buf.data(), 1, buf.size(), f);
}

bool writePng(const char* path, const unsigned char* rgba, int w, int h) {
    static bool crcReady;
    if (!crcReady) {
        initCrc();
        crcReady = true;
    }
    FILE* f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    std::vector<unsigned char> ihdr;
    put32(ihdr, w);
    put32(ihdr, h);
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(6);  // RGBA
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    chunk(f, "IHDR", ihdr);

    // Raw scanlines (filter 0), rows flipped (GL origin is bottom-left).
    std::vector<unsigned char> raw;
    raw.reserve((size_t)(w * 4 + 1) * h);
    for (int y = h - 1; y >= 0; y--) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + (size_t)y * w * 4, rgba + (size_t)(y + 1) * w * 4);
    }
    std::vector<unsigned char> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t n = raw.size() - pos;
        if (n > 65535) n = 65535;
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back(n & 255);
        z.push_back(n >> 8);
        z.push_back(~n & 255);
        z.push_back((~n >> 8) & 255);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    unsigned a = 1, b = 0;
    for (unsigned char c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    put32(z, (b << 16) | a);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    fclose(f);
    return true;
}

}  // namespace

// Logs a summary of a recorded frame (draws, state, first vertices) and
// writes its textures next to `dir`.
extern "C" void port_headless_write_png(const char* path, const unsigned char* rgba, int w, int h) { writePng(path, rgba, w, h); }
extern "C" void port_headless_xrsim(const char* path, int frames, float yawDeg, float pitchDeg);  // xr/xr_sim.cpp

static void dumpFrame(const gpu::Frame& f, const char* dir) {
    using namespace gpu;
    u32 bp[256], xf[256], treg[8], tk[8];
    memcpy(treg, f.startTevReg, sizeof(treg));
    memcpy(tk, f.startTevKonst, sizeof(tk));
    memcpy(bp, f.startBp, sizeof(bp));
    memcpy(xf, f.startXfRegs, sizeof(xf));
    const u32* c = f.cmds.data();
    const u32* end = c + f.cmds.size();
    int draws = 0;
    while (c < end) {
        u32 op = *c++;
        switch (op) {
        case CMD_XF: {
            u32 addr = c[0], count = c[1];
            for (u32 i = 0; i < count; i++) {
                if (addr + i >= 0x1000 && addr + i < 0x1100) xf[addr + i - 0x1000] = c[2 + i];
            }
            c += 2 + count;
            break;
        }
        case CMD_BP:
            bp[*c >> 24] = *c & 0xFFFFFF;
            trackTevColorWrite(treg, tk, *c >> 24, *c & 0xFFFFFF);
            if (draws < 3) {
                port_log("dump:   bp %02x = %06x", *c >> 24, *c & 0xFFFFFF);
            }
            c++;
            break;
        case CMD_TEX:
            port_log("dump:   tex unit %u <- image %u", c[0], c[1]);
            c += 2;
            break;
        case CMD_TEX_EFB:
            port_log("dump:   tex unit %u <- efb copy %u (%ux%u at %08x)", c[0], c[1], c[2], c[3], c[4]);
            c += 5;
            break;
        case CMD_EFB_COPY:
            port_log("dump:   efb copy id %u src %06x/%06x dst %06x ctrl %06x clear %06x %06x", c[0], c[1], c[2], c[3], c[9], c[6], c[7]);
            c += 10;
            break;
        case CMD_MARKER:
            port_log("dump:   marker %u", *c++);
            break;
        case CMD_DRAW: {
            u32 prim = c[0], flags = c[1], first = c[2], count = c[3];
            c += 4;
            if (draws++ < 40) {
                float p[3];
                memcpy(p, &f.verts[first], 12);
                port_log("dump: draw %d prim %02x flags %05x count %u v0 (%.2f %.2f %.2f) tev %u gen %06x cc0 %06x ac0 %06x order %06x proj %u "
                         "cmode %06x zmode %06x numtex %u",
                         draws - 1, prim, flags, count, p[0], p[1], p[2], ((bp[0] >> 10) & 15) + 1, bp[0], bp[0xC0], bp[0xC1], bp[0x28],
                         xf[0x26] & 1, bp[0x41], bp[0x40], xf[0x3F]);
                u32 words = vtxWords(flags);
                for (u32 v = 0; v < count && v < 4; v++) {
                    char line[512];
                    int n = 0;
                    for (u32 w = 0; w < words && w < 16 && n < 480; w++) {
                        u32 x = f.verts[first + v * words + w];
                        float fx;
                        memcpy(&fx, &x, 4);
                        n += snprintf(line + n, sizeof(line) - n, " %08x(%.3g)", x, fx);
                    }
                    port_log("dump:   v%u:%s", v, line);
                }
                float pr[6], vpv[6];
                for (int i = 0; i < 6; i++) {
                    memcpy(&pr[i], &xf[0x20 + i], 4);
                    memcpy(&vpv[i], &xf[0x1A + i], 4);
                }
                port_log("dump:   texgen %05x %05x %05x post %03x %03x dual %u mtxidx %08x %08x chans %u cc %04x %04x ac %04x", xf[0x40], xf[0x41], xf[0x42],
                         xf[0x50], xf[0x51], xf[0x12], xf[0x18], xf[0x19], xf[0x09], xf[0x0E], xf[0x0F], xf[0x10]);
                port_log("dump:   tevreg %06x %06x | %06x %06x | %06x %06x | %06x %06x  konst %06x %06x %06x %06x", treg[0], treg[1],
                         treg[2], treg[3], treg[4], treg[5], treg[6], treg[7], tk[0], tk[1], tk[2], tk[3]);
                port_log("dump:   proj %g %g %g %g %g %g  vp %g %g %g %g %g %g  mtxidx %06x", pr[0], pr[1], pr[2], pr[3], pr[4], pr[5], vpv[0],
                         vpv[1], vpv[2], vpv[3], vpv[4], vpv[5], xf[0x18]);
            }
            break;
        }
        default:
            c = end;
            break;
        }
    }
    // PETARI_DUMP=<n>: the first n textures (default 16).
    size_t maxTextures = atoi(getenv("PETARI_DUMP") ? getenv("PETARI_DUMP") : "") > 1 ? (size_t)atoi(getenv("PETARI_DUMP")) : 16;
    for (size_t i = 0; i < f.textures.size() && i < maxTextures; i++) {
        const TexImage& t = *f.textures[i];
        if (t.rgba.size() < (size_t)t.width * t.height) {
            continue;  // pixels already released after upload
        }
        char path[512];
        snprintf(path, sizeof(path), "%s_tex%02zu.png", dir, i);
        // PNG rows are written bottom-up; flip into a temporary.
        std::vector<unsigned char> px((size_t)t.width * t.height * 4);
        for (u32 y = 0; y < t.height; y++) {
            memcpy(&px[(size_t)(t.height - 1 - y) * t.width * 4], &t.rgba[(size_t)y * t.width], t.width * 4);
        }
        writePng(path, px.data(), (int)t.width, (int)t.height);
        port_log("dump: texture %zu: %ux%u levels %u -> %s", i, t.width, t.height, t.levels, path);
    }
}

extern "C" void port_headless_dump(const char* prefix) {
    auto frame = gpu::frameQueue().latest();
    if (frame) {
        dumpFrame(*frame, prefix);
    }
}

// The VR camera of PETARI_VRTEST for a w x h eye: through the VR rig, from
// a head at the stage origin (PETARI_VRHEAD="x,y,z,yawDeg,pitchDeg").
// Returns the rig's stage <- view transform.
static xm::Mat4 testEye(gpu::Renderer& r, int w, int h, gpu::EyeView& ev) {
    vr::RigState rig;
    vr::RigParams params;
    xm::Mat4 stageFromView = vr::updateRig(rig, params, r.camera(), 0.0f);
    float hx = 0, hy = 0, hz = 0, yaw = 0, pitch = 0;
    if (const char* hp = gpu::debugEnv("PETARI_VRHEAD")) {
        sscanf(hp, "%f,%f,%f,%f,%f", &hx, &hy, &hz, &yaw, &pitch);
    }
    float cy = cosf(yaw * 0.5f * 3.14159265f / 180.0f), sy = sinf(yaw * 0.5f * 3.14159265f / 180.0f);
    float cp = cosf(pitch * 0.5f * 3.14159265f / 180.0f), sp = sinf(pitch * 0.5f * 3.14159265f / 180.0f);
    xm::Quat qy{0, sy, 0, cy}, qp{sp, 0, 0, cp};
    xm::Quat q{qy.w * qp.x, qy.y * qp.w, -qy.y * qp.x, qy.w * qp.w};  // yaw * pitch
    xm::Mat4 eyeFromStage = xm::inversePose(q, {hx, hy, hz});
    float tv = (float)h / (float)w;
    xm::Mat4 proj = xm::projectionFov(-0.7853982f, 0.7853982f, atanf(tv), -atanf(tv), 0.05f, 1000.0f);
    vr::toRowMajor(eyeFromStage * stageFromView, ev.view);
    vr::toRowMajor(proj, ev.proj);
    bool cutTest = gpu::debugEnv("PETARI_CUTTEST") != nullptr;
    vr::setCutaway(ev, eyeFromStage * stageFromView, r.camera(), cutTest || (r.camera().flags & PORT_GX_CAMERA_OCCLUDED) ? 1.0f : 0.0f,
                   cutTest ? 3.0f : 1.0f);
    return stageFromView;
}

// Renders `frame` at `scale` x the native EFB size and writes `path` (PNG).
static unsigned long long renderShots(std::shared_ptr<const gpu::Frame> frame, const char* path, int scale) {
    gpu::Renderer& r = gpu::renderer();
    auto msSince = [](const timespec& a) {
        timespec b;
        clock_gettime(CLOCK_MONOTONIC, &b);
        return (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
    };
    timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (gpu::debugEnv("PETARI_ASYNC")) {
        // Exercise the worker-thread path the headset uses.
        for (int i = 0; i < 200 && !r.update(); i++) {
            usleep(5000);
        }
    } else {
        r.setFrame(frame);
    }
    r.finish();
    double prepMs = msSince(t0);
    int w = 640 * scale, h = 456 * scale;
    if (sTarget.width != w || sTarget.height != h) {
        if (sTarget.fbo) {
            r.destroyTarget(sTarget);
        }
        sTarget = r.createTarget(w, h);
    }
    // PETARI_GLSTEPS=a,b,...: also write <path>_s<N>.png stopped after N draws.
    if (const char* steps = gpu::debugEnv("PETARI_GLSTEPS")) {
        std::vector<unsigned char> sp((size_t)w * h * 4);
        for (const char* p = steps; *p;) {
            int n = atoi(p);
            gpu::gDebugStopAfterDraws = n;
            r.render(sTarget, nullptr, nullptr);
            r.readPixels(sTarget, sp.data());
            for (size_t i = 3; i < sp.size(); i += 4) sp[i] = 255;
            std::string sp_path = std::string(path).substr(0, strlen(path) - 4) + "_s" + std::to_string(n) + ".png";
            writePng(sp_path.c_str(), sp.data(), w, h);
            while (*p && *p != ',') p++;
            if (*p == ',') p++;
        }
        gpu::gDebugStopAfterDraws = -1;
    }
    // PETARI_VRTEST: also render <path>_vr.png through the VR rig, from a
    // head at the stage origin (PETARI_VRHEAD="x,y,z,yawDeg,pitchDeg").
    if (gpu::debugEnv("PETARI_VRTEST") && r.camera().valid) {
        gpu::EyeView ev;
        xm::Mat4 stageFromView = testEye(r, w, h, ev);
        // PETARI_VRSIZE=WxH[,WxH...]: time the eye render at headset
        // resolutions; PETARI_FOVEATE=gain[,gain...] also times each size
        // with fixed foveation at those gains (0 = none).
        if (const char* vs = gpu::debugEnv("PETARI_VRSIZE")) {
            struct Timed {
                int w, h;
                float gain;
                gpu::Target t;
            };
            static std::vector<Timed> sEyeTs;
            std::vector<float> gains;
            if (const char* fv = gpu::debugEnv("PETARI_FOVEATE")) {
                for (const char* p = fv; *p;) {
                    gains.push_back((float)atof(p));
                    while (*p && *p != ',') p++;
                    if (*p == ',') p++;
                }
            } else {
                gains.push_back(0.0f);
            }
            for (const char* p = vs; *p;) {
                int ew = 0, eh = 0;
                sscanf(p, "%dx%d", &ew, &eh);
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
                if (ew <= 0 || eh <= 0) continue;
                for (float gain : gains) {
                    Timed* tm = nullptr;
                    for (Timed& x : sEyeTs) {
                        if (x.w == ew && x.h == eh && x.gain == gain) tm = &x;
                    }
                    if (!tm) {
                        sEyeTs.push_back({ew, eh, gain, r.createTarget(ew, eh)});
                        tm = &sEyeTs.back();
                        if (gain > 0.0f && !r.setFoveation(tm->t, 0.0f, 0.0f, gain, 0.0f)) {
                            port_log("headless: foveation unsupported");
                        }
                    }
                    r.render(tm->t, &ev, nullptr, gpu::HudMode::Skip);
                    r.finish();
                    timespec ta;
                    clock_gettime(CLOCK_MONOTONIC, &ta);
                    for (int i = 0; i < 4; i++) r.render(tm->t, &ev, nullptr, gpu::HudMode::Skip);
                    r.finish();
                    long clk = 0;
                    if (FILE* g = fopen("/sys/class/kgsl/kgsl-3d0/gpuclk", "r")) {
                        if (fscanf(g, "%ld", &clk) != 1) clk = 0;
                        fclose(g);
                    }
                    port_log("headless: VR eye %dx%d foveation %.1f replay %.2f ms (GPU %ld MHz)", ew, eh, gain, msSince(ta) / 4.0, clk / 1000000);
                }
            }
        }
        // PETARI_GLSTEPS also applies here: <path>_vr_s<N>.png.
        if (const char* steps = gpu::debugEnv("PETARI_GLSTEPS")) {
            std::vector<unsigned char> sp((size_t)w * h * 4);
            for (const char* p = steps; *p;) {
                int n = atoi(p);
                gpu::gDebugStopAfterDraws = n;
                r.render(sTarget, &ev, nullptr, gpu::HudMode::Skip);
                r.readPixels(sTarget, sp.data());
                for (size_t i = 3; i < sp.size(); i += 4) sp[i] = 255;
                std::string sp_path = std::string(path).substr(0, strlen(path) - 4) + "_vr_s" + std::to_string(n) + ".png";
                writePng(sp_path.c_str(), sp.data(), w, h);
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
            gpu::gDebugStopAfterDraws = -1;
        }
        r.render(sTarget, &ev, nullptr, gpu::HudMode::Skip);
        std::vector<unsigned char> vp((size_t)w * h * 4);
        r.readPixels(sTarget, vp.data());
        for (size_t i = 3; i < vp.size(); i += 4) vp[i] = 255;
        std::string vpath = std::string(path).substr(0, strlen(path) - 4) + "_vr.png";
        writePng(vpath.c_str(), vp.data(), w, h);
        const gpu::CameraInfo& c = r.camera();
        // The player's centre in stage space (metres), to place PETARI_VRHEAD.
        const float* v = c.view;
        const float* pl = c.player;
        xm::Vec3 inView = {v[0] * pl[0] + v[1] * pl[1] + v[2] * pl[2] + v[3], v[4] * pl[0] + v[5] * pl[1] + v[6] * pl[2] + v[7],
                           v[8] * pl[0] + v[9] * pl[1] + v[10] * pl[2] + v[11]};
        xm::Vec3 ps = xm::transformPoint(stageFromView, inView);
        port_log("headless: VR view -> %s (flags %x aspect %.2f fovy %.1f, watch %.0f %.0f %.0f, up %.2f %.2f %.2f, player at stage %.2f %.2f %.2f)", vpath.c_str(),
                 c.flags, c.aspect, c.fovy, c.watch[0], c.watch[1], c.watch[2], c.watchUp[0], c.watchUp[1], c.watchUp[2], ps.x, ps.y, ps.z);
    }
    // Time a few replays of the prepared frame (the per-eye cost in VR).
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 3; i++) {
        r.render(sTarget, nullptr, nullptr);
    }
    r.finish();
    double renderMs = msSince(t0) / 3.0;
    port_log("headless: prepare %.2f ms, replay %.2f ms", prepMs, renderMs);
    if (r.api() == gpu::Api::Gles) {
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            port_log("headless: GL error 0x%x after render", err);
        }
    }
    std::vector<unsigned char> px((size_t)w * h * 4);
    r.readPixels(sTarget, px.data());
    for (size_t i = 3; i < px.size(); i += 4) {
        px[i] = 255;  // the EFB alpha channel is not part of the image
    }
    writePng(path, px.data(), w, h);
    port_log("headless: frame %llu (%u draws, %zu textures) -> %s", (unsigned long long)frame->number, frame->draws, frame->textures.size(), path);
    return frame->number;
}

// ---------------------------------------------------------------------------
// Frame files: a recorded frame saved as it was rendered, so the same frames
// can be replayed through different builds or renderer backends and the
// pictures compared exactly (the game itself is not deterministic).
//   PETARI_FRAMESAVE=<dir>: each snapshot also saves its frame as
//     <dir>/frame_<number>.gqf, the textures as <dir>/tex_<hash>.gqt.
//   PETARI_FRAMELOAD=<dir>: the first snapshot instead replays every frame
//     file in <dir> (in name order) and exits.  For each, next to the
//     snapshot path: <name>.png (flat, as a snapshot, with PETARI_VRTEST
//     <name>_vr.png), <name>_stereo.png (one picture of a stereo pair),
//     <name>_eye.png + <name>_hud.png + <name>_info.txt (a VR eye with the
//     HUD target and a depth snapshot), and with PETARI_XRSIM
//     <name>_xr.png (the headset presentation, the frame handed over as the
//     game's latest).  The game's own frames are held back meanwhile.
// Replays only compare when the shader cache holds every program (warm it
// with one replay first): programs compiled in the background are skipped
// until ready.
// ---------------------------------------------------------------------------
namespace {

const uint32_t kFrameMagic = 0x31465147;  // "GQF1"
const uint32_t kTexMagic = 0x31545147;    // "GQT1"

uint64_t fnv64(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

template <class V>
void writeVec(FILE* f, const V& v) {
    uint32_t n = (uint32_t)v.size();
    fwrite(&n, 4, 1, f);
    fwrite(v.data(), sizeof(v[0]), n, f);
}

template <class V>
bool readVec(FILE* f, V& v) {
    uint32_t n = 0;
    if (fread(&n, 4, 1, f) != 1 || n > (256u << 20)) return false;
    v.resize(n);
    return fread(v.data(), sizeof(v[0]), n, f) == n;
}

bool saveFrame(const gpu::Frame& f, const char* dir) {
    std::vector<uint64_t> texIds;
    for (const auto& img : f.textures) {
        const gpu::TexImage& t = *img;
        size_t words = 0;
        for (uint32_t l = 0, w = t.width, h = t.height; l < t.levels; l++, w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1) words += (size_t)w * h;
        if (t.rgba.size() < words) {
            port_log("headless: frame %llu not saved: texture pixels already released", (unsigned long long)f.number);
            return false;
        }
        uint32_t hdr[4] = {kTexMagic, t.width, t.height, t.levels};
        uint64_t id = fnv64(t.rgba.data(), words * 4, fnv64(hdr, sizeof(hdr)));
        texIds.push_back(id);
        char path[512];
        snprintf(path, sizeof(path), "%s/tex_%016llx.gqt", dir, (unsigned long long)id);
        if (access(path, F_OK) == 0) continue;
        FILE* out = fopen(path, "wb");
        if (!out) return false;
        fwrite(hdr, 4, 4, out);
        fwrite(t.rgba.data(), 4, words, out);
        fclose(out);
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/frame_%08llu.gqf", dir, (unsigned long long)f.number);
    FILE* out = fopen(path, "wb");
    if (!out) return false;
    fwrite(&kFrameMagic, 4, 1, out);
    fwrite(f.startBp, sizeof(f.startBp), 1, out);
    fwrite(f.startTevReg, sizeof(f.startTevReg), 1, out);
    fwrite(f.startTevKonst, sizeof(f.startTevKonst), 1, out);
    fwrite(f.startXfRegs, sizeof(f.startXfRegs), 1, out);
    fwrite(f.startXfMem, sizeof(f.startXfMem), 1, out);
    uint64_t number = f.number;
    uint32_t counts[2] = {f.xfbCopies, f.draws};
    fwrite(&number, 8, 1, out);
    fwrite(counts, 4, 2, out);
    writeVec(out, f.cmds);
    writeVec(out, f.verts);
    writeVec(out, texIds);
    fclose(out);
    port_log("headless: frame %llu saved -> %s (%zu textures)", (unsigned long long)f.number, path, texIds.size());
    return true;
}

// Loaded textures by id: frames share them as they did when recorded.
std::vector<std::pair<uint64_t, std::shared_ptr<gpu::TexImage>>> sLoadedTex;

std::unique_ptr<gpu::Frame> loadFrame(const char* path, const char* dir) {
    FILE* in = fopen(path, "rb");
    if (!in) return nullptr;
    std::unique_ptr<gpu::Frame> f(new gpu::Frame());
    uint32_t magic = 0, counts[2];
    uint64_t number = 0;
    std::vector<uint64_t> texIds;
    bool ok = fread(&magic, 4, 1, in) == 1 && magic == kFrameMagic && fread(f->startBp, sizeof(f->startBp), 1, in) == 1 &&
              fread(f->startTevReg, sizeof(f->startTevReg), 1, in) == 1 && fread(f->startTevKonst, sizeof(f->startTevKonst), 1, in) == 1 &&
              fread(f->startXfRegs, sizeof(f->startXfRegs), 1, in) == 1 && fread(f->startXfMem, sizeof(f->startXfMem), 1, in) == 1 &&
              fread(&number, 8, 1, in) == 1 && fread(counts, 4, 2, in) == 2 && readVec(in, f->cmds) && readVec(in, f->verts) && readVec(in, texIds);
    fclose(in);
    if (!ok) {
        port_log("headless: %s: not a frame file", path);
        return nullptr;
    }
    f->number = number;
    f->xfbCopies = counts[0];
    f->draws = counts[1];
    for (uint64_t id : texIds) {
        std::shared_ptr<gpu::TexImage> img;
        for (auto& kv : sLoadedTex) {
            if (kv.first == id) img = kv.second;
        }
        if (!img) {
            char tpath[512];
            snprintf(tpath, sizeof(tpath), "%s/tex_%016llx.gqt", dir, (unsigned long long)id);
            FILE* t = fopen(tpath, "rb");
            uint32_t hdr[4] = {};
            if (!t || fread(hdr, 4, 4, t) != 4 || hdr[0] != kTexMagic) {
                port_log("headless: %s: missing texture %s", path, tpath);
                if (t) fclose(t);
                return nullptr;
            }
            img = std::make_shared<gpu::TexImage>();
            img->width = hdr[1];
            img->height = hdr[2];
            img->levels = hdr[3];
            size_t words = 0;
            for (uint32_t l = 0, w = hdr[1], h = hdr[2]; l < hdr[3]; l++, w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1) words += (size_t)w * h;
            img->rgba.resize(words);
            bool got = fread(img->rgba.data(), 4, words, t) == words;
            fclose(t);
            if (!got) return nullptr;
            img->version = 1;
            sLoadedTex.emplace_back(id, img);
        }
        f->textures.push_back(img);
    }
    return f;
}

void writeText(const std::string& path, const char* text) {
    if (FILE* f = fopen(path.c_str(), "w")) {
        fputs(text, f);
        fclose(f);
    }
}

// Replays the frame files in `dir`; snapshots go next to `shotPath`.
unsigned long long replayFrames(const char* dir, const char* shotPath, int scale) {
    gpu::holdGameFrames(true);
    std::vector<std::string> names;
    if (DIR* d = opendir(dir)) {
        while (dirent* e = readdir(d)) {
            size_t n = strlen(e->d_name);
            if (n > 4 && !strcmp(e->d_name + n - 4, ".gqf")) names.push_back(std::string(e->d_name, n - 4));
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());
    std::string outDir = shotPath;
    outDir = outDir.substr(0, outDir.find_last_of('/') == std::string::npos ? 0 : outDir.find_last_of('/') + 1);
    gpu::Renderer& r = gpu::renderer();
    unsigned long long last = 0;
    for (const std::string& name : names) {
        std::unique_ptr<gpu::Frame> loaded = loadFrame((std::string(dir) + "/" + name + ".gqf").c_str(), dir);
        if (!loaded) continue;
        std::shared_ptr<const gpu::Frame> frame(loaded.release());
        std::string base = outDir + name;
        last = renderShots(frame, (base + ".png").c_str(), scale);
        int w = sTarget.width, h = sTarget.height;
        std::vector<unsigned char> px((size_t)w * h * 4);
        auto save = [&](const gpu::Target& t, const std::string& path, bool opaque) {
            r.readPixels(t, px.data());
            if (opaque) {
                for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
            }
            writePng(path.c_str(), px.data(), t.width, t.height);
        };
        // One picture of a stereo pair, the HUD shifted (and its scissor
        // boxes with it).
        gpu::EyeView pair;
        pair.flatStereo = true;
        pair.stereo[0] = 0.04f;
        pair.stereo[1] = 1500.0f;
        pair.stereo[2] = 2.0f;
        pair.stereo[3] = 0.03f;
        pair.pointer[0] = 0.01f;
        r.render(sTarget, &pair, nullptr, gpu::HudMode::Inline);
        save(sTarget, base + "_stereo.png", true);
        // A VR eye with the HUD in its own target and SpaceWarp's depth
        // snapshot.
        if (r.camera().valid) {
            static gpu::Target sHud, sSnap;
            if (sHud.width != w || sHud.height != h) {
                if (sHud.fbo) r.destroyTarget(sHud);
                if (sSnap.fbo) r.destroyTarget(sSnap);
                sHud = r.createTarget(w, h);
                sSnap = r.createDepthTarget(w / 4, h / 4);
            }
            gpu::EyeView ev;
            testEye(r, w, h, ev);
            ev.index = 1;
            ev.depthSnapshot = &sSnap;
            r.render(sTarget, &ev, &sHud, gpu::HudMode::Target);
            char info[128];
            snprintf(info, sizeof(info), "snapshot %d hud draws %d\n", r.snapshotTaken() ? 1 : 0, r.hudDrawCount());
            writeText(base + "_info.txt", info);
            save(sTarget, base + "_eye.png", true);
            save(sHud, base + "_hud.png", false);
        }
        r.finish();
        // The headset presentation: the frame goes through the queue and the
        // preparation thread, as the game's frames do.
        if (gpu::debugEnv("PETARI_XRSIM")) {
            std::unique_ptr<gpu::Frame> copy = loadFrame((std::string(dir) + "/" + name + ".gqf").c_str(), dir);
            if (copy) {
                // Picked up and prepared before the presentation starts, so
                // its steps all show the same frame.
                gpu::injectFrame(std::move(copy));
                for (int i = 0; i < 2000 && !r.update(); i++) {
                    usleep(2000);
                }
                float yaw = 0, pitch = -20;
                if (const char* xh = gpu::debugEnv("PETARI_XRHEAD")) sscanf(xh, "%f,%f", &yaw, &pitch);
                int steps = gpu::debugEnv("PETARI_XRSIM_STEPS") ? atoi(gpu::debugEnv("PETARI_XRSIM_STEPS")) : 40;
                port_headless_xrsim((base + "_xr.png").c_str(), steps > 0 ? steps : 1, yaw, pitch);
            }
        }
    }
    port_log("headless: replayed %zu frames from %s", names.size(), dir);
    fflush(nullptr);
    _exit(0);
    return last;
}

}  // namespace

// Renders the latest recorded frame at `scale` x the native EFB size and
// writes `path` (PNG).  Returns the frame number, or 0 if nothing was drawn.
extern "C" unsigned long long port_headless_render(const char* path, int scale) {
    if (!initContext()) {
        return 0;
    }
    if (const char* dir = gpu::debugEnv("PETARI_FRAMELOAD")) {
        return replayFrames(dir, path, scale);
    }
    auto frame = gpu::frameQueue().latest();
    if (!frame) {
        return 0;
    }
    if (const char* dir = gpu::debugEnv("PETARI_FRAMESAVE")) {
        saveFrame(*frame, dir);
    }
    return renderShots(frame, path, scale);
}
