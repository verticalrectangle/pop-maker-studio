#include "face_track.h"
#include "paths.h"
#include "video.h"

#include <onnxruntime_cxx_api.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
#include <cmath>

namespace fs = std::filesystem;

// ── Sessions ──────────────────────────────────────────────────────────────────

static Ort::Env& ort_env() {
    static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "pms-face");
    return env;
}

static std::unique_ptr<Ort::Session> g_det, g_lmk, g_bls;
static std::mutex g_init_mtx;
static bool g_init_tried = false;

static std::string face_models_dir() { return app_models_dir() + "/face"; }

bool face_track_available() {
    return fs::exists(face_models_dir() + "/yunet.onnx") &&
           fs::exists(face_models_dir() + "/face_landmarks_v2.onnx") &&
           fs::exists(face_models_dir() + "/face_blendshapes.onnx");
}

static bool ensure_sessions() {
    std::lock_guard<std::mutex> lk(g_init_mtx);
    if (g_det && g_lmk && g_bls) return true;
    if (g_init_tried) return false;
    g_init_tried = true;
    if (!face_track_available()) return false;
    try {
        Ort::SessionOptions o;
        o.SetIntraOpNumThreads(4);
        o.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        g_det = std::make_unique<Ort::Session>(
            ort_env(), (face_models_dir() + "/yunet.onnx").c_str(), o);
        g_lmk = std::make_unique<Ort::Session>(
            ort_env(), (face_models_dir() + "/face_landmarks_v2.onnx").c_str(), o);
        g_bls = std::make_unique<Ort::Session>(
            ort_env(), (face_models_dir() + "/face_blendshapes.onnx").c_str(), o);
        return true;
    } catch (...) {
        g_det.reset(); g_lmk.reset(); g_bls.reset();
        return false;
    }
}

// The 146-landmark subset the blendshape net consumes, in image pixel coords
// (kLandmarksSubsetIdxs — mediapipe face_blendshapes_graph.cc).
static const int kBlendSubset[146] = {
    0,1,4,5,6,7,8,10,13,14,17,21,33,37,39,40,46,52,53,54,55,58,61,63,65,66,67,70,78,80,
    81,82,84,87,88,91,93,95,103,105,107,109,127,132,133,136,144,145,146,148,149,150,152,153,154,155,157,158,159,160,
    161,162,163,168,172,173,176,178,181,185,191,195,197,234,246,249,251,263,267,269,270,276,282,283,284,285,288,291,293,295,
    296,297,300,308,310,311,312,314,317,318,321,323,324,332,334,336,338,356,361,362,365,373,374,375,377,378,379,380,381,382,
    384,385,386,387,388,389,390,397,398,400,402,405,409,415,454,466,468,469,470,471,472,473,474,475,476,477};

// ── Inference ─────────────────────────────────────────────────────────────────
// Worker-thread only (or the sync path) — scratch buffers are static.

static constexpr int DET_S = 640;   // YuNet export is fixed 640x640

