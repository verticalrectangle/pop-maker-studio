#include "screens.h"
#include "theme.h"
#include "app.h"
#include <imgui.h>
#include <cmath>

struct StyleCard {
    AnimStyle   style;
    const char* name;
    const char* desc;
    const char* tag;   // e.g. "sharp", "glitch", "soft"
};

static const StyleCard STYLES[] = {
    {AnimStyle::Fade,       "Fade",       "Opacity in/out — clean and invisible", "soft"},
    {AnimStyle::Glitch,     "Glitch",     "Digital artefact noise — corrupt feel", "glitch"},
    {AnimStyle::Typewriter, "Typewriter", "Character-by-character reveal", "retro"},
    {AnimStyle::Bounce,     "Bounce",     "Drops in with spring overshoot", "lively"},
    {AnimStyle::Scale,      "Scale",      "Punches in from small — zoom", "punchy"},
    {AnimStyle::Slide,      "Slide",      "Enters from the left, holds, exits right", "motion"},
    {AnimStyle::Stack,      "Stack",      "Lines pile — previous dims on entry", "dense"},
    {AnimStyle::Block,      "Block",      "White background fill — high contrast", "sharp"},
};
static constexpr int STYLE_COUNT = 8;

void ui_screen_styles(AppState& state) {
    ImGui::SetNextWindowPos(ImGui::GetCursorScreenPos());
    ImGui::SetNextWindowSize(ImGui::GetContentRegionAvail());
    ImGui::BeginChild("##styles_body", {0, 0});

    float win_w = ImGui::GetContentRegionAvail().x;
    float pad_x = fmaxf((win_w - 960.f) * 0.5f, 32.f);
    extern ImFont* g_font_bold;

    ImGui::Dummy({0.f, 48.f});

    // ── Header ────────────────────────────────────────────────────────────────
    ImGui::SetCursorPosX(pad_x);
    ImGui::PushFont(g_font_bold);
    ImGui::SetWindowFontScale(2.2f);
    ImGui::TextUnformatted("Pick a style.");
    ImGui::SetWindowFontScale(1.f);
    ImGui::PopFont();
    ImGui::SameLine(win_w - pad_x - ImGui::CalcTextSize("04 / 05  —  Style").x);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextUnformatted("04 / 05  —  Style");
    ImGui::PopStyleColor();

    ImGui::SetCursorPosX(pad_x);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextUnformatted("Eight motion presets, word-synced. Click to commit. Tweak per-line later.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 8.f});
    ImGui::SetCursorPosX(pad_x);
    ui_separator();
    ImGui::Dummy({0.f, 24.f});

    // ── Style grid — 2 columns ────────────────────────────────────────────────
    float grid_w  = win_w - pad_x * 2.f;
    float card_w  = (grid_w - 32.f) * 0.5f;
    float card_h  = 160.f;

    for (int i = 0; i < STYLE_COUNT; ++i) {
        if (i % 2 == 0) ImGui::SetCursorPosX(pad_x);
        else            ImGui::SameLine(0.f, 32.f);

        const auto& sc      = STYLES[i];
        bool        selected = state.style == sc.style;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, selected ? Col::bg_soft_hov : Col::bg_soft);
        ImGui::PushStyleColor(ImGuiCol_Border,  selected ? Col::fg : Col::line);

        char cid[32]; snprintf(cid, sizeof(cid), "##sc%d", i);
        if (ImGui::BeginChild(cid, {card_w, card_h}, ImGuiChildFlags_Borders)) {
            // Animated preview area — top half, simulate motion with time-based effect
            float prev_w = card_w - 24.f;
            float prev_h = 72.f;
            ImVec2 ppos  = ImGui::GetCursorScreenPos();
            ppos.x += 12.f;
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Dark stage
            dl->AddRectFilled(ppos, {ppos.x + prev_w, ppos.y + prev_h},
                to_u32(Col::accent_dark), 2.f);

            // Animated style preview — simple representations
            float t = (float)ImGui::GetTime();
            float phase = fmodf(t, 2.f) / 2.f;  // 0..1 loop every 2s

            ImU32 txt_col = to_u32(Col::fg);
            float alpha   = 1.f;

            switch (sc.style) {
                case AnimStyle::Fade:
                    alpha = phase < 0.5f ? phase * 2.f : 1.f - (phase - 0.5f) * 2.f;
                    txt_col = ImGui::ColorConvertFloat4ToU32({1,1,1,alpha});
                    break;
                case AnimStyle::Block: {
                    float bw = prev_w * 0.5f;
                    dl->AddRectFilled(
                        {ppos.x + (prev_w - bw) * 0.5f, ppos.y + prev_h * 0.3f},
                        {ppos.x + (prev_w + bw) * 0.5f, ppos.y + prev_h * 0.7f},
                        to_u32(Col::fg), 2.f);
                    txt_col = to_u32(Col::bg);
                    break;
                }
                case AnimStyle::Glitch: {
                    float shake = sinf(t * 40.f) * 3.f * phase;
                    ppos.x += shake;
                    break;
                }
                case AnimStyle::Bounce: {
                    float y_off = phase < 0.3f ? (0.3f - phase) / 0.3f * 12.f : 0.f;
                    ppos.y += y_off;
                    break;
                }
                case AnimStyle::Scale: {
                    // can't scale ImDrawList text easily — show as expanding rect hint
                    float sc_f = 0.4f + phase * 0.6f;
                    float rw = prev_w * 0.3f * sc_f;
                    dl->AddRect(
                        {ppos.x + (prev_w - rw) * 0.5f, ppos.y + prev_h * 0.35f},
                        {ppos.x + (prev_w + rw) * 0.5f, ppos.y + prev_h * 0.65f},
                        to_u32(Col::dim), 2.f);
                    break;
                }
                case AnimStyle::Slide: {
                    float x_off = phase < 0.3f ? (0.3f - phase) / 0.3f * -30.f :
                                  phase > 0.7f ? (phase - 0.7f) / 0.3f * 30.f : 0.f;
                    ppos.x += x_off;
                    break;
                }
                default: break;
            }

            // Preview text
            const char* sample = sc.name;
            ImVec2 tsz = ImGui::CalcTextSize(sample);
            dl->AddText(
                {ppos.x + (prev_w - tsz.x) * 0.5f,
                 ppos.y + (prev_h - tsz.y) * 0.5f},
                txt_col, sample);

            ImGui::Dummy({0.f, prev_h + 8.f});

            // Card info
            ImGui::SetCursorPosX(12.f);
            ImGui::PushFont(g_font_bold);
            ImGui::SetWindowFontScale(1.15f);
            ImGui::TextUnformatted(sc.name);
            ImGui::SetWindowFontScale(1.f);
            ImGui::PopFont();

            ImGui::SameLine(card_w - ImGui::CalcTextSize(sc.tag).x - 24.f);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::label);
            ImGui::TextUnformatted(sc.tag);
            ImGui::PopStyleColor();

            ImGui::SetCursorPosX(12.f);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted(sc.desc);
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();

        // Whole card is clickable
        if (ImGui::IsItemClicked())
            state.style = sc.style;

        ImGui::PopStyleColor(2);

        if (i % 2 == 1 && i < STYLE_COUNT - 1)
            ImGui::Dummy({0.f, 16.f});
    }

    // ── Bottom actions ────────────────────────────────────────────────────────
    ImGui::Dummy({0.f, 24.f});
    ImGui::SetCursorPosX(pad_x);

    // Active summary
    const char* active_name = "Block";
    for (auto& sc : STYLES)
        if (sc.style == state.style) { active_name = sc.name; break; }
    char summary[64];
    snprintf(summary, sizeof(summary), "Active — %s  ·  weight %d", active_name, state.font_weight);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::label);
    ImGui::TextUnformatted(summary);
    ImGui::PopStyleColor();

    float right_x = win_w - pad_x - 220.f;
    ImGui::SameLine(right_x);
    if (ui_btn("Back to lyrics")) state.go(Screen::Editor);
    ImGui::SameLine(0.f, 8.f);
    if (ui_btn("Render  ->", true)) state.go(Screen::Export);

    ImGui::EndChild();
}
