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

struct AppState;   // app.h — the renderer walks state.tracks for the scene loop

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
// `state` (may be null): when any layer frames are present the renderer runs
// the scene compositor over state->tracks (desktop canvas.cpp Pass-1 parity);
// otherwise it falls back to the legacy single-content path.
int  metal_render_frame(void* mtl_texture, int w, int h, double t,
                        const AppState* state);

// Store one visual layer's frame keyed by engine (track, clip): a BGRA
// CVPixelBufferRef mapped zero-copy, retained until superseded. NULL clears
// the key. rotation in quarter turns rotates the buffer upright. Thread-safe.
void metal_render_submit_layer(int track, int clip, void* cv_pixel_buffer_bgra,
                               int rotation_quarter_turns, double host_time_seconds);

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

// Latest person matte from the platform segmenter (Vision) — a OneComponent8
// (R8) CVPixelBufferRef bridged as void*; retained here, previous released.
// NULL clears (content blits plain again). Same camera frame as the content
// buffer, so it is sampled with the content's normalized UVs. `t` is the host
// time of the matte frame. Thread-safe.
void metal_render_submit_matte(void* cv_pixel_buffer_r8, double t);

// Timeline time of the current content frame — FX apply only within their [start,end].
void metal_render_set_content_time(double t);

// Block until the GPU finishes committed frames — for offline export readback.
void metal_render_wait(void);
