// Scheduling priority for the threads a frame depends on (the emulated game
// threads, the frame preparation and the headset frame loop).  On the Steam
// Frame the process has five of the eight cores and shares them with its own
// workers (disc, sound, texture decoding, the driver's) and with SteamVR; the
// game thread, which has 8 ms to produce a frame, lost the CPU for 10 ms and
// more now and then, a repeated frame each time.
//
// The system's limit allows nice -8 (RLIMIT_NICE).  RealtimeKit
// (org.freedesktop.RealtimeKit1 on the system bus, as PipeWire uses it) grants
// more: nice down to its MinNiceLevel (-15 here), and real-time scheduling
// (SCHED_RR) under a CPU-time limit.  PETARI_THREADPRIO picks the level:
//   off   leave every thread as it is
//   nice  nice -8 (or what RLIMIT_NICE allows), no RealtimeKit
//   high  (default) the lowest nice RealtimeKit allows, else as nice
//   rt    as high, and the main game thread real-time (port_boost_thread_realtime)
// RealtimeKit only grants real-time to a process whose RLIMIT_RTTIME is
// bounded (200 ms): a real-time thread that runs that long without blocking
// gets SIGKILL.  The soft limit is set lower, and its SIGXCPU puts the
// real-time threads back to normal scheduling (with their nice), so a long
// stretch of work (a scene being built) only costs the boost; the thread asks
// again a few seconds later (port_thread_prio_tick).
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <mutex>

#include "port/port.h"

