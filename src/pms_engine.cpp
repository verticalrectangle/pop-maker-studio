// pms_engine.cpp — the C ABI implementation over the engine (pms_engine.h).
// One engine instance owns an AppState and dispatches levers through
// engine_command — the same chokepoint the desktop socket server and the
// agent tools use. Rendering (pms_render) arrives with the RenderSurface
// seam (Phase 2); until then this ABI serves state, levers, and events —
// exactly what the headless test target and early SwiftUI screens need.
#include "pms_engine.h"
#include "app.h"
#include "ipc_server.h"
#include "engine_runtime.h"
#if defined(__APPLE__)
#include "metal_render.h"
#endif
#include "globals.h"
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
    // asset_root: models resolve relative to the binary today
    // (app_models_dir); an explicit override lands with the iOS bundle work.
    (void)asset_root;
#if defined(__APPLE__)
    if (graphics_device) metal_render_init(graphics_device);
#endif
    return e;
}

void pms_destroy(pms_engine* e) { delete e; }

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
    metal_render_set_live_fx(e->state.live_fx_type, e->state.live_fx_amount);
    return metal_render_frame(mtl_texture, w, h, e->clock);
#else
    (void)e; (void)mtl_texture; (void)w; (void)h;
    return 0;   // desktop renders through its own GL loop
#endif
}
void pms_submit_camera_frame(pms_engine*, void* cv_pixel_buffer, int, double) {
#if defined(__APPLE__)
    // Feed the AVFoundation frame straight to the Metal compositor. Full engine
    // intake (timeline placement, per-clip decode) is the rest of Phase 4; this
    // gives a live preview now.
    metal_render_submit_pixelbuffer(cv_pixel_buffer);
#else
    (void)cv_pixel_buffer;
#endif
}
void pms_submit_mic_block(pms_engine*, const float*, size_t, double) {}  // Phase 4
char* pms_model_status(pms_engine*) { return dup_cstr("[]"); }

void pms_free(char* p) { free(p); }

uint32_t pms_abi_version(void) { return PMS_ENGINE_ABI; }

}  // extern "C"
