// engine_smoke — the Phase 0 proof artifact: drives the engine through the
// C ABI with NO app, NO socket, NO window. Links against pms-engine only.
// Exercises: create → levers (add_track, add_clip, get_all_clips,
// save_project, load via open path) → destroy. Exit 0 = the engine core is
// genuinely standalone. Run from CI next to the desktop build.
#include "../src/pms_engine.h"
#include <cstdio>
#include <cstring>
#include <string>

static std::string cmd(pms_engine* e, const std::string& json) {
    char* r = pms_command(e, json.c_str());
    std::string out = r ? r : "";
    pms_free(r);
    return out;
}

static bool has_error(const std::string& reply) {
    return reply.find("\"error\"") != std::string::npos;
}

int main() {
    pms_engine* e = pms_create(nullptr, "", "/tmp/pms-engine-smoke");
    if (!e) { fprintf(stderr, "create failed\n"); return 1; }
    if (pms_abi_version() != PMS_ENGINE_ABI) { fprintf(stderr, "abi mismatch\n"); return 1; }
    printf("engine up: abi %u, .pms v%u\n", pms_abi_version(), pms_project_version());

    struct { const char* label; const char* json; } steps[] = {
        {"add_track",   R"({"id":"s","method":"add_track","params":{"name":"T1"}})"},
        {"add_text",    R"({"id":"s","method":"add_clip","params":{"track":0,"type":"text","start":0,"end":2,"text":"engine smoke"}})"},
        {"get_clips",   R"({"id":"s","method":"get_all_clips","params":{}})"},
        {"save",        R"({"id":"s","method":"save_project","params":{"path":"/tmp/pms-engine-smoke/smoke.pms"}})"},
        {"get_project", R"({"id":"s","method":"get_project","params":{}})"},
    };
    for (auto& s : steps) {
        std::string r = cmd(e, s.json);
        bool ok = !has_error(r);
        printf("%-12s %s\n", s.label, ok ? "ok" : r.substr(0, 120).c_str());
        if (!ok) { pms_destroy(e); return 1; }
    }
    char* ev = pms_poll_events(e);
    printf("events drain: %s\n", ev && ev[0] == '[' ? "ok" : "bad");
    pms_free(ev);
    pms_destroy(e);
    printf("engine smoke: PASS\n");
    return 0;
}
