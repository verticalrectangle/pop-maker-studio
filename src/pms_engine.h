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

// ARFrame.lightEstimate for the face being submitted (face tracking delivers
// an ARDirectionalLightEstimate). primary_dir is world space — the direction
// the light travels — in the same world the view matrix maps from. sh is
// sphericalHarmonicsCoefficients verbatim (27 floats). directional == 0 when
// only the ambient terms are valid.
typedef struct pms_arkit_light {
    float primary_dir[3];
    float primary_intensity;      // lumens
    float sh[27];
    float ambient_intensity;      // lumens, 1000 = neutral
    float ambient_kelvin;
    int   directional;
} pms_arkit_light;

// ARKit face state for one ARFrame (TrueDepth front camera); submit it in the
// same callback as that frame's pms_submit_camera_frame. All matrices are
// column-major simd_float4x4 layout. verts are ARFaceGeometry.vertices in
// face-anchor model space (meters); model = anchor transform; view/proj = the
// camera's viewMatrix/projectionMatrix for the PORTRAIT viewport (w x h) the
// submitted camera frames use. blendshapes_52 in MediaPipe order (_neutral at
// 0); light may be NULL. is_tracked == 0 clears the slot (face lost): the
// engine hides makeup instead of painting with frozen geometry.
void pms_submit_arkit_face_3d(pms_engine*, const float* verts_1220x3,
                              const float* model_4x4,
                              const float* view_4x4,
                              const float* proj_4x4,
                              const float* blendshapes_52,
                              const pms_arkit_light* light,
                              int is_tracked, int w, int h);

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

#define PMS_ENGINE_ABI 5
uint32_t pms_abi_version(void);
uint32_t pms_project_version(void);

#ifdef __cplusplus
}
#endif
