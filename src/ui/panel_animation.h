#pragma once
// panel_animation.h — animation + typography panels

#include "studio_types.h"
#include "app.h"

void panel_typography(AppState& state, float w);

// Text brick library — create text clips from the UI (click = playhead,
// drag = timeline). Style cards/preview shared via text_styles.h.
void panel_text_library(AppState& state, float w);

// Lyric brick library — drag/click a durable lyric brick onto a lyrics track.
void panel_lyric_library(AppState& state, float w);

// generate_typography — also called from pipeline completion and import_file
void generate_typography(AppState& state);
