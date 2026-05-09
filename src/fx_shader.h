#pragma once
#include "app.h"
#include <cstdint>

void fx_shader_init();
void fx_shader_shutdown();

// Apply the GPU FX chain (grade, blur, vignette, chroma-key, glitch, VHS,
// light-leak, datamosh) to src_tex and write the result into a stable per-slot
// output texture.  Returns src_tex unchanged when no FX is active.
//
// slot:  0 .. MAX_VIDEO_TRACKS*2-1  — identifies which stable output buffer to use.
//        Each slot has its own output texture that persists until the next
//        fx_apply() call for that slot, making it safe to use in a deferred
//        ImDrawList that is submitted later in the same frame.
// w, h:  pixel dimensions of src_tex.
// t:     animation time (seconds, e.g. clip-local or absolute playhead).
uintptr_t fx_apply(uintptr_t src_tex, int slot, int w, int h,
                   const EffectAccum& ea, const CreativeFXAccum& cfx, float t);

// Render a generated effect (FXType >= ChromaKey) on src_tex using default params.
// Uses an internal preview slot — safe to call outside the normal video pipeline.
// Returns a stable GL texture ID valid until the next call.
uintptr_t fx_preview_gen_effect(FXType ft, uintptr_t src_tex, int w, int h, float t);
