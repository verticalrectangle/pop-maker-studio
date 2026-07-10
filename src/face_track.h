#pragma once
// Face tracking for live filters: YuNet detector (MIT, OpenCV Zoo) +
// MediaPipe Face Landmarker v2 (Apache 2.0): 478-point mesh with iris, plus
// the 52 ARKit-style blendshape coefficients that make filters expression-
// reactive (jawOpen drives the Doggy tongue, blinks squash BigEyes, ...).
// All three models run through ONNX Runtime. A worker thread chews the
// latest submitted frame so the UI never blocks; results are EMA-smoothed
// so warps don\'t jitter.
//
// This replaced the InsightFace SCRFD + 2d106det pair (2026-07): equivalent
// pipeline, permissive licenses (the InsightFace WEIGHTS are non-commercial),
// 4.5x the landmark density, and blendshapes the old stack couldn\'t do.
//
// Canonical mesh indices used across the app (MediaPipe face mesh):
//   468/473  iris centers (eye A/B)     33+133 / 362+263  eye corners
//   1 nose tip · 98/327 nose wings · 13/14 inner lip mids · 61/291 mouth corners
//   152 chin · 172/397 lower jaw · 10 forehead top · 234/454 face sides
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

static constexpr int FT_NPTS   = 478;  // mesh points (incl. 10 iris)
static constexpr int FT_NBLEND = 52;   // ARKit-style blendshape coefficients

// Blendshape indices (kBlendshapeNames order — MediaPipe face_blendshapes):
enum : int {
    FB_BROW_INNER_UP = 3,
    FB_EYE_BLINK_L = 9,  FB_EYE_BLINK_R = 10,
    FB_EYE_WIDE_L  = 21, FB_EYE_WIDE_R  = 22,
    FB_JAW_OPEN    = 25,
    FB_MOUTH_SMILE_L = 44, FB_MOUTH_SMILE_R = 45,
};

struct FaceObs {
    bool   valid = false;
    float  pts[FT_NPTS][2];    // pixels in the submitted frame
    float  blend[FT_NBLEND] = {};  // 0..1 coefficients (EMA-smoothed live)
    bool   has_blend = false;
    float  score = 0.f;
    int    w = 0, h = 0;       // frame size the pts live in
};

bool face_track_available();  // models found (models/face/*.onnx)

// Live path: submit copies the frame and wakes the worker (call from the UI
// thread at mirror rate); latest returns the most recent smoothed result.
void face_track_submit(const uint8_t* rgb, int w, int h);
bool face_track_latest(FaceObs& out);

// Camera side-feed gate (iOS record mode): pms_submit_camera_frame only
// converts + submits frames to the face worker while this is on, so plain
// recording pays nothing. Toggled by the face_track_enable command.
void face_feed_enable(bool on);
bool face_feed_enabled();

// Synchronous single-frame run (take analysis pass) — no smoothing.
bool face_track_run_sync(const uint8_t* rgb, int w, int h, FaceObs& out);

void face_track_shutdown();
bool face_track_dump_last(const char* path);  // debug: PPM of last submitted frame

// Offline take pass: decode `video_path` (rotated upright per rot_q, matching
// the live mirror\'s submit), run the tracker over every frame, and write a
// .face cache file: per-frame score + mesh landmarks in RAW full-res frame
// coords + blendshapes. Blocking — run on a background thread (see
// face_cache.h). Returns false on decode/track failure or when `cancel` flips.
bool face_track_build_cache(const std::string& video_path, int rot_q,
                            const std::string& out_path,
                            const std::function<void(float)>& progress,
                            const std::atomic<bool>* cancel);
