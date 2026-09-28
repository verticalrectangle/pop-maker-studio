#pragma once
// Geometric face metrics over the 478-point MediaPipe topology, shared by the face
// pipeline, face FX and the script runtime so every consumer agrees on the numbers.
#include <cmath>

// Eyelid gap over eye width, averaged over both eyes (points 159/145 over 33/133 and
// 386/374 over 362/263). Absolute values depend on the landmark model (reference subject
// with glasses: MediaPipe Python 0.34-0.38 open / 0.11 closed; PMS's mesh 0.21 / 0.13-0.15),
// so blink logic normalises by the track's own open baseline. `xy` = 478 interleaved
// (x, y) pairs in any consistent unit; pass the frame size when the units are normalised
// so the ratio is computed in square pixels.
inline float face_eye_open_ratio(const float* xy, float w = 1.f, float h = 1.f) {
    auto dist = [&](int a, int b) {
        const float dx = (xy[2 * a] - xy[2 * b]) * w;
        const float dy = (xy[2 * a + 1] - xy[2 * b + 1]) * h;
        return std::sqrt(dx * dx + dy * dy);
    };
    const float left = dist(159, 145) / std::fmax(1e-6f, dist(33, 133));
    const float right = dist(386, 374) / std::fmax(1e-6f, dist(362, 263));
    return 0.5f * (left + right);
}

inline float face_smooth01(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    return t * t * (3.f - 2.f * t);
}

// Geometric blink from lid closure relative to the track's open-eye baseline. The mesh's
// lid points ride the lashes, so even a full blink only closes the ratio by ~30-40%
// (reference Live Photo, glasses: open 0.21, blinks 0.15/0.15/0.13) while ordinary
// squints/adjustments stay under ~18%: closure 0.12 -> 0.32 maps onto 0 -> 1.
inline float face_blink_geometric(float eye_open, float open_baseline) {
    if (!(open_baseline > 1e-6f) || !(eye_open > 0.f)) return 0.f;
    return face_smooth01(0.12f, 0.32f, 1.f - eye_open / open_baseline);
}

// eyeBlink blendshape remapped past its noise floor: through glasses it idles at
// 0.20-0.33 with the eyes open and reaches only 0.35-0.52 on real blinks.
inline float face_blink_blendshape(float blend_blink) { return face_smooth01(0.30f, 0.50f, blend_blink); }

// Unified blink signal (0..1): the stronger of the geometric and blendshape evidence.
// `blend_blink` = mean of the two eyeBlink coefficients (0 when absent); `eye_open` =
// face_eye_open_ratio for this frame (unsmoothed); `open_baseline` = FaceOpenBaseline.
inline float face_blink_signal(float blend_blink, float eye_open, float open_baseline) {
    const float g = face_blink_geometric(eye_open, open_baseline);
    const float b = face_blink_blendshape(blend_blink);
    return g > b ? g : b;
}

// Per-track running high percentile of eyeOpen (the open-eye baseline the
// geometric blink term normalises against). Update with each frame's eyeOpen;
// read via baseline(). Rises in a few frames (a fresh track calibrates from
// its first open frames), falls slowly so a long blink never drags the
// baseline down to the closed value.
struct FaceOpenBaseline {
    float v = 0.f;      // current baseline; <= 0 = uncalibrated
    void update(float eye_open) {
        if (!(eye_open > 0.f)) return;
        if (v <= 0.f) { v = eye_open; return; }
        const float up = eye_open - v;
        v += up * (up > 0.f ? 0.25f : 0.02f);
    }
    float baseline() const { return v; }
};
