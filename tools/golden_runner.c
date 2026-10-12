// Frame-locked headless runner for the golden captures (tools/golden.sh).
//
// platform/android/headless.c snapshots on wall-clock timers, so a capture lands on
// game frame N or N+1 from run to run, and animated pixels (water, sparkles) differ.
// This runner boots libgame.so the same way, but keys everything to the game's own
// retrace counter, which is the game's frame clock:
//   - the scripted controller (PETARI_INPUT, "seconds") is read as retraces * 1001/60000 s,
//     and the state for retrace n+1 is set mid-frame, after retrace n was handled;
//   - a snapshot at retrace K pauses the game's retraces the moment the counter reaches K,
//     lets the frame in flight finish, renders the latest frame, then resumes.
//
//   golden_runner <game root> <save root> <out dir> <last retrace> <name>:<retrace>[:x][,...]
//
// Writes <out dir>/<name>.png (flat picture) and, for shots marked :x, <name>_xr.png (both
// simulated eyes, the headset presentation; also <name>_xr_mv.png with PETARI_XRSIM_MV=1).  Environment as for petari_headless (PETARI_INPUT, PETARI_STAGE,
// PETARI_VRINI, PETARI_SHADER_CACHE, PETARI_XRSIM_*, ...).  Logs to stderr.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "port/input.h"
#include "port/input_script.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static const uintptr_t kWindowBase = 0x80000000u;
static const size_t kWindowSize = 0x60010000u;
static const uintptr_t kLibBase = 0x98000000u;
static const size_t kLibSize = 0x08000000u;
static const double kRetraceMs = 1001.0 / 60.0;  // one retrace, in ms

static const char* envSet(const char* name) {
    const char* v = getenv(name);
    return v && *v ? v : NULL;
}

