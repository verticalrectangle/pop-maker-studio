#pragma once
// canvas.h — preview canvas rendering

#include "studio_types.h"
#include "app.h"
#include <imgui.h>

void draw_preview(AppState& state, ImVec2 p, float w, float h);
void draw_canvas_handles(AppState& state, ImDrawList* dl, ImVec2 p, float w, float h);
void compute_video_bbox(AppState& state, Clip& cl, ImVec2 p, float w, float h,
                        float& bx0, float& by0, float& bx1, float& by1);

// s_scrub_until — owned here, read by screen_studio coordinator
extern double s_scrub_until;
