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
    reopen_video_slots(state);
    for (auto& tr : state.tracks)
        for (auto& cl : tr.clips)
            if (cl.clip_type == ClipType::Audio && !cl.text.empty())
                audio_source_ensure(cl.text);
    recent_projects_push(path);
    history_push(state, "Open project");
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

    // Actions
    ImGui::SetCursorPos({pad, 92.f});
    if (ui_btn("+  New Project", true, false)) enter_new_project(state);
    ImGui::SameLine(0.f, 8.f);
    if (ui_btn("Open Project\xe2\x80\xa6", false, false)) {
        std::string picked = filepicker_open("Open project", "PMS Project", "*.pms");
        open_project_path(state, picked);
    }

    // Recent grid
    ImGui::SetCursorPos({pad, 148.f});
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextUnformatted("RECENT");
    ImGui::PopStyleColor();

    std::vector<std::string> recents = recent_projects_list();
    if (recents.empty()) {
        ImGui::SetCursorPos({pad, 180.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
        ImGui::TextWrapped("No recent projects yet. Create one, then Save it (Ctrl+S) — "
                           "it'll show up here next time.");
        ImGui::PopStyleColor();
        return;
    }

    const float grid_top = 176.f;
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
        // Card: a "thumbnail" band (no real thumb yet) + a name/date footer.
        dl->AddRectFilled(a, b, hov ? IM_COL32(34, 34, 46, 255) : IM_COL32(24, 24, 33, 255), 8.f);
        float foot = b.y - 44.f;
        dl->AddRectFilled(a, {b.x, foot}, IM_COL32(40, 40, 56, 255),
                          8.f, ImDrawFlags_RoundCornersTop);
        // Film-strip glyph in the band
        dl->AddText(g_font_black, 30.f, {a.x + 16.f, a.y + (foot - a.y) * 0.5f - 18.f},
                    IM_COL32(90, 92, 120, 255), "\xe2\x96\xb6");
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