// Detect-only pass → face box. The worker calls it sparsely; between detects
// the landmark net tracks from its own previous output (a few ms), which is
// what makes the live filters feel realtime.
// YuNet decode (opencv_zoo face_detection_yunet): per-stride grids, score =
// sqrt(cls·obj); box center = (cell + bbox[0..1])·stride, size =
// exp(bbox[2..3])·stride; kps offsets decode like the center. Input is raw
// 0-255 BGR NCHW (no normalization).
static bool detect_face(const uint8_t* rgb, int w, int h,
                        float& bx1, float& by1, float& bx2, float& by2,
                        float* score_out = nullptr, float* kps_out = nullptr) {
    if (!ensure_sessions()) return false;
    static std::vector<float> blob;
    blob.assign((size_t)3 * DET_S * DET_S, 0.f);
    float scale = std::min((float)DET_S / w, (float)DET_S / h);
    int nw = (int)(w * scale), nh = (int)(h * scale);
    for (int y = 0; y < nh; ++y) {
        int sy = std::min(h - 1, (int)(y / scale));
        const uint8_t* row = rgb + (size_t)sy * w * 3;
        for (int x = 0; x < nw; ++x) {
            int sx = std::min(w - 1, (int)(x / scale));
            // RGB frame → BGR planes
            blob[(size_t)0 * DET_S * DET_S + (size_t)y * DET_S + x] = (float)row[sx * 3 + 2];
            blob[(size_t)1 * DET_S * DET_S + (size_t)y * DET_S + x] = (float)row[sx * 3 + 1];
            blob[(size_t)2 * DET_S * DET_S + (size_t)y * DET_S + x] = (float)row[sx * 3 + 0];
        }
    }
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    int64_t det_shape[4] = {1, 3, DET_S, DET_S};
    Ort::Value det_in = Ort::Value::CreateTensor<float>(
        mem, blob.data(), blob.size(), det_shape, 4);

    const char* det_in_names[]  = {"input"};
    const char* det_out_names[] = {"cls_8", "cls_16", "cls_32",
                                   "obj_8", "obj_16", "obj_32",
                                   "bbox_8", "bbox_16", "bbox_32",
                                   "kps_8", "kps_16", "kps_32"};
    std::vector<Ort::Value> det_out;
    try {
        det_out = g_det->Run(Ort::RunOptions{nullptr}, det_in_names, &det_in, 1,
                             det_out_names, 12);
    } catch (...) { return false; }

    float best_score = 0.55f;
    bx1 = by1 = bx2 = by2 = 0.f;
    float kps[10] = {};
    const int strides[3] = {8, 16, 32};
    for (int s = 0; s < 3; ++s) {
        const float* cls = det_out[s].GetTensorData<float>();
        const float* obj = det_out[s + 3].GetTensorData<float>();
        const float* bb  = det_out[s + 6].GetTensorData<float>();
        const float* kp  = det_out[s + 9].GetTensorData<float>();
        int side = DET_S / strides[s];
        int n = side * side;
        for (int j = 0; j < n; ++j) {
            float c = cls[j]; if (c < 0.f) c = 0.f; if (c > 1.f) c = 1.f;
            float o = obj[j]; if (o < 0.f) o = 0.f; if (o > 1.f) o = 1.f;
            float score = sqrtf(c * o);
            if (score <= best_score) continue;
            best_score = score;
            float col = (float)(j % side), row_ = (float)(j / side);
            float cx = (col + bb[j*4+0]) * strides[s];
            float cy = (row_ + bb[j*4+1]) * strides[s];
            float bw = expf(bb[j*4+2]) * strides[s];
            float bh = expf(bb[j*4+3]) * strides[s];
            bx1 = cx - bw * 0.5f; by1 = cy - bh * 0.5f;
            bx2 = cx + bw * 0.5f; by2 = cy + bh * 0.5f;
            // 5-point kps (eyeR, eyeL, nose, mouthR, mouthL in YuNet order —
            // consumers only compare eye-pair vs mouth-pair y, so order
            // within each pair doesn't matter).
            for (int k = 0; k < 5; ++k) {
                kps[k*2]   = (kp[j*10 + k*2]   + col)  * strides[s];
                kps[k*2+1] = (kp[j*10 + k*2+1] + row_) * strides[s];
            }
        }
    }
    if (getenv("PMS_FACE_DEBUG"))
        fprintf(stderr, "[face] yunet best=%.3f box=(%.0f,%.0f,%.0f,%.0f)\n",
                best_score, bx1, by1, bx2, by2);
    if (best_score <= 0.55f) return false;
    bx1 /= scale; by1 /= scale; bx2 /= scale; by2 /= scale;
    if (score_out) *score_out = best_score;
    if (kps_out)
        for (int k = 0; k < 10; ++k) kps_out[k] = kps[k] / scale;
    return true;
}

// Landmarks from a face box (any source: detector or previous landmarks).
// Roll of a face from its eye corners — drives the roll-normalized crop.
static float roll_of(const FaceObs& o) {
    return atan2f(o.pts[263][1] - o.pts[33][1], o.pts[263][0] - o.pts[33][0]);
}

