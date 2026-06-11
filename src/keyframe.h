#pragma once
#include <vector>

// ── Keyframing ────────────────────────────────────────────────────────────────
// Shared by the clip model (app.h) and the audio engine (audio.h), which must
// evaluate volume/pan tracks per-sample without pulling in all of app.h.

enum class InterpType { Linear, EaseIn, EaseOut, EaseBoth, Hold };

struct Keyframe {
    float      time   = 0.f;   // seconds relative to clip.start
    float      value  = 0.f;
    InterpType interp = InterpType::EaseBoth;
};

struct PropTrack {
    std::vector<Keyframe> keys;  // always sorted by time

    bool  empty()                                                     const { return keys.empty(); }
    float eval(float t)                                               const;
    void  set(float t, float v, InterpType it = InterpType::EaseBoth);
    void  remove_at(float t, float tol = 0.05f);
    int   find_nearest(float t, float tol = 0.1f)                    const;
};
