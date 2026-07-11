// project_meta.cpp — project-adjacent persistence hoisted from
// ui/screen_home.cpp during the iOS engine extraction (Phase 0): recent-
// projects list, crash-recovery slot paths + clear, and the open-project
// chokepoint shared by the Home screen and the load_project lever. The Home
// screen keeps autosave_tick / restore_recovery (thin wrappers over the
// exported paths). Behavior unchanged.
#include "engine_seams.h"
#include "app.h"
#include "audio.h"
#include "video.h"
#include "history.h"
#include "project.h"
#include "transcribe.h"
#include "globals.h"
#include "json.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;

// ── Recent-projects persistence (mirrors recent_media) ────────────────────────
static std::string recents_path() {
    const char* h = getenv("HOME");
    return h ? std::string(h) + "/.config/pop-maker-studio/recent_projects.json"
             : "/tmp/pop_maker_recent_projects.json";
}

static std::vector<std::string>& recents_store() {
    static std::vector<std::string> s;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        try {
            std::ifstream f(recents_path());
            if (f) {
                auto j = nlohmann::json::parse(f, nullptr, false);
                if (!j.is_discarded() && j.is_array())
                    s = j.get<std::vector<std::string>>();
            }
        } catch (...) {}
    }
    return s;
}

std::vector<std::string> recent_projects_list() {
    std::vector<std::string> out;
    for (auto& p : recents_store())
        if (!p.empty() && fs::exists(p)) out.push_back(p);
    return out;
}

void recent_projects_push(const std::string& path) {
    if (path.empty()) return;
    auto& v = recents_store();
    v.erase(std::remove(v.begin(), v.end(), path), v.end());
    v.insert(v.begin(), path);
    if (v.size() > 30) v.resize(30);
    try {
        fs::create_directories(fs::path(recents_path()).parent_path());
        std::ofstream(recents_path()) << nlohmann::json(v).dump(2);
    } catch (...) {}
}

static std::string recovery_dir() {
    std::string base = !g_managed_dir.empty()
        ? g_managed_dir
        : (getenv("HOME") ? std::string(getenv("HOME")) + "/.local/share/pop-maker-studio"
                          : std::string("/tmp/pop-maker-studio"));
    return base + "/recovery";
}

std::string recovery_pms_path()  { return recovery_dir() + "/autosave.pms"; }
std::string recovery_meta_path() { return recovery_dir() + "/autosave.json"; }

void recovery_clear() {
    std::error_code ec;
    fs::remove(recovery_pms_path(), ec);
    fs::remove(recovery_meta_path(), ec);
}

bool open_project_path(AppState& state, const std::string& path) {
    if (path.empty()) return false;
    // Phase timings on stderr — the open used to freeze with no clue where.
    auto t0 = std::chrono::steady_clock::now();
    auto lap = [&](const char* what) {
        auto t1 = std::chrono::steady_clock::now();
        fprintf(stderr, "[open] %-22s %6.1f ms\n", what,
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        t0 = t1;
    };
    AppState loaded;
    if (!project_load(loaded, path)) return false;
    lap("parse .pms");
    bool mr = state.models_ready, ms = state.models_skipped;
    transcribe_cancel(); history_clear();
    lap("transcribe/history");
    audio_shutdown(); audio_clips_clear(); video_close();
    lap("audio/video teardown");
    state = std::move(loaded);
    state.models_ready = mr; state.models_skipped = ms;
    state.project_path = path;
    state.splash_timer = 0.f;
    state.in_studio    = true;
    audio_init();
    lap("audio_init");
    if (!state.audio_path.empty()) audio_load(state.audio_path.c_str());
    lap("audio_load kick");
    // Slots open incrementally in the studio frame (progress bar) — opening
    // them here synchronously froze the app for seconds on media-heavy projects.
    queue_video_slot_opens(state);
    for (auto& tr : state.tracks)
        for (auto& cl : tr.clips)
            if (cl.clip_type == ClipType::Audio && !cl.text.empty())
                audio_source_ensure(cl.text);
    lap("queue+audio ensure");
    recent_projects_push(path);
    history_push(state, "Open project");
    mark_project_clean(state);
    lap("recents+history");
    return true;
}
