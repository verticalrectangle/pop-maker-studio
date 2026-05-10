#pragma once
// panel_animation.h — animation + typography panels

#include "studio_types.h"
#include "app.h"

void panel_animation(AppState& state, float w);
void panel_typography(AppState& state, float w);

// generate_typography — also called from pipeline completion and import_file
void generate_typography(AppState& state);