// `roll` rotates the sampling grid so the face lands UPRIGHT in the crop —
// the landmark model badly degrades past ~30° of head tilt (Alexis's rotation
// test: liner on the forehead, lip tint on the nose). MediaPipe's own
// pipeline does exactly this; feeding raw crops was the rigid part.
static bool landmarks_from_box(const uint8_t* rgb, int w, int h,
                               float bx1, float by1, float bx2, float by2,
                               FaceObs& out, float roll = 0.f) {
    if (!ensure_sessions()) return false;
    out = FaceObs{};
    out.w = w; out.h = h;
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // ── Landmarks: square crop 1.6× the box, 256×256 NHWC RGB in [0,1] ───────
    float cx = (bx1 + bx2) * 0.5f, cy = (by1 + by2) * 0.5f;
    float size = std::max(bx2 - bx1, by2 - by1) * 1.6f;
    float half = size * 0.5f;
    float cr = cosf(roll), sr = sinf(roll);
    static std::vector<float> crop;
    crop.assign((size_t)256 * 256 * 3, 0.f);
    for (int y = 0; y < 256; ++y) {
        float ly = (y / 256.f) * size - half;
        for (int x = 0; x < 256; ++x) {
            float lx = (x / 256.f) * size - half;
            int sx = (int)(cx + lx * cr - ly * sr);
            int sy = (int)(cy + lx * sr + ly * cr);
            if (sx < 0) sx = 0; if (sx >= w) sx = w - 1;
            if (sy < 0) sy = 0; if (sy >= h) sy = h - 1;
            const uint8_t* px = rgb + ((size_t)sy * w + sx) * 3;
            float* dst = &crop[((size_t)y * 256 + x) * 3];
            dst[0] = (float)px[0] / 255.f;
            dst[1] = (float)px[1] / 255.f;
            dst[2] = (float)px[2] / 255.f;
        }
    }
    int64_t lmk_shape[4] = {1, 256, 256, 3};
    Ort::Value lmk_in = Ort::Value::CreateTensor<float>(
        mem, crop.data(), crop.size(), lmk_shape, 4);
    const char* lmk_in_names[]  = {"input_12"};
    const char* lmk_out_names[] = {"Identity", "Identity_1"};  // mesh, face flag
    std::vector<Ort::Value> lmk_out;
    try {
        lmk_out = g_lmk->Run(Ort::RunOptions{nullptr}, lmk_in_names, &lmk_in, 1,
                             lmk_out_names, 2);
    } catch (...) { return false; }
    const float* mesh = lmk_out[0].GetTensorData<float>();   // 478 × (x,y,z), crop px
    float flag = lmk_out[1].GetTensorData<float>()[0];
    float conf = 1.f / (1.f + expf(-flag));
    if (getenv("PMS_FACE_DEBUG"))
        fprintf(stderr, "[face] mesh conf=%.3f (flag=%.2f)\n", conf, flag);
    if (conf < 0.45f) return false;                          // no face in the crop
    // Rotate landmarks back into frame coords; keep the UPRIGHT (crop-space)
    // coords too — the blendshape net wants an upright face as much as the
    // landmark net does (tilted blinks misfired before this).
    static float up_pts[FT_NPTS][2];
    for (int k = 0; k < FT_NPTS; ++k) {
        float lx = mesh[k*3]   / 256.f * size - half;
        float ly = mesh[k*3+1] / 256.f * size - half;
        out.pts[k][0] = cx + lx * cr - ly * sr;
        out.pts[k][1] = cy + lx * sr + ly * cr;
        up_pts[k][0] = cx + lx;
        up_pts[k][1] = cy + ly;
    }

    // ── Blendshapes: 146-landmark subset (upright coords) → 52 coefficients ──
    static float sub[146 * 2];
    for (int k = 0; k < 146; ++k) {
        sub[k*2]   = up_pts[kBlendSubset[k]][0];
        sub[k*2+1] = up_pts[kBlendSubset[k]][1];
    }
    int64_t bls_shape[3] = {1, 146, 2};
    Ort::Value bls_in = Ort::Value::CreateTensor<float>(
        mem, sub, 146 * 2, bls_shape, 3);
    const char* bls_in_names[]  = {"serving_default_input_points:0"};
    const char* bls_out_names[] = {"StatefulPartitionedCall:0"};
    try {
        auto bls_out = g_bls->Run(Ort::RunOptions{nullptr}, bls_in_names, &bls_in, 1,
                                  bls_out_names, 1);
        const float* bv = bls_out[0].GetTensorData<float>();
        for (int k = 0; k < FT_NBLEND; ++k) out.blend[k] = bv[k];
        out.has_blend = true;
    } catch (...) { out.has_blend = false; }   // mesh still usable without

    out.score = conf;
    out.valid = true;
    return true;
}

// Roll ladder: run the fast path (expected roll) first; if the model isn't
// confident, fan out over candidate angles and keep the best. A face tilted
// past what the landmark net tolerates (~30°) is unfindable from a single
// bad prior — the ladder recovers from any angle at the cost of a few extra
// ms on failure frames only.
static bool landmarks_best_roll(const uint8_t* rgb, int w, int h,
                                float b1, float b2, float b3, float b4,
                                FaceObs& out, float expect_roll) {
    bool fast_ok = landmarks_from_box(rgb, w, h, b1, b2, b3, b4, out, expect_roll);
    if (fast_ok && out.score >= 0.80f) return true;
    // The net's confidence is NOT a reliable arbiter between roll candidates
    // (it can be confidently wrong on off-axis crops) — so the expected-roll
    // result is the INCUMBENT: a challenger must beat it by a clear margin.
    FaceObs best = out;
    bool have = fast_ok;
    float bar = fast_ok ? out.score + 0.08f : 0.f;
    const float cands[8] = {0.f, 0.55f, -0.55f, 1.1f, -1.1f, 1.5708f, -1.5708f, 3.14159f};
    for (float c : cands) {
        if (fabsf(c - expect_roll) < 0.15f) continue;
        FaceObs trial;
        if (landmarks_from_box(rgb, w, h, b1, b2, b3, b4, trial, c) &&
            trial.score > bar) {
            best = trial; bar = trial.score; have = true;
            if (bar >= 0.85f) break;
        }
    }
    out = best;
    return have && best.valid;
}

