#pragma once
// studio_shared.h — pure helper functions shared across studio sub-modules

#include "studio_types.h"
#include "app.h"
#include "audio.h"
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
// Latest position the playhead can occupy and still show a frame: the start of
// the last whole frame. Clips are half-open [start,end), so a playhead at exactly
// `duration` selects nothing — this is the real end-of-timeline for playback,
// seeking and scrubbing. 0 when there's no content.
float last_playable_time(const AppState& state);

// Quantize a time to the timeline frame grid (round to the nearest frame). The
// playhead, markers and loop-region edges all live on frame boundaries so they
// never land mid-frame (which makes loops wrap and markers cut between frames).
float snap_to_frame(const AppState& state, float t);

// ── Loop region ────────────────────────────────────────────────────────────────
// Effective loop bounds. Fills lo/hi with the region to cycle: the loop brace
// [loop_in,loop_out] when one is set, otherwise the whole timeline [0,duration].
// Returns true when a custom brace region is set (vs the whole-timeline default).
// Single source of truth for playback (app.cpp), prefetch (canvas.cpp) and the
// brace drawing (timeline.cpp).
bool loop_region(const AppState& state, float& lo, float& hi);

// ── Markers / locators ──────────────────────────────────────────────────────────
// Drop a marker at `time` (auto-labels "Marker N" when label is empty), keeping
// state.markers sorted by time. Returns the index of the new marker.
int  marker_add(AppState& state, float time, const char* label = nullptr);
// Seek to the nearest marker before (dir<0) or after (dir>0) the playhead.
void marker_jump(AppState& state, int dir);

// Decouple a welded FX brick (track ti, clip ci): free it and lift it onto a
// fresh track inserted just BELOW the content track, keeping its span. This
// makes it a normal, movable, global brick instead of leaving it on the content
// track where it re-arms the weld timer / reads as stuck. Returns the new track
// index, or -1 on bad input.
int decouple_fx_to_new_track(AppState& state, int ti, int ci);

// ── Keyframable slider control ─────────────────────────────────────────────────
// Renders a "‹ ◆ › Label  [slider]" row: the diamond toggles a keyframe at the
// playhead (filled on a key, hollow gold when animated off-key, faint when no
// keys), the arrows jump prev/next key, and editing the slider auto-keys the
// value at the playhead when keys exist. Keyframes are stored on `clip` under
// `prop` (clip == the brick for FX bricks, == the content clip for transform).
// `w` is the available panel width; `disp` scales the field for display (100 →
// percent); `prop2` mirrors keys onto a second track (the unified Size slider).
// Returns true when the value changed this frame.
bool kf_slider(AppState& state, Clip& clip, int sel_ti, int sel_ci, float w,
               const char* prop, const char* label, float* val_ptr,
               float vmin, float vmax, const char* fmt,
               float disp = 1.f, const char* prop2 = nullptr);

// ── Record brick ──────────────────────────────────────────────────────────────
// Insert a fresh Record brick (8 s, frame-snapped) at the playhead on a new
// top track, select it, and push history. Used by the toolbox rail and the
// timeline context menu.
void add_record_brick(AppState& state);
void add_video_record_brick(AppState& state);
void add_bus_brick(AppState& state);

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

// ── Frame-rate conform ────────────────────────────────────────────────────────
// True if this clip's native fps differs enough from the project to warrant a
// conform (src_fps must already be probed; stills/src_fps<=0 are never conformed).
bool clip_needs_conform(const Clip& cl, int project_fps);
// The file the proxy/export should actually decode for this clip: the conformed
// copy when it's ready, otherwise the original (clip.text).
std::string clip_video_src(const AppState& state, const Clip& cl);
// Per-frame: probe native fps lazily, kick conforms, and reopen video slots when
// a conform becomes ready so the preview/export swap to it. Call from app_frame.
void conform_tick(AppState& state);
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
// Windowed audio FX for one clip: a segment per overlapping audio FX brick
// (and per audio entry inside overlapping MultiFX chains), each mapped into
// the clip's source time. The FX applies only over the brick's range.
// Extract the AudioFX params from a single audio FX entry (brick or chain
// entry). Returns false when the entry is inactive/not audio.
bool audio_fx_from_brick_pub(const Clip& cl, AudioFX& out);
// Bus-brick configs for the live mixer (one per ClipType::Bus clip).
std::vector<AudioBusBrick> collect_bus_bricks(const AppState& state);
std::vector<AudioFXSegment> collect_audio_fx_segments(const AppState& state,
                                                      int track_idx,
                                                      const Clip& audio_clip);

struct FxBrickColors { ImU32 fill, border, label; };
FxBrickColors fx_brick_colors(FXType ft, bool sel);

// ── Shared source-duration cache ──────────────────────────────────────────────
// Defined in studio_shared.cpp; used by panel_media, timeline, etc.
#include <unordered_map>
extern std::unordered_map<std::string, float> s_source_durations;

// ── Palette widget (defined in panel_clip.cpp, shared across panels) ──────────
// ext_focus (optional): the caller owns the "which slot a single swatch targets"
// state and shows the slot chooser itself — the widget then skips its built-in
// "Apply to" chips and reads/advances *ext_focus instead.
void palette_widget(const char* id, float** slots, int n_slots, bool has_alpha = false,
                    int* ext_focus = nullptr);
void palette_widget(const char* id, float* rgb);

// Wrapping row of category filter pills: "All" + each category in `cats`.
// `sel` is the active filter ("" = All) and is updated on click. Returns true
// if the selection changed this frame. Shared by the typography / FX / background
// libraries so they all filter in place the same way.
bool category_pills(const char* id, const std::vector<const char*>& cats, std::string& sel);

// ── group_words helper (defined in pipeline.cpp, used by panel_animation) ─────
std::vector<Clip> group_words(const std::vector<Clip>& words, SubtitleMode mode,
                               int custom_n = 5, float pause_gap = 0.8f, int max_words = 8);

// ── Panel-view write access ───────────────────────────────────────────────────
// Defined in screen_studio.cpp; some helpers (add_clip_to_track, panel_media,
// generate_typography) need to switch the panel on selection changes.
extern PanelView s_panel_view;
