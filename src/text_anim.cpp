#include "text_anim.h"
#include <cmath>

static inline float clamp01(float t) { return t < 0.f ? 0.f : (t > 1.f ? 1.f : t); }

float ease_eval(int type, float t) {
    t = clamp01(t);
    const float c1 = 1.70158f, c3 = c1 + 1.f;
    switch (type) {
        case EASE_IN_CUBIC:    return t * t * t;
        case EASE_OUT_CUBIC:   return 1.f - powf(1.f - t, 3.f);
        case EASE_INOUT_CUBIC: return t < 0.5f ? 4.f*t*t*t : 1.f - powf(-2.f*t+2.f, 3.f)/2.f;
        case EASE_OUT_BACK:    return 1.f + c3*powf(t-1.f, 3.f) + c1*powf(t-1.f, 2.f);
        case EASE_OUT_ELASTIC: {
            if (t == 0.f || t == 1.f) return t;
            const float p = (2.f * 3.14159265f) / 3.f;
            return powf(2.f, -10.f*t) * sinf((t*10.f - 0.75f) * p) + 1.f;
        }
        case EASE_OUT_BOUNCE: {
            const float n1 = 7.5625f, d1 = 2.75f;
            if (t < 1.f/d1)        return n1*t*t;
            else if (t < 2.f/d1)   { t -= 1.5f/d1;   return n1*t*t + 0.75f; }
            else if (t < 2.5f/d1)  { t -= 2.25f/d1;  return n1*t*t + 0.9375f; }
            else                   { t -= 2.625f/d1; return n1*t*t + 0.984375f; }
        }
        case EASE_OUT_EXPO:    return t == 1.f ? 1.f : 1.f - powf(2.f, -10.f*t);
        case EASE_OUT_QUINT:   return 1.f - powf(1.f - t, 5.f);
        case EASE_INOUT_QUINT: return t < 0.5f ? 16.f*t*t*t*t*t : 1.f - powf(-2.f*t+2.f, 5.f)/2.f;
        case EASE_IN_BACK:     return c3*t*t*t - c1*t*t;
        default:               return t;   // linear
    }
}

BlockAnim compute_block_anim(AnimStyle style, float local_t, float clip_dur,
                             float fade_in, float fade_out, float w, int ease) {
    BlockAnim a;
    // ease==0 means "use the style's natural default" so existing presets that
    // never set an ease keep their original feel; a non-zero ease overrides.
    auto E = [&](int dflt) { return ease != 0 ? ease : dflt; };

    switch (style) {
        case AnimStyle::Fade:
            if (local_t < fade_in)
                a.alpha = ease_eval(E(EASE_LINEAR), local_t / fade_in);
            else if (local_t > clip_dur - fade_out)
                a.alpha = ease_eval(E(EASE_LINEAR), (clip_dur - local_t) / fade_out);
            break;
        case AnimStyle::Glitch: {
            float decay = fmaxf(0.f, 1.f - local_t / 0.5f);
            a.dx = sinf(local_t * 97.f + sinf(local_t * 53.f) * 31.f) * 12.f * decay;
            break;
        }
        case AnimStyle::Typewriter:
            if (local_t < fade_in) {
                float p = local_t / fade_in;
                a.alpha = ease_eval(E(EASE_LINEAR), p);
                a.dy    = (1.f - ease_eval(E(EASE_OUT_CUBIC), p)) * (-8.f);
            }
            break;
        case AnimStyle::Bounce: {
            float bd = fminf(0.6f, clip_dur);
            if (local_t < bd) {
                float p2 = local_t / bd;
                a.dy = sinf(p2 * 3.14159f) * (-60.f) * expf(-p2 * 4.f);
            }
            break;
        }
        case AnimStyle::Slide:
            if (local_t < fade_in)
                a.dx = (ease_eval(E(EASE_OUT_CUBIC), local_t / fade_in) - 1.f) * w * 0.6f;
            else if (local_t > clip_dur - fade_out)
                a.dx = ease_eval(E(EASE_IN_CUBIC),
                                 (local_t - (clip_dur - fade_out)) / fade_out) * w * 0.6f;
            break;
        case AnimStyle::Stack:
            if (local_t < fade_in)
                a.dy = (1.f - ease_eval(E(EASE_OUT_CUBIC), local_t / fade_in)) * 80.f;
            break;
        case AnimStyle::Scale:
            if (local_t < fade_in) {
                float p = local_t / fade_in;
                a.scale = 0.55f + 0.45f * ease_eval(E(EASE_OUT_CUBIC), p);
                a.alpha = ease_eval(E(EASE_LINEAR), p);
            } else if (local_t > clip_dur - fade_out) {
                a.alpha = (clip_dur - local_t) / fade_out;
            }
            break;
        default: break;
    }
    return a;
}
