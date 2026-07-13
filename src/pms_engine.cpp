// pms_engine.cpp — the C ABI implementation over the engine (pms_engine.h).
// One engine instance owns an AppState and dispatches levers through
// engine_command — the same chokepoint the desktop socket server and the
// agent tools use. Rendering (pms_render) arrives with the RenderSurface
// seam (Phase 2); until then this ABI serves state, levers, and events —
// exactly what the headless test target and early SwiftUI screens need.
#include "pms_engine.h"
#include "app.h"
#include "audio.h"
#include "face_track.h"
#include "arkit_face.h"
#include "ipc_server.h"
#include "engine_runtime.h"
#if defined(__APPLE__)
#include "metal_render.h"
#endif
#include "globals.h"
#include "paths.h"
#include "json.hpp"
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <filesystem>

struct pms_engine {
    AppState state;
    std::mutex events_mtx;
    std::vector<nlohmann::json> events;
    double clock = 0.0;
};

static char* dup_cstr(const std::string& s) {
    char* out = (char*)malloc(s.size() + 1);
    if (out) memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

extern "C" {

pms_engine* pms_create(void* graphics_device,   // MTLDevice* on iOS; null on desktop
                       const char* asset_root,
                       const char* state_root) {
    auto* e = new pms_engine();
    if (state_root && *state_root) {
        g_managed_dir = state_root;
        std::error_code ec;
        std::filesystem::create_directories(g_managed_dir, ec);
    }
    // asset_root: centralized path override (paths.cpp). When set, bundled
    // assets/models resolve under it instead of the binary's directory.
    if (asset_root && *asset_root) pms_set_asset_root(asset_root);
#if defined(__APPLE__)
    if (graphics_device) metal_render_init(graphics_device);
#endif
    return e;
}
void pms_destroy(pms_engine* e) {
    if (e) {
        face_feed_enable(false);
        face_track_shutdown();
        delete e;
    }
}

void pms_tick(pms_engine* e, double dt) {
    if (!e) return;
    e->clock += dt;
    // The shared engine heartbeat (same function the desktop frame calls).
    // gl_ready=false until the RenderSurface seam provides a context here.
    engine_tick(e->state, dt, /*gl_ready=*/false);
}

char* pms_command(pms_engine* e, const char* json_request) {
    if (!e || !json_request) return dup_cstr("{\"error\":\"null engine/request\"}");
    return dup_cstr(engine_command(e->state, json_request));
}

char* pms_poll_events(pms_engine* e) {
    if (!e) return dup_cstr("[]");
    return dup_cstr(engine_drain_events());
}

int pms_render(pms_engine* e, void* mtl_texture, int w, int h) {
#if defined(__APPLE__)
    if (!e) return 1;
    // Legacy single-content fallback still consumes the set_live_fx adapter;
    // the scene path (any layer frames submitted) derives FX from AppState.
    metal_render_set_live_fx_stack(e->state.live_fx_json.c_str());
    return metal_render_frame(mtl_texture, w, h, e->clock, &e->state);
#else
    (void)e; (void)mtl_texture; (void)w; (void)h;
    return 0;   // desktop renders through its own GL loop
#endif
}
void pms_render_wait(pms_engine*) {
#if defined(__APPLE__)
    metal_render_wait();
#endif
}
void pms_submit_camera_frame(pms_engine*, void* cv_pixel_buffer, int rotation, double host_time) {
#if defined(__APPLE__)
    // Feed the AVFoundation frame straight to the Metal compositor. host_time is
    // the frame's timeline position, used to window FX to their brick spans.
    metal_render_submit_pixelbuffer(cv_pixel_buffer);
    metal_render_set_content_time(host_time);
    // Face side-feed (record-mode makeup filters) — conversion + gating live
    // in metal_render.mm (CoreVideo there; CarbonCore's Marker collides with
    // the engine's in plain C++ TUs). Upright frames only.
    if (cv_pixel_buffer && rotation == 0)
        metal_render_face_feed(cv_pixel_buffer, host_time);
    arkit_face_note_camera_time(host_time);
#else
    (void)cv_pixel_buffer; (void)rotation; (void)host_time;
#endif
}
void pms_submit_person_matte(pms_engine*, void* cv_pixel_buffer_r8, double host_time) {
#if defined(__APPLE__)
    metal_render_submit_matte(cv_pixel_buffer_r8, host_time);
#else
    (void)cv_pixel_buffer_r8; (void)host_time;
#endif
}

void pms_submit_arkit_face(pms_engine*, const float* vertices_1220x2,
                           const float* uvs_1220x2,
                           const float* blendshapes_52, int n_faces,
                           int w, int h) {
    if (!vertices_1220x2 || !blendshapes_52 || n_faces <= 0) {
        arkit_face_clear();
        return;
    }
    if (n_faces > ARKIT_MAX_FACES) n_faces = ARKIT_MAX_FACES;
    ARKitFaceObs obs[ARKIT_MAX_FACES];
    for (int f = 0; f < n_faces; ++f) {
        for (int i = 0; i < ARKIT_NPTS; ++i) {
            obs[f].pts[i][0] = vertices_1220x2[f * ARKIT_NPTS * 2 + i * 2 + 0];
            obs[f].pts[i][1] = vertices_1220x2[f * ARKIT_NPTS * 2 + i * 2 + 1];
            if (uvs_1220x2) {
                obs[f].uvs[i][0] = uvs_1220x2[f * ARKIT_NPTS * 2 + i * 2 + 0];
                obs[f].uvs[i][1] = uvs_1220x2[f * ARKIT_NPTS * 2 + i * 2 + 1];
            } else {
                obs[f].uvs[i][0] = 0.f;
                obs[f].uvs[i][1] = 0.f;
            }
        }
        for (int b = 0; b < ARKIT_NBLEND; ++b) {
            obs[f].blend[b] = blendshapes_52[f * ARKIT_NBLEND + b];
        }
        obs[f].has_blend = true;
        obs[f].score = 1.0f;
        obs[f].valid = true;
        obs[f].w = w;
        obs[f].h = h;
    }
    arkit_face_submit(obs, n_faces);
}
void pms_submit_arkit_face_3d(pms_engine*, const float* verts_1220x3,
                              const float* model_4x4,
                              const float* view_4x4,
                              const float* proj_4x4,
                              const float* eye_l_4x4,
                              const float* eye_r_4x4,
                              const float* blendshapes_52,
                              int is_tracked, int w, int h) {
    if (!verts_1220x3 || !model_4x4 || !view_4x4 || !proj_4x4 ||
        !is_tracked || w <= 0 || h <= 0) {
        arkit_face3d_submit(nullptr);
        return;
    }
    static ARKitFace3D f;   // large; engine submits are single-threaded
    f.valid = true;
    memcpy(f.verts, verts_1220x3, sizeof(f.verts));
    memcpy(f.model, model_4x4, sizeof(f.model));
    memcpy(f.view, view_4x4, sizeof(f.view));
    memcpy(f.proj, proj_4x4, sizeof(f.proj));
    if (eye_l_4x4) memcpy(f.eye_l, eye_l_4x4, sizeof(f.eye_l));
    else           memset(f.eye_l, 0, sizeof(f.eye_l));
    if (eye_r_4x4) memcpy(f.eye_r, eye_r_4x4, sizeof(f.eye_r));
    else           memset(f.eye_r, 0, sizeof(f.eye_r));
    f.has_blend = blendshapes_52 != nullptr;
    if (blendshapes_52) memcpy(f.blend, blendshapes_52, sizeof(f.blend));
    else                memset(f.blend, 0, sizeof(f.blend));
    f.w = w; f.h = h;
    arkit_face3d_submit(&f);
}

void pms_submit_layer_frame(pms_engine*, int track, int clip,
                            void* cv_pixel_buffer_bgra,
                            int rotation_quarter_turns, double host_time) {
#if defined(__APPLE__)
    metal_render_submit_layer(track, clip, cv_pixel_buffer_bgra,
                              rotation_quarter_turns, host_time);
#else
    (void)track; (void)clip; (void)cv_pixel_buffer_bgra;
    (void)rotation_quarter_turns; (void)host_time;
#endif
}
void pms_submit_mic_block(pms_engine* e, const float* interleaved_lr,
                          size_t frames, double sample_rate) {
    // Thin shim onto the audio module's injected-capture ring (audio.h):
    // copies + resamples to 44.1k at push time on the caller's capture
    // thread; drained by audio_capture_drain like native device capture.
    if (!e || !interleaved_lr || frames == 0) return;
    audio_capture_push(interleaved_lr, frames, sample_rate);
}
char* pms_model_status(pms_engine*) {
    // Real status for every model file the engine references (transcribe,
    // bg_remove, separate, vc/rvc, forced_align, vision_caption, face_track).
    namespace fs = std::filesystem;
    static const struct { const char* name; const char* file; } kModels[] = {
        { "whisper",             "ggml-large-v3-turbo-q5_0.bin" },
        { "rvm",                 "rvm_mobilenetv3_fp32.onnx"    },
        { "u2net_human_seg",     "u2net_human_seg.onnx"         },
        { "kim_vocal_2",         "Kim_Vocal_2.onnx"             },
        { "hubert",              "hubert.onnx"                  },
        { "rmvpe",               "rmvpe.onnx"                   },
        { "rvc_pitch_bases",     "rvc_pitch_bases.bin"          },
        { "wav2vec2_ctc",        "wav2vec2_ctc.onnx"            },
        { "moondream_encoder",   "moondream-encoder.onnx"       },
        { "moondream_decoder",   "moondream-decoder.onnx"       },
        { "moondream_embed",     "embed_tokens.onnx"            },
        { "moondream_tokenizer", "moondream-tokenizer.json"     },
        { "face_yunet",          "face/yunet.onnx"              },
        { "face_landmarks",      "face/face_landmarks_v2.onnx"  },
        { "face_blendshapes",    "face/face_blendshapes.onnx"   },
    };
    std::string mdir = app_models_dir();
    nlohmann::json out;
    out["models_dir"] = mdir;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : kModels) {
        std::error_code ec;
        fs::path p = fs::path(mdir) / m.file;
        bool present = fs::exists(p, ec) && !ec;
        uint64_t bytes = 0;
        if (present) {
            auto sz = fs::file_size(p, ec);
            if (!ec) bytes = (uint64_t)sz;
        }
        arr.push_back({{"name", m.name}, {"file", m.file},
                       {"present", present}, {"bytes", bytes}});
    }
    out["models"] = arr;
    return dup_cstr(out.dump());
}

void pms_free(char* p) { free(p); }

uint32_t pms_abi_version(void) { return PMS_ENGINE_ABI; }

}  // extern "C"
