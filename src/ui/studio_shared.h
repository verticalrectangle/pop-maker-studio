#pragma once
// studio_shared.h — pure helper functions shared across studio sub-modules

#include "studio_types.h"
#include "app.h"
#include <string>

// ── Time formatting ───────────────────────────────────────────────────────────
std::string fmt_time(float s);
std::string fmt_time_short(float s);

// ── Playback helpers ──────────────────────────────────────────────────────────
void seek_to(AppState& state, float t);
void toggle_play(AppState& state);
float tl_fps(const AppState& state);

// ── Clip / slot helpers ───────────────────────────────────────────────────────
std::string clip_slot_key(const std::string& src, float start);
std::string source_from_key(const std::string& key);
void add_clip_to_track(AppState& state, int track_idx, const std::string& path, ClipType ct);
int  slot_for_video(AppState& state, const std::string& key, const std::string& src);
void gc_video_slots(AppState& state);
void reopen_video_slots(AppState& state);
float project_end(const AppState& state);
bool  is_audio_file(const std::string& path);

// ── Panel-view helpers ────────────────────────────────────────────────────────
bool      pv_is_lib(PanelView v);
bool      pv_is_override(PanelView v);
PanelView pv_derive(const AppState& state);

// ── FX / clip display helpers (shared across panel_clip and panel_fx) ─────────
ImVec4 clip_type_badge_color(ClipType ct);
const char* clip_type_name(ClipType ct);
ImU32 fx_type_accent(FXType ft);
const char* fx_type_name(FXType ft);
const char* fx_type_display(FXType ft);
const char* clip_display_name(const Clip& cl);
ImU32 clip_badge_color(const Clip& cl);
bool fx_type_is_adjustment_style(FXType ft);

struct FxBrickColors { ImU32 fill, border, label; };
FxBrickColors fx_brick_colors(FXType ft, bool sel);

// ── Shared source-duration cache ──────────────────────────────────────────────
// Defined in studio_shared.cpp; used by panel_media, timeline, etc.
#include <unordered_map>
extern std::unordered_map<std::string, float> s_source_durations;

// ── Palette widget (defined in panel_clip.cpp, shared across panels) ──────────
void palette_widget(const char* id, float** slots, int n_slots, bool has_alpha = false);
void palette_widget(const char* id, float* rgb);

// ── group_words helper (defined in pipeline.cpp, used by panel_animation) ─────
std::vector<Clip> group_words(const std::vector<Clip>& words, SubtitleMode mode,
                               int custom_n = 5, float pause_gap = 0.8f, int max_words = 8);

// ── Panel-view write access ───────────────────────────────────────────────────
// Defined in screen_studio.cpp; some helpers (add_clip_to_track, panel_media,
// generate_typography) need to switch the panel on selection changes.
extern PanelView s_panel_view;
