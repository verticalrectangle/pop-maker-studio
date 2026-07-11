#pragma once
// studio_types.h — shared types for the studio screen split
// All other studio_*.cpp / panel_*.cpp files include this.

#include <imgui.h>
#include <vector>
#include <string>

// ── Panel view ────────────────────────────────────────────────────────────────
enum class PanelView {
    Clip, Typography, HostFX, HostAudioFX, Project, History, // tab-bar views
    LibBG, LibText, LibLyric, LibFX, LibAdj, LibAFX, LibVID, LibIMG, LibAUD, LibBFX, // library browsers
    LibBin,                                              // project bin (this-project media)
    OverrideFX, OverrideAdj, OverrideBG, OverrideAudioFX, OverrideMultiFX, OverrideAudioMultiFX, // clip-type overrides
    Filters,      // face filters for the selected camera brick
};

// ── Timeline layout constants ─────────────────────────────────────────────────
static constexpr float TL_LABEL_W     = 120.f;
static constexpr float TL_TRACK_H     = 42.f;
static constexpr float TL_RULER_H     = 34.f;  // loop-brace lane (top) + ticks/markers
static constexpr float TL_SCROLLBAR_H = 14.f;
static constexpr float TL_VSCROLLBAR_W = 10.f;

// ── Timeline drag/select state ────────────────────────────────────────────────
struct TlState {
    int drag_track=-1, drag_clip=-1; float drag_offset=0.f;
    bool drag_left=false, drag_right=false;
    int drag_hot_track=-1, drag_hot_gap=-1; bool drag_moved=false;
    float drag_origin_start=0.f, drag_origin_end=0.f;
    struct Origin { int ti, ci; float start, end; };
    std::vector<Origin> drag_multi;
    bool snap_enabled=true; float snap_indicator=-1.f;
    float body_snap_held_start=-1.f; float body_snap_held_cand=-1.f;
    int rename_track=-1; char rename_buf[64]={}; bool rename_focus=false;
    int track_drag_src=-1; bool track_dragging=false;
    float track_drag_start_y=0.f; int track_drag_insert=-1;
    bool box_selecting=false; ImVec2 box_start={0.f,0.f}; bool clip_hit=false;
    int ctx_track=-1, ctx_clip=-1;
    bool open_clip_ctx=false, open_track_ctx=false, open_tl_ctx=false;
    int trans_track=-1, trans_left_ci=-1;
    ImVec2 trans_popup_pos={0.f,0.f};
    int glass_drag=0; float glass_drag_ref_x=0.f;
    float glass_drag_ref_pre=0.f, glass_drag_ref_post=0.f;
    float glass_drag_ref_start=0.f;
    // FX timing-lane drag (glass-brick Pass 2): retime a single sub-effect's
    // active span by dragging its lane. mode: 0 none, 1 left edge, 2 right edge,
    // 3 body move. (ti,ci) = the MultiFX/AudioMultiFX brick, idx = fx_chain slot.
    int fx_lane_drag=0, fx_lane_ti=-1, fx_lane_ci=-1, fx_lane_idx=-1;
    float fx_lane_ref_x=0.f, fx_lane_ref_s=0.f, fx_lane_ref_e=0.f;
    bool  fx_lane_moved=false;
    bool ruler_drag=false;
    // FX brick (track, clip) to weld into — follows the mouse row, so
    // cross-track welds work. -1 = no candidate.
    int drag_merge_ti=-1, drag_merge_ci=-1;
    // Content clip (track, clip) a dragged FX brick is hovering to COUPLE onto,
    // when no FX-brick merge target exists — hold-to-weld onto bare content so
    // video/filter bricks weld by holding like audio ones do. -1 = none.
    int drag_couple_ti=-1, drag_couple_ci=-1;
    // Loop brace (ruler) drag: 0 none, 1 left edge, 2 right edge, 3 move body,
    // 4 create (drag out a new region).
    int   loop_drag=0;
    float loop_drag_anchor=0.f;                 // fixed edge while create/resize
    float loop_drag_ref_x=0.f, loop_drag_ref_in=0.f, loop_drag_ref_out=0.f;  // body move
    float loop_drag_down_x=0.f; bool loop_drag_moved=false;
    // Marker (locator) drag/rename in the ruler.
    int   marker_drag=-1;            // index of marker being dragged, -1 none
    bool  marker_drag_moved=false;   float marker_down_x=0.f;
    int   marker_rename=-1;          // index being renamed inline, -1 none
    bool  marker_rename_focus=false;
    char  marker_rename_buf[64]={};
    int   ctx_marker=-1;             // marker index for the right-click menu
    bool  open_marker_ctx=false;
};

// Defined in timeline.cpp
extern TlState g_tl;
