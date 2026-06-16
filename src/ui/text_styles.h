#pragma once
// Text brick name table + clip factory.
// Implemented in panel_animation.cpp; consumed by the Text library panel and
// the timeline's TEXT_STYLE drop handlers. The per-animation picker was folded
// into the Typography tab — AnimStyle now comes from a typography preset.
#include "app.h"
#include <imgui.h>

struct TextStyleCard {
    AnimStyle   style;
    const char* name;
    const char* desc;
    const char* tag;   // e.g. "sharp", "glitch", "soft"
};

// AnimStyle → display name (e.g. "Fade") for drop-history labels.
const char* text_style_name(AnimStyle st);

// A centered 4 s text clip with the given style, starting at `start`.
Clip make_text_brick(AnimStyle style, float start);
