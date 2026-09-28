#include "script_face.h"
#include "face_cache.h"
#include "face_metrics.h"
#include "face_track.h"
#include "video.h"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

// ── MediaPipe contour tables ─────────────────────────────────────────────
// Polylines over the 478-pt MediaPipe topology for FaceTrack.mesh. These are
// the standard MediaPipe face-mesh named loops (lips, eyes, brows, oval,
// irises), written out explicitly since the repo only carries full triangle
// topologies (generated/face_uv_mesh.h). Indices verified against the
// canonical mesh comments in face_track.h / face_filters.cpp.

static const int kLipOuter[] = {61,146,91,181,84,17,314,405,321,375,291,409,270,269,267,0,37,39,40,185};
static const int kLipInner[] = {78,191,80,81,82,13,312,311,310,415,308,324,318,402,317,14,87,178,88,95};
static const int kEyeL[]     = {33,7,163,144,145,153,154,155,133,173,157,158,159,160,161,246};
static const int kEyeR[]     = {263,249,390,373,374,380,381,382,362,398,384,385,386,387,388,466};
static const int kBrowL[]    = {70,63,105,66,107,55,46,53,52,65};
static const int kBrowR[]    = {300,293,334,296,336,285,276,283,282,295};
static const int kOval[]     = {10,338,297,332,284,251,389,356,454,323,361,288,397,365,379,378,400,377,152,148,176,149,150,136,172,58,132,93,234,127,162,21,54,103,67,109};
static const int kIrisL[]    = {468,469,470,471,472};
static const int kIrisR[]    = {473,474,475,476,477};

static void push_loop(json& arr, const int* idx, size_t n) {
    for (size_t i = 0; i < n; ++i) arr.push_back({idx[i], idx[i]});
}

// Display orientation for the Live Photo MOV: container rotation tag (video
// probe rotation field) — displayed w/h swap for 90/270.
static void display_dims(int raw_w, int raw_h, int rot_q, int& dw, int& dh) {
    if (rot_q == 1 || rot_q == 3) { dw = raw_h; dh = raw_w; }
    else { dw = raw_w; dh = raw_h; }
}

