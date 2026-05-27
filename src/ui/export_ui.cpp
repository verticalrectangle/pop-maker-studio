#include "studio_types.h"
#include "studio_shared.h"
#include "export_ui.h"
#include "app.h"
#include "history.h"
#include "render.h"
#include "blender_export.h"
#include "filepicker.h"
#include "theme.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <filesystem>
#include <cstdio>

namespace fs = std::filesystem;
extern ImFont* g_font_bold;

// ── Right panel: Export tab ───────────────────────────────────────────────────

void draw_export_modal(AppState& state) {
    if (!state.show_export_modal) return;
    ImGui::OpenPopup("##export_modal");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({560.f, 0.f});
    ImGui::PushStyleColor(ImGuiCol_PopupBg, to_u32(Col::bg));
    ImGui::PushStyleColor(ImGuiCol_Border,  to_u32(Col::line));

    if (ImGui::BeginPopupModal("##export_modal", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
        float pw = ImGui::GetContentRegionAvail().x - 8.f;

        ImGui::Dummy({0.f, 12.f});
        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
        ImGui::PushFont(g_font_bold);
        ImGui::TextUnformatted("Export");
        ImGui::PopFont();

        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

        // ── What will be rendered ─────────────────────────────────────────────
        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted("What will be rendered");
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 6.f});

        bool any_active = false;
        for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
            const auto& tr = state.tracks[ti];
            // Count active clips
            int n_clips = 0, n_muted = 0;
            std::string ctype;
            for (auto& cl : tr.clips) {
                ++n_clips;
                if (cl.muted) ++n_muted;
                if (ctype.empty()) {
                    switch (cl.clip_type) {
                        case ClipType::Video:    ctype = "Video";    break;
                        case ClipType::Audio:    ctype = "Audio";    break;
                        case ClipType::Text:     ctype = "Text";     break;
                        case ClipType::Subtitle: ctype = "Subtitle"; break;
                        case ClipType::Lyrics:   ctype = "Lyrics";   break;
                        case ClipType::Effect:   ctype = "Effect";   break;
                        default:                 ctype = "Clip";     break;
                    }
                }
            }
            if (n_clips == 0) continue;

            bool excluded = !tr.visible || tr.muted;
            if (!excluded) any_active = true;

            ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
            if (excluded) ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            else          ImGui::PushStyleColor(ImGuiCol_Text, Col::fg);

            char row[128];
            snprintf(row, sizeof(row), "%s  —  %s  —  %d clip%s",
                tr.name.empty() ? "(unnamed)" : tr.name.c_str(),
                ctype.c_str(), n_clips, n_clips == 1 ? "" : "s");
            ImGui::TextUnformatted(row);
            ImGui::PopStyleColor();

            if (excluded) {
                ImGui::SameLine(0.f, 6.f);
                ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                ImGui::TextUnformatted(!tr.visible ? "[hidden]" : "[muted]");
                ImGui::PopStyleColor();
            } else if (n_muted > 0) {
                ImGui::SameLine(0.f, 6.f);
                ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                char mb[32]; snprintf(mb, sizeof(mb), "[%d clip%s muted]", n_muted, n_muted==1?"":"s");
                ImGui::TextUnformatted(mb);
                ImGui::PopStyleColor();
            }
        }

        if (state.tracks.empty()) {
            ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextUnformatted("No tracks yet.");
            ImGui::PopStyleColor();
        }

        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

        // ── Settings ──────────────────────────────────────────────────────────
        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted("Quality");
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 6.f});

        // 4 quality preset buttons that map to CRF values
        struct QPreset { const char* label; int crf; };
        static constexpr QPreset kQPresets[] = {
            {"Draft", 28}, {"Balanced", 23}, {"Quality", 18}, {"Master", 12}
        };
        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
        for (auto& qp : kQPresets) {
            char id[32]; snprintf(id, sizeof(id), "%s##qm", qp.label);
            if (ui_btn(id, state.render_settings.crf == qp.crf, true))
                state.render_settings.crf = qp.crf;
            ImGui::SameLine(0.f, 4.f);
        }
        ImGui::NewLine();

        ImGui::Dummy({0.f, 8.f});
        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted("Encode speed");
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 6.f});
        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
        if (ui_btn("Fast##spm", state.render_settings.preset == "fast", true))
            state.render_settings.preset = "fast";
        ImGui::SameLine(0.f, 4.f);
        if (ui_btn("Slow##spm", state.render_settings.preset == "slow", true))
            state.render_settings.preset = "slow";
        ImGui::NewLine();

        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

        // ── Progress and action ───────────────────────────────────────────────
        float bar_w = pw - 16.f;
        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
        ImVec2 bp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(bp, {bp.x+bar_w, bp.y+4.f}, to_u32(Col::line), 2.f);
        ImGui::GetWindowDrawList()->AddRectFilled(bp, {bp.x+bar_w*state.render.progress, bp.y+4.f},
                                                   to_u32(Col::fg), 2.f);
        ImGui::Dummy({0.f, 8.f});

        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
        char pct[16]; snprintf(pct, sizeof(pct), "%d%%", (int)(state.render.progress*100.f));
        ImGui::PushFont(g_font_bold); ImGui::SetWindowFontScale(1.4f);
        ImGui::TextUnformatted(pct);
        ImGui::SetWindowFontScale(1.f); ImGui::PopFont();
        ImGui::SameLine(0.f, 8.f);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted(state.render.running ? state.render.stage.c_str() : "Ready");
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 8.f});
        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
        if (state.render.running) {
            if (ui_btn("Cancel render", false, false)) render_cancel();
        } else {
            ImGui::BeginDisabled(!any_active);
            if (ui_btn("Start render  ->", true, false)) {
                state.render_done = false;
                if (!state.audio_path.empty()) {
                    fs::path audio(state.audio_path);
                    fs::path outdir = audio.parent_path() / audio.stem();
                    fs::create_directories(outdir);
                    state.out_mp4 = (outdir / (audio.stem().string() + ".mp4")).string();
                    state.out_srt = (outdir / (audio.stem().string() + ".srt")).string();
                } else if (state.out_mp4.empty()) {
                    // Video-only project — derive output path from project file or home dir.
                    fs::path base;
                    if (!state.project_path.empty()) {
                        fs::path pp(state.project_path);
                        base = pp.parent_path() / pp.stem();
                    } else {
                        base = fs::path(std::getenv("HOME") ? std::getenv("HOME") : ".") / "Videos" / "pop_maker_export";
                    }
                    fs::create_directories(base.parent_path());
                    state.out_mp4 = base.string() + ".mp4";
                }
                render_start_gl(state);
            }
            ImGui::EndDisabled();
        }
        ImGui::SameLine(0.f, 8.f);
        if (!state.render.running) {
            if (ui_btn("Close", false, false)) {
                state.show_export_modal = false;
                ImGui::CloseCurrentPopup();
            }
        }

        // Downloads (shown after render completes)
        if (state.render_done || !state.out_mp4.empty()) {
            ImGui::Dummy({0.f, 8.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
            ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
            struct Dl2 { const char* tag; const char* name; std::string path; bool ok; };
            Dl2 dls[] = {
                {".MP4", "Lyric video", state.out_mp4, state.render_done && !state.out_mp4.empty()},
                {".WAV", "Vocals stem", state.out_wav, !state.out_wav.empty() && fs::exists(state.out_wav)},
                {".SRT", "Subtitles",   state.out_srt, !state.out_srt.empty() && fs::exists(state.out_srt)},
            };
            for (auto& dl : dls) {
                if (!dl.ok) ImGui::BeginDisabled();
                char did[32]; snprintf(did, sizeof(did), "%s %s##dlm", dl.tag, dl.name);
                if (ui_btn(did, false, true)) {
                    std::string cmd = "xdg-open \"" + fs::path(dl.path).parent_path().string() + "\"";
                    system(cmd.c_str());
                }
                if (!dl.ok) ImGui::EndDisabled();
                ImGui::SameLine(0.f, 4.f);
            }
        }

        ImGui::Dummy({0.f, 12.f});
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(2);

    if (!ImGui::IsPopupOpen("##export_modal"))
        state.show_export_modal = false;
}

void panel_export(AppState& state, float w) {
    ImGui::Dummy({0.f, 8.f});

    // Advanced settings (collapsible)
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    bool adv_open = ImGui::TreeNodeEx("Advanced##adv",
        ImGuiTreeNodeFlags_SpanFullWidth |
        (state.render_settings.advanced_open ? ImGuiTreeNodeFlags_DefaultOpen : 0));
    ImGui::PopStyleColor();
    state.render_settings.advanced_open = adv_open;
    if (adv_open) {
        ImGui::Dummy({0.f, 6.f});

        // Quality presets
        ui_label("Quality"); ImGui::Dummy({0.f, 4.f});
        struct QP { const char* lbl; int crf; };
        static constexpr QP kQP[] = {{"Draft",28},{"Balanced",23},{"Quality",18},{"Master",12}};
        for (auto& qp : kQP) {
            if (ui_btn(qp.lbl, state.render_settings.crf == qp.crf, true))
                state.render_settings.crf = qp.crf;
            ImGui::SameLine(0.f, 4.f);
        }
        ImGui::NewLine();

        // Encode speed
        ImGui::Dummy({0.f, 8.f}); ui_label("Encode speed"); ImGui::Dummy({0.f, 4.f});
        if (ui_btn("Fast", state.render_settings.preset == "fast", true))
            state.render_settings.preset = "fast";
        ImGui::SameLine(0.f, 4.f);
        if (ui_btn("Slow", state.render_settings.preset == "slow", true))
            state.render_settings.preset = "slow";
        ImGui::NewLine();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
        ImGui::TextUnformatted("Slow = better compression, same quality.");
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 4.f});
        ImGui::TreePop();
    }

    ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
    ui_label("Render"); ImGui::Dummy({0.f, 6.f});

    // Progress bar
    float bar_w = w - 16.f;
    ImVec2 bp = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(bp, {bp.x+bar_w, bp.y+4.f}, to_u32(Col::line), 2.f);
    ImGui::GetWindowDrawList()->AddRectFilled(bp, {bp.x+bar_w*state.render.progress, bp.y+4.f}, to_u32(Col::fg), 2.f);
    ImGui::Dummy({0.f, 8.f});

    char pct[16]; snprintf(pct, sizeof(pct), "%d%%", (int)(state.render.progress*100.f));
    ImGui::PushFont(g_font_bold); ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted(pct);
    ImGui::SetWindowFontScale(1.f); ImGui::PopFont();

    ImGui::SameLine(0.f, 8.f);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextUnformatted(state.render.running ? state.render.stage.c_str() : "Idle");
    ImGui::PopStyleColor();

    ImGui::Dummy({0.f, 6.f});
    if (state.render.running) {
        if (ui_btn("Cancel", false, true)) render_cancel();
    } else {
        if (ui_btn("Start render  ->", true, true)) {
            state.render_done = false;
            if (!state.audio_path.empty()) {
                fs::path audio(state.audio_path);
                fs::path outdir = audio.parent_path() / audio.stem();
                fs::create_directories(outdir);
                state.out_mp4 = (outdir / (audio.stem().string() + ".mp4")).string();
                state.out_srt = (outdir / (audio.stem().string() + ".srt")).string();
            } else if (state.out_mp4.empty()) {
                fs::path base;
                if (!state.project_path.empty()) {
                    fs::path pp(state.project_path);
                    base = pp.parent_path() / pp.stem();
                } else {
                    base = fs::path(std::getenv("HOME") ? std::getenv("HOME") : ".") / "Videos" / "pop_maker_export";
                }
                fs::create_directories(base.parent_path());
                state.out_mp4 = base.string() + ".mp4";
            }
            render_start_gl(state);
        }
    }

    ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
    ui_label("Downloads"); ImGui::Dummy({0.f, 6.f});

    struct Dl { const char* tag; const char* name; std::string path; bool ok; };
    const char* fmt_res = state.format==OutputFormat::Vertical?"9:16":state.format==OutputFormat::Horizontal?"16:9":"1:1";
    (void)fmt_res;
    Dl dls[] = {
        {".MP4", "Lyric video",  state.out_mp4, state.render_done && !state.out_mp4.empty()},
        {".WAV", "Vocals stem",  state.out_wav, !state.out_wav.empty() && fs::exists(state.out_wav)},
        {".SRT", "Subtitles",    state.out_srt, !state.out_srt.empty() && fs::exists(state.out_srt)},
    };
    for (auto& dl : dls) {
        if (!dl.ok) ImGui::BeginDisabled();
        char did[32]; snprintf(did, sizeof(did), "%s %s##dl", dl.tag, dl.name);
        if (ui_btn(did, false, true)) {
            std::string cmd = "xdg-open \"" + fs::path(dl.path).parent_path().string() + "\"";
            system(cmd.c_str());
        }
        if (!dl.ok) ImGui::EndDisabled();
        ImGui::SameLine(0.f, 4.f);
    }

    ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
    ui_label("Blender"); ImGui::Dummy({0.f, 6.f});
    static std::string blender_status;
    bool has_tracks = !state.tracks.empty();
    if (!has_tracks) ImGui::BeginDisabled();
    if (ui_btn("Export .py  ->", false, true)) {
        std::string sp = state.audio_path.empty() ? "pop_maker_blender.py" :
            (fs::path(state.audio_path).parent_path() /
             (fs::path(state.audio_path).stem().string() + "_blender.py")).string();
        if (blender_export_script(state, sp)) {
            blender_status = sp;
            system(("xdg-open \"" + fs::path(sp).parent_path().string() + "\"").c_str());
        }
    }
    if (!has_tracks) ImGui::EndDisabled();
    if (!blender_status.empty()) {
        ImGui::SameLine(0.f, 8.f);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted(("✓ " + fs::path(blender_status).filename().string()).c_str());
        ImGui::PopStyleColor();
    }
}
