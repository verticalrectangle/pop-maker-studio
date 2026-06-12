#pragma once
// Face tracking for live filters: SCRFD detector + 106-point landmarks
// (insightface 2d106det), both ONNX Runtime. A worker thread chews the
// latest submitted frame so the UI never blocks; results are EMA-smoothed
// so warps don't jitter.
//
// Landmark layout (verified empirically on the lena standard rig — the
// contour is a ZIG-ZAG, not a sequential jaw sweep):
//   0     chin bottom center
//   1     left jaw top (ear)   9–16  upper-left jawline (down)
//   2–8   lower-left jaw curving into the chin
//   17    right jaw top        25–32 upper-right jawline (down)
//   18–24 lower-right jaw curving into the chin
//   33–42 eye A     43–51 brow A
//   52–71 mouth (outer 52–63, inner 64–71)
//   72–86 nose
//   87–96 eye B     97–105 brow B
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

struct FaceObs {
    bool   valid = false;
    float  pts[106][2];   // pixels in the submitted frame
    float  score = 0.f;
    int    w = 0, h = 0;  // frame size the pts live in
};

bool face_track_available();  // models found (models/face/*.onnx)

// Live path: submit copies the frame and wakes the worker (call from the UI
// thread at mirror rate); latest returns the most recent smoothed result.
void face_track_submit(const uint8_t* rgb, int w, int h);
bool face_track_latest(FaceObs& out);

// Synchronous single-frame run (take analysis pass) — no smoothing.
bool face_track_run_sync(const uint8_t* rgb, int w, int h, FaceObs& out);

void face_track_shutdown();
bool face_track_dump_last(const char* path);  // debug: PPM of last submitted frame

// Offline take pass: decode `video_path` (rotated upright per rot_q, matching
// the live mirror's submit), run the tracker over every frame, and write a
// .face cache file: per-frame score + 106 landmarks in RAW full-res frame
// coords. Blocking — run on a background thread (see face_cache.h). Returns
// false on decode/track failure or when `cancel` flips.
bool face_track_build_cache(const std::string& video_path, int rot_q,
                            const std::string& out_path,
                            const std::function<void(float)>& progress,
                            const std::atomic<bool>* cancel);
