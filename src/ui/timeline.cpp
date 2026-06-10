#include "studio_types.h"
#include "studio_shared.h"
#include "timeline.h"
#include "pipeline.h"
#include "panel_clip.h"
#include "panel_animation.h"
#include "panel_fx.h"
#include "panel_media.h"
#include "app.h"
#include "audio.h"
#include "video.h"
#include "proxy.h"
#include "history.h"
#include "filepicker.h"
#include "waveform.h"
#include "bg_presets.h"
#include "theme.h"
#include "body_fx.h"
#include "bg_remove.h"
#include "render.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>
#include "json.hpp"

namespace fs = std::filesystem;
extern ImFont* g_font_bold;
extern ImFont* g_font_black;

// g_tl — timeline drag/select state (declared extern in studio_types.h)
TlState g_tl;

// Drop state — declared extern in timeline.h
int   s_tl_hover_track   = -1;
float s_drop_flash_t     = 0.f;
int   s_drop_flash_track = -1;

static bool clips_conflict(const Clip& a, const Clip& b) {
    if (a.clip_type == ClipType::Effect  || b.clip_type == ClipType::Effect  ||
        a.clip_type == ClipType::MultiFX || b.clip_type == ClipType::MultiFX ||
        a.clip_type == ClipType::BodyFX  || b.clip_type == ClipType::BodyFX) return false;
    return a.start < b.end && a.end > b.start;
}
static void merge_fx_clips(Clip& target, Clip dragged) {
    if (target.clip_type == ClipType::MultiFX) {
        if (dragged.clip_type == ClipType::MultiFX) {
            for (auto& se : dragged.fx_chain)
                target.fx_chain.push_back(se);
        } else {
            target.fx_chain.push_back(dragged);
        }
    } else {
        // target is Effect or BodyFX — promote to MultiFX
        Clip sub0 = target;
        sub0.clip_type = (target.clip_type == ClipType::BodyFX) ? ClipType::BodyFX : ClipType::Effect;
        sub0.rel_start = 0.f; sub0.rel_end = 0.f;
        target.clip_type = ClipType::MultiFX;
        target.fx_chain.clear();
        target.fx_chain.push_back(sub0);
        if (dragged.clip_type == ClipType::MultiFX) {
            for (auto& se : dragged.fx_chain)
                target.fx_chain.push_back(se);
        } else {
            Clip sub1 = dragged;
            sub1.rel_start = 0.f; sub1.rel_end = 0.f;
            target.fx_chain.push_back(sub1);
        }
        target.fx_chain_selected = 0;
    }
}

// ── Timeline ──────────────────────────────────────────────────────────────────

