// face-smoke — functional gate for the face-tracking stack (Stage 1 of
// pms-ios docs/MAKEUP_PLAN.md). Loads a face photo, runs the REAL engine
// pipeline synchronously (YuNet detect → 478-pt landmarks → blendshapes via
// face_track_run_sync), and asserts a confident, sane observation. Fails when
// models are missing/mismatched — the same failure the record-mode makeup
// filters would hit at runtime.
//
//   ./face-smoke [image] [asset_root]
//     image      default assets/test_face.png
//     asset_root default "." — models resolve at <asset_root>/models/face/
//                (pms_set_asset_root, same rule the iOS bundle uses).
#include "../src/face_track.h"
#include "../src/paths.h"
#include "stb_image.h"
#include <cstdio>
#include <cmath>

int main(int argc, char** argv) {
    const char* img_path = argc > 1 ? argv[1] : "assets/test_face.png";
    pms_set_asset_root(argc > 2 ? argv[2] : ".");

    if (!face_track_available()) {
        fprintf(stderr, "face smoke: FAIL — models missing (models/face/"
                        "{yunet,face_landmarks_v2,face_blendshapes}.onnx)\n");
        return 1;
    }
    int w = 0, h = 0, n = 0;
    unsigned char* rgb = stbi_load(img_path, &w, &h, &n, 3);
    if (!rgb) { fprintf(stderr, "face smoke: FAIL — cannot load %s\n", img_path); return 1; }

    FaceObs obs;
    if (!face_track_run_sync(rgb, w, h, obs) || !obs.valid) {
        fprintf(stderr, "face smoke: FAIL — no face found in %s (%dx%d)\n", img_path, w, h);
        return 1;
    }
    // Sanity: confident, landmarks inside the frame, plausible spread.
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (int k = 0; k < FT_NPTS; ++k) {
        minx = std::fmin(minx, obs.pts[k][0]); maxx = std::fmax(maxx, obs.pts[k][0]);
        miny = std::fmin(miny, obs.pts[k][1]); maxy = std::fmax(maxy, obs.pts[k][1]);
    }
    bool inside = minx > -w * 0.2f && maxx < w * 1.2f && miny > -h * 0.2f && maxy < h * 1.2f;
    bool spread = (maxx - minx) > w * 0.1f && (maxy - miny) > h * 0.1f;
    printf("face smoke: score=%.3f mesh=[%.0f,%.0f..%.0f,%.0f] blend=%s "
           "jawOpen=%.3f blinkL=%.3f\n",
           obs.score, minx, miny, maxx, maxy, obs.has_blend ? "yes" : "no",
           obs.blend[FB_JAW_OPEN], obs.blend[FB_EYE_BLINK_L]);
    if (obs.score < 0.6f || !inside || !spread) {
        fprintf(stderr, "face smoke: FAIL — implausible observation\n");
        return 1;
    }
    if (!obs.has_blend) {
        fprintf(stderr, "face smoke: FAIL — blendshapes missing\n");
        return 1;
    }
    printf("face smoke: PASS\n");
    stbi_image_free(rgb);
    return 0;
}
