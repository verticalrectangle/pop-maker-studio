#pragma once
// perf.h — preview hot-path timers + UI frame-time ring (th/perf).
//
// Lightweight, lock-free-enough for the main thread. Workers only touch the
// async decode accumulator via atomics. No dependency on AppState so video.cpp,
// canvas.cpp, main.cpp and ipc_server.cpp can all record without cycles.
#include <cstdint>

namespace perf {

// Per-stage timers. Call record(stage, ms) around each preview stage on the
// main thread; stages are fixed (index by the Stage enum).
enum Stage {
    S_PREFETCH = 0,  // video_prefetch_frames submit (async dispatch only)
    S_DECODE,        // async worker CPU decode (accumulated, per UI frame)
    S_UPLOAD,        // main-thread GL uploads from completed frames
    S_CLIPFX,        // glass/body/runtime/face FX per clip
    S_COMPOSITE,     // scene compositor ops (layers, track FX, blit)
    S_TEXT,          // text layout + scene text layers
    S_SHAPES,        // shape tessellation + layers
    S_SCRIPT,        // Script clips: JS render + Skia record/flush + passes (CPU)
    S_SWAP,          // glfwSwapBuffers (vsync wait included)
    S_COUNT
};

void record(Stage s, double ms);
// Workers call this with CPU decode time; it folds into S_DECODE once per
// UI frame (drain_decode folds the accumulator and clears it).
void record_decode_cpu(double ms);
double drain_decode();

// UI frame-time ring (ms). frame_sample once per presented frame.
void frame_sample(double dt_ms);
void frame_percentiles(double& p50, double& p95, double& p99, double& maxv);
void frame_reset();

// Last/EMA/max snapshot per stage (ms).
struct StageStats { double last = 0.0, ema = 0.0, max = 0.0; uint64_t n = 0; };
void stage_stats(Stage s, StageStats& out);
const char* stage_name(Stage s);

}  // namespace perf