void draw_timeline(AppState& state, ImVec2 origin, float total_w, float total_h) {
    ImDrawList* dl      = ImGui::GetWindowDrawList();
    // Any open popup (clip/track context menus, transition picker, modals)
    // silences the timeline's raw hit-testing below: ImGui popups don't
    // intercept IsMouseClicked, so clicks on menu items — and the click that
    // dismisses a popup — were also landing on whatever sat underneath
    // (toggling track icons, deselecting, starting drags, opening another
    // menu). Drag continuation (IsMouseDown/Dragging/Released) stays live so
    // an in-flight drag still finishes if a popup opens mid-gesture.
    const bool tl_any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId |
                                                     ImGuiPopupFlags_AnyPopupLevel);
    float clip_area_w        = total_w - TL_LABEL_W - TL_VSCROLLBAR_W;
    state.tl_clip_area_w     = clip_area_w;
    float dur                = fmaxf(state.duration, 1.f);
    float& zoom         = state.tl_zoom;
    float& scroll       = state.tl_scroll;

    // Minimum zoom that fits the entire timeline in the visible clip area.
    // Recomputed every frame so it tracks window resize automatically.
    float zoom_min = fmaxf(1.f, clip_area_w / dur);
    state.tl_zoom_min = zoom_min;
    zoom = fmaxf(zoom, zoom_min);  // lift zoom up if window resized or duration shrank
    if (zoom <= zoom_min) scroll = 0.f;  // fully zoomed out → always show from frame 0

    // Deferred zoom-to-fit: set by add_clip_to_track / import whenever a new clip is added.
    // Always compute the target zoom for the clip; only apply it if it means zooming OUT
    // (new_zoom < current zoom). This means: if the user is already zoomed out enough to
    // see the full clip, nothing changes; if not, the timeline adjusts to show it with spacing.
    if (state.tl_zoom_to_fit_end > 0.f && clip_area_w > 0.f) {
        float target   = state.tl_zoom_to_fit_end * 1.15f;
        float new_zoom = fmaxf(zoom_min, fminf(clip_area_w / target, 4000.f));
        if (new_zoom < zoom) {
            float left_t = scroll / zoom;
            zoom   = new_zoom;
            scroll = fmaxf(0.f, left_t * new_zoom);
        }
        state.tl_zoom_to_fit_end = 0.f;
    }

    float tl_content_w  = dur * zoom;

    // Scroll to bring selected clip into view (triggered by preview click-to-select)
    if (state.request_scroll_to_clip &&
        state.selected_track >= 0 && state.selected_clip >= 0 &&
        state.selected_track < (int)state.tracks.size()) {
        state.request_scroll_to_clip = false;
        auto& cl = state.tracks[state.selected_track].clips[state.selected_clip];
        // Horizontal: ensure clip start is visible with some margin
        float clip_px  = cl.start * zoom;
        float clip_px1 = cl.end   * zoom;
        float margin   = 40.f;
        if (clip_px - scroll < margin)
            scroll = fmaxf(0.f, clip_px - margin);
        else if (clip_px1 - scroll > clip_area_w - margin)
            scroll = clip_px1 - clip_area_w + margin;
        // Vertical: ensure track row is visible
        float track_top = state.selected_track * TL_TRACK_H;
        float vis_h     = total_h - TL_RULER_H - TL_SCROLLBAR_H;
        if (track_top < state.tl_v_scroll)
            state.tl_v_scroll = track_top;
        else if (track_top + TL_TRACK_H > state.tl_v_scroll + vis_h)
            state.tl_v_scroll = track_top + TL_TRACK_H - vis_h;
    }

    // Tick drop flash timer
    if (s_drop_flash_t > 0.f)
        s_drop_flash_t -= ImGui::GetIO().DeltaTime;

    ImVec2 mouse = ImGui::GetIO().MousePos;
    bool in_tl = mouse.x >= origin.x && mouse.x <= origin.x + total_w &&
                 mouse.y >= origin.y && mouse.y <= origin.y + total_h;

    if (in_tl) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (fabsf(wheel) > 0.f) {
            bool in_track_body = mouse.y >= origin.y + TL_RULER_H && mouse.y < origin.y + total_h - TL_SCROLLBAR_H;
            if (ImGui::GetIO().KeyCtrl) {
                // Ctrl+scroll = zoom (anchor under cursor)
                float old_zoom = zoom;
                zoom = fmaxf(zoom_min, fminf(zoom * (1.f + wheel * 0.1f), 4000.f));
                float mouse_t = (mouse.x - origin.x - TL_LABEL_W + scroll) / old_zoom;
                scroll = fmaxf(0.f, mouse_t * zoom - (mouse.x - origin.x - TL_LABEL_W));
                scroll = fminf(scroll, fmaxf(0.f, dur * zoom - clip_area_w + 60.f));
            } else if (!in_track_body) {
                // Plain scroll in ruler/header = horizontal pan only
                scroll = fmaxf(0.f, scroll - wheel * 60.f);
                scroll = fminf(scroll, fmaxf(0.f, tl_content_w - clip_area_w + 60.f));
            }
            // Plain scroll in track body is handled by the vertical scroll block below
        }
    }

    // Background
    dl->AddRectFilled(origin, {origin.x+total_w, origin.y+total_h}, to_u32(Col::bg));
    dl->AddRect(origin, {origin.x+total_w, origin.y+total_h}, to_u32(Col::line));

    // ── Fps + frame snap helper ───────────────────────────────────────────────
    float fps = (state.proxy_ready && video_info(0).fps > 0.0)
                ? (float)video_info(0).fps : 30.f;
    // snap(t): round t to nearest frame unless Ctrl is held
    auto snap = [&](float t) -> float {
        if (ImGui::GetIO().KeyCtrl || fps <= 0.f) return t;
        return roundf(t * fps) / fps;
    };

    // ── Edge / playhead snapping ──────────────────────────────────────────────

    // Build candidate list (all clip edges + playhead), excluding one clip.
    auto build_snap_candidates = [&](int ex_ti, int ex_ci) -> std::vector<float> {
        std::vector<float> cands;
        cands.push_back(state.playhead);
        cands.push_back(0.f);
        for (int ti = 0; ti < (int)state.tracks.size(); ++ti)
            for (int ci = 0; ci < (int)state.tracks[ti].clips.size(); ++ci) {
                if (ti == ex_ti && ci == ex_ci) continue;
                cands.push_back(state.tracks[ti].clips[ci].start);
                cands.push_back(state.tracks[ti].clips[ci].end);
            }
        for (float bt : state.beats) cands.push_back(bt);
        return cands;
    };

    // Snap state aliases (needed before the per-track statics block below)
    auto& s_snap_enabled   = g_tl.snap_enabled;
    auto& s_snap_indicator = g_tl.snap_indicator;

    // Try to snap t to a nearby candidate; returns snapped value and sets indicator.
    // threshold_px: snap radius in screen pixels.
    const float SNAP_PX = 8.f;
    auto edge_snap = [&](float t, const std::vector<float>& cands) -> float {
        if (!s_snap_enabled || ImGui::GetIO().KeyCtrl) { s_snap_indicator = -1.f; return t; }
        float best_dt = SNAP_PX / zoom;
        float result  = t;
        for (float c : cands) {
            float dt = fabsf(c - t);
            if (dt < best_dt) { best_dt = dt; result = c; }
        }
        s_snap_indicator = (result != t) ? result : -1.f;
        return result;
    };

    // ── Ruler ─────────────────────────────────────────────────────────────────
    float ruler_y = origin.y;
    dl->AddRectFilled({origin.x, ruler_y},
        {origin.x+total_w, ruler_y+TL_RULER_H}, to_u32(Col::bg_soft));
    dl->AddLine({origin.x, ruler_y+TL_RULER_H},
        {origin.x+total_w, ruler_y+TL_RULER_H}, to_u32(Col::line));

    // Snap toggle button in label column of ruler
    {
        const char* lbl = s_snap_enabled ? "⊿ Snap" : "  Snap";
        ImVec2 btn_min = {origin.x + 4.f, ruler_y + 2.f};
        ImVec2 btn_max = {origin.x + TL_LABEL_W - 4.f, ruler_y + TL_RULER_H - 2.f};
        bool hov = mouse.x >= btn_min.x && mouse.x <= btn_max.x &&
                   mouse.y >= btn_min.y && mouse.y <= btn_max.y;
        ImU32 btn_col = s_snap_enabled
            ? IM_COL32(120, 200, 255, hov ? 180 : 120)
            : to_u32(hov ? Col::line_hover : Col::line);
        dl->AddRectFilled(btn_min, btn_max, IM_COL32(0,0,0,0));
        dl->AddRect(btn_min, btn_max, btn_col, 3.f);
        ImU32 txt_col = s_snap_enabled
            ? IM_COL32(120, 200, 255, 255)
            : to_u32(Col::muted);
        float tw = ImGui::CalcTextSize(lbl).x;
        float th = ImGui::CalcTextSize(lbl).y;
        dl->AddText({btn_min.x + ((btn_max.x-btn_min.x)-tw)*0.5f,
                     btn_min.y + ((btn_max.y-btn_min.y)-th)*0.5f}, txt_col, lbl);
        if (hov && (!tl_any_popup && ImGui::IsMouseClicked(0)))
            s_snap_enabled = !s_snap_enabled;
    }

    // Adaptive tick ladder {major_interval_secs, subdivisions}.
    // Selected so that major ticks are always ≥ 80 px apart.
    struct TickLevel { float secs; int subdivs; };
    const float f1 = 1.f / fps;
    const TickLevel levels[] = {
        {f1,      1},   // 1 frame
        {2*f1,    2},   // 2 frames,  minor every 1f
        {5*f1,    5},   // 5 frames,  minor every 1f
        {10*f1,   2},   // 10 frames, minor every 5f
        {0.5f,    5},   // 0.5 s,     minor every 0.1 s
        {1.f,     4},   // 1 s,       minor every 0.25 s
        {2.f,     4},   // 2 s,       minor every 0.5 s
        {5.f,     5},   // 5 s,       minor every 1 s
        {10.f,    2},   // 10 s,      minor every 5 s
        {30.f,    3},   // 30 s,      minor every 10 s
        {60.f,    4},   // 1 min,     minor every 15 s
        {300.f,   5},   // 5 min,     minor every 1 min
        {600.f,   2},   // 10 min,    minor every 5 min
    };
    const int NUM_LEVELS = (int)(sizeof(levels)/sizeof(levels[0]));
    const float MIN_MAJOR_PX = 80.f;

    TickLevel chosen = levels[NUM_LEVELS-1];
    for (int li = 0; li < NUM_LEVELS; ++li) {
        if (levels[li].secs * zoom >= MIN_MAJOR_PX) { chosen = levels[li]; break; }
    }

    float minor_secs = chosen.secs / (float)chosen.subdivs;
    float first_tick = floorf((scroll / zoom) / minor_secs) * minor_secs;

    for (float t = first_tick; t <= dur + chosen.secs; t += minor_secs) {
        float px = origin.x + TL_LABEL_W + t * zoom - scroll;
        if (px < origin.x + TL_LABEL_W - 1.f || px > origin.x + total_w) continue;

        int   tick_idx = (int)roundf(t / minor_secs);
        bool  is_major = (tick_idx % chosen.subdivs == 0);

        if (is_major) {
            dl->AddLine({px, ruler_y + 4.f}, {px, ruler_y + TL_RULER_H},
                        to_u32(Col::muted));
            char tbuf[16];
            if (chosen.secs >= 1.f)
                snprintf(tbuf, sizeof(tbuf), "%s", fmt_time_short(t).c_str());
            else
                snprintf(tbuf, sizeof(tbuf), "%d", (int)roundf(t * fps));
            float tw = ImGui::CalcTextSize(tbuf).x;
            float lx = px - tw * 0.5f;
            if (lx >= origin.x + TL_LABEL_W + 2.f && lx + tw <= origin.x + total_w - 2.f)
                dl->AddText({lx, ruler_y + 3.f}, to_u32(Col::muted), tbuf);
        } else {
            dl->AddLine({px, ruler_y + 10.f}, {px, ruler_y + TL_RULER_H},
                        to_u32(Col::dim));
        }
    }

    // Beat tick marks (small orange triangles at the bottom of the ruler)
    if (!state.beats.empty()) {
        ImU32 beat_col = IM_COL32(255, 160, 50, 180);
        float ty = ruler_y + TL_RULER_H - 5.f;
        for (float bt : state.beats) {
            float px = origin.x + TL_LABEL_W + bt * zoom - scroll;
            if (px < origin.x + TL_LABEL_W || px > origin.x + total_w) continue;
            dl->AddTriangleFilled({px-3.f, ty}, {px+3.f, ty}, {px, ty+5.f}, beat_col);
        }
    }

    // Chapter marker lines — colored vertical lines + labels in the ruler
    for (auto& m : state.markers) {
        float px = origin.x + TL_LABEL_W + m.time * zoom - scroll;
        if (px < origin.x + TL_LABEL_W || px > origin.x + total_w) continue;
        ImU32 mc = (m.color & 0x00FFFFFFu) | 0xCC000000u;  // use stored RGB, force alpha=0xCC
        dl->AddLine({px, ruler_y}, {px, origin.y + total_h}, mc, 1.5f);
        if (!m.label.empty()) {
            ImVec2 tp = {px + 3.f, ruler_y + 2.f};
            dl->AddText(tp, mc, m.label.c_str());
        }
    }

    // Tracks
    // Vertical scroll: mouse wheel in the track body area
    float track_area_top = origin.y + TL_RULER_H;
    float track_area_bot = origin.y + total_h - TL_SCROLLBAR_H;
    float tracks_total_h = ((int)state.tracks.size() + 1) * TL_TRACK_H;  // +1 for add-track row
    float max_v_scroll   = fmaxf(0.f, tracks_total_h - (track_area_bot - track_area_top));
    if (ImGui::IsMouseHoveringRect({origin.x, track_area_top}, {origin.x+total_w, track_area_bot})) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f && !ImGui::GetIO().KeyCtrl)
            state.tl_v_scroll -= wheel * TL_TRACK_H;
    }
    state.tl_v_scroll = fmaxf(0.f, fminf(max_v_scroll, state.tl_v_scroll));

    float track_y = track_area_top - state.tl_v_scroll;
    s_tl_hover_track = -1;  // reset each frame; set below as we scan rows

    // Unpack g_tl into short aliases for readability inside this function
    auto& drag_track       = g_tl.drag_track;
    auto& drag_clip        = g_tl.drag_clip;
    auto& drag_offset      = g_tl.drag_offset;
    auto& drag_left        = g_tl.drag_left;
    auto& drag_right       = g_tl.drag_right;
    auto& drag_hot_track   = g_tl.drag_hot_track;
    auto& drag_hot_gap     = g_tl.drag_hot_gap;
    auto& s_drag_moved     = g_tl.drag_moved;
    auto& drag_origin_start= g_tl.drag_origin_start;
    auto& drag_origin_end  = g_tl.drag_origin_end;
    auto& s_body_snap_held_start = g_tl.body_snap_held_start;
    auto& s_body_snap_held_cand  = g_tl.body_snap_held_cand;
    auto& s_rename_track   = g_tl.rename_track;
    auto& s_rename_buf     = g_tl.rename_buf;
    auto& s_rename_focus   = g_tl.rename_focus;
    auto& s_track_drag_src = g_tl.track_drag_src;
    auto& s_track_dragging = g_tl.track_dragging;
    auto& s_track_drag_start_y = g_tl.track_drag_start_y;
    auto& s_track_drag_insert  = g_tl.track_drag_insert;
    auto& s_box_selecting  = g_tl.box_selecting;
    auto& s_box_start      = g_tl.box_start;
    auto& s_clip_hit       = g_tl.clip_hit;
    auto& ctx_track        = g_tl.ctx_track;
    auto& ctx_clip         = g_tl.ctx_clip;
    auto& open_clip_ctx    = g_tl.open_clip_ctx;
    auto& open_track_ctx   = g_tl.open_track_ctx;
    auto& open_tl_ctx      = g_tl.open_tl_ctx;
    auto& s_trans_track    = g_tl.trans_track;
    auto& s_trans_left_ci  = g_tl.trans_left_ci;
    auto& s_trans_popup_pos= g_tl.trans_popup_pos;
    auto& s_glass_drag     = g_tl.glass_drag;
    auto& s_glass_drag_ref_x    = g_tl.glass_drag_ref_x;
    auto& s_glass_drag_ref_pre  = g_tl.glass_drag_ref_pre;
    auto& s_glass_drag_ref_post = g_tl.glass_drag_ref_post;
    auto& s_glass_drag_ref_start= g_tl.glass_drag_ref_start;
    bool s_trans_hit_this_frame = false;

    // Clip all track drawing to the scrollable area (below ruler, above add-track row).
    // Use ImGui::PushClipRect (not dl->) so the window's ClipRect is updated too:
    // ImGui::PopClipRect inside clip labels restores the window ClipRect from the
    // draw-list stack top, and a dl-only push would leave the window ClipRect stuck
    // at the track area for the rest of the frame — which then makes the scrollbar's
    // IsMouseHoveringRect always return false because its rect is below track_area_bot.
    ImGui::PushClipRect({origin.x, track_area_top}, {origin.x+total_w-TL_VSCROLLBAR_W, track_area_bot}, true);

    for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
        Track& track = state.tracks[ti];
        ImVec2 row_tl = {origin.x, track_y};
        ImVec2 row_br = {origin.x+total_w, track_y+TL_TRACK_H};
        bool row_hov = mouse.y >= row_tl.y && mouse.y < row_br.y;
        if (row_hov) s_tl_hover_track = ti;

        dl->AddRectFilled(row_tl, row_br,
            to_u32(row_hov ? Col::bg_soft_hov : Col::bg_soft));
        dl->AddLine({origin.x, row_br.y}, {origin.x+total_w, row_br.y}, to_u32(Col::line));

        // FX preset drag-drop target on the clip area of this row.
        // Clamp to track_area_bot so the button doesn't overlap (and swallow
        // clicks on) the horizontal scrollbar below.
        {
            ImVec2 drop_tl = {origin.x + TL_LABEL_W, track_y};
            ImVec2 drop_br = {origin.x + total_w,
                              fminf(track_y + TL_TRACK_H, track_area_bot)};
            if (drop_br.y > drop_tl.y) {
            ImGui::SetCursorScreenPos(drop_tl);
            ImGui::InvisibleButton(("##fxdrop" + std::to_string(ti)).c_str(),
                                   {drop_br.x - drop_tl.x, drop_br.y - drop_tl.y});
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("FX_PRESET")) {
                    int idx = *(const int*)pay->Data;
                    const EffectPreset* preset = nullptr;
                    int bn = (int)g_builtin_presets.size();
                    if (idx < bn) preset = &g_builtin_presets[idx];
                    else if (idx - bn < (int)state.user_presets.size())
                        preset = &state.user_presets[idx - bn];
                    if (preset) {
                        float drop_t = fmaxf(0.f, (ImGui::GetMousePos().x - (origin.x + TL_LABEL_W) + scroll) / zoom);
                        Clip cl;
                        cl.clip_type = ClipType::Effect;
                        cl.start     = drop_t;
                        cl.end       = drop_t + 2.f;
                        preset_apply(cl, *preset);
                        state.tracks[ti].clips.push_back(cl);
                        state.selected_track = ti;
                        state.selected_clip  = (int)state.tracks[ti].clips.size() - 1;
                        s_drop_flash_track = ti;
                        s_drop_flash_t     = 0.6f;
                        history_push(state, "Drop Effect preset: " + preset->name);
                    }
                }
                if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("BG_PRESET")) {
                    const char* preset_id = (const char*)pay->Data;
                    const BgPreset* pr = bg_preset_by_id(preset_id);
                    if (pr) {
                        float drop_t = fmaxf(0.f, (ImGui::GetMousePos().x - (origin.x + TL_LABEL_W) + scroll) / zoom);
                        float proj_dur = 7.f;
                        Clip cl;
                        cl.clip_type    = ClipType::Background;
                        cl.text         = preset_id;
                        cl.start        = drop_t;
                        cl.end          = drop_t + proj_dur;
                        cl.bg_speed     = pr->default_speed;
                        cl.bg_intensity = 0.85f;
                        memcpy(cl.bg_c1, pr->dc1, sizeof(float)*4);
                        memcpy(cl.bg_c2, pr->dc2, sizeof(float)*4);
                        memcpy(cl.bg_c3, pr->dc3, sizeof(float)*4);
                        state.tracks[ti].clips.push_back(cl);
                        state.selected_track = ti;
                        state.selected_clip  = (int)state.tracks[ti].clips.size() - 1;
                        s_drop_flash_track = ti;
                        s_drop_flash_t     = 0.6f;
                        history_push(state, std::string("Drop Background: ") + pr->label);
                    }
                }
                if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("FX_CREATIVE")) {
                    FXType ft = (FXType)*(const int*)pay->Data;
                    float drop_t = fmaxf(0.f, (ImGui::GetMousePos().x - (origin.x + TL_LABEL_W) + scroll) / zoom);
                    Clip cl;
                    cl.clip_type = ClipType::Effect;
                    cl.fx_type   = ft;
                    cl.start     = drop_t;
                    cl.end       = drop_t + 5.f;
                    state.tracks[ti].clips.push_back(cl);
                    state.selected_track = ti;
                    state.selected_clip  = (int)state.tracks[ti].clips.size() - 1;
                    s_drop_flash_track = ti;
                    s_drop_flash_t     = 0.6f;
                    history_push(state, std::string("Drop FX: ") + fx_type_name(ft));
                }
                auto accept_media_drop = [&](const char* ptype) {
                    if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload(ptype)) {
                        std::string path((const char*)pay->Data, pay->DataSize - 1);
                        bool img = is_image_path(path);
                        float drop_t = fmaxf(0.f, (ImGui::GetMousePos().x - (origin.x + TL_LABEL_W) + scroll) / zoom);
                        float dur = img ? 5.f : video_probe_duration(path);
                        if (dur <= 0.f) dur = 4.f;
                        Clip cl; cl.clip_type = ClipType::Video; cl.text = path;
                        cl.source_id = path; cl.start = drop_t; cl.end = drop_t + dur;
                        s_source_durations[path] = dur;
                        state.tracks[ti].clips.push_back(cl);
                        state.selected_track = ti;
                        state.selected_clip  = (int)state.tracks[ti].clips.size() - 1;
                        s_drop_flash_track = ti; s_drop_flash_t = 0.6f;
                        proxy_start(path);
                        int slot = slot_for_video(state, clip_slot_key(path, drop_t), path);
                        if (slot >= 0) video_open_still(slot, proxy_still_path(path));
                        state.video_loaded = true;
                        recent_media_push(path, img ? MediaKind::Image : MediaKind::Video);
                        history_push(state, (img ? "Drop image: " : "Drop video: ") +
                                     fs::path(path).filename().string());
                    }
                };
                accept_media_drop("MEDIA_VID");
                accept_media_drop("MEDIA_IMG");
                if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("MEDIA_AUD")) {
                    std::string path((const char*)pay->Data, pay->DataSize - 1);
                    float drop_t = fmaxf(0.f, (ImGui::GetMousePos().x - (origin.x + TL_LABEL_W) + scroll) / zoom);
                    AudioMeta meta{}; float dur = audio_probe(path, meta) ? meta.duration_secs : 4.f;
                    if (dur <= 0.f) dur = 4.f;
                    Clip cl; cl.clip_type = ClipType::Audio; cl.text = path;
                    cl.source_id = path; cl.start = drop_t; cl.end = drop_t + dur;
                    s_source_durations[path] = dur;
                    state.tracks[ti].clips.push_back(cl);
                    state.selected_track = ti;
                    state.selected_clip  = (int)state.tracks[ti].clips.size() - 1;
                    s_drop_flash_track = ti; s_drop_flash_t = 0.6f;
                    audio_source_ensure(path);
                    recent_media_push(path, MediaKind::Audio);
                    history_push(state, "Drop audio: " + fs::path(path).filename().string());
                }
                // Highlight the row while dragging over it
                dl->AddRectFilled(drop_tl, drop_br, IM_COL32(180,130,255,40));
                dl->AddRect(drop_tl, drop_br, IM_COL32(180,130,255,180), 2.f);
                ImGui::EndDragDropTarget();
            }
            }
        }

        // Drop flash — briefly illuminate the row that just received a drop.
        if (s_drop_flash_t > 0.f && s_drop_flash_track == ti) {
            float alpha = s_drop_flash_t / 0.6f;  // 1→0 linear fade
            // Pulse: quick bright flash then fade
            float pulse = alpha > 0.7f ? (1.f - alpha) / 0.3f : 1.f;
            ImU32 flash_col = IM_COL32(120, 200, 255, (int)(pulse * alpha * 120));
            dl->AddRectFilled(row_tl, row_br, flash_col, 2.f);
            dl->AddRect(row_tl, row_br, IM_COL32(120, 200, 255, (int)(alpha * 200)), 2.f);
        }

        // Cross-track drag ghost — show target landing zone
        if (drag_hot_track == ti && drag_track >= 0 && drag_clip >= 0 &&
            drag_hot_track != drag_track && !drag_left && !drag_right &&
            drag_track < (int)state.tracks.size() &&
            drag_clip  < (int)state.tracks[drag_track].clips.size()) {
            const Clip& gc = state.tracks[drag_track].clips[drag_clip];
            float gx0 = fmaxf(origin.x+TL_LABEL_W, origin.x+TL_LABEL_W+gc.start*zoom-scroll);
            float gx1 = fminf(origin.x+total_w,     origin.x+TL_LABEL_W+gc.end*zoom-scroll);
            float gy0 = track_y+3.f, gy1 = track_y+TL_TRACK_H-3.f;
            if (gx1 > gx0) {
                dl->AddRectFilled({gx0,gy0},{gx1,gy1}, IM_COL32(255,255,255,35), 2.f);
                dl->AddRect({gx0,gy0},{gx1,gy1}, IM_COL32(255,255,255,160), 2.f, 0, 1.5f);
            }
        }

        // Track label — single click selects, double-click renames
        bool track_sel = state.selected_track == ti;
        bool in_label = mouse.x >= origin.x+2.f && mouse.x < origin.x+TL_LABEL_W-54.f &&
                        mouse.y >= track_y && mouse.y < track_y+TL_TRACK_H;

        if (s_rename_track == ti) {
            // Inline rename field
            ImGui::SetCursorScreenPos({origin.x+4.f, track_y+(TL_TRACK_H-18.f)*0.5f});
            ImGui::SetNextItemWidth(TL_LABEL_W - 26.f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
            ImGui::PushStyleColor(ImGuiCol_Border,  Col::fg);
            if (s_rename_focus) { ImGui::SetKeyboardFocusHere(); s_rename_focus = false; }
            bool committed = ImGui::InputText("##rename", s_rename_buf, sizeof(s_rename_buf),
                ImGuiInputTextFlags_EnterReturnsTrue);
            if (committed || (ImGui::IsItemDeactivated() && !ImGui::IsKeyPressed(ImGuiKey_Escape))) {
                std::string newname(s_rename_buf);
                if (!newname.empty() && newname != track.name) {
                    track.name = newname;
                    history_push(state, "Rename track");
                }
                s_rename_track = -1;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) s_rename_track = -1;
            ImGui::PopStyleColor(2);
        } else {
            // Managed track: amber left accent stripe
            if (track.managed)
                dl->AddRectFilled({origin.x, track_y+2.f},
                                  {origin.x+3.f, track_y+TL_TRACK_H-2.f},
                                  to_u32(Col::clip_lyrics));
            // Truncate with ellipsis if the name would overflow into the
            // icon buttons (lock at TL_LABEL_W-45, then mute, eye).
            float max_w = TL_LABEL_W - 8.f - 54.f;
            std::string label = track.name;
            if (ImGui::CalcTextSize(label.c_str()).x > max_w) {
                const char* ell = "\xE2\x80\xA6";  // U+2026
                float ell_w = ImGui::CalcTextSize(ell).x;
                while (!label.empty() &&
                       ImGui::CalcTextSize(label.c_str()).x + ell_w > max_w) {
                    // pop one UTF-8 code point off the end
                    size_t n = label.size();
                    while (n > 0 && (label[n-1] & 0xC0) == 0x80) --n;
                    if (n > 0) --n;
                    label.resize(n);
                }
                label += ell;
            }
            dl->AddText({origin.x+8.f, track_y+(TL_TRACK_H-13.f)*0.5f},
                to_u32(track_sel ? Col::fg : Col::muted), label.c_str());
            if ((!tl_any_popup && ImGui::IsMouseClicked(0)) && in_label) {
                state.selected_track  = ti;
                state.selected_clip   = -1;
                state.clip_selection.clear();
                s_track_drag_src      = ti;
                s_track_drag_start_y  = mouse.y;
                s_track_dragging      = false;
            }
            if ((!tl_any_popup && ImGui::IsMouseDoubleClicked(0)) && in_label && !s_track_dragging) {
                s_track_drag_src = -1;
                s_rename_track = ti;
                strncpy(s_rename_buf, track.name.c_str(), sizeof(s_rename_buf)-1);
                s_rename_buf[sizeof(s_rename_buf)-1] = '\0';
                s_rename_focus = true;
            }
        }

        // Track icon buttons — lock · mute · eye — right side of label
        float btn_y  = track_y + TL_TRACK_H * 0.5f;
        // Eye button
        ImVec2 eye_c = {origin.x + TL_LABEL_W - 13.f, btn_y};
        {
            bool hov = fabsf(mouse.x-eye_c.x)<8.f && fabsf(mouse.y-eye_c.y)<8.f;
            ImU32 ecol = to_u32(track.visible ? Col::fg : Col::dim);
            dl->AddCircle(eye_c, 4.5f, ecol, 12, 1.2f);
            dl->AddCircleFilled(eye_c, 1.8f, ecol);
            if (!track.visible) {  // strike-through when hidden
                dl->AddLine({eye_c.x-5.f, eye_c.y-4.f}, {eye_c.x+5.f, eye_c.y+4.f},
                            to_u32(Col::dim), 1.5f);
            }
            if (hov && (!tl_any_popup && ImGui::IsMouseClicked(0))) {
                track.visible = !track.visible;
                history_push(state, "Toggle track visibility");
            }
        }
        // Lock button (padlock icon)
        ImVec2 lock_c = {origin.x + TL_LABEL_W - 45.f, btn_y};
        {
            bool hov  = fabsf(mouse.x-lock_c.x)<8.f && fabsf(mouse.y-lock_c.y)<8.f;
            ImU32 lc  = track.locked ? IM_COL32(255,200,60,240) : to_u32(Col::dim);
            // Shackle arc
            dl->AddRect({lock_c.x-3.f, lock_c.y-1.f}, {lock_c.x+3.f, lock_c.y+4.f}, lc, 1.f, 0, 1.2f);
            if (track.locked)  // closed shackle
                dl->AddRect({lock_c.x-3.f, lock_c.y-4.f}, {lock_c.x+3.f, lock_c.y}, lc, 2.f,
                    ImDrawFlags_RoundCornersTop, 1.2f);
            else               // open shackle — right side lifted
                dl->AddBezierQuadratic({lock_c.x-3.f, lock_c.y-1.f},
                    {lock_c.x-3.f, lock_c.y-5.f}, {lock_c.x+3.f, lock_c.y-5.f}, lc, 1.2f);
            if (hov && (!tl_any_popup && ImGui::IsMouseClicked(0))) {
                track.locked = !track.locked;
                history_push(state, track.locked ? "Lock track" : "Unlock track");
            }
        }
        // Mute button (speaker icon)
        ImVec2 mut_c = {origin.x + TL_LABEL_W - 29.f, btn_y};
        {
            bool hov = fabsf(mouse.x-mut_c.x)<8.f && fabsf(mouse.y-mut_c.y)<8.f;
            ImU32 mcol = track.muted ? IM_COL32(255,80,80,230) : to_u32(Col::dim);
            // Simple speaker: small filled rect + triangle
            dl->AddRectFilled({mut_c.x-4.f, mut_c.y-2.5f}, {mut_c.x-1.f, mut_c.y+2.5f}, mcol);
            dl->AddTriangleFilled({mut_c.x-1.f, mut_c.y-4.f},
                                  {mut_c.x-1.f, mut_c.y+4.f},
                                  {mut_c.x+4.f, mut_c.y}, mcol);
            if (track.muted) {  // X lines when muted
                dl->AddLine({mut_c.x+2.f, mut_c.y-3.f}, {mut_c.x+6.f, mut_c.y+3.f},
                            IM_COL32(255,80,80,230), 1.5f);
                dl->AddLine({mut_c.x+6.f, mut_c.y-3.f}, {mut_c.x+2.f, mut_c.y+3.f},
                            IM_COL32(255,80,80,230), 1.5f);
            }
            if (hov && (!tl_any_popup && ImGui::IsMouseClicked(0))) {
                track.muted = !track.muted;
                history_push(state, "Toggle track mute");
            }
        }

        dl->AddLine({origin.x+TL_LABEL_W, track_y},
                    {origin.x+TL_LABEL_W, track_y+TL_TRACK_H}, to_u32(Col::line));

        // Right-click track label
        if ((!tl_any_popup && ImGui::IsMouseClicked(1)) &&
            mouse.x >= origin.x && mouse.x < origin.x+TL_LABEL_W &&
            mouse.y >= track_y  && mouse.y < track_y+TL_TRACK_H) {
            ctx_track = ti; ctx_clip = -1;
            open_track_ctx = true;
            ImGui::OpenPopup("##track_ctx");
        }

        // Clips — two passes so FX bricks render on top of video clips.
        // Interaction lambda runs for both passes; FX pass runs second so
        // FX selection takes priority over video clips when they overlap.
        bool clip_ctx_opened_this_frame = false;

        auto clip_interact = [&](int ci, Clip& clip, float vis_x0, float vis_x1, float cy0, float cy1, bool sel) {
            // Clamp the Y range used for mouse hit-testing to the track area.
            // Without this, clips on tracks partially clipped at the bottom
            // extend cy1 past track_area_bot and steal scrollbar clicks.
            cy1 = std::min(cy1, track_area_bot);
            const float ew = 6.f, ew_hit = 12.f;
            // Edge handles
            if (sel) {
                dl->AddRectFilled({vis_x0,cy0},{vis_x0+ew,cy1},to_u32(Col::muted),1.f);
                dl->AddRectFilled({vis_x1-ew,cy0},{vis_x1,cy1},to_u32(Col::muted),1.f);
            }
            // Resize cursor
            {
                float orig_cx0h = origin.x+TL_LABEL_W+clip.start*zoom-scroll;
                float orig_cx1h = origin.x+TL_LABEL_W+clip.end*zoom-scroll;
                bool in_clip = mouse.y>=cy0 && mouse.y<=cy1 &&
                               mouse.x>=vis_x0 && mouse.x<=vis_x1;
                float clip_screen_wh = orig_cx1h - orig_cx0h;
                if (in_clip && clip_screen_wh > 2.f * ew_hit &&
                    (mouse.x <= orig_cx0h+ew_hit || mouse.x >= orig_cx1h-ew_hit))
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            // Keyframe diamond markers
            if (!clip.ktracks.empty()) {
                float kf_mid_y = (cy0 + cy1) * 0.5f;
                float d = 4.f;
                for (auto& [pname, pt] : clip.ktracks) {
                    for (auto& kf : pt.keys) {
                        float kx = origin.x + TL_LABEL_W + (clip.start + kf.time) * zoom - scroll;
                        if (kx < vis_x0 || kx > vis_x1) continue;
                        bool kf_sel = (state.kf_sel_track == ti &&
                                       state.kf_sel_clip  == ci &&
                                       state.kf_sel_prop  == pname &&
                                       &kf == &pt.keys[state.kf_sel_idx > 0 &&
                                           state.kf_sel_idx < (int)pt.keys.size()
                                           ? state.kf_sel_idx : 0]);
                        ImU32 kc = kf_sel ? IM_COL32(255,200,60,255) : IM_COL32(220,220,220,200);
                        dl->AddQuadFilled(
                            {kx, kf_mid_y-d}, {kx+d, kf_mid_y},
                            {kx, kf_mid_y+d}, {kx-d, kf_mid_y}, kc);
                        if ((!tl_any_popup && ImGui::IsMouseClicked(0)) &&
                            fabsf(mouse.x - kx) < d+2.f &&
                            fabsf(mouse.y - kf_mid_y) < d+2.f) {
                            state.kf_sel_track = ti;
                            state.kf_sel_clip  = ci;
                            state.kf_sel_prop  = pname;
                            int idx = (int)(&kf - pt.keys.data());
                            state.kf_sel_idx   = idx;
                            state.selected_track = ti;
                            state.selected_clip  = ci;
                        }
                    }
                }
            }
            // Left click — select / drag
            bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
            if (!s_trans_hit_this_frame && !any_popup && (!tl_any_popup && ImGui::IsMouseClicked(0))) {
                if (mouse.y>=cy0 && mouse.y<=cy1 && mouse.x>=vis_x0 && mouse.x<=vis_x1) {
                    s_clip_hit = true;
                    auto key = std::make_pair(ti, ci);
                    bool ctrl  = ImGui::GetIO().KeyCtrl;
                    bool shift = ImGui::GetIO().KeyShift;
                    if (ctrl) {
                        if (state.clip_selection.count(key))
                            state.clip_selection.erase(key);
                        else
                            state.clip_selection.insert(key);
                    } else if (shift && state.selected_track >= 0 && state.selected_clip >= 0) {
                        struct TL { float start; int ti, ci; };
                        std::vector<TL> all;
                        for (int t2=0; t2<(int)state.tracks.size(); ++t2)
                            for (int c2=0; c2<(int)state.tracks[t2].clips.size(); ++c2)
                                all.push_back({state.tracks[t2].clips[c2].start, t2, c2});
                        std::sort(all.begin(), all.end(), [](const TL& a, const TL& b){ return a.start < b.start; });
                        float t_focus = state.tracks[state.selected_track].clips[state.selected_clip].start;
                        float t_click = clip.start;
                        float t_lo = fminf(t_focus, t_click), t_hi = fmaxf(t_focus, t_click);
                        for (auto& e : all)
                            if (e.start >= t_lo && e.start <= t_hi)
                                state.clip_selection.insert({e.ti, e.ci});
                    } else {
                        state.clip_selection.clear();
                        state.clip_selection.insert(key);
                    }
                    state.selected_track = ti;
                    state.selected_clip  = ci;
                    strncpy(s_edit_buf, clip.text.c_str(), sizeof(s_edit_buf)-1);
                    s_edit_buf[sizeof(s_edit_buf)-1] = '\0';
                    s_edit_focus_next = (clip.clip_type==ClipType::Text || clip.clip_type==ClipType::Lyrics ||
                                         clip.clip_type==ClipType::Subtitle);
                    float orig_cx0 = origin.x+TL_LABEL_W+clip.start*zoom-scroll;
                    float orig_cx1 = origin.x+TL_LABEL_W+clip.end*zoom-scroll;
                    if (!track.locked) {
                        drag_origin_start = clip.start;
                        drag_origin_end   = clip.end;
                        float clip_scr_w  = orig_cx1 - orig_cx0;
                        bool left_edge  = clip_scr_w > 2.f*ew_hit && mouse.x <= orig_cx0+ew_hit;
                        bool right_edge = clip_scr_w > 2.f*ew_hit && mouse.x >= orig_cx1-ew_hit;
                        if (left_edge) {
                            drag_track=ti; drag_clip=ci; drag_left=true; drag_right=false; drag_offset=0.f;
                            g_tl.drag_multi.clear();
                        } else if (right_edge) {
                            drag_track=ti; drag_clip=ci; drag_right=true; drag_left=false; drag_offset=0.f;
                            g_tl.drag_multi.clear();
                        } else if (!track.managed) {  // body move blocked on managed tracks
                            drag_track=ti; drag_clip=ci; drag_left=false; drag_right=false;
                            drag_offset = (mouse.x - origin.x - TL_LABEL_W + scroll) / zoom - clip.start;
                            s_body_snap_held_start = -1.f; s_body_snap_held_cand = -1.f;
                            g_tl.drag_multi.clear();
                            for (auto& [stc, stci] : state.clip_selection) {
                                if (stc < (int)state.tracks.size() &&
                                    stci < (int)state.tracks[stc].clips.size()) {
                                    auto& sc = state.tracks[stc].clips[stci];
                                    g_tl.drag_multi.push_back({stc, stci, sc.start, sc.end});
                                }
                            }
                        }
                    }
                    if ((drag_left || drag_right) && !clip.text.empty() &&
                        s_source_durations.find(clip.text) == s_source_durations.end()) {
                        float dur = 0.f;
                        if (clip.clip_type == ClipType::Video)
                            dur = video_probe_duration(clip.text);
                        else if (clip.clip_type == ClipType::Audio) {
                            AudioMeta meta;
                            if (audio_probe(clip.text, meta)) dur = meta.duration_secs;
                        }
                        if (dur > 0.f) s_source_durations[clip.text] = dur;
                    }
                }
            }
            // Right-click context
            if (!clip_ctx_opened_this_frame && (!tl_any_popup && ImGui::IsMouseClicked(1)) &&
                mouse.y>=cy0 && mouse.y<=cy1 && mouse.x>=vis_x0 && mouse.x<=vis_x1) {
                ctx_track = ti; ctx_clip = ci;
                state.selected_track = ti; state.selected_clip = ci;
                open_clip_ctx = true;
                clip_ctx_opened_this_frame = true;
                ImGui::OpenPopup("##clip_ctx");
            }
        }; // end clip_interact

        // ── Pass 1: non-FX clips ─────────────────────────────────────────────
        for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
            Clip& clip = track.clips[ci];
            if (clip.clip_type == ClipType::Effect  ||
                clip.clip_type == ClipType::MultiFX ||
                clip.clip_type == ClipType::BodyFX) continue;
            float cx0 = origin.x+TL_LABEL_W+clip.start*zoom-scroll;
            float cx1 = origin.x+TL_LABEL_W+clip.end*zoom-scroll;
            if (cx1 < origin.x+TL_LABEL_W || cx0 > origin.x+total_w) continue;
            float vis_x0 = fmaxf(cx0, origin.x+TL_LABEL_W);
            float vis_x1 = fminf(cx1, origin.x+total_w);
            float cy0 = track_y+3.f, cy1 = track_y+TL_TRACK_H-3.f;
            bool sel = state.clip_selection.count({ti, ci}) > 0;

            if (clip.clip_type == ClipType::Video) {
                // Ensure proxy is queued for any video clip visible on the timeline,
                // including ones added programmatically via IPC (not just drag-dropped).
                if (!clip.text.empty()) proxy_start(clip.text);
                // Film strip: dark body normally; inverted bright-purple when selected
                ImU32 film_bg  = sel ? IM_COL32(160,  80, 255, 255) : IM_COL32(28, 28, 38, 255);
                ImU32 film_bdr = sel ? IM_COL32(200, 140, 255, 255) : IM_COL32(60, 60, 80, 255);
                ImU32 perf_col = sel ? IM_COL32( 90,  30, 160, 255) : IM_COL32(55, 55, 70, 255);
                dl->AddRectFilled({vis_x0,cy0},{vis_x1,cy1}, film_bg, 2.f);
                // Perforation strip top + bottom (always shown, color varies)
                dl->PushClipRect({vis_x0,cy0},{vis_x1,cy1},true);
                float ph = 4.f, pw = 3.f, pgap = 8.f;
                for (float px2 = vis_x0+4.f; px2+pw < vis_x1; px2 += pgap) {
                    dl->AddRectFilled({px2,cy0+2.f},{px2+pw,cy0+2.f+ph}, perf_col, 1.f);
                    dl->AddRectFilled({px2,cy1-2.f-ph},{px2+pw,cy1-2.f}, perf_col, 1.f);
                }
                dl->PopClipRect();
                dl->AddRect({vis_x0,cy0},{vis_x1,cy1}, film_bdr, 2.f);
                ImGui::PushClipRect({vis_x0,cy0},{vis_x1,cy1},true);
                std::string fname_s = clip.text.empty() ? "Video"
                    : fs::path(clip.text).filename().string();
                const char* fname = fname_s.c_str();
                ImU32 ftcol = sel ? IM_COL32(20, 8, 40, 255) : IM_COL32(200, 200, 220, 255);
                dl->AddText({vis_x0+4.f, cy0+(cy1-cy0-13.f)*0.5f}, ftcol, fname);
                ImGui::PopClipRect();
                // Proxy progress bar — shown while transcoding, absent once ready
                if (!clip.text.empty()) {
                    auto pst = proxy_job_status(clip.text);
                    const float bar_h = 3.f;
                    if (pst.state == ProxyJobStatus::State::Generating) {
                        float filled = (vis_x1 - vis_x0) * pst.progress;
                        dl->AddRectFilled({vis_x0, cy1-bar_h}, {vis_x0+filled, cy1},
                                          IM_COL32(100, 210, 255, 255));
                        dl->AddRectFilled({vis_x0+filled, cy1-bar_h}, {vis_x1, cy1},
                                          IM_COL32(30, 40, 60, 200));
                    } else if (pst.state == ProxyJobStatus::State::Queued) {
                        dl->AddRectFilled({vis_x0, cy1-bar_h}, {vis_x1, cy1},
                                          IM_COL32(60, 70, 100, 160));
                    }
                }
            } else if (clip.clip_type == ClipType::Background) {
                // Background brick: deep purple gradient with shimmer lines
                ImU32 bg_fill   = sel ? IM_COL32(210,90,200,255) : IM_COL32(80,20,90,255);
                ImU32 bg_border = sel ? IM_COL32(255,160,255,255) : IM_COL32(180,60,160,200);
                dl->AddRectFilled({vis_x0,cy0},{vis_x1,cy1}, bg_fill, 2.f);
                // Shimmer diagonal lines for "animated" feel
                dl->PushClipRect({vis_x0,cy0},{vis_x1,cy1},true);
                float bh = cy1-cy0;
                for (float ox = vis_x0-bh; ox < vis_x1+bh; ox += 12.f)
                    dl->AddLine({ox,cy0},{ox+bh,cy1}, IM_COL32(255,120,255, sel?60:30), 1.f);
                dl->PopClipRect();
                dl->AddRect({vis_x0,cy0},{vis_x1,cy1}, bg_border, 2.f, 0, 1.5f);
                // Label: preset id or "BG"
                ImGui::PushClipRect({vis_x0,cy0},{vis_x1,cy1},true);
                const char* lbl = clip.text.empty() ? "BG" : clip.text.c_str();
                dl->AddText({vis_x0+4.f, cy0+(cy1-cy0-13.f)*0.5f},
                    sel ? IM_COL32(40,0,40,255) : IM_COL32(220,150,220,255), lbl);
                ImGui::PopClipRect();
            } else if (clip.clip_type == ClipType::BodyFX) {
                // Solid BodyFX brick: teal/cyan accent
                ImU32 bfx_fill   = sel ? IM_COL32( 20,180,160,255) : IM_COL32(10, 80, 75, 255);
                ImU32 bfx_border = sel ? IM_COL32( 80,255,220,255) : IM_COL32(30,160,130,200);
                dl->AddRectFilled({vis_x0,cy0},{vis_x1,cy1}, bfx_fill, 2.f);
                dl->AddRect({vis_x0,cy0},{vis_x1,cy1}, bfx_border, 2.f, 0, 1.5f);
                // Label: "BodyFX — EffectName"
                ImGui::PushClipRect({vis_x0,cy0},{vis_x1,cy1},true);
                {
                    const BodyFXInfo* info = body_fx_find_info(clip.body_fx_type);
                    char lbl[64];
                    snprintf(lbl, sizeof(lbl), "Body FX%s%s",
                             info ? " \xe2\x80\x94 " : "", info ? info->name : "");
                    ImU32 ltcol = sel ? IM_COL32(0,30,25,255) : IM_COL32(160,240,220,255);
                    dl->AddText({vis_x0+4.f, cy0+(cy1-cy0-13.f)*0.5f}, ltcol, lbl);
                }
                ImGui::PopClipRect();
            } else {
                ImVec4 clip_fill = (clip.clip_type==ClipType::Lyrics)   ? Col::clip_lyrics
                                 : (clip.clip_type==ClipType::Subtitle) ? Col::clip_subtitle
                                 : (clip.clip_type==ClipType::Text)     ? Col::clip_sub
                                                                        : Col::clip_audio;
                dl->AddRectFilled({vis_x0,cy0},{vis_x1,cy1},
                    to_u32(sel ? Col::fg : clip_fill), 2.f);
                dl->AddRect({vis_x0,cy0},{vis_x1,cy1},
                    to_u32(sel ? Col::fg : Col::line), 2.f);

                if ((clip.clip_type==ClipType::Text || clip.clip_type==ClipType::Lyrics ||
                     clip.clip_type==ClipType::Subtitle) && !clip.text.empty()) {
                    ImGui::PushClipRect({vis_x0,cy0},{vis_x1,cy1},true);
                    dl->AddText({vis_x0+4.f, cy0+(cy1-cy0-13.f)*0.5f},
                        to_u32(sel ? Col::bg : Col::fg), clip.text.c_str());
                    ImGui::PopClipRect();
                } else if (clip.clip_type==ClipType::Audio) {
                    const WaveformData* wd = !clip.text.empty()
                        ? waveform_get(clip.text) : nullptr;
                    ImGui::PushClipRect({vis_x0,cy0},{vis_x1,cy1},true);
                    if (wd && !wd->samples.empty()) {
                        float mid  = (cy0 + cy1) * 0.5f;
                        float half = (cy1 - cy0) * 0.44f;
                        ImU32 wcol = sel ? IM_COL32(0,0,0,160) : IM_COL32(255,255,255,120);
                        for (float px2 = vis_x0; px2 < vis_x1; px2 += 1.f) {
                            float t_src = clip.in_point + (px2 - cx0) / zoom;
                            int   fi    = (int)(t_src * WAVEFORM_FPS);
                            if (fi < 0 || fi >= (int)wd->samples.size()) continue;
                            float amp = wd->samples[fi] * half;
                            if (amp < 1.f) amp = 1.f;
                            dl->AddLine({px2, mid-amp}, {px2, mid+amp}, wcol);
                        }
                    }
                    ImGui::PopClipRect();
                }
            }

            // Locked track stripe
            if (track.locked) {
                dl->PushClipRect({vis_x0,cy0},{vis_x1,cy1},true);
                ImU32 sc = IM_COL32(255,255,255,55);
                float h = cy1 - cy0, step = 8.f, span = (vis_x1 - vis_x0) + h;
                for (float d = -h; d < span; d += step)
                    dl->AddLine({vis_x0 + d, cy0}, {vis_x0 + d + h, cy1}, sc, 1.2f);
                dl->PopClipRect();
            }
            // Per-clip mute icon
            if (clip.muted && vis_x1 - vis_x0 > 16.f) {
                float ix = vis_x1 - 10.f, iy = cy0 + 7.f;
                ImU32 ic = IM_COL32(255, 80, 80, 220);
                dl->AddRectFilled({ix-3.f, iy-2.f}, {ix-1.f, iy+2.f}, ic);
                dl->AddTriangleFilled({ix-1.f,iy-3.f},{ix-1.f,iy+3.f},{ix+3.f,iy}, ic);
                dl->AddLine({ix+1.f,iy-3.f},{ix+4.f,iy+2.f}, ic, 1.2f);
                dl->AddLine({ix+4.f,iy-3.f},{ix+1.f,iy+2.f}, ic, 1.2f);
            }

            clip_interact(ci, clip, vis_x0, vis_x1, cy0, cy1, sel);
        }

        // ── Pass 2: FX / MultiFX / BodyFX bricks — rendered on top, interact last ──
        for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
            Clip& clip = track.clips[ci];
            bool is_mfx = (clip.clip_type == ClipType::MultiFX);
            bool is_bfx = (clip.clip_type == ClipType::BodyFX);
            if (clip.clip_type != ClipType::Effect && !is_mfx && !is_bfx) continue;
            float cx0 = origin.x+TL_LABEL_W+clip.start*zoom-scroll;
            float cx1 = origin.x+TL_LABEL_W+clip.end*zoom-scroll;
            if (cx1 < origin.x+TL_LABEL_W || cx0 > origin.x+total_w) continue;
            float vis_x0 = fmaxf(cx0, origin.x+TL_LABEL_W);
            float vis_x1 = fminf(cx1, origin.x+total_w);
            float cy0 = track_y+3.f, cy1 = track_y+TL_TRACK_H-3.f;
            bool sel = state.clip_selection.count({ti, ci}) > 0;

            // BodyFX bricks: always glass, drawn with teal accent
            if (is_bfx) {
                ImU32 bfx_fill   = sel ? IM_COL32( 20, 180, 160, 255) : IM_COL32(10, 80, 75, 255);
                ImU32 bfx_border = sel ? IM_COL32( 80, 255, 220, 255) : IM_COL32(30, 160, 130, 200);
                dl->AddRectFilled({vis_x0, cy0}, {vis_x1, cy1}, bfx_fill, 2.f);
                dl->AddRect({vis_x0, cy0}, {vis_x1, cy1}, bfx_border, 2.f, 0, 1.5f);
                ImGui::PushClipRect({vis_x0, cy0}, {vis_x1, cy1}, true);
                {
                    const BodyFXInfo* info = body_fx_find_info(clip.body_fx_type);
                    char lbl[64];
                    snprintf(lbl, sizeof(lbl), "Body FX%s%s",
                             info ? " \xe2\x80\x94 " : "", info ? info->name : "");
                    ImU32 ltcol = sel ? IM_COL32(0, 30, 25, 255) : IM_COL32(160, 240, 220, 255);
                    dl->AddText({vis_x0 + 4.f, cy0 + (cy1 - cy0 - 13.f) * 0.5f}, ltcol, lbl);
                }
                ImGui::PopClipRect();
                clip_interact(ci, clip, vis_x0, vis_x1, cy0, cy1, sel);
                if (g_tl.drag_merge_ci == ci && drag_track == ti) {
                    float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 6.f);
                    ImU32 mc = IM_COL32(80, 255, 220, (int)(120 + 80 * pulse));
                    dl->AddRect({vis_x0, cy0}, {vis_x1, cy1}, mc, 2.f, 0, 2.5f);
                }
                continue;
            }

            bool is_glass = fx_clip_is_glass(state, ti, clip);
            FxBrickColors fbc = is_mfx ? fx_brick_colors(FXType::Glitch, sel)
                                       : fx_brick_colors(clip.fx_type, sel);

            if (is_glass) {
                // Glass brick: semi-transparent frosted overlay over the video clip below it.
                ImU32 fill_col = IM_COL32(
                    (int)(((fbc.fill>>0)&0xFF)*0.4f + 80),
                    (int)(((fbc.fill>>8)&0xFF)*0.4f + 80),
                    (int)(((fbc.fill>>16)&0xFF)*0.4f + 80), 160);
                dl->AddRectFilled({vis_x0,cy0},{vis_x1,cy1}, fill_col, 2.f);
                dl->PushClipRect({vis_x0,cy0},{vis_x1,cy1},true);
                float gh = cy1 - cy0;
                for (float ox = vis_x0 - gh; ox < vis_x1 + gh; ox += 9.f)
                    dl->AddLine({ox, cy0}, {ox + gh, cy1}, IM_COL32(180, 230, 255, 30), 1.f);
                dl->PopClipRect();
                ImU32 border_col = IM_COL32(130, 210, 255, sel ? 255 : 210);
                dl->AddRect({vis_x0,cy0},{vis_x1,cy1}, border_col, 2.f, 0, 2.f);
            } else {
                // Global FX brick: opaque, sits on its own track row.
                dl->AddRectFilled({vis_x0,cy0},{vis_x1,cy1}, fbc.fill, 2.f);
                dl->AddRect({vis_x0,cy0},{vis_x1,cy1}, fbc.border, 2.f, 0, 1.5f);

                // Active glow on tracks below (scope indicator for global FX)
                if (state.playhead >= clip.start && state.playhead < clip.end) {
                    float gx = origin.x + TL_LABEL_W;
                    for (int gti = ti+1; gti < (int)state.tracks.size(); ++gti) {
                        float gty = (track_area_top - state.tl_v_scroll) + gti * TL_TRACK_H;
                        if (gty+TL_TRACK_H < track_area_top || gty > track_area_bot) continue;
                        dl->AddRectFilled({gx,gty},{gx+2.f,gty+TL_TRACK_H},
                                          IM_COL32(160,110,255,80));
                    }
                }
            }

            ImGui::PushClipRect({vis_x0,cy0},{vis_x1,cy1},true);
            float ly = cy0 + (cy1-cy0-13.f)*0.5f;
            ImU32 lbl_col = is_glass ? IM_COL32(200, 240, 255, 255) : fbc.label;
            if (is_mfx) {
                // Show "N FX" label with stacked-layers indicator
                char mfx_lbl[24];
                snprintf(mfx_lbl, sizeof(mfx_lbl), "MULTI %d FX", (int)clip.fx_chain.size());
                dl->AddText({vis_x0+5.f, ly}, lbl_col, mfx_lbl);
                // Stacked layers icon: three small horizontal bars at right
                if (vis_x1 - vis_x0 > 40.f) {
                    float ix = vis_x1 - 14.f, iy = cy0 + (cy1-cy0)*0.35f;
                    for (int k = 0; k < 3; ++k)
                        dl->AddRectFilled({ix, iy+k*4.f}, {ix+10.f, iy+k*4.f+2.f}, lbl_col);
                }
            } else {
                dl->AddText({vis_x0+5.f, ly}, lbl_col, fx_type_name(clip.fx_type));
                // Scope arrow
                if (vis_x1-vis_x0 > 30.f) {
                    float ax=vis_x1-12.f, ay=cy0+(cy1-cy0)*0.35f;
                    if (is_glass)
                        dl->AddTriangleFilled({ax-5.f,ay},{ax+5.f,ay},{ax,ay+6.f},
                                              IM_COL32(130,210,255,220));
                    else
                        dl->AddTriangleFilled({ax-5.f,ay},{ax+5.f,ay},{ax,ay+7.f},fbc.label);
                }
            }
            ImGui::PopClipRect();

            clip_interact(ci, clip, vis_x0, vis_x1, cy0, cy1, sel);
            if (g_tl.drag_merge_ci == ci && drag_track == ti) {
                float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 6.f);
                ImU32 mc = IM_COL32(255, 180, 60, (int)(130 + 80 * pulse));
                dl->AddRect({vis_x0, cy0}, {vis_x1, cy1}, mc, 2.f, 0, 2.5f);
            }
        }

        // Left-click empty track body (no clip hit) — deselect
        if (!s_clip_hit && (!tl_any_popup && ImGui::IsMouseClicked(0)) &&
            mouse.y >= track_y && mouse.y < track_y+TL_TRACK_H &&
            mouse.x > origin.x+TL_LABEL_W && mouse.x < origin.x+total_w) {
            state.clip_selection.clear();
            state.selected_track = -1;
            state.selected_clip  = -1;
        }

        // Right-click empty timeline area (this track row, no clip hit)
        if (!clip_ctx_opened_this_frame && (!tl_any_popup && ImGui::IsMouseClicked(1)) &&
            mouse.y >= track_y && mouse.y < track_y+TL_TRACK_H &&
            mouse.x >= origin.x+TL_LABEL_W && mouse.x <= origin.x+total_w) {
            open_tl_ctx = true;
            ImGui::OpenPopup("##tl_ctx");
        }

        // Transition glass — drawn at every adjacent video clip cut point
        for (int ci = 0; ci + 1 < (int)track.clips.size(); ++ci) {
            Clip& a = track.clips[ci];
            Clip& b = track.clips[ci + 1];
            if (a.clip_type != ClipType::Video || b.clip_type != ClipType::Video) continue;
            if (fabsf(b.start - a.end) > 0.5f) continue;

            float cut_x   = origin.x + TL_LABEL_W + a.end   * zoom - scroll;
            float cy0     = track_y + 2.f;
            float cy1     = track_y + TL_TRACK_H - 2.f;
            float mid_y   = (cy0 + cy1) * 0.5f;
            bool  has_trans = (a.transition_type != TransitionType::None);
            bool  this_glass_active = (s_trans_track == ti && s_trans_left_ci == ci);

            if (!has_trans) {
                // Diamond affordance: click to add a transition
                float r = 7.f;
                ImVec2 pts[4] = {{cut_x,cut_x-r},{cut_x+r,mid_y},{cut_x,mid_y+r},{cut_x-r,mid_y}};
                pts[0] = {cut_x, mid_y - r};
                pts[1] = {cut_x + r, mid_y};
                pts[2] = {cut_x, mid_y + r};
                pts[3] = {cut_x - r, mid_y};
                bool hov = fabsf(mouse.x - cut_x) + fabsf(mouse.y - mid_y) <= r + 3.f;
                dl->AddConvexPolyFilled(pts, 4, IM_COL32(18,18,18,220));
                dl->AddPolyline(pts, 4, hov ? IM_COL32(255,255,255,230) : IM_COL32(210,210,210,200),
                                ImDrawFlags_Closed, 1.5f);
                if (hov && (!tl_any_popup && ImGui::IsMouseClicked(0))) {
                    s_trans_track    = ti; s_trans_left_ci = ci;
                    s_trans_popup_pos = {cut_x - 80.f, cy0 - 8.f};
                    s_trans_hit_this_frame = true;
                    ImGui::OpenPopup("##trans_picker");
                }
                if (hov) ImGui::SetTooltip("Add transition");
            } else {
                // Glass block
                float pre_px  = a.transition_pre  * zoom;
                float post_px = a.transition_post * zoom;
                float gx0 = cut_x - pre_px;
                float gx1 = cut_x + post_px;
                float vis_gx0 = fmaxf(gx0, origin.x + TL_LABEL_W);
                float vis_gx1 = fminf(gx1, origin.x + total_w);

                // Glass fill + border
                ImU32 glass_fill   = IM_COL32(130,190,255,55);
                ImU32 glass_border = this_glass_active
                    ? IM_COL32(160,210,255,255) : IM_COL32(130,190,255,200);
                dl->AddRectFilled({vis_gx0, cy0}, {vis_gx1, cy1}, glass_fill, 3.f);
                // Diagonal lines inside glass to suggest blend
                dl->PushClipRect({vis_gx0,cy0},{vis_gx1,cy1}, true);
                float step = 10.f;
                float gh   = cy1 - cy0;
                for (float ox = gx0 - gh; ox < gx1 + gh; ox += step)
                    dl->AddLine({ox, cy0}, {ox + gh, cy1}, IM_COL32(160,210,255,30), 1.f);
                dl->PopClipRect();
                dl->AddRect({vis_gx0, cy0}, {vis_gx1, cy1}, glass_border, 3.f, 0, 1.5f);

                // Edge drag handles
                float hw = 5.f;
                auto draw_handle = [&](float hx, bool hov_h) {
                    ImU32 hc = hov_h ? IM_COL32(255,255,255,255) : IM_COL32(160,210,255,230);
                    dl->AddRectFilled({hx-hw, cy0+2.f}, {hx+hw, cy1-2.f}, hc, 2.f);
                };

                float handle_tol = hw + 4.f;
                bool in_glass = mouse.y >= cy0 && mouse.y <= cy1 &&
                                mouse.x >= vis_gx0 && mouse.x <= vis_gx1;
                bool hov_left  = in_glass && fabsf(mouse.x - gx0) <= handle_tol && s_glass_drag == 0;
                bool hov_right = in_glass && fabsf(mouse.x - gx1) <= handle_tol && !hov_left && s_glass_drag == 0;
                bool hov_body  = in_glass && !hov_left && !hov_right;

                if (gx0 >= origin.x + TL_LABEL_W) draw_handle(gx0, hov_left);
                if (gx1 <= origin.x + total_w)     draw_handle(gx1, hov_right);

                // Cursor
                if (hov_left || hov_right || (s_glass_drag == 1 && this_glass_active) ||
                    (s_glass_drag == 2 && this_glass_active))
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

                // Tooltip
                if (hov_body && s_glass_drag == 0) {
                    const char* tname = (a.transition_type == TransitionType::Dissolve)  ? "Dissolve"
                                      : (a.transition_type == TransitionType::FadeBlack) ? "Fade to Black"
                                      : "Dip to White";
                    ImGui::SetTooltip("%s  %.2fs | %.2fs", tname, a.transition_pre, a.transition_post);
                }

                // Mouse down — start drag or open picker
                if ((!tl_any_popup && ImGui::IsMouseClicked(0)) && in_glass) {
                    s_trans_track    = ti; s_trans_left_ci = ci;
                    s_trans_hit_this_frame = true;
                    s_glass_drag_ref_x    = mouse.x;
                    s_glass_drag_ref_pre  = a.transition_pre;
                    s_glass_drag_ref_post = a.transition_post;
                    s_glass_drag_ref_start = a.start;
                    if (hov_left)       s_glass_drag = 1;
                    else if (hov_right) s_glass_drag = 2;
                    else                s_glass_drag = 3;
                    // Cancel any clip drag the clip loop already registered this frame
                    drag_track = -1; drag_clip = -1; drag_left = false; drag_right = false;
                }

                // Right-click removes transition
                if ((!tl_any_popup && ImGui::IsMouseClicked(1)) && in_glass) {
                    a.transition_type = TransitionType::None;
                    a.transition_pre  = 0.f; a.transition_post = 0.f;
                    s_glass_drag = 0;
                    s_trans_track = -1; s_trans_left_ci = -1;
                    history_push(state, "Remove transition");
                    s_trans_hit_this_frame = true;
                }
            }
        }

        // Glass drag update (runs once per frame, outside clip loop)
        if (s_glass_drag != 0 && s_trans_track == ti &&
            s_trans_left_ci >= 0 && s_trans_left_ci < (int)track.clips.size()) {
            Clip& a = track.clips[s_trans_left_ci];
            Clip* b_ptr = (s_trans_left_ci + 1 < (int)track.clips.size())
                          ? &track.clips[s_trans_left_ci + 1] : nullptr;
            float dx = (mouse.x - s_glass_drag_ref_x) / zoom;

            if (ImGui::IsMouseDragging(0)) {
                if (s_glass_drag == 1) {
                    // Left handle → adjust pre (drag left = more pre)
                    float new_pre = fmaxf(0.01f, fminf(s_glass_drag_ref_pre - dx,
                                                       a.end - a.start - 0.05f));
                    a.transition_pre = new_pre;
                } else if (s_glass_drag == 2) {
                    // Right handle → adjust post
                    float new_post = fmaxf(0.01f, fminf(s_glass_drag_ref_post + dx,
                                                        b_ptr ? (b_ptr->end - b_ptr->start - 0.05f) : 10.f));
                    a.transition_post = new_post;
                } else {
                    // Body drag → move linked pair
                    float new_start = fmaxf(0.f, s_glass_drag_ref_start + dx);
                    float dur_a = a.end - a.start;
                    a.start = new_start; a.end = new_start + dur_a;
                    if (b_ptr) {
                        float dur_b = b_ptr->end - b_ptr->start;
                        b_ptr->start = a.end; b_ptr->end = a.end + dur_b;
                    }
                }
            }
            // Single click (no meaningful drag) on glass body → open type picker
            if (ImGui::IsMouseReleased(0)) {
                if (s_glass_drag == 3 && fabsf(mouse.x - s_glass_drag_ref_x) < 4.f) {
                    float cut_x2 = origin.x + TL_LABEL_W + a.end * zoom - scroll;
                    s_trans_popup_pos = {cut_x2 - 80.f, track_y - 8.f};
                    ImGui::OpenPopup("##trans_picker");
                } else if (s_glass_drag != 3) {
                    history_push(state, "Adjust transition");
                }
                s_glass_drag = 0;
            }
        }

        track_y += TL_TRACK_H;
    }

    // "+ Add Track" — scrolls with the track list as the last row
    {
        ImVec2 row_tl = {origin.x,           track_y};
        ImVec2 row_br = {origin.x + total_w,  track_y + TL_TRACK_H};
        bool add_hov       = mouse.y >= track_y && mouse.y < track_y + TL_TRACK_H &&
                             mouse.x >= origin.x && mouse.x <= origin.x + total_w;
        bool add_label_hov = add_hov && mouse.x < origin.x + TL_LABEL_W;
        dl->AddRectFilled(row_tl, {origin.x + TL_LABEL_W, track_y + TL_TRACK_H},
                          to_u32(add_label_hov ? Col::bg_soft_hov : Col::bg_soft));
        dl->AddRectFilled({origin.x + TL_LABEL_W, track_y}, row_br, to_u32(Col::bg));
        dl->AddLine(row_tl, {origin.x + total_w, track_y}, to_u32(Col::line));
        dl->AddLine({origin.x + TL_LABEL_W, track_y}, {origin.x + TL_LABEL_W, track_y + TL_TRACK_H},
                    to_u32(Col::line));
        float lh = ImGui::GetTextLineHeight();
        dl->AddText({origin.x + 8.f, track_y + (TL_TRACK_H - lh) * 0.5f},
                    to_u32(add_label_hov ? Col::fg : Col::muted), "+ Add Track");
        if (add_label_hov && (!tl_any_popup && ImGui::IsMouseClicked(0))) {
            Track t;
            char name[32]; snprintf(name, sizeof(name), "Track %d", (int)state.tracks.size() + 1);
            t.name = name; state.tracks.insert(state.tracks.begin(), std::move(t));
        }
    }

    ImGui::PopClipRect();  // end scrollable track area clip (restores window ClipRect)

    // ── Track reorder drag ────────────────────────────────────────────────────
    if (s_track_drag_src >= 0) {
        if (ImGui::IsMouseDown(0)) {
            if (!s_track_dragging && fabsf(mouse.y - s_track_drag_start_y) > 4.f)
                s_track_dragging = true;
            if (s_track_dragging) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                float rel = mouse.y - (origin.y + TL_RULER_H);
                int   ins = (int)roundf(rel / TL_TRACK_H);
                s_track_drag_insert = std::clamp(ins, 0, (int)state.tracks.size());
            }
        } else {
            if (s_track_dragging && s_track_drag_insert >= 0) {
                int src = s_track_drag_src;
                int dst = s_track_drag_insert;
                if (dst > src) dst--;
                if (dst != src && dst >= 0 && dst < (int)state.tracks.size()) {
                    Track moved = std::move(state.tracks[src]);
                    state.tracks.erase(state.tracks.begin() + src);
                    state.tracks.insert(state.tracks.begin() + dst, std::move(moved));
                    if      (state.selected_track == src)                                       state.selected_track = dst;
                    else if (src < dst && state.selected_track > src && state.selected_track <= dst) state.selected_track--;
                    else if (src > dst && state.selected_track >= dst && state.selected_track < src) state.selected_track++;
                    history_push(state, "Reorder track");
                }
            }
            s_track_drag_src    = -1;
            s_track_dragging    = false;
            s_track_drag_insert = -1;
        }
    }

    if (s_track_dragging && s_track_drag_src >= 0 &&
        s_track_drag_src < (int)state.tracks.size()) {
        // Insert line
        float ins_y = origin.y + TL_RULER_H + s_track_drag_insert * TL_TRACK_H;
        dl->AddLine({origin.x, ins_y}, {origin.x + total_w, ins_y},
                    IM_COL32(120, 200, 255, 220), 2.f);
        dl->AddCircleFilled({origin.x + 5.f, ins_y}, 4.f, IM_COL32(120, 200, 255, 220));

        // Ghost label following cursor
        float gy0 = mouse.y - TL_TRACK_H * 0.5f;
        float gy1 = gy0 + TL_TRACK_H;
        dl->AddRectFilled({origin.x, gy0}, {origin.x + TL_LABEL_W, gy1},
                          IM_COL32(20, 20, 20, 230), 3.f);
        dl->AddRect({origin.x, gy0}, {origin.x + TL_LABEL_W, gy1},
                    IM_COL32(120, 200, 255, 200), 3.f, 0, 1.5f);
        dl->AddText({origin.x + 8.f, gy0 + (TL_TRACK_H - ImGui::GetTextLineHeight()) * 0.5f},
                    IM_COL32(255, 255, 255, 255),
                    state.tracks[s_track_drag_src].name.c_str());
    }

    // ── Box select ───────────────────────────────────────────────────────────
    {
        bool in_body = mouse.x > origin.x+TL_LABEL_W && mouse.x < origin.x+total_w &&
                       mouse.y > origin.y+TL_RULER_H  && mouse.y < origin.y+total_h - TL_SCROLLBAR_H;
        bool ldown  = ImGui::IsMouseDown(0);
        bool lclick = (!tl_any_popup && ImGui::IsMouseClicked(0));

        // Deselect on click in the label column (any track row or below all tracks)
        bool in_label_empty = lclick && !ImGui::IsAnyItemActive() &&
                              mouse.x >= origin.x && mouse.x < origin.x+TL_LABEL_W &&
                              mouse.y > origin.y+TL_RULER_H && mouse.y < origin.y+total_h - TL_SCROLLBAR_H &&
                              s_rename_track < 0;  // don't deselect while renaming

        // Start box select when clicking empty body space (no clip was hit)
        if (lclick && in_body && !s_clip_hit) {
            s_box_selecting = true;
            s_box_start     = mouse;
            if (!ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) {
                state.clip_selection.clear();
                state.selected_track = -1;
                state.selected_clip  = -1;
            }
        } else if (in_label_empty) {
            state.clip_selection.clear();
            state.selected_track = -1;
            state.selected_clip  = -1;
        }
        s_clip_hit = false; // reset for next frame

        if (s_box_selecting) {
            if (ldown) {
                // Draw rubber-band rect
                float bx0 = fminf(s_box_start.x, mouse.x);
                float bx1 = fmaxf(s_box_start.x, mouse.x);
                float by0 = fminf(s_box_start.y, mouse.y);
                float by1 = fmaxf(s_box_start.y, mouse.y);
                dl->AddRectFilled({bx0,by0},{bx1,by1}, IM_COL32(120,200,255,30));
                dl->AddRect({bx0,by0},{bx1,by1}, IM_COL32(120,200,255,180), 0.f, 0, 1.f);

                // Live update selection while dragging
                float t0 = (bx0 - origin.x - TL_LABEL_W + scroll) / zoom;
                float t1 = (bx1 - origin.x - TL_LABEL_W + scroll) / zoom;
                // tl_v_scroll shifts all track rows up — must add it back to map screen-y → track index
                int n_tr  = (int)state.tracks.size();
                int   tr0 = (int)((by0 - origin.y - TL_RULER_H + state.tl_v_scroll) / TL_TRACK_H);
                int   tr1 = (int)((by1 - origin.y - TL_RULER_H + state.tl_v_scroll) / TL_TRACK_H);
                tr0 = (tr0 < 0) ? 0 : (tr0 >= n_tr ? n_tr-1 : tr0);
                tr1 = (tr1 < 0) ? 0 : (tr1 >= n_tr ? n_tr-1 : tr1);

                if (!ImGui::GetIO().KeyCtrl) state.clip_selection.clear();
                // Only select clips when the box has meaningful size — a zero-size
                // box (single click, t0==t1) would match any clip spanning the cursor,
                // immediately re-selecting what we just deselected.
                if (bx1 - bx0 > 4.f || by1 - by0 > 4.f)
                for (int t2=0; t2<(int)state.tracks.size(); ++t2) {
                    if (t2 < tr0 || t2 > tr1) continue;
                    for (int c2=0; c2<(int)state.tracks[t2].clips.size(); ++c2) {
                        const Clip& bc = state.tracks[t2].clips[c2];
                        if (bc.end > t0 && bc.start < t1)
                            state.clip_selection.insert({t2, c2});
                    }
                }
            } else {
                // Released — finalise, update focus to first selected clip
                s_box_selecting = false;
                if (!state.clip_selection.empty()) {
                    auto first = *state.clip_selection.begin();
                    state.selected_track = first.first;
                    state.selected_clip  = first.second;
                }
            }
        }

        // Escape clears selection
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && s_rename_track < 0) {
            state.clip_selection.clear();
            state.selected_track = -1;
            state.selected_clip  = -1;
            s_box_selecting = false;
        }
    }

    // ── Transition picker popup ───────────────────────────────────────────────
    {
        ImVec2 disp = ImGui::GetIO().DisplaySize;
        constexpr float PW = 200.f, PH = 160.f;
        ImVec2 pp = s_trans_popup_pos;
        pp.x = fmaxf(4.f, fminf(pp.x, disp.x - PW - 4.f));
        pp.y = fmaxf(4.f, fminf(pp.y, disp.y - PH - 4.f));
        ImGui::SetNextWindowPos(pp, ImGuiCond_Appearing);
    }
    ImGui::SetNextWindowSize({200.f, 0.f}, ImGuiCond_Appearing);
    if (ImGui::BeginPopup("##trans_picker")) {
        if (s_trans_track >= 0 && s_trans_left_ci >= 0 &&
            s_trans_track < (int)state.tracks.size() &&
            s_trans_left_ci < (int)state.tracks[s_trans_track].clips.size()) {
            Clip& tc = state.tracks[s_trans_track].clips[s_trans_left_ci];

            struct TBtn { TransitionType t; const char* l; };
            TBtn btns[] = {
                {TransitionType::Dissolve, "Dissolve"},
                {TransitionType::FadeBlack,"Fade to Black"},
                {TransitionType::DipWhite, "Dip to White"},
            };
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4.f, 4.f});
            for (auto& b : btns) {
                bool sel = (tc.transition_type == b.t);
                if (sel) ImGui::PushStyleColor(ImGuiCol_Button, to_u32(Col::fg));
                if (ImGui::Button(b.l, {ImGui::GetContentRegionAvail().x, 0.f})) {
                    tc.transition_type = b.t;
                    if (tc.transition_pre  <= 0.f) tc.transition_pre  = 0.25f;
                    if (tc.transition_post <= 0.f) tc.transition_post = 0.25f;
                    history_push(state, "Transition");
                    ImGui::CloseCurrentPopup();
                }
                if (sel) ImGui::PopStyleColor();
            }
            ImGui::Dummy({0.f, 2.f});
            if (tc.transition_type != TransitionType::None) {
                ImGui::Separator();
                ImGui::Dummy({0.f, 2.f});
                if (ImGui::Button("Remove", {ImGui::GetContentRegionAvail().x, 0.f})) {
                    tc.transition_type = TransitionType::None;
                    tc.transition_pre  = 0.f; tc.transition_post = 0.f;
                    history_push(state, "Remove transition");
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::PopStyleVar();
        }
        ImGui::EndPopup();
    }

    // ── Past-end dim overlay ──────────────────────────────────────────────────
    {
        float end_x = origin.x + TL_LABEL_W + dur * zoom - scroll;
        if (end_x < origin.x + total_w) {
            float dim_x = fmaxf(end_x, origin.x + TL_LABEL_W);
            // Ruler area
            dl->AddRectFilled({dim_x, origin.y},
                {origin.x + total_w, origin.y + TL_RULER_H},
                IM_COL32(0, 0, 0, 110));
            // Track rows
            dl->AddRectFilled({dim_x, origin.y + TL_RULER_H},
                {origin.x + total_w, track_area_bot},
                IM_COL32(0, 0, 0, 85));
            // Subtle end-of-project line
            if (end_x >= origin.x + TL_LABEL_W)
                dl->AddLine({end_x, origin.y},
                    {end_x, track_area_bot},
                    IM_COL32(255, 255, 255, 50));
        }
    }

    // ── Horizontal scrollbar ─────────────────────────────────────────────────────
    {
        float sb_x0 = origin.x + TL_LABEL_W;
        float sb_x1 = origin.x + total_w - TL_VSCROLLBAR_W;
        float sb_w  = sb_x1 - sb_x0;
        float sb_y0 = origin.y + total_h - TL_SCROLLBAR_H;
        float sb_y1 = origin.y + total_h;

        // Bottom strip background spans the full width including the bottom-right
        // dead-corner where the H and V scrollbars meet.
        dl->AddRectFilled({origin.x, sb_y0}, {origin.x + total_w, sb_y1},
                          IM_COL32(18, 18, 18, 255));
        dl->AddLine({origin.x, sb_y0}, {origin.x + total_w, sb_y0},
                    to_u32(Col::line));

        if (tl_content_w > clip_area_w + 1.f || scroll > 0.f) {
            float max_scroll   = tl_content_w - clip_area_w;
            float thumb_w      = fmaxf(20.f, sb_w * clip_area_w / tl_content_w);
            float thumb_travel = fmaxf(1.f, sb_w - thumb_w);
            float thumb_x0     = sb_x0 + scroll / max_scroll * thumb_travel;
            float thumb_x1     = thumb_x0 + thumb_w;

            bool hov_thumb = ImGui::IsMouseHoveringRect({thumb_x0, sb_y0}, {thumb_x1, sb_y1});

            static bool  s_sb_drag       = false;
            static float s_sb_drag_ox    = 0.f;
            static float s_sb_drag_osc   = 0.f;

            if (hov_thumb && (!tl_any_popup && ImGui::IsMouseClicked(0))) {
                s_sb_drag     = true;
                s_sb_drag_ox  = mouse.x;
                s_sb_drag_osc = scroll;
            }
            if (s_sb_drag) {
                if (ImGui::IsMouseDown(0)) {
                    float dx = mouse.x - s_sb_drag_ox;
                    scroll = fmaxf(0.f, fminf(s_sb_drag_osc + dx * max_scroll / thumb_travel, max_scroll));
                } else {
                    s_sb_drag = false;
                }
            }

            // Click in trough → jump
            if (!s_sb_drag && !hov_thumb && (!tl_any_popup && ImGui::IsMouseClicked(0)) &&
                ImGui::IsMouseHoveringRect({sb_x0, sb_y0}, {sb_x1, sb_y1})) {
                float t = (mouse.x - sb_x0 - thumb_w * 0.5f) / thumb_travel;
                scroll = fmaxf(0.f, fminf(t * max_scroll, max_scroll));
            }

            ImU32 thumb_col = s_sb_drag  ? IM_COL32(170, 170, 170, 255)
                            : hov_thumb  ? IM_COL32(130, 130, 130, 255)
                                         : IM_COL32(80,  80,  80,  255);
            dl->AddRectFilled({thumb_x0 + 1.f, sb_y0 + 2.f},
                              {thumb_x1 - 1.f, sb_y1 - 2.f},
                              thumb_col, 2.f);
        }
    }

    // ── Vertical scrollbar ──────────────────────────────────────────────────────
    // Mirrors the horizontal scrollbar but for tl_v_scroll.  Spans only the track
    // area (below the ruler, above the horizontal scrollbar) so the bottom-right
    // corner where they would meet is left as a dead-corner.
    {
        float vb_x0 = origin.x + total_w - TL_VSCROLLBAR_W;
        float vb_x1 = origin.x + total_w;
        float vb_y0 = track_area_top;
        float vb_y1 = track_area_bot;
        float vb_h  = vb_y1 - vb_y0;

        dl->AddRectFilled({vb_x0, vb_y0}, {vb_x1, vb_y1}, IM_COL32(18, 18, 18, 255));
        dl->AddLine({vb_x0, vb_y0}, {vb_x0, vb_y1}, to_u32(Col::line));

        float v_visible = track_area_bot - track_area_top;
        if (tracks_total_h > v_visible + 1.f || state.tl_v_scroll > 0.f) {
            float v_max_scroll   = fmaxf(1.f, tracks_total_h - v_visible);
            float v_thumb_h      = fmaxf(20.f, vb_h * v_visible / tracks_total_h);
            float v_thumb_travel = fmaxf(1.f, vb_h - v_thumb_h);
            float v_thumb_y0     = vb_y0 + state.tl_v_scroll / v_max_scroll * v_thumb_travel;
            float v_thumb_y1     = v_thumb_y0 + v_thumb_h;

            bool hov_vthumb = ImGui::IsMouseHoveringRect({vb_x0, v_thumb_y0}, {vb_x1, v_thumb_y1});

            static bool  s_vsb_drag     = false;
            static float s_vsb_drag_oy  = 0.f;
            static float s_vsb_drag_osc = 0.f;

            if (hov_vthumb && (!tl_any_popup && ImGui::IsMouseClicked(0))) {
                s_vsb_drag     = true;
                s_vsb_drag_oy  = mouse.y;
                s_vsb_drag_osc = state.tl_v_scroll;
            }
            if (s_vsb_drag) {
                if (ImGui::IsMouseDown(0)) {
                    float dy = mouse.y - s_vsb_drag_oy;
                    state.tl_v_scroll = fmaxf(0.f,
                        fminf(s_vsb_drag_osc + dy * v_max_scroll / v_thumb_travel, v_max_scroll));
                } else {
                    s_vsb_drag = false;
                }
            }

            // Click in trough → jump (center thumb on click point)
            if (!s_vsb_drag && !hov_vthumb && (!tl_any_popup && ImGui::IsMouseClicked(0)) &&
                ImGui::IsMouseHoveringRect({vb_x0, vb_y0}, {vb_x1, vb_y1})) {
                float t = (mouse.y - vb_y0 - v_thumb_h * 0.5f) / v_thumb_travel;
                state.tl_v_scroll = fmaxf(0.f, fminf(t * v_max_scroll, v_max_scroll));
            }

            ImU32 vthumb_col = s_vsb_drag  ? IM_COL32(170, 170, 170, 255)
                             : hov_vthumb  ? IM_COL32(130, 130, 130, 255)
                                           : IM_COL32(80,  80,  80,  255);
            dl->AddRectFilled({vb_x0 + 2.f, v_thumb_y0 + 1.f},
                              {vb_x1 - 2.f, v_thumb_y1 - 1.f},
                              vthumb_col, 2.f);
        }
    }

    // Drag handling (frame-snapped + edge-snapped, Ctrl bypasses both)
    if (drag_track>=0 && drag_clip>=0 && (drag_left||drag_right))
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (drag_track>=0 && drag_clip>=0 && s_glass_drag==0 && ImGui::IsMouseDragging(0)) {
        s_drag_moved = true;
        Clip& dc = state.tracks[drag_track].clips[drag_clip];
        auto cands = build_snap_candidates(drag_track, drag_clip);
        float new_t = (mouse.x - origin.x - TL_LABEL_W + scroll) / zoom - drag_offset;
        // Helper: find transition-linked neighbour clip on same track (same-track adjacency only)
        auto linked_right = [&]() -> Clip* {
            auto& clips = state.tracks[drag_track].clips;
            if (drag_clip + 1 >= (int)clips.size()) return nullptr;
            Clip& nb = clips[drag_clip + 1];
            if (nb.clip_type != ClipType::Video) return nullptr;
            return (dc.transition_type != TransitionType::None) ? &nb : nullptr;
        };
        auto linked_left = [&]() -> Clip* {
            if (drag_clip <= 0) return nullptr;
            Clip& nb = state.tracks[drag_track].clips[drag_clip - 1];
            if (nb.clip_type != ClipType::Video) return nullptr;
            return (nb.transition_type != TransitionType::None) ? &nb : nullptr;
        };

        // Inner edges (shared with a transition) are locked — only outer edges trim
        bool right_locked = (dc.clip_type == ClipType::Video &&
                             dc.transition_type != TransitionType::None);
        bool left_locked  = (dc.clip_type == ClipType::Video &&
                             linked_left() != nullptr);

        // Slot key is keyed by clip.start. When start changes during drag, update
        // proxy_paths in-place so gc_video_slots doesn't close/reopen every frame.
        float old_dc_start = dc.start;
        auto sync_proxy_key = [&]() {
            if (dc.clip_type != ClipType::Video || dc.text.empty()) return;
            if (dc.start == old_dc_start) return;
            std::string old_key = clip_slot_key(dc.text, old_dc_start);
            std::string new_key = clip_slot_key(dc.text, dc.start);
            for (int i = 0; i < MAX_VIDEO_TRACKS; ++i)
                if (state.proxy_paths[i] == old_key) { state.proxy_paths[i] = new_key; break; }
        };

        // Look up source duration once for this drag update (0 = unknown, no clamp).
        auto src_dur_it = s_source_durations.find(dc.text);
        float src_dur = (src_dur_it != s_source_durations.end()) ? src_dur_it->second : 0.f;

        if (drag_left && !left_locked) {
            float t = edge_snap(snap(new_t), cands);
            bool still_img_l = dc.clip_type == ClipType::Video && is_image_path(dc.text);
            float src_floor = (src_dur > 0.f && !still_img_l)
                ? dc.start - dc.in_point / fmaxf(0.01f, dc.speed) : 0.f;
            float new_start = fmaxf(src_floor, fmaxf(0.f, fminf(t, dc.end - f1)));
            dc.in_point = fmaxf(0.f, dc.in_point + (new_start - dc.start));
            dc.start = new_start;
            sync_proxy_key();
        } else if (drag_right && !right_locked) {
            float et = (mouse.x - origin.x - TL_LABEL_W + scroll) / zoom;
            float t = edge_snap(snap(et), cands);
            // Stills hold a single frame indefinitely, so the source-duration
            // cap doesn't apply — let the user stretch the brick freely.
            bool still_img = dc.clip_type == ClipType::Video && is_image_path(dc.text);
            float max_end = (src_dur > 0.f && !still_img)
                ? dc.start + (src_dur - dc.in_point) / fmaxf(0.01f, dc.speed)
                : t;
            dc.end = fmaxf(dc.start + f1, fminf(t, max_end));
        } else if (!drag_left && !drag_right) {
            float dur_clip = dc.end - dc.start;
            // Try snapping both edges; use whichever is closer to a candidate.
            float left_raw  = snap(new_t);
            float right_raw = left_raw + dur_clip;
            float thresh        = SNAP_PX / zoom;
            float escape_thresh = 3.f / fps;
            float best_dt    = thresh;
            float best_start = left_raw;
            s_snap_indicator = -1.f;
            // Hysteresis: once snapped, hold until raw position escapes 2.5× the snap radius.
            // Prevents flickering between two nearby candidates.
            if (s_body_snap_held_start >= 0.f) {
                if (fabsf(left_raw - s_body_snap_held_start) < escape_thresh) {
                    best_start       = s_body_snap_held_start;
                    s_snap_indicator = s_body_snap_held_cand;
                } else {
                    s_body_snap_held_start = -1.f;
                    s_body_snap_held_cand  = -1.f;
                }
            }
            if (s_body_snap_held_start < 0.f && s_snap_enabled && !ImGui::GetIO().KeyCtrl) {
                for (float c : cands) {
                    float dl = fabsf(c - left_raw);
                    if (dl < best_dt) { best_dt = dl; best_start = c;           s_snap_indicator = c; }
                    float dr = fabsf(c - right_raw);
                    if (dr < best_dt) { best_dt = dr; best_start = c - dur_clip; s_snap_indicator = c; }
                }
                if (best_start != left_raw) {
                    s_body_snap_held_start = best_start;
                    s_body_snap_held_cand  = s_snap_indicator;
                }
            }
            dc.start = fmaxf(0.f, best_start);
            dc.end   = dc.start + dur_clip;
            sync_proxy_key();
            // Co-move all other selected clips by the same delta
            if (g_tl.drag_multi.size() > 1) {
                float delta = dc.start - drag_origin_start;
                for (auto& orig : g_tl.drag_multi) {
                    if (orig.ti == drag_track && orig.ci == drag_clip) continue;
                    if (orig.ti >= (int)state.tracks.size()) continue;
                    if (orig.ci >= (int)state.tracks[orig.ti].clips.size()) continue;
                    Clip& oc = state.tracks[orig.ti].clips[orig.ci];
                    oc.start = fmaxf(0.f, orig.start + delta);
                    oc.end   = oc.start + (orig.end - orig.start);
                }
            }
            // Co-move transition-linked neighbours
            if (Clip* nb = linked_right()) { float d = nb->end - nb->start; nb->start = dc.end;   nb->end = nb->start + d; }
            if (Clip* nb = linked_left())  { float d = nb->end - nb->start; nb->end   = dc.start; nb->start = fmaxf(0.f, nb->end - d); }
            // Track which row the mouse is hovering for cross-track transfer.
            // hot == tracks.size() means below all tracks → "New Track" ghost row.
            // drag_hot_gap >= 0 means between two existing tracks → insert new track there.
            // Scroll-corrected origin: tracks visually start here on screen.
            float track_origin_y = origin.y + TL_RULER_H - state.tl_v_scroll;
            int n_tracks = (int)state.tracks.size();
            // Midpoint assignment: drop target is whichever track center is closest.
            // Gap insertion fires only when mouse is within GAP_PX of a seam.
            const float GAP_PX = 5.f;
            drag_hot_gap = -1;
            for (int gi = 0; gi < n_tracks; ++gi) {
                float boundary_y = track_origin_y + gi * TL_TRACK_H;
                if (fabsf(mouse.y - boundary_y) < GAP_PX) {
                    drag_hot_gap = gi;
                    break;
                }
            }
            // Always assign hot track by midpoint — gap zone does not steal the target.
            {
                int hot = (int)((mouse.y - track_origin_y) / TL_TRACK_H);
                drag_hot_track = (hot >= 0 && hot <= n_tracks) ? hot : -1;
            }
            // Merge-target detection: FX-on-FX overlap on the same track.
            g_tl.drag_merge_ci = -1;
            if (!drag_left && !drag_right) {
                const Clip& dc_ref = state.tracks[drag_track].clips[drag_clip];
                bool dc_is_fx = (dc_ref.clip_type == ClipType::Effect ||
                                 dc_ref.clip_type == ClipType::MultiFX ||
                                 dc_ref.clip_type == ClipType::BodyFX);
                if (dc_is_fx) {
                    for (int ci2 = 0; ci2 < (int)state.tracks[drag_track].clips.size(); ++ci2) {
                        if (ci2 == drag_clip) continue;
                        const Clip& oc = state.tracks[drag_track].clips[ci2];
                        if (oc.clip_type != ClipType::Effect &&
                            oc.clip_type != ClipType::MultiFX &&
                            oc.clip_type != ClipType::BodyFX) continue;
                        if (dc_ref.start < oc.end && dc_ref.end > oc.start) {
                            g_tl.drag_merge_ci = ci2;
                            break;
                        }
                    }
                }
            }
        }
    } else {
        s_snap_indicator = -1.f;
        g_tl.drag_merge_ci = -1;
    }
    if (ImGui::IsMouseReleased(0)) {
        if (drag_track >= 0 && drag_clip >= 0) {
            {
            if (!drag_left && !drag_right && drag_hot_gap >= 0) {
                // Drop into gap between tracks — insert new track there
                Clip moved = state.tracks[drag_track].clips[drag_clip];
                state.tracks[drag_track].clips.erase(
                    state.tracks[drag_track].clips.begin() + drag_clip);
                Track nt;
                char name[32];
                snprintf(name, sizeof(name), "Track %d", (int)state.tracks.size() + 1);
                nt.name = name;
                nt.clips.push_back(moved);
                state.tracks.insert(state.tracks.begin() + drag_hot_gap, std::move(nt));
                state.selected_track = drag_hot_gap;
                state.selected_clip  = 0;
                state.clip_selection.clear();
                state.clip_selection.insert({state.selected_track, state.selected_clip});
                history_push(state, "Move clip to new track");
            } else if (!drag_left && !drag_right && s_drag_moved &&
                drag_hot_track >= 0 && drag_hot_track != drag_track) {
                Clip moved = state.tracks[drag_track].clips[drag_clip];
                state.tracks[drag_track].clips.erase(
                    state.tracks[drag_track].clips.begin() + drag_clip);
                if (drag_hot_track == (int)state.tracks.size()) {
                    // Drop below all tracks — create new track
                    Track nt;
                    char name[32];
                    snprintf(name, sizeof(name), "Track %d", (int)state.tracks.size() + 1);
                    nt.name = name;
                    nt.clips.push_back(moved);
                    state.tracks.push_back(nt);
                    state.selected_track = (int)state.tracks.size() - 1;
                    state.selected_clip  = 0;
                    state.clip_selection.clear();
                    state.clip_selection.insert({state.selected_track, state.selected_clip});
                    history_push(state, "Move clip to new track");
                } else {
                    // Abort drop if any clip on the target track conflicts with the moved clip.
                    bool overlaps = false;
                    for (const Clip& oc : state.tracks[drag_hot_track].clips)
                        if (clips_conflict(moved, oc)) { overlaps = true; break; }
                    if (overlaps) {
                        // Put clip back on its original track at its original position
                        moved.start = drag_origin_start;
                        moved.end   = drag_origin_end;
                        state.tracks[drag_track].clips.insert(
                            state.tracks[drag_track].clips.begin() + drag_clip, moved);
                        state.selected_track = drag_track;
                        state.selected_clip  = drag_clip;
                        state.clip_selection.clear();
                        state.clip_selection.insert({state.selected_track, state.selected_clip});
                    } else {
                        state.tracks[drag_hot_track].clips.push_back(moved);
                        state.selected_track = drag_hot_track;
                        state.selected_clip  = (int)state.tracks[drag_hot_track].clips.size() - 1;
                        state.clip_selection.clear();
                        state.clip_selection.insert({state.selected_track, state.selected_clip});
                        history_push(state, "Move clip to track");
                    }
                }
            } else {
                // Merge: FX brick dropped on top of another FX brick (same track)
                if (!drag_left && !drag_right && g_tl.drag_merge_ci >= 0) {
                    int tgt_ci = g_tl.drag_merge_ci;
                    Clip dragged_copy = state.tracks[drag_track].clips[drag_clip];
                    merge_fx_clips(state.tracks[drag_track].clips[tgt_ci], dragged_copy);
                    // Erase the dragged clip; adjust index if target is after it
                    state.tracks[drag_track].clips.erase(
                        state.tracks[drag_track].clips.begin() + drag_clip);
                    int sel_ci = (tgt_ci > drag_clip) ? tgt_ci - 1 : tgt_ci;
                    state.selected_track = drag_track;
                    state.selected_clip  = sel_ci;
                    state.clip_selection.clear();
                    state.clip_selection.insert({drag_track, sel_ci});
                    g_tl.drag_merge_ci = -1;
                    history_push(state, "Merge FX bricks");
                    goto drag_done;
                }
                // Body drag on same track — validate with conflict predicate, restore on conflict
                bool overlaps = false;
                if (!drag_left && !drag_right) {
                    // Check the focus clip and all multi-drag clips for conflicts
                    auto check_clip_conflicts = [&](int chk_ti, int chk_ci) -> bool {
                        if (chk_ti >= (int)state.tracks.size()) return false;
                        const Clip& mc = state.tracks[chk_ti].clips[chk_ci];
                        for (int ci2 = 0; ci2 < (int)state.tracks[chk_ti].clips.size(); ++ci2) {
                            if (ci2 == chk_ci) continue;
                            // Skip other multi-drag clips (they moved together, no relative conflict)
                            bool in_multi = false;
                            for (auto& orig : g_tl.drag_multi)
                                if (orig.ti == chk_ti && orig.ci == ci2) { in_multi = true; break; }
                            if (in_multi) continue;
                            if (clips_conflict(mc, state.tracks[chk_ti].clips[ci2])) return true;
                        }
                        return false;
                    };
                    if (check_clip_conflicts(drag_track, drag_clip)) overlaps = true;
                    if (!overlaps) {
                        for (auto& orig : g_tl.drag_multi) {
                            if (orig.ti == drag_track && orig.ci == drag_clip) continue;
                            if (check_clip_conflicts(orig.ti, orig.ci)) { overlaps = true; break; }
                        }
                    }
                }
                if (overlaps) {
                    // Restore focus clip
                    Clip& moved_clip = state.tracks[drag_track].clips[drag_clip];
                    if (moved_clip.clip_type == ClipType::Video && !moved_clip.text.empty()
                        && moved_clip.start != drag_origin_start) {
                        std::string old_key = clip_slot_key(moved_clip.text, moved_clip.start);
                        std::string new_key = clip_slot_key(moved_clip.text, drag_origin_start);
                        for (int i = 0; i < MAX_VIDEO_TRACKS; ++i)
                            if (state.proxy_paths[i] == old_key) { state.proxy_paths[i] = new_key; break; }
                    }
                    moved_clip.start = drag_origin_start;
                    moved_clip.end   = drag_origin_end;
                    // Restore all other multi-drag clips
                    for (auto& orig : g_tl.drag_multi) {
                        if (orig.ti == drag_track && orig.ci == drag_clip) continue;
                        if (orig.ti >= (int)state.tracks.size()) continue;
                        if (orig.ci >= (int)state.tracks[orig.ti].clips.size()) continue;
                        state.tracks[orig.ti].clips[orig.ci].start = orig.start;
                        state.tracks[orig.ti].clips[orig.ci].end   = orig.end;
                    }
                } else if (s_drag_moved || drag_left || drag_right) {
                    const char* act = drag_left  ? "Trim clip start" :
                                      drag_right ? "Trim clip end"   : "Move clip";
                    history_push(state, act);
                }
            }
            }
        }
        drag_done:
        drag_track=-1; drag_clip=-1; drag_left=false; drag_right=false;
        drag_hot_track=-1; drag_hot_gap=-1;
        g_tl.drag_merge_ci = -1;
        s_drag_moved = false;
        s_body_snap_held_start = -1.f; s_body_snap_held_cand = -1.f;
        g_tl.drag_multi.clear();
    }

    // Playhead
    float ph_x = origin.x+TL_LABEL_W+state.playhead*zoom-scroll;
    if (ph_x >= origin.x+TL_LABEL_W && ph_x <= origin.x+total_w) {
        dl->AddLine({ph_x, origin.y}, {ph_x, origin.y+total_h}, to_u32(Col::fg));
        dl->AddTriangleFilled({ph_x-5.f,origin.y},{ph_x+5.f,origin.y},{ph_x,origin.y+10.f},to_u32(Col::fg));
    }

    // Click/drag ruler to seek. Once grabbed, mouse can roam outside the ruler strip.
    auto& s_ruler_drag = g_tl.ruler_drag;
    bool any_popup_global = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (ImGui::IsMouseReleased(0)) s_ruler_drag = false;
    if (!any_popup_global && drag_track < 0) {
        bool in_ruler = mouse.y >= origin.y && mouse.y <= origin.y + TL_RULER_H &&
                        mouse.x >= origin.x + TL_LABEL_W && mouse.x <= origin.x + total_w;
        if (in_ruler && (!tl_any_popup && ImGui::IsMouseClicked(0))) s_ruler_drag = true;
        if (s_ruler_drag && ImGui::IsMouseDown(0)) {
            float raw = (mouse.x - origin.x - TL_LABEL_W + scroll) / zoom;
            std::vector<float> edge_cands;
            for (int ti = 0; ti < (int)state.tracks.size(); ++ti)
                for (auto& cl : state.tracks[ti].clips) {
                    edge_cands.push_back(cl.start);
                    edge_cands.push_back(cl.end);
                }
            edge_cands.push_back(0.f);
            float t = edge_snap(snap(fmaxf(0.f, fminf(raw, dur))), edge_cands);
            seek_to(state, t);
        }
    }

    // ── Snap indicator line ───────────────────────────────────────────────────
    if (s_snap_indicator >= 0.f) {
        float sx = origin.x + TL_LABEL_W + s_snap_indicator * zoom - scroll;
        if (sx >= origin.x + TL_LABEL_W && sx <= origin.x + total_w) {
            dl->AddLine({sx, origin.y + TL_RULER_H}, {sx, origin.y + total_h},
                        IM_COL32(120, 200, 255, 200), 1.f);
        }
    }

    // ── Between-track gap ghost (insert new track between two existing ones) ────
    if (drag_track >= 0 && drag_clip >= 0 && !drag_left && !drag_right &&
        drag_hot_gap >= 0 && drag_hot_gap < (int)state.tracks.size()) {
        float gap_y = origin.y + TL_RULER_H + drag_hot_gap * TL_TRACK_H;
        dl->AddLine({origin.x, gap_y}, {origin.x + total_w, gap_y},
                    IM_COL32(120, 200, 255, 220), 2.f);
        dl->AddCircleFilled({origin.x + 5.f, gap_y}, 4.f, IM_COL32(120, 200, 255, 220));
        float lh = ImGui::GetTextLineHeight();
        float label_off = (drag_hot_gap == 0) ? 3.f : (-lh - 3.f);
        dl->AddText({origin.x + 14.f, gap_y + label_off},
                    IM_COL32(120, 200, 255, 200), "New Track");
    }

    // ── New-track ghost row (shown when dragging below all tracks) ────────────
    if (drag_track >= 0 && drag_clip >= 0 && !drag_left && !drag_right &&
        drag_hot_track == (int)state.tracks.size() &&
        drag_track < (int)state.tracks.size() &&
        drag_clip  < (int)state.tracks[drag_track].clips.size()) {
        ImVec2 gr_tl = {origin.x,           track_y};
        ImVec2 gr_br = {origin.x + total_w,  track_y + TL_TRACK_H};
        // Dashed outline row
        dl->AddRectFilled(gr_tl, gr_br, IM_COL32(255,255,255,8));
        dl->AddRect(gr_tl, gr_br, IM_COL32(255,255,255,60), 0.f, 0, 1.f);
        // "New Track" label
        dl->AddText({origin.x + 8.f, track_y + (TL_TRACK_H - ImGui::GetTextLineHeight()) * 0.5f},
                    IM_COL32(255,255,255,100), "New Track");
        // Clip ghost
        const Clip& gc = state.tracks[drag_track].clips[drag_clip];
        float gx0 = fmaxf(origin.x+TL_LABEL_W, origin.x+TL_LABEL_W+gc.start*zoom-scroll);
        float gx1 = fminf(origin.x+total_w,     origin.x+TL_LABEL_W+gc.end*zoom-scroll);
        float gy0 = track_y + 3.f, gy1 = track_y + TL_TRACK_H - 3.f;
        if (gx1 > gx0) {
            dl->AddRectFilled({gx0,gy0},{gx1,gy1}, IM_COL32(255,255,255,35), 2.f);
            dl->AddRect({gx0,gy0},{gx1,gy1}, IM_COL32(255,255,255,160), 2.f, 0, 1.5f);
        }
        track_y += TL_TRACK_H;  // push "+ Add Track" down so they don't overlap
    }

    // ── New-track drop zone for picker bricks (BG / FX_CREATIVE) ─────────────
    // Covers the row immediately below all existing tracks so dragging
    // a preset card past the last track creates a new track on drop.
    {
        ImVec2 dz_tl = {origin.x + TL_LABEL_W, track_y};
        ImVec2 dz_br = {origin.x + total_w, fminf(track_y + TL_TRACK_H, track_area_bot)};
        if (dz_br.y > origin.y + TL_RULER_H && dz_br.y > dz_tl.y) {
            ImGui::SetCursorScreenPos(dz_tl);
            ImGui::InvisibleButton("##picker_new_track",
                                   {dz_br.x - dz_tl.x, dz_br.y - dz_tl.y});
            if (ImGui::BeginDragDropTarget()) {
                // reuse_empty: media/audio land on an existing empty track when
                // one is free. Background/FX bricks always get a real new track
                // — their vertical position decides what they affect.
                auto make_new_track = [&](Clip&& cl, const char* act, bool reuse_empty) {
                    int target = reuse_empty ? find_empty_track(state) : -1;
                    if (target < 0) {
                        Track nt;
                        nt.name = clip_type_name(cl.clip_type);
                        state.tracks.push_back(std::move(nt));
                        target = (int)state.tracks.size() - 1;
                    }
                    state.tracks[target].clips.push_back(std::move(cl));
                    state.selected_track = target;
                    state.selected_clip  = (int)state.tracks[target].clips.size() - 1;
                    s_drop_flash_track   = target;
                    s_drop_flash_t       = 0.6f;
                    history_push(state, act);
                };
                float drop_t = fmaxf(0.f, (ImGui::GetMousePos().x - (origin.x + TL_LABEL_W) + scroll) / zoom);

                if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("BG_PRESET")) {
                    const char* pid = (const char*)pay->Data;
                    const BgPreset* pr = bg_preset_by_id(pid);
                    if (pr) {
                        float proj_dur = 7.f;
                        Clip cl; cl.clip_type = ClipType::Background; cl.text = pid;
                        cl.start = drop_t; cl.end = drop_t + proj_dur;
                        cl.bg_speed = pr->default_speed; cl.bg_intensity = 0.85f;
                        memcpy(cl.bg_c1, pr->dc1, 16); memcpy(cl.bg_c2, pr->dc2, 16); memcpy(cl.bg_c3, pr->dc3, 16);
                        // Backgrounds are content (a generated texture layer),
                        // not position-sensitive like FX bricks — reuse-empty.
                        make_new_track(std::move(cl), (std::string("Drop Background: ") + pr->label).c_str(), true);
                    }
                }
                if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("FX_CREATIVE")) {
                    FXType ft = (FXType)*(const int*)pay->Data;
                    Clip cl; cl.clip_type = ClipType::Effect; cl.fx_type = ft;
                    cl.start = drop_t; cl.end = drop_t + 5.f;
                    make_new_track(std::move(cl), (std::string("Drop FX: ") + fx_type_name(ft)).c_str(), false);
                }
                auto new_track_media = [&](const char* ptype) {
                    if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload(ptype)) {
                        std::string path((const char*)pay->Data, pay->DataSize - 1);
                        bool img = is_image_path(path);
                        float dur = img ? 5.f : video_probe_duration(path);
                        if (dur <= 0.f) dur = 4.f;
                        Clip cl; cl.clip_type = ClipType::Video; cl.text = path;
                        cl.source_id = path; cl.start = drop_t; cl.end = drop_t + dur;
                        s_source_durations[path] = dur;
                        std::string act = (img ? "Drop image: " : "Drop video: ") +
                                          fs::path(path).filename().string();
                        make_new_track(std::move(cl), act.c_str(), true);
                        proxy_start(path);
                        int slot = slot_for_video(state, clip_slot_key(path, drop_t), path);
                        if (slot >= 0) video_open_still(slot, proxy_still_path(path));
                        state.video_loaded = true;
                        recent_media_push(path, img ? MediaKind::Image : MediaKind::Video);
                    }
                };
                new_track_media("MEDIA_VID");
                new_track_media("MEDIA_IMG");
                if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("MEDIA_AUD")) {
                    std::string path((const char*)pay->Data, pay->DataSize - 1);
                    AudioMeta meta{}; float dur = audio_probe(path, meta) ? meta.duration_secs : 4.f;
                    if (dur <= 0.f) dur = 4.f;
                    Clip cl; cl.clip_type = ClipType::Audio; cl.text = path;
                    cl.source_id = path; cl.start = drop_t; cl.end = drop_t + dur;
                    s_source_durations[path] = dur;
                    std::string act = "Drop audio: " + fs::path(path).filename().string();
                    make_new_track(std::move(cl), act.c_str(), true);
                    audio_source_ensure(path);
                    recent_media_push(path, MediaKind::Audio);
                }
                // Highlight drop zone
                dl->AddRectFilled(dz_tl, dz_br, IM_COL32(180,130,255,20));
                dl->AddLine(dz_tl, {dz_br.x, dz_tl.y}, IM_COL32(180,130,255,160), 1.5f);
                dl->AddText({dz_tl.x + 6.f, dz_tl.y + (TL_TRACK_H - 13.f) * 0.5f},
                            IM_COL32(180,130,255,180), "+ New Track");
                ImGui::EndDragDropTarget();
            }
        }
    }

    // ── Context menus ─────────────────────────────────────────────────────────

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.f, 6.f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   {8.f, 4.f});

    if (ImGui::BeginPopup("##clip_ctx")) {
        open_clip_ctx = false;
        int ti = ctx_track, ci = ctx_clip;
        bool valid = ti>=0 && ti<(int)state.tracks.size() &&
                     ci>=0 && ci<(int)state.tracks[ti].clips.size();
        Track* ct = valid ? &state.tracks[ti] : nullptr;
        Clip*  cc = valid ? &ct->clips[ci]    : nullptr;

        // ── Rip audio (video clips only) ─────────────────────────────────────
        if (cc && cc->clip_type==ClipType::Video) {
            bool ext_busy = state.extract_running;
            if (ext_busy) ImGui::BeginDisabled();
            if (ImGui::MenuItem(ext_busy ? "Ripping audio…" : "Rip audio to new track")) {
                state.extract_source_track = ctx_track;
                extract_audio_start(state, cc->text);
            }
            if (ext_busy) ImGui::EndDisabled();
            ImGui::Separator();
        }

        // ── Make lyric video / Transcribe / Separate (Audio & Video clips) ──────
        if (cc && (cc->clip_type==ClipType::Audio || cc->clip_type==ClipType::Video)) {
            bool busy         = transcribe_running();
            bool models_ready = state.models_ready;
            bool disabled     = busy || !models_ready;

            if (disabled) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Make lyric video")) {
                state.pipeline_on_done = generate_typography;
                kick_pipeline(state, cc->text, PipelineMode::Both);
            }
            if (ImGui::MenuItem("Transcribe  (subtitles only)")) {
                kick_pipeline(state, cc->text, PipelineMode::TranscribeOnly);
            }
            if (ImGui::MenuItem("Separate vocals")) {
                kick_pipeline(state, cc->text, PipelineMode::SeparateOnly);
            }
            if (disabled) ImGui::EndDisabled();

            if (!models_ready && !busy) {
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Set up AI first");
                if (ImGui::MenuItem("Set Up AI Features…"))
                    state.show_model_dl_modal = true;
            }
            ImGui::Separator();
        }

        // ── Subtitle clips ────────────────────────────────────────────────────
        if (cc && (cc->clip_type==ClipType::Text || cc->clip_type==ClipType::Lyrics)) {
            if (ImGui::MenuItem("Edit text")) {
                s_panel_view = PanelView::Clip;
                s_edit_focus_next = true;
            }
            // Re-group submenu — only if we have source JSON
            bool has_json = !state.words_json_path.empty() &&
                            fs::exists(state.words_json_path);
            if (ImGui::BeginMenu("Re-group track as…")) {
                struct { SubtitleMode m; const char* label; } rmodes[] = {
                    {SubtitleMode::Word,    "Word-by-word"},
                    {SubtitleMode::Phrase,  "Phrase  (pauses >0.3s)"},
                    {SubtitleMode::Line,    "Line  (gaps >0.8s)"},
                    {SubtitleMode::Karaoke, "Karaoke  (line + per-word highlight)"},
                    {SubtitleMode::Segment, "Segment  (sentence)"},
                    {SubtitleMode::CustomN, "Custom N words"},
                };
                for (auto& rm : rmodes) {
                    bool cur = state.subtitle_mode == rm.m;
                    if (!has_json) ImGui::BeginDisabled();
                    if (ImGui::MenuItem(rm.label, nullptr, cur)) {
                        state.subtitle_mode = rm.m;
                        apply_subtitle_mode(state);
                        history_push(state, std::string("Grouping — ") + rm.label);
                    }
                    if (!has_json) ImGui::EndDisabled();
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
        }

        // ── Convert single Effect → Multi-FX ─────────────────────────────────
        if (cc && cc->clip_type == ClipType::Effect) {
            if (ImGui::MenuItem("Wrap in Multi-FX")) {
                Clip se    = *cc;
                cc->clip_type  = ClipType::MultiFX;
                cc->fx_chain.clear();
                cc->fx_chain_selected = 0;
                // Sub-clip inherits the parent's time span as full duration (rel_start=0, rel_end=0)
                se.rel_start = 0.f; se.rel_end = 0.f;
                cc->fx_chain.push_back(std::move(se));
                history_push(state, "Wrap in Multi-FX");
            }
            ImGui::Separator();
        }

        bool trk_locked = ct && ct->locked;
        if (trk_locked) ImGui::BeginDisabled();
        if (ImGui::MenuItem("Split at playhead", "S")) {
            if (valid) {
                float cut = state.playhead;
                if (cut > cc->start+0.02f && cut < cc->end-0.02f) {
                    Clip right = *cc; cc->end = cut; right.start = cut;
                    right.in_point += (cut - cc->start) * cc->speed;
                    ct->clips.insert(ct->clips.begin()+ci+1, right);
                    history_push(state, "Split clip");
                }
            }
        }
        if (ImGui::MenuItem("Duplicate clip")) {
            if (valid) {
                Clip dup = *cc; dup.start = cc->end; dup.end = dup.start + (cc->end - cc->start);
                ct->clips.insert(ct->clips.begin()+ci+1, dup);
                history_push(state, "Duplicate clip");
            }
        }
        if (trk_locked) ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::MenuItem("Seek to clip start")) {
            if (valid) { seek_to(state, cc->start); }
        }
        if (ImGui::MenuItem("Seek to clip end")) {
            if (valid) { seek_to(state, cc->end); }
        }
        ImGui::Separator();
        if (trk_locked) ImGui::BeginDisabled();
        if (valid) {
            const char* mute_lbl = cc->muted ? "Unmute clip" : "Mute clip";
            if (ImGui::MenuItem(mute_lbl)) {
                cc->muted = !cc->muted;
                history_push(state, cc->muted ? "Mute clip" : "Unmute clip");
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete clip")) {
            if (valid) {
                ct->clips.erase(ct->clips.begin()+ci);
                state.selected_clip = -1;
                history_push(state, "Delete clip");
            }
        }
        if (trk_locked) ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##track_ctx")) {
        open_track_ctx = false;
        int ti = ctx_track;
        bool valid = ti>=0 && ti<(int)state.tracks.size();
        Track* ct = valid ? &state.tracks[ti] : nullptr;

        if (ct) {
            // Rename — inline edit
            static char rename_buf[64] = {};
            static bool rename_open = false;
            if (!rename_open) { strncpy(rename_buf, ct->name.c_str(), 63); }
            ImGui::SetNextItemWidth(140.f);
            if (ImGui::InputText("##rename", rename_buf, sizeof(rename_buf),
                    ImGuiInputTextFlags_EnterReturnsTrue)) {
                ct->name = rename_buf;
                history_push(state, "Rename track");
                ImGui::CloseCurrentPopup();
            }
            rename_open = ImGui::IsItemActive();
            ImGui::Separator();
        }

        // Managed track controls
        if (ct && ct->managed) {
            if (ImGui::MenuItem("Detach from preset")) {
                ct->managed = false;
                history_push(state, "Detach track from preset");
            }
            ImGui::Separator();
        }

        // ── Add clip to this track ────────────────────────────────────────────
        if (valid && (!ct || !ct->managed)) {
            if (ImGui::MenuItem("Add Text Clip")) {
                add_clip_to_track(state, ti, "", ClipType::Text);
                s_edit_focus_next = true;
            }
            if (ImGui::MenuItem("Add Video Clip…")) {
                std::string p = filepicker_open("Add video clip",
                    "Video", "*.mp4 *.mov *.mkv *.avi *.webm");
                if (!p.empty()) add_clip_to_track(state, ti, p, ClipType::Video);
            }
            if (ImGui::MenuItem("Add Audio Clip…")) {
                std::string p = filepicker_open("Add audio clip",
                    "Audio", "*.wav *.mp3 *.m4a *.flac *.aac");
                if (!p.empty()) add_clip_to_track(state, ti, p, ClipType::Audio);
            }
            ImGui::Separator();
        }

        if (valid && ti > 0) {
            if (ImGui::MenuItem("Move up")) {
                std::swap(state.tracks[ti], state.tracks[ti-1]);
                state.selected_track = ti-1;
                history_push(state, "Move track up");
            }
        }
        if (valid && ti < (int)state.tracks.size()-1) {
            if (ImGui::MenuItem("Move down")) {
                std::swap(state.tracks[ti], state.tracks[ti+1]);
                state.selected_track = ti+1;
                history_push(state, "Move track down");
            }
        }
        ImGui::Separator();
        if (ct) {
            if (ImGui::MenuItem(ct->visible ? "Hide track" : "Show track")) {
                ct->visible = !ct->visible;
                history_push(state, "Track visibility");
            }
            if (ImGui::MenuItem(ct->muted ? "Unmute" : "Mute")) {
                ct->muted = !ct->muted;
                history_push(state, "Track mute");
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete track", nullptr, false, valid)) {
            std::string tname = ct ? ct->name : "";
            if (state.selected_track == ti) { state.selected_track=-1; state.selected_clip=-1; }
            state.tracks.erase(state.tracks.begin()+ti);
            history_push(state, "Delete track — " + tname);
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##tl_ctx")) {
        open_tl_ctx = false;
        if (ImGui::MenuItem("Add Track")) {
            Track t;
            char n[32]; snprintf(n,sizeof(n),"Track %d",(int)state.tracks.size()+1);
            t.name=n; state.tracks.insert(state.tracks.begin(), std::move(t));
            history_push(state, "Add Track");
        }
        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(2);
    (void)open_clip_ctx; (void)open_track_ctx; (void)open_tl_ctx;
}
