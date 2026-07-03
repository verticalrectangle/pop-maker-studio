#pragma once
// pipeline.h — ML pipeline, beat detection, subtitle pipeline

#include "studio_types.h"
#include "app.h"
#include "transcribe.h"
#include <string>
#include <vector>

// ── SRT / subtitle ────────────────────────────────────────────────────────────
std::vector<Clip> parse_srt(const std::string& path);
void apply_subtitle_mode(AppState& state);
void apply_subtitle_pipeline(AppState& state);
void save_all_srts(AppState& state);
// Apply stored word edits to a freshly-grouped lyrics clip. Matches by word
// start frame (int(word.start * fps)). Call after clip.words is populated.
void apply_lyrics_edits(AppState& state, Clip& c);

// ── File import ───────────────────────────────────────────────────────────────
void import_file(AppState& state, const std::string& path);
void kick_pipeline(AppState& state, const std::string& path, PipelineMode mode);

// ── Beat / envelope detection ─────────────────────────────────────────────────
void run_beat_detect(AppState& state);
void run_envelope_extract(AppState& state);
void load_envelope_cache(AppState& state);
void load_words_cache(AppState& state);
void kick_clip_beat_detect(const std::string& src);
void poll_clip_beat_analysis(AppState& state);

// ── Pipeline UI strip ─────────────────────────────────────────────────────────
void draw_pipeline_strip(AppState& state, float w);
// Inline status strip for windowed find_and_add_clip / search_transcript runs.
// Renders the current search range (e.g. "MDX-Net 1:30 – 2:35: separating
// vocals…"), progress, a cancel control, and disappears when the search ends.
// Mirrors draw_pipeline_strip so agent-driven searches are visible to the
// human in the same panel as the full pipeline.
void draw_search_strip(float w);

// Safe-zone constants (SAFE_TOP/BOT/SIDE) moved to engine_seams.h — the
// overlay renderer (engine) needs them too.
#include "../engine_seams.h"
