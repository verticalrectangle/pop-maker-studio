#include "studio_types.h"
#include "studio_shared.h"
#include "canvas.h"
#include "../text_renderer.h"
#include "../text_anim.h"
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
#include "../ipc_server.h"
#include "render.h"
#include "waveform.h"
#include "body_fx.h"
#include "bg_remove.h"
#include "video_recorder.h"
#include "face_track.h"
#include "face_filters.h"
#include "face_cache.h"
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

// ── Social (TikTok/Reels/Shorts) safe-zone model ──────────────────────────────
// One conservative envelope covering all three vertical feeds — their exact UI
// pixels drift per release, so we approximate the chrome rather than chase one
// app. Values are fractions of a 9:16 canvas.
static constexpr float SOCIAL_TABS_T = 0.10f;  // top: For-You / search tabs
static constexpr float SOCIAL_CAP_B  = 0.22f;  // bottom: caption + handle + music
static constexpr float SOCIAL_RAIL_R = 0.12f;  // right: like / comment / share rail
static constexpr float SOCIAL_SIDE_L = 0.08f;  // left gutter (caption/handle reach)

// Active centre-snap target (canvas fractions). Normally the geometric centre;
// with the social overlay on in 9:16 it shifts to the centre of the *visible*
// (un-chromed) box — up and slightly left — so "drag to the middle" lands where
// viewers actually look, not under the caption or behind the action rail.
static inline void canvas_center_target(const AppState& s, float& cx, float& cy) {
    cx = 0.5f; cy = 0.5f;
    if (s.show_social_safe && s.format == OutputFormat::Vertical) {
        cx = (SOCIAL_SIDE_L + (1.f - SOCIAL_RAIL_R)) * 0.5f;  // ~0.46
        cy = (SOCIAL_TABS_T + (1.f - SOCIAL_CAP_B))  * 0.5f;  // ~0.44
    }
}

