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

static std::unique_ptr<Ort::Session> g_det, g_lmk;
static std::mutex g_init_mtx;
static bool g_init_tried = false;

static std::string face_models_dir() { return app_models_dir() + "/face"; }

bool face_track_available() {
    return fs::exists(face_models_dir() + "/scrfd_10g.onnx") &&
           fs::exists(face_models_dir() + "/2d106det.onnx");
}

static bool ensure_sessions() {
    std::lock_guard<std::mutex> lk(g_init_mtx);
    if (g_det && g_lmk) return true;
    if (g_init_tried) return false;
    g_init_tried = true;
    if (!face_track_available()) return false;
    try {
        Ort::SessionOptions o;
        o.SetIntraOpNumThreads(4);
        o.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        g_det = std::make_unique<Ort::Session>(
            ort_env(), (face_models_dir() + "/scrfd_10g.onnx").c_str(), o);
        g_lmk = std::make_unique<Ort::Session>(
            ort_env(), (face_models_dir() + "/2d106det.onnx").c_str(), o);
        return true;
    } catch (...) {
        g_det.reset(); g_lmk.reset();
        return false;
    }
}

// ── Inference ─────────────────────────────────────────────────────────────────
// Worker-thread only (or the sync path) — scratch buffers are static.

static constexpr int DET_S = 480;   // letterbox side for SCRFD (dynamic input)

// Detect-only pass → face box. Heavy (~30-60 ms CPU), so the worker calls
// it sparsely; between detects the landmark net tracks from its own previous
// output (a few ms), which is what makes the live filters feel realtime.
static bool detect_face(const uint8_t* rgb, int w, int h,
                        float& bx1, float& by1, float& bx2, float& by2,
                        float* score_out = nullptr) {
    if (!ensure_sessions()) return false;
    // ── Detect: letterbox to DET_S, (x-127.5)/128 ────────────────────────────
    static std::vector<float> blob;
    blob.assign((size_t)3 * DET_S * DET_S, -127.5f / 128.f);  // pad value for 0
    float scale = std::min((float)DET_S / w, (float)DET_S / h);
    int nw = (int)(w * scale), nh = (int)(h * scale);
    for (int y = 0; y < nh; ++y) {
        int sy = std::min(h - 1, (int)(y / scale));
        const uint8_t* row = rgb + (size_t)sy * w * 3;
        for (int x = 0; x < nw; ++x) {
            int sx = std::min(w - 1, (int)(x / scale));
            for (int c = 0; c < 3; ++c)
                blob[(size_t)c * DET_S * DET_S + (size_t)y * DET_S + x] =
                    ((float)row[sx * 3 + c] - 127.5f) / 128.f;
        }
    }
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    int64_t det_shape[4] = {1, 3, DET_S, DET_S};
    Ort::Value det_in = Ort::Value::CreateTensor<float>(
        mem, blob.data(), blob.size(), det_shape, 4);

    const char* det_in_names[]  = {"input.1"};
    const char* det_out_names[] = {"448", "471", "494", "451", "474", "497",
                                   "454", "477", "500"};
    std::vector<Ort::Value> det_out;
    try {
        det_out = g_det->Run(Ort::RunOptions{nullptr}, det_in_names, &det_in, 1,
                             det_out_names, 9);
    } catch (...) { return false; }

    // Best box across strides 8/16/32 (2 anchors per cell, distances*stride).
    float best_score = 0.35f;
    bx1 = by1 = bx2 = by2 = 0.f;
    const int strides[3] = {8, 16, 32};
    for (int s = 0; s < 3; ++s) {
        const float* sc = det_out[s].GetTensorData<float>();
        const float* bb = det_out[s + 3].GetTensorData<float>();
        size_t n = det_out[s].GetTensorTypeAndShapeInfo().GetElementCount();
        int side = DET_S / strides[s];
        for (size_t j = 0; j < n; ++j) {
            if (sc[j] <= best_score) continue;
            int cell = (int)(j / 2);
            float cx = (float)(cell % side) * strides[s];
            float cy = (float)(cell / side) * strides[s];
            best_score = sc[j];
            bx1 = cx - bb[j*4+0] * strides[s];
            by1 = cy - bb[j*4+1] * strides[s];
            bx2 = cx + bb[j*4+2] * strides[s];
            by2 = cy + bb[j*4+3] * strides[s];
        }
    }
    if (best_score <= 0.35f) return false;
    bx1 /= scale; by1 /= scale; bx2 /= scale; by2 /= scale;
    if (score_out) *score_out = best_score;
    return true;
}

