#include "screens.h"
#include "studio_shared.h"
#include "filepicker.h"
#include "theme.h"
#include "app.h"
#include "audio.h"
#include "video.h"
#include "project.h"
#include "transcribe.h"
#include "history.h"
#include "paths.h"

#include <imgui.h>
#include "json.hpp"
#include <algorithm>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <sys/stat.h>

namespace fs = std::filesystem;
extern ImFont* g_font_black;
extern ImFont* g_font_bold;

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

// ── Autosave / crash recovery ───────────────────────────────────────────────────
// We continuously write the live project to a fixed recovery slot while editing.
// On a clean shutdown the slot is deleted; if the app crashes it survives, and the
// launcher offers to restore it on the next start.
extern std::string g_managed_dir;  // ~/.local/share/pop-maker-studio
static std::string mtime_str(const std::string& path);  // defined below

static std::string recovery_dir() {
    std::string base = !g_managed_dir.empty()
        ? g_managed_dir
        : (getenv("HOME") ? std::string(getenv("HOME")) + "/.local/share/pop-maker-studio"
                          : std::string("/tmp/pop-maker-studio"));
    return base + "/recovery";
}
static std::string recovery_pms()  { return recovery_dir() + "/autosave.pms"; }
static std::string recovery_meta() { return recovery_dir() + "/autosave.json"; }

static bool project_has_content(const AppState& s) {
    for (auto& tr : s.tracks)
        if (!tr.clips.empty()) return true;
    return false;
}

void recovery_clear() {
    std::error_code ec;
    fs::remove(recovery_pms(), ec);
    fs::remove(recovery_meta(), ec);
}

void autosave_tick(AppState& state, float dt) {
    static float    accum    = 0.f;
    static int      last_pos = -999999;  // history cursor at the last successful write
    const float     INTERVAL = 20.f;     // seconds between recovery writes

    accum += dt;
    if (accum < INTERVAL) return;
    accum = 0.f;

    if (!project_has_content(state)) return;
    int pos = history_pos();
    if (pos == last_pos) return;         // nothing changed since the last write

    try {
        fs::create_directories(recovery_dir());
        std::string tmp = recovery_pms() + ".tmp";
        if (project_save(state, tmp)) {
            std::error_code ec;
            fs::rename(tmp, recovery_pms(), ec);   // atomic swap
            if (ec) { fs::remove(tmp, ec); return; }
            nlohmann::json m;
            m["orig_path"] = state.project_path;
            m["name"]      = state.project_path.empty()
                                 ? std::string("Untitled project")
                                 : fs::path(state.project_path).stem().string();
            std::ofstream(recovery_meta()) << m.dump(2);
            last_pos = pos;
        }
    } catch (...) {}
}

bool recovery_available(std::string* name, std::string* when) {
    if (!fs::exists(recovery_pms())) return false;
    if (name) {
        *name = "Recovered project";
        try {
            std::ifstream f(recovery_meta());
            if (f) {
                auto j = nlohmann::json::parse(f, nullptr, false);
                if (!j.is_discarded() && j.contains("name"))
                    *name = j["name"].get<std::string>();
            }
        } catch (...) {}
    }
    if (when) *when = mtime_str(recovery_pms());
    return true;
}

// ── Project actions ───────────────────────────────────────────────────────────
void enter_new_project(AppState& state) {
    // Reset to a blank project but keep the environment flags (models/skip), or
    // the setup screen would reappear, and enter the editor.
    bool mr = state.models_ready, ms = state.models_skipped;
    transcribe_cancel(); history_clear();
    audio_shutdown(); audio_clips_clear(); video_close();
    state = AppState{};
    state.models_ready = mr; state.models_skipped = ms;
    state.splash_timer = 0.f;
    state.in_studio    = true;
    audio_init();
    history_push(state, "New project");
    mark_project_clean(state);
}

static void open_project_path(AppState& state, const std::string& path) {
    if (path.empty()) return;
    AppState loaded;
    if (!project_load(loaded, path)) return;
    bool mr = state.models_ready, ms = state.models_skipped;
    transcribe_cancel(); history_clear();
    audio_shutdown(); audio_clips_clear(); video_close();
    state = std::move(loaded);
    state.models_ready = mr; state.models_skipped = ms;
    state.project_path = path;
    state.splash_timer = 0.f;
    state.in_studio    = true;
    audio_init();
    if (!state.audio_path.empty()) audio_load(state.audio_path.c_str());
    // Slots open incrementally in the studio frame (progress bar) — opening
    // them here synchronously froze the app for seconds on media-heavy projects.
    queue_video_slot_opens(state);
    for (auto& tr : state.tracks)
        for (auto& cl : tr.clips)
            if (cl.clip_type == ClipType::Audio && !cl.text.empty())
                audio_source_ensure(cl.text);
    recent_projects_push(path);
    history_push(state, "Open project");
    mark_project_clean(state);
}

