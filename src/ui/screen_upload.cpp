#include "screens.h"
#include "theme.h"
#include "app.h"
#include "transcribe.h"
#include "audio.h"
#include <imgui.h>
#include "json.hpp"
#include <fstream>
#include <filesystem>
#include <cmath>

namespace fs = std::filesystem;

// ── JSON word loading ─────────────────────────────────────────────────────────

static void load_words_json(const std::string& path, AppState& state) {
    std::ifstream f(path);
    if (!f) return;
    try {
        auto j = nlohmann::json::parse(f);
        state.lines.clear();
        LyricLine current_line;
        float last_end = -1.f;

        for (auto& w : j) {
            Word word;
            word.text  = w["word"].get<std::string>();
            word.start = w["start"].get<float>();
            word.end   = w["end"].get<float>();

            // New line if gap > 0.8s
            if (last_end >= 0.f && word.start - last_end > 0.8f && !current_line.words.empty()) {
                state.lines.push_back(current_line);
                current_line = LyricLine{};
            }
            current_line.words.push_back(word);
            last_end = word.end;
        }
        if (!current_line.words.empty())
            state.lines.push_back(current_line);

        // Count total words for the panel header
        int total = 0;
        for (auto& l : state.lines) total += (int)l.words.size();
        (void)total;
    } catch (...) {}
}

// ── Dropzone drag-and-drop ────────────────────────────────────────────────────

static bool is_audio_file(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    return ext == ".wav" || ext == ".mp3" || ext == ".m4a" ||
           ext == ".flac" || ext == ".mp4" || ext == ".mov" || ext == ".aac";
}

// ── Screen ────────────────────────────────────────────────────────────────────

static std::string g_drop_hover_path;

