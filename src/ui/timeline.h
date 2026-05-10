#pragma once
// timeline.h — timeline widget

#include "studio_types.h"
#include "app.h"
#include <imgui.h>

void draw_timeline(AppState& state, ImVec2 origin, float total_w, float total_h);

// Drop state — read by ui_studio OS-drop handler
extern int   s_tl_hover_track;
extern float s_drop_flash_t;
extern int   s_drop_flash_track;