// Restore the crash-recovery slot into the live editor. Unlike open_project_path
// we point project_path at the *original* location (from the meta sidecar), so the
// next Save lands where the user expected — not on the recovery file itself.
static void restore_recovery(AppState& state) {
    AppState loaded;
    if (!project_load(loaded, recovery_pms())) return;
    std::string orig;
    try {
        std::ifstream f(recovery_meta());
        if (f) {
            auto j = nlohmann::json::parse(f, nullptr, false);
            if (!j.is_discarded() && j.contains("orig_path"))
                orig = j["orig_path"].get<std::string>();
        }
    } catch (...) {}
    bool mr = state.models_ready, ms = state.models_skipped;
    transcribe_cancel(); history_clear();
    audio_shutdown(); audio_clips_clear(); video_close();
    state = std::move(loaded);
    state.models_ready = mr; state.models_skipped = ms;
    state.project_path = orig;   // may be empty → Save will prompt for a location
    state.splash_timer = 0.f;
    state.in_studio    = true;
    audio_init();
    if (!state.audio_path.empty()) audio_load(state.audio_path.c_str());
    queue_video_slot_opens(state);
    for (auto& tr : state.tracks)
        for (auto& cl : tr.clips)
            if (cl.clip_type == ClipType::Audio && !cl.text.empty())
                audio_source_ensure(cl.text);
    history_push(state, "Recovered project");
    mark_project_clean(state);
}

// ── Rendering ─────────────────────────────────────────────────────────────────
static std::string mtime_str(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return "";
    char buf[32]; std::tm tmv{};
    localtime_r(&st.st_mtime, &tmv);
    strftime(buf, sizeof(buf), "%b %d, %Y  %H:%M", &tmv);
    return buf;
}

