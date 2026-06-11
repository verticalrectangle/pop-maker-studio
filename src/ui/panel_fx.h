#pragma once
// panel_fx.h — FX library + clip FX inspector + adjustment + background panels

#include "studio_types.h"
#include "app.h"

void panel_fx_creative(AppState& state, float w);
void panel_fx_audio(AppState& state, float w);
void panel_fx_clip(AppState& state, float w);
void panel_multifx(AppState& state, float w);
void panel_audio_fx_clip(AppState& state, float w);
void panel_adjustment_library(AppState& state, float w);
void panel_adjustment(AppState& state, float w);
void panel_body_fx_library(AppState& state, float w);
void panel_background(AppState& state, float w, bool clip_only = false);

// Layout section for glass FX bricks (Effect/MultiFX/BodyFX): edits the HOST
// clip's rotation — the brick has no transform of its own. No-op for
// non-glass bricks.
void glass_host_layout(AppState& state, Clip& brick, float w);

// FX preset helpers (used by timeline drag-drop)
#include "presets.h"
void preset_apply(Clip& clip, const EffectPreset& p);