// Full pipeline (sync path / cold start).
static bool run_inference(const uint8_t* rgb, int w, int h, FaceObs& out) {
    float b1, b2, b3, b4;
    if (!detect_face(rgb, w, h, b1, b2, b3, b4)) return false;
    return landmarks_from_box(rgb, w, h, b1, b2, b3, b4, out);
}

// ── Worker (live path) ────────────────────────────────────────────────────────

static std::mutex              g_work_mtx;
static std::condition_variable g_work_cv;
static std::vector<uint8_t>    g_pending;        // latest submitted frame
static int                     g_pend_w = 0, g_pend_h = 0;
static bool                    g_pend_fresh = false;
static std::atomic<bool>       g_worker_quit{false};
static std::thread             g_worker;
static bool                    g_worker_started = false;

static std::mutex g_latest_mtx;
// Velocity-adaptive smoothing (One-Euro spirit): each landmark picks its own
// alpha from its own speed — still points smooth hard (no jitter), moving
// points snap (no trailing makeup during speech or head turns). The old
// global 0.65 EMA gated on NOSE velocity lagged the mouth by ~3 frames.
static inline float adaptive_alpha(float speed_px, float frame_w) {
    float a = 0.35f + (speed_px / frame_w) * 45.f;
    return a > 1.f ? 1.f : a;
}
static FaceObs    g_latest;        // adaptively smoothed
static FaceObs    g_raw_prev;
std::atomic<int>  g_dbg_flip180{0};      // worker debug state (IPC readout)
std::atomic<int>  g_dbg_since_detect{0};
std::atomic<int>  g_dbg_detects{0};

static void rot180(std::vector<uint8_t>& f, int w, int h) {
    const size_t n = (size_t)w * h;
    for (size_t i = 0, j = n - 1; i < j; ++i, --j) {
        for (int c = 0; c < 3; ++c)
            std::swap(f[i*3 + c], f[j*3 + c]);
    }
}

// Orientation sanity: a frame handed to the landmark net is upright by
// contract, so a valid face must have its eyes ABOVE the chin (image y grows
// down). The landmark net happily returns a plausible-looking but WRONG pose
// for an upside-down crop — without this check a stale flip state poses
// every overlay as if the face looked another way.
static bool upright_ok(const FaceObs& o) {
    // Roll-aware: the old check (chin strictly below the eyes in IMAGE y)
    // rejected legitimately tilted heads past ~60°. The real job is to
    // arbitrate 180°-flip candidates — so only reject when the face is
    // closer to upside-down than upright.
    float exd = (o.pts[468][0] + o.pts[473][0]) * 0.5f - o.pts[152][0];
    float eyd = (o.pts[468][1] + o.pts[473][1]) * 0.5f - o.pts[152][1];
    // eyes-to-chin vector should point generally UP (negative y); allow any
    // tilt short of past-horizontal-plus (~115°).
    return atan2f(fabsf(exd), -eyd) < 2.0f;
}

// Garbage-landmark guard: the landmark net answers SOMETHING for any crop; a
// real face fills most of it. A collapsed cluster (tiny fraction of the box
// it was cropped from) is garbage — it can pass the RELATIVE upright check,
// and the tracking loop then crops ever smaller around it, locking the
// collapse in at "score 1.0".
static bool lm_sane(const FaceObs& o, float b1, float b2, float b3, float b4) {
    float x0 = o.pts[0][0], x1 = x0, y0 = o.pts[0][1], y1 = y0;
    for (int k = 1; k < FT_NPTS; ++k) {
        x0 = std::min(x0, o.pts[k][0]); x1 = std::max(x1, o.pts[k][0]);
        y0 = std::min(y0, o.pts[k][1]); y1 = std::max(y1, o.pts[k][1]);
    }
    if (x1 - x0 < 12.f || y1 - y0 < 12.f) return false;
    return x1 - x0 >= (b3 - b1) * 0.35f &&
           y1 - y0 >= (b4 - b2) * 0.35f;
}

// Detect-box geometry carried through tracking mode. The landmark net is
// trained on DETECTOR-shaped crops (forehead included, sitting higher and
// larger than the landmark extent). Cropping from the previous landmark
// bbox instead makes the net's output drift a little every frame — the
// pose walks off the face and locks onto a stable wrong answer (kodim04:
// the whole set climbed onto the hat at score 1.0). So tracking mode
// re-centres the LAST DETECTOR BOX on the tracked face instead.
struct DetBoxGeom {
    bool  valid = false;
    float w = 0, h = 0;    // detector box size
    float dx = 0, dy = 0;  // box center minus landmark centroid (submitted coords)
};

