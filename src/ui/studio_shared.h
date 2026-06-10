#pragma once
// studio_shared.h — pure helper functions shared across studio sub-modules

#include "studio_types.h"
#include "app.h"
#include <cmath>

// ── Frame-boundary snapping ───────────────────────────────────────────────────
inline float snap_to_frame(float t, int fps) {
    if (fps <= 0) return t;
    return std::roundf(t * fps) / fps;
}
// Clip ends always ceil to the next frame so no sub-frame gap is left before
// the following clip. The 1e-4 epsilon absorbs float noise when t is already
// exactly on a frame boundary.
inline float snap_end_to_frame(float t, int fps) {
    if (fps <= 0) return t;
    return std::ceil(t * fps - 1e-4f) / fps;
}
#include <string>

// ── Time formatting ───────────────────────────────────────────────────────────
std::string fmt_time(float s);
std::string fmt_time_short(float s);

// ── Playback helpers ──────────────────────────────────────────────────────────
void seek_to(AppState& state, float t);
void toggle_play(AppState& state);
float tl_fps(const AppState& state);

// ── Retime ────────────────────────────────────────────────────────────────────
// Rescale a media clip's timeline width and any FX bricks overlapping it,
// anchored at the clip's start. ratio = new_speed / old_speed. Used by the
// clip panel's speed control and the IPC set_clip_prop(s) speed path so both
// have the same semantics: changing speed keeps the source span, not the
// timeline width.
void rescale_glass_bricks(AppState& state, int media_ti, int media_ci, float speed_ratio);

// ── Multi-selection ops ───────────────────────────────────────────────────────
// Both fall back to the primary single selection when clip_selection has <=1
// entry. Return true if anything changed (caller pushes history).
bool delete_selected_clips(AppState& state);
bool duplicate_selected_clips(AppState& state);

// ── Clip / slot helpers ───────────────────────────────────────────────────────
// First track that's safe to reuse for a newly added media clip: no clips,
// visible, not locked, not managed (managed = lyric/typography tracks — those
// keep their reserved spot even when empty). Returns -1 when none exists and
// the caller should create a fresh track.
int find_empty_track(const AppState& state);
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
bool fx_type_is_audio_fx(FXType ft);
AudioFX collect_audio_fx_for_clip(const AppState& state, int track_idx, const Clip& audio_clip);

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