// Landmarks from a face box (any source: detector or previous landmarks).
static bool landmarks_from_box(const uint8_t* rgb, int w, int h,
                               float bx1, float by1, float bx2, float by2,
                               FaceObs& out) {
    if (!ensure_sessions()) return false;
    out = FaceObs{};
    out.w = w; out.h = h;
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // ── Landmarks: square crop 1.5× the box, 192×192, raw RGB floats ─────────
    float cx = (bx1 + bx2) * 0.5f, cy = (by1 + by2) * 0.5f;
    float size = std::max(bx2 - bx1, by2 - by1) * 1.5f;
    float half = size * 0.5f;
    static std::vector<float> crop;
    crop.assign((size_t)3 * 192 * 192, 0.f);
    for (int y = 0; y < 192; ++y) {
        int sy = (int)(cy - half + (y / 192.f) * size);
        if (sy < 0) sy = 0; if (sy >= h) sy = h - 1;
        const uint8_t* row = rgb + (size_t)sy * w * 3;
        for (int x = 0; x < 192; ++x) {
            int sx = (int)(cx - half + (x / 192.f) * size);
            if (sx < 0) sx = 0; if (sx >= w) sx = w - 1;
            for (int c = 0; c < 3; ++c)
                crop[(size_t)c * 192 * 192 + (size_t)y * 192 + x] =
                    (float)row[sx * 3 + c];
        }
    }
    int64_t lmk_shape[4] = {1, 3, 192, 192};
    Ort::Value lmk_in = Ort::Value::CreateTensor<float>(
        mem, crop.data(), crop.size(), lmk_shape, 4);
    const char* lmk_in_names[]  = {"data"};
    const char* lmk_out_names[] = {"fc1"};
    std::vector<Ort::Value> lmk_out;
    try {
        lmk_out = g_lmk->Run(Ort::RunOptions{nullptr}, lmk_in_names, &lmk_in, 1,
                             lmk_out_names, 1);
    } catch (...) { return false; }
    const float* fc = lmk_out[0].GetTensorData<float>();
    for (int k = 0; k < 106; ++k) {
        // fc in [-1,1] over the 192 crop → frame pixels
        float px = (fc[k*2]   + 1.f) * 96.f / 192.f * size + (cx - half);
        float py = (fc[k*2+1] + 1.f) * 96.f / 192.f * size + (cy - half);
        out.pts[k][0] = px;
        out.pts[k][1] = py;
    }
    out.score = 1.f;
    out.valid = true;
    return true;
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
static FaceObs    g_latest;        // EMA-smoothed
static FaceObs    g_raw_prev;

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
    float eyy = 0.f;
    for (int k = 33; k < 43; ++k) eyy += o.pts[k][1];
    for (int k = 87; k < 97; ++k) eyy += o.pts[k][1];
    eyy /= 20.f;
    float face_h = fabsf(o.pts[0][1] - eyy);
    return o.pts[0][1] - eyy > face_h * 0.4f;        // chin clearly below
}

// Garbage-landmark guard: the landmark net answers SOMETHING for any crop; a
// real face fills most of it. A collapsed cluster (tiny fraction of the box
// it was cropped from) is garbage — it can pass the RELATIVE upright check,
// and the tracking loop then crops ever smaller around it, locking the
// collapse in at "score 1.0".
static bool lm_sane(const FaceObs& o, float b1, float b2, float b3, float b4) {
    float x0 = o.pts[0][0], x1 = x0, y0 = o.pts[0][1], y1 = y0;
    for (int k = 1; k < 106; ++k) {
        x0 = std::min(x0, o.pts[k][0]); x1 = std::max(x1, o.pts[k][0]);
        y0 = std::min(y0, o.pts[k][1]); y1 = std::max(y1, o.pts[k][1]);
    }
    if (x1 - x0 < 12.f || y1 - y0 < 12.f) return false;
    return x1 - x0 >= (b3 - b1) * 0.35f &&
           y1 - y0 >= (b4 - b2) * 0.35f;
}

