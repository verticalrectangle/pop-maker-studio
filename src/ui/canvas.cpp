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
#include "video_recorder.h"
#include <turbojpeg.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
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

// True when a real ImGui widget claims the mouse — the transport pill's
// scrubber/buttons and other overlays float INSIDE the preview rect, so the
// canvas's raw hit-testing must stand down or clicks on them fall through to
// layer-select / drag-start underneath. Widgets are submitted after this code
// runs each frame, so same-frame HoveredId is always still 0 here:
// HoveredIdPreviousFrame catches the click frame (hover precedes click),
// ActiveId catches a drag already in progress (it persists across frames).
static bool ui_widget_claims_mouse() {
    ImGuiContext& g = *GImGui;
    return g.HoveredIdPreviousFrame != 0 || g.ActiveId != 0;
}

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
            // Crop changes the displayed aspect — bbox must match the render.
            float va = cl.cropped_aspect(video_info(s).width, video_info(s).height);
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

// ── Crop-edit mode ────────────────────────────────────────────────────────────
// Targets state.crop_edit_track/clip. The scene shows the clip's full frame
// (unrotated); this draws the dimmed surround, the crop window with drag
// handles, and a small pill with aspect presets + Reset / Cancel / Apply.
// Crop values are applied live to the clip; Cancel restores the entry values,
// Apply pushes one history entry. Esc = cancel, Enter = apply.
static struct {
    int    target_track = -1, target_clip = -1;     // entry snapshot owner
    float  entry_l = 0.f, entry_t = 0.f, entry_r = 0.f, entry_b = 0.f;
    int    aspect = 0;   // 0 free, 1 = 1:1, 2 = 9:16, 3 = 16:9
    int    drag   = 0;   // 0 none, 1 TL, 2 TR, 3 BR, 4 BL, 5 T, 6 B, 7 L, 8 R, 9 body
    float  ref_l = 0.f, ref_t = 0.f, ref_r = 0.f, ref_b = 0.f;
    ImVec2 ref_mouse = {};
} s_crop;

static void crop_mode_exit(AppState& state) {
    state.crop_edit_track = state.crop_edit_clip = -1;
    s_crop.target_track   = s_crop.target_clip   = -1;
    s_crop.drag = 0;
}

