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

// Composite one frame into `mtl_texture` (id<MTLTexture>, bridged). `t` drives
// time-based animation (wall clock). Returns 0 on success, non-zero if Metal
// isn't ready. No-op (returns 1) if init hasn't run.
int  metal_render_frame(void* mtl_texture, int w, int h, double t);
