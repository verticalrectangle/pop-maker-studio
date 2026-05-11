#pragma once
#include "app.h"
#include <cstdint>

static const int MAX_BG_SLOTS = 8;

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

// ── Scene compositor ──────────────────────────────────────────────────────────
// Accumulates video clip textures into an offscreen FBO via alpha-correct "over"
// compositing, then exposes the result for one-shot global FX and ImGui draw.
//
// Usage per frame:
//   scene_begin(w, h)                       — clear scene to transparent black
//   scene_add_layer(tex, cx, cy, hw, hh, …) — composite each clip
//   scene_add_solid(r, g, b, a)             — composite a solid colour layer
//   scene_apply_fx(w, h, ea, cfx, t)        — apply global FX to composited scene
//   uintptr_t tex = scene_result()          — get GL texture (Y-flipped vs ImGui)
//
// Draw tex to ImGui with Y-flipped UVs: tl=(0,1) tr=(1,1) br=(1,0) bl=(0,0).
// Uses standard (straight) alpha — no AddCallback/blend-mode change needed.
static const int kSceneFxSlot = MAX_VIDEO_TRACKS * 2 - 2;  // reserved for global FX

void      scene_begin    (int canvas_w, int canvas_h);
void      scene_add_layer(uintptr_t clip_tex, float cx, float cy, float hw, float hh,
                          float cos_r, float sin_r, float alpha);
void      scene_add_solid(float r, float g, float b, float a);
void      scene_apply_fx (int canvas_w, int canvas_h,
                          const EffectAccum& ea, const CreativeFXAccum& cfx, float t);
uintptr_t scene_result   ();

// Blit src_tex (straight copy) into an existing GL FBO.  Saves/restores GL state.
void fx_blit(uintptr_t src_tex, unsigned dst_fbo, int w, int h);

// Renders a BG preset into a stable per-slot GL texture. Returns 0 on failure.
// slot: 0..MAX_BG_SLOTS-1
uintptr_t bg_render_to_texture(const char* preset_id, int slot,
                                int canvas_w, int canvas_h,
                                float t, float speed, float intensity,
                                const float c1[4], const float c2[4], const float c3[4]);