static void draw_crop_mode(AppState& state, ImDrawList* dl, ImVec2 p, float w, float h) {
    // Validate target — clip deleted / track hidden ends the mode.
    if (state.crop_edit_track < 0 || state.crop_edit_track >= (int)state.tracks.size())
        { crop_mode_exit(state); return; }
    Track& tr = state.tracks[state.crop_edit_track];
    if (state.crop_edit_clip < 0 || state.crop_edit_clip >= (int)tr.clips.size())
        { crop_mode_exit(state); return; }
    Clip& cl = tr.clips[state.crop_edit_clip];
    if (cl.clip_type != ClipType::Video || !tr.visible)
        { crop_mode_exit(state); return; }

    // First frame on this target: snapshot for Cancel.
    if (s_crop.target_track != state.crop_edit_track ||
        s_crop.target_clip  != state.crop_edit_clip) {
        s_crop.target_track = state.crop_edit_track;
        s_crop.target_clip  = state.crop_edit_clip;
        s_crop.entry_l = cl.crop_l; s_crop.entry_t = cl.crop_t;
        s_crop.entry_r = cl.crop_r; s_crop.entry_b = cl.crop_b;
        s_crop.aspect  = 0;
        s_crop.drag    = 0;
    }

    // Source dims (for the px readout and aspect-lock math).
    int src_w = 0, src_h = 0;
    {
        std::string vkey = clip_slot_key(cl.text, cl.start);
        for (int s = 0; s < MAX_VIDEO_TRACKS; ++s)
            if (state.proxy_paths[s] == vkey && video_info(s).width > 0)
                { src_w = video_info(s).width; src_h = video_info(s).height; break; }
    }

    // Full-frame fit box — must mirror the editing_crop branch of the scene
    // draw: full aspect, no rotation, pos/scale applied.
    float px = cl.eval_prop("pos_x",   state.playhead) * w + p.x;
    float py = cl.eval_prop("pos_y",   state.playhead) * h + p.y;
    float sx = cl.eval_prop("scale_x", state.playhead);
    float sy = cl.eval_prop("scale_y", state.playhead);
    float fit_w = w, fit_h = h;
    if (src_w > 0 && src_h > 0) {
        float va = (float)src_w / (float)src_h, ca = w / h;
        if (va > ca) { fit_w = w; fit_h = w / va; }
        else         { fit_h = h; fit_w = h * va; }
    }
    float fw = fit_w * sx, fh = fit_h * sy;
    float fx0 = px - fw * 0.5f, fy0 = py - fh * 0.5f;
    float fx1 = fx0 + fw,       fy1 = fy0 + fh;

    // Crop window in screen space.
    float cx0 = fx0 + cl.crop_l * fw, cy0 = fy0 + cl.crop_t * fh;
    float cx1 = fx1 - cl.crop_r * fw, cy1 = fy1 - cl.crop_b * fh;

    ImVec2 mpos = ImGui::GetIO().MousePos;
    bool ldown  = ImGui::IsMouseDown(0);
    bool lclick = ImGui::IsMouseClicked(0) && !ui_widget_claims_mouse() &&
                  !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId |
                                          ImGuiPopupFlags_AnyPopupLevel);

    // ── Dimmed surround + window border ───────────────────────────────────────
    const ImU32 DIM = IM_COL32(0, 0, 0, 150);
    dl->AddRectFilled({fx0, fy0}, {fx1, cy0}, DIM);                  // top band
    dl->AddRectFilled({fx0, cy1}, {fx1, fy1}, DIM);                  // bottom band
    dl->AddRectFilled({fx0, cy0}, {cx0, cy1}, DIM);                  // left band
    dl->AddRectFilled({cx1, cy0}, {fx1, cy1}, DIM);                  // right band
    dl->AddRect({fx0, fy0}, {fx1, fy1}, IM_COL32(255,255,255,50));   // full frame
    dl->AddRect({cx0, cy0}, {cx1, cy1}, IM_COL32(255,255,255,230), 0.f, 0, 1.5f);
    // Thirds grid
    for (int i = 1; i <= 2; ++i) {
        float gx = cx0 + (cx1 - cx0) * (i / 3.f);
        float gy = cy0 + (cy1 - cy0) * (i / 3.f);
        dl->AddLine({gx, cy0}, {gx, cy1}, IM_COL32(255,255,255,40));
        dl->AddLine({cx0, gy}, {cx1, gy}, IM_COL32(255,255,255,40));
    }
    // Pixel readout under the window
    if (src_w > 0) {
        char dim_lbl[48];
        snprintf(dim_lbl, sizeof(dim_lbl), "%d x %d",
                 (int)roundf(src_w * (1.f - cl.crop_l - cl.crop_r)),
                 (int)roundf(src_h * (1.f - cl.crop_t - cl.crop_b)));
        dl->AddText({cx0 + 4.f, cy1 + 4.f}, IM_COL32(220,220,220,200), dim_lbl);
    }

    // ── Handles ───────────────────────────────────────────────────────────────
    const float CR = 4.5f, HIT = 8.f;
    float cmx = (cx0 + cx1) * 0.5f, cmy = (cy0 + cy1) * 0.5f;
    struct H { float x, y; int id; };
    H corners[4] = {{cx0,cy0,1},{cx1,cy0,2},{cx1,cy1,3},{cx0,cy1,4}};
    H edges[4]   = {{cmx,cy0,5},{cmx,cy1,6},{cx0,cmy,7},{cx1,cmy,8}};
    bool aspect_locked = s_crop.aspect != 0;

    auto draw_handle = [&](float hx, float hy, int id) {
        bool hov = fabsf(mpos.x - hx) <= HIT && fabsf(mpos.y - hy) <= HIT;
        ImU32 c = (hov || s_crop.drag == id) ? IM_COL32(100,180,255,255)
                                             : IM_COL32(255,255,255,230);
        dl->AddRectFilled({hx-CR, hy-CR}, {hx+CR, hy+CR}, c, 2.f);
        dl->AddRect      ({hx-CR, hy-CR}, {hx+CR, hy+CR}, IM_COL32(0,0,0,180), 2.f, 0, 0.8f);
        if (hov && lclick && s_crop.drag == 0) {
            s_crop.drag = id;
            s_crop.ref_l = cl.crop_l; s_crop.ref_t = cl.crop_t;
            s_crop.ref_r = cl.crop_r; s_crop.ref_b = cl.crop_b;
            s_crop.ref_mouse = mpos;
        }
    };
    for (auto& hc : corners) draw_handle(hc.x, hc.y, hc.id);
    if (!aspect_locked)                       // edges break a locked ratio
        for (auto& he : edges) draw_handle(he.x, he.y, he.id);

    // Body drag (move the window)
    bool in_window = mpos.x > cx0+CR*2 && mpos.x < cx1-CR*2 &&
                     mpos.y > cy0+CR*2 && mpos.y < cy1-CR*2;
    if (in_window && s_crop.drag == 0)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    if (in_window && lclick && s_crop.drag == 0) {
        s_crop.drag = 9;
        s_crop.ref_l = cl.crop_l; s_crop.ref_t = cl.crop_t;
        s_crop.ref_r = cl.crop_r; s_crop.ref_b = cl.crop_b;
        s_crop.ref_mouse = mpos;
    }

    // ── Drag update ───────────────────────────────────────────────────────────
    const float MIN_WIN = 0.05f;  // smallest visible window per axis
    if (s_crop.drag != 0 && ldown && fw > 0.f && fh > 0.f) {
        float dxf = (mpos.x - s_crop.ref_mouse.x) / fw;
        float dyf = (mpos.y - s_crop.ref_mouse.y) / fh;
        float l = s_crop.ref_l, t = s_crop.ref_t, r = s_crop.ref_r, b = s_crop.ref_b;
        auto clampf = [](float v, float lo, float hi) {
            return v < lo ? lo : (v > hi ? hi : v);
        };
        if (s_crop.drag == 9) {                       // move window
            float shift_x = clampf(dxf, -l, r);
            float shift_y = clampf(dyf, -t, b);
            cl.crop_l = l + shift_x; cl.crop_r = r - shift_x;
            cl.crop_t = t + shift_y; cl.crop_b = b - shift_y;
        } else if (aspect_locked && s_crop.drag <= 4 && src_w > 0) {
            // Corner drag with ratio lock: opposite corner anchored; the new
            // width (from x motion) drives the height via the fraction ratio
            // k = cw/ch that yields the target pixel aspect.
            float A = (s_crop.aspect == 1) ? 1.f : (s_crop.aspect == 2) ? 9.f/16.f : 16.f/9.f;
            float k = A * (float)src_h / (float)src_w;
            bool left_c = (s_crop.drag == 1 || s_crop.drag == 4);
            bool top_c  = (s_crop.drag == 1 || s_crop.drag == 2);
            float cw = left_c ? (1.f - r) - (l + dxf) : (1.f - l) - (r - dxf);
            cw = clampf(cw, MIN_WIN, left_c ? 1.f - r : 1.f - l);
            // keep the derived height inside its own bounds
            float ch_max = top_c ? 1.f - b : 1.f - t;
            cw = fminf(cw, ch_max * k);
            cw = fmaxf(cw, MIN_WIN);
            float ch = cw / k;
            if (left_c) cl.crop_l = (1.f - r) - cw; else cl.crop_r = (1.f - l) - cw;
            if (top_c)  cl.crop_t = (1.f - b) - ch; else cl.crop_b = (1.f - t) - ch;
        } else {                                       // free corner / edge
            bool eL = s_crop.drag == 1 || s_crop.drag == 4 || s_crop.drag == 7;
            bool eR = s_crop.drag == 2 || s_crop.drag == 3 || s_crop.drag == 8;
            bool eT = s_crop.drag == 1 || s_crop.drag == 2 || s_crop.drag == 5;
            bool eB = s_crop.drag == 3 || s_crop.drag == 4 || s_crop.drag == 6;
            if (eL) cl.crop_l = clampf(l + dxf, 0.f, 1.f - r - MIN_WIN);
            if (eR) cl.crop_r = clampf(r - dxf, 0.f, 1.f - l - MIN_WIN);
            if (eT) cl.crop_t = clampf(t + dyf, 0.f, 1.f - b - MIN_WIN);
            if (eB) cl.crop_b = clampf(b - dyf, 0.f, 1.f - t - MIN_WIN);
        }
    }
    if (!ldown) s_crop.drag = 0;

    // ── Pill: aspect presets + Reset / Cancel / Apply ─────────────────────────
    auto apply_preset = [&](int preset) {
        s_crop.aspect = preset;
        if (preset == 0 || src_w <= 0) return;
        float A  = (preset == 1) ? 1.f : (preset == 2) ? 9.f/16.f : 16.f/9.f;
        float k  = A * (float)src_h / (float)src_w;   // cw/ch fraction ratio
        float cw = fminf(1.f, k), ch = cw / k;
        cl.crop_l = cl.crop_r = (1.f - cw) * 0.5f;
        cl.crop_t = cl.crop_b = (1.f - ch) * 0.5f;
    };

    struct PB { const char* lbl; int preset; };  // preset >= 0; -1 Reset, -2 Cancel, -3 Apply
    PB btns[] = {{"Free",0},{"1:1",1},{"9:16",2},{"16:9",3},
                 {"Reset",-1},{"Cancel",-2},{"Apply",-3}};
    float bh = 24.f, gap = 6.f, pad = 10.f, total_w = 0.f;
    for (auto& pb : btns) total_w += ImGui::CalcTextSize(pb.lbl).x + 16.f + gap;
    total_w += pad * 2.f - gap;
    float bx = p.x + (w - total_w) * 0.5f, by = p.y + 12.f;
    dl->AddRectFilled({bx, by}, {bx + total_w, by + bh + pad},
                      IM_COL32(18,18,22,215), 14.f);
    dl->AddRect({bx, by}, {bx + total_w, by + bh + pad},
                IM_COL32(255,255,255,25), 14.f);
    float cur_x = bx + pad;
    for (auto& pb : btns) {
        float bw2 = ImGui::CalcTextSize(pb.lbl).x + 16.f;
        ImGui::SetCursorScreenPos({cur_x, by + pad * 0.5f});
        ImGui::InvisibleButton(pb.lbl, {bw2, bh});
        bool hov = ImGui::IsItemHovered();
        bool sel = (pb.preset >= 0 && s_crop.aspect == pb.preset);
        ImU32 bg = sel ? IM_COL32(130,100,255,220)
                 : hov ? IM_COL32(62,62,82,230)
                 : pb.preset == -3 ? IM_COL32(46,46,66,230) : IM_COL32(34,34,44,200);
        dl->AddRectFilled({cur_x, by + pad*0.5f}, {cur_x + bw2, by + pad*0.5f + bh}, bg, 12.f);
        dl->AddText({cur_x + 8.f, by + pad*0.5f + (bh - ImGui::GetFontSize()) * 0.5f},
                    IM_COL32(232,232,238,255), pb.lbl);
        if (ImGui::IsItemClicked()) {
            if      (pb.preset >= 0)  apply_preset(pb.preset);
            else if (pb.preset == -1) { cl.crop_l=cl.crop_t=cl.crop_r=cl.crop_b=0.f; s_crop.aspect=0; }
            else if (pb.preset == -2) {
                cl.crop_l = s_crop.entry_l; cl.crop_t = s_crop.entry_t;
                cl.crop_r = s_crop.entry_r; cl.crop_b = s_crop.entry_b;
                crop_mode_exit(state);
                return;
            } else {                   // Apply
                history_push(state, "Crop clip");
                crop_mode_exit(state);
                return;
            }
        }
        cur_x += bw2 + gap;
    }

    // Keyboard: Esc = cancel, Enter = apply.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        cl.crop_l = s_crop.entry_l; cl.crop_t = s_crop.entry_t;
        cl.crop_r = s_crop.entry_r; cl.crop_b = s_crop.entry_b;
        crop_mode_exit(state);
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        history_push(state, "Crop clip");
        crop_mode_exit(state);
        return;
    }
}

