#include "studio_types.h"
#include "studio_shared.h"
#include "canvas.h"
#include "../text_renderer.h"
#include "pipeline.h"
#include "app.h"
#include "runtime_fx.h"
#include "audio.h"
#include "video.h"
#include "proxy.h"
#include "history.h"
#include "fx_shader.h"
#include "bg_presets.h"
#include "theme.h"
#include "render.h"
#include "waveform.h"
#include "body_fx.h"
#include "bg_remove.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace fs = std::filesystem;
extern ImFont* g_font_bold;
extern ImFont* g_font_black;

// s_scrub_until is declared extern in canvas.h, defined here
double s_scrub_until = 0.0;

// ── Canvas object system ──────────────────────────────────────────────────────
// TextLayout: tight rendered bbox computed each frame during text draw.
// Persists so draw_canvas_handles can use accurate extents for handles.
struct TextLayout {
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;  // tight bbox, canvas pixels
    float block_ax = 0.f;   // anchor X (canvas pixels)
    float fsz      = 0.f;   // final rendered font size
    bool  valid    = false;
};
static std::unordered_map<uint64_t, TextLayout> s_text_layouts;

enum class CanvasHandle {
    None, Body,
    CornerTL, CornerTR, CornerBL, CornerBR,
    EdgeL, EdgeR, EdgeT, EdgeB,
    Rotate
};

struct CanvasTransform {
    CanvasHandle handle     = CanvasHandle::None;
    int          track_idx  = -1, clip_idx = -1;
    float        drag_sx    = 0.f, drag_sy = 0.f;
    float        start_pos_x = 0.f, start_pos_y = 0.f;
    float        start_wrap_w    = 0.f;
    float        start_font_size = 0.f;
    float        start_scale_x   = 0.f, start_scale_y = 0.f;
    float        start_rot       = 0.f;
    int          start_anchor    = 1;
    float        start_bbox_x0   = 0.f, start_bbox_y0 = 0.f;
    float        start_bbox_x1   = 0.f, start_bbox_y1 = 0.f;
    bool         dirty = false;
};
static CanvasTransform s_ctx;

void compute_video_bbox(AppState& state, Clip& cl, ImVec2 p, float w, float h,
                                float& bx0, float& by0, float& bx1, float& by1) {
    float px = cl.eval_prop("pos_x",   state.playhead) * w + p.x;
    float py = cl.eval_prop("pos_y",   state.playhead) * h + p.y;
    float sx = cl.eval_prop("scale_x", state.playhead);
    float sy = cl.eval_prop("scale_y", state.playhead);
    float fit_w = w, fit_h = h;
    std::string vkey = clip_slot_key(cl.text, cl.start);
    for (int s = 0; s < MAX_VIDEO_TRACKS; ++s) {
        if (state.proxy_paths[s] == vkey && video_info(s).width > 0) {
            float va = (float)video_info(s).width / (float)video_info(s).height;
            float ca = w / h;
            if (va > ca) { fit_w = w; fit_h = w / va; }
            else         { fit_h = h; fit_w = h * va; }
            break;
        }
    }
    float hw = fit_w * sx * 0.5f, hh = fit_h * sy * 0.5f;
    bx0 = px - hw; by0 = py - hh;
    bx1 = px + hw; by1 = py + hh;
}

