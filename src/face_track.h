#pragma once
// Face tracking for live filters: SCRFD detector + 106-point landmarks
// (insightface 2d106det), both ONNX Runtime. A worker thread chews the
// latest submitted frame so the UI never blocks; results are EMA-smoothed
// so warps don't jitter.
//
// Landmark layout (verified empirically on take frames):
//   0–32  face contour (chin ≈ 16)
//   33–42 eye A     43–51 brow A
//   52–71 mouth (outer 52–63, inner 64–71)
//   72–86 nose
//   87–96 eye B     97–105 brow B
#include <cstdint>

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
