#include "screens.h"
#include "theme.h"
#include "app.h"
#include "audio.h"
#include <imgui.h>
#include <cmath>
#include <string>
#include <cstdio>

static std::string fmt_time(float s) {
    int m   = (int)(s / 60);
    int sec = (int)s % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d:%02d", m, sec);
    return buf;
}

void ui_screen_editor(AppState& state) {
    ImGui::SetNextWindowPos(ImGui::GetCursorScreenPos());
    ImGui::SetNextWindowSize(ImGui::GetContentRegionAvail());
    ImGui::BeginChild("##editor_body", {0, 0});

    float win_w = ImGui::GetContentRegionAvail().x;
    float pad_x = fmaxf((win_w - 1160.f) * 0.5f, 24.f);
    extern ImFont* g_font_bold;

    ImGui::Dummy({0.f, 32.f});

    // ── Section header ────────────────────────────────────────────────────────
    ImGui::SetCursorPosX(pad_x);
    ImGui::PushFont(g_font_bold);
    ImGui::SetWindowFontScale(2.2f);
    ImGui::TextUnformatted("Lyrics editor.");
    ImGui::SetWindowFontScale(1.f);
    ImGui::PopFont();
    ImGui::SameLine(win_w - pad_x - ImGui::CalcTextSize("03 / 05  —  Editor").x);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextUnformatted("03 / 05  —  Editor");
    ImGui::PopStyleColor();

    ImGui::SetCursorPosX(pad_x);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextUnformatted("Click a word to seek. Double-click to retype.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 8.f});
    ImGui::SetCursorPosX(pad_x);
    ui_separator();
    ImGui::Dummy({0.f, 16.f});

    // ── Toolbar ───────────────────────────────────────────────────────────────
    ImGui::SetCursorPosX(pad_x);
    if (ui_btn("+ Line", false, true)) {
        LyricLine blank;
        Word w; w.text = "Word"; w.start = 0.f; w.end = 0.5f;
        blank.words.push_back(w);
        state.lines.push_back(blank);
    }
    ImGui::SameLine(0.f, 8.f);
    if (ui_btn("Re-align", false, true)) { /* re-run alignment — TODO */ }
    ImGui::SameLine(win_w - pad_x - 300.f);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    if (!state.audio_path.empty())
        ImGui::TextUnformatted(state.audio_path.c_str());
    else
        ImGui::TextUnformatted("No file loaded");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 8.f});

    // ── Two-column grid ───────────────────────────────────────────────────────
    float col_gap = 16.f;
    float col_w   = (win_w - pad_x * 2.f - col_gap) * 0.5f;
    float grid_h  = ImGui::GetContentRegionAvail().y - 8.f;

    // LEFT — lyric list
    ImGui::SetCursorPosX(pad_x);

    // Count words
    int total_words = 0;
    for (auto& l : state.lines) total_words += (int)l.words.size();
    char wcount[32];
    snprintf(wcount, sizeof(wcount), "%d words", total_words);

    ImGui::PushStyleColor(ImGuiCol_ChildBg,   Col::bg_soft);
    ImGui::PushStyleColor(ImGuiCol_Border,    Col::line);
    if (ImGui::BeginChild("##lyric_panel", {col_w, grid_h}, ImGuiChildFlags_Borders)) {
        // Panel header
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Col::transparent);
        if (ImGui::BeginChild("##lp_head", {0.f, 30.f}, ImGuiChildFlags_Borders)) {
            ImGui::TextUnformatted("Lyrics — Word Timestamps");
            ImGui::SameLine(col_w - ImGui::CalcTextSize(wcount).x - 16.f);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted(wcount);
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // Lyric lines
        ImGui::SetNextWindowContentSize({col_w - 12.f, 0.f});
        ImGui::BeginChild("##lyric_scroll", {0.f, 0.f}, ImGuiChildFlags_None,
            ImGuiWindowFlags_HorizontalScrollbar);

        static int editing_line = -1;
        static int editing_word = -1;
        static char edit_buf[128] = {};

        for (int li = 0; li < (int)state.lines.size(); ++li) {
            auto& line = state.lines[li];
            bool line_active = (li == state.active_line);

            char card_id[32]; snprintf(card_id, sizeof(card_id), "##ll%d", li);
            ImGui::PushStyleColor(ImGuiCol_ChildBg,
                line_active ? Col::bg_soft_hov : Col::bg_soft);
            ImGui::PushStyleColor(ImGuiCol_Border,
                line_active ? Col::fg : Col::line);

            float line_h = 64.f;
            if (ImGui::BeginChild(card_id, {col_w - 20.f, line_h}, ImGuiChildFlags_Borders)) {
                // Line header
                char lhead[64];
                snprintf(lhead, sizeof(lhead), "Line %02d   %s -> %s",
                    li + 1,
                    fmt_time(line.start_time()).c_str(),
                    fmt_time(line.end_time()).c_str());
                ImGui::PushStyleColor(ImGuiCol_Text, line_active ? Col::fg : Col::muted);
                ImGui::TextUnformatted(lhead);
                ImGui::PopStyleColor();

                // Words
                for (int wi = 0; wi < (int)line.words.size(); ++wi) {
                    if (wi) ImGui::SameLine(0.f, 4.f);
                    auto& word = line.words[wi];
                    bool  word_active = (li == state.active_line && wi == state.active_word);

                    if (editing_line == li && editing_word == wi) {
                        ImGui::SetNextItemWidth(
                            fmaxf(60.f, ImGui::CalcTextSize(word.text.c_str()).x + 16.f));
                        if (ImGui::InputText("##edit", edit_buf, sizeof(edit_buf),
                                ImGuiInputTextFlags_EnterReturnsTrue)) {
                            word.text    = edit_buf;
                            editing_line = -1;
                            editing_word = -1;
                        }
                        if (ImGui::IsItemDeactivated()) {
                            word.text    = edit_buf;
                            editing_line = -1;
                            editing_word = -1;
                        }
                    } else {
                        if (word_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button,        Col::fg);
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col::fg);
                            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Col::fg);
                            ImGui::PushStyleColor(ImGuiCol_Text,          Col::bg);
                        } else {
                            ImGui::PushStyleColor(ImGuiCol_Button,        Col::transparent);
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col::bg_soft_hov);
                            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Col::bg_soft_hov);
                            ImGui::PushStyleColor(ImGuiCol_Text,          Col::fg);
                        }
                        char wid[32];
                        snprintf(wid, sizeof(wid), "%s##w%d_%d", word.text.c_str(), li, wi);
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4.f, 2.f});
                        if (ImGui::Button(wid)) {
                            // single click = seek
                            state.playhead    = word.start;
                            state.active_line = li;
                            state.active_word = wi;
                            audio_seek(word.start);
                        }
                        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                            // double click = edit
                            editing_line = li;
                            editing_word = wi;
                            strncpy(edit_buf, word.text.c_str(), sizeof(edit_buf) - 1);
                            ImGui::SetKeyboardFocusHere(-1);
                        }
                        ImGui::PopStyleVar();
                        ImGui::PopStyleColor(4);
                    }
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor(2);
            ImGui::Dummy({0.f, 2.f});
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);

    // RIGHT — preview + transport
    ImGui::SameLine(0.f, col_gap);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col::bg_soft);
    ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
    if (ImGui::BeginChild("##preview_panel", {col_w, grid_h}, ImGuiChildFlags_Borders)) {
        // Panel header
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Col::transparent);
        if (ImGui::BeginChild("##pp_head", {0.f, 30.f}, ImGuiChildFlags_Borders)) {
            ImGui::TextUnformatted("Live preview");
            ImGui::SameLine(col_w - 200.f);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            const char* style_names[] = {"Fade","Glitch","Typewriter","Bounce",
                                         "Scale","Slide","Stack","Block"};
            char stag[64];
            snprintf(stag, sizeof(stag), "Style — %s", style_names[(int)state.style]);
            ImGui::TextUnformatted(stag);
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // Preview stage — 16:9
        float stage_w = col_w - 24.f;
        float stage_h = stage_w * 9.f / 16.f;
        ImGui::Dummy({0.f, 8.f});
        ImGui::SetCursorPosX(12.f);

        ImVec2 stage_p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Background
        dl->AddRectFilled(stage_p, {stage_p.x + stage_w, stage_p.y + stage_h},
            to_u32(Col::accent_dark), 2.f);
        dl->AddRect(stage_p, {stage_p.x + stage_w, stage_p.y + stage_h},
            to_u32(Col::line), 2.f);

        // Corner marks
        float cm = 12.f;
        ImU32 cc = to_u32(Col::muted);
        dl->AddLine(stage_p, {stage_p.x + cm, stage_p.y}, cc);
        dl->AddLine(stage_p, {stage_p.x, stage_p.y + cm}, cc);
        dl->AddLine({stage_p.x + stage_w, stage_p.y},
                    {stage_p.x + stage_w - cm, stage_p.y}, cc);
        dl->AddLine({stage_p.x + stage_w, stage_p.y},
                    {stage_p.x + stage_w, stage_p.y + cm}, cc);
        dl->AddLine({stage_p.x, stage_p.y + stage_h},
                    {stage_p.x + cm, stage_p.y + stage_h}, cc);
        dl->AddLine({stage_p.x, stage_p.y + stage_h},
                    {stage_p.x, stage_p.y + stage_h - cm}, cc);
        dl->AddLine({stage_p.x + stage_w, stage_p.y + stage_h},
                    {stage_p.x + stage_w - cm, stage_p.y + stage_h}, cc);
        dl->AddLine({stage_p.x + stage_w, stage_p.y + stage_h},
                    {stage_p.x + stage_w, stage_p.y + stage_h - cm}, cc);

        // Stage label
        dl->AddText({stage_p.x + 8.f, stage_p.y + 6.f},
            to_u32(Col::muted), "Preview  1080p");
        dl->AddText({stage_p.x + stage_w - 60.f, stage_p.y + 6.f},
            to_u32(Col::fg), "● Live");

        // Current lyric text
        std::string now_text  = "DROP";
        std::string next_text = "A FILE TO BEGIN";
        if (!state.lines.empty()) {
            if (state.active_line >= 0 && state.active_line < (int)state.lines.size()) {
                now_text = state.lines[state.active_line].full_text();
                int next_li = state.active_line + 1;
                if (next_li < (int)state.lines.size())
                    next_text = "Next — " + state.lines[next_li].full_text();
                else
                    next_text = "";
            } else {
                now_text  = state.lines[0].full_text();
                next_text = state.lines.size() > 1 ?
                    "Next — " + state.lines[1].full_text() : "";
            }
        }

        ImGui::PushFont(g_font_bold);
        ImGui::SetWindowFontScale(1.8f);
        ImVec2 tsz = ImGui::CalcTextSize(now_text.c_str());
        ImVec2 tpos = {
            stage_p.x + (stage_w - tsz.x) * 0.5f,
            stage_p.y + (stage_h - tsz.y) * 0.5f - 12.f
        };
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.8f,
            tpos, to_u32(Col::fg), now_text.c_str());
        ImGui::SetWindowFontScale(1.f);
        ImGui::PopFont();

        if (!next_text.empty()) {
            ImVec2 nsz  = ImGui::CalcTextSize(next_text.c_str());
            ImVec2 npos = {stage_p.x + (stage_w - nsz.x) * 0.5f,
                           tpos.y + tsz.y + 8.f};
            dl->AddText(npos, to_u32(Col::muted), next_text.c_str());
        }

        ImGui::Dummy({stage_w, stage_h + 8.f});

        // ── Transport ─────────────────────────────────────────────────────────
        ImGui::SetCursorPosX(8.f);
        float dur  = fmaxf(state.duration, 0.01f);
        float fill = state.playhead / dur;

        // Play/pause button
        if (ui_btn(state.playing ? "||" : ">", false, false)) {
            state.playing = !state.playing;
            if (state.playing) audio_play();
            else               audio_pause();
        }
        ImGui::SameLine(0.f, 8.f);

        // Scrub bar
        float sb_w = col_w - 120.f;
        ImVec2 sb_p = ImGui::GetCursorScreenPos();
        float  sb_h = 28.f;
        ImGui::PushStyleColor(ImGuiCol_FrameBg,       Col::bg_soft);
        ImGui::PushStyleColor(ImGuiCol_Border,        Col::line);
        ImGui::PushStyleColor(ImGuiCol_SliderGrab,    Col::fg);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0.f, 0.f});
        ImGui::SetNextItemWidth(sb_w);
        float scrub_val = fill;
        if (ImGui::SliderFloat("##scrub", &scrub_val, 0.f, 1.f, "")) {
            state.playhead = scrub_val * dur;
            audio_seek(state.playhead);
        }

        // Waveform ticks drawn on top of the scrub bar
        int tick_n = (int)(sb_w / 4.f);
        for (int t = 0; t < tick_n; ++t) {
            float amp = 0.3f + 0.6f * fabsf(sinf(t * 0.4f) * cosf(t * 0.13f));
            float tx  = sb_p.x + t * 4.f;
            float th  = amp * (sb_h - 6.f);
            dl->AddLine({tx, sb_p.y + (sb_h - th) * 0.5f},
                        {tx, sb_p.y + (sb_h + th) * 0.5f},
                        to_u32(fill > (float)t / tick_n ? Col::fg : Col::line));
        }

        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();

        // Time display
        ImGui::SameLine(0.f, 8.f);
        char tbuf[32];
        snprintf(tbuf, sizeof(tbuf), "%s / %s",
            fmt_time(state.playhead).c_str(), fmt_time(dur).c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted(tbuf);
        ImGui::PopStyleColor();

        // Font weight + navigation row
        ImGui::Dummy({0.f, 8.f});
        ImGui::SetCursorPosX(8.f);
        ui_label("Font weight");
        ImGui::SameLine(col_w - 180.f);
        for (int w : {400, 700, 900}) {
            char wlabel[16]; snprintf(wlabel, sizeof(wlabel), "%d", w);
            if (ui_btn(wlabel, state.font_weight == w, true))
                state.font_weight = w;
            ImGui::SameLine(0.f, 4.f);
        }

        ImGui::Dummy({0.f, 4.f});
        ui_separator();
        ImGui::SetCursorPosX(8.f);
        ui_label("Style");
        ImGui::SameLine(col_w - 200.f);
        if (ui_btn("Change style  ->")) state.go(Screen::Styles);
        ImGui::SameLine(0.f, 8.f);
        if (ui_btn("Render  ->", true)) state.go(Screen::Export);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);

    ImGui::EndChild();
}