static void prev_lm_box(const FaceObs& o, float& b1, float& b2,
                        float& b3, float& b4) {
    b1 = b3 = o.pts[0][0];
    b2 = b4 = o.pts[0][1];
    for (int k = 0; k < 106; ++k) {
        b1 = std::min(b1, o.pts[k][0]);
        b2 = std::min(b2, o.pts[k][1]);
        b3 = std::max(b3, o.pts[k][0]);
        b4 = std::max(b4, o.pts[k][1]);
    }
}

// Re-detect helper shared by the live worker and the offline take pass.
// SCRFD fires on an UPSIDE-DOWN face too, just at a lower score (lena rig:
// 0.66 upside-down vs 0.76 upright) — so never short-circuit on the first
// orientation that detects. Score both, landmark the better one first, fall
// back to the other. `frame` carries the current flip state on entry;
// flip180 is updated to the orientation that won. Landmarks are returned in
// the WINNING orientation's coordinates (caller flips back if flip180).
static bool detect_both_orientations(const std::vector<uint8_t>& frame,
                                     int fw, int fh, bool& flip180,
                                     FaceObs& obs) {
    std::vector<uint8_t> raw = frame;
    if (flip180) rot180(raw, fw, fh);       // undo intake flip → raw
    std::vector<uint8_t> flp = raw;
    rot180(flp, fw, fh);
    struct Cand {
        const std::vector<uint8_t>* img;
        bool flipped; bool det; float score; float b[4];
    } cands[2] = {{&raw, false, false, 0.f, {0,0,0,0}},
                  {&flp, true,  false, 0.f, {0,0,0,0}}};
    for (Cand& cd : cands)
        cd.det = detect_face(cd.img->data(), fw, fh,
                             cd.b[0], cd.b[1], cd.b[2], cd.b[3], &cd.score);
    if (cands[1].det && (!cands[0].det || cands[1].score > cands[0].score))
        std::swap(cands[0], cands[1]);
    for (Cand& cd : cands) {
        if (!cd.det) continue;
        if (landmarks_from_box(cd.img->data(), fw, fh,
                               cd.b[0], cd.b[1], cd.b[2], cd.b[3], obs) &&
            upright_ok(obs) &&
            lm_sane(obs, cd.b[0], cd.b[1], cd.b[2], cd.b[3])) {
            obs.score = cd.score;
            flip180 = cd.flipped;
            return true;
        }
    }
    return false;
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
        // Tracking mode: while locked, skip the heavy detector and crop
        // around the previous landmarks (the landmark net is a few ms, so
        // the loop runs at camera rate). Re-detect on loss or every ~2 s.
        static int s_since_detect = 0;
        FaceObs obs;
        bool ok = false;
        bool have_prev = false;
        float pb1 = 0, pb2 = 0, pb3 = 0, pb4 = 0;
        {
            std::lock_guard<std::mutex> lk(g_latest_mtx);
            if (g_latest.valid && g_latest.w == fw && g_latest.h == fh) {
                have_prev = true;
                prev_lm_box(g_latest, pb1, pb2, pb3, pb4);
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
            ok = landmarks_from_box(frame.data(), fw, fh, pb1, pb2, pb3, pb4, obs);
            if (ok && (!upright_ok(obs) ||
                       !lm_sane(obs, pb1, pb2, pb3, pb4)))
                ok = false;                       // wrong pose → re-detect
        }
        if (!ok) {
            s_since_detect = 0;
            ok = detect_both_orientations(frame, fw, fh, s_flip180, obs);
        }
        if (ok && s_flip180) {
            // Publish landmarks in the SUBMITTED frame's coordinates.
            for (int k = 0; k < 106; ++k) {
                obs.pts[k][0] = (float)fw - 1.f - obs.pts[k][0];
                obs.pts[k][1] = (float)fh - 1.f - obs.pts[k][1];
            }
        }
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
            float dx = obs.pts[86][0] - g_latest.pts[86][0];
            float dy = obs.pts[86][1] - g_latest.pts[86][1];
            float jump = sqrtf(dx*dx + dy*dy);
            float alpha = jump > obs.w * 0.08f ? 1.f : 0.65f;
            for (int k = 0; k < 106; ++k) {
                g_latest.pts[k][0] += (obs.pts[k][0] - g_latest.pts[k][0]) * alpha;
                g_latest.pts[k][1] += (obs.pts[k][1] - g_latest.pts[k][1]) * alpha;
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
    std::vector<float> records;     // per frame: score + 106*2, raw full-res
    records.reserve((size_t)(total_est > 0 ? total_est : 256) * 213);

    bool flip180 = false;
    int since_detect = 60;          // force detect on the first frame
    FaceObs smooth; smooth.valid = false;
    int n = 0;
    const float hw2f = (float)hw2, hh2f = (float)hh2;

    while (fread(frame.data(), 1, frame_bytes, p) == frame_bytes) {
        if (cancel && cancel->load()) { pclose(p); return false; }
        if (flip180) rot180(frame, fw, fh);
        FaceObs obs;
        bool ok = false;
        if (smooth.valid && ++since_detect < 60) {
            float b1, b2, b3, b4;
            prev_lm_box(smooth, b1, b2, b3, b4);
            if (flip180) {
                float n1 = (float)fw - 1.f - b3, n2 = (float)fh - 1.f - b4;
                float n3 = (float)fw - 1.f - b1, n4 = (float)fh - 1.f - b2;
                b1 = n1; b2 = n2; b3 = n3; b4 = n4;
            }
            ok = landmarks_from_box(frame.data(), fw, fh, b1, b2, b3, b4, obs);
            if (ok && (!upright_ok(obs) || !lm_sane(obs, b1, b2, b3, b4)))
                ok = false;
        }
        if (!ok) {
            since_detect = 0;
            ok = detect_both_orientations(frame, fw, fh, flip180, obs);
        }
        if (ok && flip180) {
            for (int k = 0; k < 106; ++k) {
                obs.pts[k][0] = (float)fw - 1.f - obs.pts[k][0];
                obs.pts[k][1] = (float)fh - 1.f - obs.pts[k][1];
            }
        }
        if (!ok) {
            smooth.score *= 0.7f;
            if (smooth.score < 0.15f) smooth.valid = false;
        } else if (!smooth.valid) {
            smooth = obs;
        } else {
            float dx = obs.pts[86][0] - smooth.pts[86][0];
            float dy = obs.pts[86][1] - smooth.pts[86][1];
            float jump = sqrtf(dx*dx + dy*dy);
            float alpha = jump > (float)fw * 0.08f ? 1.f : 0.65f;
            for (int k = 0; k < 106; ++k) {
                smooth.pts[k][0] += (obs.pts[k][0] - smooth.pts[k][0]) * alpha;
                smooth.pts[k][1] += (obs.pts[k][1] - smooth.pts[k][1]) * alpha;
            }
            smooth.score = obs.score;
        }
        // Record in RAW full-res coords (same remap as the live mirror).
        records.push_back(smooth.valid ? smooth.score : 0.f);
        if (!smooth.valid) {
            records.insert(records.end(), 212, 0.f);
            ++n;
            continue;
        }
        for (int k = 0; k < 106; ++k) {
            float ux = smooth.pts[k][0], uy = smooth.pts[k][1];
            float rx2, ry2;
            if (rot_q == 1)      { rx2 = uy;              ry2 = hh2f - 1.f - ux; }
            else if (rot_q == 3) { rx2 = hw2f - 1.f - uy; ry2 = ux; }
            else if (rot_q == 2) { rx2 = hw2f - 1.f - ux; ry2 = hh2f - 1.f - uy; }
            else                 { rx2 = ux;              ry2 = uy; }
            records.push_back(rx2 * 2.f);
            records.push_back(ry2 * 2.f);
        }
        ++n;
        if (progress && total_est > 0 && (n & 7) == 0)
            progress((float)n / (float)total_est);
    }
    pclose(p);
    if (n == 0) return false;

    std::string tmp = out_path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    uint32_t magic = 0x46534D50, version = 1;    // 'PMSF'
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
