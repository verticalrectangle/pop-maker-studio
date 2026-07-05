#pragma once
// metal_render.h — the iOS Metal RenderSurface (Phase 3). The GL renderer is
// compiled out under PMS_HEADLESS; this provides pms_render's real backend on
// Apple: composite the engine's current frame into the app-provided MTLTexture.
//
// This first cut establishes the pipeline end-to-end (MTLDevice from
// pms_create → command queue → render pass into the target → a full-screen
// shader). It grows into the scene compositor (procedural backgrounds via the
// transpiled MSL registry, then textured layers, text, and the FX passes).
#include <cstdint>

// Called from pms_create with the app's MTLDevice (bridged void*). Idempotent.
void metal_render_init(void* mtl_device);

// Set the current content frame — BGRA8, tightly packed, top-left origin (the
// AVFoundation / camera pixel layout). pms_render composites it aspect-fit over
// the background. Pass null / 0 dims to clear back to the aurora. Thread-safe.
void metal_render_set_content_bgra(const void* bgra, int w, int h);

// Set the content frame from a CVPixelBufferRef (32BGRA) — the AVFoundation
// capture/decode output. Handles row stride. Bridged void* (CoreVideo not in
// this header to keep it plain C). Thread-safe.
void metal_render_submit_pixelbuffer(void* cv_pixel_buffer);

// Composite one frame into `mtl_texture` (id<MTLTexture>, bridged). `t` drives
// time-based animation (wall clock). Returns 0 on success, non-zero if Metal
// isn't ready. No-op (returns 1) if init hasn't run.
int  metal_render_frame(void* mtl_texture, int w, int h, double t);

// Set the ordered render-time FX stack the Metal backend applies to the current
// frame — a JSON array [{"fx_type":str,"params":{name:num,...}}, ...] (the
// set_live_fx payload). Cheap when unchanged; parses + rebuilds only on change.
void metal_render_set_live_fx_stack(const char* json_utf8);

// Optional: override where msl/<name>.metal + params_manifest.json are loaded
// from (default = the app bundle's msl/ dir). For headless/test.
void metal_render_set_shader_dir(const char* dir);

// FX-runner diagnostic JSON (manifest_count, has_content, stack[] with per-effect
// in_manifest/pso_ok) — surfaced through the `fx_debug` IPC command.
const char* metal_render_fx_debug(void);

// Timeline time of the current content frame — FX apply only within their [start,end].
void metal_render_set_content_time(double t);
