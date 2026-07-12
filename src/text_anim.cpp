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
        default: break;   // WaveText/Jitter/Explode/Gravity are per-element only
    }
    return a;
}


ElemAnim compute_elem_anim(AnimStyle style, float local_t, float clip_dur,
                           float fade_in, float fade_out, float w, int ease,
                           int i, int n, float stagger, float line_h) {
    ElemAnim a;
    float et       = local_t - (float)i * stagger;   // element-local time
    float tail     = clip_dur - fade_out;
    float exit_mul = (local_t > tail && fade_out > 0.f)
                     ? fmaxf(0.f, (clip_dur - local_t) / fade_out) : 1.f;

    switch (style) {
        case AnimStyle::WaveText: {
            // Continuous ripple: each element bobs on a sine phased by its index.
            float intro = fade_in > 0.f ? ease_eval(EASE_OUT_CUBIC, local_t / fade_in) : 1.f;
            a.dy    = sinf(local_t * 4.f + (float)i * 0.6f) * (line_h * 0.16f);
            a.alpha = intro * exit_mul;
            break;
        }
        case AnimStyle::Jitter: {
            float intro = fade_in > 0.f ? ease_eval(EASE_OUT_CUBIC, local_t / fade_in) : 1.f;
            float ph    = local_t * 38.f;
            a.dx    = (sinf(ph + (float)i * 12.9898f) ) * (line_h * 0.05f);
            a.dy    = (cosf(ph * 1.13f + (float)i * 4.1414f)) * (line_h * 0.05f);
            a.alpha = intro * exit_mul;
            break;
        }
        case AnimStyle::Explode: {
            // Fly in from a random direction/distance to the resting spot.
            if (et < 0.f) { a.alpha = 0.f; break; }
            float p   = fade_in > 0.f ? ease_eval(EASE_OUT_CUBIC, et / fade_in) : 1.f;
            float ang = hash01(i, 1) * 6.2831853f;
            float dist= (0.6f + hash01(i, 2)) * w * 0.4f;
            a.dx    = cosf(ang) * dist * (1.f - p);
            a.dy    = sinf(ang) * dist * (1.f - p);
            a.scale = 0.4f + 0.6f * p;
            a.alpha = ease_eval(EASE_LINEAR, et / fmaxf(0.0001f, fade_in)) * exit_mul;
            if (a.alpha > 1.f) a.alpha = 1.f;
            break;
        }
        case AnimStyle::Gravity: {
            // Drop from above and bounce to the baseline, staggered per element.
            if (et < 0.f) { a.alpha = 0.f; break; }
            float dur = fmaxf(0.25f, fminf(0.7f, clip_dur));
            float p   = ease_eval(EASE_OUT_BOUNCE, et / dur);
            a.dy    = (p - 1.f) * line_h * 3.f;   // starts 3 lines up, bounces to 0
            a.alpha = exit_mul;
            break;
        }
        case AnimStyle::ScratchFilm: {
            // Letters stay legible; per-frame scratch marks are drawn in
            // render_text_block. Here we handle alpha (fade in/out) and a
            // subtle per-frame position jitter for the "boiling" hand-scratched feel.
            float intro = fade_in > 0.f ? ease_eval(EASE_OUT_CUBIC, local_t / fade_in) : 1.f;
            int frame_i = (int)(local_t * 24.f);   // 24 fps scratch cadence
            a.alpha = intro * exit_mul;
            a.dx = (hash01(i, frame_i) - 0.5f) * line_h * 0.02f;
            a.dy = (hash01(i, frame_i + 7) - 0.5f) * line_h * 0.02f;
            break;
        }
        default: {
            // Intro-ramp styles applied per element: hidden until the element's
            // staggered turn, then it runs the normal block motion; exit fades
            // uniformly so the line leaves together.
            if (et < 0.f) { a.alpha = 0.f; break; }
            BlockAnim b = compute_block_anim(style, et, clip_dur + (float)i * stagger,
                                             fade_in, fade_out, w, ease);
            a.dx = b.dx; a.dy = b.dy; a.scale = b.scale;
            a.alpha = b.alpha * exit_mul;
            break;
        }
    }
    return a;
}
