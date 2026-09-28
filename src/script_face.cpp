#include "script_face.h"
#include "face_cache.h"
#include "face_metrics.h"
#include "face_track.h"
#include "generated/face_mesh_edges.h"
#include "json.hpp"

#include <algorithm>

using json = nlohmann::json;

// [[a, b], …] landmark index pairs.
template <size_t N>
static json edges_json(const int (&edges)[N][2]) {
    json arr = json::array();
    for (size_t i = 0; i < N; ++i) arr.push_back({edges[i][0], edges[i][1]});
    return arr;
}

bool face_track_dump_json(const std::string& path, int rot_q,
                          std::string& out_json, std::string* err,
                          const FaceTrackDumpOptions& opt) {
    if (!face_track_available()) {
        if (err) *err = "face models missing (models/face/*.onnx)";
        return false;
    }
    rot_q = ((rot_q % 4) + 4) % 4;
    float fps = 0.f;
    int rw = 0, rh = 0;
    int count = face_cache_frame_count(path, rot_q, &fps, &rw, &rh);
    if (count <= 0 || rw <= 0 || rh <= 0) {
        if (err) {
            FaceCacheStatus st = face_cache_status(path, nullptr);
            *err = st == FaceCacheStatus::Failed ? "face cache failed" : "face cache building";
        }
        return false;
    }
    const bool swap = rot_q == 1 || rot_q == 3;
    const int n = std::min(count, std::max(1, opt.max_frames));
    json times = json::array(), lms = json::array(), eyes = json::array(), blinks = json::array();
    // Same streaming open-eye baseline + unified blink signal as get_face_blink.
    FaceOpenBaseline base;
    for (int i = 0; i < n; ++i) {
        FaceObs o;
        double st = 0.0;
        bool face = face_cache_frame(path, rot_q, i, o, &st);
        times.push_back(st);
        json frame = json::array();
        for (int k = 0; k < FT_NPTS; ++k) {
            if (!face) { frame.push_back({0.0, 0.0}); continue; }
            // Raw take pixels → normalised display orientation (rot_q quarter
            // turns clockwise, as the tracker rotated the frames upright).
            const float u = o.pts[k][0] / (float)rw, v = o.pts[k][1] / (float)rh;
            float x = u, y = v;
            if (rot_q == 1) { x = 1.f - v; y = u; }
            else if (rot_q == 2) { x = 1.f - u; y = 1.f - v; }
            else if (rot_q == 3) { x = v; y = 1.f - u; }
            frame.push_back({x, y});
        }
        lms.push_back(std::move(frame));
        if (face) base.update(o.eye_open);
        eyes.push_back(face ? o.eye_open : 0.f);
        blinks.push_back(face ? face_blink_of(o, base.baseline()) : 0.f);
    }
    json j;
    j["width"] = swap ? rh : rw;
    j["height"] = swap ? rw : rh;
    j["fps"] = fps;
    j["times"] = std::move(times);
    j["landmarks"] = std::move(lms);
    j["eyeOpen"] = std::move(eyes);
    j["blink"] = std::move(blinks);
    j["mesh"] = {{"tesselation", edges_json(kFaceTesselation)},
                 {"contours", edges_json(kFaceContours)},
                 {"irises", edges_json(kFaceIrises)}};
    out_json = j.dump();
    return true;
}