static void lm_centroid(const FaceObs& o, float& cx, float& cy) {
    cx = cy = 0.f;
    for (int k = 0; k < FT_NPTS; ++k) { cx += o.pts[k][0]; cy += o.pts[k][1]; }
    cx /= (float)FT_NPTS; cy /= (float)FT_NPTS;
}

static void store_box_geom(const FaceObs& obs_submitted, const float box[4],
                           DetBoxGeom& g) {
    float cx, cy;
    lm_centroid(obs_submitted, cx, cy);
    g.w  = box[2] - box[0];
    g.h  = box[3] - box[1];
    g.dx = (box[0] + box[2]) * 0.5f - cx;
    g.dy = (box[1] + box[3]) * 0.5f - cy;
    g.valid = true;
}

static void crop_box_from_prev(const FaceObs& prev, const DetBoxGeom& g,
                               float& b1, float& b2, float& b3, float& b4) {
    float cx, cy;
    lm_centroid(prev, cx, cy);
    b1 = cx + g.dx - g.w * 0.5f;
    b2 = cy + g.dy - g.h * 0.5f;
    b3 = b1 + g.w;
    b4 = b2 + g.h;
}

// Re-detect helper shared by the live worker and the offline take pass.
// SCRFD fires on an UPSIDE-DOWN face too, just at a lower score (lena rig:
// 0.66 upside-down vs 0.76 upright) — so never short-circuit on the first
// orientation that detects. Score both, landmark the better one first, fall
// back to the other. `frame` carries the current flip state on entry;
// flip180 is updated to the orientation that won. Landmarks are returned in
// the WINNING orientation's coordinates (caller flips back if flip180);
// out_box gets the winning detector box, same coordinates.
static bool detect_both_orientations(const std::vector<uint8_t>& frame,
                                     int fw, int fh, bool& flip180,
                                     FaceObs& obs, float out_box[4]) {
    std::vector<uint8_t> raw = frame;
    if (flip180) rot180(raw, fw, fh);       // undo intake flip → raw
    std::vector<uint8_t> flp = raw;
    rot180(flp, fw, fh);
    struct Cand {
        const std::vector<uint8_t>* img;
        bool flipped; bool det; float score; float b[4]; float kps[10];
    } cands[2] = {{&raw, false, false, 0.f, {0,0,0,0}, {}},
                  {&flp, true,  false, 0.f, {0,0,0,0}, {}}};
    for (Cand& cd : cands)
        cd.det = detect_face(cd.img->data(), fw, fh,
                             cd.b[0], cd.b[1], cd.b[2], cd.b[3],
                             &cd.score, cd.kps);
    // SCRFD's own 5-point keypoints land on the REAL features even when it
    // detects an upside-down face (which it does, sometimes at a HIGHER box
    // score than the upright view — kodim04 letterboxed: 0.674 flipped vs
    // 0.636 upright). Box score can't arbitrate orientation; eyes-above-
    // mouth from the kps can. Only when neither candidate passes the gate
    // (kps unreliable) does score order decide alone.
    auto kps_upright = [](const Cand& cd) {
        float eyes  = (cd.kps[1] + cd.kps[3]) * 0.5f;   // eyeL.y, eyeR.y
        float mouth = (cd.kps[7] + cd.kps[9]) * 0.5f;   // mouthL.y, mouthR.y
        return eyes + (cd.b[3] - cd.b[1]) * 0.02f < mouth;
    };
    bool up0 = cands[0].det && kps_upright(cands[0]);
    bool up1 = cands[1].det && kps_upright(cands[1]);
    if (up0 || up1) {
        cands[0].det = cands[0].det && up0;
        cands[1].det = cands[1].det && up1;
    }
    if (cands[1].det && (!cands[0].det || cands[1].score > cands[0].score))
        std::swap(cands[0], cands[1]);
    for (Cand& cd : cands) {
        if (!cd.det) continue;
        bool got = landmarks_best_roll(cd.img->data(), fw, fh,
                                       cd.b[0], cd.b[1], cd.b[2], cd.b[3],
                                       obs, 0.f);
        // Refine once with the measured roll (the ladder may have landed on
        // a coarse candidate).
        if (got) {
            float r0 = roll_of(obs);
            FaceObs fine;
            if (landmarks_from_box(cd.img->data(), fw, fh,
                                   cd.b[0], cd.b[1], cd.b[2], cd.b[3],
                                   fine, r0) && fine.score >= obs.score * 0.9f)
                obs = fine;
        }
        if (got && upright_ok(obs) &&
            lm_sane(obs, cd.b[0], cd.b[1], cd.b[2], cd.b[3])) {
            obs.score = cd.score;
            flip180 = cd.flipped;
            for (int i = 0; i < 4; ++i) out_box[i] = cd.b[i];
            return true;
        }
    }
    return false;
}

