#pragma once
// timeline.h — timeline widget

#include "studio_types.h"
#include "app.h"
#include <imgui.h>

void draw_timeline(AppState& state, ImVec2 origin, float total_w, float total_h);

// Couple the FX brick at (ti, ci) to the content clip at host_ci — converts
// singles to a Video Multi-FX, merges into the host's existing coupled chain,
// re-windows entries. Returns the resulting brick index. Used by the 1.5 s
// coupling ring, project migration, and IPC.
int timeline_couple_fx_brick(AppState& state, int ti, int ci, int host_ci);

// Drop state — read by ui_studio OS-drop handler
extern int   s_tl_hover_track;
extern float s_drop_flash_t;
extern int   s_drop_flash_track;
