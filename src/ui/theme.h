#pragma once
#include <imgui.h>

void theme_apply();

extern ImFont* g_font_mono;

// Typography face lookup by preset font id (sanitized family name, e.g.
// "anton", "playfairdisplay"). Returns the bundled display face, or the default
// lyrics face (Inter Black) for "" / unknown / a font that failed to bundle.
ImFont* typo_font_get(const char* id);

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

// "+" add-at-playhead button in a card's top-right corner. Caller calls
// ImGui::SetNextItemAllowOverlap() before the card's InvisibleButton so this
// can sit on top of the drag-source card body. Returns true when clicked.
bool ui_card_add_btn(ImVec2 card_tl, float card_w, int uid);

// ── Shared library-card hover-dwell clock ─────────────────────────────────────
// One card is tracked at a time (only one panel is ever visible). Use distinct
// id ranges per panel to avoid cross-panel aliasing. ui_card_hover_secs returns
// the seconds `card_id` has been continuously hovered (0 when it isn't the
// hovered card / the frame hover begins) — drives thumbnail animation AND the
// dwell gate. ui_card_hover_ready is the dwell test for opening a hover popover.
float ui_card_hover_secs(int card_id, bool hovered);
bool  ui_card_hover_ready(int card_id, float dwell_secs);

// ── Shared progress UI (one clean look for every async job) ───────────────────
// A progress bar at (x,y), width bw, height bh. progress in [0,1] fills; progress
// < 0 draws an indeterminate sweep (use for download / unknown-duration phases).
void ui_progress_bar(ImDrawList* dl, float x, float y, float bw, float progress,
                     ImU32 accent = IM_COL32(255, 165, 0, 255), float bh = 4.f);

// A progress banner centered along the bottom of a canvas rect (p, w, h): a dark
// pill with `title` and the shared bar. Used for clip-scoped jobs (bg removal,
// voice convert) so they read the same as each other over the preview.
void ui_canvas_progress_banner(ImDrawList* dl, ImVec2 p, float w, float h,
                               const char* title, float progress,
                               ImU32 accent = IM_COL32(255, 165, 0, 255));

// Big hover-preview popover anchored to the LEFT of a card (panels hug the right
// screen edge), screen-clamped, drawn on the foreground draw list. Shows `tex`
// fit within a box at its natural aspect (img_w/img_h), then name + optional
// subtitle. `flip` draws the image with flipped V (GL bottom-up sources).
void ui_card_image_popover(ImVec2 card_tl, ImTextureID tex,
                           float img_w, float img_h, bool flip,
                           const char* name, const char* subtitle);