namespace {

enum Mode { kOff, kNice, kHigh, kRealtime };

Mode mode() {
    static const Mode m = [] {
        const char* v = getenv("PETARI_THREADPRIO");
        if (!v || !*v) return kHigh;
        if (!strcmp(v, "off") || !strcmp(v, "0")) return kOff;
        if (!strcmp(v, "nice")) return kNice;
        if (!strcmp(v, "rt") || !strcmp(v, "realtime")) return kRealtime;
        return kHigh;
    }();
    return m;
}

const int kWantedNice = -15;
const int kWantedRtPrio = 5;       // low: below the system's audio and display threads
const int64_t kRtSoftUs = 150000;  // SIGXCPU here, before RealtimeKit's hard 200 ms

int tidSelf() { return (int)syscall(SYS_gettid); }

// sd-bus through dlopen (libsystemd is not linked).
struct SdBusError {
    const char* name;
    const char* message;
    int needFree;
};
typedef int (*SdOpenFn)(void** bus);
typedef int (*SdCallFn)(void* bus, const char* dest, const char* path, const char* iface, const char* member, SdBusError* err, void** reply,
                        const char* types, ...);
typedef int (*SdGetPropFn)(void* bus, const char* dest, const char* path, const char* iface, const char* member, SdBusError* err, char type,
                           void* ret);
typedef void* (*SdUnrefFn)(void* p);
typedef void (*SdErrFreeFn)(SdBusError* e);

const char* const kRtkitDest = "org.freedesktop.RealtimeKit1";
const char* const kRtkitPath = "/org/freedesktop/RealtimeKit1";

struct Rtkit {
    bool tried = false, ok = false;
    void* bus = nullptr;
    SdCallFn call = nullptr;
    SdGetPropFn getProp = nullptr;
    SdUnrefFn msgUnref = nullptr;
    SdErrFreeFn errFree = nullptr;
    int minNice = kWantedNice, maxRtPrio = kWantedRtPrio;
    int64_t rttimeMaxUs = 200000;
};

std::mutex sLock;  // guards sRtkit and the bus (sd-bus objects are not thread-safe)
Rtkit sRtkit;

// The bus, opened on first use and kept (sLock held).
Rtkit* rtkit() {
    Rtkit& r = sRtkit;
    if (r.tried) return r.ok ? &r : nullptr;
    r.tried = true;
    void* lib = dlopen("libsystemd.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        port_log("threads: RealtimeKit not used (no libsystemd)");
        return nullptr;
    }
    SdOpenFn open = (SdOpenFn)dlsym(lib, "sd_bus_open_system");
    r.call = (SdCallFn)dlsym(lib, "sd_bus_call_method");
    r.getProp = (SdGetPropFn)dlsym(lib, "sd_bus_get_property_trivial");
    r.msgUnref = (SdUnrefFn)dlsym(lib, "sd_bus_message_unref");
    r.errFree = (SdErrFreeFn)dlsym(lib, "sd_bus_error_free");
    int rc = open && r.call && r.getProp && r.msgUnref && r.errFree ? open(&r.bus) : -ENOSYS;
    if (rc < 0 || !r.bus) {
        port_log("threads: RealtimeKit not used (system bus: %s)", strerror(-rc));
        return nullptr;
    }
    auto prop = [&r](const char* name, char type, void* out) {
        SdBusError e = {};
        int rc2 = r.getProp(r.bus, kRtkitDest, kRtkitPath, kRtkitDest, name, &e, type, out);
        r.errFree(&e);
        return rc2;
    };
    int32_t v = 0;
    int64_t x = 0;
    if (prop("MinNiceLevel", 'i', &v) < 0) {
        port_log("threads: RealtimeKit not available");
        return nullptr;
    }
    r.minNice = v;
    if (prop("MaxRealtimePriority", 'i', &v) >= 0) r.maxRtPrio = v;
    if (prop("RTTimeUSecMax", 'x', &x) >= 0) r.rttimeMaxUs = x;
    r.ok = true;
    port_log("threads: RealtimeKit allows nice %d, real-time priority %d, %lld ms of real-time CPU at a stretch", r.minNice, r.maxRtPrio,
             (long long)(r.rttimeMaxUs / 1000));
    return &r;
}

// A RealtimeKit call taking (thread id, value); sLock held.  Returns 0 or a
// message.
const char* rtkitCall(Rtkit& r, const char* member, const char* types, int tid, int value, char* why, size_t whyLen) {
    SdBusError e = {};
    void* reply = nullptr;
    int rc = r.call(r.bus, kRtkitDest, kRtkitPath, kRtkitDest, member, &e, &reply, types, (uint64_t)tid, (int32_t)value);
    const char* res = nullptr;
    if (rc < 0) {
        snprintf(why, whyLen, "%s", e.message ? e.message : strerror(-rc));
        res = why;
    }
    if (reply) r.msgUnref(reply);
    r.errFree(&e);
    return res;
}

// The nice the rlimit allows (decided once): -8 on the Frame, 0 if none.
int rlimitNice() {
    static const int n = [] {
        struct rlimit rl;
        if (getrlimit(RLIMIT_NICE, &rl) == 0 && rl.rlim_max > rl.rlim_cur) {
            rl.rlim_cur = rl.rlim_max;
            setrlimit(RLIMIT_NICE, &rl);
        }
        int allowed = (getrlimit(RLIMIT_NICE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) ? 20 - (int)rl.rlim_cur : -20;
        return allowed < -8 ? -8 : allowed > 0 ? 0 : allowed;
    }();
    return n;
}

// Real-time threads, for the SIGXCPU handler (thread ids, 0 = free).
const int kMaxRt = 8;
std::atomic<int> sRtTids[kMaxRt];
std::atomic<int> sRtDemoted[kMaxRt];   // 1: put back to normal scheduling by the handler
std::atomic<int64_t> sDemotedAt{0};
std::atomic<int> sDemotions{0};

void onCpuLimit(int) {
    // Async-signal-safe: syscalls and atomics only.
    int saved = errno;
    struct sched_param p;
    memset(&p, 0, sizeof(p));
    for (int i = 0; i < kMaxRt; i++) {
        int tid = sRtTids[i].load(std::memory_order_relaxed);
        if (tid && !sRtDemoted[i].load(std::memory_order_relaxed)) {
            syscall(SYS_sched_setscheduler, tid, SCHED_OTHER, &p);  // keeps the thread's nice
            sRtDemoted[i].store(1, std::memory_order_relaxed);
        }
    }
    sDemotedAt.store(port_host_time_ns(), std::memory_order_relaxed);
    sDemotions.fetch_add(1, std::memory_order_relaxed);
    errno = saved;
}

// RLIMIT_RTTIME within RealtimeKit's bound and the SIGXCPU handler, once.
bool prepareRealtime(Rtkit& r) {
    static int state = 0;  // 0 not yet, 1 ready, -1 failed
    if (state) return state > 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onCpuLimit;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    struct rlimit rl;
    rl.rlim_max = (rlim_t)r.rttimeMaxUs;
    rl.rlim_cur = (rlim_t)(kRtSoftUs < r.rttimeMaxUs ? kRtSoftUs : r.rttimeMaxUs * 3 / 4);
    if (sigaction(SIGXCPU, &sa, nullptr) != 0 || setrlimit(RLIMIT_RTTIME, &rl) != 0) {
        port_log("threads: no real-time scheduling (RLIMIT_RTTIME: %s)", strerror(errno));
        state = -1;
        return false;
    }
    state = 1;
    return true;
}

// Asks for real-time scheduling for `tid` (sLock held); true if granted.
bool makeRealtime(int tid, int slot, bool logIt) {
    Rtkit* r = rtkit();
    if (!r || !prepareRealtime(*r)) return false;
    int prio = r->maxRtPrio < kWantedRtPrio ? r->maxRtPrio : kWantedRtPrio;
    char why[200];
    if (const char* err = rtkitCall(*r, "MakeThreadRealtime", "tu", tid, prio, why, sizeof(why))) {
        if (logIt) port_log("threads: real-time scheduling refused for thread %d: %s", tid, err);
        return false;
    }
    sRtDemoted[slot].store(0, std::memory_order_relaxed);
    sRtTids[slot].store(tid, std::memory_order_relaxed);
    if (logIt) port_log("threads: thread %d real-time (SCHED_RR %d through RealtimeKit)", tid, prio);
    return true;
}

}  // namespace

extern "C" {

// High priority for the calling thread (see above).
void port_boost_thread(void) {
#ifndef __ANDROID__
    Mode m = mode();
    if (m == kOff) return;
    int tid = tidSelf();
    int nice = rlimitNice();
    if (nice < 0) setpriority(PRIO_PROCESS, (id_t)tid, nice);
    const char* how = "setpriority";
    char why[200] = "";
    if (m >= kHigh) {
        std::lock_guard<std::mutex> lock(sLock);
        if (Rtkit* r = rtkit()) {
            int want = r->minNice > kWantedNice ? r->minNice : kWantedNice;
            if (want < nice) {
                if (const char* err = rtkitCall(*r, "MakeThreadHighPriority", "ti", tid, want, why, sizeof(why))) {
                    how = why;
                } else {
                    nice = want;
                    how = "RealtimeKit";
                }
            }
        }
    }
    static std::atomic<int> sLogged{0};
    if (sLogged.fetch_add(1) < 1) {
        port_log("threads: nice %d for the game threads and the frame loop (%s; limit allows %d)", nice, how, rlimitNice());
    }
#endif
}

// The main game thread: high priority, and with PETARI_THREADPRIO=rt
// real-time scheduling.
void port_boost_thread_realtime(void) {
    port_boost_thread();
#ifndef __ANDROID__
    if (mode() != kRealtime) return;
    int tid = tidSelf();
    std::lock_guard<std::mutex> lock(sLock);
    for (int i = 0; i < kMaxRt; i++) {
        int cur = sRtTids[i].load(std::memory_order_relaxed);
        if (cur == tid || cur == 0) {
            makeRealtime(tid, i, true);
            return;
        }
    }
#endif
}

// Called by a real-time thread now and then (each frame): after the CPU
// limit put it back to normal scheduling, it asks again 5 s later.
void port_thread_prio_tick(void) {
#ifndef __ANDROID__
    if (sDemotions.load(std::memory_order_relaxed) == 0) return;
    int tid = tidSelf();
    for (int i = 0; i < kMaxRt; i++) {
        if (sRtTids[i].load(std::memory_order_relaxed) != tid || !sRtDemoted[i].load(std::memory_order_relaxed)) continue;
        if (port_host_time_ns() - sDemotedAt.load(std::memory_order_relaxed) < 5000000000ll) return;
        static std::atomic<int> sLogged{0};
        bool logIt = sLogged.fetch_add(1) < 3;
        if (logIt) port_log("threads: thread %d real-time again (it had run %lld ms without a break)", tid, (long long)(kRtSoftUs / 1000));
        std::lock_guard<std::mutex> lock(sLock);
        if (!makeRealtime(tid, i, logIt)) {
            sRtDemoted[i].store(0, std::memory_order_relaxed);
            sRtTids[i].store(0, std::memory_order_relaxed);  // refused: stays normal
        }
        return;
    }
#endif
}

}  // extern "C"
