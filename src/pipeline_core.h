#pragma once
// pipeline_core.h — ML pipeline, beat detection, subtitle pipeline (engine).
// Hoisted from ui/pipeline.h; the draw_* strips stay in ui/pipeline.h.

#include "app.h"
#include "transcribe.h"
#include <string>
#include <vector>

// ── Word grouping (impl: pipeline_core.cpp) ──────────────────────────────────
std::vector<Clip> group_words(const std::vector<Clip>& words, SubtitleMode mode,
                               int custom_n = 5, float pause_gap = 0.8f, int max_words = 8);
std::vector<Clip> group_words_segmented(const std::vector<Clip>& words,
                                        const std::vector<Clip>& segments,
                                        SubtitleMode mode, float pause_gap = 0.3f,
                                        int max_words = 0);

// ── SRT / subtitle ────────────────────────────────────────────────────────────
std::vector<Clip> parse_srt(const std::string& path);
std::vector<Clip> read_segment_clips(const std::string& seg_path);
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


// Safe-zone constants (SAFE_TOP/BOT/SIDE) moved to engine_seams.h — the
// overlay renderer (engine) needs them too.
#include "engine_seams.h"