// Flip a box's coordinates 180° within a fw×fh frame (matches the landmark
// publish flip so box and landmarks stay in the same space).
static void flip_box(float b[4], int fw, int fh) {
    float n1 = (float)fw - 1.f - b[2], n2 = (float)fh - 1.f - b[3];
    float n3 = (float)fw - 1.f - b[0], n4 = (float)fh - 1.f - b[1];
    b[0] = n1; b[1] = n2; b[2] = n3; b[3] = n4;
}

static void worker_main() {
    std::vector<uint8_t> frame;
    int fw = 0, fh = 0;
    static bool s_flip180 = false;   // camera mounted upside down — detected
    while (!g_worker_quit.load()) {
        {
            std::unique_lock<std::mutex> lk(g_work_mtx);
            g_work_cv.wait(lk, [] { return g_pend_fresh || g_worker_quit.load(); });
            if (g_worker_quit.load()) return;
            frame = g_pending;          // copy: keep last frame for debug dump
            fw = g_pend_w; fh = g_pend_h;
            g_pend_fresh = false;
        }
        if (s_flip180) rot180(frame, fw, fh);
        // Tracking mode: while locked, skip the heavy detector and crop a
        // DETECT-SHAPED box re-centred on the previous landmarks (the
        // landmark net is a few ms, so the loop runs at camera rate).
        // Re-detect on loss or every ~2 s.
        static int s_since_detect = 0;
        static DetBoxGeom s_boxg;
        FaceObs obs;
        bool ok = false;
        bool have_prev = false;
        float pb1 = 0, pb2 = 0, pb3 = 0, pb4 = 0;
        {
            std::lock_guard<std::mutex> lk(g_latest_mtx);
            if (g_latest.valid && g_latest.w == fw && g_latest.h == fh &&
                s_boxg.valid) {
                have_prev = true;
                crop_box_from_prev(g_latest, s_boxg, pb1, pb2, pb3, pb4);
            }
        }
        if (have_prev && s_flip180) {
            // prev landmarks are in submitted coords; the working frame is
            // flipped — flip the crop box to match.
            float nb1 = (float)fw - 1.f - pb3, nb2 = (float)fh - 1.f - pb4;
            float nb3 = (float)fw - 1.f - pb1, nb4 = (float)fh - 1.f - pb2;
            pb1 = nb1; pb2 = nb2; pb3 = nb3; pb4 = nb4;
        }
        if (have_prev && ++s_since_detect < 60) {
            ok = landmarks_best_roll(frame.data(), fw, fh, pb1, pb2, pb3, pb4, obs,
                                     g_latest.valid ? roll_of(g_latest) : 0.f);
            if (ok && (!upright_ok(obs) ||
                       !lm_sane(obs, pb1, pb2, pb3, pb4)))
                ok = false;                       // wrong pose → re-detect
        }
        bool fresh_detect = false;
        float det_box[4] = {0, 0, 0, 0};
        if (!ok) {
            s_since_detect = 0;
            ok = detect_both_orientations(frame, fw, fh, s_flip180, obs, det_box);
            fresh_detect = ok;
        }
        if (ok && s_flip180) {
            // Publish landmarks in the SUBMITTED frame's coordinates.
            for (int k = 0; k < FT_NPTS; ++k) {
                obs.pts[k][0] = (float)fw - 1.f - obs.pts[k][0];
                obs.pts[k][1] = (float)fh - 1.f - obs.pts[k][1];
            }
            if (fresh_detect) flip_box(det_box, fw, fh);
        }
        if (fresh_detect) { store_box_geom(obs, det_box, s_boxg); ++g_dbg_detects; }
        g_dbg_flip180.store(s_flip180 ? 1 : 0);
        g_dbg_since_detect.store(s_since_detect);
        std::lock_guard<std::mutex> lk(g_latest_mtx);
        if (!ok) {
            g_latest.score *= 0.7f;
            if (g_latest.score < 0.15f) g_latest.valid = false;
            continue;
        }
        if (!g_latest.valid || g_latest.w != obs.w || g_latest.h != obs.h) {
            g_latest = obs;            // fresh lock-on
        } else {
            // Jump = new face/cut → snap; light EMA otherwise (the tracking
            // loop runs at camera rate now, so smoothing can be gentle).
            for (int k = 0; k < FT_NPTS; ++k) {
                float ddx = obs.pts[k][0] - g_latest.pts[k][0];
                float ddy = obs.pts[k][1] - g_latest.pts[k][1];
                float alpha = adaptive_alpha(sqrtf(ddx*ddx + ddy*ddy), (float)obs.w);
                g_latest.pts[k][0] += ddx * alpha;
                g_latest.pts[k][1] += ddy * alpha;
            }
            if (obs.has_blend) {
                if (!g_latest.has_blend) {
                    for (int k = 0; k < FT_NBLEND; ++k) g_latest.blend[k] = obs.blend[k];
                } else {
                    for (int k = 0; k < FT_NBLEND; ++k) {
                        float db = obs.blend[k] - g_latest.blend[k];
                        float ab = 0.4f + fabsf(db) * 6.f;
                        g_latest.blend[k] += db * (ab > 1.f ? 1.f : ab);
                    }
                }
                g_latest.has_blend = true;
            }
            g_latest.score = obs.score;
            g_latest.valid = true;
        }
    }
}