void ui_home(AppState& state) {
    ImGuiIO& io = ImGui::GetIO();
    float W = io.DisplaySize.x, H = io.DisplaySize.y;
    ImDrawList* bg = ImGui::GetWindowDrawList();
    bg->AddRectFilled({0, 0}, {W, H}, IM_COL32(14, 14, 18, 255));

    const float pad = 48.f;
    ImGui::SetCursorPos({pad, 40.f});
    ImGui::PushFont(g_font_black);
    ImGui::PushStyleColor(ImGuiCol_Text, to_u32(Col::fg));
    ImGui::TextUnformatted("POP MAKER STUDIO");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SetCursorPosX(pad);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextUnformatted("Pick up where you left off, or start something new.");
    ImGui::PopStyleColor();

    // Cache footer (bottom-left): size readout + one-click clear. The size is a
    // recursive walk, so we cache it and only refresh on first show / after clear.
    {
        static long long  cached_sz = -1;
        if (cached_sz < 0) cached_sz = (long long)cache_size_bytes();
        auto human = [](long long b) {
            char s[32];
            if (b >= 1024ll*1024*1024) snprintf(s, sizeof(s), "%.1f GB", b/(1024.0*1024*1024));
            else if (b >= 1024*1024)   snprintf(s, sizeof(s), "%.0f MB", b/(1024.0*1024));
            else if (b >= 1024)        snprintf(s, sizeof(s), "%.0f KB", b/1024.0);
            else                       snprintf(s, sizeof(s), "%lld B", b);
            return std::string(s);
        };
        ImGui::SetCursorPos({pad, H - 44.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
        ImGui::Text("Media cache: %s", human(cached_sz).c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine(0.f, 14.f);
        ImGui::SetCursorPosY(H - 48.f);
        if (cached_sz > 0 && ui_btn("Clear cache", false, true)) {
            cache_clear();
            cached_sz = (long long)cache_size_bytes();
        }
    }

    // Actions
    ImGui::SetCursorPos({pad, 92.f});
    if (ui_btn("+  New Project", true, false)) enter_new_project(state);
    ImGui::SameLine(0.f, 8.f);
    if (ui_btn("Open Project\xe2\x80\xa6", false, false)) {
        std::string picked = filepicker_open("Open project", "PMS Project", "*.pms");
        open_project_path(state, picked);
    }

    // Crash-recovery banner — only when an autosave slot survived a previous run.
    float yoff = 0.f;
    {
        std::string rname, rwhen;
        if (recovery_available(&rname, &rwhen)) {
            const float bx = pad, by = 140.f, bw = W - pad * 2.f, bh = 56.f;
            ImDrawList* d = ImGui::GetWindowDrawList();
            ImGui::SetCursorPos({bx, by});
            ImGui::PushID("recover");
            ImGui::InvisibleButton("##rec", {bw, bh});
            bool hov = ImGui::IsItemHovered();
            bool clk = ImGui::IsItemClicked();
            ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            d->AddRectFilled(a, b, hov ? IM_COL32(64, 52, 30, 255) : IM_COL32(48, 40, 24, 255), 8.f);
            d->AddRect(a, b, IM_COL32(210, 160, 60, 200), 8.f, 0, 1.5f);
            ImGui::PushFont(g_font_bold);
            d->AddText(ImGui::GetFont(), 15.f, {a.x + 16.f, a.y + 9.f},
                       IM_COL32(245, 220, 160, 255), "\xe2\x9a\xa0  Unsaved work recovered");
            ImGui::PopFont();
            std::string sub = rname + "   \xc2\xb7   " + rwhen + "    \xe2\x80\x94  click to restore";
            d->AddText({a.x + 16.f, a.y + 31.f}, IM_COL32(210, 195, 165, 220), sub.c_str());
            // a small dismiss "x" on the right
            ImVec2 xc = {b.x - 22.f, a.y + bh * 0.5f};
            bool xhov = ImGui::IsMouseHoveringRect({xc.x - 12, xc.y - 12}, {xc.x + 12, xc.y + 12});
            d->AddText({xc.x - 5.f, xc.y - 8.f}, xhov ? IM_COL32(255,255,255,255) : IM_COL32(200,190,170,200), "\xc3\x97");
            ImGui::PopID();
            if (clk) {
                if (xhov) recovery_clear();
                else { restore_recovery(state); return; }
            }
            yoff = bh + 24.f;
        }
    }

    // Recent grid
    ImGui::SetCursorPos({pad, 148.f + yoff});
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextUnformatted("RECENT");
    ImGui::PopStyleColor();

    std::vector<std::string> recents = recent_projects_list();
    if (recents.empty()) {
        ImGui::SetCursorPos({pad, 180.f + yoff});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
        ImGui::TextWrapped("No recent projects yet. Create one, then Save it (Ctrl+S) — "
                           "it'll show up here next time.");
        ImGui::PopStyleColor();
        return;
    }

    const float grid_top = 176.f + yoff;
    const float card_w = 224.f, card_h = 150.f, gap = 16.f;
    int cols = std::max(1, (int)((W - pad * 2 + gap) / (card_w + gap)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::string to_open;
    for (int i = 0; i < (int)recents.size(); ++i) {
        int r = i / cols, c = i % cols;
        ImVec2 cp = {pad + c * (card_w + gap), grid_top + r * (card_h + gap)};
        ImGui::SetCursorPos(cp);
        ImGui::PushID(i);
        ImGui::InvisibleButton("##proj", {card_w, card_h});
        bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) to_open = recents[i];
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        // Card: thumbnail band (written by the canvas on every save) + footer.
        dl->AddRectFilled(a, b, hov ? IM_COL32(34, 34, 46, 255) : IM_COL32(24, 24, 33, 255), 8.f);
        float foot = b.y - 44.f;
        dl->AddRectFilled(a, {b.x, foot}, IM_COL32(40, 40, 56, 255),
                          8.f, ImDrawFlags_RoundCornersTop);
        int tw = 0, th = 0;
        uintptr_t thumb = video_load_thumb(project_thumb_path(recents[i]), &tw, &th);
        if (thumb && tw > 0 && th > 0) {
            // Cover-fit crop into the band (UVs trimmed on the long axis).
            float bw2 = b.x - a.x, bh2 = foot - a.y;
            float su = 1.f, sv = 1.f;
            float img_asp = (float)tw / (float)th, band_asp = bw2 / bh2;
            if (img_asp > band_asp) su = band_asp / img_asp;
            else                    sv = img_asp / band_asp;
            ImVec2 uv0{0.5f - su * 0.5f, 0.5f - sv * 0.5f};
            ImVec2 uv1{0.5f + su * 0.5f, 0.5f + sv * 0.5f};
            dl->AddImageRounded((ImTextureID)thumb, a, {b.x, foot}, uv0, uv1,
                                IM_COL32(255,255,255,255), 8.f,
                                ImDrawFlags_RoundCornersTop);
        } else {
            // No thumb yet (never saved since this feature landed) — glyph band.
            dl->AddText(g_font_black, 30.f, {a.x + 16.f, a.y + (foot - a.y) * 0.5f - 18.f},
                        IM_COL32(90, 92, 120, 255), "\xe2\x96\xb6");
        }
        dl->AddRect(a, b, hov ? IM_COL32(120, 130, 230, 220) : IM_COL32(50, 50, 68, 200),
                    8.f, 0, hov ? 2.f : 1.f);
        std::string name = fs::path(recents[i]).stem().string();
        ImGui::PushFont(g_font_bold);
        dl->AddText(ImGui::GetFont(), 14.f, {a.x + 12.f, foot + 6.f},
                    IM_COL32(240, 240, 250, 240), name.c_str());
        ImGui::PopFont();
        dl->AddText({a.x + 12.f, foot + 25.f}, IM_COL32(140, 140, 160, 200),
                    mtime_str(recents[i]).c_str());
        ImGui::PopID();
    }
    if (!to_open.empty()) open_project_path(state, to_open);
}
