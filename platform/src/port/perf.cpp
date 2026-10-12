// Frame-time statistics for the game thread and the renderer, logged every
// 10 s: always with PETARI_PERFLOG=1, otherwise only while the game runs
// below 58 fps (so the headset's log shows slowdowns).
//
// Also, always, one "perf1s" line a second (written by the game thread at
// the end of a frame's work, so a stalled game thread makes a longer
// window).  Space-separated key=value fields, times in ms:
//   perf1s t=<s since the first frame> dt=<window s> frames=<game frames>
//     work_p50= work_p95= work_max=   game work per frame (frame start to the retrace wait)
//     rec_max=                        GX recording, the most in one frame
//     worst_rec= worst_tex=           the slowest frame's GX recording and texture work
//     worst_flt= flt=                 page faults of the game thread: in the slowest frame, in all
//     texchk_ms=                      texture work on the game thread (lookups, hashing,
//                                     copies, and decodes made there), summed
//     texdec_ms= texdec_max= texdec_n= texdec_kb=   texture decodes: sum, longest, count, decoded KB
//     build_ms= build_max= builds=    frame preparation (Builder) on the worker thread
//     up_ms= up_max= up_n= up_kb=     texture uploads: time, longest frame's, textures, KB
//     prog_miss= prog_new=            programs not ready when a frame needed them (batches skipped
//                                     until compiled), programs built (compiles, not cache loads)
//     render_n= render_ms= render_max=   displayed frames: count, CPU time avg and max
#include <algorithm>
#include <stdlib.h>
#include <sys/resource.h>

#include <atomic>
#include <mutex>

#include "port/port.h"

namespace {

struct Stat {
    double sum = 0.0, max = 0.0;
    unsigned count = 0;
    void add(double v) {
        sum += v;
        max = v > max ? v : max;
        count++;
    }
    double avg() const { return count ? sum / count : 0.0; }
};

std::mutex sLock;
Stat sWork, sRecord, sPrepare, sRenderCpu, sUpload, sGpuWait;
unsigned sFrames = 0, sRenderFrames = 0;
int64_t sFrameStart = 0, sRecordThisFrame = 0, sWindowStart = 0;

// The perf1s window (guarded by sLock).
struct Second {
    float work[512];  // game work per frame, ms (the first 512 of the window)
    unsigned frames = 0;
    double recMax = 0, texChk = 0;
    double worstWork = 0, worstRec = 0, worstTex = 0;
    long worstFlt = 0, flt = 0;
    Stat texDec, build, upload, render;
    uint64_t texDecBytes = 0, upBytes = 0;
    unsigned upCount = 0, progMiss = 0, progNew = 0;
};
Second sSec;
int64_t sSecStart = 0, sFirstFrame = 0, sTexThisFrame = 0;
long sFltAtStart = 0;

// Page faults of the calling thread so far.
long threadFaults() {
    struct rusage ru;
    return getrusage(RUSAGE_THREAD, &ru) == 0 ? ru.ru_minflt + ru.ru_majflt : 0;
}

float percentile(float* v, unsigned n, unsigned pct) {
    if (!n) return 0.0f;
    unsigned k = (n - 1) * pct / 100;
    std::nth_element(v, v + k, v + n);
    return v[k];
}

void reportSecond(int64_t now) {
    Second& s = sSec;
    unsigned n = s.frames < 512 ? s.frames : 512;
    float maxWork = 0.0f;
    for (unsigned i = 0; i < n; i++) maxWork = s.work[i] > maxWork ? s.work[i] : maxWork;
    float p50 = percentile(s.work, n, 50), p95 = percentile(s.work, n, 95);
    port_log("perf1s t=%.1f dt=%.2f frames=%u work_p50=%.1f work_p95=%.1f work_max=%.1f rec_max=%.1f worst_rec=%.1f worst_tex=%.1f worst_flt=%ld flt=%ld texchk_ms=%.1f "
             "texdec_ms=%.1f texdec_max=%.1f "
             "texdec_n=%u texdec_kb=%llu build_ms=%.1f build_max=%.1f builds=%u up_ms=%.1f up_max=%.1f up_n=%u up_kb=%llu prog_miss=%u prog_new=%u "
             "render_n=%u render_ms=%.1f render_max=%.1f",
             (now - sFirstFrame) / 1e9, (now - sSecStart) / 1e9, s.frames, p50, p95, maxWork, s.recMax, s.worstRec, s.worstTex, s.worstFlt, s.flt, s.texChk, s.texDec.sum, s.texDec.max,
             s.texDec.count, (unsigned long long)(s.texDecBytes >> 10), s.build.sum, s.build.max, s.build.count, s.upload.sum, s.upload.max, s.upCount,
             (unsigned long long)(s.upBytes >> 10), s.progMiss, s.progNew, s.render.count, s.render.avg(), s.render.max);
    s = Second();
    sSecStart = now;
}
// Read on first use: the VR app sets debug variables after the library loads.
bool alwaysLog() {
    static const bool v = getenv("PETARI_PERFLOG") != nullptr;
    return v;
}

void report(int64_t now) {
    double secs = (now - sWindowStart) / 1e9;
    double fps = sFrames / secs;
    if (alwaysLog() || fps < 58.0) {
        port_log("perf: game %.1f fps, work %.1f ms avg %.1f max (GX recording %.1f avg); renderer %.1f fps, cpu %.1f ms avg %.1f max, "
                 "upload %.1f avg, prepare %.1f avg %.1f max",
                 fps, sWork.avg(), sWork.max, sRecord.avg(), sRenderFrames / secs, sRenderCpu.avg(), sRenderCpu.max, sUpload.avg(), sPrepare.avg(),
                 sPrepare.max);
        if (sGpuWait.count) {
            port_log("perf: GPU finished %.1f ms avg %.1f max after the render thread's submission", sGpuWait.avg(), sGpuWait.max);
        }
    }
    sWork = sRecord = sPrepare = sRenderCpu = sUpload = sGpuWait = Stat();
    sFrames = sRenderFrames = 0;
    sWindowStart = now;
}

}  // namespace

