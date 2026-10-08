// Linux launcher (the Steam Frame's own SteamVR runtime).
//
// Same job as platform/android/launcher.c: the game code assumes a 32-bit
// address space, so reserve the console window 0x80000000-0xE0010000 and load
// libgame.so into the 0x98000000 slot, then hand control to it.  glibc has no
// android_dlopen_ext, but libgame.so is linked with that slot as its image
// base and the dynamic loader asks the kernel for it: the slot is left out of
// the reservation and the library is checked to have landed there.
//
//   galaxyquest [game folder]
//
// Everything the app keeps (game/, nand/, settings, log, shader cache) is in
// $GALAXYQUEST_HOME, else $XDG_DATA_HOME/GalaxyQuest, else
// ~/.local/share/GalaxyQuest.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#define PR_SET_VMA_ANON_NAME 0
#endif

static const uintptr_t kWindowBase = 0x80000000u;
static const size_t kWindowSize = 0x60010000u;  // up to 0xE0010000 (locked cache)
static const uintptr_t kLibBase = 0x98000000u;
static const size_t kLibSize = 0x08000000u;

typedef void (*PortLinuxMain)(const char* home, const char* gameDir, uintptr_t windowBase, size_t windowSize);

static void reserve(uintptr_t base, size_t size) {
    void* p = mmap((void*)base, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void*)base) {
        fprintf(stderr, "galaxyquest: cannot reserve the game address window at %p (+0x%zx): %s\n", (void*)base, size,
                p == MAP_FAILED ? strerror(errno) : "placed elsewhere");
        exit(1);
    }
    prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, base, size, "petari game window");
}

int main(int argc, char** argv) {
    // The window but the library slot, which the library itself takes.
    reserve(kWindowBase, kLibBase - kWindowBase);
    reserve(kLibBase + kLibSize, kWindowBase + kWindowSize - (kLibBase + kLibSize));

    char path[PATH_MAX];
    const char* override = getenv("GALAXYQUEST_LIB");
    if (override) {
        snprintf(path, sizeof(path), "%s", override);
    } else {
        ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (n <= 0) {
            fprintf(stderr, "galaxyquest: cannot find the launcher's own path\n");
            return 1;
        }
        path[n] = 0;
        char* slash = strrchr(path, '/');
        snprintf(slash + 1, sizeof(path) - (size_t)(slash + 1 - path), "libgame.so");
    }

    void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "galaxyquest: dlopen(%s) failed: %s\n", path, dlerror());
        return 1;
    }
    PortLinuxMain entry = (PortLinuxMain)dlsym(lib, "port_linux_main");
    Dl_info info;
    if (!entry || !dladdr((void*)entry, &info)) {
        fprintf(stderr, "galaxyquest: port_linux_main not found: %s\n", dlerror());
        return 1;
    }
    if ((uintptr_t)info.dli_fbase != kLibBase) {
        fprintf(stderr, "galaxyquest: libgame.so landed at %p, not %p (address in use)\n", info.dli_fbase, (void*)kLibBase);
        return 1;
    }

    char home[PATH_MAX];
    const char* env = getenv("GALAXYQUEST_HOME");
    const char* xdg = getenv("XDG_DATA_HOME");
    if (env && *env) {
        snprintf(home, sizeof(home), "%s", env);
    } else if (xdg && *xdg) {
        snprintf(home, sizeof(home), "%s/GalaxyQuest", xdg);
    } else {
        snprintf(home, sizeof(home), "%s/.local/share/GalaxyQuest", getenv("HOME") ? getenv("HOME") : ".");
    }
    fprintf(stderr, "galaxyquest: libgame.so at %p, home %s\n", info.dli_fbase, home);
    entry(home, argc > 1 ? argv[1] : NULL, kWindowBase, kWindowSize);
    return 0;
}
