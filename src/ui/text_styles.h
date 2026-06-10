#pragma once
// Text brick style cards + animated mini-preview + clip factory.
// Implemented in panel_animation.cpp; consumed by the Text library panel and
// the timeline's TEXT_STYLE drop handlers.
#include "app.h"
#include <imgui.h>

struct TextStyleCard {
    AnimStyle   style;
    const char* name;
    const char* desc;
    const char* tag;   // e.g. "sharp", "glitch", "soft"
};

// "Project Style" (AnimStyle::None) first, then the 8 named styles.
const TextStyleCard* text_style_cards(int* count);
const char*          text_style_name(AnimStyle st);   // "Fade", …

// Dark stage + animated sample text, looping every 2 s. `sample` defaults to
// the style's name when null.
void draw_text_style_preview(AnimStyle style, ImDrawList* dl, ImVec2 pos,
                             float w, float h, const char* sample = nullptr);

// A centered 4 s text clip with the given style, starting at `start`.
Clip make_text_brick(AnimStyle style, float start);