// Debug: dump the last submitted frame so orientation bugs are visible.
bool face_track_dump_last(const char* path) {
    std::lock_guard<std::mutex> lk(g_work_mtx);
    if (g_pending.empty() || g_pend_w <= 0) {
        return false;
    }
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", g_pend_w, g_pend_h);
    fwrite(g_pending.data(), 1, g_pending.size(), f);
    fclose(f);
    return true;
}

void face_track_submit(const uint8_t* rgb, int w, int h) {
    if (!face_track_available() || w <= 0 || h <= 0) return;
    if (!g_worker_started) {
        g_worker_started = true;
        g_worker = std::thread(worker_main);
        g_worker.detach();
    }
    std::lock_guard<std::mutex> lk(g_work_mtx);
    g_pending.assign(rgb, rgb + (size_t)w * h * 3);
    g_pend_w = w; g_pend_h = h;
    g_pend_fresh = true;
    g_work_cv.notify_one();
}

bool face_track_latest(FaceObs& out) {
    std::lock_guard<std::mutex> lk(g_latest_mtx);
    out = g_latest;
    return out.valid;
}

bool face_track_run_sync(const uint8_t* rgb, int w, int h, FaceObs& out) {
    return run_inference(rgb, w, h, out);
}

void face_track_shutdown() {
    g_worker_quit.store(true);
    g_work_cv.notify_all();
}

// ── Offline take pass ─────────────────────────────────────────────────────────
// Decode a take at half-res, rotated upright per the brick rotation (ffmpeg
// transpose matches the live mirror's rotate-upright submit exactly), run the
// same detect-sparse/track-dense loop with the same sanity checks and EMA,
// and write per-frame landmarks in RAW full-res coords. ~850 B/frame.