void draw_canvas_handles(AppState& state, ImDrawList* dl, ImVec2 p, float w, float h) {
    if (state.selected_track < 0 || state.selected_clip < 0) return;
    if (state.selected_track >= (int)state.tracks.size()) return;
    Track& tr = state.tracks[state.selected_track];
    if (state.selected_clip >= (int)tr.clips.size()) return;
    Clip& cl = tr.clips[state.selected_clip];

    ImVec2 mpos   = ImGui::GetIO().MousePos;
    bool   ldown  = ImGui::IsMouseDown(0);
    bool   lclick = ImGui::IsMouseClicked(0);

    bool in_preview  = mpos.x >= p.x && mpos.x <= p.x+w &&
                       mpos.y >= p.y && mpos.y <= p.y+h;
    bool drag_active = (s_ctx.handle != CanvasHandle::None);
    if (!in_preview && !drag_active) return;

    // ── Handle visual constants ───────────────────────────────────────────────
    const float CR       = 4.5f;    // corner half-size
    const float EL       = 11.f;    // edge handle long-axis half
    const float ES       = 2.5f;    // edge handle short-axis half
    const float ROT_DIST = 28.f;

    ImU32 box_col  = IM_COL32(255, 255, 255, 180);
    ImU32 hdl_col  = IM_COL32(255, 255, 255, 230);
    ImU32 hdl_bdr  = IM_COL32(0,   0,   0,   180);
    ImU32 hdl_hov  = IM_COL32(100, 180, 255, 255);
    ImU32 snap_col = IM_COL32(100, 180, 255, 160);

    // ── Draw helpers (return true if clicked) ─────────────────────────────────
    auto draw_corner_h = [&](float cx, float cy, CanvasHandle ht) -> bool {
        bool hov = fabsf(mpos.x - cx) <= CR + 4.f && fabsf(mpos.y - cy) <= CR + 4.f;
        ImU32 c = (hov || s_ctx.handle == ht) ? hdl_hov : hdl_col;
        dl->AddRectFilled({cx-CR, cy-CR}, {cx+CR, cy+CR}, c, 2.f);
        dl->AddRect      ({cx-CR, cy-CR}, {cx+CR, cy+CR}, hdl_bdr, 2.f, 0, 0.8f);
        return hov && lclick && s_ctx.handle == CanvasHandle::None;
    };
    // vertical=true → tall bar (left/right edges); false → wide bar (top/bottom)
    auto draw_edge_h = [&](float ex, float ey, bool vertical, CanvasHandle ht) -> bool {
        float ex0 = vertical ? ex-ES : ex-EL, ey0 = vertical ? ey-EL : ey-ES;
        float ex1 = vertical ? ex+ES : ex+EL, ey1 = vertical ? ey+EL : ey+ES;
        bool hov = mpos.x >= ex0-4.f && mpos.x <= ex1+4.f &&
                   mpos.y >= ey0-4.f && mpos.y <= ey1+4.f;
        ImU32 c = (hov || s_ctx.handle == ht) ? hdl_hov : hdl_col;
        dl->AddRectFilled({ex0, ey0}, {ex1, ey1}, c, 2.5f);
        dl->AddRect      ({ex0, ey0}, {ex1, ey1}, hdl_bdr, 2.5f, 0, 0.6f);
        return hov && lclick && s_ctx.handle == CanvasHandle::None;
    };
    auto begin_drag = [&](CanvasHandle ht) {
        s_ctx.handle    = ht;
        s_ctx.track_idx = state.selected_track;
        s_ctx.clip_idx  = state.selected_clip;
        s_ctx.drag_sx   = mpos.x;
        s_ctx.drag_sy   = mpos.y;
        s_ctx.dirty     = false;
    };

    // ── Video clip ────────────────────────────────────────────────────────────
    if (cl.clip_type == ClipType::Video) {
        float bx0, by0, bx1, by1;
        compute_video_bbox(state, cl, p, w, h, bx0, by0, bx1, by1);
        float vmx = (bx0+bx1)*0.5f, vmy = (by0+by1)*0.5f;
        float vcx  = cl.eval_prop("pos_x", state.playhead) * w + p.x;
        float vcy  = cl.eval_prop("pos_y", state.playhead) * h + p.y;

        // Solid box
        dl->AddRect({bx0, by0}, {bx1, by1}, box_col, 0.f, 0, 1.5f);

        // Rotation handle
        ImVec2 rot_pos = {vmx, by0 - ROT_DIST};
        dl->AddLine({vmx, by0}, rot_pos, IM_COL32(255,255,255,80));
        dl->AddCircleFilled(rot_pos, CR+1.5f, IM_COL32(0,0,0,120));
        bool rot_act = (s_ctx.handle == CanvasHandle::Rotate);
        dl->AddCircle(rot_pos, CR+1.5f, rot_act ? hdl_hov : hdl_col);
        float rdist = sqrtf((mpos.x-rot_pos.x)*(mpos.x-rot_pos.x) +
                            (mpos.y-rot_pos.y)*(mpos.y-rot_pos.y));
        if (rdist <= CR+5.f && lclick && s_ctx.handle == CanvasHandle::None) {
            begin_drag(CanvasHandle::Rotate);
            s_ctx.start_rot = cl.rotation;
        }

        // Corners (proportional scale)
        if (draw_corner_h(bx0, by0, CanvasHandle::CornerTL)) {
            begin_drag(CanvasHandle::CornerTL);
            s_ctx.start_scale_x = cl.scale_x; s_ctx.start_scale_y = cl.scale_y;
            s_ctx.start_bbox_y0 = by0;         s_ctx.start_bbox_y1 = by1;
        }
        if (draw_corner_h(bx1, by0, CanvasHandle::CornerTR)) {
            begin_drag(CanvasHandle::CornerTR);
            s_ctx.start_scale_x = cl.scale_x; s_ctx.start_scale_y = cl.scale_y;
            s_ctx.start_bbox_y0 = by0;         s_ctx.start_bbox_y1 = by1;
        }
        if (draw_corner_h(bx1, by1, CanvasHandle::CornerBR)) {
            begin_drag(CanvasHandle::CornerBR);
            s_ctx.start_scale_x = cl.scale_x; s_ctx.start_scale_y = cl.scale_y;
            s_ctx.start_bbox_y0 = by0;         s_ctx.start_bbox_y1 = by1;
        }
        if (draw_corner_h(bx0, by1, CanvasHandle::CornerBL)) {
            begin_drag(CanvasHandle::CornerBL);
            s_ctx.start_scale_x = cl.scale_x; s_ctx.start_scale_y = cl.scale_y;
            s_ctx.start_bbox_y0 = by0;         s_ctx.start_bbox_y1 = by1;
        }
        // Edges
        if (draw_edge_h(vmx, by0, false, CanvasHandle::EdgeT)) {
            begin_drag(CanvasHandle::EdgeT);
            s_ctx.start_scale_y = cl.scale_y; s_ctx.start_bbox_y0 = by0; s_ctx.start_bbox_y1 = by1;
        }
        if (draw_edge_h(vmx, by1, false, CanvasHandle::EdgeB)) {
            begin_drag(CanvasHandle::EdgeB);
            s_ctx.start_scale_y = cl.scale_y; s_ctx.start_bbox_y0 = by0; s_ctx.start_bbox_y1 = by1;
        }
        if (draw_edge_h(bx0, vmy, true, CanvasHandle::EdgeL)) {
            begin_drag(CanvasHandle::EdgeL);
            s_ctx.start_scale_x = cl.scale_x; s_ctx.start_bbox_x0 = bx0; s_ctx.start_bbox_x1 = bx1;
        }
        if (draw_edge_h(bx1, vmy, true, CanvasHandle::EdgeR)) {
            begin_drag(CanvasHandle::EdgeR);
            s_ctx.start_scale_x = cl.scale_x; s_ctx.start_bbox_x0 = bx0; s_ctx.start_bbox_x1 = bx1;
        }

        // Interior → move
        bool in_vid = mpos.x > bx0+CR*2 && mpos.x < bx1-CR*2 &&
                      mpos.y > by0+CR*2 && mpos.y < by1-CR*2;
        if (in_vid) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            if (lclick && s_ctx.handle == CanvasHandle::None) {
                begin_drag(CanvasHandle::Body);
                s_ctx.start_pos_x = cl.pos_x; s_ctx.start_pos_y = cl.pos_y;
            }
        }

        // Apply video drag
        if (drag_active && s_ctx.track_idx == state.selected_track &&
            s_ctx.clip_idx == state.selected_clip && cl.clip_type == ClipType::Video) {
            float dmx = mpos.x - s_ctx.drag_sx;
            float dmy = mpos.y - s_ctx.drag_sy;
            Clip& mc = state.tracks[s_ctx.track_idx].clips[s_ctx.clip_idx];
            float orig_h = s_ctx.start_bbox_y1 - s_ctx.start_bbox_y0;
            float orig_w = s_ctx.start_bbox_x1 - s_ctx.start_bbox_x0;
            switch (s_ctx.handle) {
                case CanvasHandle::Body:
                    mc.pos_x = fmaxf(-1.f, fminf(2.f, s_ctx.start_pos_x + dmx/w));
                    mc.pos_y = fmaxf(-1.f, fminf(2.f, s_ctx.start_pos_y + dmy/h));
                    break;
                case CanvasHandle::Rotate: {
                    float ang0 = atan2f(s_ctx.drag_sy - vcy, s_ctx.drag_sx - vcx);
                    float ang1 = atan2f(mpos.y - vcy,        mpos.x - vcx);
                    mc.rotation = fmodf(s_ctx.start_rot + (ang1-ang0)*180.f/3.14159f, 360.f);
                    break;
                }
                case CanvasHandle::CornerTL: case CanvasHandle::CornerTR: {
                    if (orig_h > 0.f) {
                        float scale = (orig_h - dmy) / orig_h;
                        mc.scale_x = fmaxf(0.05f, s_ctx.start_scale_x * scale);
                        mc.scale_y = fmaxf(0.05f, s_ctx.start_scale_y * scale);
                    }
                    break;
                }
                case CanvasHandle::CornerBL: case CanvasHandle::CornerBR: {
                    if (orig_h > 0.f) {
                        float scale = (orig_h + dmy) / orig_h;
                        mc.scale_x = fmaxf(0.05f, s_ctx.start_scale_x * scale);
                        mc.scale_y = fmaxf(0.05f, s_ctx.start_scale_y * scale);
                    }
                    break;
                }
                case CanvasHandle::EdgeT:
                    if (orig_h > 0.f) mc.scale_y = fmaxf(0.05f, s_ctx.start_scale_y * (orig_h-dmy)/orig_h);
                    break;
                case CanvasHandle::EdgeB:
                    if (orig_h > 0.f) mc.scale_y = fmaxf(0.05f, s_ctx.start_scale_y * (orig_h+dmy)/orig_h);
                    break;
                case CanvasHandle::EdgeL:
                    if (orig_w > 0.f) mc.scale_x = fmaxf(0.05f, s_ctx.start_scale_x * (orig_w-dmx)/orig_w);
                    break;
                case CanvasHandle::EdgeR:
                    if (orig_w > 0.f) mc.scale_x = fmaxf(0.05f, s_ctx.start_scale_x * (orig_w+dmx)/orig_w);
                    break;
                default: break;
            }
            s_ctx.dirty = true;

            // Center snap guides for move
            if (s_ctx.handle == CanvasHandle::Body) {
                float cx3 = mc.pos_x * w + p.x, cy3 = mc.pos_y * h + p.y;
                if (fabsf(cx3 - (p.x+w*0.5f)) < 6.f) {
                    mc.pos_x = 0.5f;
                    dl->AddLine({p.x+w*0.5f, p.y}, {p.x+w*0.5f, p.y+h}, snap_col);
                }
                if (fabsf(cy3 - (p.y+h*0.5f)) < 6.f) {
                    mc.pos_y = 0.5f;
                    dl->AddLine({p.x, p.y+h*0.5f}, {p.x+w, p.y+h*0.5f}, snap_col);
                }
            }
        }
    }

    // ── Background clip ───────────────────────────────────────────────────────
    if (cl.clip_type == ClipType::Background) {
        float px2 = cl.eval_prop("pos_x",   state.playhead) * w + p.x;
        float py2 = cl.eval_prop("pos_y",   state.playhead) * h + p.y;
        float sx2 = cl.eval_prop("scale_x", state.playhead);
        float sy2 = cl.eval_prop("scale_y", state.playhead);
        float hw2 = w * sx2 * 0.5f, hh2 = h * sy2 * 0.5f;
        float bx0 = px2 - hw2, by0 = py2 - hh2;
        float bx1 = px2 + hw2, by1 = py2 + hh2;
        float vmx2 = (bx0+bx1)*0.5f, vmy2 = (by0+by1)*0.5f;

        // Solid box
        dl->AddRect({bx0, by0}, {bx1, by1}, box_col, 0.f, 0, 1.5f);

        // Rotation handle
        ImVec2 rot_pos2 = {vmx2, by0 - ROT_DIST};
        dl->AddLine({vmx2, by0}, rot_pos2, IM_COL32(255,255,255,80));
        dl->AddCircleFilled(rot_pos2, CR+1.5f, IM_COL32(0,0,0,120));
        bool rot_act2 = (s_ctx.handle == CanvasHandle::Rotate);
        dl->AddCircle(rot_pos2, CR+1.5f, rot_act2 ? hdl_hov : hdl_col);
        float rdist2 = sqrtf((mpos.x-rot_pos2.x)*(mpos.x-rot_pos2.x) +
                             (mpos.y-rot_pos2.y)*(mpos.y-rot_pos2.y));
        if (rdist2 <= CR+5.f && lclick && s_ctx.handle == CanvasHandle::None) {
            begin_drag(CanvasHandle::Rotate);
            s_ctx.start_rot = cl.rotation;
        }

        // Corners (proportional scale)
        if (draw_corner_h(bx0, by0, CanvasHandle::CornerTL)) {
            begin_drag(CanvasHandle::CornerTL);
            s_ctx.start_scale_x = cl.scale_x; s_ctx.start_scale_y = cl.scale_y;
            s_ctx.start_bbox_y0 = by0;         s_ctx.start_bbox_y1 = by1;
        }
        if (draw_corner_h(bx1, by0, CanvasHandle::CornerTR)) {
            begin_drag(CanvasHandle::CornerTR);
            s_ctx.start_scale_x = cl.scale_x; s_ctx.start_scale_y = cl.scale_y;
            s_ctx.start_bbox_y0 = by0;         s_ctx.start_bbox_y1 = by1;
        }
        if (draw_corner_h(bx1, by1, CanvasHandle::CornerBR)) {
            begin_drag(CanvasHandle::CornerBR);
            s_ctx.start_scale_x = cl.scale_x; s_ctx.start_scale_y = cl.scale_y;
            s_ctx.start_bbox_y0 = by0;         s_ctx.start_bbox_y1 = by1;
        }
        if (draw_corner_h(bx0, by1, CanvasHandle::CornerBL)) {
            begin_drag(CanvasHandle::CornerBL);
            s_ctx.start_scale_x = cl.scale_x; s_ctx.start_scale_y = cl.scale_y;
            s_ctx.start_bbox_y0 = by0;         s_ctx.start_bbox_y1 = by1;
        }
        // Edges
        if (draw_edge_h(vmx2, by0, false, CanvasHandle::EdgeT)) {
            begin_drag(CanvasHandle::EdgeT);
            s_ctx.start_scale_y = cl.scale_y; s_ctx.start_bbox_y0 = by0; s_ctx.start_bbox_y1 = by1;
        }
        if (draw_edge_h(vmx2, by1, false, CanvasHandle::EdgeB)) {
            begin_drag(CanvasHandle::EdgeB);
            s_ctx.start_scale_y = cl.scale_y; s_ctx.start_bbox_y0 = by0; s_ctx.start_bbox_y1 = by1;
        }
        if (draw_edge_h(bx0, vmy2, true, CanvasHandle::EdgeL)) {
            begin_drag(CanvasHandle::EdgeL);
            s_ctx.start_scale_x = cl.scale_x; s_ctx.start_bbox_x0 = bx0; s_ctx.start_bbox_x1 = bx1;
        }
        if (draw_edge_h(bx1, vmy2, true, CanvasHandle::EdgeR)) {
            begin_drag(CanvasHandle::EdgeR);
            s_ctx.start_scale_x = cl.scale_x; s_ctx.start_bbox_x0 = bx0; s_ctx.start_bbox_x1 = bx1;
        }

        // Interior → move
        bool in_bg = mpos.x > bx0+CR*2 && mpos.x < bx1-CR*2 &&
                     mpos.y > by0+CR*2 && mpos.y < by1-CR*2;
        if (in_bg) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            if (lclick && s_ctx.handle == CanvasHandle::None) {
                begin_drag(CanvasHandle::Body);
                s_ctx.start_pos_x = cl.pos_x; s_ctx.start_pos_y = cl.pos_y;
            }
        }

        // Apply BG drag (same logic as video)
        if (drag_active && s_ctx.track_idx == state.selected_track &&
            s_ctx.clip_idx == state.selected_clip && cl.clip_type == ClipType::Background) {
            float dmx = mpos.x - s_ctx.drag_sx;
            float dmy = mpos.y - s_ctx.drag_sy;
            Clip& mc = state.tracks[s_ctx.track_idx].clips[s_ctx.clip_idx];
            float orig_h = s_ctx.start_bbox_y1 - s_ctx.start_bbox_y0;
            float orig_w = s_ctx.start_bbox_x1 - s_ctx.start_bbox_x0;
            switch (s_ctx.handle) {
                case CanvasHandle::Body:
                    mc.pos_x = fmaxf(-1.f, fminf(2.f, s_ctx.start_pos_x + dmx/w));
                    mc.pos_y = fmaxf(-1.f, fminf(2.f, s_ctx.start_pos_y + dmy/h));
                    break;
                case CanvasHandle::Rotate: {
                    float ang0 = atan2f(s_ctx.drag_sy - py2, s_ctx.drag_sx - px2);
                    float ang1 = atan2f(mpos.y - py2,        mpos.x - px2);
                    mc.rotation = fmodf(s_ctx.start_rot + (ang1-ang0)*180.f/3.14159f, 360.f);
                    break;
                }
                case CanvasHandle::CornerTL: case CanvasHandle::CornerTR: {
                    if (orig_h > 0.f) {
                        float scale = (orig_h - dmy) / orig_h;
                        mc.scale_x = fmaxf(0.05f, s_ctx.start_scale_x * scale);
                        mc.scale_y = fmaxf(0.05f, s_ctx.start_scale_y * scale);
                    }
                    break;
                }
                case CanvasHandle::CornerBL: case CanvasHandle::CornerBR: {
                    if (orig_h > 0.f) {
                        float scale = (orig_h + dmy) / orig_h;
                        mc.scale_x = fmaxf(0.05f, s_ctx.start_scale_x * scale);
                        mc.scale_y = fmaxf(0.05f, s_ctx.start_scale_y * scale);
                    }
                    break;
                }
                case CanvasHandle::EdgeT:
                    if (orig_h > 0.f) mc.scale_y = fmaxf(0.05f, s_ctx.start_scale_y * (orig_h-dmy)/orig_h);
                    break;
                case CanvasHandle::EdgeB:
                    if (orig_h > 0.f) mc.scale_y = fmaxf(0.05f, s_ctx.start_scale_y * (orig_h+dmy)/orig_h);
                    break;
                case CanvasHandle::EdgeL:
                    if (orig_w > 0.f) mc.scale_x = fmaxf(0.05f, s_ctx.start_scale_x * (orig_w-dmx)/orig_w);
                    break;
                case CanvasHandle::EdgeR:
                    if (orig_w > 0.f) mc.scale_x = fmaxf(0.05f, s_ctx.start_scale_x * (orig_w+dmx)/orig_w);
                    break;
                default: break;
            }
            s_ctx.dirty = true;

            // Center snap guides for move
            if (s_ctx.handle == CanvasHandle::Body) {
                float cx3 = mc.pos_x * w + p.x, cy3 = mc.pos_y * h + p.y;
                if (fabsf(cx3 - (p.x+w*0.5f)) < 6.f) {
                    mc.pos_x = 0.5f;
                    dl->AddLine({p.x+w*0.5f, p.y}, {p.x+w*0.5f, p.y+h}, snap_col);
                }
                if (fabsf(cy3 - (p.y+h*0.5f)) < 6.f) {
                    mc.pos_y = 0.5f;
                    dl->AddLine({p.x, p.y+h*0.5f}, {p.x+w, p.y+h*0.5f}, snap_col);
                }
            }
        }
    }

    // ── Text / subtitle / lyrics box ─────────────────────────────────────────
    if (cl.clip_type == ClipType::Text || cl.clip_type == ClipType::Subtitle ||
        cl.clip_type == ClipType::Lyrics) {
        uint64_t tl_key = ((uint64_t)state.selected_track << 32) | (uint32_t)state.selected_clip;
        auto it = s_text_layouts.find(tl_key);
        if (it == s_text_layouts.end() || !it->second.valid) return;
        const TextLayout& tl = it->second;

        float bx0 = tl.x0, by0 = tl.y0, bx1 = tl.x1, by1 = tl.y1;
        float tmx = (bx0+bx1)*0.5f, tmy = (by0+by1)*0.5f;

        // Solid box
        dl->AddRect({bx0, by0}, {bx1, by1}, box_col, 0.f, 0, 1.5f);

        // Corners → scale font size
        if (draw_corner_h(bx0, by0, CanvasHandle::CornerTL)) {
            begin_drag(CanvasHandle::CornerTL);
            s_ctx.start_font_size = cl.font_size > 0.f ? cl.font_size : 0.09f;
            s_ctx.start_bbox_y0 = by0; s_ctx.start_bbox_y1 = by1;
        }
        if (draw_corner_h(bx1, by0, CanvasHandle::CornerTR)) {
            begin_drag(CanvasHandle::CornerTR);
            s_ctx.start_font_size = cl.font_size > 0.f ? cl.font_size : 0.09f;
            s_ctx.start_bbox_y0 = by0; s_ctx.start_bbox_y1 = by1;
        }
        if (draw_corner_h(bx1, by1, CanvasHandle::CornerBR)) {
            begin_drag(CanvasHandle::CornerBR);
            s_ctx.start_font_size = cl.font_size > 0.f ? cl.font_size : 0.09f;
            s_ctx.start_bbox_y0 = by0; s_ctx.start_bbox_y1 = by1;
        }
        if (draw_corner_h(bx0, by1, CanvasHandle::CornerBL)) {
            begin_drag(CanvasHandle::CornerBL);
            s_ctx.start_font_size = cl.font_size > 0.f ? cl.font_size : 0.09f;
            s_ctx.start_bbox_y0 = by0; s_ctx.start_bbox_y1 = by1;
        }

        // Left/Right edges → wrap width
        if (draw_edge_h(bx0, tmy, true, CanvasHandle::EdgeL)) {
            begin_drag(CanvasHandle::EdgeL);
            s_ctx.start_wrap_w = cl.sub_wrap_w; s_ctx.start_anchor = cl.sub_anchor_h;
            s_ctx.start_bbox_x0 = bx0; s_ctx.start_bbox_x1 = bx1;
        }
        if (draw_edge_h(bx1, tmy, true, CanvasHandle::EdgeR)) {
            begin_drag(CanvasHandle::EdgeR);
            s_ctx.start_wrap_w = cl.sub_wrap_w; s_ctx.start_anchor = cl.sub_anchor_h;
            s_ctx.start_bbox_x0 = bx0; s_ctx.start_bbox_x1 = bx1;
        }

        // Top/Bottom edges → vertical nudge
        if (draw_edge_h(tmx, by0, false, CanvasHandle::EdgeT)) {
            begin_drag(CanvasHandle::EdgeT);
            s_ctx.start_pos_y = ((by0+by1)*0.5f - p.y) / h;
        }
        if (draw_edge_h(tmx, by1, false, CanvasHandle::EdgeB)) {
            begin_drag(CanvasHandle::EdgeB);
            s_ctx.start_pos_y = ((by0+by1)*0.5f - p.y) / h;
        }

        // Interior → move
        bool in_txt = mpos.x > bx0+CR*2 && mpos.x < bx1-CR*2 &&
                      mpos.y > by0+CR*2 && mpos.y < by1-CR*2;
        if (in_txt) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            if (lclick && s_ctx.handle == CanvasHandle::None) {
                begin_drag(CanvasHandle::Body);
                s_ctx.start_pos_x  = ((bx0+bx1)*0.5f - p.x) / w;
                s_ctx.start_pos_y  = ((by0+by1)*0.5f - p.y) / h;
                s_ctx.start_wrap_w = cl.sub_wrap_w;
                s_ctx.start_anchor = cl.sub_anchor_h;
                s_ctx.start_bbox_x0 = bx0; s_ctx.start_bbox_x1 = bx1;
                s_ctx.start_bbox_y0 = by0; s_ctx.start_bbox_y1 = by1;
            }
        }

        // Apply text drag
        if (drag_active && s_ctx.track_idx == state.selected_track &&
            s_ctx.clip_idx == state.selected_clip &&
            (cl.clip_type == ClipType::Text || cl.clip_type == ClipType::Subtitle ||
             cl.clip_type == ClipType::Lyrics)) {
            float dmx = mpos.x - s_ctx.drag_sx;
            float dmy = mpos.y - s_ctx.drag_sy;
            Clip& mc = state.tracks[s_ctx.track_idx].clips[s_ctx.clip_idx];
            float orig_bbox_h = s_ctx.start_bbox_y1 - s_ctx.start_bbox_y0;

            switch (s_ctx.handle) {
                case CanvasHandle::Body:
                    mc.sub_pos      = 3;
                    mc.sub_anchor_h = 1;
                    mc.sub_pos_x    = fmaxf(SAFE_SIDE, fminf(1.f-SAFE_SIDE,
                                        s_ctx.start_pos_x + dmx/w));
                    mc.sub_pos_y    = fmaxf(SAFE_TOP,  fminf(1.f-SAFE_BOT,
                                        s_ctx.start_pos_y + dmy/h));
                    break;
                case CanvasHandle::CornerTL: case CanvasHandle::CornerTR:
                    if (orig_bbox_h > 0.f) {
                        float scale = (orig_bbox_h - dmy) / orig_bbox_h;
                        mc.font_size = fmaxf(0.02f, fminf(0.5f, s_ctx.start_font_size * scale));
                    }
                    break;
                case CanvasHandle::CornerBL: case CanvasHandle::CornerBR:
                    if (orig_bbox_h > 0.f) {
                        float scale = (orig_bbox_h + dmy) / orig_bbox_h;
                        mc.font_size = fmaxf(0.02f, fminf(0.5f, s_ctx.start_font_size * scale));
                    }
                    break;
                case CanvasHandle::EdgeL: {
                    float new_x0 = s_ctx.start_bbox_x0 + dmx;
                    float new_w  = s_ctx.start_bbox_x1 - new_x0;
                    if (new_w > 20.f) {
                        mc.sub_wrap_w   = fmaxf(0.08f, fminf(0.98f, new_w/w));
                        mc.sub_anchor_h = 1;
                        mc.sub_pos_x    = fmaxf(0.f, fminf(1.f,
                            (new_x0 + new_w*0.5f - p.x) / w));
                    }
                    break;
                }
                case CanvasHandle::EdgeR: {
                    float new_x1 = s_ctx.start_bbox_x1 + dmx;
                    float new_w  = new_x1 - s_ctx.start_bbox_x0;
                    if (new_w > 20.f) {
                        mc.sub_wrap_w   = fmaxf(0.08f, fminf(0.98f, new_w/w));
                        mc.sub_anchor_h = 1;
                        mc.sub_pos_x    = fmaxf(0.f, fminf(1.f,
                            (s_ctx.start_bbox_x0 + new_w*0.5f - p.x) / w));
                    }
                    break;
                }
                case CanvasHandle::EdgeT: case CanvasHandle::EdgeB:
                    mc.sub_pos   = 3;
                    mc.sub_pos_y = fmaxf(SAFE_TOP, fminf(1.f-SAFE_BOT,
                                    s_ctx.start_pos_y + dmy/h));
                    break;
                default: break;
            }
            s_ctx.dirty = true;

            // Center snap for text body move
            if (s_ctx.handle == CanvasHandle::Body) {
                float cx3 = mc.sub_pos_x * w + p.x;
                if (fabsf(cx3 - (p.x+w*0.5f)) < 8.f) {
                    mc.sub_pos_x = 0.5f;
                    dl->AddLine({p.x+w*0.5f, p.y}, {p.x+w*0.5f, p.y+h}, snap_col);
                }
            }
        }
    }

    // Release drag → push history once
    if (!ldown && s_ctx.handle != CanvasHandle::None) {
        if (s_ctx.dirty) {
            const char* act =
                (s_ctx.handle == CanvasHandle::Body)                              ? "Move clip"   :
                (s_ctx.handle == CanvasHandle::Rotate)                            ? "Rotate clip" :
                (s_ctx.handle == CanvasHandle::CornerTL || s_ctx.handle == CanvasHandle::CornerTR ||
                 s_ctx.handle == CanvasHandle::CornerBL || s_ctx.handle == CanvasHandle::CornerBR) ? "Resize clip" :
                (s_ctx.handle == CanvasHandle::EdgeL    || s_ctx.handle == CanvasHandle::EdgeR)    ? "Wrap width"  :
                "Adjust clip";
            history_push(state, act);
        }
        s_ctx = CanvasTransform{};
    }
}

