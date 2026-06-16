#pragma once
#include "app.h"

// ── Shared typography animation + easing ──────────────────────────────────────
// One source of truth for the kinetic-text motion so the live canvas preview and
// the GL export (overlay_renderer) stay identical. Wave 1 computes a single
// whole-block transform; Wave 2 extends the same vocabulary per word/letter.

// Easing curves. 0 = linear; the rest are the standard Penner-style set. Used by
// presets via the `ease` id so motion feels designed instead of linear.
enum {
    EASE_LINEAR = 0,
    EASE_IN_CUBIC, EASE_OUT_CUBIC, EASE_INOUT_CUBIC,
    EASE_OUT_BACK, EASE_OUT_ELASTIC, EASE_OUT_BOUNCE,
    EASE_OUT_EXPO, EASE_OUT_QUINT, EASE_INOUT_QUINT,
    EASE_IN_BACK,   // anticipate
};

// Evaluate easing curve `type` at t (clamped to [0,1]).
float ease_eval(int type, float t);

struct BlockAnim {
    float alpha = 1.f;
    float dx    = 0.f;
    float dy    = 0.f;
    float scale = 1.f;
};

// Compute the whole-block transform for `style` at `local_t` (seconds since the
// clip started). `ease` overrides the per-style default ramp easing (0 = use the
// style's natural default so existing presets are unchanged). `w` is canvas
// width in px (slide distance).
BlockAnim compute_block_anim(AnimStyle style, float local_t, float clip_dur,
                             float fade_in, float fade_out, float w, int ease);

// Per-element (per-word / per-letter) transform for kinetic typography. Element
// `i` of `n` enters on a stagger (`i * stagger` seconds late) so words/letters
// cascade in. The "intro ramp" styles (Fade/Scale/Slide/Stack/Typewriter/Bounce)
// reuse compute_block_anim per element, gated so an element is invisible until
// its turn; WaveText/Jitter are continuous; Explode/Gravity are staggered intros
// with motion that needs the element index. dx/dy are pixel offsets, scale is a
// multiplier about the element's centre, alpha multiplies the element's opacity.
struct ElemAnim {
    float alpha = 1.f;
    float dx    = 0.f;
    float dy    = 0.f;
    float scale = 1.f;
};

ElemAnim compute_elem_anim(AnimStyle style, float local_t, float clip_dur,
                           float fade_in, float fade_out, float w, int ease,
                           int i, int n, float stagger, float line_h);
