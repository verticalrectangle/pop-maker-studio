#pragma once
#include <imgui.h>

void theme_apply();

// ── Design tokens (match verticalrectangle.com) ───────────────────────────────
namespace Col {
    inline constexpr ImVec4 bg            = {0.000f, 0.000f, 0.000f, 1.000f};
    inline constexpr ImVec4 fg            = {1.000f, 1.000f, 1.000f, 1.000f};
    inline constexpr ImVec4 accent_dark   = {0.031f, 0.039f, 0.055f, 1.000f};
    inline constexpr ImVec4 line          = {1.000f, 1.000f, 1.000f, 0.150f};
    inline constexpr ImVec4 line_hover    = {1.000f, 1.000f, 1.000f, 0.500f};
    inline constexpr ImVec4 btn_line      = {1.000f, 1.000f, 1.000f, 0.300f};
    inline constexpr ImVec4 btn_line_hov  = {1.000f, 1.000f, 1.000f, 0.800f};
    inline constexpr ImVec4 bg_soft       = {1.000f, 1.000f, 1.000f, 0.030f};
    inline constexpr ImVec4 bg_soft_hov   = {1.000f, 1.000f, 1.000f, 0.040f};
    inline constexpr ImVec4 muted         = {1.000f, 1.000f, 1.000f, 0.450f};
    inline constexpr ImVec4 label         = {1.000f, 1.000f, 1.000f, 0.400f};
    inline constexpr ImVec4 dim           = {1.000f, 1.000f, 1.000f, 0.250f};
    inline constexpr ImVec4 transparent   = {0.000f, 0.000f, 0.000f, 0.000f};
}

// Convert ImVec4 → ImU32 for draw list calls
inline ImU32 to_u32(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }

// Label helper — uppercase small caps feel
void ui_label(const char* text, ImVec4 col = Col::label);

// Hairline separator
void ui_separator();

// A card-style child region
bool ui_card_begin(const char* id, ImVec2 size, bool selected = false, bool hoverable = true);
void ui_card_end();

// Ghost button (btn-line style)
bool ui_btn(const char* label, bool filled = false, bool small = false);

// Top bar + screen nav (shared across all screens)
void ui_topbar(struct AppState& state);
