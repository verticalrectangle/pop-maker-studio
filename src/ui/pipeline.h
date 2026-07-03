#pragma once
// pipeline.h — the pipeline status strips (UI). All pipeline LOGIC moved to
// pipeline_core.h / src/pipeline_core.cpp in the engine.
#include "../pipeline_core.h"
#include "../app.h"

// ── Pipeline UI strip ─────────────────────────────────────────────────────────
void draw_pipeline_strip(AppState& state, float w);
// Inline status strip for windowed find_and_add_clip / search_transcript runs.
// Renders the current search range (e.g. "MDX-Net 1:30 – 2:35: separating
// vocals…"), progress, a cancel control, and disappears when the search ends.
// Mirrors draw_pipeline_strip so agent-driven searches are visible to the
// human in the same panel as the full pipeline.
void draw_search_strip(float w);