bool face_track_build_cache(const std::string& video_path, int rot_q,
                            const std::string& out_path,
                            const std::function<void(float)>& progress,
                            const std::atomic<bool>* cancel) {
    if (!face_track_available()) return false;
    MediaFileInfo info = video_probe_file(video_path);
    if (!info.has_video || info.width <= 0 || info.fps <= 0.0) return false;
    const int W = info.width, H = info.height;
    const int hw2 = W / 2, hh2 = H / 2;
    rot_q = ((rot_q % 4) + 4) % 4;
    // Upright half-res dims the tracker sees (90° steps swap them).
    const int fw = (rot_q == 1 || rot_q == 3) ? hh2 : hw2;
    const int fh = (rot_q == 1 || rot_q == 3) ? hw2 : hh2;
    const char* xpose = rot_q == 1 ? ",transpose=1"
                      : rot_q == 3 ? ",transpose=2"
                      : rot_q == 2 ? ",hflip,vflip" : "";
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -v error -i '%s' -vf 'scale=%d:%d%s' "
             "-f rawvideo -pix_fmt rgb24 - 2>/dev/null",
             video_path.c_str(), hw2, hh2, xpose);
    FILE* p = popen(cmd, "r");
    if (!p) return false;

    const int total_est = (int)(info.duration * info.fps + 0.5);
    const size_t frame_bytes = (size_t)hw2 * hh2 * 3;   // pre-transpose == post (same pixel count)
    std::vector<uint8_t> frame(frame_bytes);
    // per frame: score + FT_NPTS*2 (raw full-res) + FT_NBLEND
    const size_t REC = 1 + (size_t)FT_NPTS * 2 + FT_NBLEND;
    std::vector<float> records;
    records.reserve((size_t)(total_est > 0 ? total_est : 256) * REC);

    bool flip180 = false;
    int since_detect = 60;          // force detect on the first frame
    DetBoxGeom boxg;
    FaceObs smooth; smooth.valid = false;
    int n = 0;
    const float hw2f = (float)hw2, hh2f = (float)hh2;

    while (fread(frame.data(), 1, frame_bytes, p) == frame_bytes) {
        if (cancel && cancel->load()) { pclose(p); return false; }
        if (flip180) rot180(frame, fw, fh);
        FaceObs obs;
        bool ok = false;
        if (smooth.valid && boxg.valid && ++since_detect < 60) {
            float b1, b2, b3, b4;
            crop_box_from_prev(smooth, boxg, b1, b2, b3, b4);
            if (flip180) {
                float n1 = (float)fw - 1.f - b3, n2 = (float)fh - 1.f - b4;
                float n3 = (float)fw - 1.f - b1, n4 = (float)fh - 1.f - b2;
                b1 = n1; b2 = n2; b3 = n3; b4 = n4;
            }
            ok = landmarks_best_roll(frame.data(), fw, fh, b1, b2, b3, b4, obs,
                                     smooth.valid ? roll_of(smooth) : 0.f);
            if (ok && (!upright_ok(obs) || !lm_sane(obs, b1, b2, b3, b4)))
                ok = false;
        }
        bool fresh_detect = false;
        float det_box[4] = {0, 0, 0, 0};
        if (!ok) {
            since_detect = 0;
            ok = detect_both_orientations(frame, fw, fh, flip180, obs, det_box);
            fresh_detect = ok;
        }
        if (ok && flip180) {
            for (int k = 0; k < FT_NPTS; ++k) {
                obs.pts[k][0] = (float)fw - 1.f - obs.pts[k][0];
                obs.pts[k][1] = (float)fh - 1.f - obs.pts[k][1];
            }
            if (fresh_detect) flip_box(det_box, fw, fh);
        }
        if (fresh_detect) store_box_geom(obs, det_box, boxg);
        if (!ok) {
            smooth.score *= 0.7f;
            if (smooth.score < 0.15f) smooth.valid = false;
        } else if (!smooth.valid) {
            smooth = obs;
        } else {
            for (int k = 0; k < FT_NPTS; ++k) {
                float ddx = obs.pts[k][0] - smooth.pts[k][0];
                float ddy = obs.pts[k][1] - smooth.pts[k][1];
                float alpha = adaptive_alpha(sqrtf(ddx*ddx + ddy*ddy), (float)fw);
                smooth.pts[k][0] += ddx * alpha;
                smooth.pts[k][1] += ddy * alpha;
            }
            if (obs.has_blend) {
                if (!smooth.has_blend) {
                    for (int k = 0; k < FT_NBLEND; ++k) smooth.blend[k] = obs.blend[k];
                } else {
                    for (int k = 0; k < FT_NBLEND; ++k) {
                        float db = obs.blend[k] - smooth.blend[k];
                        float ab = 0.4f + fabsf(db) * 6.f;
                        smooth.blend[k] += db * (ab > 1.f ? 1.f : ab);
                    }
                }
                smooth.has_blend = true;
            }
            smooth.score = obs.score;
        }
        // Record in RAW full-res coords (same remap as the live mirror).
        records.push_back(smooth.valid ? smooth.score : 0.f);
        if (!smooth.valid) {
            records.insert(records.end(), REC - 1, 0.f);
            ++n;
            continue;
        }
        for (int k = 0; k < FT_NPTS; ++k) {
            float ux = smooth.pts[k][0], uy = smooth.pts[k][1];
            float rx2, ry2;
            if (rot_q == 1)      { rx2 = uy;              ry2 = hh2f - 1.f - ux; }
            else if (rot_q == 3) { rx2 = hw2f - 1.f - uy; ry2 = ux; }
            else if (rot_q == 2) { rx2 = hw2f - 1.f - ux; ry2 = hh2f - 1.f - uy; }
            else                 { rx2 = ux;              ry2 = uy; }
            records.push_back(rx2 * 2.f);
            records.push_back(ry2 * 2.f);
        }
        for (int k = 0; k < FT_NBLEND; ++k)
            records.push_back(smooth.has_blend ? smooth.blend[k] : 0.f);
        ++n;
        if (progress && total_est > 0 && (n & 7) == 0)
            progress((float)n / (float)total_est);
    }
    pclose(p);
    if (n == 0) return false;

    std::string tmp = out_path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    uint32_t magic = 0x46534D50, version = 8;   // v8: incumbent-margin roll arbitration   // 'PMSF'
    int32_t  rq = rot_q, rw = W, rh = H;
    float    fps = (float)info.fps;
    uint32_t count = (uint32_t)n;
    fwrite(&magic, 4, 1, f);   fwrite(&version, 4, 1, f);
    fwrite(&rq, 4, 1, f);      fwrite(&fps, 4, 1, f);
    fwrite(&rw, 4, 1, f);      fwrite(&rh, 4, 1, f);
    fwrite(&count, 4, 1, f);
    fwrite(records.data(), sizeof(float), records.size(), f);
    bool wok = !ferror(f);
    fclose(f);
    if (!wok) { ::remove(tmp.c_str()); return false; }
    ::remove(out_path.c_str());
    if (::rename(tmp.c_str(), out_path.c_str()) != 0) return false;
    if (progress) progress(1.f);
    return true;
}