// ── Canvas object system ──────────────────────────────────────────────────────
// TextLayout: tight rendered bbox computed each frame during text draw.
// Persists so draw_canvas_handles can use accurate extents for handles.
struct TextLayout {
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;  // tight bbox (un-rotated, canvas px)
    float block_ax = 0.f;   // anchor X (canvas pixels)
    float fsz      = 0.f;   // final rendered font size
    float rot      = 0.f;   // clip rotation in degrees
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
static bool s_rot_snapped = false;   // rotate drag is currently angle-snapped

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

bool camera_live_native_dims(int& cw, int& ch);   // defined below

void compute_video_bbox(AppState& state, Clip& cl, ImVec2 p, float w, float h,
                                float& bx0, float& by0, float& bx1, float& by1) {
    float px = cl.eval_prop("pos_x",   state.playhead) * w + p.x;
    float py = cl.eval_prop("pos_y",   state.playhead) * h + p.y;
    float sx = cl.eval_prop("scale_x", state.playhead);
    float sy = cl.eval_prop("scale_y", state.playhead);
    float fit_w = w, fit_h = h;
    bool got_aspect = false;
    std::string vkey = clip_slot_key(clip_video_src(state, cl), cl.start);
    for (int s = 0; s < MAX_VIDEO_TRACKS; ++s) {
        if (state.proxy_paths[s] == vkey && video_info(s).width > 0) {
            // Crop changes the displayed aspect — bbox must match the render.
            float va = cl.cropped_aspect(video_info(s).width, video_info(s).height);
            float ca = w / h;
            if (va > ca) { fit_w = w; fit_h = w / va; }
            else         { fit_h = h; fit_w = h * va; }
            got_aspect = true;
            break;
        }
    }
    // Camera brick with no take yet (or live-monitoring): the box must match
    // the live preview, which fits the camera's native frame — so the
    // selection handles frame what's actually on screen.
    int cw = 0, ch = 0;
    if (!got_aspect && cl.clip_type == ClipType::VideoRecord &&
        camera_live_native_dims(cw, ch)) {
        float va = (float)cw / (float)ch, ca = w / h;
        if (va > ca) { fit_w = w; fit_h = w / va; }
        else         { fit_h = h; fit_w = h * va; }
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
        std::string vkey = clip_slot_key(clip_video_src(state, cl), cl.start);
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

static CanvasHandleGeom s_handle_geom;
CanvasHandleGeom canvas_handle_geom() { return s_handle_geom; }

// True when the mouse sits on one of the selection's transform handles
// (geometry from last frame's draw_canvas_handles). Layer picking runs
// earlier in the frame than draw_canvas_handles, so without this a click on
// the rotate knob — which sits OUTSIDE the selected clip's bbox — counted as
// a click on empty canvas (deselect) or on whatever clip sat underneath
// (reselect), and the rotate drag never began. Hit extents mirror the handle
// hit tests below (CR/EL/ES + 4px grace).
static bool mouse_on_handle_spot(ImVec2 m) {
    const CanvasHandleGeom& g = s_handle_geom;
    if (!g.valid) return false;
    const float CR = 4.5f;
    // Rotate knob (already in rotated screen position)
    if (fabsf(m.x - g.rot_x) <= CR+5.f && fabsf(m.y - g.rot_y) <= CR+5.f) return true;
    // Transform the mouse into the clip's LOCAL frame, then test the
    // un-rotated corner/edge handle spots (the handles rotate with the clip).
    float gcx = (g.bx0 + g.bx1) * 0.5f, gcy = (g.by0 + g.by1) * 0.5f;
    float lhw = (g.bx1 - g.bx0) * 0.5f, lhh = (g.by1 - g.by0) * 0.5f;
    float rad = -g.rot_deg * 3.14159265f / 180.f;   // inverse rotation
    float dx = m.x - gcx, dy = m.y - gcy;
    float lx = dx * cosf(rad) - dy * sinf(rad);
    float ly = dx * sinf(rad) + dy * cosf(rad);
    const float c = CR + 5.f;
    // Corners + edge mids (edges are small squares at mid-edge in local space)
    const float hx[3] = {-lhw, 0.f, lhw}, hy[3] = {-lhh, 0.f, lhh};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            if (i == 1 && j == 1) continue;          // skip center
            if (fabsf(lx - hx[i]) <= c && fabsf(ly - hy[j]) <= c) return true;
        }
    return false;
}

void draw_canvas_handles(AppState& state, ImDrawList* dl, ImVec2 p, float w, float h) {
    s_handle_geom = CanvasHandleGeom{};
    // Crop-edit mode replaces the normal transform handles entirely.
    if (state.crop_edit_track >= 0) { draw_crop_mode(state, dl, p, w, h); return; }
    if (state.selected_track < 0 || state.selected_clip < 0) return;
    if (state.selected_track >= (int)state.tracks.size()) return;
    Track& tr = state.tracks[state.selected_track];
    if (state.selected_clip >= (int)tr.clips.size()) return;

    // Glass FX bricks (Effect/MultiFX/BodyFX riding a video) carry no
    // transform of their own — selecting one hands the canvas handles to
    // the host clip underneath, so move/scale/rotate always works no
    // matter which layer of the stack is selected.
    int sel_ti = state.selected_track, sel_ci = state.selected_clip;
    {
        Clip& sc = tr.clips[(size_t)state.selected_clip];
        bool is_fx = sc.clip_type == ClipType::Effect ||
                     sc.clip_type == ClipType::MultiFX ||
                     sc.clip_type == ClipType::BodyFX;
        if (is_fx && fx_clip_is_glass(state, state.selected_track, sc)) {
            int host = fx_glass_host_index(state, state.selected_track, sc);
            if (host >= 0) sel_ci = host;
        }
    }
    Clip& cl = tr.clips[(size_t)sel_ci];

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

    // ── Handle visual constants ───────────────────────────────────────────────
    const float CR       = 4.5f;    // corner half-size
    const float ROT_DIST = 28.f;

    // Record handle geometry before the hover early-out — IPC clients driving
    // the canvas via ui_input need knob/bbox positions even while the real
    // cursor is parked outside the preview.
    if (clip_is_videolike_type(cl.clip_type) || cl.clip_type == ClipType::Background) {
        float gcx = cl.eval_prop("pos_x", state.playhead) * w + p.x;
        float gcy = cl.eval_prop("pos_y", state.playhead) * h + p.y;
        float ghw, ghh;
        if (cl.clip_type == ClipType::Background) {
            ghw = w * cl.eval_prop("scale_x", state.playhead) * 0.5f;
            ghh = h * cl.eval_prop("scale_y", state.playhead) * 0.5f;
        } else {
            float gx0, gy0, gx1, gy1;
            compute_video_bbox(state, cl, p, w, h, gx0, gy0, gx1, gy1);
            ghw = (gx1-gx0)*0.5f; ghh = (gy1-gy0)*0.5f;
        }
        // Rotated knob position so the rotate-click veto works when tilted.
        float gdeg = cl.eval_prop("rotation", state.playhead);
        float grad = gdeg * 3.14159265f / 180.f;
        float kx = gcx - (-(ghh + ROT_DIST)) * sinf(grad);
        float ky = gcy + (-(ghh + ROT_DIST)) * cosf(grad);
        s_handle_geom = {true, gcx-ghw, gcy-ghh, gcx+ghw, gcy+ghh,
                         kx, ky, gcx, gcy, gdeg};
    } else if (cl.clip_type == ClipType::Text || cl.clip_type == ClipType::Subtitle ||
               cl.clip_type == ClipType::Lyrics) {
        // Text geometry comes from last frame's rendered TextLayout.
        uint64_t tlk = ((uint64_t)sel_ti << 32) | (uint32_t)sel_ci;
        auto it = s_text_layouts.find(tlk);
        if (it != s_text_layouts.end() && it->second.valid) {
            const TextLayout& tl = it->second;
            float gcx = (tl.x0+tl.x1)*0.5f, gcy = (tl.y0+tl.y1)*0.5f;
            float ghh = (tl.y1-tl.y0)*0.5f, grad = tl.rot * 3.14159265f / 180.f;
            float kx = gcx - (-(ghh + ROT_DIST)) * sinf(grad);
            float ky = gcy + (-(ghh + ROT_DIST)) * cosf(grad);
            s_handle_geom = {true, tl.x0, tl.y0, tl.x1, tl.y1,
                             kx, ky, gcx, gcy, tl.rot};
        }
    }

    if (!in_preview && !drag_active) return;

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
    auto begin_drag = [&](CanvasHandle ht) {
        s_ctx.handle    = ht;
        s_ctx.track_idx = sel_ti;
        s_ctx.clip_idx  = sel_ci;
        s_ctx.drag_sx   = mpos.x;
        s_ctx.drag_sy   = mpos.y;
        s_ctx.dirty     = false;
    };

    // Snap a dragged element's centre to alignment guides: the canvas borders,
    // the canvas/social centre, and (with the social overlay on) the safe-box
    // edges. ecx/ecy = element centre in screen px; hw/hh = its half-extents;
    // tol = catch radius. Updates ecx/ecy in place and draws the active guides.
    auto snap_move = [&](float& ecx, float& ecy, float hw, float hh, float tol) {
        float tcx, tcy; canvas_center_target(state, tcx, tcy);
        struct Guide { float line, off; };   // off = element ref point vs centre
        Guide xc[6]; int nx = 0;
        Guide yc[6]; int ny = 0;
        xc[nx++] = { p.x,           -hw };    // canvas left border
        xc[nx++] = { p.x + w,       +hw };    // canvas right border
        xc[nx++] = { p.x + w * tcx,  0.f };   // centre (canvas / social)
        yc[ny++] = { p.y,           -hh };    // canvas top border
        yc[ny++] = { p.y + h,       +hh };    // canvas bottom border
        yc[ny++] = { p.y + h * tcy,  0.f };   // centre
        if (state.show_social_safe && state.format == OutputFormat::Vertical) {
            xc[nx++] = { p.x + SOCIAL_SIDE_L * w,         -hw };
            xc[nx++] = { p.x + (1.f - SOCIAL_RAIL_R) * w, +hw };
            yc[ny++] = { p.y + SOCIAL_TABS_T * h,         -hh };
            yc[ny++] = { p.y + (1.f - SOCIAL_CAP_B) * h,  +hh };
        }
        float bd = tol; int bi = -1;
        for (int i = 0; i < nx; ++i) {
            float d = fabsf((ecx + xc[i].off) - xc[i].line);
            if (d < bd) { bd = d; bi = i; }
        }
        if (bi >= 0) {
            ecx = xc[bi].line - xc[bi].off;
            dl->AddLine({xc[bi].line, p.y}, {xc[bi].line, p.y + h}, snap_col);
        }
        bd = tol; bi = -1;
        for (int i = 0; i < ny; ++i) {
            float d = fabsf((ecy + yc[i].off) - yc[i].line);
            if (d < bd) { bd = d; bi = i; }
        }
        if (bi >= 0) {
            ecy = yc[bi].line - yc[bi].off;
            dl->AddLine({p.x, yc[bi].line}, {p.x + w, yc[bi].line}, snap_col);
        }
    };

    // ── Rotation-aware transform handles (video + background share this) ──────
    // Box, corner/edge handles, and rotate knob all rotate with the clip;
    // hit-testing and scale drags work in the clip's LOCAL (un-rotated) space.
    // cx,cy: box center (screen px). lhw,lhh: LOCAL half-extents (px). Reads/
    // writes mc.pos_x/pos_y/scale_x/scale_y/rotation.
    auto do_transform = [&](Clip& mc, float cx, float cy, float lhw, float lhh) {
        float rad = mc.eval_prop("rotation", state.playhead) * 3.14159265f / 180.f;
        float axx = cosf(rad), axy = sinf(rad);    // local +x in screen
        float ayx = -sinf(rad), ayy = cosf(rad);   // local +y (down) in screen
        auto L = [&](float lx, float ly) {         // local → screen
            return ImVec2{cx + lx*axx + ly*ayx, cy + lx*axy + ly*ayy};
        };
        ImVec2 TL = L(-lhw,-lhh), TR = L(lhw,-lhh), BR = L(lhw,lhh), BL = L(-lhw,lhh);
        ImVec2  knob = L(0.f, -lhh - ROT_DIST);
        ImVec2 etop = L(0.f,-lhh), ebot = L(0.f,lhh), elef = L(-lhw,0.f), erig = L(lhw,0.f);

        // Box outline
        dl->AddQuad(TL, TR, BR, BL, box_col, 1.5f);

        // Rotate knob — turns the snap colour while angle-snapped (45° steps).
        bool rot_act  = (s_ctx.handle == CanvasHandle::Rotate);
        bool rot_snap = rot_act && s_rot_snapped;
        ImU32 knob_col = rot_snap ? snap_col : (rot_act ? hdl_hov : hdl_col);
        dl->AddLine(etop, knob, rot_snap ? snap_col : IM_COL32(255,255,255,80));
        dl->AddCircleFilled(knob, CR+1.5f, IM_COL32(0,0,0,120));
        dl->AddCircle(knob, CR+1.5f, knob_col);
        float rdist = sqrtf((mpos.x-knob.x)*(mpos.x-knob.x) + (mpos.y-knob.y)*(mpos.y-knob.y));
        if (rdist <= CR+5.f && lclick && s_ctx.handle == CanvasHandle::None) {
            begin_drag(CanvasHandle::Rotate);
            s_ctx.start_rot = mc.rotation;
        }

        // Store local extents on drag-begin so scale math is rotation-free.
        auto begin_scale = [&](CanvasHandle ht) {
            begin_drag(ht);
            s_ctx.start_scale_x = mc.scale_x; s_ctx.start_scale_y = mc.scale_y;
            s_ctx.start_bbox_x0 = 0.f; s_ctx.start_bbox_x1 = 2.f*lhw;  // orig_w
            s_ctx.start_bbox_y0 = 0.f; s_ctx.start_bbox_y1 = 2.f*lhh;  // orig_h
        };
        if (draw_corner_h(TL.x, TL.y, CanvasHandle::CornerTL)) begin_scale(CanvasHandle::CornerTL);
        if (draw_corner_h(TR.x, TR.y, CanvasHandle::CornerTR)) begin_scale(CanvasHandle::CornerTR);
        if (draw_corner_h(BR.x, BR.y, CanvasHandle::CornerBR)) begin_scale(CanvasHandle::CornerBR);
        if (draw_corner_h(BL.x, BL.y, CanvasHandle::CornerBL)) begin_scale(CanvasHandle::CornerBL);
        // Edge handles: small squares (rotation-agnostic) at rotated mids.
        if (draw_corner_h(etop.x, etop.y, CanvasHandle::EdgeT)) begin_scale(CanvasHandle::EdgeT);
        if (draw_corner_h(ebot.x, ebot.y, CanvasHandle::EdgeB)) begin_scale(CanvasHandle::EdgeB);
        if (draw_corner_h(elef.x, elef.y, CanvasHandle::EdgeL)) begin_scale(CanvasHandle::EdgeL);
        if (draw_corner_h(erig.x, erig.y, CanvasHandle::EdgeR)) begin_scale(CanvasHandle::EdgeR);

        // Interior → move (point-in-rotated-rect via local projection)
        float mlx = (mpos.x-cx)*axx + (mpos.y-cy)*axy;
        float mly = (mpos.x-cx)*ayx + (mpos.y-cy)*ayy;
        bool inside = fabsf(mlx) < lhw - CR*2 && fabsf(mly) < lhh - CR*2;
        if (inside) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            if (lclick && s_ctx.handle == CanvasHandle::None) {
                begin_drag(CanvasHandle::Body);
                s_ctx.start_pos_x = mc.pos_x; s_ctx.start_pos_y = mc.pos_y;
            }
        }

        // Apply drag
        if (drag_active && s_ctx.track_idx == sel_ti && s_ctx.clip_idx == sel_ci) {
            float dmx = mpos.x - s_ctx.drag_sx, dmy = mpos.y - s_ctx.drag_sy;
            // Ignore zero-movement clicks (selection only) — a click must not
            // nudge position, push history, or center-snap a near-centred clip.
            bool real_drag = s_ctx.dirty || fabsf(dmx) > 3.f || fabsf(dmy) > 3.f;
            if (real_drag) {
                float dlx = dmx*axx + dmy*axy;   // delta in local +x
                float dly = dmx*ayx + dmy*ayy;   // delta in local +y (down)
                float orig_w = s_ctx.start_bbox_x1, orig_h = s_ctx.start_bbox_y1;
                switch (s_ctx.handle) {
                    case CanvasHandle::Body:
                        mc.pos_x = fmaxf(-1.f, fminf(2.f, s_ctx.start_pos_x + dmx/w));
                        mc.pos_y = fmaxf(-1.f, fminf(2.f, s_ctx.start_pos_y + dmy/h));
                        break;
                    case CanvasHandle::Rotate: {
                        float a0 = atan2f(s_ctx.drag_sy - cy, s_ctx.drag_sx - cx);
                        float a1 = atan2f(mpos.y - cy,        mpos.x - cx);
                        float raw = fmodf(s_ctx.start_rot + (a1-a0)*180.f/3.14159265f, 360.f);
                        if (raw < 0.f) raw += 360.f;
                        // Snap to the nearest 45° (covers 45/90/135/180/…) when
                        // within ~6°, unless Shift is held for free rotation.
                        float snapped = roundf(raw / 45.f) * 45.f;
                        bool snap = !ImGui::GetIO().KeyShift && fabsf(raw - snapped) < 6.f;
                        mc.rotation = fmodf(snap ? snapped : raw, 360.f);
                        s_rot_snapped = snap;
                        break;
                    }
                    case CanvasHandle::CornerTL: case CanvasHandle::CornerTR:
                        if (orig_h > 0.f) {
                            float s = (orig_h - dly) / orig_h;
                            mc.scale_x = fmaxf(0.05f, s_ctx.start_scale_x * s);
                            mc.scale_y = fmaxf(0.05f, s_ctx.start_scale_y * s);
                        }
                        break;
                    case CanvasHandle::CornerBL: case CanvasHandle::CornerBR:
                        if (orig_h > 0.f) {
                            float s = (orig_h + dly) / orig_h;
                            mc.scale_x = fmaxf(0.05f, s_ctx.start_scale_x * s);
                            mc.scale_y = fmaxf(0.05f, s_ctx.start_scale_y * s);
                        }
                        break;
                    case CanvasHandle::EdgeT:
                        if (orig_h > 0.f) mc.scale_y = fmaxf(0.05f, s_ctx.start_scale_y * (orig_h-dly)/orig_h);
                        break;
                    case CanvasHandle::EdgeB:
                        if (orig_h > 0.f) mc.scale_y = fmaxf(0.05f, s_ctx.start_scale_y * (orig_h+dly)/orig_h);
                        break;
                    case CanvasHandle::EdgeL:
                        if (orig_w > 0.f) mc.scale_x = fmaxf(0.05f, s_ctx.start_scale_x * (orig_w-dlx)/orig_w);
                        break;
                    case CanvasHandle::EdgeR:
                        if (orig_w > 0.f) mc.scale_x = fmaxf(0.05f, s_ctx.start_scale_x * (orig_w+dlx)/orig_w);
                        break;
                    default: break;
                }
                s_ctx.dirty = true;

                // Border snap for resize — rotation-correct. As you drag a handle
                // the moved point P (the edge's midpoint, or the grabbed corner)
                // travels along a FIXED screen-space direction d as the scale
                // grows: P = C + t·d, where t is the half-extent (edges) or the
                // uniform factor (corners) and C is the box centre. So to land P
                // exactly on a canvas border we just solve for the t that puts it
                // there. This works at any rotation — at 90/270° the dragged edge
                // is axis-aligned and it gives a clean full-bleed fill, which the
                // old "snap scale to 1.0" couldn't do once the clip was rotated.
                {
                    CanvasHandle hd = s_ctx.handle;
                    float fit_w = (s_ctx.start_scale_x > 1e-6f) ? orig_w / s_ctx.start_scale_x : 0.f;
                    float fit_h = (s_ctx.start_scale_y > 1e-6f) ? orig_h / s_ctx.start_scale_y : 0.f;
                    float dx = 0.f, dy = 0.f, t_cur = 0.f;
                    int mode = 0;   // 1 = scale_x edge, 2 = scale_y edge, 3 = corner
                    float sgnx = 0.f, sgny = 0.f;
                    switch (hd) {
                        case CanvasHandle::EdgeR: dx =  axx; dy =  axy; t_cur = fit_w*mc.scale_x*0.5f; mode = 1; break;
                        case CanvasHandle::EdgeL: dx = -axx; dy = -axy; t_cur = fit_w*mc.scale_x*0.5f; mode = 1; break;
                        case CanvasHandle::EdgeB: dx =  ayx; dy =  ayy; t_cur = fit_h*mc.scale_y*0.5f; mode = 2; break;
                        case CanvasHandle::EdgeT: dx = -ayx; dy = -ayy; t_cur = fit_h*mc.scale_y*0.5f; mode = 2; break;
                        case CanvasHandle::CornerTL: sgnx = -1.f; sgny = -1.f; mode = 3; break;
                        case CanvasHandle::CornerTR: sgnx =  1.f; sgny = -1.f; mode = 3; break;
                        case CanvasHandle::CornerBR: sgnx =  1.f; sgny =  1.f; mode = 3; break;
                        case CanvasHandle::CornerBL: sgnx = -1.f; sgny =  1.f; mode = 3; break;
                        default: break;
                    }
                    if (mode == 3) {
                        // The corner rides the uniform factor f about the centre.
                        dx = sgnx*(orig_w*0.5f)*axx + sgny*(orig_h*0.5f)*ayx;
                        dy = sgnx*(orig_w*0.5f)*axy + sgny*(orig_h*0.5f)*ayy;
                        t_cur = (s_ctx.start_scale_x > 1e-6f) ? mc.scale_x / s_ctx.start_scale_x : 1.f;
                    }
                    if (mode != 0 && fit_w > 0.f && fit_h > 0.f) {
                        float dlen = sqrtf(dx*dx + dy*dy);
                        const float TOLPX = fmaxf(8.f, 0.022f * w);
                        float vlines[2] = { p.x, p.x + w };
                        float hlines[2] = { p.y, p.y + h };
                        float best_d = TOLPX, best_t = t_cur, snap_line = 0.f;
                        bool  snapped = false, snap_vert = false;
                        for (int i = 0; i < 2; ++i) if (fabsf(dx) > 1e-3f) {
                            float tc = (vlines[i] - cx) / dx;
                            float d  = fabsf(tc - t_cur) * dlen;
                            if (tc > 1e-3f && d < best_d) { best_d = d; best_t = tc; snapped = true; snap_vert = true;  snap_line = vlines[i]; }
                        }
                        for (int i = 0; i < 2; ++i) if (fabsf(dy) > 1e-3f) {
                            float tc = (hlines[i] - cy) / dy;
                            float d  = fabsf(tc - t_cur) * dlen;
                            if (tc > 1e-3f && d < best_d) { best_d = d; best_t = tc; snapped = true; snap_vert = false; snap_line = hlines[i]; }
                        }
                        if (snapped) {
                            if      (mode == 1) mc.scale_x = fmaxf(0.05f, 2.f*best_t/fit_w);
                            else if (mode == 2) mc.scale_y = fmaxf(0.05f, 2.f*best_t/fit_h);
                            else { mc.scale_x = fmaxf(0.05f, s_ctx.start_scale_x*best_t);
                                   mc.scale_y = fmaxf(0.05f, s_ctx.start_scale_y*best_t); }
                            if (snap_vert) dl->AddLine({snap_line, p.y}, {snap_line, p.y + h}, snap_col, 1.5f);
                            else           dl->AddLine({p.x, snap_line}, {p.x + w, snap_line}, snap_col, 1.5f);
                        }
                    }
                }

                // Snap move to canvas borders, centre, and safe-box edges.
                if (s_ctx.handle == CanvasHandle::Body) {
                    float ecx = mc.pos_x * w + p.x, ecy = mc.pos_y * h + p.y;
                    snap_move(ecx, ecy, lhw, lhh, 7.f);
                    mc.pos_x = (ecx - p.x) / w;
                    mc.pos_y = (ecy - p.y) / h;
                }
            }
        }
    };

    // ── Video clip ────────────────────────────────────────────────────────────
    if (clip_is_videolike_type(cl.clip_type)) {
        float bx0, by0, bx1, by1;
        compute_video_bbox(state, cl, p, w, h, bx0, by0, bx1, by1);
        float vcx = cl.eval_prop("pos_x", state.playhead) * w + p.x;
        float vcy = cl.eval_prop("pos_y", state.playhead) * h + p.y;
        do_transform(state.tracks[sel_ti].clips[(size_t)sel_ci], vcx, vcy,
                     (bx1-bx0)*0.5f, (by1-by0)*0.5f);
    }

    // ── Background clip ───────────────────────────────────────────────────────
    if (cl.clip_type == ClipType::Background) {
        float px2 = cl.eval_prop("pos_x",   state.playhead) * w + p.x;
        float py2 = cl.eval_prop("pos_y",   state.playhead) * h + p.y;
        float sx2 = cl.eval_prop("scale_x", state.playhead);
        float sy2 = cl.eval_prop("scale_y", state.playhead);
        do_transform(state.tracks[sel_ti].clips[(size_t)sel_ci], px2, py2,
                     w * sx2 * 0.5f, h * sy2 * 0.5f);
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

        // Rotation-aware: box + handles rotate with the clip; hit-testing and
        // size/wrap drags work in the block's LOCAL frame (same as video/bg).
        float trad = tl.rot * 3.14159265f / 180.f;
        float tax = cosf(trad), tay = sinf(trad);          // local +x in screen
        float lhw = (bx1-bx0)*0.5f, lhh = (by1-by0)*0.5f;
        auto TL_ = [&](float lx, float ly) {
            return ImVec2{tmx + lx*tax - ly*tay, tmy + lx*tay + ly*tax};
        };
        ImVec2 qTL = TL_(-lhw,-lhh), qTR = TL_(lhw,-lhh), qBR = TL_(lhw,lhh), qBL = TL_(-lhw,lhh);
        ImVec2 eTm = TL_(0,-lhh), eBm = TL_(0,lhh), eLm = TL_(-lhw,0), eRm = TL_(lhw,0);
        ImVec2 tknob = TL_(0, -lhh - ROT_DIST);

        // Box outline
        dl->AddQuad(qTL, qTR, qBR, qBL, box_col, 1.5f);

        // Rotate knob (with 45° snapping, same cue as the other clips)
        bool trot_act  = (s_ctx.handle == CanvasHandle::Rotate);
        bool trot_snap = trot_act && s_rot_snapped;
        ImU32 tkc = trot_snap ? snap_col : (trot_act ? hdl_hov : hdl_col);
        dl->AddLine(eTm, tknob, trot_snap ? snap_col : IM_COL32(255,255,255,80));
        dl->AddCircleFilled(tknob, CR+1.5f, IM_COL32(0,0,0,120));
        dl->AddCircle(tknob, CR+1.5f, tkc);
        if (sqrtf((mpos.x-tknob.x)*(mpos.x-tknob.x)+(mpos.y-tknob.y)*(mpos.y-tknob.y)) <= CR+5.f &&
            lclick && s_ctx.handle == CanvasHandle::None) {
            begin_drag(CanvasHandle::Rotate);
            s_ctx.start_rot = cl.rotation;
        }

        // Corners → scale font size
        auto txt_corner = [&](ImVec2 pos, CanvasHandle ht) {
            if (draw_corner_h(pos.x, pos.y, ht)) {
                begin_drag(ht);
                s_ctx.start_font_size = cl.font_size > 0.f ? cl.font_size : 0.09f;
                s_ctx.start_bbox_y0 = by0; s_ctx.start_bbox_y1 = by1;
            }
        };
        txt_corner(qTL, CanvasHandle::CornerTL); txt_corner(qTR, CanvasHandle::CornerTR);
        txt_corner(qBR, CanvasHandle::CornerBR); txt_corner(qBL, CanvasHandle::CornerBL);

        // Left/Right edges → wrap width (square handles at rotated mids)
        auto txt_wrap = [&](ImVec2 pos, CanvasHandle ht) {
            if (draw_corner_h(pos.x, pos.y, ht)) {
                begin_drag(ht);
                s_ctx.start_wrap_w = cl.sub_wrap_w; s_ctx.start_anchor = cl.sub_anchor_h;
                s_ctx.start_bbox_x0 = bx0; s_ctx.start_bbox_x1 = bx1;
            }
        };
        txt_wrap(eLm, CanvasHandle::EdgeL); txt_wrap(eRm, CanvasHandle::EdgeR);

        // Top/Bottom edges → vertical nudge
        auto txt_vnudge = [&](ImVec2 pos, CanvasHandle ht) {
            if (draw_corner_h(pos.x, pos.y, ht)) {
                begin_drag(ht);
                s_ctx.start_pos_y = (tmy - p.y) / h;
            }
        };
        txt_vnudge(eTm, CanvasHandle::EdgeT); txt_vnudge(eBm, CanvasHandle::EdgeB);

        // Interior → move (point-in-rotated-rect)
        float mlx = (mpos.x-tmx)*tax + (mpos.y-tmy)*tay;
        float mly = -(mpos.x-tmx)*tay + (mpos.y-tmy)*tax;
        bool in_txt = fabsf(mlx) < lhw - CR*2 && fabsf(mly) < lhh - CR*2;
        if (in_txt) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            if (lclick && s_ctx.handle == CanvasHandle::None) {
                begin_drag(CanvasHandle::Body);
                s_ctx.start_pos_x  = (tmx - p.x) / w;
                s_ctx.start_pos_y  = (tmy - p.y) / h;
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
            // A click that only selects must NOT be treated as a drag: until the
            // pointer actually moves, skip every mutation. Otherwise clicking
            // text on the canvas flipped it to custom position (sub_pos=3) +
            // anchor, clobbering the Clip panel's Bottom/Center/Top + anchor.
            bool real_drag = s_ctx.dirty || fabsf(dmx) > 3.f || fabsf(dmy) > 3.f;
            if (real_drag) {
                // Local-frame projection of the drag delta (font size + wrap
                // edit along the block's own axes when it's rotated).
                float dly = -(dmx*tay - dmy*tax);   // local +y down → matches dmy at rot 0
                float dlx =   dmx*tax + dmy*tay;
                Clip& mc = state.tracks[s_ctx.track_idx].clips[s_ctx.clip_idx];
                float orig_bbox_h = s_ctx.start_bbox_y1 - s_ctx.start_bbox_y0;

                switch (s_ctx.handle) {
                    case CanvasHandle::Rotate: {
                        float a0 = atan2f(s_ctx.drag_sy - tmy, s_ctx.drag_sx - tmx);
                        float a1 = atan2f(mpos.y - tmy,        mpos.x - tmx);
                        float raw = fmodf(s_ctx.start_rot + (a1-a0)*180.f/3.14159265f, 360.f);
                        if (raw < 0.f) raw += 360.f;
                        float snapped = roundf(raw / 45.f) * 45.f;
                        bool snap = !ImGui::GetIO().KeyShift && fabsf(raw - snapped) < 6.f;
                        mc.rotation = fmodf(snap ? snapped : raw, 360.f);
                        s_rot_snapped = snap;
                        break;
                    }
                    case CanvasHandle::Body:
                        mc.sub_pos      = 3;
                        mc.sub_anchor_h = 1;
                        // Text moves as freely as any other canvas object — past
                        // the safe margins and off-canvas if you want — not penned
                        // into the safe zone. Same [-1, 2] range as image/video.
                        mc.sub_pos_x    = fmaxf(-1.f, fminf(2.f, s_ctx.start_pos_x + dmx/w));
                        mc.sub_pos_y    = fmaxf(-1.f, fminf(2.f, s_ctx.start_pos_y + dmy/h));
                        break;
                    case CanvasHandle::CornerTL: case CanvasHandle::CornerTR:
                        if (orig_bbox_h > 0.f) {
                            float scale = (orig_bbox_h - dly) / orig_bbox_h;
                            mc.font_size = fmaxf(0.02f, fminf(0.5f, s_ctx.start_font_size * scale));
                        }
                        break;
                    case CanvasHandle::CornerBL: case CanvasHandle::CornerBR:
                        if (orig_bbox_h > 0.f) {
                            float scale = (orig_bbox_h + dly) / orig_bbox_h;
                            mc.font_size = fmaxf(0.02f, fminf(0.5f, s_ctx.start_font_size * scale));
                        }
                        break;
                    case CanvasHandle::EdgeL: case CanvasHandle::EdgeR: {
                        // Wrap width along the block's local-x; grow symmetrically
                        // about the centre so it stays anchored under rotation.
                        float start_w_px = s_ctx.start_bbox_x1 - s_ctx.start_bbox_x0;
                        float new_w = start_w_px +
                                      (s_ctx.handle == CanvasHandle::EdgeR ? dlx : -dlx);
                        if (new_w > 20.f) {
                            mc.sub_wrap_w   = fmaxf(0.08f, fminf(0.98f, new_w/w));
                            mc.sub_anchor_h = 1;
                        }
                        break;
                    }
                    case CanvasHandle::EdgeT: case CanvasHandle::EdgeB:
                        mc.sub_pos   = 3;
                        mc.sub_pos_y = fmaxf(-1.f, fminf(2.f, s_ctx.start_pos_y + dmy/h));
                        break;
                    default: break;
                }
                s_ctx.dirty = true;

                // Center snap for text body move — magnetic to the canvas centre
                // on BOTH axes so dragging text "to the middle" actually catches.
                // The old 8 px window (~3% of the canvas) was too tight to land on
                // by hand, and only the horizontal axis snapped. A wider radius +
                // a vertical centre line make centring obvious while dragging.
                if (s_ctx.handle == CanvasHandle::Body) {
                    // Text body drag is centre-anchored (sub_anchor_h forced to 1),
                    // so sub_pos_* is the centre. Snap to borders / centre / safe box.
                    float hw = (tl.x1 - tl.x0) * 0.5f, hh = (tl.y1 - tl.y0) * 0.5f;
                    float ecx = mc.sub_pos_x * w + p.x, ecy = mc.sub_pos_y * h + p.y;
                    snap_move(ecx, ecy, hw, hh, 12.f);
                    mc.sub_pos_x = (ecx - p.x) / w;
                    mc.sub_pos_y = (ecy - p.y) / h;
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

// ── Live camera preview ───────────────────────────────────────────────────────
// While the camera brick monitors, warms up, or records, the latest camera
// frame is composited into the scene AT THE BRICK'S TRANSFORM (pos/scale/
// rotation) — framed exactly like the take will sit, only mirror-flipped for
// self-view. Glass FX + face filter are baked into the texture so the live
// preview and the recorded take render through the same machinery.
static FaceObs s_mirror_obs;          // latest tracked face (raw-frame coords)
static MirrorDebugGeom s_mirror_dbg;
MirrorDebugGeom mirror_debug_geom() { return s_mirror_dbg; }
static int     s_mirror_filter = 0;
static float   s_mirror_filter_amt = 1.f;

// Decoded live frame — file scope so compute_video_bbox can read native dims.
static GLuint   s_cam_tex    = 0;
static int      s_cam_w      = 0, s_cam_h = 0;
static uint64_t s_cam_serial = 0;
static tjhandle s_cam_tj     = nullptr;
static std::vector<uint8_t> s_rgba;

// Native dims of the live camera frame, valid only while a frame is decoded.
bool camera_live_native_dims(int& cw, int& ch) {
    if (!s_cam_tex || s_cam_w <= 0) return false;
    cw = s_cam_w; ch = s_cam_h; return true;
}

// The live preview's frame outline (rotated quad, ImGui-space corners) +
// state, stashed during compositing so the REC border + label draw on the
// ImGui list after the scene blit.
static struct {
    bool   active = false;
    ImVec2 c[4]   = {};
    bool   recording = false, warming = false;
} s_cam_box;

// The camera brick that drives the live preview: the selected one, else the
// record target's, else the first VideoRecord brick. Returns its track index.
static const Clip* camera_brick_find(AppState& st, int& out_ti) {
    out_ti = -1;
    if (st.selected_track >= 0 && st.selected_track < (int)st.tracks.size() &&
        st.selected_clip >= 0 &&
        st.selected_clip < (int)st.tracks[st.selected_track].clips.size()) {
        const Clip& c2 = st.tracks[st.selected_track].clips[st.selected_clip];
        if (c2.clip_type == ClipType::VideoRecord) {
            out_ti = st.selected_track;
            return &c2;
        }
    }
    for (int ti = 0; ti < (int)st.tracks.size(); ++ti)
        for (auto& c2 : st.tracks[(size_t)ti].clips)
            if (c2.clip_type == ClipType::VideoRecord) { out_ti = ti; return &c2; }
    return nullptr;
}

// Decode the latest camera frame and bake the brick's glass FX + face filter
// into a texture. Returns 0 when no frame / no brick; fills out_brick/ti/dims.
static uintptr_t camera_live_tex(AppState& st, const Clip*& out_brick,
                                 int& out_ti, int& out_w, int& out_h) {
    out_brick = nullptr; out_ti = -1; out_w = out_h = 0;

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
    if (!s_cam_tex || s_cam_w <= 0) return 0;

    int bti = -1;
    const Clip* br = camera_brick_find(st, bti);
    if (!br || bti < 0) return 0;
    out_brick = br; out_ti = bti;
    out_w = s_cam_w; out_h = s_cam_h;

    uintptr_t draw_tex = s_cam_tex;

    {
        {
            // Run the brick's glass FX chain (beauty MultiFX etc.) on the
            // live frame — the preview shows you filtered, like the take will.
            {
                {
                    // Sample the chain at the live playhead within the brick span
                    // (clamped), so windowed / keyframed / beat-synced sub-effects
                    // activate over their real spans while recording — not frozen
                    // at the brick's first instant (which hid any FX not starting
                    // at rel 0 until the take was placed).
                    float bt = br->start + 0.001f;
                    if (br->end > br->start)
                        bt = fminf(fmaxf(st.playhead, br->start), br->end - 0.001f);
                    EffectAccum     ea  = collect_glass_effects(st, bt, bti);
                    CreativeFXAccum cfx = collect_glass_fx(st, bt, bti);
                    // kSceneFxSlot (MAX*2-2) belongs to the scene
                    // compositor every frame — sharing it collided FBOs and
                    // rendered the preview solid green. The last slot index
                    // is unclaimed by clips and the scene pass.
                    draw_tex = fx_apply((uintptr_t)s_cam_tex,
                                        MAX_VIDEO_TRACKS * 2 - 1,
                                        s_cam_w, s_cam_h, ea, cfx,
                                        (float)ImGui::GetTime());
                }
            }

            // Face filter: track the live frame, warp the texture. The
            // detector can't see sideways faces, so the tracker gets the
            // frame rotated upright per the brick's rotation (90° steps)
            // and landmarks are mapped back into raw-frame coords.
            s_mirror_filter     = br->face_filter;
            s_mirror_filter_amt = br->face_filter_amt;
            if (br->face_filter != 0 && face_track_available()) {
                int rot_q = ((int)lroundf(br->rotation / 90.f) % 4 + 4) % 4;
                static uint64_t s_track_serial = 0;
                static int s_sub_rotq = 0;
                if (serial != s_track_serial && !s_rgba.empty()) {
                    // Half-res submit: 4× less conversion + inference input;
                    // landmark precision at half-res is ample for warps.
                    static std::vector<uint8_t> rgb;
                    const uint8_t* src4 = s_rgba.data();
                    int W = s_cam_w, H = s_cam_h;
                    int hw2 = W / 2, hh2 = H / 2;
                    if (rot_q == 0 || rot_q == 2) {
                        rgb.resize((size_t)hw2 * hh2 * 3);
                        for (int y = 0; y < hh2; ++y)
                            for (int x = 0; x < hw2; ++x) {
                                size_t si = ((size_t)(y*2) * W + x*2) * 4;
                                size_t di = rot_q == 0
                                    ? ((size_t)y * hw2 + x) * 3
                                    : ((size_t)(hh2-1-y) * hw2 + (hw2-1-x)) * 3;
                                rgb[di+0] = src4[si+0];
                                rgb[di+1] = src4[si+1];
                                rgb[di+2] = src4[si+2];
                            }
                        face_track_submit(rgb.data(), hw2, hh2);
                    } else {
                        // 90° CW (rot_q 1) or CCW (rot_q 3): dims swap.
                        rgb.resize((size_t)hw2 * hh2 * 3);
                        for (int y = 0; y < hh2; ++y)
                            for (int x = 0; x < hw2; ++x) {
                                size_t si = ((size_t)(y*2) * W + x*2) * 4;
                                int ux, uy;   // position in upright (hh2×hw2) frame
                                if (rot_q == 1) { ux = hh2 - 1 - y; uy = x; }
                                else            { ux = y;           uy = hw2 - 1 - x; }
                                size_t di = ((size_t)uy * hh2 + ux) * 3;
                                rgb[di+0] = src4[si+0];
                                rgb[di+1] = src4[si+1];
                                rgb[di+2] = src4[si+2];
                            }
                        face_track_submit(rgb.data(), hh2, hw2);
                    }
                    s_track_serial = serial;
                    s_sub_rotq = rot_q;
                }
                FaceObs obs;
                if (face_track_latest(obs)) {
                    // Map landmarks back into RAW full-res frame coords
                    // (tracker ran on the half-res upright frame).
                    {
                        FaceObs raw = obs;
                        raw.w = s_cam_w; raw.h = s_cam_h;
                        float hw2f = (float)(s_cam_w / 2), hh2f = (float)(s_cam_h / 2);
                        for (int k = 0; k < 106; ++k) {
                            float ux = obs.pts[k][0], uy = obs.pts[k][1];
                            float rx2, ry2;   // half-res raw coords
                            if (s_sub_rotq == 1)      { rx2 = uy;             ry2 = hh2f - 1.f - ux; }
                            else if (s_sub_rotq == 3) { rx2 = hw2f - 1.f - uy; ry2 = ux; }
                            else if (s_sub_rotq == 2) { rx2 = hw2f - 1.f - ux; ry2 = hh2f - 1.f - uy; }
                            else                      { rx2 = ux;             ry2 = uy; }
                            raw.pts[k][0] = rx2 * 2.f;
                            raw.pts[k][1] = ry2 * 2.f;
                        }
                        obs = raw;
                    }
                    s_mirror_obs = obs;
                    // Bake the warp + doggy sprites into the texture (same
                    // helper the take/export path uses — no overlay), so the
                    // composited live frame already carries the filter.
                    draw_tex = face_filter_apply_obs(
                        br->face_filter, br->face_filter_amt, obs,
                        (float)ImGui::GetTime(), draw_tex,
                        MAX_VIDEO_TRACKS * 2, s_cam_w, s_cam_h);
                } else {
                    s_mirror_obs.valid = false;
                }
            } else {
                s_mirror_obs.valid = false;
            }
        }
    }
    s_mirror_dbg.valid      = true;
    s_mirror_dbg.cam_w      = s_cam_w; s_mirror_dbg.cam_h = s_cam_h;
    s_mirror_dbg.rot_deg    = br->rotation;
    s_mirror_dbg.face_valid = s_mirror_obs.valid;
    if (s_mirror_obs.valid)
        memcpy(s_mirror_dbg.pts, s_mirror_obs.pts, sizeof(s_mirror_dbg.pts));
    return draw_tex;
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

    // The playhead is clamped to the last playable frame upstream (app.cpp), so
    // it never parks at the exclusive project end — no render-time nudge needed.
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
    // to the crop window/handles (draw_crop_mode) — and on the selection's
    // transform handles (the rotate knob sits outside the clip bbox; picking
    // there deselected the clip before the rotate drag could begin).
    if (lclick && in_preview_area && s_ctx.handle == CanvasHandle::None &&
        state.crop_edit_track < 0 && !mouse_on_handle_spot(mpos)) {
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
    // FX animation time MUST be the timeline position, not wall-clock. Export and
    // scrub feed the (small, bounded) playhead into time-driven shaders; play used
    // to feed ImGui::GetTime() (seconds since app start — large and unbounded).
    // GPU sin/fract/hash lose precision at large arguments, so after the app had
    // been running a while those effects flattened out during playback only — they
    // were fine paused, scrubbing, and on export. Using the playhead makes preview
    // match export (WYSIWYG) and keeps the argument bounded.
    float t_anim    = state.playhead;

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
            std::string vsrc = clip_video_src(state, *cl);   // conformed copy if ready
            int slot = slot_for_video(const_cast<AppState&>(state),
                                      clip_slot_key(vsrc, cl->start), vsrc);
            if (slot < 0 || !video_is_open(slot)) return;
            if (already_queued(slot)) return;
            video_set_pixel_fx(slot, make_pfx(cl, ti));
            float src_t = clip_src_time(*cl, at_time);
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

        // Loop warm: when transport-looping and approaching the region end,
        // prefetch the clips that become active right after the wrap (at the
        // loop region start) so the loop seam hits a warm ring, not a sync
        // decode — clean cycle, no frame hitch. Slot dedupe makes this free when
        // the same source spans the wrap.
        if (state.loop_play && state.playing && state.duration > 0.f) {
            float wlo = 0.f, whi = state.duration;
            loop_region(state, wlo, whi);
            if (whi > wlo && state.playhead > whi - BOUNDARY_WARM_S) {
                constexpr int LOOP_WARM_FRAMES = 3;
                for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
                    auto& track = state.tracks[ti];
                    if (!track.visible) continue;
                    for (auto& cl : track.clips) {
                        if (!clip_is_videolike_type(cl.clip_type)) continue;
                        if (cl.start <= wlo + 1e-4f && cl.end > wlo + 1e-4f) {
                            add_clip(&cl, wlo, ti, LOOP_WARM_FRAMES);
                            break;
                        }
                    }
                }
            }
        }
        video_prefetch_frames(reqs.data(), (int)reqs.size());
    }

    // Live camera preview: resolve the texture and which track the camera brick
    // sits on, so it composites at THAT track's z-order inside the loop below
    // (tracks above it occlude it). It used to be drawn after the loop, which
    // pinned it on top of everything — the layering only started working once a
    // take was recorded and rendered as a normal video clip.
    const Clip* cam_br = nullptr; int cam_ti = -1, cam_cw = 0, cam_ch = 0;
    uintptr_t cam_ltex = 0;
    s_cam_box.active = false;
    if (vrecorder_monitor_get() || vrecorder_active())
        cam_ltex = camera_live_tex(state, cam_br, cam_ti, cam_cw, cam_ch);

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
                std::string vsrc = clip_video_src(state, *cl_ptr);  // conformed copy if ready
                int slot = slot_for_video(const_cast<AppState&>(state),
                               clip_slot_key(vsrc, cl_ptr->start), vsrc);
                float src_t = clip_src_time(*cl_ptr, at_time);

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
                    // Same source-time mapping as the frame fetch (×speed —
                    // this used to divide, desyncing masks on retimed clips).
                    float src_t = clip_src_time(*cl_ptr, at_time);
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
                                          cl_ptr->eval_prop("runtime_fx_amount", t_anim), t_anim);
                }

                // Face filter on the take (Pretty/Doggy…): cached landmark
                // pass per take, same helper as export — no divergence.
                if (cl_ptr && cl_ptr->face_filter != 0 && slot >= 0) {
                    VideoInfo vi_f = video_info(slot);
                    int fwd = (vi_f.width  > 0) ? vi_f.width  : (int)w;
                    int fhd = (vi_f.height > 0) ? vi_f.height : (int)h;
                    tex = face_filter_apply_take(*cl_ptr, (double)src_t,
                                                 tex, slot, fwd, fhd);
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
                // Camera-record takes render MIRRORED — same as the live
                // preview you recorded in (front-facing-cam convention).
                // Without this the take flips horizontally the instant
                // recording stops, which reads as the image jumping. Mirror =
                // swap the horizontal UV window.
                bool mirror = (cl_ptr->clip_type == ClipType::VideoRecord);
                if (editing_crop)
                    // Crop-edit shows the full frame, unrotated — the crop is
                    // defined in source space, so the editing view is source
                    // view (the overlay rect in draw_crop_mode matches this).
                    scene_add_layer(tex, cx, cy, hw, hh, 1.f, 0.f, alpha);
                else {
                    // UV window (crop), then mirror/flip by swapping the U and/or
                    // V extents — a real flip, independent of scale/position.
                    float u0 = cl_ptr->crop_l, u1 = 1.f - cl_ptr->crop_r;
                    float v0 = cl_ptr->crop_t, v1 = 1.f - cl_ptr->crop_b;
                    if (mirror)         { float t = u0; u0 = u1; u1 = t; }
                    if (cl_ptr->flip_h) { float t = u0; u0 = u1; u1 = t; }
                    if (cl_ptr->flip_v) { float t = v0; v0 = v1; v1 = t; }
                    scene_add_layer(tex, cx, cy, hw, hh, cos_r, sin_r, alpha, u0, v0, u1, v1);
                }
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

        // Live camera preview(s): composite the feed at EVERY camera brick on
        // this track that's active now (or the one being framed) — so two or more
        // camera bricks all show at once, each at its own transform/track z-order.
        // (Mirror-flipped for self-view; glass FX + face filter baked into cam_ltex
        // for the primary brick and shared across the rest.)
        if (cam_ltex && cam_cw > 0 && cam_ch > 0) {
            for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
                const Clip& cb = track.clips[(size_t)ci];
                if (cb.clip_type != ClipType::VideoRecord) continue;
                // Span-gated like any clip: a camera brick only previews while the
                // playhead is over it, so multiple bricks sequence on the timeline
                // (a brick that starts later must not show before its in-point).
                if (state.playhead < cb.start || state.playhead >= cb.end) continue;
                float px  = cb.eval_prop("pos_x",    state.playhead);
                float py  = cb.eval_prop("pos_y",    state.playhead);
                float sx  = cb.eval_prop("scale_x",  state.playhead);
                float sy  = cb.eval_prop("scale_y",  state.playhead);
                float rot = cb.eval_prop("rotation", state.playhead);
                float alpha = cb.eval_prop("opacity", state.playhead);
                float fit_w = w, fit_h = h;
                float va = (float)cam_cw / (float)cam_ch, ca = w / h;
                if (va > ca) { fit_w = w; fit_h = w / va; }
                else         { fit_h = h; fit_w = h * va; }
                float cx = px * w, cy = py * h;
                float hw = fit_w * sx * 0.5f, hh = fit_h * sy * 0.5f;
                float rad = rot * 3.14159265f / 180.f;
                float cs = cosf(rad), sn = sinf(rad);
                scene_add_layer(cam_ltex, cx, cy, hw, hh, cs, sn,
                                fmaxf(0.f, fminf(1.f, alpha)), 1.f, 0.f, 0.f, 1.f);
                // REC border + framing box: the brick being recorded wins; else
                // the first active brick this frame carries it.
                if (vrecorder_is_target(ti, ci) || !s_cam_box.active) {
                    auto rp = [&](float x, float y) {
                        return ImVec2{p.x + cx + x * cs - y * sn,
                                      p.y + cy + x * sn + y * cs};
                    };
                    s_cam_box.active    = true;
                    s_cam_box.recording = vrecorder_recording();
                    s_cam_box.warming   = vrecorder_warming();
                    s_cam_box.c[0] = rp(-hw, -hh); s_cam_box.c[1] = rp(hw, -hh);
                    s_cam_box.c[2] = rp(hw,  hh);  s_cam_box.c[3] = rp(-hw, hh);
                    s_mirror_dbg.cx = p.x + cx; s_mirror_dbg.cy = p.y + cy;
                    s_mirror_dbg.hw = hw;       s_mirror_dbg.hh = hh;
                    s_mirror_dbg.rot_deg = rot;
                }
            }
        }

        // Composite this track's text overlay at its z-order (interleaved with
        // video), so text is occluded by video on more-foreground tracks instead
        // of always drawing on top.
        scene_add_text_layer(state, state.playhead, ti, (int)w, (int)h);

        // Standalone (uncoupled) FX/MultiFX bricks on this track act as a video
        // group-bus: they process the composite of the tracks BELOW them (already
        // accumulated, since we composite bottom-to-top), gated by their span.
        {
            EffectAccum     ea  = collect_effects_for_track(state, state.playhead, ti);
            CreativeFXAccum cfx = collect_creative_fx_for_track(state, state.playhead, ti);
            if (ea.any_color || ea.any_blur || ea.any_vignette || ea.any_text ||
                cfx.any_cfx || cfx.any_gen_fx)
                scene_apply_fx((int)w, (int)h, ea, cfx, t_anim);
        }
    }  // end Pass 1 track loop

    // (Live camera preview now composites at its track's z-order inside the
    // Pass 1 loop above — it used to be drawn here, on top of every track.)

    // (Scene FX are now applied per-track inside Pass 1 — each standalone FX
    // brick processes the tracks below it, not the whole frame.)

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
            // Active text is composited into the scene at its track z-order in
            // Pass 1 (so video can occlude it). This pass rebuilds the
            // canvas-space TextLayout that click-to-select, the edit box and the
            // drag handles read from s_text_layouts — for BOTH:
            //   • the clip active at the playhead, so it stays clickable and
            //     draggable even though Pass 1 already drew it (no glyphs here);
            //   • the selected text clip when the playhead is not over it, drawn
            //     as a dim static preview so you can still see and edit it.
            // active_ci stays -1 in the layout below → resting position, no
            // intro animation, so the box lands where the text comes to rest.
            bool has_text_clips = false;
            int  active_text_ci = -1;
            for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
                auto ct = track.clips[ci].clip_type;
                if (ct != ClipType::Text && ct != ClipType::Lyrics && ct != ClipType::Subtitle)
                    continue;
                has_text_clips = true;   // counts toward the stacking offset
                if (active_text_ci < 0 &&
                    state.playhead >= track.clips[ci].start &&
                    state.playhead <  track.clips[ci].end)
                    active_text_ci = ci;
            }
            int sel_text_ci = -1;
            if (state.selected_track == ti && state.selected_clip >= 0 &&
                state.selected_clip < (int)track.clips.size()) {
                auto ct = track.clips[state.selected_clip].clip_type;
                if (ct == ClipType::Text || ct == ClipType::Lyrics || ct == ClipType::Subtitle)
                    sel_text_ci = state.selected_clip;
            }
            if (active_text_ci < 0 && sel_text_ci < 0) {
                if (has_text_clips) ++text_rendered;
                continue;
            }
            const int stack_idx = text_rendered;   // this track's stacking slot

            // 1–2 jobs: the active clip (hit box only) and/or the selected clip
            // (dim preview + box). When the selection IS the active clip, one job.
            struct TextJob { int ci; bool draw_glyphs; };
            TextJob jobs[2]; int njobs = 0;
            if (active_text_ci >= 0) jobs[njobs++] = { active_text_ci, false };
            if (sel_text_ci >= 0 && sel_text_ci != active_text_ci)
                jobs[njobs++] = { sel_text_ci, true };

            for (int j = 0; j < njobs; ++j) {
                const int   show_ci     = jobs[j].ci;
                const bool  draw_glyphs = jobs[j].draw_glyphs;
                const Clip* show        = &track.clips[show_ci];
                int active_ci = -1;

            // Typography face: the preset's bundled font (falls back to Inter
            // Black for "" / unknown). PushFont so layout + ImGui agree.
            ImGui::PushFont(typo_font_get(show->sub_font.c_str()));
            ImFont* txt_font = ImGui::GetFont();
            // Keyframable text transform — eval_prop(playhead) so an animated
            // font/wrap/position previews exactly as it exports (overlay_renderer
            // reads the same props via eval_prop). Static when un-keyed.
            float fs_kf  = show->eval_prop("font_size", state.playhead);
            float fsz    = fs_kf > 0.f ? fs_kf * h : h * 0.055f;
            float line_h = fsz * 1.25f;

            // Word-wrap: break text into lines that fit sub_wrap_w * canvas width
            float max_line_w = fmaxf(40.f, show->eval_prop("sub_wrap_w", state.playhead) * w);
            std::vector<std::string> txt_lines;
            // Render-time letter case (non-destructive) — matches overlay/export.
            std::string disp_text = show->text;
            if      (show->text_case == 1) for (auto& ch : disp_text) ch = (char)toupper((unsigned char)ch);
            else if (show->text_case == 2) for (auto& ch : disp_text) ch = (char)tolower((unsigned char)ch);
            {
                const char* src = disp_text.c_str();
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

            // Post-wrap font scale: keep a single over-long token (an unbroken URL,
            // say) from spilling past the wrap column. NO safe-margin penalty — the
            // text holds its size as you drag it anywhere on the canvas; it's free
            // to overflow the edges like any other object.
            {
                float max_fit_w = fmaxf(40.f, max_line_w);
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
                slot_y = p.y + sz_top + stack_idx * slot_h;
            else if (show->sub_pos == 3)
                slot_y = p.y + show->eval_prop("sub_pos_y", state.playhead) * h - block_h * 0.5f;
            else
                slot_y = p.y + h - sz_bot - block_h - stack_idx * slot_h;

            // Preset positions are penned into the safe zone so they never land
            // under platform UI chrome. Custom (dragged) text — sub_pos == 3 —
            // honours its exact position, past the margins and off-canvas, to
            // match the canvas drag's [-1, 2] range.
            if (show->sub_pos != 3)
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
            float anim_scale = 1.f;

            AnimStyle eff_style = (show->clip_style != AnimStyle::None)
                                  ? show->clip_style : state.style;

            // Per-element clips (anim_unit != 0) animate each word/letter inside
            // render_text_block — leave the block transform at identity here so
            // the motion isn't applied twice.
            if (active_ci >= 0 && show->anim_unit == 0) {
                BlockAnim ba = compute_block_anim(eff_style, local_t, clip_dur,
                                                  fade_in, fade_out, w, show->ease);
                anim_dx = ba.dx; anim_dy = ba.dy;
                anim_alpha = ba.alpha; anim_scale = ba.scale;
            }

            if (text_fx.any_text) {
                anim_alpha *= text_fx.opacity_mul;
                fsz        *= text_fx.scale_mul;
                line_h      = fsz * 1.25f;
                block_h     = txt_lines.size() * line_h;
            }
            if (anim_scale != 1.f) {                 // AnimStyle::Scale pop
                fsz    *= anim_scale;
                line_h  = fsz * 1.25f;
                block_h = txt_lines.size() * line_h;
            }

            float block_ax = p.x + show->eval_prop("sub_pos_x", state.playhead) * w;   // anchor point X (meaning depends on sub_anchor_h)
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

            if (draw_glyphs) {
                // Active text is already in the scene (Pass 1); only the selected
                // off-time preview draws here, dimmed so it reads as inactive.
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
                trc.rotation    = render_clip.eval_prop("rotation", state.playhead);
                trc.canvas_w    = w;
                trc.canvas_x0   = p.x;
                trc.canvas_h    = h;
                trc.canvas_y0   = p.y;
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
                tl.rot      = show->eval_prop("rotation", state.playhead);
                tl.valid    = true;
            }
            }   // for j — text layout jobs (active hit box + selected preview)
            if (has_text_clips) ++text_rendered;
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

    // Social safe-zone overlay (9:16) — the keep-out chrome of TikTok / Reels /
    // Shorts (top tabs, right action rail, bottom caption), so subjects clear the
    // UI and land where viewers look. Toggled from View ▸ Social safe zones;
    // only meaningful for vertical output. The drag centre-snap shifts to match.
    if (state.show_social_safe && state.format == OutputFormat::Vertical) {
        float lx = p.x + SOCIAL_SIDE_L * w;
        float rx = p.x + (1.f - SOCIAL_RAIL_R) * w;
        float ty = p.y + SOCIAL_TABS_T * h;
        float by = p.y + (1.f - SOCIAL_CAP_B) * h;
        // Faint wash over the four keep-out bands — tiled as canvas-minus-safebox
        // so the corners aren't double-darkened.
        ImU32 wash = IM_COL32(0, 0, 0, 64);
        dl->AddRectFilled({p.x, p.y}, {p.x + w, ty},     wash);  // top tabs
        dl->AddRectFilled({p.x, by},  {p.x + w, p.y + h}, wash);  // bottom caption
        dl->AddRectFilled({p.x, ty},  {lx,      by},      wash);  // left gutter
        dl->AddRectFilled({rx,  ty},  {p.x + w, by},      wash);  // right action rail
        // Safe box outline + corner ticks (same readable style as the lyrics guide).
        dl->AddRect({lx, ty}, {rx, by}, IM_COL32(255, 255, 255, 40), 0.f, 0, 1.f);
        float tk = 8.f; ImU32 tc = IM_COL32(255, 255, 255, 70);
        dl->AddLine({lx, ty}, {lx + tk, ty}, tc); dl->AddLine({lx, ty}, {lx, ty + tk}, tc);
        dl->AddLine({rx, ty}, {rx - tk, ty}, tc); dl->AddLine({rx, ty}, {rx, ty + tk}, tc);
        dl->AddLine({lx, by}, {lx + tk, by}, tc); dl->AddLine({lx, by}, {lx, by - tk}, tc);
        dl->AddLine({rx, by}, {rx - tk, by}, tc); dl->AddLine({rx, by}, {rx, by - tk}, tc);
        // Social centre mark — where the move-snap pulls to (shifted up/left).
        float ccx, ccy; canvas_center_target(state, ccx, ccy);
        float mx = p.x + ccx * w, my = p.y + ccy * h;
        ImU32 mkc = IM_COL32(120, 200, 255, 150);
        dl->AddLine({mx - 7.f, my}, {mx + 7.f, my}, mkc);
        dl->AddLine({mx, my - 7.f}, {mx, my + 7.f}, mkc);
    }

    // Safe zone guide — shown when a managed Lyrics track exists (suppressed when
    // the richer social overlay above is already on).
    // Represents the region guaranteed visible on TikTok/Reels/Shorts.
    {
        bool has_lyrics = false;
        for (auto& t : state.tracks)
            if (t.managed) { has_lyrics = true; break; }
        if (has_lyrics && !state.show_social_safe) {
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

    // Background-removal progress banner — so the human sees the AI working
    // (and that it IS working) even when the Clip panel isn't open.
    {
        const Clip* bgp = nullptr;
        for (auto& tr : state.tracks) {
            for (auto& c : tr.clips)
                if (clip_is_videolike_type(c.clip_type) &&
                    c.bg_remove_status == BgRemoveStatus::Processing &&
                    state.playhead >= c.start && state.playhead < c.end) { bgp = &c; break; }
            if (bgp) break;
        }
        if (bgp) {
            float prog = bgp->bg_remove_progress;
            char msg[64];
            if (prog < 0.f) snprintf(msg, sizeof(msg), "Downloading AI model…");
            else snprintf(msg, sizeof(msg), "Removing background…  %d%%",
                          (int)(fmaxf(0.f, fminf(1.f, prog)) * 100.f));
            ui_canvas_progress_banner(dl, p, w, h, msg, prog);
        }

        // Voice conversion on an audio clip — same banner so it reads consistently.
        const Clip* vcp = nullptr;
        for (auto& tr : state.tracks) {
            for (auto& c : tr.clips)
                if (c.vc_status == VcStatus::Processing &&
                    state.playhead >= c.start && state.playhead < c.end) { vcp = &c; break; }
            if (vcp) break;
        }
        if (vcp && !bgp) {
            char msg[48];
            snprintf(msg, sizeof(msg), "Converting voice…  %d%%",
                     (int)(fmaxf(0.f, fminf(1.f, vcp->vc_progress)) * 100.f));
            ui_canvas_progress_banner(dl, p, w, h, msg, vcp->vc_progress);
        }

        // Scene analysis (describe_video) — the agent's vision pass. Show what
        // it's chewing through so the long wait isn't a mystery box.
        {
            int vi = 0, vt = 0, fi = 0, ft = 0;
            if (scene_analysis_progress(&vi, &vt, &fi, &ft)) {
                char msg[80];
                if (vt > 1)
                    snprintf(msg, sizeof(msg), "Analyzing video %d/%d…  caption %d/%d",
                             vi, vt, fi + (ft > 0), ft);
                else
                    snprintf(msg, sizeof(msg), "Analyzing video…  caption %d/%d", fi + (ft > 0), ft);
                // Progress across the whole batch: completed videos + this video's frames.
                float per_vid = (ft > 0) ? (float)fi / (float)ft : 0.f;
                float prog = (vt > 0) ? ((float)(vi - 1) + per_vid) / (float)vt
                                      : (ft > 0 ? per_vid : -1.f);
                ui_canvas_progress_banner(dl, p, w, h, msg, prog);
            }
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

    // Snapshot button — top-right corner. Enabled for ANY visible content at the
    // playhead (not just video), so people making stills — text / backgrounds /
    // images / FX with no video clip — can export the frame as a full-res image.
    {
        bool has_content_at_play = false;
        for (auto& tr : state.tracks) {
            if (!tr.visible) continue;
            for (auto& cl : tr.clips) {
                if (cl.start > state.playhead || cl.end <= state.playhead) continue;
                ClipType t = cl.clip_type;
                if (clip_is_videolike_type(t) || t == ClipType::Background ||
                    t == ClipType::Text || t == ClipType::Lyrics ||
                    t == ClipType::Subtitle) { has_content_at_play = true; break; }
            }
            if (has_content_at_play) break;
        }

        const char* snap_lbl = state.snapshot_running ? "..." : "[o]";
        ImVec2 slsz = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.f, snap_lbl);
        float  btn_pad = 4.f;
        float  btn_w = slsz.x + btn_pad * 2.f;
        float  btn_h = slsz.y + btn_pad * 2.f;
        ImVec2 btn_tl = {p.x + w - btn_w - 6.f, p.y + 6.f};
        ImVec2 btn_br = {btn_tl.x + btn_w, btn_tl.y + btn_h};

        bool snap_hov = mpos.x >= btn_tl.x && mpos.x <= btn_br.x &&
                        mpos.y >= btn_tl.y && mpos.y <= btn_br.y;
        bool snap_ena = has_content_at_play && !state.snapshot_running;

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

    // ── Live camera frame outline ─────────────────────────────────────────────
    // The preview itself is composited in the scene (above); here we just
    // outline its frame and label it. While recording the outline is the
    // REC-orange border; otherwise a faint guide. The selection handles
    // (draw_canvas_handles, when the brick is selected) sit on top.
    if (s_cam_box.active) {
        ImU32 col = s_cam_box.recording ? IM_COL32(235, 90, 40, 255)
                                        : IM_COL32(160, 160, 180, 150);
        dl->AddQuad(s_cam_box.c[0], s_cam_box.c[1], s_cam_box.c[2], s_cam_box.c[3],
                    col, s_cam_box.recording ? 3.f : 1.5f);
        const char* tag = s_cam_box.recording ? "\xe2\x97\x8f REC"
                        : s_cam_box.warming    ? "starting\xe2\x80\xa6"
                                               : "camera";
        float lx = fminf(fminf(s_cam_box.c[0].x, s_cam_box.c[1].x),
                         fminf(s_cam_box.c[2].x, s_cam_box.c[3].x)) + 10.f;
        float ly = fminf(fminf(s_cam_box.c[0].y, s_cam_box.c[1].y),
                         fminf(s_cam_box.c[2].y, s_cam_box.c[3].y)) + 8.f;
        dl->AddText({lx, ly}, col, tag);
    }

    // ── Empty camera brick: "Turn on camera preview" call-to-action ───────────
    // A selected camera brick with no take and preview off leaves the canvas
    // blank — show its frame outline (where the shot will land) with a one-
    // click pill to start the live preview. The pill is a real ImGui button,
    // so the layer-pick/handle hit-testing stands down for it automatically.
    if (!s_cam_box.active && !vrecorder_monitor_get() && !vrecorder_active() &&
        state.selected_track >= 0 && state.selected_track < (int)state.tracks.size() &&
        state.selected_clip  >= 0) {
        auto& sclips = state.tracks[state.selected_track].clips;
        if (state.selected_clip < (int)sclips.size()) {
            Clip& br = sclips[(size_t)state.selected_clip];
            bool no_take = br.rec_take_sel < 0 || br.rec_takes.empty();
            if (br.clip_type == ClipType::VideoRecord && no_take) {
                float bx0, by0, bx1, by1;
                compute_video_bbox(state, br, p, w, h, bx0, by0, bx1, by1);
                float bcx = (bx0+bx1)*0.5f, bcy = (by0+by1)*0.5f;
                float lhw = (bx1-bx0)*0.5f, lhh = (by1-by0)*0.5f;
                float rad = br.eval_prop("rotation", state.playhead) * 3.14159265f / 180.f;
                float ax = cosf(rad), ay = sinf(rad);
                auto L = [&](float lx, float ly) {
                    return ImVec2{bcx + lx*ax - ly*ay, bcy + lx*ay + ly*ax};
                };
                ImVec2 q[4] = {L(-lhw,-lhh), L(lhw,-lhh), L(lhw,lhh), L(-lhw,lhh)};
                // Dashed frame outline (placeholder look).
                auto dashed = [&](ImVec2 a, ImVec2 b) {
                    float len = sqrtf((b.x-a.x)*(b.x-a.x) + (b.y-a.y)*(b.y-a.y));
                    if (len < 1.f) return;
                    float dx = (b.x-a.x)/len, dy = (b.y-a.y)/len;
                    for (float s = 0.f; s < len; s += 14.f) {
                        float e = fminf(s + 8.f, len);
                        dl->AddLine({a.x+dx*s, a.y+dy*s}, {a.x+dx*e, a.y+dy*e},
                                    IM_COL32(150, 160, 190, 130), 1.5f);
                    }
                };
                dashed(q[0], q[1]); dashed(q[1], q[2]);
                dashed(q[2], q[3]); dashed(q[3], q[0]);

                // Centered pill: camera glyph + label.
                const char* label = "Turn on camera preview";
                float fs = ImGui::GetFontSize();
                float tw = ImGui::CalcTextSize(label).x;
                float icon_w = fs * 1.2f, padx = 14.f, gap = 9.f;
                float pill_w = padx*2 + icon_w + gap + tw;
                float pill_h = fs + 16.f;
                ImVec2 pc = {bcx, bcy};
                ImVec2 p0 = {pc.x - pill_w*0.5f, pc.y - pill_h*0.5f};
                bool hov = ImGui::GetIO().MousePos.x >= p0.x &&
                           ImGui::GetIO().MousePos.x <= p0.x + pill_w &&
                           ImGui::GetIO().MousePos.y >= p0.y &&
                           ImGui::GetIO().MousePos.y <= p0.y + pill_h;
                dl->AddRectFilled(p0, {p0.x+pill_w, p0.y+pill_h},
                                  hov ? IM_COL32(60, 70, 96, 245)
                                      : IM_COL32(30, 34, 46, 230), pill_h*0.5f);
                dl->AddRect(p0, {p0.x+pill_w, p0.y+pill_h},
                            IM_COL32(120, 150, 230, hov ? 220 : 140), pill_h*0.5f, 0, 1.5f);
                // Mini camera icon: body + lens.
                float ix = p0.x + padx, iy = pc.y;
                float bw = icon_w, bh = icon_w * 0.72f;
                dl->AddRectFilled({ix, iy - bh*0.5f}, {ix + bw, iy + bh*0.5f},
                                  IM_COL32(225, 232, 245, 255), 2.5f);
                dl->AddRectFilled({ix + bw*0.18f, iy - bh*0.5f - bh*0.22f},
                                  {ix + bw*0.5f, iy - bh*0.5f + 1.f},
                                  IM_COL32(225, 232, 245, 255), 1.5f);  // viewfinder bump
                dl->AddCircleFilled({ix + bw*0.5f, iy}, bh*0.27f, IM_COL32(40, 44, 58, 255));
                dl->AddText({ix + icon_w + gap, pc.y - fs*0.5f},
                            IM_COL32(232, 238, 248, 255), label);
                // Hit area (claims the mouse so handles/layer-pick stand down).
                ImGui::SetCursorScreenPos(p0);
                ImGui::InvisibleButton("##cam_preview_cta", {pill_w, pill_h});
                if (ImGui::IsItemClicked()) vrecorder_monitor_set(true);
            }
        }
    }
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
