#pragma once
// Async per-clip ML skin-mask cache for the skin-gated beauty shaders.
//
// Design (parallel to body_fx's mask-texture cache, but frame-keyed):
// - Key: source media path + frame size. Beauty shaders run on the composited
//   frame (w,h), so the mask is cached at exactly that size — no rescale.
// - Value: GL R8 texture of face+body-skin confidence (0..1), LRU-capped.
// - Fill: a worker thread runs skin_segment_run on the clip's source frame
//   (export path) or the decoded preview pixels (canvas path) and uploads on
//   the GL thread. The UI thread NEVER blocks: lookup returns 0 (no mask)
//   while the fill is in flight, and shaders fall back to YCbCr meanwhile.
// - When the model file is missing every function below is a cheap no-op
//   (0 / false) and shaders stay on YCbCr permanently.
//
// Callers: fx_shader.cpp skin_smooth/glass_skin passes (GL thread); the fill
// worker is internal. Headless (no GL): all texture functions return 0.
#include <cstdint>
#include <string>

// Is an ML mask usable right now (model installed)?
bool skin_mask_available();

// Non-blocking lookup of the cached mask texture for this source frame at
// this frame size. Returns 0 when absent (still filling, evicted, failed, or
// headless) — the caller must fall back to the YCbCr window. GL thread only.
// `src_key` identifies the source frame (clip path + source-frame index);
// `w,h` is the CURRENT frame size (mask is exact-size or miss).
unsigned skin_mask_texture(const std::string& src_key, int w, int h);

// Request an async fill for this source frame. Cheap + idempotent (in-flight
// and ready keys are skipped). `rgb` is copied. Safe from any thread.
// `w,h` = source frame size; `frame_w,frame_h` = display size to cache at.
void skin_mask_request(const std::string& src_key,
                       const uint8_t* rgb, int w, int h,
                       int frame_w, int frame_h);

// Pump completed fills to GL textures. GL thread only (called from fx_apply).
// `max_uploads` bounds per-frame upload work (each is one TexImage).
void skin_mask_pump(int max_uploads = 1);

// Drop all cached masks (call on model removal / memory pressure).
void skin_mask_evict_all();

// No-GL/CPU-test hook: how many fills are pending or ready-but-unpumped.
int skin_mask_pending();