extern "C" {

void port_perf_frame_begin(void) {
    port_thread_prio_tick();  // the main game thread
    std::lock_guard<std::mutex> lock(sLock);
    int64_t now = port_host_time_ns();
    if (!sWindowStart) sWindowStart = now;
    sFrameStart = now;
    sRecordThisFrame = 0;
    sTexThisFrame = 0;
    sFltAtStart = threadFaults();
}

void port_perf_frame_work_done(void) {
    std::lock_guard<std::mutex> lock(sLock);
    int64_t now = port_host_time_ns();
    if (sFrameStart) {
        double work = (now - sFrameStart) / 1e6;
        sWork.add(work);
        sRecord.add(sRecordThisFrame / 1e6);
        sFrames++;
        if (sSec.frames < 512) sSec.work[sSec.frames] = (float)work;
        sSec.frames++;
        sSec.recMax = std::max(sSec.recMax, sRecordThisFrame / 1e6);
        long flt = threadFaults() - sFltAtStart;
        sSec.flt += flt;
        if (work >= sSec.worstWork) {
            sSec.worstWork = work;
            sSec.worstFlt = flt;
            sSec.worstRec = sRecordThisFrame / 1e6;
            sSec.worstTex = sTexThisFrame / 1e6;
        }
        if (!sFirstFrame) sFirstFrame = sSecStart = sFrameStart;
    }
    if (now - sWindowStart >= 10000000000ll) {
        report(now);
    }
    if (sSecStart && now - sSecStart >= 1000000000ll) {
        reportSecond(now);
    }
}

void port_perf_tex_check(int64_t ns) {
    std::lock_guard<std::mutex> lock(sLock);
    sSec.texChk += ns / 1e6;
    sTexThisFrame += ns;
}

void port_perf_tex_decode(int64_t ns, uint64_t bytes) {
    std::lock_guard<std::mutex> lock(sLock);
    sSec.texDec.add(ns / 1e6);
    sSec.texDecBytes += bytes;
}

void port_perf_tex_upload(int64_t ns, unsigned count, uint64_t bytes) {
    std::lock_guard<std::mutex> lock(sLock);
    sSec.upload.add(ns / 1e6);
    sSec.upCount += count;
    sSec.upBytes += bytes;
}

void port_perf_programs(unsigned missed, unsigned created) {
    std::lock_guard<std::mutex> lock(sLock);
    sSec.progMiss += missed;
    sSec.progNew += created;
}

void port_perf_record(int64_t ns) {
    std::lock_guard<std::mutex> lock(sLock);
    sRecordThisFrame += ns;
}

void port_perf_prepare(int64_t ns) {
    std::lock_guard<std::mutex> lock(sLock);
    sPrepare.add(ns / 1e6);
    sSec.build.add(ns / 1e6);
}

void port_perf_gpu_wait(int64_t ns) {
    std::lock_guard<std::mutex> lock(sLock);
    sGpuWait.add(ns / 1e6);
}

void port_perf_render(int64_t cpuNs, int64_t uploadNs) {
    std::lock_guard<std::mutex> lock(sLock);
    sRenderCpu.add(cpuNs / 1e6);
    sUpload.add(uploadNs / 1e6);
    sRenderFrames++;
    sSec.render.add(cpuNs / 1e6);
}

}  // extern "C"
