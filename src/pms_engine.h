// pms_engine.h — the C ABI over the Pop Maker Studio engine (pms-engine
// static lib). This is the surface the iOS Swift shell consumes and the
// headless test target proves; it deliberately matches the contract published
// in the pms-ios repo (Engine/include/pms_engine.h there is the same file,
// kept in sync by hand until the engine build exports it).
//
// Desktop notes:
//  - `graphics_device` is the platform graphics handle: MTLDevice* on iOS,
//    ignored (pass null) on desktop GL where the caller owns the context.
//  - pms_render is not implemented yet (Phase 2 — RenderSurface seam); the
//    desktop app still renders through its own loop.
//  - pms_poll_events currently drains a minimal engine event queue; the
//    full event formalization tracks docs/IOS_PORT_PLAN.md Phase 0.
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pms_engine pms_engine;

pms_engine* pms_create(void* graphics_device,
                       const char* asset_root,
                       const char* state_root);
void        pms_destroy(pms_engine*);

// Advance clocks / pump worker results. Call once per frame.
void pms_tick(pms_engine*, double dt_seconds);

// Composite the current frame into a Metal texture (MTLTexture*, bridged
// void*). Returns 0 on success. STUB until the Metal RenderSurface (Phase 3).
int pms_render(pms_engine*, void* mtl_texture, int width, int height);

// Block until the GPU has finished the committed render — for offline export
// (submit a frame, render into an output texture, wait, read it back, encode).
void pms_render_wait(pms_engine*);

// Capture intake (AVFoundation feeds these). STUB until the CaptureBackend
// (Phase 4). camera: CVPixelBufferRef; mic: interleaved stereo float.
void pms_submit_camera_frame(pms_engine*, void* cv_pixel_buffer,
                             int rotation_quarter_turns, double host_time_seconds);
void pms_submit_mic_block(pms_engine*, const float* interleaved_lr,
                          size_t frames, double sample_rate);

// Person matte from the platform segmenter (Vision on iOS): a retained
// OneComponent8 CVPixelBufferRef bridged as void*. NULL clears the matte.
void pms_submit_person_matte(pms_engine*, void* cv_pixel_buffer_r8,
                             double host_time_seconds);

// Submit ARKit face anchor data (front camera, TrueDepth). Up to 4 faces.
// vertices: flat float array [n_faces * 1220 * 2] — 2D pixel coords in the
//   composited frame space (projected by the Swift caller).
// blendshapes: flat float array [n_faces * 52] — ARKit blendshape coefficients.
// w,h: size of the frame the vertices are projected into (all faces share it).
// n_faces: number of tracked faces (0..4). Pass n_faces=0 to clear (face lost).
void pms_submit_arkit_face(pms_engine*, const float* vertices_1220x2,
                           const float* blendshapes_52, int n_faces,
                           int w, int h);

// Submit one visual layer's frame, addressed by engine clip identity
// (track index, clip index). BGRA CVPixelBufferRef bridged as void*; the
// engine retains it until superseded. Text/overlay layers may be submitted
// once and persist until replaced or cleared (pass NULL to clear that key).
// rotation_quarter_turns rotates the buffer upright (camera parity). When any
// layer frames are present pms_render walks the timeline tracks as a scene
// compositor; with none it falls back to the single-content (camera) path.
void pms_submit_layer_frame(pms_engine*, int track, int clip,
                            void* cv_pixel_buffer_bgra,
                            int rotation_quarter_turns,
                            double host_time_seconds);

// JSON status of model packs (bundled/absent/downloading/ready).
char* pms_model_status(pms_engine*);

// Execute one lever (same JSON protocol as the desktop IPC socket / agent
// tools; see docs/LEVERS.md in pms-ios). Returns a malloc'd JSON string —
// free with pms_free. Thread: main.
char* pms_command(pms_engine*, const char* json_request);

// Drain pending engine events as a JSON array string (malloc'd; pms_free).
char* pms_poll_events(pms_engine*);

void pms_free(char*);

#define PMS_ENGINE_ABI 4
uint32_t pms_abi_version(void);
uint32_t pms_project_version(void);

#ifdef __cplusplus
}
#endif
