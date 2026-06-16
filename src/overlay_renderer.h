#pragma once
#include "app.h"
#include <imgui.h>

// Render active text/subtitle overlays at time t into dl.
// p = canvas top-left, w/h = canvas dimensions in draw-list coordinates.
// Only clips that are actually active (start <= t < end) are rendered.
// only_track >= 0 draws just that track's text (others still count toward the
// vertical stacking offset, so positions are unchanged) — used to render one
// track's text to its own texture so it can composite at the track's z-order.
void draw_text_overlays(ImDrawList* dl, const AppState& state, float t,
                        ImVec2 p, float w, float h, int only_track = -1);
