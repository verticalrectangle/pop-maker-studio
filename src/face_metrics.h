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