void draw_canvas_handles(AppState& state, ImDrawList* dl, ImVec2 p, float w, float h) {
    // Crop-edit mode replaces the normal transform handles entirely.
    if (state.crop_edit_track >= 0) { draw_crop_mode(state, dl, p, w, h); return; }
    if (state.selected_track < 0 || state.selected_clip < 0) return;
    if (state.selected_track >= (int)state.tracks.size()) return;
    Track& tr = state.tracks[state.selected_track];
    if (state.selected_clip >= (int)tr.clips.size()) return;
    Clip& cl = tr.clips[state.selected_clip];

    ImVec2 mpos   = ImGui::GetIO().MousePos;
    bool   ldown  = ImGui::IsMouseDown(0);
    // No drag-start while a popup is open (popups don't block IsMouseClicked),
    // while a real widget claims the mouse (transport pill over the canvas),
    // or during Alt+click — Alt cycles the layer selection in draw_preview,
    // and immediately grabbing the newly selected layer would move it.
    bool   lclick = ImGui::IsMouseClicked(0) &&
                    !ImGui::GetIO().KeyAlt &&
                    !ui_widget_claims_mouse() &&
                    !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId |
                                            ImGuiPopupFlags_AnyPopupLevel);

    // Interaction region is the whole preview zone (the letterbox surround
    // included), not just the canvas — clips dragged off-canvas must stay
    // grabbable. Drawing past the zone is cut by the child window clip rect.
    ImVec2 zp = ImGui::GetWindowPos();
    ImVec2 zs = ImGui::GetWindowSize();
    bool in_preview  = mpos.x >= zp.x && mpos.x <= zp.x+zs.x &&
                       mpos.y >= zp.y && mpos.y <= zp.y+zs.y;
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
    if (clip_is_videolike_type(cl.clip_type)) {
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
            s_ctx.clip_idx == state.selected_clip &&
            clip_is_videolike_type(cl.clip_type)) {
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

// Canvas-source snapshot: capture the live preview rect from the window
// framebuffer after ImGui renders this frame. frames_left counts down a few
// frames after the request so an IPC seek's async proxy decode has time to
// land before we grab the pixels; the rect is refreshed every frame so it
// tracks the live layout.
static struct {
    int    frames_left = -1;   // -1 idle, >0 warming, 0 capture after render
    bool   full_ui     = false; // capture the whole window, not the canvas rect
    ImVec2 p           = {};
    float  w = 0.f, h = 0.f;
} g_canvas_cap;

// ── Live camera mirror ────────────────────────────────────────────────────────
// While the camera brick monitors, warms up, or records, the latest camera
// frame is drawn over the canvas (fit, slightly inset) so the performer can
// frame themselves. Decode happens only when a new frame arrived.
static void draw_camera_mirror(ImDrawList* dl, ImVec2 p, float w, float h) {
    if (!vrecorder_monitor_get() && !vrecorder_active()) return;

    static GLuint   s_cam_tex    = 0;
    static int      s_cam_w      = 0, s_cam_h = 0;
    static uint64_t s_cam_serial = 0;
    static tjhandle s_cam_tj     = nullptr;
    static std::vector<uint8_t> s_rgba;

    uint64_t serial = vrecorder_frame_serial();
    if (serial != s_cam_serial) {
        std::vector<uint8_t> jpeg;
        if (vrecorder_latest_jpeg(jpeg)) {
            if (!s_cam_tj) s_cam_tj = tjInitDecompress();
            int jw = 0, jh = 0, sub = 0, cs = 0;
            if (s_cam_tj && tjDecompressHeader3(s_cam_tj, jpeg.data(),
                    (unsigned long)jpeg.size(), &jw, &jh, &sub, &cs) == 0) {
                s_rgba.resize((size_t)jw * jh * 4);
                if (tjDecompress2(s_cam_tj, jpeg.data(),
                        (unsigned long)jpeg.size(), s_rgba.data(),
                        jw, 0, jh, TJPF_RGBA, TJFLAG_FASTDCT) == 0) {
                    if (!s_cam_tex) glGenTextures(1, &s_cam_tex);
                    glBindTexture(GL_TEXTURE_2D, s_cam_tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, jw, jh, 0,
                                 GL_RGBA, GL_UNSIGNED_BYTE, s_rgba.data());
                    glBindTexture(GL_TEXTURE_2D, 0);
                    s_cam_w = jw; s_cam_h = jh;
                }
            }
        }
        s_cam_serial = serial;
    }
    if (!s_cam_tex || s_cam_w <= 0) return;

    // Fit inside the canvas with a small inset; mirror horizontally so it
    // behaves like a mirror (recorded takes keep the true orientation).
    // Rotation comes from the camera brick (selected, or the record target)
    // so the mirror matches how the take will sit on the canvas — phone
    // cameras over v4l2loopback often arrive sideways.
    float rot_deg = 0.f;
    {
        extern AppState* g_state_for_mirror;   // set by draw_preview below
        if (g_state_for_mirror) {
            AppState& st = *g_state_for_mirror;
            const Clip* br = nullptr;
            if (st.selected_track >= 0 && st.selected_track < (int)st.tracks.size() &&
                st.selected_clip >= 0 &&
                st.selected_clip < (int)st.tracks[st.selected_track].clips.size()) {
                const Clip& c2 = st.tracks[st.selected_track].clips[st.selected_clip];
                if (c2.clip_type == ClipType::VideoRecord) br = &c2;
            }
            if (!br) {
                for (auto& tr : st.tracks)
                    for (auto& c2 : tr.clips)
                        if (c2.clip_type == ClipType::VideoRecord) { br = &c2; break; }
            }
            if (br) rot_deg = br->rotation;
        }
    }
    // The mirror flips horizontally, which inverts apparent rotation:
    // mirrored content rotated -θ looks like true content rotated +θ. Negate
    // so the SAME brick rotation makes mirror and recorded take match.
    float rad = -rot_deg * 3.14159265f / 180.f;
    float cr = cosf(rad), sr = sinf(rad);

    // Fit the ROTATED frame inside the canvas with a small inset.
    float inset = 12.f;
    float aw = w - inset * 2.f, ah = h - inset * 2.f;
    float bw = fabsf((float)s_cam_w * cr) + fabsf((float)s_cam_h * sr);
    float bh = fabsf((float)s_cam_w * sr) + fabsf((float)s_cam_h * cr);
    float sc = fminf(aw / bw, ah / bh);
    float hw = s_cam_w * sc * 0.5f, hh = s_cam_h * sc * 0.5f;
    ImVec2 c  = {p.x + w * 0.5f, p.y + h * 0.5f};
    auto rotp = [&](float x, float y) {
        return ImVec2{c.x + x * cr - y * sr, c.y + x * sr + y * cr};
    };
    ImVec2 p0 = rotp(-hw, -hh), p1 = rotp(hw, -hh),
           p2 = rotp(hw, hh),   p3 = rotp(-hw, hh);
    dl->AddRectFilled({p.x, p.y}, {p.x + w, p.y + h}, IM_COL32(0, 0, 0, 160));
    // u flipped → mirror behaviour (recorded takes keep true orientation)
    dl->AddImageQuad((ImTextureID)(intptr_t)s_cam_tex, p0, p1, p2, p3,
                     {1, 0}, {0, 0}, {0, 1}, {1, 1});
    ImU32 frame_col = vrecorder_recording() ? IM_COL32(235, 90, 40, 255)
                                            : IM_COL32(160, 160, 180, 160);
    dl->AddQuad(p0, p1, p2, p3, frame_col, vrecorder_recording() ? 3.f : 1.5f);
    const char* tag2 = vrecorder_recording() ? "\xe2\x97\x8f REC"
                     : vrecorder_warming()   ? "starting\xe2\x80\xa6"
                                             : "camera";
    dl->AddText({fminf(fminf(p0.x,p1.x),fminf(p2.x,p3.x)) + 10.f,
                 fminf(fminf(p0.y,p1.y),fminf(p2.y,p3.y)) + 8.f}, frame_col, tag2);
}

AppState* g_state_for_mirror = nullptr;

void draw_preview(AppState& state, ImVec2 p, float w, float h) {
    // IPC-triggered snapshot — fulfilled here on the GL thread
    if (state.snapshot_request) {
        state.snapshot_request = false;
        if (state.snapshot_source_canvas) {
            g_canvas_cap.frames_left = 3;
            g_canvas_cap.full_ui     = state.snapshot_source_ui;
        } else {
            render_snapshot_gl(state, state.playhead);
        }
    }
    if (g_canvas_cap.frames_left > 0) {
        g_canvas_cap.p = p; g_canvas_cap.w = w; g_canvas_cap.h = h;
        --g_canvas_cap.frames_left;  // 0 → canvas_capture_after_render fires
    }

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


    // Empty state / agent loading panel
    if (state.tracks.empty()) {
        // Auto-timeout: clear stale agent flag if no end_batch arrived within 8 s
        static double s_agent_start = 0.0;
        if (state.agent_active) {
            if (s_agent_start == 0.0) s_agent_start = ImGui::GetTime();
            if (ImGui::GetTime() - s_agent_start > 8.0) {
                state.agent_active = false;
                s_agent_start = 0.0;
            }
        } else {
            s_agent_start = 0.0;
        }

        if (state.agent_active) {
            double t = ImGui::GetTime();

            static const char* k_msgs[] = {
                "finding the vibe...",
                "building your world...",
                "assembling the timeline...",
                "the beats are loading...",
                "it's giving...",
                "setting the scene...",
                "cooking something up...",
            };
            constexpr int k_msg_count = (int)(sizeof(k_msgs) / sizeof(k_msgs[0]));
            const char* status_msg = k_msgs[(int)(t / 2.5) % k_msg_count];

            float card_w = std::min(w * 0.65f, 220.f);
            float card_h = 110.f;
            float cx = p.x + w * 0.5f;
            float cy = p.y + h * 0.5f;
            ImVec2 tl2 = {cx - card_w * 0.5f, cy - card_h * 0.5f};
            ImVec2 br2 = {cx + card_w * 0.5f, cy + card_h * 0.5f};

            dl->AddRectFilled(tl2, br2, IM_COL32(22, 22, 28, 220), 10.f);
            dl->AddRect(tl2, br2, IM_COL32(80, 60, 120, 160), 10.f);

            // Spinning plumbob (diamond)
            float spin = (float)(t * 1.8);
            float cs = cosf(spin), sn = sinf(spin);
            float pr = 13.f;
            ImVec2 pc = {cx, tl2.y + 26.f};
            auto rot = [&](float dx, float dy) -> ImVec2 {
                return {pc.x + dx * cs - dy * sn, pc.y + dx * sn + dy * cs};
            };
            ImVec2 ptop   = rot(0,    -pr);
            ImVec2 pright = rot(pr * 0.7f, 0);
            ImVec2 pbot   = rot(0,     pr);
            ImVec2 pleft  = rot(-pr * 0.7f, 0);
            dl->AddTriangleFilled(ptop, pright, pbot, IM_COL32(160, 80, 255, 220));
            dl->AddTriangleFilled(ptop, pleft,  pbot, IM_COL32(120, 50, 200, 220));
            dl->AddTriangle(ptop, pright, pbot, IM_COL32(200, 140, 255, 180));
            dl->AddTriangle(ptop, pleft,  pbot, IM_COL32(200, 140, 255, 180));

            float tsz = ImGui::GetFontSize();
            ImVec2 smid_sz = ImGui::GetFont()->CalcTextSizeA(tsz, FLT_MAX, -1.f, status_msg);
            dl->AddText({cx - smid_sz.x * 0.5f, pc.y + pr + 8.f},
                IM_COL32(200, 180, 230, 230), status_msg);

            if (!state.agent_msg.empty()) {
                float lsz = tsz * 0.78f;
                ImVec2 lsz_v = ImGui::GetFont()->CalcTextSizeA(lsz, FLT_MAX, -1.f, state.agent_msg.c_str());
                dl->AddText(ImGui::GetFont(), lsz,
                    {cx - lsz_v.x * 0.5f, pc.y + pr + 8.f + smid_sz.y + 3.f},
                    IM_COL32(120, 100, 150, 180), state.agent_msg.c_str());
            }

            // Fugazi progress bar — breathes between 20 % and 80 %
            float bar_fill = 0.5f + 0.3f * sinf((float)t * 0.7f);
            float bar_y    = br2.y - 22.f;
            float bar_x0   = tl2.x + 16.f;
            float bar_x1   = br2.x - 16.f;
            float bar_h_px = 5.f;
            dl->AddRectFilled({bar_x0, bar_y}, {bar_x1, bar_y + bar_h_px},
                IM_COL32(50, 40, 70, 200), 3.f);
            dl->AddRectFilled({bar_x0, bar_y},
                {bar_x0 + (bar_x1 - bar_x0) * bar_fill, bar_y + bar_h_px},
                IM_COL32(160, 80, 255, 220), 3.f);
        } else {
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
        }

        dl->AddRect(p, {p.x+w, p.y+h}, to_u32(Col::line), 2.f);
        return;
    }

    // Unified z-order pass: track index 0 = frontmost, so iterate high→low (background first).
    // Each track draws whichever clip type is active — video and text are interleaved correctly.
    ImVec2 mpos  = ImGui::GetIO().MousePos;
    // Popups don't intercept IsMouseClicked — without this guard a click on a
    // context-menu item overlapping the preview also ran layer selection. The
    // widget guard does the same for the transport pill floating over the
    // canvas: scrubbing or hitting play must not select/deselect layers.
    bool   lclick = ImGui::IsMouseClicked(0) &&
                    !ui_widget_claims_mouse() &&
                    !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId |
                                            ImGuiPopupFlags_AnyPopupLevel);

    // Click-to-select using tight bboxes: video computed inline, text from
    // previous-frame layouts. The pickable region is the whole preview zone —
    // clips dragged off-canvas park in the letterbox surround and must stay
    // clickable there.
    ImVec2 zone_p = ImGui::GetWindowPos();
    ImVec2 zone_s = ImGui::GetWindowSize();
    bool in_preview_area = mpos.x >= zone_p.x && mpos.x <= zone_p.x+zone_s.x &&
                           mpos.y >= zone_p.y && mpos.y <= zone_p.y+zone_s.y;

    // Layer picking stands down during crop-edit mode — clicks there belong
    // to the crop window/handles (draw_crop_mode).
    if (lclick && in_preview_area && s_ctx.handle == CanvasHandle::None &&
        state.crop_edit_track < 0) {
        struct HitCandidate { int ti, ci; float area; };
        std::vector<HitCandidate> hits;
        for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
            auto& tr2 = state.tracks[ti];
            if (!tr2.visible) continue;
            for (int ci = 0; ci < (int)tr2.clips.size(); ++ci) {
                auto& cl2 = tr2.clips[ci];
                if (state.playhead < cl2.start || state.playhead >= cl2.end) continue;
                if (clip_is_videolike_type(cl2.clip_type)) {
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
        // Front-to-back: track 0 draws last, i.e. frontmost, so the lowest ti
        // hit is the layer the user actually sees under the cursor. The old
        // smallest-bbox-wins pick kept selecting clips that were completely
        // hidden behind larger ones.
        std::sort(hits.begin(), hits.end(), [](const HitCandidate& a, const HitCandidate& b) {
            if (a.ti != b.ti) return a.ti < b.ti;
            return a.ci < b.ci;
        });
        if (!hits.empty()) {
            int pick = 0;
            // Alt+click digs through the stack: from the currently selected
            // layer, advance to the next hit beneath it (wrapping to the top),
            // so covered layers stay reachable from the canvas.
            if (ImGui::GetIO().KeyAlt) {
                for (int i = 0; i < (int)hits.size(); ++i)
                    if (hits[i].ti == state.selected_track &&
                        hits[i].ci == state.selected_clip) {
                        pick = (i + 1) % (int)hits.size();
                        break;
                    }
            }
            if (state.selected_track != hits[pick].ti || state.selected_clip != hits[pick].ci) {
                state.selected_track = hits[pick].ti;
                state.selected_clip  = hits[pick].ci;
                state.request_scroll_to_clip = true;
            }
        } else {
            state.selected_track = -1;
            state.selected_clip  = -1;
        }
    }

    float lookahead = state.playing ? ImGui::GetIO().DeltaTime : 0.f;
    float t_anim    = state.playing ? (float)ImGui::GetTime() : state.playhead;

    // Collect global creative FX (for full-frame effects — LightLeak, grade, etc.)
    CreativeFXAccum global_cfx = collect_creative_fx(state, state.playhead, (int)state.tracks.size());

    // ── Pass 1: BG clips (ImGui draw list) + Video clips (→ scene FBO) ────────
    scene_begin((int)w, (int)h);

    // Pre-walk: identify every video clip that will be decoded this frame
    // (active clip per track + any transition partner) and dispatch a parallel
    // JPEG decode batch. The draw loop below then hits a cached upload path.
    // Mirrors the active-clip selection logic farther down — keep in sync.
    {
        auto make_pfx = [&](const Clip* cl_ptr, int ti) {
            PixelFX pfx;
            CreativeFXAccum cfx2 = collect_glass_fx(state, state.playhead, ti);
            pfx.bg_remove_on       = cl_ptr->bg_remove_on &&
                                     cl_ptr->bg_remove_status == BgRemoveStatus::Ready;
            pfx.bg_remove_mask_dir = cl_ptr->bg_remove_mask_dir;
            pfx.bg_remove_softness = cl_ptr->bg_remove_softness;
            pfx.bg_remove_box_on   = cl_ptr->bg_remove_box_on;
            pfx.bg_remove_box_l    = cl_ptr->bg_remove_box_l;
            pfx.bg_remove_box_r    = cl_ptr->bg_remove_box_r;
            pfx.bg_remove_box_t    = cl_ptr->bg_remove_box_t;
            pfx.bg_remove_box_b    = cl_ptr->bg_remove_box_b;
            pfx.datamosh_on        = cfx2.datamosh_on;
            pfx.datamosh_intensity = cfx2.datamosh_intensity;
            pfx.datamosh_spread    = cfx2.datamosh_spread;
            pfx.time               = t_anim;
            return pfx;
        };
        std::vector<VideoPrefetchReq> reqs;
        reqs.reserve(MAX_VIDEO_TRACKS * 2);
        // Dedup by slot: each slot only needs one prefetch window per frame. The
        // ring caches RING_FRAMES forward, so a second req for the same slot
        // would just race for the same ring entries.
        auto already_queued = [&](int slot) {
            for (auto& r : reqs) if (r.track_id == slot) return true;
            return false;
        };
        auto add_clip = [&](const Clip* cl, float at_time, int ti, int max_frames = 0) {
            if (!cl || !clip_is_videolike_type(cl->clip_type)) return;
            int slot = slot_for_video(const_cast<AppState&>(state),
                                      clip_slot_key(cl->text, cl->start), cl->text);
            if (slot < 0 || !video_is_open(slot)) return;
            if (already_queued(slot)) return;
            video_set_pixel_fx(slot, make_pfx(cl, ti));
            float src_t = cl->in_point + (at_time - cl->start) * cl->speed;
            reqs.push_back({slot, (double)(src_t + lookahead), max_frames});
        };

        // Boundary warm distance: prefetch the next/previous clip's slot when
        // the playhead is within this many seconds of a clip boundary, so a
        // scrub across the cut hits a warm ring instead of a sync JPEG decode.
        constexpr float BOUNDARY_WARM_S = 1.0f;

        for (int ti = (int)state.tracks.size() - 1; ti >= 0; --ti) {
            auto& track = state.tracks[ti];
            if (!track.visible) continue;

            const Clip* active = nullptr; int active_ci = -1;
            for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
                auto& cl = track.clips[ci];
                if (clip_is_videolike_type(cl.clip_type) &&
                    state.playhead >= cl.start && state.playhead < cl.end)
                    { active = &cl; active_ci = ci; break; }
            }
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
            if (!active) continue;
            add_clip(active, state.playhead, ti);

            bool in_trans_out = (active->transition_type != TransitionType::None &&
                                 active->transition_pre > 0.f &&
                                 state.playhead >= active->end - active->transition_pre);
            if (in_trans_out && active_ci + 1 < (int)track.clips.size()) {
                const Clip& nc = track.clips[active_ci + 1];
                if (nc.clip_type == ClipType::Video) add_clip(&nc, state.playhead, ti);
            } else if (active_ci > 0) {
                const Clip& pc = track.clips[active_ci - 1];
                if (pc.clip_type == ClipType::Video &&
                    pc.transition_type != TransitionType::None &&
                    pc.transition_post > 0.f &&
                    state.playhead < active->start + pc.transition_post) {
                    float at = fminf(state.playhead, pc.end - 1e-4f);
                    add_clip(&pc, at, ti);
                }
            }

            // Boundary warm: forward into the upcoming clip's first frame,
            // backward into the previous clip's last frame. add_clip's slot
            // dedupe makes this a no-op when active/neighbor share a source.
            // Cap warm window so we don't drag the active clip's prefetch.
            constexpr int BOUNDARY_WARM_FRAMES = 3;
            float t_to_end   = active->end       - state.playhead;
            float t_to_start = state.playhead    - active->start;
            if (t_to_end < BOUNDARY_WARM_S && active_ci + 1 < (int)track.clips.size()) {
                const Clip& nc = track.clips[active_ci + 1];
                if (nc.clip_type == ClipType::Video)
                    add_clip(&nc, nc.start, ti, BOUNDARY_WARM_FRAMES);
            }
            if (t_to_start < BOUNDARY_WARM_S && active_ci > 0) {
                const Clip& pc = track.clips[active_ci - 1];
                if (pc.clip_type == ClipType::Video) {
                    float prev_at = fmaxf(pc.start, pc.end - 1e-3f);
                    add_clip(&pc, prev_at, ti, BOUNDARY_WARM_FRAMES);
                }
            }
        }
        video_prefetch_frames(reqs.data(), (int)reqs.size());
    }

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

                // Glass BodyFX: standalone BodyFX bricks and MultiFX BodyFX sub-effects on this track.
                // Both use the video clip's own bg_remove masks.
                if (cl_ptr && slot >= 0 &&
                    cl_ptr->bg_remove_status == BgRemoveStatus::Ready &&
                    !cl_ptr->bg_remove_mask_dir.empty()) {
                    std::string mask_dir = cl_ptr->bg_remove_mask_dir;
                    float mask_fps = bg_remove_read_fps(mask_dir);
                    float src_t = cl_ptr->in_point + (at_time - cl_ptr->start) / cl_ptr->speed;
                    int frame_i = (int)(src_t * mask_fps);
                    VideoInfo vi_g = video_info(slot);
                    int bw = (vi_g.width  > 0) ? vi_g.width  : (int)w;
                    int bh = (vi_g.height > 0) ? vi_g.height : (int)h;

                    // Standalone glass BodyFX bricks on this track
                    for (auto& bfx_cl : state.tracks[ti].clips) {
                        if (bfx_cl.clip_type != ClipType::BodyFX) continue;
                        if (at_time < bfx_cl.start || at_time >= bfx_cl.end) continue;
                        unsigned mask_tex = body_fx_mask_texture(mask_dir, frame_i);
                        if (!mask_tex) continue;
                        tex = body_fx_apply(bfx_cl.body_fx_type, tex, mask_tex, bw, bh,
                                            bfx_cl.body_fx_params, bfx_cl.body_fx_amount, t_anim);
                    }

                    // BodyFX sub-effects inside glass MultiFX bricks on this track
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
                            tex = body_fx_apply(se.body_fx_type, tex, mask_tex, bw, bh,
                                                se.body_fx_params, se.body_fx_amount, t_anim);
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
                // While THIS clip is in crop-edit mode the full frame is shown
                // (full aspect, full UVs) so the user can drag the crop window
                // over it; otherwise the cropped sub-rect IS the clip.
                bool editing_crop = (state.crop_edit_track == ti &&
                                     state.crop_edit_clip  >= 0 &&
                                     state.crop_edit_clip  < (int)track.clips.size() &&
                                     &track.clips[state.crop_edit_clip] == cl_ptr);
                float fit_w = w, fit_h = h;
                if (vi.width > 0 && vi.height > 0) {
                    // Fit box follows the CROPPED region's aspect — the crop
                    // sub-rect fills the same role the full frame used to.
                    float vid_asp = editing_crop
                        ? (float)vi.width / (float)vi.height
                        : cl_ptr->cropped_aspect(vi.width, vi.height);
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

                alpha = fmaxf(0.f, fminf(1.f, alpha));
                if (editing_crop)
                    // Crop-edit shows the full frame, unrotated — the crop is
                    // defined in source space, so the editing view is source
                    // view (the overlay rect in draw_crop_mode matches this).
                    scene_add_layer(tex, cx, cy, hw, hh, 1.f, 0.f, alpha);
                else if (cl_ptr->has_crop())
                    scene_add_layer(tex, cx, cy, hw, hh, cos_r, sin_r, alpha,
                                    cl_ptr->crop_l, cl_ptr->crop_t,
                                    1.f - cl_ptr->crop_r, 1.f - cl_ptr->crop_b);
                else
                    scene_add_layer(tex, cx, cy, hw, hh, cos_r, sin_r, alpha);
            };

            // Find the active video clip and check for transitions
            const Clip* active = nullptr;
            int active_ci = -1;
            for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
                auto& cl = track.clips[ci];
                if (clip_is_videolike_type(cl.clip_type) && !cl.text.empty() &&
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
                    float t_a = fmaxf(0.f, fminf(1.f, (state.playhead - (cut - pre)) / fmaxf(pre, 1e-5f)));
                    float t_b = fmaxf(0.f, fminf(1.f, (state.playhead - cut) / fmaxf(post, 1e-5f)));

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
                    float t = fmaxf(0.f, fminf(1.f,
                        (state.playhead - active->start) / fmaxf(prev_cl->transition_post, 1e-5f)));
                    if (prev_cl->transition_type == TransitionType::Dissolve) {
                        draw_vid_clip(prev_cl, fminf(state.playhead, prev_cl->end - 1e-4f), 1.f - t);
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

    if (uintptr_t scene_tex = scene_result()) {
        // ── Draw scene FBO to ImGui draw list ─────────────────────────────────
        // Y-flip UVs: GL FBO t=0 is at the bottom, ImGui tl=(0,0) is at the top.
        dl->AddImageQuad(ImTextureRef((ImTextureID)scene_tex),
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

    // Ghost outlines for clips parked fully outside the canvas: their pixels
    // can't render (the scene FBO is the canvas), but a dim dashed box in the
    // letterbox surround keeps them visible and gives picking a target.
    for (int gti = 0; gti < (int)state.tracks.size(); ++gti) {
        auto& gtr = state.tracks[gti];
        if (!gtr.visible) continue;
        for (int gci = 0; gci < (int)gtr.clips.size(); ++gci) {
            auto& gcl = gtr.clips[gci];
            if (state.playhead < gcl.start || state.playhead >= gcl.end) continue;
            if (gcl.clip_type != ClipType::Video &&
                gcl.clip_type != ClipType::Background) continue;
            float gx0, gy0, gx1, gy1;
            if (gcl.clip_type == ClipType::Video) {
                compute_video_bbox(state, gcl, p, w, h, gx0, gy0, gx1, gy1);
            } else {
                float gpx = gcl.eval_prop("pos_x",   state.playhead) * w + p.x;
                float gpy = gcl.eval_prop("pos_y",   state.playhead) * h + p.y;
                float ghw = w * gcl.eval_prop("scale_x", state.playhead) * 0.5f;
                float ghh = h * gcl.eval_prop("scale_y", state.playhead) * 0.5f;
                gx0 = gpx - ghw; gy0 = gpy - ghh; gx1 = gpx + ghw; gy1 = gpy + ghh;
            }
            bool overlaps_canvas = gx1 > p.x && gx0 < p.x + w &&
                                   gy1 > p.y && gy0 < p.y + h;
            if (overlaps_canvas) continue;
            bool gsel = (gti == state.selected_track && gci == state.selected_clip);
            ImU32 gc = gsel ? IM_COL32(255, 255, 255, 150)
                            : IM_COL32(255, 255, 255, 60);
            dl->AddRect({gx0, gy0}, {gx1, gy1}, gc, 3.f, 0, 1.5f);
            std::string gname = gcl.text.empty() ? "clip"
                              : fs::path(gcl.text).filename().string();
            ImVec2 gsz = ImGui::CalcTextSize(gname.c_str());
            if (gsz.x < gx1 - gx0 - 8.f)
                dl->AddText({(gx0 + gx1 - gsz.x) * 0.5f,
                             (gy0 + gy1 - gsz.y) * 0.5f}, gc, gname.c_str());
        }
    }

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
            render_snapshot_gl(state, state.playhead, true);
    }

    // Stamp time when render thread signals a new snapshot message
    if (state.snapshot_msg_new) {
        state.snapshot_msg_t   = ImGui::GetTime();
        state.snapshot_msg_new = false;
    }

    // Snapshot flash message — bottom-center, fades after 3 s
    // Agent-active pill badge — top-right corner, shown when agent is working on a live project
    if (state.agent_active && !state.tracks.empty()) {
        double t2 = ImGui::GetTime();
        float pulse = 0.6f + 0.4f * sinf((float)t2 * 3.f);  // pulsing dot
        const char* badge = "  agent working";
        float bsz = ImGui::GetFontSize() * 0.78f;
        ImVec2 bext = ImGui::GetFont()->CalcTextSizeA(bsz, FLT_MAX, -1.f, badge);
        float pad2  = 5.f;
        float dot_r = 3.5f;
        float bx    = p.x + w - bext.x - pad2 * 2.f - dot_r * 2.f - 8.f;
        float by    = p.y + 8.f;
        ImVec2 btl  = {bx - pad2, by - pad2 * 0.5f};
        ImVec2 bbr  = {bx + bext.x + dot_r * 2.f + 6.f + pad2, by + bext.y + pad2 * 0.5f};
        dl->AddRectFilled(btl, bbr, IM_COL32(22, 22, 28, 200), 8.f);
        dl->AddCircleFilled({bx + dot_r, by + bext.y * 0.5f}, dot_r,
            IM_COL32(160, 80, 255, (int)(pulse * 230.f)));
        dl->AddText(ImGui::GetFont(), bsz, {bx + dot_r * 2.f + 4.f, by},
            IM_COL32(180, 150, 220, 200), badge);
    }

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

    g_state_for_mirror = &state;
    draw_camera_mirror(dl, p, w, h);
}

// ── Canvas-source snapshot capture ───────────────────────────────────────────
// Runs after ImGui_ImplOpenGL3_RenderDrawData and before buffer swap (called
// from the main loop) so the back buffer holds the fully drawn frame. Reads
// the preview rect — the exact pixels the user sees, scene compositor output
// plus text overlays — and writes it as the snapshot PNG. This is the
// "source: canvas" ground-truth path; render_snapshot_gl is the export path.
#include "stb_image_write.h"

void canvas_capture_after_render(AppState& state) {
    if (g_canvas_cap.frames_left != 0) return;
    g_canvas_cap.frames_left = -1;

    auto fail = [&](const char* why) {
        state.snapshot_done_err = why;
        state.snapshot_done     = true;
    };

    ImGuiIO& io = ImGui::GetIO();
    float sx = io.DisplayFramebufferScale.x, sy = io.DisplayFramebufferScale.y;
    int fb_h = (int)(io.DisplaySize.y * sy);
    int rx, ry, rw, rh;
    if (g_canvas_cap.full_ui) {
        // "ui" source: the entire window backbuffer — full app state as the
        // user sees it (timeline, panels, canvas, popups).
        rx = 0; ry = 0;
        rw = (int)(io.DisplaySize.x * sx);
        rh = fb_h;
    } else {
        rx = (int)(g_canvas_cap.p.x * sx);
        rw = (int)(g_canvas_cap.w   * sx);
        rh = (int)(g_canvas_cap.h   * sy);
        // GL reads from the bottom-left; the rect's top is p.y in UI coords.
        ry = fb_h - (int)((g_canvas_cap.p.y + g_canvas_cap.h) * sy);
    }
    if (rw <= 0 || rh <= 0) { fail("canvas rect is empty"); return; }

    std::vector<uint8_t> raw((size_t)rw * rh * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(rx, ry, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());

    // Flip rows (GL bottom-up → PNG top-down) and force opaque alpha — the
    // window backbuffer's alpha channel is whatever compositing left there,
    // which makes the PNG look blank in viewers if kept.
    std::vector<uint8_t> img((size_t)rw * rh * 4);
    int rb = rw * 4;
    for (int y = 0; y < rh; ++y) {
        uint8_t* dst = img.data() + (size_t)y * rb;
        memcpy(dst, raw.data() + (size_t)(rh - 1 - y) * rb, rb);
        for (int x = 0; x < rw; ++x) dst[x*4 + 3] = 255;
    }

    // Same naming scheme as render_snapshot_gl, with a _canvas_ marker.
    std::string base_path = state.audio_path;
    if (base_path.empty()) {
        for (auto& tr : state.tracks) {
            for (auto& cl : tr.clips)
                if (cl.clip_type == ClipType::Video && !cl.text.empty())
                    { base_path = cl.text; break; }
            if (!base_path.empty()) break;
        }
    }
    int total_ms = (int)(state.playhead * 1000.f);
    int ms = total_ms % 1000, ss = (total_ms / 1000) % 60, mm = total_ms / 60000;
    char ts[32]; snprintf(ts, sizeof(ts), "%02dm%02ds%03dms", mm, ss, ms);
    // UI grabs are agent-debugging artifacts — keep them in /tmp instead of
    // littering the user's media folder like project snapshots do.
    std::string out;
    if (g_canvas_cap.full_ui) {
        out = std::string("/tmp/pop-maker-studio_ui_") + ts + ".png";
    } else if (base_path.empty()) {
        out = std::string("/tmp/pop-maker-studio_canvas_") + ts + ".png";
    } else {
        out = fs::path(base_path).parent_path().string() + "/" +
              fs::path(base_path).stem().string() + "_canvas_" + ts + ".png";
    }

    if (!stbi_write_png(out.c_str(), rw, rh, 4, img.data(), rb)) {
        fail("PNG write failed");
        return;
    }
    state.snapshot_msg       = "Saved " + fs::path(out).filename().string();
    state.snapshot_msg_new   = true;
    state.snapshot_done_path = out;
    state.snapshot_done_err.clear();
    state.snapshot_done      = true;
}
