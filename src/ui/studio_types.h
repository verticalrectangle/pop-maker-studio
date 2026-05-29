#pragma once
// studio_types.h — shared types for the studio screen split
// All other studio_*.cpp / panel_*.cpp files include this.

#include <imgui.h>
#include <vector>
#include <string>

// ── Panel view ────────────────────────────────────────────────────────────────
enum class PanelView {
    Clip, Animation, Typography, Project, History,       // tab-bar views
    LibBG, LibFX, LibAdj, LibAFX, LibVID, LibIMG, LibAUD, LibBFX, // library browsers
    OverrideFX, OverrideAdj, OverrideBG, OverrideAudioFX, OverrideMultiFX, // clip-type overrides
};

// ── Timeline layout constants ─────────────────────────────────────────────────
static constexpr float TL_LABEL_W = 120.f;
static constexpr float TL_TRACK_H = 42.f;
static constexpr float TL_RULER_H = 24.f;

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
    bool ruler_drag=false;
};

// Defined in timeline.cpp
extern TlState g_tl;
