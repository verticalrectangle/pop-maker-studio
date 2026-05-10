#pragma once
// panel_clip.h — clip inspector panel

#include "studio_types.h"
#include "app.h"

void panel_clip(AppState& state, float w);

// Shared text-edit state — also written by timeline context menus
extern char s_edit_buf[512];
extern bool s_edit_focus_next;
