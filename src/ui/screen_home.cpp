#include "screens.h"
#include "theme.h"
#include "app.h"
#include <imgui.h>
#include <cstring>
#include <cmath>

static const char* MARQUEE_ITEMS[] = {
    "WHISPER-V3 ALIGNMENT",
    "WORD-LEVEL TIMESTAMPS",
    "KARAOKE BOUNCE READY",
    "SRT / VTT EXPORT",
    "9:16  16:9  1:1",
    "NO TEMPLATES  NO LOOPS",
    "DEMUCS VOCAL SEPARATION",
    "RUNS LOCALLY  NO UPLOADS",
};
static constexpr int MARQUEE_COUNT = 8;

static float g_marquee_offset = 0.f;

void ui_screen_home(AppState& state) {
    ImGui::SetNextWindowPos(ImGui::GetCursorScreenPos());
    ImGui::SetNextWindowSize(ImGui::GetContentRegionAvail());
    ImGui::BeginChild("##home_body", {0, 0}, ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    float win_w = ImGui::GetContentRegionAvail().x;

    // ── Eyebrow ───────────────────────────────────────────────────────────────
    ImGui::Dummy({0.f, 48.f});
    ImGui::SetCursorPosX((win_w - 420.f) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextUnformatted("POP MAKER STUDIO  —  EST. 2026  /  LYRIC & KARAOKE VIDEO FOR POP ARTISTS");
    ImGui::PopStyleColor();

    // ── Hero title ────────────────────────────────────────────────────────────
    ImGui::Dummy({0.f, 24.f});

    // Title lines — large bold
    extern ImFont* g_font_bold;
    ImGui::PushFont(g_font_bold);

    struct TitleLine { const char* text; bool outline; };
    TitleLine title_lines[] = {
        {"MAKE YOUR",    false},
        {"LYRIC VIDEO",  true },
        {"BEFORE THE",   false},
        {"TRACK DROPS.", false},
    };

    float title_sz = 72.f;
    for (auto& line : title_lines) {
        ImVec2 tsz = ImGui::CalcTextSize(line.text);
        float scale = title_sz / 14.f; // 14 is our loaded font size
        ImGui::SetCursorPosX((win_w - tsz.x * scale) * 0.5f);

        ImGui::SetWindowFontScale(scale);
        if (line.outline) {
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted(line.text);
            ImGui::PopStyleColor();
        } else {
            ImGui::TextUnformatted(line.text);
        }
        ImGui::SetWindowFontScale(1.f);
    }
    ImGui::PopFont();

    // ── Tagline ───────────────────────────────────────────────────────────────
    ImGui::Dummy({0.f, 24.f});
    const char* tag = "Drop a stem. Get word-timed lyrics, a render in three formats,\n"
                      "and a karaoke export — locally. No uploads. Your files stay yours.";
    ImVec2 tag_sz = ImGui::CalcTextSize(tag, nullptr, false, 520.f);
    ImGui::SetCursorPosX((win_w - 520.f) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 520.f);
    ImGui::TextUnformatted(tag);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    // ── CTAs ──────────────────────────────────────────────────────────────────
    ImGui::Dummy({0.f, 24.f});
    float cta_x = (win_w - 280.f) * 0.5f;
    ImGui::SetCursorPosX(cta_x);
    if (ui_btn("Upload your track  ->", true))
        state.go(Screen::Upload);
    ImGui::SameLine(0.f, 12.f);
    if (ui_btn("See the styles  ->"))
        state.go(Screen::Styles);

    // ── Stats bar ─────────────────────────────────────────────────────────────
    ImGui::Dummy({0.f, 48.f});
    ui_separator();

    struct Stat { const char* num; const char* cap; };
    Stat stats[] = {
        {"3:42",  "Avg. render — 1080p"},
        {".97",   "Whisper-grade alignment"},
        {"9:16",  "Native vertical out"},
        {"Local", "Files never leave your machine"},
    };

    float col_w = win_w / 4.f;
    ImGui::BeginGroup();
    for (int i = 0; i < 4; ++i) {
        if (i > 0) {
            ImGui::SameLine(0.f, 0.f);
            // vertical divider
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(
                {p.x, p.y}, {p.x, p.y + 64.f}, to_u32(Col::line));
        }
        ImGui::BeginGroup();
        ImGui::Dummy({col_w, 0.f});
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24.f);

        ImGui::PushFont(g_font_bold);
        ImGui::SetWindowFontScale(2.2f);
        ImGui::TextUnformatted(stats[i].num);
        ImGui::SetWindowFontScale(1.f);
        ImGui::PopFont();

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24.f);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted(stats[i].cap);
        ImGui::PopStyleColor();
        ImGui::EndGroup();
    }
    ImGui::EndGroup();
    ui_separator();

    // ── Marquee ───────────────────────────────────────────────────────────────
    ImGui::Dummy({0.f, 4.f});
    g_marquee_offset += ImGui::GetIO().DeltaTime * 60.f;

    ImVec2 marquee_pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl     = ImGui::GetWindowDrawList();
    float item_w       = 220.f;
    float total_w      = item_w * MARQUEE_COUNT;
    float offset       = fmodf(g_marquee_offset, total_w);

    dl->PushClipRect(marquee_pos, {marquee_pos.x + win_w, marquee_pos.y + 20.f}, true);
    for (int rep = 0; rep < 3; ++rep) {
        for (int i = 0; i < MARQUEE_COUNT; ++i) {
            float x = marquee_pos.x + i * item_w + rep * total_w - offset;
            dl->AddText({x, marquee_pos.y + 2.f}, to_u32(Col::muted), MARQUEE_ITEMS[i]);
            dl->AddCircleFilled({x - 12.f, marquee_pos.y + 8.f}, 2.f, to_u32(Col::dim));
        }
    }
    dl->PopClipRect();
    ImGui::Dummy({0.f, 20.f});

    ImGui::EndChild();
}
