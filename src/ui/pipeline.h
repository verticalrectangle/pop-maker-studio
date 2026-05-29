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
void draw_vision_download_strip(float w);

// ── Safe-zone constants (used by canvas & typography) ─────────────────────────
static constexpr float SAFE_TOP  = 0.08f;
static constexpr float SAFE_BOT  = 0.20f;
static constexpr float SAFE_SIDE = 0.05f;