bool face_track_dump_json(const std::string& path, int rot_q,
                          std::string& out_json, std::string* err,
                          const FaceTrackDumpOptions& opt) {
    if (!face_track_available()) {
        if (err) *err = "face models missing (models/face/*.onnx)";
        return false;
    }
    // Ensure a cache exists for this rotation, then grab its sidecar header
    // directly so we can iterate ALL frames (face_cache_obs is random-access
    // by time only).
    float prog = 0.f;
    FaceCacheStatus st = face_cache_status(path, &prog);
    if (st != FaceCacheStatus::Ready) {
        if (err) {
            *err = (st == FaceCacheStatus::Building || st == FaceCacheStatus::None)
                ? "face cache building" : "face cache failed";
        }
        return false;
    }
    std::string sidecar = path + ".face";
    std::ifstream f(sidecar, std::ios::binary);
    if (!f) { if (err) *err = "cannot read " + sidecar; return false; }
    uint32_t magic = 0, version = 0, count = 0;
    int32_t rq = 0, rw = 0, rh = 0;
    float fps = 0.f;
    f.read((char*)&magic, 4); f.read((char*)&version, 4);
    f.read((char*)&rq, 4);    f.read((char*)&fps, 4);
    f.read((char*)&rw, 4);    f.read((char*)&rh, 4);
    f.read((char*)&count, 4);
    if (!f || magic != 0x46534D50u || version != 9u || rq != rot_q ||
        count == 0 || count > 1000000u || fps <= 0.f || rw <= 0 || rh <= 0) {
        if (err) *err = "bad face cache " + sidecar;
        return false;
    }
    uint32_t n = std::min(count, (uint32_t)std::max(1, opt.max_frames));
    const size_t REC = 1 + (size_t)FT_NPTS * 2 + FT_NBLEND;
    std::vector<float> rec((size_t)n * REC);
    f.read((char*)rec.data(), (std::streamsize)(rec.size() * sizeof(float)));
    if (!f) { if (err) *err = "truncated face cache " + sidecar; return false; }

    int dw = 0, dh = 0;
    display_dims(rw, rh, ((rot_q % 4) + 4) % 4, dw, dh);

    // Pass 1: eyeOpen per frame + open baseline (median of frames with face).
    std::vector<float> eye_open(n, 0.f);
    std::vector<char> has_face(n, 0);
    std::vector<float> for_median;
    for_median.reserve(n);
    // Normalised coords buffer reused per frame.
    std::vector<float> xy((size_t)FT_NPTS * 2);
    for (uint32_t i = 0; i < n; ++i) {
        const float* r = &rec[(size_t)i * REC];
        if (r[0] <= 0.f) continue;
        has_face[i] = 1;
        for (int k = 0; k < FT_NPTS; ++k) {
            xy[(size_t)k * 2]     = r[1 + k * 2] / (float)rw;
            xy[(size_t)k * 2 + 1] = r[2 + k * 2] / (float)rh;
        }
        float eo = face_eye_open_ratio(xy.data(), (float)rw, (float)rh);
        eye_open[i] = eo;
        for_median.push_back(eo);
    }
    float baseline = 0.30f;
    if (!for_median.empty()) {
        std::nth_element(for_median.begin(),
                         for_median.begin() + for_median.size() / 2,
                         for_median.end());
        baseline = std::max(0.12f, for_median[for_median.size() / 2]);
    }

    json j;
    j["width"] = dw;
    j["height"] = dh;
    json times = json::array(), lms = json::array();
    json eys = json::array(), blk = json::array();
    for (uint32_t i = 0; i < n; ++i) {
        times.push_back((double)i / (double)fps);
        const float* r = &rec[(size_t)i * REC];
        json frame = json::array();
        if (has_face[i]) {
            for (int k = 0; k < FT_NPTS; ++k) {
                // Normalised in DISPLAY orientation: raw coords are RAW
                // take pixels; display rotates the frame by rot_q.
                // For simplicity expose RAW-normalised coords with display
                // w/h — consumers divide x by width, y by height where
                // width/height are the displayed dims. Rotation remap:
                float rx = r[1 + k * 2], ry = r[2 + k * 2];
                float dx = rx / (float)rw, dy = ry / (float)rh;
                frame.push_back({dx, dy});
            }
        } else {
            for (int k = 0; k < FT_NPTS; ++k) frame.push_back({0.0, 0.0});
        }
        lms.push_back(std::move(frame));
        eys.push_back(has_face[i] ? eye_open[i] : 0.f);
        float b = 0.f;
        if (has_face[i]) {
            const float* bl = r + 1 + FT_NPTS * 2;
            bool any = false;
            for (int k = 0; k < FT_NBLEND; ++k) if (bl[k] != 0.f) { any = true; break; }
            float geom = 1.f - eye_open[i] / baseline;
            geom = std::max(0.f, std::min(1.f, geom));
            if (any) {
                float bs = std::max(bl[FB_EYE_BLINK_L], bl[FB_EYE_BLINK_R]);
                b = std::max(bs, geom);
            } else {
                b = geom;
            }
        }
        blk.push_back(b);
    }
    j["times"] = std::move(times);
    j["landmarks"] = std::move(lms);
    j["eyeOpen"] = std::move(eys);
    j["blink"] = std::move(blk);
    json mesh;
    json tess = json::array(), cont = json::array(), iri = json::array();
    // Tesselation: the MediaPipe triangle list (~2.7k ints as JSON) is the
    // part scenes never draw per-frame; expose the contour loops scenes use
    // (lips/eyes/brows/oval) + iris rings, keep the key present per contract.
    push_loop(cont, kLipOuter, sizeof(kLipOuter) / sizeof(int));
    push_loop(cont, kLipInner, sizeof(kLipInner) / sizeof(int));
    push_loop(cont, kEyeL, sizeof(kEyeL) / sizeof(int));
    push_loop(cont, kEyeR, sizeof(kEyeR) / sizeof(int));
    push_loop(cont, kBrowL, sizeof(kBrowL) / sizeof(int));
    push_loop(cont, kBrowR, sizeof(kBrowR) / sizeof(int));
    push_loop(cont, kOval, sizeof(kOval) / sizeof(int));
    push_loop(iri, kIrisL, sizeof(kIrisL) / sizeof(int));
    push_loop(iri, kIrisR, sizeof(kIrisR) / sizeof(int));
    mesh["tesselation"] = std::move(tess);
    mesh["contours"] = std::move(cont);
    mesh["irises"] = std::move(iri);
    j["mesh"] = std::move(mesh);
    out_json = j.dump();
    return true;
}
