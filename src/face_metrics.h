#pragma once
// Geometric face metrics over the 478-point MediaPipe topology, shared by the face
// pipeline, face FX and the script runtime so every consumer agrees on the numbers.
#include <cmath>

// Eyelid gap over eye width, averaged over both eyes (points 159/145 over 33/133 and
// 386/374 over 362/263). ~0.34-0.38 open, ~0.11 closed on the reference subject; unlike the
// eyeBlink blendshapes it stays well separated behind glasses. `xy` = 478 interleaved
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

// Unified blink signal: max(blendshape blink, geometric 1 − eyeOpen/baseline).
// The blendshape head misreads through glasses (contract above); the geometric
// term stays well separated there. `blend_blink` = mean of the two eyeBlink
// coefficients (0 when the observation carries none); `eye_open` = the ratio
// above for this frame; `open_baseline` = the running high percentile of
// eyeOpen for that track (i.e. the wide-open value). Returns 0..1.
inline float face_blink_signal(float blend_blink, float eye_open, float open_baseline) {
    float geom = 0.f;
    if (open_baseline > 1e-6f) {
        geom = 1.f - eye_open / open_baseline;
        geom = geom < 0.f ? 0.f : (geom > 1.f ? 1.f : geom);
    }
    float b = blend_blink < 0.f ? 0.f : (blend_blink > 1.f ? 1.f : blend_blink);
    return b > geom ? b : geom;
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
