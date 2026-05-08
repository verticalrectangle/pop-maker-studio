#pragma once
#include "app.h"
#include <imgui.h>

// Render all active text/subtitle overlays at time t into dl.
// p = canvas top-left, w/h = canvas dimensions in draw-list coordinates.
// Only clips that are actually active (start <= t < end) are rendered.
void draw_text_overlays(ImDrawList* dl, const AppState& state, float t,
                        ImVec2 p, float w, float h);