// ── Preview ───────────────────────────────────────────────────────────────────

void draw_preview(AppState& state, ImVec2 p, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Stage background: fine transparency checker so chroma-keyed holes look intentional.
    // Two very dark grays at 10px tile size — subtle enough to not distract, clear enough
    // to distinguish from actual black content.
    {
        const float CSZ = 10.f;
        const ImU32 CA  = IM_COL32(22, 22, 22, 255);
        const ImU32 CB  = IM_COL32(32, 32, 32, 255);
        dl->AddRectFilled(p, {p.x+w, p.y+h}, CA, 2.f);
        dl->PushClipRect(p, {p.x+w, p.y+h}, true);
        int nx = (int)(w / CSZ) + 2, ny = (int)(h / CSZ) + 2;
        for (int iy = 0; iy < ny; ++iy)
            for (int ix = 0; ix < nx; ++ix)
                if ((ix + iy) % 2 == 0)
                    dl->AddRectFilled({p.x + ix*CSZ, p.y + iy*CSZ},
                                     {p.x + ix*CSZ + CSZ, p.y + iy*CSZ + CSZ}, CB);
        dl->PopClipRect();
    }

    // Clip everything to the video frame — text/effects never bleed into surrounding UI.
    dl->PushClipRect(p, {p.x+w, p.y+h}, true);


    // Empty state prompt
    if (state.tracks.empty()) {
        ImGui::PushFont(g_font_bold);
        float hint_sz = ImGui::GetFontSize() * 1.3f;
        const char* hint = "Drop a file to start";
        ImVec2 hsz = ImGui::GetFont()->CalcTextSizeA(hint_sz, FLT_MAX, -1.f, hint);
        dl->AddText(ImGui::GetFont(), hint_sz,
            {p.x + (w - hsz.x) * 0.5f, p.y + h * 0.5f - hsz.y},
            to_u32(Col::muted), hint);
        ImGui::PopFont();

        const char* sub = "or  File \xe2\x86\x92 Import Audio";
        ImVec2 ssz = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.f, sub);
        dl->AddText({p.x + (w - ssz.x) * 0.5f, p.y + h * 0.5f + 8.f},
            to_u32(Col::dim), sub);

        dl->AddRect(p, {p.x+w, p.y+h}, to_u32(Col::line), 2.f);
        return;
    }

    // Unified z-order pass: track index 0 = frontmost, so iterate high→low (background first).
    // Each track draws whichever clip type is active — video and text are interleaved correctly.
    ImVec2 mpos  = ImGui::GetIO().MousePos;
    bool   lclick = ImGui::IsMouseClicked(0);

    // Click-to-select using tight bboxes: video computed inline, text from previous-frame layouts.
    bool in_preview_area = mpos.x >= p.x && mpos.x <= p.x+w &&
                           mpos.y >= p.y && mpos.y <= p.y+h;

    if (lclick && in_preview_area && s_ctx.handle == CanvasHandle::None) {
        struct HitCandidate { int ti, ci; float area; };
        std::vector<HitCandidate> hits;
        for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
            auto& tr2 = state.tracks[ti];
            if (!tr2.visible) continue;
            for (int ci = 0; ci < (int)tr2.clips.size(); ++ci) {
                auto& cl2 = tr2.clips[ci];
                if (state.playhead < cl2.start || state.playhead >= cl2.end) continue;
                if (cl2.clip_type == ClipType::Video) {
                    float hbx0, hby0, hbx1, hby1;
                    compute_video_bbox(state, cl2, p, w, h, hbx0, hby0, hbx1, hby1);
                    if (mpos.x >= hbx0 && mpos.x <= hbx1 &&
                        mpos.y >= hby0 && mpos.y <= hby1)
                        hits.push_back({ti, ci, (hbx1-hbx0)*(hby1-hby0)});
                } else if (cl2.clip_type == ClipType::Background) {
                    float hpx = cl2.eval_prop("pos_x",   state.playhead) * w + p.x;
                    float hpy = cl2.eval_prop("pos_y",   state.playhead) * h + p.y;
                    float hsx = cl2.eval_prop("scale_x", state.playhead);
                    float hsy = cl2.eval_prop("scale_y", state.playhead);
                    float hhw = w * hsx * 0.5f, hhh = h * hsy * 0.5f;
                    float hbx0 = hpx - hhw, hby0 = hpy - hhh;
                    float hbx1 = hpx + hhw, hby1 = hpy + hhh;
                    if (mpos.x >= hbx0 && mpos.x <= hbx1 &&
                        mpos.y >= hby0 && mpos.y <= hby1)
                        hits.push_back({ti, ci, (hbx1-hbx0)*(hby1-hby0)});
                } else if (cl2.clip_type == ClipType::Text ||
                           cl2.clip_type == ClipType::Subtitle ||
                           cl2.clip_type == ClipType::Lyrics) {
                    uint64_t tk = ((uint64_t)ti << 32) | (uint32_t)ci;
                    auto it2 = s_text_layouts.find(tk);
                    if (it2 != s_text_layouts.end() && it2->second.valid) {
                        auto& tl2 = it2->second;
                        if (mpos.x >= tl2.x0 && mpos.x <= tl2.x1 &&
                            mpos.y >= tl2.y0 && mpos.y <= tl2.y1)
                            hits.push_back({ti, ci, (tl2.x1-tl2.x0)*(tl2.y1-tl2.y0)});
                    }
                }
            }
        }
        std::sort(hits.begin(), hits.end(), [](const HitCandidate& a, const HitCandidate& b) {
            if (a.area != b.area) return a.area < b.area;
            return a.ti < b.ti;
        });
        if (!hits.empty()) {
            if (state.selected_track != hits[0].ti || state.selected_clip != hits[0].ci) {
                state.selected_track = hits[0].ti;
                state.selected_clip  = hits[0].ci;
                state.request_scroll_to_clip = true;
            }
        } else {
            state.selected_track = -1;
            state.selected_clip  = -1;
        }
    }

    float lookahead = ImGui::GetIO().DeltaTime;
    float t_anim    = state.playing ? (float)ImGui::GetTime() : state.playhead;

    // Collect global creative FX (for full-frame effects — LightLeak, grade, etc.)
    CreativeFXAccum global_cfx = collect_creative_fx(state, state.playhead, (int)state.tracks.size());

    // ── Pass 1: BG clips (ImGui draw list) + Video clips (→ scene FBO) ────────
    scene_begin((int)w, (int)h);

    for (int ti = (int)state.tracks.size() - 1; ti >= 0; --ti) {
        auto& track = state.tracks[ti];
        if (!track.visible) continue;

        // ── Background clip ────────────────────────────────────────────────────
        for (auto& cl : track.clips) {
            if (cl.clip_type != ClipType::Background) continue;
            if (state.playhead < cl.start || state.playhead >= cl.end) continue;
            if (cl.text.empty()) break;

            int bg_slot = ti % MAX_BG_SLOTS;
            uintptr_t tex = bg_render_to_texture(cl.text.c_str(), bg_slot,
                (int)w, (int)h, t_anim, cl.bg_speed, cl.bg_intensity,
                cl.bg_c1, cl.bg_c2, cl.bg_c3);
            if (tex) {
                float px    = cl.eval_prop("pos_x",    state.playhead);
                float py    = cl.eval_prop("pos_y",    state.playhead);
                float sx    = cl.eval_prop("scale_x",  state.playhead);
                float sy    = cl.eval_prop("scale_y",  state.playhead);
                float rot   = cl.eval_prop("rotation", state.playhead);
                float alpha = cl.eval_prop("opacity",  state.playhead);
                float cx = px * w, cy = py * h;
                float hw = w * sx * 0.5f, hh = h * sy * 0.5f;
                float rad = rot * 3.14159265f / 180.f;
                scene_add_layer(tex, cx, cy, hw, hh, cosf(rad), sinf(rad), alpha);
            }
            break;
        }

        // ── Video clip ─────────────────────────────────────────────────────────
        {
            // Helper: composite a video clip into the scene FBO.
            // Global FX are collected only for CPU-side datamosh and ZoomPunch;
            // the GPU shader pass for global FX is applied once after all clips
            // via scene_apply_fx, not here.
            auto draw_vid_clip = [&](const Clip* cl_ptr, float at_time, float alpha_mul) {
                if (!cl_ptr) return;
                int slot = slot_for_video(const_cast<AppState&>(state),
                               clip_slot_key(cl_ptr->text, cl_ptr->start), cl_ptr->text);
                float src_t = cl_ptr->in_point + (at_time - cl_ptr->start) * cl_ptr->speed;

                // Glass-only cfx for CPU-side datamosh and ZoomPunch — global FX
                // are applied once to the full composite via scene_apply_fx, not per-clip.
                CreativeFXAccum cfx = collect_glass_fx(state, at_time, ti);
                if (slot >= 0) {
                    PixelFX pfx;
                    pfx.bg_remove_on       = cl_ptr->bg_remove_on &&
                                             cl_ptr->bg_remove_status == BgRemoveStatus::Ready;
                    pfx.bg_remove_mask_dir = cl_ptr->bg_remove_mask_dir;
                    pfx.bg_remove_softness = cl_ptr->bg_remove_softness;
                    pfx.bg_remove_box_on   = cl_ptr->bg_remove_box_on;
                    pfx.bg_remove_box_l    = cl_ptr->bg_remove_box_l;
                    pfx.bg_remove_box_r    = cl_ptr->bg_remove_box_r;
                    pfx.bg_remove_box_t    = cl_ptr->bg_remove_box_t;
                    pfx.bg_remove_box_b    = cl_ptr->bg_remove_box_b;
                    pfx.datamosh_on        = cfx.datamosh_on;
                    pfx.datamosh_intensity = cfx.datamosh_intensity;
                    pfx.datamosh_spread    = cfx.datamosh_spread;
                    pfx.time               = t_anim;
                    video_set_pixel_fx(slot, pfx);
                }

                uintptr_t tex = (slot >= 0 && video_is_open(slot))
                    ? video_get_texture(slot, (double)(src_t + lookahead)) : 0;
                if (!tex) return;

                // Glass FX: applied pre-composite to this clip only.
                if (slot >= 0) {
                    EffectAccum     glass_ea  = collect_glass_effects(state, at_time, ti);
                    CreativeFXAccum glass_cfx = collect_glass_fx     (state, at_time, ti);
                    if (glass_cfx.any_gen_fx || glass_cfx.any_cfx ||
                        glass_ea.any_color || glass_ea.any_blur ||
                        glass_ea.any_vignette || glass_ea.any_text) {
                        VideoInfo vi_g = video_info(slot);
                        tex = fx_apply(tex, slot, vi_g.width, vi_g.height, glass_ea, glass_cfx, t_anim);
                    }
                }

                // Glass BodyFX: from BodyFX sub-effects in glass MultiFX bricks on this track
                if (cl_ptr && slot >= 0) {
                    std::string mask_dir = bg_remove_proxy_dir(cl_ptr->source_id);
                    if (!mask_dir.empty()) {
                        float mask_fps = bg_remove_read_fps(mask_dir);
                        float src_t = cl_ptr->in_point + (at_time - cl_ptr->start) / cl_ptr->speed;
                        int frame_i = (int)(src_t * mask_fps);
                        for (auto& mfx_cl : state.tracks[ti].clips) {
                            if (mfx_cl.clip_type != ClipType::MultiFX) continue;
                            if (at_time < mfx_cl.start || at_time >= mfx_cl.end) continue;
                            if (!fx_clip_is_glass(state, ti, mfx_cl)) continue;
                            float rel = at_time - mfx_cl.start;
                            float parent_dur = mfx_cl.end - mfx_cl.start;
                            for (auto& se : mfx_cl.fx_chain) {
                                if (se.clip_type != ClipType::BodyFX) continue;
                                float se_end = (se.rel_end <= 0.f) ? parent_dur : se.rel_end;
                                if (rel < se.rel_start || rel >= se_end) continue;
                                unsigned mask_tex = body_fx_mask_texture(mask_dir, frame_i);
                                if (!mask_tex) continue;
                                VideoInfo vi_g = video_info(slot);
                                int bw = (vi_g.width  > 0) ? vi_g.width  : (int)w;
                                int bh = (vi_g.height > 0) ? vi_g.height : (int)h;
                                tex = body_fx_apply(se.body_fx_type, tex, mask_tex, bw, bh,
                                                    se.body_fx_params, se.body_fx_amount, t_anim);
                            }
                        }
                    }
                }

                // Runtime FX (hot-reload custom effect)
                if (cl_ptr && !cl_ptr->runtime_fx_id.empty()) {
                    VideoInfo vi_r = (slot >= 0) ? video_info(slot) : VideoInfo{};
                    int rw = (vi_r.width  > 0) ? vi_r.width  : (int)w;
                    int rh = (vi_r.height > 0) ? vi_r.height : (int)h;
                    tex = runtime_fx_apply(cl_ptr->runtime_fx_id, tex, rw, rh,
                                          cl_ptr->runtime_fx_params,
                                          cl_ptr->runtime_fx_amount, t_anim);
                }

                float px    = cl_ptr->eval_prop("pos_x",    at_time);
                float py    = cl_ptr->eval_prop("pos_y",    at_time);
                float sx    = cl_ptr->eval_prop("scale_x",  at_time);
                float sy    = cl_ptr->eval_prop("scale_y",  at_time);
                float rot   = cl_ptr->eval_prop("rotation", at_time);
                float alpha = cl_ptr->eval_prop("opacity",  at_time) * alpha_mul;
                VideoInfo vi = video_info(slot);
                float fit_w = w, fit_h = h;
                if (vi.width > 0 && vi.height > 0) {
                    float vid_asp = (float)vi.width / (float)vi.height;
                    float can_asp = w / h;
                    if (vid_asp > can_asp) { fit_w = w; fit_h = w / vid_asp; }
                    else                   { fit_h = h; fit_w = h * vid_asp; }
                }
                float cx = px * w, cy = py * h;  // canvas-relative (not ImGui-space)
                float hw = fit_w * sx * 0.5f, hh = fit_h * sy * 0.5f;
                float rad = rot * 3.14159265f / 180.f;
                float cos_r = cosf(rad), sin_r = sinf(rad);

                // ZoomPunch — per-clip transform (no GPU shader needed)
                if (cfx.zoom_on && !state.beats.empty()) {
                    float punch = 0.f;
                    float decay = fmaxf(0.05f, cfx.zoom_decay);
                    for (float bt : state.beats) {
                        if (bt <= at_time) {
                            float elapsed = at_time - bt;
                            punch = fmaxf(punch, cfx.zoom_strength * expf(-elapsed / decay));
                        }
                    }
                    if (punch > 0.001f) {
                        float sf = 1.f + punch;
                        hw *= sf; hh *= sf;
                        if (cfx.zoom_shake > 0.f) {
                            float tf = floorf(t_anim * 60.f);
                            float sa = cfx.zoom_shake * punch * w * 0.025f;
                            cx += sinf(tf * 127.1f) * sa;
                            cy += cosf(tf * 311.7f) * sa;
                        }
                    }
                }

                alpha = std::fmaxf(0.f, std::fminf(1.f, alpha));
                scene_add_layer(tex, cx, cy, hw, hh, cos_r, sin_r, alpha);
            };

            // Find the active video clip and check for transitions
            const Clip* active = nullptr;
            int active_ci = -1;
            for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
                auto& cl = track.clips[ci];
                if (cl.clip_type == ClipType::Video &&
                    state.playhead >= cl.start && state.playhead < cl.end)
                    { active = &cl; active_ci = ci; break; }
            }

            // Check if incoming clip has a transition that started before its start
            // Also show incoming clip B when playhead is inside its transition_post zone
            // (clip B hasn't started in [active] loop above, but needs to be visible)
            if (!active) {
                for (int ci = 1; ci < (int)track.clips.size(); ++ci) {
                    const Clip& prev = track.clips[ci - 1];
                    const Clip& cl   = track.clips[ci];
                    if (prev.clip_type != ClipType::Video || cl.clip_type != ClipType::Video) continue;
                    if (prev.transition_type == TransitionType::None || prev.transition_post <= 0.f) continue;
                    if (state.playhead >= cl.start && state.playhead < cl.start + prev.transition_post)
                        { active = &track.clips[ci]; active_ci = ci; break; }
                }
            }

            if (active) {
                bool in_trans_out = (active->transition_type != TransitionType::None &&
                                     active->transition_pre > 0.f &&
                                     state.playhead >= active->end - active->transition_pre);
                const Clip* next_cl = nullptr;
                if (in_trans_out && active_ci + 1 < (int)track.clips.size()) {
                    const Clip& nc = track.clips[active_ci + 1];
                    if (nc.clip_type == ClipType::Video) next_cl = &nc;
                }

                bool in_trans_in = false;
                const Clip* prev_cl = nullptr;
                if (!in_trans_out && active_ci > 0) {
                    const Clip& pc = track.clips[active_ci - 1];
                    if (pc.clip_type == ClipType::Video &&
                        pc.transition_type != TransitionType::None &&
                        pc.transition_post > 0.f &&
                        state.playhead < active->start + pc.transition_post) {
                        in_trans_in = true;
                        prev_cl = &pc;
                    }
                }

                if (in_trans_out && next_cl) {
                    // t_a: 0→1 over [end-pre .. end], t_b: 0→1 over [end .. end+post]
                    float pre = active->transition_pre, post = active->transition_post;
                    float cut = active->end;
                    float t_a = std::fmaxf(0.f, std::fminf(1.f, (state.playhead - (cut - pre)) / fmaxf(pre, 1e-5f)));
                    float t_b = std::fmaxf(0.f, std::fminf(1.f, (state.playhead - cut) / fmaxf(post, 1e-5f)));

                    if (active->transition_type == TransitionType::Dissolve) {
                        draw_vid_clip(active,  state.playhead, 1.f - t_a);
                        draw_vid_clip(next_cl, state.playhead, t_b > 0.f ? t_b : t_a); // blend once cut passes
                    } else if (active->transition_type == TransitionType::FadeBlack) {
                        draw_vid_clip(active,  state.playhead, 1.f - t_a);
                        draw_vid_clip(next_cl, state.playhead, t_b);
                    } else { // DipWhite
                        draw_vid_clip(active, state.playhead, 1.f - t_a);
                        float white_a = t_a * (1.f - t_b);
                        if (white_a > 0.01f)
                            scene_add_solid(1.f, 1.f, 1.f, white_a);
                        draw_vid_clip(next_cl, state.playhead, t_b);
                    }
                } else if (in_trans_in && prev_cl) {
                    float t = std::fmaxf(0.f, std::fminf(1.f,
                        (state.playhead - active->start) / fmaxf(prev_cl->transition_post, 1e-5f)));
                    if (prev_cl->transition_type == TransitionType::Dissolve) {
                        draw_vid_clip(prev_cl, std::fminf(state.playhead, prev_cl->end - 1e-4f), 1.f - t);
                        draw_vid_clip(active,  state.playhead, t);
                    } else if (prev_cl->transition_type == TransitionType::FadeBlack) {
                        draw_vid_clip(active, state.playhead, t);
                    } else { // DipWhite: white overlay fades out, clip B fades in
                        float white_a = 1.f - t;
                        if (white_a > 0.01f)
                            scene_add_solid(1.f, 1.f, 1.f, white_a);
                        draw_vid_clip(active, state.playhead, t);
                    }
                } else {
                    draw_vid_clip(active, state.playhead, 1.f);
                }
            }
        }
    }  // end Pass 1 track loop

    // ── Apply global FX to composited scene ──────────────────────────────────
    {
        EffectAccum global_ea = collect_effects(state, state.playhead, (int)state.tracks.size());
        scene_apply_fx((int)w, (int)h, global_ea, global_cfx, t_anim);
    }

    // ── Solid BodyFX bricks: post-composite pass ──────────────────────────────
    // Tracks iterate 0 (top) to N-1 (bottom). A solid BodyFX brick has no video
    // clip on its own track; it affects the composited scene below it.
    if (uintptr_t scene_tex = scene_result()) {
        uintptr_t final_tex = scene_tex;
        for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
            auto& bfx_track = state.tracks[ti];
            for (auto& bfx_cl : bfx_track.clips) {
                if (bfx_cl.clip_type != ClipType::BodyFX) continue;
                if (state.playhead < bfx_cl.start || state.playhead >= bfx_cl.end) continue;
                // Confirm solid (no video clip co-inhabiting this track at this time)
                bool is_glass = false;
                for (auto& tc : bfx_track.clips) {
                    if (tc.clip_type == ClipType::Video &&
                        state.playhead >= tc.start && state.playhead < tc.end)
                        { is_glass = true; break; }
                }
                if (is_glass) continue;
                // Find topmost video clip on a track below (higher index = lower in stack)
                const Clip* vid_cl = nullptr;
                for (int vi = ti + 1; vi < (int)state.tracks.size(); ++vi) {
                    for (auto& vc : state.tracks[vi].clips) {
                        if (vc.clip_type == ClipType::Video &&
                            state.playhead >= vc.start && state.playhead < vc.end)
                            { vid_cl = &vc; break; }
                    }
                    if (vid_cl) break;
                }
                if (!vid_cl) continue;
                std::string mask_dir = bg_remove_proxy_dir(vid_cl->source_id);
                if (mask_dir.empty()) continue;
                float mask_fps = bg_remove_read_fps(mask_dir);
                float src_t    = vid_cl->in_point + (state.playhead - vid_cl->start) / vid_cl->speed;
                int   frame_i  = (int)(src_t * mask_fps);
                unsigned mask_tex_id = body_fx_mask_texture(mask_dir, frame_i);
                if (!mask_tex_id) continue;
                final_tex = body_fx_apply(bfx_cl.body_fx_type, final_tex, mask_tex_id,
                                          (int)w, (int)h, bfx_cl.body_fx_params,
                                          bfx_cl.body_fx_amount, t_anim);
            }
        }

        // ── Draw scene FBO to ImGui draw list ─────────────────────────────────
        // Y-flip UVs: GL FBO t=0 is at the bottom, ImGui tl=(0,0) is at the top.
        dl->AddImageQuad(ImTextureRef((ImTextureID)final_tex),
            p,                      {p.x + w, p.y},
            {p.x + w, p.y + h},    {p.x, p.y + h},
            {0.f, 1.f}, {1.f, 1.f}, {1.f, 0.f}, {0.f, 0.f},
            IM_COL32_WHITE);
    }

    // ── Pass 2: Text clips (drawn on top of scene) ────────────────────────────
    int text_rendered = 0;
    for (int ti = (int)state.tracks.size() - 1; ti >= 0; --ti) {
        auto& track = state.tracks[ti];
        if (!track.visible) continue;

        // ── Text / subtitle clip ───────────────────────────────────────────────
        {
            const Clip* active = nullptr;
            int active_ci = -1;
            for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
                auto ct = track.clips[ci].clip_type;
                if (ct != ClipType::Text && ct != ClipType::Lyrics && ct != ClipType::Subtitle)
                    continue;
                if (state.playhead >= track.clips[ci].start &&
                    state.playhead <  track.clips[ci].end)
                    { active = &track.clips[ci]; active_ci = ci; break; }
            }
            const Clip* show = active;
            int show_ci = active_ci;
            if (!show && state.selected_track == ti && state.selected_clip >= 0 &&
                state.selected_clip < (int)track.clips.size()) {
                auto ct = track.clips[state.selected_clip].clip_type;
                if (ct == ClipType::Text || ct == ClipType::Lyrics || ct == ClipType::Subtitle) {
                    show    = &track.clips[state.selected_clip];
                    show_ci = state.selected_clip;
                }
            }
            if (!show) {
                // Only count this track toward the stacking offset if it's actually a text track
                // (has at least one Text/Lyrics/Subtitle clip). Pure video/audio/FX tracks must
                // not shift the vertical slot, or large-font presets like Cyberpunk get pushed
                // off-screen when multiple non-text tracks precede the Lyrics track.
                bool has_text_clips = false;
                for (auto& c : track.clips) {
                    auto ct = c.clip_type;
                    if (ct == ClipType::Text || ct == ClipType::Lyrics || ct == ClipType::Subtitle)
                        { has_text_clips = true; break; }
                }
                if (has_text_clips) ++text_rendered;
                continue;
            }

            // Hover preview: temporarily render with the hovered preset's style.
            ImGui::PushFont(g_font_black);
            ImFont* txt_font = ImGui::GetFont();
            float fsz    = show->font_size > 0.f ? show->font_size * h
                                                  : h * 0.055f;
            float line_h = fsz * 1.25f;

            // Word-wrap: break text into lines that fit sub_wrap_w * canvas width
            float max_line_w = fmaxf(40.f, show->sub_wrap_w * w);
            std::vector<std::string> txt_lines;
            {
                const char* src = show->text.c_str();
                const char* wp  = src;
                std::string cur;
                while (true) {
                    const char* ep = wp;
                    while (*ep && *ep != ' ') ++ep;
                    std::string word(wp, ep);
                    std::string test = cur.empty() ? word : cur + " " + word;
                    if (!cur.empty() &&
                        txt_font->CalcTextSizeA(fsz, FLT_MAX, -1.f, test.c_str()).x > max_line_w) {
                        txt_lines.push_back(cur);
                        cur = word;
                    } else {
                        cur = test;
                    }
                    if (!*ep) break;
                    wp = ep + 1;
                }
                if (!cur.empty()) txt_lines.push_back(cur);
                if (txt_lines.empty()) txt_lines.push_back("");
            }

            float block_h = txt_lines.size() * line_h;

            // Post-wrap font scale: ensure no rendered line overflows the canvas given the anchor.
            // Checks actual line widths (including trailing spaces/punctuation from transcription).
            {
                float max_fit_w;
                if (show->sub_anchor_h == 0)
                    max_fit_w = (1.f - show->sub_pos_x - SAFE_SIDE) * w;
                else if (show->sub_anchor_h == 2)
                    max_fit_w = (show->sub_pos_x - SAFE_SIDE) * w;
                else
                    max_fit_w = 2.f * fminf(show->sub_pos_x - SAFE_SIDE,
                                            1.f - show->sub_pos_x - SAFE_SIDE) * w;
                max_fit_w = fminf(max_fit_w, max_line_w);              // also cap at wrap column
                max_fit_w = fmaxf(max_fit_w, 40.f);                    // safety floor

                float max_rendered_w = 0.f;
                for (auto& ln : txt_lines)
                    max_rendered_w = fmaxf(max_rendered_w,
                        txt_font->CalcTextSizeA(fsz, FLT_MAX, -1.f, ln.c_str()).x);

                if (max_rendered_w > max_fit_w && max_rendered_w > 0.f) {
                    fsz     *= max_fit_w / max_rendered_w;
                    line_h   = fsz * 1.25f;
                    block_h  = txt_lines.size() * line_h;
                }
            }

            // Vertical slot positioning (uses block_h so multi-line is centred correctly)
            float sz_top = SAFE_TOP * h;
            float sz_bot = SAFE_BOT * h;
            float slot_h = fmaxf(40.f, block_h);
            float slot_y;
            if (show->sub_pos == 1)
                slot_y = p.y + h * 0.5f - block_h * 0.5f;
            else if (show->sub_pos == 2)
                slot_y = p.y + sz_top + text_rendered * slot_h;
            else if (show->sub_pos == 3)
                slot_y = p.y + show->sub_pos_y * h - block_h * 0.5f;
            else
                slot_y = p.y + h - sz_bot - block_h - text_rendered * slot_h;

            // Clamp to safe zone so text never lands under platform UI chrome.
            slot_y = fmaxf(p.y + sz_top, fminf(p.y + h - sz_bot - block_h, slot_y));

            // Kinetic typography
            float local_t  = state.playhead - show->start;
            float clip_dur = show->end - show->start;
            float fade_in  = fminf(0.25f, clip_dur * 0.3f);
            float fade_out = fminf(0.25f, clip_dur * 0.2f);

            EffectAccum text_fx = collect_effects(state, state.playhead, ti);

            float anim_dx    = 0.f;
            float anim_dy    = 0.f;
            float anim_alpha = 1.f;

            AnimStyle eff_style = (show->clip_style != AnimStyle::None)
                                  ? show->clip_style : state.style;

            if (active_ci >= 0) {
                switch (eff_style) {
                    case AnimStyle::Fade:
                        if (local_t < fade_in)       anim_alpha = local_t / fade_in;
                        else if (local_t > clip_dur - fade_out)
                                                      anim_alpha = (clip_dur - local_t) / fade_out;
                        break;
                    case AnimStyle::Glitch: {
                        float decay = fmaxf(0.f, 1.f - local_t / 0.5f);
                        anim_dx = sinf(local_t * 97.f + sinf(local_t * 53.f) * 31.f) * 12.f * decay;
                        break;
                    }
                    case AnimStyle::Typewriter:
                        if (local_t < fade_in) {
                            anim_alpha = local_t / fade_in;
                            anim_dy    = (fade_in - local_t) / fade_in * (-8.f);
                        }
                        break;
                    case AnimStyle::Bounce: {
                        float bd = fminf(0.6f, clip_dur);
                        if (local_t < bd) {
                            float p2 = local_t / bd;
                            anim_dy = sinf(p2 * 3.14159f) * (-60.f) * expf(-p2 * 4.f);
                        }
                        break;
                    }
                    case AnimStyle::Slide:
                        if (local_t < fade_in)
                            anim_dx = (local_t / fade_in - 1.f) * w * 0.6f;
                        else if (local_t > clip_dur - fade_out)
                            anim_dx = ((local_t - (clip_dur - fade_out)) / fade_out) * w * 0.6f;
                        break;
                    case AnimStyle::Stack:
                        if (local_t < fade_in)
                            anim_dy = (1.f - local_t / fade_in) * 80.f;
                        break;
                    default: break;
                }
            }

            if (text_fx.any_text) {
                anim_alpha *= text_fx.opacity_mul;
                fsz        *= text_fx.scale_mul;
                line_h      = fsz * 1.25f;
                block_h     = txt_lines.size() * line_h;
            }

            float block_ax = p.x + show->sub_pos_x * w;   // anchor point X (meaning depends on sub_anchor_h)
            float ty_anim  = slot_y + anim_dy;
            // Collect karaoke words for this clip
            std::vector<const WordEntry*> clip_words;
            bool has_karaoke = (active_ci >= 0 && show->karaoke && !state.words_cache.empty());
            if (has_karaoke) {
                for (auto& we : state.words_cache)
                    if (we.end > show->start && we.start < show->end)
                        clip_words.push_back(&we);
                if (clip_words.empty()) has_karaoke = false;
            }

            float block_max_w = 0.f;
            for (auto& ln : txt_lines)
                block_max_w = fmaxf(block_max_w, txt_font->CalcTextSizeA(fsz, FLT_MAX, -1.f, ln.c_str()).x);

            {
                // When the clip is not active, dim inactive subtitle text
                Clip render_clip = *show;
                if (!show->sub_color_override && active_ci < 0) {
                    render_clip.sub_color_override = true;
                    float a = anim_alpha * 0.5f;
                    render_clip.sub_color[0] = render_clip.sub_color[1] = render_clip.sub_color[2] = 1.f;
                    render_clip.sub_color[3] = a;
                }

                TextRenderCtx trc;
                trc.dl          = dl;
                trc.font        = txt_font;
                trc.fsz         = fsz;
                trc.anim_alpha  = anim_alpha;
                trc.anim_dx     = anim_dx;
                trc.anim_dy     = 0.f;
                trc.clip        = &render_clip;
                trc.eff_style   = (active_ci >= 0) ? eff_style : AnimStyle::None;
                trc.anchor_h    = show->sub_anchor_h;
                trc.block_cx    = block_ax;
                trc.ty          = ty_anim;
                trc.line_h      = line_h;
                trc.t           = state.playhead;
                trc.clip_words  = has_karaoke ? &clip_words : nullptr;
                render_text_block(trc, txt_lines);
            }

            ImGui::PopFont();

            // Store tight TextLayout for accurate handle hit testing (used next frame for
            // click-to-select and this frame for draw_canvas_handles called after this loop).
            {
                float blk_x0, blk_x1;
                if (show->sub_anchor_h == 0)      { blk_x0 = block_ax; blk_x1 = block_ax + block_max_w; }
                else if (show->sub_anchor_h == 2) { blk_x0 = block_ax - block_max_w; blk_x1 = block_ax; }
                else                               { blk_x0 = block_ax - block_max_w*0.5f; blk_x1 = block_ax + block_max_w*0.5f; }
                uint64_t tl_key = ((uint64_t)ti << 32) | (uint32_t)show_ci;
                TextLayout& tl = s_text_layouts[tl_key];
                tl.x0       = blk_x0 - 4.f;
                tl.y0       = ty_anim - 4.f;
                tl.x1       = blk_x1 + 4.f;
                tl.y1       = ty_anim + block_h + 4.f;
                tl.block_ax = block_ax;
                tl.fsz      = fsz;
                tl.valid    = true;
            }
            ++text_rendered;
        }
    }

    dl->PopClipRect();  // end video-frame clip region

    // Safe zone guide — shown when a managed Lyrics track exists.
    // Represents the region guaranteed visible on TikTok/Reels/Shorts.
    {
        bool has_lyrics = false;
        for (auto& t : state.tracks)
            if (t.managed) { has_lyrics = true; break; }
        if (has_lyrics) {
            float sx0 = p.x + SAFE_SIDE * w,  sy0 = p.y + SAFE_TOP * h;
            float sx1 = p.x + (1.f - SAFE_SIDE) * w, sy1 = p.y + (1.f - SAFE_BOT) * h;
            dl->AddRect({sx0, sy0}, {sx1, sy1}, IM_COL32(255, 255, 255, 22), 0.f, 0, 1.f);
            // Corner ticks to make the guide readable without being distracting
            float tk = 8.f;
            ImU32 tc = IM_COL32(255, 255, 255, 55);
            dl->AddLine({sx0, sy0}, {sx0 + tk, sy0}, tc);  dl->AddLine({sx0, sy0}, {sx0, sy0 + tk}, tc);
            dl->AddLine({sx1, sy0}, {sx1 - tk, sy0}, tc);  dl->AddLine({sx1, sy0}, {sx1, sy0 + tk}, tc);
            dl->AddLine({sx0, sy1}, {sx0 + tk, sy1}, tc);  dl->AddLine({sx0, sy1}, {sx0, sy1 - tk}, tc);
            dl->AddLine({sx1, sy1}, {sx1 - tk, sy1}, tc);  dl->AddLine({sx1, sy1}, {sx1, sy1 - tk}, tc);
        }
    }

    // Transform box overlay — drawn above content, below border chrome
    draw_canvas_handles(state, dl, p, w, h);

    // Border and chrome drawn last so they sit on top of all content
    dl->AddRect(p, {p.x+w, p.y+h}, to_u32(Col::line), 2.f);
    {
        const char* fmt_lbl = state.format == OutputFormat::Vertical   ? "9:16" :
                              state.format == OutputFormat::Horizontal  ? "16:9" : "1:1";
        dl->AddText({p.x+6.f, p.y+6.f}, to_u32(Col::dim), fmt_lbl);
    }
    float cm = 10.f; ImU32 cc = to_u32(Col::muted);
    dl->AddLine(p,             {p.x+cm, p.y},       cc);
    dl->AddLine(p,             {p.x, p.y+cm},       cc);
    dl->AddLine({p.x+w, p.y}, {p.x+w-cm, p.y},     cc);
    dl->AddLine({p.x+w, p.y}, {p.x+w, p.y+cm},     cc);
    dl->AddLine({p.x, p.y+h}, {p.x+cm, p.y+h},     cc);
    dl->AddLine({p.x, p.y+h}, {p.x, p.y+h-cm},     cc);
    dl->AddLine({p.x+w,p.y+h},{p.x+w-cm,p.y+h},    cc);
    dl->AddLine({p.x+w,p.y+h},{p.x+w,p.y+h-cm},    cc);

    // Snapshot button — top-right corner, only when a video clip is at the playhead
    {
        bool has_video_at_play = false;
        for (auto& tr : state.tracks)
            for (auto& cl : tr.clips)
                if (cl.clip_type == ClipType::Video &&
                    cl.start <= state.playhead && cl.end > state.playhead &&
                    !cl.source_id.empty())
                    has_video_at_play = true;

        const char* snap_lbl = state.snapshot_running ? "..." : "[o]";
        ImVec2 slsz = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.f, snap_lbl);
        float  btn_pad = 4.f;
        float  btn_w = slsz.x + btn_pad * 2.f;
        float  btn_h = slsz.y + btn_pad * 2.f;
        ImVec2 btn_tl = {p.x + w - btn_w - 6.f, p.y + 6.f};
        ImVec2 btn_br = {btn_tl.x + btn_w, btn_tl.y + btn_h};

        bool snap_hov = mpos.x >= btn_tl.x && mpos.x <= btn_br.x &&
                        mpos.y >= btn_tl.y && mpos.y <= btn_br.y;
        bool snap_ena = has_video_at_play && !state.snapshot_running;

        ImU32 btn_bg  = snap_hov && snap_ena ? IM_COL32(70, 70, 70, 200) : IM_COL32(30, 30, 30, 180);
        ImU32 lbl_col = snap_ena ? to_u32(Col::fg) : to_u32(Col::dim);

        dl->AddRectFilled(btn_tl, btn_br, btn_bg, 3.f);
        dl->AddRect(btn_tl, btn_br, to_u32(Col::line), 3.f);
        dl->AddText({btn_tl.x + btn_pad, btn_tl.y + btn_pad}, lbl_col, snap_lbl);

        if (snap_hov && snap_ena && lclick)
            render_snapshot_gl(state, state.playhead);
    }

    // Stamp time when render thread signals a new snapshot message
    if (state.snapshot_msg_new) {
        state.snapshot_msg_t   = ImGui::GetTime();
        state.snapshot_msg_new = false;
    }

    // Snapshot flash message — bottom-center, fades after 3 s
    if (!state.snapshot_msg.empty()) {
        double elapsed = ImGui::GetTime() - state.snapshot_msg_t;
        float  alpha   = elapsed < 2.0 ? 1.f : (float)(1.0 - (elapsed - 2.0) / 1.0);
        if (alpha <= 0.f) {
            state.snapshot_msg.clear();
        } else {
            alpha = std::min(alpha, 1.f);
            const char* msg = state.snapshot_msg.c_str();
            ImVec2 msz = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.f, msg);
            float  pad = 5.f;
            ImVec2 mtl = {p.x + (w - msz.x) * 0.5f - pad, p.y + h - msz.y - 14.f};
            ImVec2 mbr = {mtl.x + msz.x + pad * 2.f, mtl.y + msz.y + pad};
            dl->AddRectFilled(mtl, mbr, IM_COL32(20, 20, 20, (int)(alpha * 200.f)), 3.f);
            dl->AddText({mtl.x + pad, mtl.y + pad * 0.5f},
                        IM_COL32(220, 220, 220, (int)(alpha * 255.f)), msg);
        }
    }
}
