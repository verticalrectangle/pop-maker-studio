#pragma once
// studio_shared.h — pure helper functions shared across studio sub-modules

#include "studio_types.h"
#include "app.h"
#include "audio.h"
#include <cmath>
#include <vector>

// Timeline geometry (TLGeom) + frame-boundary snapping moved to
// engine_seams.h — the IPC dispatcher (engine) consumes both.
#include "../engine_seams.h"
#include <string>

// ── Time formatting ───────────────────────────────────────────────────────────
std::string fmt_time(float s);
std::string fmt_time_short(float s);

// ── Playback helpers ──────────────────────────────────────────────────────────
void seek_to(AppState& state, float t);
void toggle_play(AppState& state);
// Latest position the playhead can occupy and still show a frame: the start of
// the last whole frame. Clips are half-open [start,end), so a playhead at exactly
// `duration` selects nothing — this is the real end-of-timeline for playback,
// seeking and scrubbing. 0 when there's no content.
float last_playable_time(const AppState& state);

// Quantize a time to the timeline frame grid (round to the nearest frame). The
// playhead, markers and loop-region edges all live on frame boundaries so they
// never land mid-frame (which makes loops wrap and markers cut between frames).
float snap_to_frame(const AppState& state, float t);

// Snap EVERY time-bearing field (clip start/end/in_point/transitions/fades,
// keyframes, markers, loop points) onto the project frame grid. Run per-frame so
// nothing can sit between the ruler's frame ticks, however it was added.
void normalize_timeline_to_grid(AppState& state);

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

// Keyframe an RGB colour stored as three Clip float fields named <prefix>_r/_g/_b.
// kf_color_diamond draws one key-diamond that toggles keys on all three at the
// playhead (mirrors kf_slider). kf_color_edit, called after a colour picker writes
// the base r/g/b, mirrors the picked colour into the key at the playhead if one exists.
void kf_color_diamond(AppState& state, Clip& clip, int sel_ti, int sel_ci, const char* prefix);
void kf_color_edit(AppState& state, Clip& clip, const char* prefix, float r, float g, float b);

// ── Cross-surface FX clipboard ──────────────────────────────────────────────────
// One copied chain sub-effect — its params AND keyframes ride along on the Clip.
// Shared by the FX panel and the timeline chain lanes so copy-here / paste-there
// works either direction. `audio` guards against video<->audio cross-paste.
extern Clip s_fx_clipboard;
extern bool s_fx_clipboard_has;
extern bool s_fx_clipboard_audio;
void fx_clip_copy(const Clip& se, bool audio);
bool fx_clip_can_paste(const Clip& brick);          // clipboard full + kind matches brick
void fx_chain_duplicate(AppState& state, Clip& brick, int idx);
void fx_chain_paste(AppState& state, Clip& brick, int after_idx);   // after_idx < 0 → append
void fx_chain_delete(AppState& state, Clip& brick, int idx);

// ── Record brick ──────────────────────────────────────────────────────────────
// Insert a fresh Record brick (8 s, frame-snapped) at the playhead on a new
// top track, select it, and push history. Used by the toolbox rail and the
// timeline context menu.
void add_record_brick(AppState& state);
void add_video_record_brick(AppState& state);
void add_photo_capture_brick(AppState& state);
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
std::string source_from_key(const std::string& key);
// reveal: scroll the timeline to the new clip (default). Pass false for drops —
// the user placed it where they're already looking, so just glow it in place.
void add_clip_to_track(AppState& state, int track_idx, const std::string& path, ClipType ct, bool reveal = true);
void reopen_video_slots(AppState& state);
// Incremental slot opening for project load: queue every video-like source,
// then open a few per frame (tick) so the UI shows progress instead of
// freezing. reopen_video_slots() stays synchronous for small in-session heals.
void queue_video_slot_opens(AppState& state);

// ── Project open (THE load path) ──────────────────────────────────────────────
// Loads a .pms and performs the full swap: transcript/audio teardown, state
// move, audio_init + async audio_load, queued (non-blocking) video slot opens,
// audio_source_ensure per audio clip, recents push, fresh history baseline,
// and enters the studio. UI clicks and the IPC load_project handler both go
// through here — an agent-driven load must land the human in the loaded
// project too, not leave the Home page lying about what state holds.
bool open_project_path(AppState& state, const std::string& path);