void ui_screen_upload(AppState& state) {
    ImGui::SetNextWindowPos(ImGui::GetCursorScreenPos());
    ImGui::SetNextWindowSize(ImGui::GetContentRegionAvail());
    ImGui::BeginChild("##upload_body", {0, 0});

    float win_w  = ImGui::GetContentRegionAvail().x;
    float pad_x  = fmaxf((win_w - 960.f) * 0.5f, 32.f);

    ImGui::Dummy({0.f, 48.f});

    // ── Section header ────────────────────────────────────────────────────────
    ImGui::SetCursorPosX(pad_x);
    ImGui::BeginGroup();

    extern ImFont* g_font_bold;
    ImGui::PushFont(g_font_bold);
    ImGui::SetWindowFontScale(2.6f);
    ImGui::TextUnformatted("Drop the track.");
    ImGui::SetWindowFontScale(1.f);
    ImGui::PopFont();

    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextUnformatted("WAV, MP3, M4A, or a video file. Up to 12 minutes.\nYour file never leaves this machine.");
    ImGui::PopStyleColor();
    ImGui::EndGroup();

    // Section index — top right
    ImVec2 idx_sz = ImGui::CalcTextSize("02 / 05  —  Upload");
    ImGui::SameLine(win_w - pad_x - idx_sz.x);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextUnformatted("02 / 05  —  Upload");
    ImGui::PopStyleColor();

    ImGui::Dummy({0.f, 12.f});
    ImGui::SetCursorPosX(pad_x);
    ui_separator();
    ImGui::Dummy({0.f, 24.f});

    // ── Dropzone ──────────────────────────────────────────────────────────────
    float dz_w = win_w - pad_x * 2.f;
    float dz_h = 180.f;
    ImGui::SetCursorPosX(pad_x);

    bool file_pending = !state.audio_path.empty() &&
                        state.pipeline.stage != PipelineStage::Idle;
    bool is_dragging  = !g_drop_hover_path.empty();

    ImGui::PushStyleColor(ImGuiCol_ChildBg,
        is_dragging ? Col::bg_soft_hov : Col::bg_soft);
    ImGui::PushStyleColor(ImGuiCol_Border,
        is_dragging ? Col::line_hover : Col::line);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.f);

    if (ImGui::BeginChild("##dz", {dz_w, dz_h}, ImGuiChildFlags_Borders)) {
        float cx = ImGui::GetContentRegionAvail().x * 0.5f;

        ImGui::Dummy({0.f, 24.f});
        ImGui::SetCursorPosX(cx - 24.f);

        // Icon box
        ImVec2 icon_p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRect(icon_p, {icon_p.x + 48.f, icon_p.y + 48.f},
            to_u32(Col::line), 2.f);
        ImGui::GetWindowDrawList()->AddText(
            {icon_p.x + 16.f, icon_p.y + 14.f}, to_u32(Col::fg), "v");
        ImGui::Dummy({0.f, 56.f});

        if (state.audio_path.empty()) {
            std::string title = is_dragging ? g_drop_hover_path : "Drag a file here";
            ImVec2 tsz = ImGui::CalcTextSize(title.c_str());
            ImGui::SetCursorPosX((dz_w - tsz.x) * 0.5f);
            ImGui::PushFont(g_font_bold);
            ImGui::SetWindowFontScale(1.6f);
            ImGui::TextUnformatted(title.c_str());
            ImGui::SetWindowFontScale(1.f);
            ImGui::PopFont();

            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImVec2 ssz = ImGui::CalcTextSize("Or click to browse  ·  Audio & video accepted");
            ImGui::SetCursorPosX((dz_w - ssz.x) * 0.5f);
            ImGui::TextUnformatted("Or click to browse  ·  Audio & video accepted");
            ImGui::PopStyleColor();
        } else {
            std::string name = fs::path(state.audio_path).filename().string();
            ImVec2 tsz = ImGui::CalcTextSize(name.c_str());
            ImGui::SetCursorPosX((dz_w - tsz.x) * 0.5f);
            ImGui::PushFont(g_font_bold);
            ImGui::SetWindowFontScale(1.4f);
            ImGui::TextUnformatted(name.c_str());
            ImGui::SetWindowFontScale(1.f);
            ImGui::PopFont();
        }

        // Format chips
        ImGui::Dummy({0.f, 12.f});
        const char* fmts[] = {".wav", ".mp3", ".m4a", ".flac", ".mp4", ".mov"};
        float total_chips = 0.f;
        for (auto& fmt : fmts) total_chips += ImGui::CalcTextSize(fmt).x + 20.f;
        ImGui::SetCursorPosX((dz_w - total_chips) * 0.5f);
        for (int i = 0; i < 6; ++i) {
            if (i) ImGui::SameLine(0.f, 6.f);
            ImVec2 cp = ImGui::GetCursorScreenPos();
            float cw  = ImGui::CalcTextSize(fmts[i]).x + 16.f;
            ImGui::GetWindowDrawList()->AddRect(cp, {cp.x + cw, cp.y + 20.f},
                to_u32(Col::line), 2.f);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::label);
            ImGui::TextUnformatted(fmts[i]);
            ImGui::PopStyleColor();
            ImGui::SameLine(0.f, 0.f);
            ImGui::Dummy({8.f, 0.f});
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();

    // Click dropzone to open file dialog (imgui-native: handled via GLFW/OS drag)
    if (ImGui::IsItemClicked() && state.audio_path.empty()) {
        // Native file dialog would go here — for now type in path
    }

    // OS drag-and-drop handled in main.cpp via GLFW drop callback

    // ── Pipeline steps ────────────────────────────────────────────────────────
    ImGui::Dummy({0.f, 24.f});
    ImGui::SetCursorPosX(pad_x);

    struct Step { const char* num; const char* name; const char* desc; PipelineStage stage; };
    Step steps[] = {
        {"Stage 01", "Extract",    "Pull stems — isolate vocals from instrumental", PipelineStage::Extract},
        {"Stage 02", "Transcribe", "Word-level alignment — Whisper-V3 + forced",   PipelineStage::Transcribe},
        {"Stage 03", "Ready",      "Hand off to lyrics editor & preview",           PipelineStage::Align},
    };

    float step_w = (win_w - pad_x * 2.f) / 3.f;

    for (int i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine(0.f, 0.f);
        bool done   = (int)state.pipeline.stage > (int)steps[i].stage;
        bool active = state.pipeline.stage == steps[i].stage;

        ImGui::SetCursorPosX(pad_x + i * step_w);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Col::bg_soft);
        ImGui::PushStyleColor(ImGuiCol_Border,  active ? Col::fg : Col::line);
        if (ImGui::BeginChild(steps[i].num, {step_w - 1.f, 110.f}, ImGuiChildFlags_Borders)) {
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted(steps[i].num);
            ImGui::PopStyleColor();
            ImGui::SameLine(step_w - 40.f);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextUnformatted("->");
            ImGui::PopStyleColor();

            ImGui::PushFont(g_font_bold);
            ImGui::SetWindowFontScale(1.4f);
            ImGui::PushStyleColor(ImGuiCol_Text, (active || done) ? Col::fg : Col::muted);
            ImGui::TextUnformatted(steps[i].name);
            ImGui::PopStyleColor();
            ImGui::SetWindowFontScale(1.f);
            ImGui::PopFont();

            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted(steps[i].desc);
            ImGui::PopStyleColor();

            // State label
            const char* state_str = done   ? "Done  ✓" :
                                    active ? state.pipeline.message.c_str() :
                                             "—";
            ImGui::PushStyleColor(ImGuiCol_Text, (active || done) ? Col::fg : Col::muted);
            ImGui::TextUnformatted(state_str);
            ImGui::PopStyleColor();

            // Progress bar at bottom
            if (active) {
                ImVec2 bar_p = ImGui::GetWindowPos();
                bar_p.y += 109.f;
                ImGui::GetWindowDrawList()->AddRectFilled(
                    bar_p, {bar_p.x + step_w * state.pipeline.progress, bar_p.y + 1.f},
                    to_u32(Col::fg));
            } else if (done) {
                ImVec2 bar_p = ImGui::GetWindowPos();
                bar_p.y += 109.f;
                ImGui::GetWindowDrawList()->AddRectFilled(
                    bar_p, {bar_p.x + step_w, bar_p.y + 1.f}, to_u32(Col::fg));
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
    }

    // ── Bottom actions ────────────────────────────────────────────────────────
    ImGui::Dummy({0.f, 24.f});
    ImGui::SetCursorPosX(pad_x);

    // File meta
    if (!state.audio_path.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::label);
        ImGui::TextUnformatted(fs::path(state.audio_path).filename().string().c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::label);
        ImGui::TextUnformatted("No file selected");
        ImGui::PopStyleColor();
    }

    float right_x = win_w - pad_x - 280.f;
    ImGui::SameLine(right_x);
    if (ui_btn("Cancel")) {
        transcribe_cancel();
        state.audio_path.clear();
        state.pipeline = PipelineStatus{};
        state.lines.clear();
    }
    ImGui::SameLine(0.f, 8.f);

    bool ready = state.pipeline.stage == PipelineStage::Done;
    if (!ready) ImGui::BeginDisabled();
    if (ui_btn("Continue to lyrics  ->", true)) {
        // Load JSON into lines
        load_words_json(state.words_json_path, state);
        state.duration = audio_duration();
        state.go(Screen::Editor);
    }
    if (!ready) ImGui::EndDisabled();

    // ── If a file was dropped via GLFW, kick off pipeline ────────────────────
    // Check flag set by main.cpp drop callback
    extern std::string g_dropped_file;
    if (!g_dropped_file.empty() && is_audio_file(g_dropped_file)) {
        state.audio_path = g_dropped_file;
        state.pipeline   = PipelineStatus{};
        g_dropped_file.clear();

        // Determine output paths
        fs::path audio(state.audio_path);
        fs::path outdir = audio.parent_path() / audio.stem();
        state.words_json_path = (outdir / (audio.stem().string() + ".json")).string();
        state.vocals_path     = (outdir / "vocals.wav").string();
        state.out_srt         = (outdir / (audio.stem().string() + ".srt")).string();
        state.out_wav         = state.vocals_path;

        // Find pipeline script next to executable
        extern std::string g_pipeline_script;
        transcribe_start(
            state.audio_path,
            state.python_path,
            g_pipeline_script,
            state.pipeline,
            state.words_json_path,
            state.vocals_path
        );
        audio_load(state.audio_path);
        state.duration = audio_duration();
    }

    ImGui::EndChild();
}
