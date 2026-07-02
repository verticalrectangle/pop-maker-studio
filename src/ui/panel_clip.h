#pragma once
// panel_clip.h — clip inspector panel

#include "studio_types.h"
#include "app.h"

void panel_clip(AppState& state, float w);
void panel_face_filters(AppState& state, float w);

// Reusable style sections — shared with the Typography panel so subtitle/lyric
// styling (track-wide) and standalone Text (per-clip) use the same controls.
// Return true if a control was edited this frame (so the caller can restyle).
bool section_fade(AppState& state, float& fade_in, float& fade_out, float w);
bool section_text_style(AppState& state, TextStyle& ts, float w);

// Shared text-edit state — also written by timeline context menus
extern char s_edit_buf[512];
extern bool s_edit_focus_next;
