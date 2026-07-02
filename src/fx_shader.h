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
// Face-filter warp: bumps = n × {cx, cy, radius, scale, dx, dy} in frame UV.
// Renders into the slot's FBO and returns its texture (src on no-op).
// Beauty pass (industry-style): landmark-masked skin smoothing (bilateral-
// lite), brighten/warmth, and eye pop. Runs BEFORE the warp pass into its own
// per-slot buffer (never the warp's g_out — same-slot read/write feedback).
// All geometry is in PIXELS of the w×h texture.
struct FaceBeautyParams {
    float smooth   = 0.f;   // 0..1 skin smoothing mix
    float brighten = 0.f;   // 0..1 soft-light skin lift
    float warmth   = 0.f;   // 0..1 warm tint on skin
    float eye_pop  = 0.f;   // 0..1 eye brightening
    float blush    = 0.f;   // 0..1 cheek makeup
    float lip_tint = 0.f;   // 0..1 lip color
    float lip_grad = 1.f;   // bitten-lip gradient amount (0 = full matte)
    float nose_x = 0, nose_y = 0;   // nose bridge (px)
    float nose_blush = 0.f; // 0..1 e-girl across-the-nose blush
    float freckles  = 0.f;  // 0..1 faux freckles over nose + upper cheeks
    float blush_col[3] = {1.f, 0.45f, 0.55f};   // makeup colors
    float lip_col[3]   = {0.95f, 0.25f, 0.35f};
    float cheekL_x = 0, cheekL_y = 0, cheekR_x = 0, cheekR_y = 0;
    // Cyber layer: skin recolor / chrome / holo — all masked to skin.
    float eye_glow = 0.f;   // 0..1 additive colored glow at the eyes
    float eye_glow_col[3] = {0.2f, 0.9f, 1.f};
    float skin_tint = 0.f;  // 0..1 colorize skin toward tint_col (luma kept)
    float tint_col[3] = {0.7f, 0.8f, 1.f};
    float desat     = 0.f;  // 0..1 desaturate skin
    float chrome    = 0.f;  // 0..1 metallic contrast curve on skin
    float scanlines = 0.f;  // 0..1 holo scanlines on skin
    // mask geometry (pixels)
    float face_cx = 0, face_cy = 0;     // face ellipse center
    float face_rx = 1, face_ry = 1;     // semi-axes along right/up basis
    float upx = 0,  upy = -1;           // face up unit vector
    float eyeL_x = 0, eyeL_y = 0, eyeR_x = 0, eyeR_y = 0, eye_r = 0;
    float mouth_x = 0, mouth_y = 0, mouth_r = 0;
    float mouth_sw = 1, mouth_sh = 1;   // lip ellipse semi-axes (px), face basis
    float lip_poly[12][2] = {};         // outer-lip ring (px) — follows the smile
    float brow_r  = 0;                  // exclusion above the eyes
};
uintptr_t face_beauty_apply(uintptr_t src_tex, int slot, int w, int h,
                            const FaceBeautyParams& p);

uintptr_t face_warp_apply(uintptr_t src_tex, int slot, int w, int h,
                          const float* bumps, int n_bumps);

// Face-warp/sprite output slot for a video clip — a bank parallel to the
// per-clip fx slots, so the warped texture survives until scene composite
// without clobbering the clip's own fx_apply output.
int fx_face_clip_slot(int video_slot);

// Dedicated output slots for the face-filter PICKER preview thumbnails — one
// per filter id, persistent so the whole grid can show all warps at once.
int fx_face_preview_slot(int filter_id);

// Doggy sprites at playback/export: alpha-blend textured quads over src_tex
// inside the slot's FBO. Corner positions in target-frame UV ((0,0) = image
// top-left), order tl/tr/br/bl; u0/u1 flip the sprite art horizontally.
// In-place when src_tex is already the slot's texture (face_warp output).
struct FaceSpriteQuad {
    unsigned tex = 0;
    float    p[4][2];
    float    u0 = 0.f, u1 = 1.f;
};
uintptr_t face_sprites_apply(uintptr_t src_tex, int slot, int w, int h,
                             const FaceSpriteQuad* quads, int n);

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
// u0..v1: source UV window (non-destructive crop) — the quad samples only this
// sub-rect of clip_tex. Defaults sample the full texture.
void      scene_add_layer(uintptr_t clip_tex, float cx, float cy, float hw, float hh,
                          float cos_r, float sin_r, float alpha,
                          float u0 = 0.f, float v0 = 0.f,
                          float u1 = 1.f, float v1 = 1.f);
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