static void sleepMs(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void padAt(const PortInputEvent* ev, int count, int ms, PortPadState* pad) {
    memset(pad, 0, sizeof(*pad));
    pad->connected = 1;
    pad->pointerDist = 2.0f;
    portApplyInputScript(ev, count, ms, pad);
}

typedef struct {
    char name[64];
    unsigned retrace;
    int xr;
} Shot;

static int cmpShot(const void* a, const void* b) {
    unsigned x = ((const Shot*)a)->retrace, y = ((const Shot*)b)->retrace;
    return x < y ? -1 : x > y;
}

int main(int argc, char** argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s <game root> <save root> <out dir> <last retrace> <name>:<retrace>[:x][,...]\n", argv[0]);
        return 2;
    }
    const char* outDir = argv[3];
    unsigned lastRetrace = (unsigned)atoi(argv[4]);
    static Shot shots[256];
    int shotCount = 0;
    {
        char* spec = strdup(argv[5]);
        char* save = NULL;
        for (char* tok = strtok_r(spec, ",", &save); tok && shotCount < 256; tok = strtok_r(NULL, ",", &save)) {
            char* colon = strchr(tok, ':');
            if (!colon) continue;
            *colon = 0;
            snprintf(shots[shotCount].name, sizeof(shots[0].name), "%s", tok);
            shots[shotCount].retrace = (unsigned)atoi(colon + 1);
            shots[shotCount].xr = strstr(colon + 1, ":x") != NULL;
            shotCount++;
        }
        qsort(shots, shotCount, sizeof(Shot), cmpShot);
    }

    // The game's address window, with the library slot left free (platform/linux/launcher.c).
    uintptr_t parts[2][2] = {{kWindowBase, kLibBase}, {kLibBase + kLibSize, kWindowBase + kWindowSize}};
    for (int i = 0; i < 2; i++) {
        void* p = mmap((void*)parts[i][0], parts[i][1] - parts[i][0], PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                       -1, 0);
        if (p != (void*)parts[i][0]) {
            fprintf(stderr, "cannot reserve %p: got %p errno %d\n", (void*)parts[i][0], p, errno);
            return 1;
        }
    }
    char path[512];
    snprintf(path, sizeof(path), "%s", argv[0]);
    char* slash = strrchr(path, '/');
    const char* libDir = getenv("GALAXYQUEST_LIB");  // directory holding libgame.so (default: next to this program)
    if (libDir && *libDir) {
        snprintf(path, sizeof(path), "%s/libgame.so", libDir);
    } else {
        snprintf(slash ? slash + 1 : path, sizeof(path) - (slash ? (size_t)(slash + 1 - path) : 0), "libgame.so");
    }
    void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "dlopen %s: %s\n", path, dlerror());
        return 1;
    }
    Dl_info info;
    if (!dladdr(dlsym(lib, "port_boot"), &info) || (uintptr_t)info.dli_fbase != kLibBase) {
        fprintf(stderr, "libgame.so is not at %p\n", (void*)kLibBase);
        return 1;
    }
    int* toStderr = (int*)dlsym(lib, "port_log_to_stderr");
    if (toStderr) *toStderr = 1;
    void (*setWindow)(uintptr_t, size_t) = (void (*)(uintptr_t, size_t))dlsym(lib, "port_mem_set_reserved_window");
    void (*boot)(const char*, const char*) = (void (*)(const char*, const char*))dlsym(lib, "port_boot");
    void (*inputSet)(int, const PortPadState*) = (void (*)(int, const PortPadState*))dlsym(lib, "port_input_set");
    void (*setPaused)(int) = (void (*)(int))dlsym(lib, "port_set_paused");
    unsigned (*retraceCount)(void) = (unsigned (*)(void))dlsym(lib, "VIGetRetraceCount");
    unsigned long long (*render)(const char*, int) = (unsigned long long (*)(const char*, int))dlsym(lib, "port_headless_render");
    void (*xrsim)(const char*, int, float, float) = (void (*)(const char*, int, float, float))dlsym(lib, "port_headless_xrsim");
    if (!setWindow || !boot || !inputSet || !setPaused || !retraceCount || !render) {
        fprintf(stderr, "missing entry points: %s\n", dlerror());
        return 1;
    }
    setWindow(kWindowBase, kWindowSize);

    static PortInputEvent events[1024];
    int eventCount = envSet("PETARI_INPUT") ? portParseInputScript(envSet("PETARI_INPUT"), events, 1024) : 0;
    PortPadState pad;
    padAt(events, eventCount, 0, &pad);
    inputSet(0, &pad);

    // PETARI_WARP and friends compare wall time with PETARI_T0_MS; not used in lockstep.
    fprintf(stderr, "golden: libgame.so at %p; booting\n", (void*)kLibBase);
    boot(argv[1], argv[2]);

    int steps = envSet("PETARI_XRSIM_STEPS") ? atoi(envSet("PETARI_XRSIM_STEPS")) : 40;
    float yaw = 0, pitch = -20;
    if (envSet("PETARI_XRHEAD")) sscanf(envSet("PETARI_XRHEAD"), "%f,%f", &yaw, &pitch);
    unsigned last = retraceCount();
    int next = 0;
    unsigned lastButtonsLog = 0;
    for (;;) {
        unsigned c = retraceCount();
        if (c == last) {
            usleep(500);
            continue;
        }
        last = c;
        if (next < shotCount && c >= shots[next].retrace) {
            // Stop further retraces, let the frame in flight finish, then render the latest.
            setPaused(1);
            unsigned at = retraceCount();
            sleepMs(300);
            char file[640];
            snprintf(file, sizeof(file), "%s/%s.png", outDir, shots[next].name);
            fprintf(stderr, "golden: shot %s at retrace %u (wanted %u)\n", shots[next].name, at, shots[next].retrace);
            unsigned long long frame = render(file, 2);
            fprintf(stderr, "golden: shot %s is game frame %llu\n", shots[next].name, frame);
            if (shots[next].xr && xrsim) {
                snprintf(file, sizeof(file), "%s/%s_xr.png", outDir, shots[next].name);
                xrsim(file, steps > 0 ? steps : 1, yaw, pitch);
            }
            next++;
            setPaused(0);
            last = retraceCount();
            if (next >= shotCount) break;
            continue;
        }
        if (c > lastRetrace) {
            fprintf(stderr, "golden: reached retrace %u before all shots were taken\n", c);
            return 3;
        }
        // The controller state the game reads after the next retrace, set mid-frame.
        sleepMs(8);
        padAt(events, eventCount, (int)((c + 1) * kRetraceMs), &pad);
        inputSet(0, &pad);
        if (pad.buttons != lastButtonsLog) {
            fprintf(stderr, "golden: retrace %u buttons %04x\n", c + 1, pad.buttons);
            lastButtonsLog = pad.buttons;
        }
    }
    fprintf(stderr, "golden: done at retrace %u\n", retraceCount());
    _exit(0);
}
