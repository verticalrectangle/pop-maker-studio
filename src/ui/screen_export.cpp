#include "screens.h"
#include "theme.h"
#include "app.h"
#include "render.h"
#include "blender_export.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

void ui_screen_export(AppState& state) {
    ImGui::SetNextWindowPos(ImGui::GetCursorScreenPos());
    ImGui::SetNextWindowSize(ImGui::GetContentRegionAvail());
    ImGui::BeginChild("##export_body", {0, 0});

    float win_w = ImGui::GetContentRegionAvail().x;
    float pad_x = fmaxf((win_w - 960.f) * 0.5f, 32.f);
    extern ImFont* g_font_bold;

    ImGui::Dummy({0.f, 48.f});

    // ── Header ────────────────────────────────────────────────────────────────
    ImGui::SetCursorPosX(pad_x);
    ImGui::PushFont(g_font_bold);
    ImGui::SetWindowFontScale(2.2f);
    ImGui::TextUnformatted("Render & export.");
    ImGui::SetWindowFontScale(1.f);
    ImGui::PopFont();
    ImGui::SameLine(win_w - pad_x - ImGui::CalcTextSize("05 / 05  —  Export").x);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextUnformatted("05 / 05  —  Export");
    ImGui::PopStyleColor();

    ImGui::SetCursorPosX(pad_x);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextUnformatted("Pick the format. Hit render. Download lyric video, vocal stem, and SRT in one pass.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 8.f});
    ImGui::SetCursorPosX(pad_x);
    ui_separator();
    ImGui::Dummy({0.f, 24.f});

    // ── Format picker ─────────────────────────────────────────────────────────
    ImGui::SetCursorPosX(pad_x);
    ui_label("Output format");
    ImGui::Dummy({0.f, 8.f});

    struct Format {
        OutputFormat fmt;
        const char*  name;
        const char*  ratio;
        const char*  res;
        float        shape_w;  // relative width of preview rect
        float        shape_h;
    };
    Format fmts[] = {
        {OutputFormat::Vertical,   "TikTok / Reels", "9:16",  "1080x1920  30 fps",  36.f, 64.f},
        {OutputFormat::Horizontal, "YouTube",        "16:9",  "1920x1080  30 fps",  80.f, 45.f},
        {OutputFormat::Square,     "Square",         "1:1",   "1080x1080  30 fps",  56.f, 56.f},
    };

    float fmt_w = (win_w - pad_x * 2.f - 32.f) / 3.f;

    for (int i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine(0.f, 16.f);
        ImGui::SetCursorPosX(pad_x + i * (fmt_w + 16.f));
        bool sel = state.format == fmts[i].fmt;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, sel ? Col::bg_soft_hov : Col::bg_soft);
        ImGui::PushStyleColor(ImGuiCol_Border,  sel ? Col::fg : Col::line);
        char fid[16]; snprintf(fid, sizeof(fid), "##fmt%d", i);
        if (ImGui::BeginChild(fid, {fmt_w, 130.f}, ImGuiChildFlags_Borders)) {
            // Shape preview
            float cx  = (fmt_w - fmts[i].shape_w) * 0.5f;
            ImVec2 sp = ImGui::GetCursorScreenPos();
            sp.x += cx;
            ImGui::GetWindowDrawList()->AddRect(
                sp, {sp.x + fmts[i].shape_w, sp.y + fmts[i].shape_h},
                to_u32(sel ? Col::fg : Col::line), 2.f);
            // Ratio label inside shape
            ImVec2 rsz = ImGui::CalcTextSize(fmts[i].ratio);
            ImGui::GetWindowDrawList()->AddText(
                {sp.x + (fmts[i].shape_w - rsz.x) * 0.5f,
                 sp.y + (fmts[i].shape_h - rsz.y) * 0.5f},
                to_u32(sel ? Col::fg : Col::muted), fmts[i].ratio);

            ImGui::Dummy({0.f, fmts[i].shape_h + 12.f});
            ImGui::SetCursorPosX(8.f);
            ImGui::PushFont(g_font_bold);
            ImGui::SetWindowFontScale(1.1f);
            ImGui::PushStyleColor(ImGuiCol_Text, sel ? Col::fg : Col::muted);
            ImGui::TextUnformatted(fmts[i].name);
            ImGui::PopStyleColor();
            ImGui::SetWindowFontScale(1.f);
            ImGui::PopFont();

            ImGui::SetCursorPosX(8.f);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::label);
            ImGui::TextUnformatted(fmts[i].res);
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        if (ImGui::IsItemClicked())
            state.format = fmts[i].fmt;
        ImGui::PopStyleColor(2);
    }

    // ── Render status ─────────────────────────────────────────────────────────
    ImGui::Dummy({0.f, 32.f});
    ImGui::SetCursorPosX(pad_x);
    ui_separator();
    ImGui::Dummy({0.f, 16.f});
    ImGui::SetCursorPosX(pad_x);
    ui_label("Render status");
    ImGui::Dummy({0.f, 8.f});

    // Percentage + ETA
    ImGui::SetCursorPosX(pad_x);
    ImGui::PushFont(g_font_bold);
    ImGui::SetWindowFontScale(3.f);
    char pct_str[16];
    snprintf(pct_str, sizeof(pct_str), "%d%%", (int)(state.render.progress * 100.f));
    ImGui::TextUnformatted(pct_str);
    ImGui::SetWindowFontScale(1.f);
    ImGui::PopFont();

    ImGui::SameLine(pad_x + 120.f);
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextUnformatted("Time remaining");
    ImGui::PopStyleColor();
    if (state.render.running) {
        int eta = (int)state.render.eta_secs;
        char eta_str[16];
        snprintf(eta_str, sizeof(eta_str), "%d:%02d", eta / 60, eta % 60);
        ImGui::PushFont(g_font_bold);
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextUnformatted(eta_str);
        ImGui::SetWindowFontScale(1.f);
        ImGui::PopFont();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted("—:—");
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();

    // Progress bar
    ImGui::Dummy({0.f, 8.f});
    ImGui::SetCursorPosX(pad_x);
    float bar_w = win_w - pad_x * 2.f;
    ImVec2 bar_p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(
        bar_p, {bar_p.x + bar_w, bar_p.y + 4.f}, to_u32(Col::line), 2.f);
    ImGui::GetWindowDrawList()->AddRectFilled(
        bar_p, {bar_p.x + bar_w * state.render.progress, bar_p.y + 4.f},
        to_u32(Col::fg), 2.f);
    ImGui::Dummy({0.f, 12.f});

    // Stage + frame
    ImGui::SetCursorPosX(pad_x);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextUnformatted(state.render.running ? state.render.stage.c_str() : "Idle — press render");
    ImGui::SameLine(win_w - pad_x - 120.f);
    char frame_str[32];
    snprintf(frame_str, sizeof(frame_str), "%d / %d frames",
        state.render.frame, state.render.total_frames);
    ImGui::TextUnformatted(frame_str);
    ImGui::PopStyleColor();

    // Render / cancel buttons
    ImGui::Dummy({0.f, 12.f});
    ImGui::SetCursorPosX(pad_x);
    if (state.render.running) {
        if (ui_btn("Cancel")) render_cancel();
    } else {
        if (ui_btn("Start render  ->", true)) {
            state.render_done = false;
            // Set output paths
            if (!state.audio_path.empty()) {
                fs::path audio(state.audio_path);
                fs::path outdir = audio.parent_path() / audio.stem();
                fs::create_directories(outdir);
                state.out_mp4 = (outdir / (audio.stem().string() + ".mp4")).string();
                state.out_wav = (outdir / "vocals.wav").string();
                state.out_srt = (outdir / (audio.stem().string() + ".srt")).string();
            }
            render_start(state);
        }
    }

    // ── Downloads ─────────────────────────────────────────────────────────────
    ImGui::Dummy({0.f, 32.f});
    ImGui::SetCursorPosX(pad_x);
    ui_separator();
    ImGui::Dummy({0.f, 16.f});
    ImGui::SetCursorPosX(pad_x);
    ui_label("Downloads");
    ImGui::Dummy({0.f, 8.f});

    struct Download {
        const char* tag;
        const char* name;
        std::string meta;
        std::string path;
        bool        available;
    };

    // Format-specific MP4 label
    const char* fmt_res = state.format == OutputFormat::Vertical   ? "9:16  1080x1920" :
                          state.format == OutputFormat::Horizontal  ? "16:9  1920x1080" :
                                                                       "1:1  1080x1080";

    Download downloads[] = {
        {".MP4", "Lyric video",  std::string(fmt_res) + "  H.264",
            state.out_mp4, state.render_done && !state.out_mp4.empty()},
        {".WAV", "Vocals only",  "Stem  isolated  96kHz 24-bit",
            state.out_wav, !state.out_wav.empty() && fs::exists(state.out_wav)},
        {".SRT", "Subtitles",    "Word-level timing  UTF-8",
            state.out_srt, !state.out_srt.empty() && fs::exists(state.out_srt)},
    };

    for (auto& dl : downloads) {
        ImGui::SetCursorPosX(pad_x);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Col::bg_soft);
        ImGui::PushStyleColor(ImGuiCol_Border,  dl.available ? Col::line : Col::line);
        if (ImGui::BeginChild(dl.tag, {win_w - pad_x * 2.f, 52.f}, ImGuiChildFlags_Borders)) {
            // Tag badge
            ImVec2 tp = ImGui::GetCursorScreenPos();
            float tag_w = 52.f;
            ImGui::GetWindowDrawList()->AddRectFilled(
                tp, {tp.x + tag_w, tp.y + 36.f}, to_u32(Col::line), 2.f);
            ImGui::GetWindowDrawList()->AddText(
                {tp.x + 6.f, tp.y + 10.f}, to_u32(Col::fg), dl.tag);

            ImGui::Dummy({tag_w + 8.f, 0.f});
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::PushFont(g_font_bold);
            ImGui::TextUnformatted(dl.name);
            ImGui::PopFont();
            ImGui::SameLine(0.f, 16.f);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::label);
            ImGui::TextUnformatted(dl.meta.c_str());
            ImGui::PopStyleColor();
            ImGui::EndGroup();

            // Download button
            float btn_x = win_w - pad_x * 2.f - 120.f;
            ImGui::SameLine(btn_x);
            if (!dl.available) ImGui::BeginDisabled();
            char dl_id[32]; snprintf(dl_id, sizeof(dl_id), "Download  ->##dl_%s", dl.tag);
            if (ui_btn(dl_id)) {
                // On Linux: open containing folder via xdg-open
                std::string cmd = "xdg-open \"" + fs::path(dl.path).parent_path().string() + "\"";
                system(cmd.c_str());
            }
            if (!dl.available) ImGui::EndDisabled();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::Dummy({0.f, 4.f});
    }

    // ── Blender export ────────────────────────────────────────────────────────
    ImGui::Dummy({0.f, 24.f});
    ImGui::SetCursorPosX(pad_x);
    ui_separator();
    ImGui::Dummy({0.f, 16.f});
    ImGui::SetCursorPosX(pad_x);
    ui_label("Blender export");
    ImGui::Dummy({0.f, 8.f});

    ImGui::SetCursorPosX(pad_x);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col::bg_soft);
    ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
    if (ImGui::BeginChild("##blender_row", {win_w - pad_x * 2.f, 52.f}, ImGuiChildFlags_Borders)) {
        // Badge
        ImVec2 bp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(bp, {bp.x + 52.f, bp.y + 36.f},
            to_u32(Col::line), 2.f);
        ImGui::GetWindowDrawList()->AddText({bp.x + 4.f, bp.y + 10.f},
            to_u32(Col::fg), ".PY");

        ImGui::Dummy({60.f, 0.f});
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushFont(g_font_bold);
        ImGui::TextUnformatted("Blender scene script");
        ImGui::PopFont();
        ImGui::SameLine(0.f, 16.f);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::label);
        ImGui::TextUnformatted("lyric-video-blender  ·  Run in Blender Text Editor");
        ImGui::PopStyleColor();
        ImGui::EndGroup();

        float btn_x = win_w - pad_x * 2.f - 160.f;
        ImGui::SameLine(btn_x);

        static std::string blender_status;
        bool has_lyrics = !state.tracks.empty();
        if (!has_lyrics) ImGui::BeginDisabled();
        if (ui_btn("Export script  ->")) {
            // Write next to the audio file, or in current dir
            std::string script_path;
            if (!state.audio_path.empty()) {
                fs::path audio(state.audio_path);
                script_path = (audio.parent_path() /
                    (audio.stem().string() + "_blender.py")).string();
            } else {
                script_path = "pop_maker_blender.py";
            }
            if (blender_export_script(state, script_path)) {
                blender_status = script_path;
                std::string cmd = "xdg-open \"" +
                    fs::path(script_path).parent_path().string() + "\"";
                system(cmd.c_str());
            }
        }
        if (!has_lyrics) ImGui::EndDisabled();

        if (!blender_status.empty()) {
            ImGui::SameLine(0.f, 12.f);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted(("Saved: " + fs::path(blender_status).filename().string()).c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);

    ImGui::EndChild();
}