// ── Track groups (folder rows) ────────────────────────────────────────────────
// A GroupHead track is a folder over the group_children tracks below it.
// group_head_of: index of the head whose run contains ti (-1 = ungrouped).
// normalize_track_groups: per-frame self-heal — clamps runs to the track list,
// stops them at the next head (no nesting), dissolves empty heads.
int  group_head_of(const AppState& state, int ti);
void normalize_track_groups(AppState& state);

// ── Save / dirty tracking ─────────────────────────────────────────────────────
// Call after every successful project save: records the clean history point
// (project_dirty goes false) and queues the home-screen thumbnail capture.
void mark_project_saved(AppState& state, const std::string& path);
// Call after load / new-project once the baseline history entry is pushed.
void mark_project_clean(AppState& state);
// True when edits exist past the last save/load point (undo back = clean).
bool project_dirty(const AppState& state);

// ── Splitter capture ─────────────────────────────────────────────────────────
// The studio's panel-resize handles are raw geometry hit-tests (not ImGui
// items). While one is hot or mid-drag this is true, and mouse consumers that
// do their own raw hit-testing (canvas picking) must stand down.
void ui_set_splitter_capture(bool on);
bool ui_splitter_capture();

// Double-click "homebase" reset for the LAST submitted slider item: when it is
// hovered and double-clicked, *v snaps back to defv (and the drag ImGui began
// on the first click is cancelled). Returns true on reset.
bool ui_slider_home(AppState& state, float* v, float defv, const char* hist_label);
// Default ("homebase") value of a keyframable clip prop — what a fresh Clip
// carries. Returns cur when the prop isn't in the kClipKfFields registry.
float clip_prop_default(const char* prop, float cur);
// Output canvas height in pixels for the current format (1920 vertical,
// 1080 horizontal/square) — the reference that fraction-based sizes
// (font_size et al.) scale against at render time.
float output_px_height(const AppState& state);
// Default for any float member of a struct instance, derived by pointer
// offset against a default-constructed twin — no per-field registry needed.
// Returns *v unchanged when v doesn't point inside obj (e.g. a local copy).
template <class T>
inline float struct_field_default(const T& obj, const float* v) {
    static const T s_def{};
    ptrdiff_t off = (const char*)v - (const char*)&obj;
    if (off < 0 || off + (ptrdiff_t)sizeof(float) > (ptrdiff_t)sizeof(T)) return *v;
    return *(const float*)((const char*)&s_def + off);
}

// ── Clip source resolution ────────────────────────────────────────────────────
// The file the proxy/export should actually decode for this clip: the all-intra
// H.264 intermediate when its proxy is ready, otherwise the original (clip.text).
std::string clip_video_src(const AppState& state, const Clip& cl);
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
// Search-enabled variant: draws a search field ABOVE the pills. `query` is the
// live filter text; combine with lib_search_match in the panel's item loop.
bool category_pills(const char* id, const std::vector<const char*>& cats,
                    std::string& sel, std::string& query);
// Case-insensitive substring match over up to two haystacks (label +
// description). Empty query matches everything.
bool lib_search_match(const std::string& query, const char* hay1,
                      const char* hay2 = nullptr);

// ── Eyedropper ────────────────────────────────────────────────────────────────
// A small pipette button to place beside any color picker. Clicking arms the
// dropper; the next click on the preview canvas samples the rendered pixel
// (canvas feeds it via ui_dropper_feed) and the SAME call site returns true
// once with the picked color — no dangling pointers, keyframe-aware at the
// caller (route the result through kf_color_edit where applicable).
bool ui_dropper_button(const char* id, float rgb_out[3]);
bool ui_dropper_active();
void ui_dropper_feed(float r, float g, float b);
void ui_dropper_cancel();

// ── group_words helpers (defined in pipeline.cpp, used by panel_animation) ────
// Segment-accurate grouping: words + Whisper segments (same time space) → clips.
std::vector<Clip> read_segment_clips(const std::string& seg_path);

// ── Panel-view write access ───────────────────────────────────────────────────
// Defined in screen_studio.cpp; some helpers (add_clip_to_track, panel_media,
// generate_typography) need to switch the panel on selection changes.
extern PanelView s_panel_view;
void app_register_engine_hooks();   // wire app callbacks into engine hook points

// A timeline interaction (clicking an FX lane below an expanded clip) can ask the
// props panel to open a specific view NEXT — overriding the default view that the
// panel router would otherwise derive from the new selection. The lane click
// selects the HOST content clip (so its FX tab can inspect the coupled brick) and
// requests HostFX/HostAudioFX so the clicked entry's keyframable sliders open
// straight away. Set by timeline.cpp, consumed once by the panel router.
void request_panel_view(PanelView v);
