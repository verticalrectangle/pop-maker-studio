// pms_engine.cpp — the C ABI implementation over the engine (pms_engine.h).
// One engine instance owns an AppState and dispatches levers through
// engine_command — the same chokepoint the desktop socket server and the
// agent tools use. Rendering (pms_render) arrives with the RenderSurface
// seam (Phase 2); until then this ABI serves state, levers, and events —
// exactly what the headless test target and early SwiftUI screens need.
#include "pms_engine.h"
#include "app.h"
#include "ipc_server.h"
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

pms_engine* pms_create(void* /*graphics_device — Metal on iOS; desktop GL
                         callers own their context*/,
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
    return e;
}

void pms_destroy(pms_engine* e) { delete e; }

void pms_tick(pms_engine* e, double dt) {
    if (!e) return;
    e->clock += dt;
    // Worker pumps (proxy scans, slot opens, pipeline polls) currently run
    // inside the desktop frame; they migrate here as the app loop thins out
    // (tracked in docs/IOS_PORT_PLAN.md Phase 0 exit).
}

char* pms_command(pms_engine* e, const char* json_request) {
    if (!e || !json_request) return dup_cstr("{\"error\":\"null engine/request\"}");
    return dup_cstr(engine_command(e->state, json_request));
}

char* pms_poll_events(pms_engine* e) {
    if (!e) return dup_cstr("[]");
    std::lock_guard<std::mutex> lk(e->events_mtx);
    nlohmann::json arr = nlohmann::json::array();
    for (auto& ev : e->events) arr.push_back(std::move(ev));
    e->events.clear();
    return dup_cstr(arr.dump());
}

void pms_free(char* p) { free(p); }

uint32_t pms_abi_version(void) { return PMS_ENGINE_ABI; }

}  // extern "C"
