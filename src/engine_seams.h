#pragma once
// engine_seams.h — the ONLY doorway between engine code (src/*.cpp) and the
// desktop app layer (src/ui/*, main.cpp, app.cpp).
//
// Rules (enforced by tools/check_engine_deps.sh at build time):
//   - engine sources must not include "ui/..." headers or the GLFW backend;
//     anything they need from the app side is declared HERE;
//   - every declaration here is a TODO: it marks app-side code the engine
//     depends on. The iOS port (docs/IOS_PORT_PLAN.md Phase 0) hoists these
//     implementations into engine files one by one until this header is
//     empty — at which point pms-engine links standalone.
//
// ImGui CORE types (ImVec2/ImU32/ImFont/draw lists) are engine-legal: ImGui
// is a portable library and the engine uses its drawlist/font machinery as
// the text rasterizer on every platform (Metal backend on iOS). Only the
// GLFW backend and the desktop panels are app-side.

#include "app.h"
#include "audio.h"      // AudioBusBrick
#include "audio_fx.h"   // AudioFX, AudioFXSegment
#include "face_track.h" // FT_NPTS (MirrorDebugGeom)
#include <imgui.h>
#include <cmath>
#include <string>
#include <vector>

// ── Safe-zone layout constants (canvas, typography, overlay renderer) ────────
static constexpr float SAFE_TOP  = 0.08f;
static constexpr float SAFE_BOT  = 0.20f;
static constexpr float SAFE_SIDE = 0.05f;

// ── Media bin (impl: src/media_bin.cpp — hoisted) ────────────────────────────
enum class MediaKind { Video, Image, Audio };
struct RecentMedia { std::vector<std::string> videos, images, audio; };
RecentMedia& recent_media();
void recent_media_push(const std::string& path, MediaKind kind);
MediaKind kind_for_path(const std::string& path);
bool bin_contains(const AppState& state, const std::string& path);
float tl_fps(const AppState& state);
bool clip_needs_conform(const Clip& cl, int project_fps);
bool is_animated_image(const std::string& p);

// ── Frame-grid snapping (pure engine logic, MOVED here from ui) ──────────────
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
float snap_to_frame(const AppState& state, float t);
int find_empty_track(const AppState& state);
std::string clip_slot_key(const std::string& src, float start);
int slot_for_video(AppState& state, const std::string& key, const std::string& src);

// ── Timeline geometry snapshot (agents drive the UI via ui_input) ────────────
struct TLGeomClip { int track, clip; float x0, y0, x1, y1; bool has_fx, expanded; float disc_cx, disc_cy; };
struct TLGeomLane { int track, clip, idx; float x0, y0, x1, y1; bool audio; };
struct TLGeom { std::vector<TLGeomClip> clips; std::vector<TLGeomLane> lanes; };
const TLGeom& tl_geom();

// ── Timeline / project maintenance (impl: ui/timeline.cpp, ui/panel_media.cpp)
int  timeline_couple_fx_brick(AppState& state, int ti, int ci, int host_ci);
int  decouple_fx_to_new_track(AppState& state, int ti, int ci);
void rescale_glass_bricks(AppState& state, int media_ti, int media_ci, float speed_ratio);
bool is_image_path(const std::string& p);
void mark_project_saved(AppState& state, const std::string& path);
void recovery_clear();
void bin_backfill_from_timeline(AppState& state);
void bin_add(AppState& state, const std::string& path);
void bin_remove(AppState& state, const std::string& path);
int  group_head_of(const AppState& state, int ti);

// ── Project open/save chokepoint (impl: ui/studio_shared.cpp) ────────────────
bool open_project_path(AppState& state, const std::string& path);
void recent_projects_push(const std::string& path);
std::vector<std::string> recent_projects_list();
std::string recovery_pms_path();
std::string recovery_meta_path();
void mark_project_clean(AppState& state);
void queue_video_slot_opens(AppState& state);

// ── Overlap resolution shared by UI drops and IPC adds (ui/studio_shared.cpp)
bool row_overlap_on_track(AppState& state, int ti, float start, float end,
                          int ignore_ci);

// ── Clip source resolution (impl: ui/studio_shared.cpp — engine logic, hoist)
std::string clip_video_src(const AppState& state, const Clip& cl);

// ── Audio FX collection (impl: ui/studio_shared.cpp — engine logic, hoist) ───
bool fx_type_is_audio_fx(FXType ft);
bool audio_fx_from_brick_pub(const Clip& cl, AudioFX& out);
std::vector<AudioBusBrick> collect_bus_bricks(const AppState& state);
std::vector<AudioFXSegment> collect_audio_fx_segments(const AppState& state,
                                                      int track_idx,
                                                      const Clip& audio_clip);

// ── Typography (impl: ui/panel_animation.cpp) ────────────────────────────────
void    generate_typography(AppState& state);          // impl: typography_core.cpp (engine)
void    apply_typo_style(Clip& c, const struct TypographyPreset& pr,
                         const AppState& state);        // impl: typography_core.cpp (engine)
void    app_focus_typography_panel();                   // engine-owned; no-op unless the app registers
void    set_focus_typography_hook(void (*fn)());        // app registers its panel flip here
ImFont* typo_font_get(const char* id);           // impl: text_renderer.cpp (engine)
void    typo_font_clear();                        // theme_apply() calls these
void    typo_font_register(const char* name, ImFont* font);

// ── ML pipeline (impl: ui/pipeline.cpp — engine logic, hoist) ────────────────
void import_file(AppState& state, const std::string& path);
void kick_pipeline(AppState& state, const std::string& path, PipelineMode mode);
void apply_subtitle_pipeline(AppState& state);
void apply_subtitle_mode(AppState& state);
std::vector<Clip> parse_srt(const std::string& path);
void run_beat_detect(AppState& state);
void load_words_cache(AppState& state);

// ── Canvas / snapshot / live-mirror debug (impl: ui/canvas.cpp) ──────────────
struct MirrorDebugGeom {
    bool  valid = false;
    float cx = 0, cy = 0;
    float hw = 0, hh = 0;
    float rot_deg = 0;
    int   cam_w = 0, cam_h = 0;
    bool  face_valid = false;
    float pts[FT_NPTS][2];
};
MirrorDebugGeom mirror_debug_geom();

struct CanvasHandleGeom {
    bool  valid = false;
    float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;  // selection bbox (un-rotated, local)
    float rot_x = 0, rot_y = 0;                // rotate knob center (rotated)
    float cx = 0, cy = 0;                      // rotation pivot (clip center)
    float rot_deg = 0;                         // clip rotation (handles rotate with it)
};
CanvasHandleGeom canvas_handle_geom();
// App-writable stores behind the accessors above (engine owns storage —
// see src/ui_geom.cpp; the desktop UI fills them per frame).
extern CanvasHandleGeom g_canvas_handle_geom;
extern MirrorDebugGeom  g_mirror_dbg_geom;
extern TLGeom           g_tl_geom;

// Injected pointer/keyboard input for agent-driven UI (ui_input lever). The
// engine dispatcher forwards to this; the app implements it against ImGuiIO.
void app_ui_input_inject(const char* kind, float x, float y, int button,
                         float wheel, const char* text);
