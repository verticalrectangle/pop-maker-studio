#pragma once
#include <imgui.h>

void theme_apply();

extern ImFont* g_font_mono;

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
    // per-track-type clip fill colors
    inline constexpr ImVec4 clip_sub      = {0.200f, 0.480f, 0.900f, 0.700f};  // blue  — subtitle
    inline constexpr ImVec4 clip_audio    = {0.180f, 0.700f, 0.420f, 0.700f};  // green — audio
    inline constexpr ImVec4 clip_video    = {0.560f, 0.200f, 0.900f, 0.700f};  // purple — video
    inline constexpr ImVec4 clip_lyrics   = {0.950f, 0.720f, 0.100f, 0.700f};  // amber — lyrics
    inline constexpr ImVec4 clip_subtitle = {0.100f, 0.750f, 0.750f, 0.700f};  // teal  — subtitle
    inline constexpr ImVec4 label         = {1.000f, 1.000f, 1.000f, 0.400f};
    inline constexpr ImVec4 dim           = {1.000f, 1.000f, 1.000f, 0.250f};
    inline constexpr ImVec4 transparent   = {0.000f, 0.000f, 0.000f, 0.000f};
}

inline ImU32 to_u32(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }

void ui_label(const char* text, ImVec4 col = Col::label);
void ui_separator();
bool ui_card_begin(const char* id, ImVec2 size, bool selected = false, bool hoverable = true);
void ui_card_end();
bool ui_btn(const char* label, bool filled = false, bool small = false);
