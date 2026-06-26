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
#include "face_cache.h"
#include "../recorder.h"
#include "../video_recorder.h"
#include "../video.h"
#include "bg_presets.h"
#include "text_styles.h"
#include "theme.h"
#include "body_fx.h"
#include "bg_remove.h"
#include "runtime_fx.h"
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

// How long a drag must dwell on a merge target before it welds — shared by
// keyframe super diamonds, FX brick merging, and content coupling so the
// gesture feels the same everywhere.
static const double kWeldHoldSec = 1.0;

// In-progress keyframe-diamond retime drag. Grabbing a super diamond drags
// every key at that timestamp (one entry per member prop); member indices are
// kept current as keys bubble through their sorted tracks.
static struct {
    bool active = false;
    int  ti = -1, ci = -1;
    struct Mem { std::string prop; int idx; };
    std::vector<Mem> mems;
    bool moved = false;          // only push undo history if it actually moved
    // Hold-to-weld: parking the drag beside another diamond arms a 3 s timer;
    // only when it completes do the keys snap together into a super diamond.
    float  weld_target = -1.f;   // group time we're parked beside (-1 = none)
    double weld_start  = 0.0;    // ImGui::GetTime() when the timer was armed
} s_kf_drag;

// Brief expanding-ring flash when a weld completes.
static struct {
    bool   active = false;
    double t0     = 0.0;
    int    ti = -1, ci = -1;
    float  time   = 0.f;         // clip-relative time of the new super diamond
} s_kf_flash;

// Super-diamond right-click context (unpair menu).
static struct { int ti = -1, ci = -1; float time = 0.f; } s_kf_ctx;

// Hold-to-weld for FX brick merging — the keyframe-diamond gesture applied to
// bricks: overlapping a merge target arms the dwell timer; the merge only
// fires when it completes (or instantly on an Alt drop). An early release
// just leaves the bricks overlapping, which is legal for FX (both render).
static struct {
    int    target_ti = -1;   // current merge candidate (mirrors drag_merge_*)
    int    target_ci = -1;
    double t0        = 0.0;  // when the timer was armed
} s_fx_weld;

// Expanding-ring flash over a freshly merged FX brick.
static struct {
    bool   active = false;
    double t0     = 0.0;
    int    ti = -1, ci = -1;
} s_fx_flash;

// Pending content-coupling: an uncoupled video FX brick sitting on content
// arms this timer (ring drawn on the brick); after kWeldHoldSec it couples —
// converting to a Video Multi-FX chain or merging into the host's existing
// one. Passive scan, so every entry path (drag drop, toolbox drop, IPC,
// decoupled brick dragged back on) behaves identically; moving the brick
// re-arms, dragging it pauses.
static struct {
    int    ti = -1, ci = -1;  // the brick
    int    host_ci = -1;      // content clip beneath it
    double t0      = 0.0;
    int    hist_len = -1;     // history size at arm — amend only if unchanged
} s_couple_pend;

// The weld timer ring, filling clockwise at the merge target's center —
// same visual language as the keyframe-diamond weld ring.
static void draw_fx_weld_ring(ImDrawList* dl, float x0, float x1,
                              float y0, float y1, ImU32 col) {
    float prog = (float)((ImGui::GetTime() - s_fx_weld.t0) / kWeldHoldSec);
    if (prog <= 0.f || prog >= 1.f) return;
    ImVec2 c{(x0 + x1) * 0.5f, (y0 + y1) * 0.5f};
    float pr = 10.f * (1.f + 0.1f * sinf((float)ImGui::GetTime() * 9.f));
    dl->PathArcTo(c, pr, -IM_PI * 0.5f, -IM_PI * 0.5f + prog * 2.f * IM_PI, 24);
    dl->PathStroke(col, 0, 2.f);
}

// Cute "snap-together" celebration when an FX brick couples: a warm glow bloom
// behind the brick, a couple of expanding rings for the pop, and a little burst
// of twinkling sparkles radiating from the center. Brief and bouncy.
static void draw_fx_merge_flash(ImDrawList* dl, int ti, int ci,
                                float x0, float x1, float y0, float y1) {
    if (!s_fx_flash.active || s_fx_flash.ti != ti || s_fx_flash.ci != ci) return;
    const float DUR = 0.6f;
    float el = (float)(ImGui::GetTime() - s_fx_flash.t0);
    if (el > DUR) { s_fx_flash.active = false; return; }
    float k    = el / DUR;                                   // 0 → 1
    float ease = 1.f - (1.f - k) * (1.f - k) * (1.f - k);    // ease-out cubic
    float fade = 1.f - k;                                    // linear fade-out
    float bump = sinf(k * IM_PI);                            // 0 → 1 → 0 swell
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    float now = (float)ImGui::GetTime();

    // Soft glow bloom — layered rounded rects that breathe out and fade.
    for (int g = 0; g < 3; ++g) {
        float gx = 5.f + g * 7.f + ease * 9.f;
        int   ga = (int)(80.f * fade * bump / (g + 1));
        dl->AddRectFilled({x0 - gx, y0 - gx}, {x1 + gx, y1 + gx},
                          IM_COL32(150, 220, 255, ga), 9.f + g * 3.f);
    }

    // Two expanding rings (staggered) for the snap "pop".
    for (int r = 0; r < 2; ++r) {
        float rk = k - r * 0.12f;
        if (rk <= 0.f) continue;
        float re = 1.f - (1.f - rk) * (1.f - rk);
        float ex = 5.f + re * 18.f;
        dl->AddRect({x0 - ex, y0 - ex}, {x1 + ex, y1 + ex},
                    IM_COL32(185, 238, 255, (int)(210.f * (1.f - rk))),
                    4.f + ex * 0.2f, 0, 2.f);
    }

    // Twinkling sparkles radiating from the center — tiny 4-point stars that
    // shoot out, swell mid-flight, then wink out. Confetti vibe.
    const int N = 7;
    float dist = 9.f + ease * 24.f;
    for (int s = 0; s < N; ++s) {
        float ang = (float)s / N * 2.f * IM_PI + s_fx_flash.t0 * 1.3f;
        float sx  = cx + cosf(ang) * dist * 1.7f;
        float sy  = cy + sinf(ang) * dist;
        float sz  = 1.5f + 1.9f * bump;
        int   a   = (int)(255.f * fade);
        ImU32 col = (s % 2) ? IM_COL32(255, 240, 170, a)    // warm gold
                            : IM_COL32(215, 246, 255, a);   // icy white
        float tw  = 0.6f + 0.4f * sinf(now * 18.f + s);     // little twinkle
        dl->AddLine({sx - sz * tw, sy}, {sx + sz * tw, sy}, col, 1.4f);
        dl->AddLine({sx, sy - sz * tw}, {sx, sy + sz * tw}, col, 1.4f);
    }
}

// Drop state — declared extern in timeline.h
int   s_tl_hover_track   = -1;
float s_drop_flash_t     = 0.f;
int   s_drop_flash_track = -1;

static bool is_fx_clip(const Clip& c) {
    return c.clip_type == ClipType::Effect ||
           c.clip_type == ClipType::MultiFX ||
           c.clip_type == ClipType::BodyFX ||
           c.clip_type == ClipType::AudioMultiFX;
}
// Audio FX bricks weld with each other into an Audio Multi-FX chain; video
// bricks weld into a Video Multi-FX chain. Never across the divide.
static bool fx_bricks_weldable(const Clip& a, const Clip& b) {
    return fx_brick_is_audio_kind(a) == fx_brick_is_audio_kind(b);
}
static bool clips_conflict(const Clip& a, const Clip& b) {
    // FX bricks never conflict with content clips (glass bricks ride over
    // video on the same track). Among FX bricks, only SAME-kind ones conflict:
    // two audio (or two video) bricks must weld into a chain or bounce back,
    // never stack. A video chain and an audio chain are different kinds that
    // coexist as separate glass bricks on the same host — so they don't
    // conflict, and a solo video FX brick can be dragged onto content that
    // already carries an audio chain (and vice-versa).
    if (is_fx_clip(a) != is_fx_clip(b)) return false;
    if (fx_brick_is_audio_kind(a) != fx_brick_is_audio_kind(b)) return false;
    return a.start < b.end && a.end > b.start;
}
static bool is_chain_brick(const Clip& c) {
    return c.clip_type == ClipType::MultiFX ||
           c.clip_type == ClipType::AudioMultiFX;
}
static void merge_fx_clips(Clip& target, Clip dragged) {
    if (is_chain_brick(target)) {
        if (is_chain_brick(dragged)) {
            for (auto& se : dragged.fx_chain)
                target.fx_chain.push_back(se);
        } else {
            target.fx_chain.push_back(dragged);
        }
    } else {
        // target is a single brick — promote to the matching chain type
        Clip sub0 = target;
        sub0.clip_type = (target.clip_type == ClipType::BodyFX) ? ClipType::BodyFX : ClipType::Effect;
        sub0.rel_start = 0.f; sub0.rel_end = 0.f;
        target.clip_type = fx_brick_is_audio_kind(target) ? ClipType::AudioMultiFX
                                                          : ClipType::MultiFX;
        target.fx_chain.clear();
        target.fx_chain.push_back(sub0);
        if (is_chain_brick(dragged)) {
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

// Couple the brick at (ti, ci) to the content clip at host_ci: chain entries
// are re-windowed against the host span so effective timing survives the
// snap, singles convert to a Video Multi-FX, and a host that already owns a
// coupled chain absorbs the new entries instead of growing a second brick.
// Returns the index of the resulting coupled brick.
int timeline_couple_fx_brick(AppState& state, int ti, int ci, int host_ci) {
    auto& clips = state.tracks[ti].clips;
    const Clip host = clips[(size_t)host_ci];   // copy: erase below shifts refs
    const float h0 = host.start, hlen = host.end - host.start;
    Clip brick = clips[(size_t)ci];

    auto window_entry = [&](Clip& se, float b0, float b1) {
        float abs0 = (se.rel_end <= 0.f) ? b0 : b0 + se.rel_start;
        float abs1 = (se.rel_end <= 0.f) ? b1 : b0 + se.rel_end;
        se.rel_start = fmaxf(0.f, abs0 - h0);
        se.rel_end   = fmaxf(se.rel_start, fminf(abs1 - h0, hlen));
        if (se.rel_start <= 0.001f && se.rel_end >= hlen - 0.001f) {
            se.rel_start = 0.f;   // full-host window = "always on"
            se.rel_end   = 0.f;
        }
        se.fx_coupled = false;
        se.fx_host_sid.clear();
    };

    std::vector<Clip> entries;
    if (is_chain_brick(brick)) entries = brick.fx_chain;
    else {
        Clip se = brick;
        se.fx_chain.clear();
        se.rel_start = se.rel_end = 0.f;
        entries.push_back(se);
    }
    for (auto& se : entries) window_entry(se, brick.start, brick.end);

    const ClipType chain_type = fx_brick_is_audio_kind(clips[(size_t)ci])
                              ? ClipType::AudioMultiFX : ClipType::MultiFX;

    // Coupling an audio FX brick onto a record brick turns "Hear effects" on, so
    // you immediately monitor through it (and can dial in its dry/wet).
    if (chain_type == ClipType::AudioMultiFX &&
        (host.clip_type == ClipType::Record ||
         host.clip_type == ClipType::VideoRecord))
        audio_monitor_fx_set(true);

    // Host already has a coupled chain of this kind? Merge into it.
    for (int k = 0; k < (int)clips.size(); ++k) {
        if (k == ci) continue;
        Clip& oc = clips[(size_t)k];
        if (oc.clip_type == chain_type && oc.fx_coupled &&
            fx_coupled_host(state, ti, oc) == host_ci) {
            for (auto& se : entries) oc.fx_chain.push_back(se);
            if (oc.fx_chain_selected < 0) oc.fx_chain_selected = 0;
            clips.erase(clips.begin() + ci);
            return k > ci ? k - 1 : k;
        }
    }

    Clip& b = clips[(size_t)ci];
    b.clip_type         = chain_type;
    b.fx_chain          = std::move(entries);
    b.fx_chain_selected = b.fx_chain.empty() ? -1 : 0;
    b.fx_coupled        = true;
    b.fx_host_sid       = fx_host_fingerprint(host);
    b.start = host.start;
    b.end   = host.end;
    return ci;
}

// Passive coupling scan: any uncoupled video FX brick resting on content
// (not mid-drag) arms the 1.5 s ring; completion couples it. One pending
// brick at a time. Runs every frame, so toolbox drops, drag drops, IPC
// bricks and re-dropped decoupled bricks all behave identically.
// Immediately weld a just-dropped FX brick onto the best-overlap content on
// its track — no 1.5s dwell. A panel drop ONTO content is a deliberate
// placement, so there's nothing to reconsider (the dwell timer exists for
// repositioning EXISTING bricks). Returns the resulting brick index; pushes
// no history, so the caller's drop entry captures the welded result as one
// undo step.
static int couple_fx_now(AppState& state, int ti, int ci) {
    if (ti < 0 || ti >= (int)state.tracks.size()) return ci;
    auto& cls = state.tracks[ti].clips;
    if (ci < 0 || ci >= (int)cls.size()) return ci;
    Clip& c = cls[(size_t)ci];
    if (c.fx_coupled || !is_fx_clip(c)) return ci;
    bool audio_kind = fx_brick_is_audio_kind(c);
    if (!audio_kind && !fx_brick_is_video(c)) return ci;
    int best = -1; float bov = 0.05f;
    for (int k = 0; k < (int)cls.size(); ++k) {
        if (k == ci) continue;
        const Clip& hc = cls[(size_t)k];
        bool hostable = audio_kind
            ? (hc.clip_type == ClipType::Audio || hc.clip_type == ClipType::Record ||
               hc.clip_type == ClipType::Video || hc.clip_type == ClipType::VideoRecord)
            : (clip_is_videolike_type(hc.clip_type) || hc.clip_type == ClipType::Background);
        if (!hostable) continue;
        float ov = fminf(c.end, hc.end) - fmaxf(c.start, hc.start);
        if (ov > bov) { bov = ov; best = k; }
    }
    if (best < 0) return ci;
    int nci = timeline_couple_fx_brick(state, ti, ci, best);
    s_fx_flash.active = true; s_fx_flash.t0 = ImGui::GetTime();
    s_fx_flash.ti = ti; s_fx_flash.ci = nci;
    return nci;
}

// The FX chain rendered on a coupled glass brick, morphing with zoom:
//   narrow brick → a frosted deck of cards (depth = effect count);
//   wide brick   → per-FX timing lanes (lane length = that effect's span).
// The SAME panes interpolate between the two by a `spread` factor, so the
// transition is continuous as you zoom. Each pane is tinted by its FX type.
// Spread (0=deck/narrow, 1=full timing lanes/wide) and visible-pane count for
// the FX stack. Shared by the renderer and the lane hit-test so the editable
// rects always match what's drawn.
static void fx_stack_layout(const Clip& clip, float vis_x0, float vis_x1,
                            int& shown, float& spread) {
    int N = (int)clip.fx_chain.size();
    shown = N < 5 ? N : 5;
    (void)vis_x0; (void)vis_x1;
    // FX chips are a constant-size deck — their size must NOT scale with the
    // brick's span/width (resizing a welded brick used to stretch them via the
    // deck→timing-lane morph). Timing editing still works: the lane-drag maps
    // the cursor to time through `zoom`, independent of the chip rect.
    spread = 0.f;
}

// Morphed (deck↔lane) screen rect for sub-effect slot i, plus the rel-space
// span it represents. fx_lane_drag editing and the draw path both consume this.
struct FxLaneRect { float x0, y0, x1, y1; float rel_s, rel_e; };
static FxLaneRect fx_lane_rect(const Clip& clip, int i, int shown, float spread,
                               float cx0, float vis_x0, float vis_x1,
                               float cy0, float cy1, float zoom) {
    const float brick_h = cy1 - cy0;
    const float lane_h  = (brick_h - 4.f) / (float)shown;
    const float dur     = clip.end - clip.start;
    const Clip& se = clip.fx_chain[(size_t)i];
    float rel_s = se.rel_start;
    float rel_e = (se.rel_end > 0.f) ? se.rel_end : dur;
    float d_x0 = vis_x0 + 5.f + i*3.f, d_x1 = d_x0 + 17.f;
    float d_y0 = cy0 + 2.f,            d_y1 = cy1 - 2.f;
    float l_x0 = fmaxf(vis_x0, cx0 + rel_s*zoom);
    float l_x1 = fminf(vis_x1, cx0 + rel_e*zoom);
    if (l_x1 < l_x0 + 3.f) l_x1 = l_x0 + 3.f;
    float l_y0 = cy0 + 2.f + i*lane_h, l_y1 = l_y0 + lane_h - 1.f;
    FxLaneRect r;
    r.x0 = d_x0 + (l_x0-d_x0)*spread; r.x1 = d_x1 + (l_x1-d_x1)*spread;
    r.y0 = d_y0 + (l_y0-d_y0)*spread; r.y1 = d_y1 + (l_y1-d_y1)*spread;
    r.rel_s = rel_s; r.rel_e = rel_e;
    return r;
}

static void draw_fx_stack(ImDrawList* dl, const Clip& clip, float cx0,
                          float vis_x0, float vis_x1, float cy0, float cy1,
                          float zoom, bool sel) {
    int N = (int)clip.fx_chain.size();
    if (N <= 0) {
        dl->AddText({vis_x0 + 5.f, cy0 + (cy1-cy0-13.f)*0.5f},
                    IM_COL32(200,240,255,255), "FX");
        return;
    }
    const float brick_h = cy1 - cy0;
    int shown; float spread;
    fx_stack_layout(clip, vis_x0, vis_x1, shown, spread);
    for (int i = 0; i < shown; ++i) {
        const Clip& se = clip.fx_chain[(size_t)i];
        // A Body FX sub-effect names itself by its body_fx_type — its plain
        // fx_type is 0 (Grade), which is why every Body FX used to read "Grade".
        const char* se_name = fx_type_name(se.fx_type);
        if (se.clip_type == ClipType::BodyFX) {
            const BodyFXInfo* bi = body_fx_find_info(se.body_fx_type);
            if (bi) se_name = bi->name;
        }
        FxBrickColors c = fx_brick_colors(se.fx_type, sel);
        ImU32 tint = IM_COL32((int)((c.fill>>IM_COL32_R_SHIFT)&0xFF),
                              (int)((c.fill>>IM_COL32_G_SHIFT)&0xFF),
                              (int)((c.fill>>IM_COL32_B_SHIFT)&0xFF),
                              sel ? 235 : 200);
        FxLaneRect r = fx_lane_rect(clip, i, shown, spread, cx0,
                                    vis_x0, vis_x1, cy0, cy1, zoom);
        dl->AddRectFilled({r.x0,r.y0}, {r.x1,r.y1}, tint, 2.f);
        dl->AddRect({r.x0,r.y0}, {r.x1,r.y1}, IM_COL32(255,255,255, sel?170:90), 2.f, 0, 1.f);
        // Effect name once the lane has room.
        if (spread > 0.45f && (r.x1-r.x0) > 34.f)
            dl->AddText({r.x0+3.f, (r.y0+r.y1)*0.5f - 6.5f},
                        IM_COL32(255,255,255,235), se_name);
    }
    // Count badge in deck mode (and an overflow tail when capped).
    if (spread < 0.5f) {
        char b[12]; snprintf(b, sizeof(b), "\xc3\x97%d", N);
        dl->AddText({vis_x0 + 5.f + shown*3.f + 21.f, cy0 + brick_h*0.5f - 6.5f},
                    IM_COL32(220,240,255,255), b);
    } else if (N > shown) {
        char b[12]; snprintf(b, sizeof(b), "+%d", N - shown);
        dl->AddText({vis_x1 - 22.f, cy1 - 14.f}, IM_COL32(220,240,255,220), b);
    }
}

// Remove a content clip at (ti, ci) PLUS every glass brick coupled to it from
// track ti, returned as a group with the host first. A coupled brick must live
// on its host's track (fx_coupled_host only searches that track), so a
// cross-track move has to carry the welded bricks along — otherwise the
// content slides out from under its glass and the bricks orphan.
static std::vector<Clip> extract_host_group(AppState& state, int ti, int ci) {
    auto& cls = state.tracks[(size_t)ti].clips;
    std::vector<int> idx{ci};
    for (int k = 0; k < (int)cls.size(); ++k) {
        if (k == ci) continue;
        if (cls[(size_t)k].fx_coupled && is_fx_clip(cls[(size_t)k]) &&
            fx_coupled_host(state, ti, cls[(size_t)k]) == ci)
            idx.push_back(k);
    }
    std::sort(idx.begin(), idx.end());
    std::vector<Clip> grp;
    grp.push_back(cls[(size_t)ci]);                 // host first
    for (int k : idx) if (k != ci) grp.push_back(cls[(size_t)k]);
    std::sort(idx.rbegin(), idx.rend());            // erase high→low
    for (int k : idx) cls.erase(cls.begin() + k);
    return grp;
}

// Clip indices on track ti of every FX brick welded to the content clip host_ci.
static std::vector<int> coupled_fx_of(AppState& state, int ti, int host_ci) {
    std::vector<int> out;
    auto& cls = state.tracks[(size_t)ti].clips;
    for (int k = 0; k < (int)cls.size(); ++k)
        if (cls[(size_t)k].fx_coupled && is_fx_clip(cls[(size_t)k]) &&
            fx_coupled_host(state, ti, cls[(size_t)k]) == host_ci)
            out.push_back(k);
    return out;
}

// Deferred FX-brick removal requested from the clip context menu — applied after
// the popup closes so we never erase clips while cc/ci are still live in the menu.
static int              s_ctx_fx_del_ti = -1;
static std::vector<int> s_ctx_fx_del_cis;

// Lyric brick body-drag — managed lyrics track only. The brick slides within
// its own track, clamped to its neighbors (no overlap, can't leave the track).
// Kept separate from the video drag machinery: no cross-track, no JSON. The new
// position is saved with the project; an explicit preset/grouping change re-lays
// from the transcript and supersedes the manual nudge.
static int   s_lyric_dt = -1, s_lyric_dc = -1;
static float s_lyric_off = 0.f, s_lyric_o_start = 0.f, s_lyric_o_end = 0.f;

static void couple_pending_tick(AppState& state) {
    int pti = -1, pci = -1, phost = -1;
    for (int ti = 0; ti < (int)state.tracks.size() && pti < 0; ++ti) {
        auto& cls = state.tracks[ti].clips;
        for (int ci = 0; ci < (int)cls.size(); ++ci) {
            Clip& c = cls[(size_t)ci];
            if (!is_fx_clip(c) || c.fx_coupled) continue;
            bool audio_kind = fx_brick_is_audio_kind(c);
            if (!audio_kind && !fx_brick_is_video(c)) continue;
            if (g_tl.drag_track == ti && g_tl.drag_clip == ci) continue;
            int best = -1; float bov = 0.05f;   // ≥50 ms overlap to arm
            for (int k = 0; k < (int)cls.size(); ++k) {
                const Clip& hc = cls[(size_t)k];
                bool hostable = audio_kind
                    ? (hc.clip_type == ClipType::Audio ||
                       hc.clip_type == ClipType::Record ||
                       hc.clip_type == ClipType::Video ||
                       hc.clip_type == ClipType::VideoRecord)
                    : (clip_is_videolike_type(hc.clip_type) ||
                       hc.clip_type == ClipType::Background);
                if (!hostable) continue;
                float ov = fminf(c.end, hc.end) - fmaxf(c.start, hc.start);
                if (ov > bov) { bov = ov; best = k; }
            }
            if (best >= 0) { pti = ti; pci = ci; phost = best; break; }
        }
    }
    if (pti != s_couple_pend.ti || pci != s_couple_pend.ci ||
        phost != s_couple_pend.host_ci) {
        s_couple_pend.ti = pti; s_couple_pend.ci = pci;
        s_couple_pend.host_ci = phost;
        s_couple_pend.t0 = ImGui::GetTime();
        s_couple_pend.hist_len = (int)history_entries().size();
        return;
    }
    if (pti < 0) return;
    if (ImGui::GetTime() - s_couple_pend.t0 < kWeldHoldSec) return;
    int nci = timeline_couple_fx_brick(state, pti, pci, phost);
    state.selected_track = pti;
    state.selected_clip  = nci;
    state.clip_selection.clear();
    state.clip_selection.insert({pti, nci});
    s_fx_flash.active = true;
    s_fx_flash.t0     = ImGui::GetTime();
    s_fx_flash.ti     = pti;
    s_fx_flash.ci     = nci;
    // Collapse the drop + couple into ONE undo step: undoing a weld must
    // bounce the brick back to where it was before the gesture, not leave
    // an uncoupled brick stranded on the content (which would instantly
    // re-arm the timer). Only safe when nothing else hit history since
    // the ring armed.
    if (s_couple_pend.hist_len == (int)history_entries().size())
        history_amend(state, "Couple FX brick");
    else
        history_push(state, "Couple FX brick");
    s_couple_pend.ti = s_couple_pend.ci = s_couple_pend.host_ci = -1;
}

// ── Timeline ──────────────────────────────────────────────────────────────────

static TLGeom s_tl_geom;
const TLGeom& tl_geom() { return s_tl_geom; }

void draw_timeline(AppState& state, ImVec2 origin, float total_w, float total_h) {
    s_tl_geom.clips.clear();
    s_tl_geom.lanes.clear();
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

    // Minimum zoom that fits the entire timeline in the visible clip area,
    // minus a small right margin so clip edges (and their trim handles) never
    // sit flush against the vertical scrollbar at full zoom-out.
    float fit_margin = fminf(fmaxf(clip_area_w * 0.05f, 24.f), 64.f);
    float zoom_min = fmaxf(1.f, (clip_area_w - fit_margin) / dur);
    state.tl_zoom_min = zoom_min;
    // Lift zoom when the floor rises (window resized, duration shrank) — but
    // never mid-gesture: trimming the last clip changes the duration every
    // frame, and chasing the moving floor live rescales the whole timeline
    // under the drag. Freeze while a clip drag is in flight, glide after.
    if (g_tl.drag_track < 0 && zoom < zoom_min) {
        float a = fminf(1.f, ImGui::GetIO().DeltaTime * 12.f);
        zoom += (zoom_min - zoom) * a;
        if (zoom_min - zoom < 0.01f) zoom = zoom_min;
    }
    // Glide scroll back into legal bounds once hands are off — trims may
    // overscroll past the content end freely while dragging. The bound keeps
    // the fit margin of empty timeline after the last clip (a trim-follow
    // release usually lands exactly on it, so nothing visibly moves). At the
    // zoom floor this converges to 0 (fully zoomed out shows frame 0).
    if (g_tl.drag_track < 0) {
        float smax = fmaxf(0.f, dur * zoom - clip_area_w + fit_margin);
        if (scroll > smax) {
            float a = fminf(1.f, ImGui::GetIO().DeltaTime * 12.f);
            scroll += (smax - scroll) * a;
            if (scroll - smax < 0.5f) scroll = smax;
        }
    }

    // Deferred zoom-to-fit: set by add_clip_to_track / import whenever a new clip is added.
    // Always compute the target zoom for the clip; only apply it if it means zooming OUT
    // (new_zoom < current zoom). This means: if the user is already zoomed out enough to
    // see the full clip, nothing changes; if not, the timeline adjusts to show it with spacing.
    if (state.tl_zoom_to_fit_end > 0.f && clip_area_w > 0.f) {
        float target   = state.tl_zoom_to_fit_end * 1.15f;
        // Floor against the duration AFTER the add lands, not this frame's
        // zoom_min: the request fires the same frame as the click, before the
        // duration sync sees the new clip, and the stale (shorter-project)
        // floor used to swallow the whole zoom-out.
        float fit_dur   = fmaxf(dur, state.tl_zoom_to_fit_end);
        float floor_fit = fmaxf(1.f, (clip_area_w - fit_margin) / fit_dur);
        float new_zoom  = fmaxf(floor_fit, fminf(clip_area_w / target, 4000.f));
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
                scroll = fminf(scroll, fmaxf(0.f, dur * zoom - clip_area_w + fit_margin));
            } else if (!in_track_body) {
                // Plain scroll in ruler/header = horizontal pan only
                scroll = fmaxf(0.f, scroll - wheel * 60.f);
                scroll = fminf(scroll, fmaxf(0.f, tl_content_w - clip_area_w + fit_margin));
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

    // Selected-take duration of a record brick, cached per path (a probe per
    // frame would hammer file IO). Returns 0 when there is no take.
    auto take_duration = [](const Clip& cl) -> float {
        if (cl.rec_take_sel < 0 || cl.rec_take_sel >= (int)cl.rec_takes.size())
            return 0.f;
        static std::unordered_map<std::string, float> cache;
        const std::string& path = cl.rec_takes[(size_t)cl.rec_take_sel];
        auto it = cache.find(path);
        if (it != cache.end()) return it->second;
        float d = (cl.clip_type == ClipType::VideoRecord)
                  ? video_probe_duration(path)
                  : recorder_wav_duration(path);
        cache[path] = d;
        return d;
    };

    // Build candidate list (all clip edges + playhead), excluding one clip.
    // Record-brick take ends are candidates too: resizing a brick (or laying
    // a clip against it) snaps to where the selected take's content ends.
    auto build_snap_candidates = [&](int ex_ti, int ex_ci) -> std::vector<float> {
        std::vector<float> cands;
        cands.push_back(state.playhead);
        cands.push_back(0.f);
        for (int ti = 0; ti < (int)state.tracks.size(); ++ti)
            for (int ci = 0; ci < (int)state.tracks[ti].clips.size(); ++ci) {
                const Clip& cl = state.tracks[ti].clips[ci];
                if (!(ti == ex_ti && ci == ex_ci)) {
                    cands.push_back(cl.start);
                    cands.push_back(cl.end);
                }
                if (cl.clip_type == ClipType::Record ||
                    cl.clip_type == ClipType::VideoRecord) {
                    float td = take_duration(cl);
                    if (td > 0.05f) cands.push_back(cl.start + td);
                }
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
    // Top band of the ruler reserved for the loop brace (grab/resize/move). The
    // seek-on-click zone and tick labels start below it so they don't fight for
    // the mouse / pixels.
    const float LOOP_STRIP_H = 12.f;
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

    // Tick ladder derived from the project frame grid: every tick at every
    // zoom sits on a frame boundary, and majors are always labeled as time —
    // never raw frame counts. Seconds stay the unit; zoomed in, each second
    // subdivides by a divisor of fps so minors land exactly on frames.
    const float f1 = 1.f / fps;
    const float MIN_MAJOR_PX = 80.f;
    const float MIN_MINOR_PX = 7.f;
    const int   ifps = (int)fmaxf(1.f, roundf(fps));

    float major_secs;        // labeled tick interval (always whole-second based)
    int   minors_per_major;  // minor ticks inside one major
    bool  frame_grid = false;
    int   frame_step = ifps; // frames between minors when frame_grid
    if (zoom >= MIN_MAJOR_PX) {
        // 1 s majors fit — subdivide the second on the frame grid with the
        // smallest fps divisor that keeps minors ≥ MIN_MINOR_PX apart.
        major_secs = 1.f;
        for (int s = 1; s <= ifps; ++s) {
            if (ifps % s) continue;
            if ((float)s * zoom / (float)ifps >= MIN_MINOR_PX) { frame_step = s; break; }
        }
        frame_grid       = frame_step < ifps;
        minors_per_major = ifps / frame_step;
    } else {
        // Whole-second ladder — minors are whole seconds (or skipped), so the
        // grid stays frame-exact at any fps.
        struct TickLevel { float secs; int subdivs; };
        static const TickLevel levels[] = {
            {2.f,    2},   // 2 s,    minor every 1 s
            {5.f,    5},   // 5 s,    minor every 1 s
            {10.f,   2},   // 10 s,   minor every 5 s
            {30.f,   3},   // 30 s,   minor every 10 s
            {60.f,   4},   // 1 min,  minor every 15 s
            {300.f,  5},   // 5 min,  minor every 1 min
            {600.f,  2},   // 10 min, minor every 5 min
            {1800.f, 3},   // 30 min, minor every 10 min
        };
        const int NUM_LEVELS = (int)(sizeof(levels)/sizeof(levels[0]));
        major_secs       = levels[NUM_LEVELS-1].secs;
        minors_per_major = levels[NUM_LEVELS-1].subdivs;
        for (int li = 0; li < NUM_LEVELS; ++li) {
            if (levels[li].secs * zoom >= MIN_MAJOR_PX) {
                major_secs       = levels[li].secs;
                minors_per_major = levels[li].subdivs;
                break;
            }
        }
    }

    // Index-based walk (no float accumulation drift on long timelines).
    const double minor_d   = (double)major_secs / (double)minors_per_major;
    const float  minor_px  = (float)(minor_d * zoom);
    const bool   label_frames = frame_grid && minor_px >= 48.f;
    long i0 = (long)floor((double)(scroll / zoom) / minor_d);
    if (i0 < 0) i0 = 0;
    for (long i = i0; (double)i * minor_d <= (double)dur + major_secs; ++i) {
        float t  = (float)((double)i * minor_d);
        float px = origin.x + TL_LABEL_W + t * zoom - scroll;
        if (px < origin.x + TL_LABEL_W - 1.f) continue;
        if (px > origin.x + total_w) break;

        int  sub      = (int)(i % minors_per_major);
        bool is_major = (sub == 0);

        // Ticks + labels live below the loop-brace lane.
        float tk = ruler_y + LOOP_STRIP_H;
        if (is_major) {
            dl->AddLine({px, tk + 1.f}, {px, ruler_y + TL_RULER_H},
                        to_u32(Col::muted));
            char tbuf[16];
            snprintf(tbuf, sizeof(tbuf), "%s", fmt_time_short(t).c_str());
            float tw = ImGui::CalcTextSize(tbuf).x;
            float lx = px - tw * 0.5f;
            if (lx >= origin.x + TL_LABEL_W + 2.f && lx + tw <= origin.x + total_w - 2.f)
                dl->AddText({lx, tk}, to_u32(Col::muted), tbuf);
        } else {
            // Frame ticks: emphasize every 5th frame so groups read at a
            // glance; label frames as +offsets under their second when wide.
            bool emph = frame_grid && frame_step == 1 && (sub % 5 == 0);
            dl->AddLine({px, tk + (emph ? 3.f : 6.f)}, {px, ruler_y + TL_RULER_H},
                        to_u32(emph ? Col::muted : Col::dim));
            if (label_frames) {
                char fbuf[12];
                snprintf(fbuf, sizeof(fbuf), "+%d", sub * frame_step);
                float tw = ImGui::CalcTextSize(fbuf).x;
                float lx = px - tw * 0.5f;
                if (lx >= origin.x + TL_LABEL_W + 2.f && lx + tw <= origin.x + total_w - 2.f)
                    dl->AddText({lx, tk}, to_u32(Col::dim), fbuf);
            }
        }
    }

    // ── Loop brace (region) ─────────────────────────────────────────────────
    {
        auto& s_loop_drag = g_tl.loop_drag;
        float clip_l = origin.x + TL_LABEL_W, clip_r = origin.x + total_w;
        auto t_to_x = [&](float t){ return origin.x + TL_LABEL_W + t * zoom - scroll; };
        auto x_to_t = [&](float x){ return fmaxf(0.f, fminf((x - clip_l + scroll) / zoom, dur)); };

        // Snap candidates: clip edges + markers + 0/end (skip the playhead — the
        // brace shouldn't stick to it while you set a region).
        auto loop_cands = [&]() {
            std::vector<float> c;
            for (auto& tr : state.tracks)
                for (auto& cl : tr.clips) { c.push_back(cl.start); c.push_back(cl.end); }
            for (auto& mk : state.markers) c.push_back(mk.time);
            c.push_back(0.f); c.push_back(dur);
            return c;
        };

        float lo0, hi0; bool have_region = loop_region(state, lo0, hi0);
        float xin = t_to_x(lo0), xout = t_to_x(hi0);
        const float EDGE_PX = 6.f;

        bool in_strip = mouse.y >= ruler_y && mouse.y < ruler_y + LOOP_STRIP_H &&
                        mouse.x >= clip_l && mouse.x <= clip_r;

        // Grab.
        if (!tl_any_popup && s_loop_drag == 0 && in_strip && ImGui::IsMouseClicked(0)) {
            g_tl.loop_drag_down_x = mouse.x; g_tl.loop_drag_moved = false;
            if (have_region && fabsf(mouse.x - xin) <= EDGE_PX) {
                s_loop_drag = 1; g_tl.loop_drag_anchor = hi0;
            } else if (have_region && fabsf(mouse.x - xout) <= EDGE_PX) {
                s_loop_drag = 2; g_tl.loop_drag_anchor = lo0;
            } else if (have_region && mouse.x > xin && mouse.x < xout) {
                s_loop_drag = 3; g_tl.loop_drag_ref_x = mouse.x;
                g_tl.loop_drag_ref_in = lo0; g_tl.loop_drag_ref_out = hi0;
            } else {
                float t = snap_to_frame(state, x_to_t(mouse.x));
                s_loop_drag = 4; g_tl.loop_drag_anchor = t;
                state.loop_in = t; state.loop_out = t;
            }
        }
        // Drag. Edge-snap to clips/markers, then quantize to the frame grid so a
        // loop boundary never falls mid-frame (which makes the loop wrap stutter).
        if (s_loop_drag != 0 && ImGui::IsMouseDown(0)) {
            if (fabsf(mouse.x - g_tl.loop_drag_down_x) > 2.f) g_tl.loop_drag_moved = true;
            float t = snap_to_frame(state, edge_snap(x_to_t(mouse.x), loop_cands()));
            if (s_loop_drag == 1) {
                state.loop_in  = fminf(t, g_tl.loop_drag_anchor - 1e-3f);
                state.loop_out = g_tl.loop_drag_anchor;
            } else if (s_loop_drag == 2) {
                state.loop_in  = g_tl.loop_drag_anchor;
                state.loop_out = fmaxf(t, g_tl.loop_drag_anchor + 1e-3f);
            } else if (s_loop_drag == 3) {
                float span = g_tl.loop_drag_ref_out - g_tl.loop_drag_ref_in;
                float ni   = g_tl.loop_drag_ref_in + (mouse.x - g_tl.loop_drag_ref_x) / zoom;
                ni = snap_to_frame(state, edge_snap(ni, loop_cands()));
                ni = fmaxf(0.f, fminf(ni, dur - span));
                state.loop_in = ni; state.loop_out = ni + span;
            } else if (s_loop_drag == 4) {
                state.loop_in  = fminf(g_tl.loop_drag_anchor, t);
                state.loop_out = fmaxf(g_tl.loop_drag_anchor, t);
            }
        }
        // Release.
        if (s_loop_drag != 0 && ImGui::IsMouseReleased(0)) {
            bool tiny = (state.loop_out - state.loop_in) < 3.f / zoom;
            if (s_loop_drag == 4 && (tiny || !g_tl.loop_drag_moved)) {
                state.loop_in = -1.f; state.loop_out = -1.f;   // stray click → no region
            } else {
                if (tiny) { state.loop_in = -1.f; state.loop_out = -1.f; }
                else      state.loop_play = true;              // a real region arms the loop
                history_push(state, "Set loop region");
            }
            s_loop_drag = 0; s_snap_indicator = -1.f;
        }

        // Persistent loop-lane background so the grab strip is discoverable as a
        // distinct bar even when no region is set (lighter when hovered).
        dl->AddRectFilled({clip_l, ruler_y}, {clip_r, ruler_y + LOOP_STRIP_H},
                          IM_COL32(255, 255, 255, in_strip ? 16 : 9));
        dl->AddLine({clip_l, ruler_y + LOOP_STRIP_H}, {clip_r, ruler_y + LOOP_STRIP_H},
                    IM_COL32(255, 255, 255, 16));

        // Draw region tint + brace (recompute from current state).
        float lo, hi; bool region = loop_region(state, lo, hi);
        if (region || s_loop_drag != 0) {
            float bx0 = fmaxf(t_to_x(lo), clip_l), bx1 = fminf(t_to_x(hi), clip_r);
            ImU32 tint = state.loop_play ? IM_COL32(90,210,150,28) : IM_COL32(150,150,170,16);
            if (bx1 > bx0) dl->AddRectFilled({bx0, ruler_y + LOOP_STRIP_H}, {bx1, origin.y + total_h}, tint);
            ImU32 bc = state.loop_play ? IM_COL32(90,220,150,255) : IM_COL32(180,180,200,220);
            if (bx1 > bx0) dl->AddRectFilled({bx0, ruler_y + 1.f}, {bx1, ruler_y + LOOP_STRIP_H - 1.f}, bc);
            float exin = t_to_x(lo), exout = t_to_x(hi);
            if (exin  >= clip_l && exin  <= clip_r) dl->AddRectFilled({exin, ruler_y},     {exin+2.f, ruler_y + TL_RULER_H}, bc);
            if (exout >= clip_l && exout <= clip_r) dl->AddRectFilled({exout-2.f, ruler_y}, {exout,   ruler_y + TL_RULER_H}, bc);
        }
        if (in_strip || s_loop_drag != 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    // ── Markers (locators) — interactive flags in the ruler band ─────────────
    {
        float band_y0 = ruler_y + LOOP_STRIP_H;
        float band_y1 = ruler_y + TL_RULER_H;
        float clip_l  = origin.x + TL_LABEL_W, clip_r = origin.x + total_w;
        auto  x_to_t  = [&](float x){ return fmaxf(0.f, fminf((x - clip_l + scroll) / zoom, dur)); };
        auto  mk_cands = [&]() {
            std::vector<float> c;
            for (auto& tr : state.tracks)
                for (auto& cl : tr.clips) { c.push_back(cl.start); c.push_back(cl.end); }
            c.push_back(0.f); c.push_back(dur);
            return c;
        };

        int hover_idx = -1;
        for (int mi = 0; mi < (int)state.markers.size(); ++mi) {
            Marker& m = state.markers[mi];
            float px = origin.x + TL_LABEL_W + m.time * zoom - scroll;
            if (px < clip_l - 60.f || px > clip_r + 4.f) continue;
            bool  being = (g_tl.marker_drag == mi);
            ImU32 rgb   = m.color & 0x00FFFFFFu;

            // Guide line down the tracks (faint unless grabbed).
            if (px >= clip_l && px <= clip_r)
                dl->AddLine({px, band_y1}, {px, origin.y + total_h},
                            rgb | (being ? 0xFF000000u : 0x55000000u), being ? 1.5f : 1.f);

            // Flag tab + downward notch at the exact time.
            const char* lbl = m.label.empty() ? "\xe2\x80\xa2" : m.label.c_str();
            float tw  = ImGui::CalcTextSize(lbl).x;
            float tabw = tw + 8.f;
            ImVec2 a = {px, band_y0}, b = {px + tabw, band_y1 - 1.f};
            bool over = mouse.x >= px - 4.f && mouse.x <= b.x &&
                        mouse.y >= band_y0 && mouse.y <= band_y1;
            if (over) hover_idx = mi;
            ImU32 fill = rgb | (over || being ? 0xFF000000u : 0xDD000000u);
            dl->AddRectFilled(a, b, fill, 2.f, ImDrawFlags_RoundCornersRight);
            dl->AddTriangleFilled({px, band_y1}, {px + 5.f, band_y1 - 5.f},
                                  {px + 5.f, band_y1}, fill);
            if (g_tl.marker_rename != mi)
                dl->AddText({px + 4.f, band_y0 + 1.f}, IM_COL32(18,18,22,255), lbl);
        }

        // Interaction (uses the topmost hovered flag).
        if (!tl_any_popup && hover_idx >= 0 && g_tl.marker_drag < 0 &&
            ImGui::IsMouseDoubleClicked(0)) {
            g_tl.marker_rename = hover_idx; g_tl.marker_rename_focus = true;
            snprintf(g_tl.marker_rename_buf, sizeof(g_tl.marker_rename_buf),
                     "%s", state.markers[hover_idx].label.c_str());
        } else if (!tl_any_popup && hover_idx >= 0 && g_tl.marker_drag < 0 &&
                   g_tl.marker_rename < 0 && ImGui::IsMouseClicked(0)) {
            g_tl.marker_drag = hover_idx; g_tl.marker_drag_moved = false;
            g_tl.marker_down_x = mouse.x;
        }
        if (g_tl.marker_drag >= 0 && g_tl.marker_drag < (int)state.markers.size() &&
            ImGui::IsMouseDown(0)) {
            if (fabsf(mouse.x - g_tl.marker_down_x) > 3.f) g_tl.marker_drag_moved = true;
            if (g_tl.marker_drag_moved)
                state.markers[g_tl.marker_drag].time =
                    snap_to_frame(state, edge_snap(x_to_t(mouse.x), mk_cands()));
        }
        if (g_tl.marker_drag >= 0 && ImGui::IsMouseReleased(0)) {
            int di = g_tl.marker_drag;
            if (di < (int)state.markers.size()) {
                if (!g_tl.marker_drag_moved) {
                    seek_to(state, state.markers[di].time);     // plain click = jump
                } else {
                    std::sort(state.markers.begin(), state.markers.end(),
                              [](const Marker& x, const Marker& y){ return x.time < y.time; });
                    history_push(state, "Move marker");
                }
            }
            g_tl.marker_drag = -1; s_snap_indicator = -1.f;
        }
        if (!tl_any_popup && hover_idx >= 0 && ImGui::IsMouseClicked(1)) {
            g_tl.ctx_marker = hover_idx; g_tl.open_marker_ctx = true;
            ImGui::OpenPopup("##marker_ctx");
        }

        // Inline rename field.
        int rmi = g_tl.marker_rename;
        if (rmi >= 0 && rmi < (int)state.markers.size()) {
            Marker& m = state.markers[rmi];
            float px = origin.x + TL_LABEL_W + m.time * zoom - scroll;
            // Keep the field inside the clip area: clamp width near the right
            // edge and never let it start left of the timeline.
            float bw = 110.f;
            float bx = px + 2.f;
            if (bx + bw > clip_r - 4.f) bx = clip_r - 4.f - bw;
            if (bx < clip_l + 2.f) bx = clip_l + 2.f;
            // Compact frame so the box fits the marker band height (band is
            // TL_RULER_H - LOOP_STRIP_H tall) instead of poking out of it.
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4.f, 2.f});
            ImGui::SetCursorScreenPos({bx, band_y0 + 1.f});
            ImGui::SetNextItemWidth(bw);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
            ImGui::PushStyleColor(ImGuiCol_Border,  Col::fg);
            if (g_tl.marker_rename_focus) { ImGui::SetKeyboardFocusHere(); g_tl.marker_rename_focus = false; }
            bool done = ImGui::InputText("##mk_rename", g_tl.marker_rename_buf,
                                         sizeof(g_tl.marker_rename_buf),
                                         ImGuiInputTextFlags_EnterReturnsTrue);
            if (done || (ImGui::IsItemDeactivated() && !ImGui::IsKeyPressed(ImGuiKey_Escape))) {
                m.label = g_tl.marker_rename_buf;
                history_push(state, "Rename marker");
                g_tl.marker_rename = -1;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) g_tl.marker_rename = -1;
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar();
        }

        // Right-click context menu.
        if (ImGui::BeginPopup("##marker_ctx")) {
            g_tl.open_marker_ctx = false;
            int mi = g_tl.ctx_marker;
            if (mi >= 0 && mi < (int)state.markers.size()) {
                Marker& m = state.markers[mi];
                if (ImGui::MenuItem("Rename")) {
                    g_tl.marker_rename = mi; g_tl.marker_rename_focus = true;
                    snprintf(g_tl.marker_rename_buf, sizeof(g_tl.marker_rename_buf), "%s", m.label.c_str());
                }
                if (ImGui::MenuItem("Loop to next marker")) {
                    float nt = -1.f;
                    for (auto& mm : state.markers) if (mm.time > m.time + 1e-3f) { nt = mm.time; break; }
                    if (nt < 0.f) nt = dur;
                    state.loop_in = m.time; state.loop_out = nt; state.loop_play = true;
                    history_push(state, "Loop to next marker");
                }
                if (ImGui::MenuItem("Set loop start here")) {
                    state.loop_in = m.time;
                    if (state.loop_out <= m.time) state.loop_out = dur;
                    state.loop_play = true;
                    history_push(state, "Set loop start");
                }
                bool chap = m.chapter;
                if (ImGui::MenuItem("Chapter point", nullptr, &chap)) {
                    m.chapter = chap; history_push(state, "Toggle chapter");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Delete")) {
                    state.markers.erase(state.markers.begin() + mi);
                    history_push(state, "Delete marker");
                }
            }
            ImGui::EndPopup();
        }

        if (hover_idx >= 0 && g_tl.marker_drag < 0) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    // Tracks
    // Vertical scroll: mouse wheel in the track body area
    float track_area_top = origin.y + TL_RULER_H;
    float track_area_bot = origin.y + total_h - TL_SCROLLBAR_H;
    // Per-track mouse hit-tests below compare against the SCROLLED track_y, which
    // the visual clip rect doesn't constrain. Gate every one on this so a row
    // scrolled out of view can't catch clicks up in the ruler, the left toolbar
    // (label column) or the right panel (track body) — both share the timeline's X.
    bool mouse_in_track_band = mouse.y >= track_area_top && mouse.y < track_area_bot;

    // ── Variable track heights ────────────────────────────────────────────────
    // A content clip with fx_expanded grows its track by a lane per coupled
    // effect, so the FX timing lanes draw beneath the clip body (the body itself
    // stays TL_TRACK_H). Anything mapping a track index ↔ a Y must go through
    // these helpers, never ti*TL_TRACK_H.
    const float FX_LANE_H = 15.f;
    auto host_fx_count = [&](int ti, int ci) -> int {
        int n = 0;
        for (int k : coupled_fx_of(state, ti, ci)) {
            const Clip& b = state.tracks[(size_t)ti].clips[(size_t)k];
            n += (b.clip_type == ClipType::MultiFX || b.clip_type == ClipType::AudioMultiFX)
                 ? (int)b.fx_chain.size() : 1;
        }
        return n;
    };
    auto track_extra_h = [&](int ti) -> float {
        if (ti < 0 || ti >= (int)state.tracks.size()) return 0.f;
        float e = 0.f;
        auto& cls = state.tracks[(size_t)ti].clips;
        for (int ci = 0; ci < (int)cls.size(); ++ci) {
            if (!cls[(size_t)ci].fx_expanded || is_fx_clip(cls[(size_t)ci])) continue;
            int n = host_fx_count(ti, ci);
            if (n > 0) e = fmaxf(e, (float)n * FX_LANE_H + 6.f);
        }
        return e;
    };
    auto track_h     = [&](int ti) -> float { return TL_TRACK_H + track_extra_h(ti); };
    auto track_top_at = [&](int ti) -> float {
        float y = track_area_top - state.tl_v_scroll;
        for (int k = 0; k < ti && k < (int)state.tracks.size(); ++k) y += track_h(k);
        return y;
    };
    auto y_to_track = [&](float my) -> int {
        float y = track_area_top - state.tl_v_scroll;
        for (int k = 0; k < (int)state.tracks.size(); ++k) {
            float h = track_h(k);
            if (my < y + h) return k;
            y += h;
        }
        return (int)state.tracks.size();
    };

    float tracks_total_h = TL_TRACK_H;   // the trailing add-track row
    for (int ti = 0; ti < (int)state.tracks.size(); ++ti) tracks_total_h += track_h(ti);
    float max_v_scroll   = fmaxf(0.f, tracks_total_h - (track_area_bot - track_area_top));
    if (ImGui::IsMouseHoveringRect({origin.x, track_area_top}, {origin.x+total_w, track_area_bot})) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f && !ImGui::GetIO().KeyCtrl)
            state.tl_v_scroll -= wheel * TL_TRACK_H;
    }

    // ── Edge auto-scroll while dragging ───────────────────────────────────────
    // Dragging a clip (body or trim edge), a transition glass, or a marquee box
    // past the visible clip area scrolls the timeline toward the mouse so the
    // gesture can continue beyond the current view. Speed scales with how far
    // the mouse overshoots the edge. Drag math recomputes positions from
    // mouse + scroll every frame, so the dragged clip rides along; the marquee
    // anchor is screen-space and gets shifted so its corner stays pinned to
    // the same timeline position.
    if ((g_tl.drag_track >= 0 || g_tl.box_selecting || g_tl.glass_drag != 0) &&
        ImGui::IsMouseDown(0)) {
        ImVec2 mp = ImGui::GetIO().MousePos;
        float  dt = ImGui::GetIO().DeltaTime;
        auto edge_step = [&](float overshoot) {
            return fminf(overshoot, 160.f) * 8.f * dt;  // px this frame, capped
        };
        // Horizontal
        {
            float x_lo = origin.x + TL_LABEL_W;
            float x_hi = x_lo + clip_area_w;
            // Edge trims scroll early (at the margin line, not the border)
            // and — crucially — with NO content-derived ceiling: during a
            // trim the user's push is the only scroll driver. The dragged
            // edge rides margin-short of the border, so any ceiling tied to
            // the (edge-defined) content end cancels against the setback and
            // deadlocks the gesture. Post-release housekeeping glides the
            // view back into legal bounds.
            bool  trim  = (g_tl.drag_left || g_tl.drag_right) && g_tl.drag_track >= 0;
            float inset = trim ? fit_margin : 0.f;
            float max_scroll = fmaxf(0.f, tl_content_w - clip_area_w + fit_margin);
            float ds = 0.f;
            if      (mp.x < x_lo + inset) ds = -edge_step(x_lo + inset - mp.x);
            else if (mp.x > x_hi - inset) ds =  edge_step(mp.x - (x_hi - inset));
            if (ds != 0.f) {
                float ns = scroll + ds;
                if (!trim) ns = fminf(ns, max_scroll);
                ns = fmaxf(0.f, ns);
                if (g_tl.box_selecting) g_tl.box_start.x -= ns - scroll;
                scroll = ns;
            }
            // NOTE: edge trims deliberately never change zoom. A mid-gesture
            // scale change — even cursor-anchored and floor-latched — swaps
            // the user's frame of reference while they're measuring by eye.
            // Auto-scroll at constant zoom is the entire follow behavior.
        }
        // Vertical (clip drags move across tracks; marquee spans them)
        if (g_tl.drag_track >= 0 || g_tl.box_selecting) {
            float dv = 0.f;
            if      (mp.y < track_area_top) dv = -edge_step(track_area_top - mp.y);
            else if (mp.y > track_area_bot) dv =  edge_step(mp.y - track_area_bot);
            if (dv != 0.f) {
                float nv = fmaxf(0.f, fminf(max_v_scroll, state.tl_v_scroll + dv));
                if (g_tl.box_selecting) g_tl.box_start.y -= nv - state.tl_v_scroll;
                state.tl_v_scroll = nv;
            }
        }
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

    // A freshly-added brick (clip_flash → glow_start) gets a quick bright pulse on
    // its rect so you can see where it landed — for your adds and the agent's. The
    // -1 sentinel set at add time is stamped with the current time on first draw,
    // then fades over NEW_GLOW_DUR. glfwPollEvents keeps the loop ticking so it
    // animates; the track-area clip rect keeps it inside the visible band.
    const float NEW_GLOW_DUR = 0.40f;
    auto draw_added_glow = [&](Clip& c, float gx0, float gy0, float gx1, float gy1) {
        if (c.glow_start < 0.0) c.glow_start = ImGui::GetTime();   // stamp pending
        if (c.glow_start <= 0.0) return;
        float el = (float)(ImGui::GetTime() - c.glow_start);
        if (el >= NEW_GLOW_DUR) return;
        float a = 1.f - el / NEW_GLOW_DUR; a *= a;                 // ease-out
        dl->AddRectFilled({gx0, gy0}, {gx1, gy1}, IM_COL32(150, 220, 255, (int)(55.f * a)), 2.f);
        dl->AddRect({gx0 - 1.f, gy0 - 1.f}, {gx1 + 1.f, gy1 + 1.f},
                    IM_COL32(175, 228, 255, (int)(210.f * a)), 3.f, 0, 2.f);
    };

    for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
        Track& track = state.tracks[ti];
        ImVec2 row_tl = {origin.x, track_y};
        ImVec2 row_br = {origin.x+total_w, track_y+TL_TRACK_H};
        // Confine the hover/drop hit to the visible band — same reason as the
        // clip clamp above: a scrolled-out row must not light up (or accept
        // drops) from a click up in the ruler or panel.
        bool row_hov = mouse.y >= fmaxf(row_tl.y, track_area_top) &&
                       mouse.y < fminf(row_br.y, track_area_bot);
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
                        float drop_t = snap_to_frame(state, fmaxf(0.f, (ImGui::GetMousePos().x - (origin.x + TL_LABEL_W) + scroll) / zoom));
                        Clip cl;
                        cl.clip_type = ClipType::Effect;
                        cl.start     = drop_t;
                        cl.end       = drop_t + 2.f;
                        preset_apply(cl, *preset);
                        state.tracks[ti].clips.push_back(cl);
                        // Drop = global brick (applies to everything below); welding
                        // is the deliberate drag-onto-content-and-hold gesture.
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
                        float drop_t = snap_to_frame(state, fmaxf(0.f, (ImGui::GetMousePos().x - (origin.x + TL_LABEL_W) + scroll) / zoom));
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
                if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("TEXT_STYLE")) {
                    AnimStyle st = (AnimStyle)*(const int*)pay->Data;
                    float drop_t = snap_to_frame(state, fmaxf(0.f, (ImGui::GetMousePos().x - (origin.x + TL_LABEL_W) + scroll) / zoom));
                    state.tracks[ti].clips.push_back(make_text_brick(st, drop_t));
                    state.selected_track = ti;
                    state.selected_clip  = (int)state.tracks[ti].clips.size() - 1;
                    s_drop_flash_track = ti;
                    s_drop_flash_t     = 0.6f;
                    history_push(state, std::string("Drop text brick: ") + text_style_name(st));
                }
                if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("FX_CREATIVE")) {
                    FXType ft = (FXType)*(const int*)pay->Data;
                    float drop_t = snap_to_frame(state, fmaxf(0.f, (ImGui::GetMousePos().x - (origin.x + TL_LABEL_W) + scroll) / zoom));
                    Clip cl;
                    cl.clip_type = ClipType::Effect;
                    cl.fx_type   = ft;
                    cl.start     = drop_t;
                    cl.end       = drop_t + 5.f;
                    state.tracks[ti].clips.push_back(cl);
                    // Drop = global brick; weld via drag-onto-content + hold 1.5s.
                    state.selected_clip  = (int)state.tracks[ti].clips.size() - 1;
                    state.selected_track = ti;
                    s_drop_flash_track = ti;
                    s_drop_flash_t     = 0.6f;
                    history_push(state, std::string("Drop FX: ") + fx_type_name(ft));
                }
                // Audio FX card → Effect clip carrying the audio_fx flag, welded
                // to the audio (or video) host under the drop via couple_fx_now.
                if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("FX_AUDIO")) {
                    FXType ft = (FXType)*(const int*)pay->Data;
                    float drop_t = snap_to_frame(state, fmaxf(0.f, (ImGui::GetMousePos().x - (origin.x + TL_LABEL_W) + scroll) / zoom));
                    Clip cl;
                    cl.clip_type = ClipType::Effect;
                    cl.fx_type   = ft;
                    cl.start     = drop_t;
                    cl.end       = drop_t + 5.f;
                    cl.audio_fx.autotune_on      = (ft == FXType::AudioAutotune);
                    cl.audio_fx.pitch_on         = (ft == FXType::AudioPitch);
                    cl.audio_fx.formant_on       = (ft == FXType::AudioFormant);
                    cl.audio_fx.delay_on         = (ft == FXType::AudioDelay);
                    cl.audio_fx.reverb_on        = (ft == FXType::AudioReverb);
                    cl.audio_fx.voice_convert_on = (ft == FXType::AudioVoiceConvert);
                    state.tracks[ti].clips.push_back(cl);
                    state.selected_clip  = couple_fx_now(state, ti, (int)state.tracks[ti].clips.size() - 1);
                    state.selected_track = ti;
                    s_drop_flash_track = ti; s_drop_flash_t = 0.6f;
                    history_push(state, std::string("Drop audio FX: ") + fx_type_name(ft));
                }
                // Custom (runtime) FX card → a per-clip property, so it applies to
                // the clip under the drop point rather than spawning a brick.
                if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("FX_RUNTIME")) {
                    int idx = *(const int*)pay->Data;
                    const auto& defs = runtime_fx_list();
                    float drop_t = snap_to_frame(state, fmaxf(0.f, (ImGui::GetMousePos().x - (origin.x + TL_LABEL_W) + scroll) / zoom));
                    int target = -1;
                    for (int k = 0; k < (int)state.tracks[ti].clips.size(); ++k) {
                        const Clip& c = state.tracks[ti].clips[(size_t)k];
                        if (clip_is_videolike_type(c.clip_type) &&
                            drop_t >= c.start && drop_t < c.end) { target = k; break; }
                    }
                    if (idx >= 0 && idx < (int)defs.size() && target >= 0 &&
                        defs[(size_t)idx].program != 0) {
                        const RuntimeFXDef& def = defs[(size_t)idx];
                        Clip& c = state.tracks[ti].clips[(size_t)target];
                        c.runtime_fx_id = def.id;
                        c.runtime_fx_params.resize(def.params.size());
                        for (int pi = 0; pi < (int)def.params.size(); ++pi)
                            c.runtime_fx_params[pi] = def.params[pi].default_val;
                        c.runtime_fx_amount = 1.f;
                        state.selected_track = ti; state.selected_clip = target;
                        s_drop_flash_track = ti; s_drop_flash_t = 0.6f;
                        history_push(state, "Apply custom FX: " + def.name);
                    }
                }
                // Body FX card → a BodyFX glass brick over the video clip under the
                // drop, with AI mask generation kicked off if not ready.
                if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("FX_BODY")) {
                    BodyFXType bt = (BodyFXType)*(const int*)pay->Data;
                    float drop_t = snap_to_frame(state, fmaxf(0.f, (ImGui::GetMousePos().x - (origin.x + TL_LABEL_W) + scroll) / zoom));
                    int target = -1;
                    for (int k = 0; k < (int)state.tracks[ti].clips.size(); ++k) {
                        const Clip& c = state.tracks[ti].clips[(size_t)k];
                        if (c.clip_type == ClipType::Video &&
                            drop_t >= c.start && drop_t < c.end) { target = k; break; }
                    }
                    const BodyFXInfo* info = body_fx_find_info(bt);
                    if (target >= 0 && info) {
                        Clip& vid_cl = state.tracks[ti].clips[(size_t)target];
                        Clip bfx;
                        bfx.clip_type    = ClipType::BodyFX;
                        bfx.start        = vid_cl.start;
                        bfx.end          = vid_cl.end;
                        bfx.source_id    = vid_cl.source_id;
                        bfx.body_fx_type = bt;
                        for (int pi = 0; pi < 4; ++pi)
                            bfx.body_fx_params[pi] = (pi < info->n_params)
                                ? info->params[pi].default_val : 0.5f;
                        bfx.body_fx_amount = 1.f;
                        state.tracks[ti].clips.push_back(bfx);
                        if (vid_cl.bg_remove_mask_dir.empty() ||
                            vid_cl.bg_remove_status != BgRemoveStatus::Ready)
                            bg_remove_start(state, ti, target);
                        state.selected_track = ti;
                        state.selected_clip  = (int)state.tracks[ti].clips.size() - 1;
                        s_drop_flash_track = ti; s_drop_flash_t = 0.6f;
                        history_push(state, std::string("Drop Body FX: ") + info->name);
                    }
                }
                auto accept_media_drop = [&](const char* ptype) {
                    if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload(ptype)) {
                        std::string path((const char*)pay->Data, pay->DataSize - 1);
                        bool img = is_image_path(path);
                        float drop_t = snap_to_frame(state, fmaxf(0.f, (ImGui::GetMousePos().x - (origin.x + TL_LABEL_W) + scroll) / zoom));
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
                    float drop_t = snap_to_frame(state, fmaxf(0.f, (ImGui::GetMousePos().x - (origin.x + TL_LABEL_W) + scroll) / zoom));
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
        bool in_label = mouse_in_track_band &&
                        mouse.x >= origin.x+2.f && mouse.x < origin.x+TL_LABEL_W-54.f &&
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
            bool hov = mouse_in_track_band && fabsf(mouse.x-eye_c.x)<8.f && fabsf(mouse.y-eye_c.y)<8.f;
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
            bool hov  = mouse_in_track_band && fabsf(mouse.x-lock_c.x)<8.f && fabsf(mouse.y-lock_c.y)<8.f;
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
            bool hov = mouse_in_track_band && fabsf(mouse.x-mut_c.x)<8.f && fabsf(mouse.y-mut_c.y)<8.f;
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
        if ((!tl_any_popup && ImGui::IsMouseClicked(1)) && mouse_in_track_band &&
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
            // Clamp the Y range used for mouse hit-testing to the visible track
            // area. Without the bottom clamp, clips partially clipped at the
            // bottom extend cy1 past track_area_bot and steal scrollbar clicks.
            // Without the top clamp, scrolling down with many tracks pushes the
            // upper clips' cy0 ABOVE track_area_top — up through the ruler and
            // into the panel above (the timeline spans the full width) — leaving
            // a live hit zone that steals ruler/panel clicks. A fully scrolled-out
            // clip ends up with cy0 > cy1, so the hit test below is empty.
            cy1 = std::min(cy1, track_area_bot);
            cy0 = std::max(cy0, track_area_top);
            const float ew = 6.f, ew_hit = 12.f;
            // Edge handles — only where the TRUE edge is on screen. vis_x0/x1
            // are clamped to the viewport, so a zoomed-in view mid-clip would
            // otherwise draw a phantom handle at the boundary where no
            // trimmable edge exists.
            if (sel) {
                float tx0 = origin.x + TL_LABEL_W + clip.start * zoom - scroll;
                float tx1 = origin.x + TL_LABEL_W + clip.end   * zoom - scroll;
                if (fabsf(tx0 - vis_x0) < 0.5f)
                    dl->AddRectFilled({vis_x0,cy0},{vis_x0+ew,cy1},to_u32(Col::muted),1.f);
                if (fabsf(tx1 - vis_x1) < 0.5f)
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
            // Keyframe diamond markers. Simultaneous keys across props merge
            // into one "super diamond" (stacked like the MultiFX brick icon):
            // clicking selects / cycles through members, dragging retimes the
            // whole group as one pose.
            bool kf_claimed = false;  // diamond consumed this click; skip clip select/drag
            if (!clip.ktracks.empty()) {
                // Dedicated lane at the bottom of the clip body: a dark strip
                // the diamonds sit on, so they stay legible over any thumbnail
                // (4px grey floating mid-clip was invisible on busy video).
                float lane_b = cy1 - 2.f, lane_t = fmaxf(cy0 + 2.f, cy1 - 13.f);
                dl->AddRectFilled({vis_x0, lane_t}, {vis_x1, lane_b},
                                  IM_COL32(8, 8, 12, 170), 3.f);
                float kf_mid_y = (lane_t + lane_b) * 0.5f;

                // Group by actual timestamp (the PropTrack::set tolerance),
                // NOT by screen overlap — a super diamond means "genuinely
                // simultaneous", so zooming out never welds unrelated keys.
                struct Mem { const std::string* prop; int idx; float value; };
                struct Grp { float time; std::vector<Mem> mems; };
                std::vector<Grp> groups;
                for (auto& [pname, pt] : clip.ktracks) {
                    for (int ki = 0; ki < (int)pt.keys.size(); ++ki) {
                        float kt = pt.keys[ki].time;
                        Grp* g = nullptr;
                        for (auto& gg : groups)
                            if (fabsf(gg.time - kt) < 0.02f) { g = &gg; break; }
                        if (!g) { groups.push_back({kt, {}}); g = &groups.back(); }
                        g->mems.push_back({&pname, ki, pt.keys[ki].value});
                    }
                }
                for (auto& g : groups)
                    std::sort(g.mems.begin(), g.mems.end(),
                              [](const Mem& a, const Mem& b){ return *a.prop < *b.prop; });

                for (auto& g : groups) {
                    bool  super = g.mems.size() > 1;
                    float d  = super ? 6.f : 5.f;
                    float kx = origin.x + TL_LABEL_W + (clip.start + g.time) * zoom - scroll;
                    if (kx < vis_x0 - d || kx > vis_x1 + d) continue;
                    auto is_mem = [&](const std::string& prop, int idx) {
                        for (auto& m : g.mems)
                            if (*m.prop == prop && m.idx == idx) return true;
                        return false;
                    };
                    bool dragging_this = s_kf_drag.active &&
                        s_kf_drag.ti == ti && s_kf_drag.ci == ci &&
                        !s_kf_drag.mems.empty() &&
                        is_mem(s_kf_drag.mems[0].prop, s_kf_drag.mems[0].idx);
                    bool kf_sel = state.kf_sel_track == ti &&
                                  state.kf_sel_clip  == ci &&
                                  is_mem(state.kf_sel_prop, state.kf_sel_idx);
                    bool hovered = !s_kf_drag.active && !tl_any_popup &&
                                   fabsf(mouse.x - kx) < d + 3.f &&
                                   fabsf(mouse.y - kf_mid_y) < d + 3.f;
                    // Singles stay white; super diamonds are blue so a merged
                    // pose reads at a glance. Selection gold wins over both.
                    ImU32 kc = (kf_sel || dragging_this) ? IM_COL32(255,200,60,255)
                             : hovered ? (super ? IM_COL32(170,220,255,255)
                                               : IM_COL32(255,235,160,230))
                                       : (super ? IM_COL32(110,190,255,255)
                                               : IM_COL32(235,235,235,255));
                    auto diamond = [&](float cx, float cy, float r, ImU32 fill) {
                        dl->AddQuadFilled({cx, cy-r}, {cx+r, cy},
                                          {cx, cy+r}, {cx-r, cy}, fill);
                        dl->AddQuad({cx, cy-r}, {cx+r, cy},
                                    {cx, cy+r}, {cx-r, cy},
                                    IM_COL32(0, 0, 0, 200), 1.f);
                    };
                    // Super diamonds get a dimmer echo peeking out behind —
                    // same stacked-layers language as the MultiFX brick.
                    if (super) diamond(kx + 3.f, kf_mid_y, d - 1.f,
                                       IM_COL32(55, 110, 185, 255));
                    diamond(kx, kf_mid_y, d, kc);

                    if (hovered) {
                        char tip[256]; int off = 0;
                        off += snprintf(tip + off, sizeof(tip) - off,
                                        "%.2fs", g.time);
                        for (auto& m : g.mems) {
                            if (off >= (int)sizeof(tip) - 32) break;
                            off += snprintf(tip + off, sizeof(tip) - off,
                                            "\n%s = %.3f", m.prop->c_str(), m.value);
                        }
                        snprintf(tip + off, sizeof(tip) - off,
                                 super ? "\nclick cycles \xc2\xb7 drag moves all %d \xc2\xb7 right-click unpairs"
                                       : "\nclick to select \xc2\xb7 drag to move",
                                 (int)g.mems.size());
                        ImGui::SetTooltip("%s", tip);
                        if (super && ImGui::IsMouseClicked(1)) {
                            s_kf_ctx.ti = ti; s_kf_ctx.ci = ci;
                            s_kf_ctx.time = g.time;
                            state.selected_track = ti;
                            state.selected_clip  = ci;
                            clip_ctx_opened_this_frame = true;  // not the clip menu
                            ImGui::OpenPopup("##kf_ctx");
                        }
                        if (ImGui::IsMouseClicked(0)) {
                            // Select: cycle through members if one is already
                            // selected (same gesture as Alt+click layer cycle
                            // on the canvas), else start at the first.
                            int cur = -1;
                            if (state.kf_sel_track == ti && state.kf_sel_clip == ci)
                                for (int mi = 0; mi < (int)g.mems.size(); ++mi)
                                    if (*g.mems[mi].prop == state.kf_sel_prop &&
                                        g.mems[mi].idx   == state.kf_sel_idx) { cur = mi; break; }
                            const Mem& pick = g.mems[(cur + 1) % (int)g.mems.size()];
                            state.kf_sel_track = ti;
                            state.kf_sel_clip  = ci;
                            state.kf_sel_prop  = *pick.prop;
                            state.kf_sel_idx   = pick.idx;
                            state.selected_track = ti;
                            state.selected_clip  = ci;
                            s_kf_drag.active = true;
                            s_kf_drag.ti = ti; s_kf_drag.ci = ci;
                            s_kf_drag.mems.clear();
                            for (auto& m : g.mems)
                                s_kf_drag.mems.push_back({*m.prop, m.idx});
                            s_kf_drag.moved = false;
                            kf_claimed = true;
                            s_clip_hit = true;  // it's still a clip hit — don't deselect
                        }
                    }
                }
                // Retime drag in progress — move every member of the grabbed
                // group to the same new time so the pose stays simultaneous.
                if (s_kf_drag.active && s_kf_drag.ti == ti && s_kf_drag.ci == ci) {
                    bool valid = !s_kf_drag.mems.empty();
                    for (auto& m : s_kf_drag.mems) {
                        auto it_kt = clip.ktracks.find(m.prop);
                        if (it_kt == clip.ktracks.end() ||
                            m.idx >= (int)it_kt->second.keys.size()) { valid = false; break; }
                    }
                    if (!valid) {
                        s_kf_drag.active = false;
                    } else if (ImGui::IsMouseDown(0)) {
                        float t_new = (mouse.x - origin.x - TL_LABEL_W + scroll) / zoom
                                      - clip.start;
                        // Hold-to-weld: hovering the drag over another diamond
                        // arms a 3 s ring timer instead of merging on contact.
                        // Until it completes the drag parks just beside the
                        // target, so releasing early leaves them neighbors;
                        // when the ring closes, the keys snap to the exact
                        // same time and become one (blue) super diamond.
                        const double now = ImGui::GetTime();
                        float weld_zone = fmaxf(0.02f, 8.f / zoom);
                        float park_off  = fmaxf(0.03f, 4.f / zoom);
                        const Grp* tgt = nullptr;
                        bool tgt_weldable = false;
                        for (auto& g : groups) {
                            bool is_dragged = false;
                            for (auto& m : g.mems)
                                if (*m.prop == s_kf_drag.mems[0].prop &&
                                    m.idx   == s_kf_drag.mems[0].idx) { is_dragged = true; break; }
                            if (is_dragged) continue;
                            if (fabsf(t_new - g.time) >= weld_zone) continue;
                            if (tgt && fabsf(t_new - g.time) >= fabsf(t_new - tgt->time))
                                continue;
                            // A super diamond holds at most one key per prop —
                            // groups sharing a prop with the drag park beside
                            // each other but never weld (no timer either).
                            bool shares_prop = false;
                            for (auto& m : g.mems) {
                                for (auto& dm : s_kf_drag.mems)
                                    if (*m.prop == dm.prop) { shares_prop = true; break; }
                                if (shares_prop) break;
                            }
                            tgt = &g;
                            tgt_weldable = !shares_prop;
                        }
                        if (tgt && !tgt_weldable) {
                            // Same-prop neighbour: can't form a pose, but don't
                            // dodge it either — let the key snap straight onto its
                            // frame so dropping it there welds (merges) on release.
                            s_kf_drag.weld_target = -1.f;
                        } else if (tgt) {
                            if (fabsf(s_kf_drag.weld_target - tgt->time) > 0.0005f) {
                                s_kf_drag.weld_target = tgt->time;   // new candidate
                                s_kf_drag.weld_start  = now;         // arm the timer
                            }
                            float prog = (float)((now - s_kf_drag.weld_start) / kWeldHoldSec);
                            if (prog >= 1.f) {
                                t_new = tgt->time;                   // weld!
                                s_kf_flash.active = true;
                                s_kf_flash.t0 = now;
                                s_kf_flash.ti = ti; s_kf_flash.ci = ci;
                                s_kf_flash.time = tgt->time;
                                s_kf_drag.weld_target = -1.f;
                                // Absorb the target's members so the merged
                                // group keeps dragging as one from here on.
                                for (auto& m : tgt->mems)
                                    s_kf_drag.mems.push_back({*m.prop, m.idx});
                            } else {
                                t_new = tgt->time +
                                        (t_new >= tgt->time ? park_off : -park_off);
                                // The timer ring, filling clockwise around the
                                // target with a gentle pulse.
                                float tx = origin.x + TL_LABEL_W +
                                           (clip.start + tgt->time) * zoom - scroll;
                                float pr = 9.f * (1.f + 0.1f * sinf((float)now * 9.f));
                                dl->PathArcTo({tx, kf_mid_y}, pr, -IM_PI * 0.5f,
                                              -IM_PI * 0.5f + prog * 2.f * IM_PI, 24);
                                dl->PathStroke(IM_COL32(110,190,255,230), 0, 2.f);
                            }
                        } else {
                            s_kf_drag.weld_target = -1.f;
                        }
                        // Snap to the frame grid like the playhead. Same-prop
                        // collisions are resolved by welding (merge) on release,
                        // not by dodging to another frame.
                        t_new = snap_to_frame(state, clip.start + t_new) - clip.start;
                        t_new = fmaxf(0.f, fminf(t_new, clip.end - clip.start));
                        for (auto& m : s_kf_drag.mems) {
                            auto& keys = clip.ktracks[m.prop].keys;
                            bool was_sel = state.kf_sel_prop == m.prop &&
                                           state.kf_sel_idx  == m.idx;
                            if (fabsf(t_new - keys[m.idx].time) > 1e-4f)
                                s_kf_drag.moved = true;
                            keys[m.idx].time = t_new;
                            // Bubble past neighbors so the track stays sorted.
                            while (m.idx > 0 &&
                                   keys[m.idx].time < keys[m.idx-1].time) {
                                std::swap(keys[m.idx], keys[m.idx-1]);
                                --m.idx;
                            }
                            while (m.idx + 1 < (int)keys.size() &&
                                   keys[m.idx].time > keys[m.idx+1].time) {
                                std::swap(keys[m.idx], keys[m.idx+1]);
                                ++m.idx;
                            }
                            if (was_sel) state.kf_sel_idx = m.idx;
                        }
                    } else {
                        if (s_kf_drag.moved) {
                            // Weld: a dropped key that shares its frame with
                            // another key of the same prop merges into it — the
                            // dragged key wins, the one it landed on is removed.
                            float kfps = tl_fps(state); if (!(kfps > 0.f)) kfps = 30.f;
                            float half = 0.5f / kfps;
                            bool welded = false;
                            for (auto& m : s_kf_drag.mems) {
                                auto itk = clip.ktracks.find(m.prop);
                                if (itk == clip.ktracks.end()) continue;
                                auto& ks = itk->second.keys;
                                if (m.idx < 0 || m.idx >= (int)ks.size()) continue;
                                float dt = ks[m.idx].time;
                                for (int k = (int)ks.size() - 1; k >= 0; --k) {
                                    if (k == m.idx) continue;
                                    if (fabsf(ks[k].time - dt) < half) {
                                        ks.erase(ks.begin() + k);
                                        if (k < m.idx) --m.idx;   // dragged key shifted left
                                        welded = true;
                                    }
                                }
                            }
                            history_push(state, welded
                                ? "Weld keyframe"
                                : (s_kf_drag.mems.size() > 1 ? "Move keyframe group"
                                                             : "Move keyframe"));
                        }
                        s_kf_drag.active = false;
                    }
                }
                // Weld-complete flash: a quick expanding ring fading out over
                // the freshly merged super diamond.
                if (s_kf_flash.active && s_kf_flash.ti == ti && s_kf_flash.ci == ci) {
                    float el = (float)(ImGui::GetTime() - s_kf_flash.t0);
                    if (el > 0.45f) {
                        s_kf_flash.active = false;
                    } else {
                        float fx = origin.x + TL_LABEL_W +
                                   (clip.start + s_kf_flash.time) * zoom - scroll;
                        float fr = 7.f + el * 34.f;
                        int   fa = (int)(220.f * (1.f - el / 0.45f));
                        dl->AddCircle({fx, kf_mid_y}, fr,
                                      IM_COL32(140, 200, 255, fa), 20, 2.f);
                    }
                }
            }
            // Left click — select / drag
            bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
            if (!kf_claimed && !s_trans_hit_this_frame && !any_popup && (!tl_any_popup && ImGui::IsMouseClicked(0))) {
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
                    // A coupled brick is an attachment of its content: left-
                    // click selects the HOST — its chain lives in the content
                    // panel's FX tab now. (Right-click still hits the brick
                    // for "Decouple".)
                    if (clip.fx_coupled && is_fx_clip(clip)) {
                        int host = fx_coupled_host(state, ti, clip);
                        if (host >= 0) {
                            state.selected_clip = host;
                            state.clip_selection.clear();
                            state.clip_selection.insert({ti, host});
                        }
                    }
                    strncpy(s_edit_buf, clip.text.c_str(), sizeof(s_edit_buf)-1);
                    s_edit_buf[sizeof(s_edit_buf)-1] = '\0';
                    s_edit_focus_next = (clip.clip_type==ClipType::Text || clip.clip_type==ClipType::Lyrics ||
                                         clip.clip_type==ClipType::Subtitle);
                    float orig_cx0 = origin.x+TL_LABEL_W+clip.start*zoom-scroll;
                    float orig_cx1 = origin.x+TL_LABEL_W+clip.end*zoom-scroll;
                    // Coupled FX bricks are pinned to their host — click
                    // selects, but no drag/trim. Decouple to move them.
                    if (!track.locked && !(clip.fx_coupled && is_fx_clip(clip))) {
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
                        } else if (clip.clip_type == ClipType::Lyrics) {
                            // Managed lyrics track: slide the brick within its
                            // track (clamped to neighbors below). Separate from
                            // the cross-track body-move blocked just above.
                            s_lyric_dt = ti; s_lyric_dc = ci;
                            s_lyric_off = (mouse.x - origin.x - TL_LABEL_W + scroll) / zoom - clip.start;
                            s_lyric_o_start = clip.start; s_lyric_o_end = clip.end;
                            g_tl.drag_multi.clear();
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
                // Proxy progress bar — shown while transcoding, absent once ready.
                // Shared bar style (blue accent flags it as a background transcode,
                // distinct from the orange AI jobs); Queued shows an idle sweep.
                if (!clip.text.empty()) {
                    auto pst = proxy_job_status(clip.text);
                    const float bar_h = 3.f;
                    const ImU32 pacc = IM_COL32(100, 210, 255, 255);
                    if (pst.state == ProxyJobStatus::State::Generating)
                        ui_progress_bar(dl, vis_x0, cy1-bar_h, vis_x1-vis_x0, pst.progress, pacc, bar_h);
                    else if (pst.state == ProxyJobStatus::State::Queued)
                        ui_progress_bar(dl, vis_x0, cy1-bar_h, vis_x1-vis_x0, -1.f, pacc, bar_h);
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
            } else if (clip.clip_type == ClipType::VideoRecord) {
                // Camera brick: slate at rest with an orange dot; pulses
                // orange while recording. Shows the selected take number.
                bool rec_live = vrecorder_is_target(ti, ci);
                bool has_take = clip.rec_take_sel >= 0 &&
                                clip.rec_take_sel < (int)clip.rec_takes.size();
                ImU32 fill = rec_live ? IM_COL32(150, 60, 18, 255)
                           : sel      ? IM_COL32(150, 130, 115, 255)
                           : has_take ? IM_COL32( 50,  42,  38, 255)
                                      : IM_COL32( 40,  34,  30, 255);
                dl->AddRectFilled({vis_x0,cy0},{vis_x1,cy1}, fill, 2.f);
                ImU32 border;
                if (rec_live) {
                    float t  = (float)ImGui::GetTime();
                    int   a  = (int)(170.f + 85.f * sinf(t * 6.f));
                    border = IM_COL32(255, 140, 60, a);
                } else {
                    border = sel ? IM_COL32(235, 215, 205, 255)
                                 : IM_COL32(110,  90,  75, 200);
                }
                dl->AddRect({vis_x0,cy0},{vis_x1,cy1}, border, 2.f, 0, rec_live ? 2.f : 1.f);
                ImGui::PushClipRect({vis_x0,cy0},{vis_x1,cy1},true);
                {
                    float mid = (cy0 + cy1) * 0.5f;
                    dl->AddCircleFilled({vis_x0 + 10.f, mid}, 3.5f,
                        rec_live ? IM_COL32(255,160,80,255) : IM_COL32(235,90,40,220));
                    // Photo-mode camera brick badges as IMG; video as CAM.
                    const char* cam_tag = clip.rec_photo ? "IMG" : "CAM";
                    char lbl[48];
                    if (rec_live)
                        snprintf(lbl, sizeof(lbl), "REC \xc2\xb7 %d", vrecorder_take_count());
                    else if (has_take)
                        snprintf(lbl, sizeof(lbl), "%s \xc2\xb7 take %d", cam_tag, clip.rec_take_sel + 1);
                    else
                        snprintf(lbl, sizeof(lbl), "%s", cam_tag);
                    ImU32 tc = sel ? IM_COL32(30,22,18,255) : IM_COL32(235,210,195,230);
                    dl->AddText({vis_x0 + 20.f, mid - 7.f}, tc, lbl);

                    // Face-filter landmark pass running on the selected take:
                    // show progress so the unfiltered playback in the
                    // meantime doesn't read as a broken filter.
                    if (!rec_live && has_take && clip.face_filter != 0) {
                        float fp = 0.f;
                        if (face_cache_status(clip.text, &fp) ==
                            FaceCacheStatus::Building) {
                            char fb[40];
                            snprintf(fb, sizeof(fb), "face \xc2\xb7 %d%%",
                                     (int)(fp * 100.f));
                            dl->AddText({vis_x0 + 20.f, mid + 8.f},
                                        IM_COL32(255, 190, 120, 230), fb);
                        }
                    }

                    // Take-end marker: where the selected take's content
                    // stops. The region past it is dimmed — resizing the
                    // brick edge snaps here (take ends are snap candidates).
                    if (!rec_live && has_take) {
                        float td = take_duration(clip);
                        float brick_len = clip.end - clip.start;
                        if (td > 0.05f && td < brick_len - 0.05f) {
                            float tx = cx0 + td * zoom;
                            if (tx > vis_x0 && tx < vis_x1) {
                                dl->AddRectFilled({tx, cy0}, {vis_x1, cy1},
                                                  IM_COL32(0, 0, 0, 110));
                                dl->AddLine({tx, cy0}, {tx, cy1},
                                            IM_COL32(235, 150, 80, 230), 2.f);
                            }
                        }
                    }
                }
                ImGui::PopClipRect();
            } else if (clip.clip_type == ClipType::Record) {
                // Record brick: neutral slate at rest with a red dot as the
                // type marker; the whole brick goes red only while recording.
                bool rec_live = recorder_is_target(ti, ci);
                bool has_take = clip.rec_take_sel >= 0 &&
                                clip.rec_take_sel < (int)clip.rec_takes.size();
                ImU32 fill = rec_live ? IM_COL32(140, 22, 26, 255)
                           : sel      ? IM_COL32(135, 135, 175, 255)
                           : has_take ? IM_COL32( 42,  42,  56, 255)
                                      : IM_COL32( 33,  33,  45, 255);
                dl->AddRectFilled({vis_x0,cy0},{vis_x1,cy1}, fill, 2.f);
                ImU32 border;
                if (rec_live) {
                    float t  = (float)ImGui::GetTime();
                    int   a  = (int)(170.f + 85.f * sinf(t * 6.f));
                    border = IM_COL32(255, 70, 70, a);
                } else {
                    border = sel ? IM_COL32(205, 205, 240, 255)
                                 : IM_COL32( 80,  80, 105, 200);
                }
                dl->AddRect({vis_x0,cy0},{vis_x1,cy1}, border, 2.f, 0, rec_live ? 2.f : 1.f);

                ImGui::PushClipRect({vis_x0,cy0},{vis_x1,cy1},true);
                float mid  = (cy0 + cy1) * 0.5f;
                float half = (cy1 - cy0) * 0.44f;
                if (rec_live) {
                    // Live waveform of the pass in progress, growing left→right.
                    int n = (int)fminf(fmaxf(cx1 - cx0, 16.f), 4096.f);
                    static std::vector<float> peaks;
                    peaks.resize((size_t)n);
                    if (recorder_live_peaks(n, peaks.data())) {
                        ImU32 wcol = IM_COL32(255, 130, 130, 230);
                        for (float px2 = vis_x0; px2 < vis_x1; px2 += 1.f) {
                            int b = (int)((px2 - cx0) / (cx1 - cx0) * (float)n);
                            if (b < 0 || b >= n) continue;
                            if (peaks[(size_t)b] <= 0.f) continue;  // not reached yet — stay flat
                            float amp = fmaxf(peaks[(size_t)b] * half, 1.f);
                            dl->AddLine({px2, mid-amp}, {px2, mid+amp}, wcol);
                        }
                    }
                } else if (has_take) {
                    // Take waveform. Takes record at speed 1, but in_point is
                    // honored so a left-trim reveals/hides take content (the
                    // waveform stays anchored to the timeline) — same mapping as
                    // a plain Audio clip, not a slide of the whole take.
                    const WaveformData* wd = waveform_get(clip.rec_takes[clip.rec_take_sel]);
                    if (wd && !wd->samples.empty()) {
                        ImU32 wcol = sel ? IM_COL32(25, 25, 45, 190)
                                         : IM_COL32(190, 190, 220, 130);
                        for (float px2 = vis_x0; px2 < vis_x1; px2 += 1.f) {
                            float t_src = clip.in_point + (px2 - cx0) / zoom;
                            int   fi    = (int)(t_src * WAVEFORM_FPS);
                            if (fi < 0 || fi >= (int)wd->samples.size()) continue;
                            float amp = wd->samples[fi] * half;
                            if (amp < 1.f) amp = 1.f;
                            dl->AddLine({px2, mid-amp}, {px2, mid+amp}, wcol);
                        }
                    }
                }
                // Red dot, top-left — the record-brick type marker. Brighter
                // (and the brick itself red) while recording.
                dl->AddCircleFilled({vis_x0 + 8.f, cy0 + 8.f}, 3.f,
                    rec_live ? IM_COL32(255, 90, 90, 255) : IM_COL32(225, 55, 55, 255));
                // Label badge, right of the dot.
                char lbl[48];
                if (rec_live)
                    snprintf(lbl, sizeof(lbl), "Take %d", recorder_take_count() + 1);
                else if (has_take)
                    snprintf(lbl, sizeof(lbl), "Take %d", clip.rec_take_sel + 1);
                else if (!clip.rec_takes.empty())
                    snprintf(lbl, sizeof(lbl), "No take selected");
                else
                    snprintf(lbl, sizeof(lbl), "Empty audio record brick");
                ImU32 lcol = rec_live ? IM_COL32(255, 220, 220, 255)
                           : sel      ? IM_COL32( 25,  25,  45, 255)
                                      : IM_COL32(195, 195, 220, 220);
                dl->AddText({vis_x0 + 15.f, cy0 + 3.f}, lcol, lbl);
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

            // Hold-to-weld ring: a dragged FX brick is parked over this content
            // clip and will couple onto it when the dwell completes — same
            // visual language as the FX-merge ring drawn on brick targets.
            if (g_tl.drag_couple_ti == ti && g_tl.drag_couple_ci == ci) {
                float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 6.f);
                ImU32 mc = IM_COL32(80, 255, 220, (int)(120 + 80 * pulse));
                dl->AddRect({vis_x0, cy0}, {vis_x1, cy1}, mc, 2.f, 0, 2.5f);
                draw_fx_weld_ring(dl, vis_x0, vis_x1, cy0, cy1,
                                  IM_COL32(80, 255, 220, 230));
            }

            // Capture geometry for IPC-driven testing.
            {
                TLGeomClip g; g.track = ti; g.clip = ci;
                g.x0 = vis_x0; g.y0 = cy0; g.x1 = vis_x1; g.y1 = cy1;
                g.has_fx = !is_fx_clip(clip) && host_fx_count(ti, ci) > 0;
                g.expanded = clip.fx_expanded;
                g.disc_cx = vis_x0 + 5.f + 4.5f; g.disc_cy = cy1 - 9.f - 1.f + 4.5f;
                s_tl_geom.clips.push_back(g);
            }

            // FX disclosure chevron is drawn later (Pass 2.5) so it lands on top
            // of the coupled glass FX brick instead of being painted over by it.

            clip_interact(ci, clip, vis_x0, vis_x1, cy0, cy1, sel);
        }

        // ── Pass 2: FX / MultiFX / BodyFX bricks — rendered on top, interact last ──
        for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
            Clip& clip = track.clips[ci];
            bool is_mfx = (clip.clip_type == ClipType::MultiFX);
            bool is_bfx = (clip.clip_type == ClipType::BodyFX);
            bool is_amfx = (clip.clip_type == ClipType::AudioMultiFX);
            if (clip.clip_type != ClipType::Effect && !is_mfx && !is_bfx && !is_amfx)
                continue;
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
                if (g_tl.drag_merge_ti == ti && g_tl.drag_merge_ci == ci) {
                    float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 6.f);
                    ImU32 mc = IM_COL32(80, 255, 220, (int)(120 + 80 * pulse));
                    dl->AddRect({vis_x0, cy0}, {vis_x1, cy1}, mc, 2.f, 0, 2.5f);
                    draw_fx_weld_ring(dl, vis_x0, vis_x1, cy0, cy1,
                                      IM_COL32(80, 255, 220, 230));
                }
                continue;
            }

            bool is_glass = fx_clip_is_glass(state, ti, clip);
            FxBrickColors fbc = (is_mfx || is_amfx)
                                       ? fx_brick_colors(is_amfx ? FXType::AudioAutotune
                                                                 : FXType::Glitch, sel)
                                       : fx_brick_colors(clip.fx_type, sel);

            // A clip can carry BOTH a video glass chain and an audio glass chain
            // (they never weld). If so, split the row so they don't draw on top
            // of each other — video on the top half, audio on the bottom — each
            // tinted by kind and independently clickable/draggable.
            if (is_glass) {
                int  host_ci    = fx_coupled_host(state, ti, clip);
                bool this_audio = fx_brick_is_audio_kind(clip);
                bool other_kind = false;
                for (int oc = 0; host_ci >= 0 && oc < (int)track.clips.size(); ++oc) {
                    if (oc == ci) continue;
                    const Clip& o = track.clips[(size_t)oc];
                    if (!o.fx_coupled || !is_fx_clip(o)) continue;
                    if (fx_coupled_host(state, ti, o) != host_ci) continue;
                    if (fx_brick_is_audio_kind(o) != this_audio) { other_kind = true; break; }
                }
                if (other_kind) {
                    float mid = (cy0 + cy1) * 0.5f;
                    if (this_audio) cy0 = mid + 1.f;   // audio chain → bottom half
                    else            cy1 = mid - 1.f;   // video chain → top half
                }
            }

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
                        float gty = track_top_at(gti);
                        if (gty+TL_TRACK_H < track_area_top || gty > track_area_bot) continue;
                        dl->AddRectFilled({gx,gty},{gx+2.f,gty+TL_TRACK_H},
                                          IM_COL32(160,110,255,80));
                    }
                }
            }

            ImGui::PushClipRect({vis_x0,cy0},{vis_x1,cy1},true);
            float ly = cy0 + (cy1-cy0-13.f)*0.5f;
            ImU32 lbl_col = is_glass ? IM_COL32(200, 240, 255, 255) : fbc.label;
            bool lane_grabbed = false;
            if (is_mfx || is_amfx) {
                // The FX chain as a stack that morphs with zoom: a frosted
                // deck of cards when the brick is narrow (depth = count),
                // unfolding into per-FX timing lanes as it widens (lane
                // length = that effect's active span). See draw_fx_stack.
                draw_fx_stack(dl, clip, cx0, vis_x0, vis_x1, cy0, cy1, zoom, sel);
                // Pass 2: once the lanes are unfolded, each one is a draggable
                // timing handle — left/right edge retimes that effect's span,
                // body shifts the whole window. (Deck mode stays click-to-select.)
                int shown; float spread;
                fx_stack_layout(clip, vis_x0, vis_x1, shown, spread);
                bool lane_mode = spread >= 0.6f && g_tl.fx_lane_drag == 0 &&
                                 !track.locked;
                if (lane_mode && mouse.x >= vis_x0 && mouse.x <= vis_x1 &&
                    mouse.y >= cy0 && mouse.y <= cy1) {
                    const float etol = 5.f;
                    for (int i = 0; i < shown; ++i) {
                        FxLaneRect r = fx_lane_rect(clip, i, shown, spread, cx0,
                                                    vis_x0, vis_x1, cy0, cy1, zoom);
                        if (mouse.y < r.y0 - 1.f || mouse.y > r.y1 + 1.f) continue;
                        bool can_l = (r.x1 - r.x0) > 2.f*etol;
                        bool hov_l = can_l && fabsf(mouse.x - r.x0) <= etol;
                        bool hov_r = can_l && fabsf(mouse.x - r.x1) <= etol && !hov_l;
                        bool hov_b = mouse.x >= r.x0 && mouse.x <= r.x1 && !hov_l && !hov_r;
                        if (!hov_l && !hov_r && !hov_b) continue;
                        // Hover affordance: brighten the lane + its grabbed edge.
                        dl->AddRect({r.x0,r.y0},{r.x1,r.y1}, IM_COL32(255,255,255,235), 2.f, 0, 1.5f);
                        if (hov_l) dl->AddLine({r.x0,r.y0},{r.x0,r.y1}, IM_COL32(255,255,255,255), 2.f);
                        if (hov_r) dl->AddLine({r.x1,r.y0},{r.x1,r.y1}, IM_COL32(255,255,255,255), 2.f);
                        ImGui::SetMouseCursor((hov_l||hov_r) ? ImGuiMouseCursor_ResizeEW
                                                             : ImGuiMouseCursor_Hand);
                        if (!tl_any_popup && ImGui::IsMouseClicked(0)) {
                            g_tl.fx_lane_drag = hov_l ? 1 : hov_r ? 2 : 3;
                            g_tl.fx_lane_ti = ti; g_tl.fx_lane_ci = ci; g_tl.fx_lane_idx = i;
                            g_tl.fx_lane_ref_x = mouse.x;
                            g_tl.fx_lane_ref_s = r.rel_s; g_tl.fx_lane_ref_e = r.rel_e;
                            g_tl.fx_lane_moved = false;
                            drag_track = -1; drag_clip = -1; drag_left = false; drag_right = false;
                            s_clip_hit = true;   // keep the empty-track deselect from firing
                            lane_grabbed = true;
                        }
                        break;
                    }
                    // Clicked the unfolded brick but missed every lane (e.g. the
                    // empty region of a retimed lane). Select the host so its FX
                    // panel opens — but LEAVE the content's body drag (armed by
                    // Pass 1 beneath the glass) intact: a drag from here moves the
                    // whole welded group (the coupled bricks follow their host),
                    // a click just selects. We only swallow Pass 2's own handling.
                    if (is_glass && !lane_grabbed && !tl_any_popup && ImGui::IsMouseClicked(0)) {
                        int hostsel = ci;
                        if (clip.fx_coupled && is_fx_clip(clip)) {
                            int host = fx_coupled_host(state, ti, clip);
                            if (host >= 0) hostsel = host;
                        }
                        state.selected_track = ti;
                        state.selected_clip  = hostsel;
                        state.clip_selection.clear();
                        state.clip_selection.insert({ti, hostsel});
                        s_clip_hit   = true;
                        lane_grabbed = true;
                    }
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

            if (!lane_grabbed)
                clip_interact(ci, clip, vis_x0, vis_x1, cy0, cy1, sel);
            if (g_tl.drag_merge_ti == ti && g_tl.drag_merge_ci == ci) {
                float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 6.f);
                ImU32 mc = IM_COL32(255, 180, 60, (int)(130 + 80 * pulse));
                dl->AddRect({vis_x0, cy0}, {vis_x1, cy1}, mc, 2.f, 0, 2.5f);
                draw_fx_weld_ring(dl, vis_x0, vis_x1, cy0, cy1,
                                  IM_COL32(255, 180, 60, 230));
            }
            // Pending content-coupling: ring fills on the brick about to
            // become (or merge into) the host's Video Multi-FX chain.
            if (s_couple_pend.ti == ti && s_couple_pend.ci == ci) {
                float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 6.f);
                ImU32 cc = IM_COL32(120, 200, 255, (int)(110 + 80 * pulse));
                dl->AddRect({vis_x0, cy0}, {vis_x1, cy1}, cc, 2.f, 0, 2.f);
                float prog = (float)((ImGui::GetTime() - s_couple_pend.t0) / kWeldHoldSec);
                if (prog > 0.f && prog < 1.f) {
                    ImVec2 cctr{(vis_x0 + vis_x1) * 0.5f, (cy0 + cy1) * 0.5f};
                    float pr = 10.f * (1.f + 0.1f * sinf((float)ImGui::GetTime() * 9.f));
                    dl->PathArcTo(cctr, pr, -IM_PI * 0.5f,
                                  -IM_PI * 0.5f + prog * 2.f * IM_PI, 24);
                    dl->PathStroke(IM_COL32(120, 200, 255, 235), 0, 2.f);
                }
            }
            // Weld-complete flash (the merged brick is always MultiFX, so it
            // renders through this path, never the BodyFX one).
            draw_fx_merge_flash(dl, ti, ci, vis_x0, vis_x1, cy0, cy1);
        }

        // ── Pass 2.5: FX disclosure chevron — on top of the coupled glass brick ──
        // A plain white chevron at a content clip's bottom-left that toggles the
        // per-FX timing lanes beneath it. Drawn after Pass 2 so the glass FX
        // brick riding on the clip doesn't paint over it (the original Pass-1
        // chevron was invisible — there was no button to expand the FX). Right
        // chevron = "click to open", down chevron = "open". The click is handled
        // here too, so it sits above the glass brick's own hit-testing.
        for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
            Clip& clip = track.clips[(size_t)ci];
            if (is_fx_clip(clip) || host_fx_count(ti, ci) <= 0) continue;
            float cx0 = origin.x+TL_LABEL_W+clip.start*zoom-scroll;
            float cx1 = origin.x+TL_LABEL_W+clip.end*zoom-scroll;
            if (cx1 < origin.x+TL_LABEL_W || cx0 > origin.x+total_w) continue;
            float vis_x0 = fmaxf(cx0, origin.x+TL_LABEL_W);
            float cy1 = track_y+TL_TRACK_H-3.f;
            if (cy1 > track_area_bot || track_y < track_area_top) continue;
            float cx = vis_x0 + 9.f, cy = cy1 - 6.f;
            bool thov = !tl_any_popup && mouse_in_track_band &&
                        mouse.x >= cx - 7.f && mouse.x <= cx + 7.f &&
                        mouse.y >= cy - 7.f && mouse.y <= cy + 7.f;
            // A small dark pad behind the glyph so it reads on any glass tint.
            dl->AddRectFilled({cx - 7.f, cy - 7.f}, {cx + 7.f, cy + 7.f},
                              IM_COL32(0, 0, 0, thov ? 150 : 90), 3.f);
            ImU32 tcol = thov ? IM_COL32(255, 255, 255, 255)
                              : IM_COL32(235, 240, 248, 235);
            if (clip.fx_expanded) {
                dl->AddLine({cx - 4.f, cy - 2.f}, {cx, cy + 3.f}, tcol, 2.f);
                dl->AddLine({cx, cy + 3.f}, {cx + 4.f, cy - 2.f}, tcol, 2.f);
            } else {
                dl->AddLine({cx - 2.f, cy - 4.f}, {cx + 3.f, cy}, tcol, 2.f);
                dl->AddLine({cx + 3.f, cy}, {cx - 2.f, cy + 4.f}, tcol, 2.f);
            }
            if (thov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (thov && !track.locked && ImGui::IsMouseClicked(0)) {
                clip.fx_expanded = !clip.fx_expanded;
                s_clip_hit = true;   // don't let it also deselect/drag
            }
        }

        // Left-click empty track body (no clip hit) — deselect
        if (!s_clip_hit && (!tl_any_popup && ImGui::IsMouseClicked(0)) && mouse_in_track_band &&
            mouse.y >= track_y && mouse.y < track_y+TL_TRACK_H &&
            mouse.x > origin.x+TL_LABEL_W && mouse.x < origin.x+total_w) {
            state.clip_selection.clear();
            state.selected_track = -1;
            state.selected_clip  = -1;
        }

        // Right-click empty timeline area (this track row, no clip hit)
        if (!clip_ctx_opened_this_frame && (!tl_any_popup && ImGui::IsMouseClicked(1)) &&
            mouse_in_track_band &&
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
                                      : (a.transition_type == TransitionType::Shake)     ? "Shake"
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
                    // Left handle → adjust pre (drag left = more pre). Frame-snap
                    // the duration so the crossfade spans whole frames.
                    float new_pre = fmaxf(0.01f, fminf(s_glass_drag_ref_pre - dx,
                                                       a.end - a.start - 0.05f));
                    a.transition_pre = snap_to_frame(state, new_pre);
                } else if (s_glass_drag == 2) {
                    // Right handle → adjust post
                    float new_post = fmaxf(0.01f, fminf(s_glass_drag_ref_post + dx,
                                                        b_ptr ? (b_ptr->end - b_ptr->start - 0.05f) : 10.f));
                    a.transition_post = snap_to_frame(state, new_post);
                } else {
                    // Body drag → move linked pair (frame-snapped)
                    float new_start = snap_to_frame(state, fmaxf(0.f, s_glass_drag_ref_start + dx));
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

        // FX timing-lane drag update (glass-brick Pass 2) — runs once/frame for
        // the brick whose lane was grabbed. Edits rel_start/rel_end in the
        // sub-effect; snaps the moving edge to timeline edges, beats, the
        // playhead and sibling-FX boundaries.
        if (g_tl.fx_lane_drag != 0 && g_tl.fx_lane_ti == ti &&
            g_tl.fx_lane_ci >= 0 && g_tl.fx_lane_ci < (int)track.clips.size()) {
            Clip& bclip = track.clips[g_tl.fx_lane_ci];
            int idx = g_tl.fx_lane_idx;
            if (idx >= 0 && idx < (int)bclip.fx_chain.size()) {
                Clip& se = bclip.fx_chain[(size_t)idx];
                float dur = bclip.end - bclip.start;
                const float MINLEN = 0.05f;
                if (ImGui::IsMouseDragging(0)) {
                    g_tl.fx_lane_moved = true;
                    float dt = (mouse.x - g_tl.fx_lane_ref_x) / zoom;
                    auto cands = build_snap_candidates(g_tl.fx_lane_ti, g_tl.fx_lane_ci);
                    for (int k = 0; k < (int)bclip.fx_chain.size(); ++k) {
                        if (k == idx) continue;
                        const Clip& o = bclip.fx_chain[(size_t)k];
                        cands.push_back(bclip.start + o.rel_start);
                        cands.push_back(bclip.start + (o.rel_end > 0.f ? o.rel_end : dur));
                    }
                    // Edge-snap to clip/sibling edges, then quantize the absolute
                    // time to the frame grid so the FX window starts/ends on a
                    // frame (brick.start is frame-aligned, so rel becomes a frame
                    // multiple) — same rule as the playhead, markers and loop.
                    if (g_tl.fx_lane_drag == 1) {            // left edge → rel_start
                        float abs_s = snap_to_frame(state, edge_snap(bclip.start + g_tl.fx_lane_ref_s + dt, cands));
                        se.rel_start = fmaxf(0.f, fminf(abs_s - bclip.start,
                                                        g_tl.fx_lane_ref_e - MINLEN));
                        se.rel_end   = g_tl.fx_lane_ref_e;
                    } else if (g_tl.fx_lane_drag == 2) {    // right edge → rel_end
                        float abs_e = snap_to_frame(state, edge_snap(bclip.start + g_tl.fx_lane_ref_e + dt, cands));
                        se.rel_start = g_tl.fx_lane_ref_s;
                        se.rel_end   = fminf(dur, fmaxf(abs_e - bclip.start,
                                                        g_tl.fx_lane_ref_s + MINLEN));
                    } else {                                 // body → shift window
                        float len = g_tl.fx_lane_ref_e - g_tl.fx_lane_ref_s;
                        float abs_s = snap_to_frame(state, edge_snap(bclip.start + g_tl.fx_lane_ref_s + dt, cands));
                        float rel_s = fmaxf(0.f, fminf(abs_s - bclip.start, dur - len));
                        se.rel_start = rel_s;
                        se.rel_end   = rel_s + len;
                    }
                }
                if (ImGui::IsMouseReleased(0)) {
                    if (g_tl.fx_lane_moved) {
                        // Full-host window collapses back to the "always on" encoding.
                        if (se.rel_start <= 0.001f && se.rel_end >= dur - 0.001f) {
                            se.rel_start = 0.f; se.rel_end = 0.f;
                        }
                        history_push(state, "Retime FX");
                    } else {
                        // Plain click (no drag): open this entry's keyframable
                        // sliders. Coupled chains render through the host content
                        // clip's FX tab, so select the HOST and request that view
                        // with the clicked entry active — not the brick directly,
                        // which surfaces an empty Multi-FX clip panel.
                        if (g_tl.fx_lane_idx >= 0 &&
                            g_tl.fx_lane_idx < (int)bclip.fx_chain.size())
                            bclip.fx_chain_selected = g_tl.fx_lane_idx;
                        int host = fx_coupled_host(state, ti, bclip);
                        int sel  = host >= 0 ? host : g_tl.fx_lane_ci;
                        state.selected_track = ti;
                        state.selected_clip  = sel;
                        state.clip_selection.clear();
                        state.clip_selection.insert({ti, sel});
                        request_panel_view(fx_brick_is_audio_kind(bclip)
                            ? PanelView::HostAudioFX : PanelView::HostFX);
                    }
                    g_tl.fx_lane_drag = 0;
                    g_tl.fx_lane_ti = g_tl.fx_lane_ci = g_tl.fx_lane_idx = -1;
                    g_tl.snap_indicator = -1.f;
                }
            } else {
                g_tl.fx_lane_drag = 0;
                g_tl.fx_lane_ti = g_tl.fx_lane_ci = g_tl.fx_lane_idx = -1;
            }
        }

        // ── Pass 3: expanded FX timing lanes beneath the clip ─────────────────
        // One bar per coupled effect (video chains first, then audio) showing how
        // long that effect runs on the clip. Drawn in the extra height the track
        // grew by; the clip body above is unchanged.
        for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
            Clip& host = track.clips[(size_t)ci];
            if (is_fx_clip(host) || !host.fx_expanded) continue;
            std::vector<int> fxk = coupled_fx_of(state, ti, ci);
            if (fxk.empty()) continue;
            float lane_top = track_y + TL_TRACK_H + 3.f;
            int row = 0;

            // Collapse handle — a plain white chevron in the label gutter, lined
            // up with the lane stack, so there's an obvious spot to fold the lanes
            // back from where the lanes actually are. (The clip-body disclosure
            // chevron opens them but ends up under the glass brick once expanded,
            // so the collapse control lives with the lanes instead.) Down-chevron
            // = "click to fold"; no filled block, just the glyph.
            {
                int   n_lanes = host_fx_count(ti, ci);
                float stack_h = (float)n_lanes * FX_LANE_H;
                float cxm = origin.x + TL_LABEL_W - 11.f;
                float gy0 = lane_top, gy1 = lane_top + stack_h - 3.f;
                float cym = (gy0 + gy1) * 0.5f;
                if (gy1 > gy0 + 5.f && gy1 <= track_area_bot && gy0 >= track_area_top) {
                    bool ghov = !tl_any_popup &&
                                mouse.x >= cxm - 8.f && mouse.x <= cxm + 8.f &&
                                mouse.y >= gy0 && mouse.y <= gy1;
                    float w = 5.f, h = 3.f;
                    ImU32 chc = ghov ? IM_COL32(255, 255, 255, 255)
                                     : IM_COL32(220, 226, 235, 220);
                    // down chevron (collapse)
                    dl->AddLine({cxm - w, cym - h}, {cxm, cym + h}, chc, 2.f);
                    dl->AddLine({cxm, cym + h}, {cxm + w, cym - h}, chc, 2.f);
                    if (ghov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (ghov && !track.locked && ImGui::IsMouseClicked(0)) {
                        host.fx_expanded = false;
                        s_clip_hit = true;
                        continue;   // host folded — skip drawing its lanes this frame
                    }
                }
            }
            // k = brick clip index, ei = entry index in the brick's chain (-1 for
            // a single brick). The edges resize the effect's rel window; clicking
            // selects the effect so its keyframable sliders open in the panel.
            auto draw_lane = [&](int k, int ei, FXType ft, float rs, float re) {
                Clip& brick = track.clips[(size_t)k];
                float ax0 = origin.x + TL_LABEL_W + (brick.start + rs) * zoom - scroll;
                float ax1 = origin.x + TL_LABEL_W + (brick.start + re) * zoom - scroll;
                float vx0 = fmaxf(ax0, origin.x + TL_LABEL_W);
                float vx1 = fminf(ax1, origin.x + total_w);
                if (vx1 < vx0 + 3.f) vx1 = vx0 + 3.f;
                float ly0 = lane_top + row * FX_LANE_H, ly1 = ly0 + FX_LANE_H - 2.f;
                // The lane reads as "selected" whenever its entry is the brick's
                // active chain entry and the panel is inspecting this lane group —
                // which means either the host content clip (FX-tab path) or the
                // brick itself is the current selection.
                bool selr = state.selected_track == ti &&
                            (state.selected_clip == ci || state.selected_clip == k) &&
                            brick.fx_chain_selected == ei;
                if (ly1 <= track_area_bot && ly0 >= track_area_top) {
                    FxBrickColors c = fx_brick_colors(ft, selr);
                    dl->AddRectFilled({vx0, ly0}, {vx1, ly1},
                                      (c.fill & 0x00FFFFFFu) | (selr ? 0xF0000000u : 0xC8000000u), 2.f);
                    dl->AddRect({vx0, ly0}, {vx1, ly1}, c.border, 2.f, 0, selr ? 1.8f : 1.f);
                    ImGui::PushClipRect({vx0, ly0}, {vx1, ly1}, true);
                    dl->AddText({vx0 + 4.f, ly0 + 1.f}, IM_COL32(235, 240, 250, 255),
                                fx_type_name(ft));
                    ImGui::PopClipRect();
                    TLGeomLane gl; gl.track = ti; gl.clip = ci; gl.idx = row;
                    gl.x0 = vx0; gl.y0 = ly0; gl.x1 = vx1; gl.y1 = ly1;
                    gl.audio = fx_brick_is_audio_kind(brick);
                    s_tl_geom.lanes.push_back(gl);

                    // Resize / select interaction (chain entries only).
                    if (ei >= 0 && !track.locked && g_tl.fx_lane_drag == 0 &&
                        !tl_any_popup &&
                        mouse.x >= vx0 - 4.f && mouse.x <= vx1 + 4.f &&
                        mouse.y >= ly0 && mouse.y <= ly1) {
                        const float etol = 4.f;
                        bool can_l = (vx1 - vx0) > 2.f * etol;
                        bool hov_l = can_l && fabsf(mouse.x - vx0) <= etol;
                        bool hov_r = can_l && fabsf(mouse.x - vx1) <= etol && !hov_l;
                        ImGui::SetMouseCursor((hov_l || hov_r) ? ImGuiMouseCursor_ResizeEW
                                                               : ImGuiMouseCursor_Hand);
                        if (hov_l) dl->AddLine({vx0,ly0},{vx0,ly1}, IM_COL32(255,255,255,255), 2.f);
                        if (hov_r) dl->AddLine({vx1,ly0},{vx1,ly1}, IM_COL32(255,255,255,255), 2.f);
                        if (ImGui::IsMouseClicked(0)) {
                            g_tl.fx_lane_drag = hov_l ? 1 : hov_r ? 2 : 3;
                            g_tl.fx_lane_ti = ti; g_tl.fx_lane_ci = k; g_tl.fx_lane_idx = ei;
                            g_tl.fx_lane_ref_x = mouse.x;
                            g_tl.fx_lane_ref_s = rs; g_tl.fx_lane_ref_e = re;
                            g_tl.fx_lane_moved = false;
                            // Select the HOST content clip and open its FX tab with
                            // this entry active, so the entry's keyframable sliders
                            // appear (coupled chains render via the host's FX tab,
                            // not by selecting the brick directly).
                            state.selected_track = ti; state.selected_clip = ci;
                            state.clip_selection.clear();
                            state.clip_selection.insert({ti, ci});
                            brick.fx_chain_selected = ei;
                            request_panel_view(fx_brick_is_audio_kind(brick)
                                ? PanelView::HostAudioFX : PanelView::HostFX);
                            s_clip_hit = true;
                        }
                    }
                }
                row++;
            };
            for (int pass = 0; pass < 2; ++pass) {       // 0 = video, 1 = audio
                for (int k : fxk) {
                    Clip& brick = track.clips[(size_t)k];
                    if ((fx_brick_is_audio_kind(brick) ? 1 : 0) != pass) continue;
                    float bdur = brick.end - brick.start;
                    if (brick.clip_type == ClipType::MultiFX ||
                        brick.clip_type == ClipType::AudioMultiFX) {
                        for (int ei = 0; ei < (int)brick.fx_chain.size(); ++ei) {
                            auto& se = brick.fx_chain[(size_t)ei];
                            draw_lane(k, ei, se.fx_type, se.rel_start,
                                      se.rel_end > 0.f ? se.rel_end : bdur);
                        }
                    } else {
                        draw_lane(k, -1, brick.fx_type, 0.f, bdur);
                    }
                }
            }
        }

        // New-brick glow — drawn on top of this track's clips (content AND FX),
        // for any brick clip_flash() marked. Recompute the rect like the passes.
        for (int gci = 0; gci < (int)track.clips.size(); ++gci) {
            Clip& gc = track.clips[gci];
            if (gc.glow_start == 0.0) continue;            // idle — cheap skip
            float gcx0 = origin.x+TL_LABEL_W+gc.start*zoom-scroll;
            float gcx1 = origin.x+TL_LABEL_W+gc.end*zoom-scroll;
            if (gcx1 < origin.x+TL_LABEL_W || gcx0 > origin.x+total_w) continue;
            draw_added_glow(gc, fmaxf(gcx0, origin.x+TL_LABEL_W), track_y+3.f,
                            fminf(gcx1, origin.x+total_w), track_y+TL_TRACK_H-3.f);
        }

        track_y += track_h(ti);   // variable: grows when a clip's FX are expanded
    }

    // ── "Affects-below" influence wash ────────────────────────────────────────
    // Bus bricks and standalone (uncoupled) FX/MultiFX bricks process the
    // composite of the tracks beneath them. Paint a faint, span-gated tint over
    // that zone so the downward influence is legible at a glance — brighter when
    // the brick is selected or hovered. A bus's scope stops at the next bus
    // brick below it (mirrors the audio submix bound); an FX brick's runs to the
    // bottom. The tint is drawn over the lanes below, never over the brick row.
    {
        const float clip_l = origin.x + TL_LABEL_W;
        const float clip_r = origin.x + total_w;
        const int   ntr    = (int)state.tracks.size();
        auto track_top_y = [&](int ti){ return track_top_at(ti); };
        auto affects_below = [](const Clip& c) -> bool {
            if (c.clip_type == ClipType::Bus) return true;
            return (c.clip_type == ClipType::Effect ||
                    c.clip_type == ClipType::MultiFX) && !c.fx_coupled;
        };
        // Exclusive lower track bound of a brick's influence.
        auto scope_end_track = [&](int ti, const Clip& b) -> int {
            if (b.clip_type == ClipType::Bus) {
                for (int tj = ti + 1; tj < ntr; ++tj)
                    for (auto& c : state.tracks[tj].clips)
                        if (c.clip_type == ClipType::Bus &&
                            c.start < b.end && c.end > b.start)
                            return tj;   // wash stops above the next bus
            }
            return ntr;                  // FX bricks reach the bottom
        };
        auto brick_rgb = [](const Clip& c) -> ImU32 {
            if (c.clip_type == ClipType::Bus) return IM_COL32(90, 200, 165, 255);
            return fx_brick_colors(c.fx_type, false).border;
        };

        for (int ti = 0; ti < ntr; ++ti) {
            for (int ci = 0; ci < (int)state.tracks[ti].clips.size(); ++ci) {
                const Clip& b = state.tracks[ti].clips[ci];
                if (!affects_below(b)) continue;
                int te = scope_end_track(ti, b);     // exclusive lower bound
                if (te <= ti + 1) continue;          // nothing below in scope

                float zx0 = fmaxf(clip_l, clip_l + b.start * zoom - scroll);
                float zx1 = fminf(clip_r, clip_l + b.end   * zoom - scroll);
                if (zx1 <= zx0) continue;
                float zy0 = fmaxf(track_area_top, track_top_y(ti + 1));
                float zy1 = fminf(track_area_bot, track_top_y(te));
                if (zy1 <= zy0) continue;

                bool sel = state.selected_track == ti && state.selected_clip == ci;
                float by0 = track_top_y(ti);
                bool hov = mouse.x >= zx0 && mouse.x <= zx1 &&
                           mouse.y >= by0 && mouse.y <= by0 + TL_TRACK_H;
                bool hot = sel || hov;

                ImU32 rgb  = brick_rgb(b) & 0x00FFFFFFu;
                dl->AddRectFilled({zx0, zy0}, {zx1, zy1},
                                  rgb | ((hot ? 34u : 16u) << 24));
                // Accent edge at the top of the zone — influence emanates down.
                dl->AddRectFilled({zx0, zy0}, {zx1, zy0 + (hot ? 2.f : 1.f)},
                                  rgb | ((hot ? 200u : 90u) << 24));
            }
        }
    }

    // "+ Add Track" — scrolls with the track list as the last row
    {
        ImVec2 row_tl = {origin.x,           track_y};
        ImVec2 row_br = {origin.x + total_w,  track_y + TL_TRACK_H};
        bool add_hov       = mouse_in_track_band &&
                             mouse.y >= track_y && mouse.y < track_y + TL_TRACK_H &&
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
                // Insert gap = the boundary nearest the cursor, with variable heights.
                int ins = 0; float y = track_area_top - state.tl_v_scroll;
                for (int k = 0; k < (int)state.tracks.size(); ++k) {
                    float h = track_h(k);
                    if (mouse.y < y + h * 0.5f) break;
                    ins = k + 1; y += h;
                }
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
        float ins_y = track_top_at(s_track_drag_insert);
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

        // Start box select when clicking empty body space (no clip was hit).
        // Gate on !IsAnyItemActive() — same as the label-deselect below — so
        // clicking a widget that lives in the timeline body (an on-brick slider,
        // an InvisibleButton) doesn't read as an empty-space click and deselect.
        if (lclick && in_body && !s_clip_hit && !ImGui::IsAnyItemActive()) {
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
                // Map screen-y → track index through the variable-height helper.
                int n_tr  = (int)state.tracks.size();
                int   tr0 = y_to_track(by0);
                int   tr1 = y_to_track(by1);
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

        // Escape clears selection (unless crop-edit mode owns Esc this frame)
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && s_rename_track < 0 &&
            state.crop_edit_track < 0) {
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
                {TransitionType::Shake,    "Shake"},
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
            // Scroll range includes the fit margin: fully scrolled right, the
            // last clip end sits margin-short of the border, same as the
            // zoomed-out fit and the trim-follow lead.
            float max_scroll   = fmaxf(1.f, tl_content_w - clip_area_w + fit_margin);
            float thumb_w      = fmaxf(20.f, sb_w * clip_area_w / (tl_content_w + fit_margin));
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

        // The dragged edge is locked inside the visible clip area minus the
        // fit margin: the margin line is a wall, and auto-scroll feeding
        // timeline under it is the only way the edge advances — so there is
        // always that much empty timeline visible ahead of the edge. The left
        // wall lifts when scrolled home so a left trim can still reach 0:00.
        const float vis_t_lo = (scroll > 0.5f) ? (scroll + fit_margin) / zoom : 0.f;
        const float vis_t_hi = (scroll + clip_area_w - fit_margin) / zoom;

        if (drag_left && !left_locked) {
            // Clamp to the visible window first, then snap (see the right-edge note)
            // so a left trim stays frame-aligned while auto-scrolling toward 0:00.
            float lt = fmaxf(vis_t_lo, fminf(new_t, vis_t_hi));
            float t = edge_snap(snap(lt), cands);
            bool still_img_l = dc.clip_type == ClipType::Video && is_image_path(dc.text);
            float src_floor = (src_dur > 0.f && !still_img_l)
                ? dc.start - dc.in_point / fmaxf(0.01f, dc.speed) : 0.f;
            float new_start = fmaxf(src_floor, fmaxf(0.f, fminf(t, dc.end - f1)));
            float d = new_start - dc.start;
            // in_point is in source seconds — scale the timeline delta by speed
            // (matches src_floor above, which divides by speed).
            dc.in_point = fmaxf(0.f, dc.in_point + d * fmaxf(0.01f, dc.speed));
            // Keys are relative to clip.start; shift them back so the
            // animation stays put on the timeline while the edge moves.
            clip_keys_shift(dc, -d);
            dc.start = new_start;
            sync_proxy_key();
        } else if (drag_right && !right_locked) {
            float et = (mouse.x - origin.x - TL_LABEL_W + scroll) / zoom;
            // Clamp to the visible window FIRST, then frame-snap, so the edge stays
            // frame-aligned even while auto-scroll feeds timeline under it. Clamping
            // AFTER the snap let vis_t_hi — a smooth function of the pixel-by-pixel
            // scroll — override the snap, so the edge slid smoothly instead of frame
            // to frame once auto-scroll kicked in.
            et = fmaxf(vis_t_lo, fminf(et, vis_t_hi));
            // When the playhead sits at this clip's end it's clamped to follow the
            // shrinking end (app.cpp). Snapping the edge TO the playhead then makes
            // the two chase each other and bounce — so drop it (and anything sitting
            // on it) from the candidate set while they're coupled.
            std::vector<float> rcands;
            const std::vector<float>* use = &cands;
            if (state.playhead >= dc.end - 1.5f * f1) {
                rcands.reserve(cands.size());
                for (float c : cands) if (fabsf(c - state.playhead) > 1e-4f) rcands.push_back(c);
                use = &rcands;
            }
            float t = edge_snap(snap(et), *use);
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
            for (int gi = 0; gi <= n_tracks; ++gi) {
                float boundary_y = track_top_at(gi);
                if (fabsf(mouse.y - boundary_y) < GAP_PX) {
                    drag_hot_gap = gi;
                    break;
                }
            }
            // Always assign hot track by midpoint — gap zone does not steal the target.
            {
                int hot = y_to_track(mouse.y);
                drag_hot_track = (hot >= 0 && hot <= n_tracks) ? hot : -1;
            }
            (void)track_origin_y;
            // Merge-target detection: dragged FX brick over another FX brick.
            // The target track follows the mouse row (drag_hot_track), so
            // dropping a brick from one track onto a brick on another welds
            // the same way as a same-track drop.
            g_tl.drag_merge_ti = -1;
            g_tl.drag_merge_ci = -1;
            g_tl.drag_couple_ti = -1;
            g_tl.drag_couple_ci = -1;
            if (!drag_left && !drag_right && drag_hot_gap < 0) {
                const Clip& dc_ref = state.tracks[drag_track].clips[drag_clip];
                int tt = (drag_hot_track >= 0 &&
                          drag_hot_track < (int)state.tracks.size())
                             ? drag_hot_track : drag_track;
                if (is_fx_clip(dc_ref)) {
                    for (int ci2 = 0; ci2 < (int)state.tracks[tt].clips.size(); ++ci2) {
                        if (tt == drag_track && ci2 == drag_clip) continue;
                        const Clip& oc = state.tracks[tt].clips[ci2];
                        if (!is_fx_clip(oc)) continue;
                        if (!fx_bricks_weldable(dc_ref, oc)) continue;
                        if (dc_ref.start < oc.end && dc_ref.end > oc.start) {
                            g_tl.drag_merge_ti = tt;
                            g_tl.drag_merge_ci = ci2;
                            break;
                        }
                    }
                }
                // No FX-brick to merge into? Hold-to-weld onto the bare hostable
                // content under the brick instead. Audio bricks effectively had
                // this already — their host usually carries a chain to merge into
                // — while bare video content didn't, forcing a precise drop. Pick
                // the best-overlap host on the mouse row (same rule as couple_fx_now).
                if (g_tl.drag_merge_ci < 0 && is_fx_clip(dc_ref) && !dc_ref.fx_coupled) {
                    bool audio_kind = fx_brick_is_audio_kind(dc_ref);
                    float best_ov = 0.05f; int best = -1;
                    for (int ci2 = 0; ci2 < (int)state.tracks[tt].clips.size(); ++ci2) {
                        if (tt == drag_track && ci2 == drag_clip) continue;
                        const Clip& hc = state.tracks[tt].clips[ci2];
                        bool hostable = audio_kind
                            ? (hc.clip_type == ClipType::Audio ||
                               hc.clip_type == ClipType::Record ||
                               hc.clip_type == ClipType::Video ||
                               hc.clip_type == ClipType::VideoRecord)
                            : (clip_is_videolike_type(hc.clip_type) ||
                               hc.clip_type == ClipType::Background);
                        if (!hostable) continue;
                        float ov = fminf(dc_ref.end, hc.end) - fmaxf(dc_ref.start, hc.start);
                        if (ov > best_ov) { best_ov = ov; best = ci2; }
                    }
                    if (best >= 0) { g_tl.drag_couple_ti = tt; g_tl.drag_couple_ci = best; }
                }
                // Multi-drags never weld (merge or couple).
                if (g_tl.drag_multi.size() > 1) {
                    g_tl.drag_merge_ti = -1;  g_tl.drag_merge_ci = -1;
                    g_tl.drag_couple_ti = -1; g_tl.drag_couple_ci = -1;
                }
                // Unified hold-to-weld: prefer an FX-brick merge, else a content
                // couple. Overlapping a target only arms the dwell timer (ring at
                // the target); the weld fires when it completes.
                bool couple = g_tl.drag_merge_ci < 0 && g_tl.drag_couple_ci >= 0;
                int wt_ti = couple ? g_tl.drag_couple_ti : g_tl.drag_merge_ti;
                int wt_ci = couple ? g_tl.drag_couple_ci : g_tl.drag_merge_ci;
                if (wt_ti != s_fx_weld.target_ti || wt_ci != s_fx_weld.target_ci) {
                    s_fx_weld.target_ti = wt_ti;   // new candidate
                    s_fx_weld.target_ci = wt_ci;
                    s_fx_weld.t0        = ImGui::GetTime();     // arm the timer
                }
                if (wt_ci >= 0 && ImGui::GetTime() - s_fx_weld.t0 >= kWeldHoldSec) {
                    int sel_ti, sel_ci;
                    if (couple) {
                        // Move the brick onto the host's track if needed, then
                        // couple — timeline_couple_fx_brick windows entries to the
                        // host span and absorbs into the host's existing chain.
                        int hci = wt_ci;
                        Clip brick = state.tracks[drag_track].clips[drag_clip];
                        state.tracks[drag_track].clips.erase(
                            state.tracks[drag_track].clips.begin() + drag_clip);
                        if (wt_ti == drag_track && hci > drag_clip) --hci;
                        state.tracks[wt_ti].clips.push_back(std::move(brick));
                        int bci = (int)state.tracks[wt_ti].clips.size() - 1;
                        sel_ci = timeline_couple_fx_brick(state, wt_ti, bci, hci);
                        sel_ti = wt_ti;
                        history_push(state, "Couple FX brick");
                    } else {
                        int tgt_ti = wt_ti, tgt_ci = wt_ci;
                        Clip dragged_copy = state.tracks[drag_track].clips[drag_clip];
                        merge_fx_clips(state.tracks[tgt_ti].clips[tgt_ci], dragged_copy);
                        state.tracks[drag_track].clips.erase(
                            state.tracks[drag_track].clips.begin() + drag_clip);
                        sel_ci = (tgt_ti == drag_track && tgt_ci > drag_clip)
                                     ? tgt_ci - 1 : tgt_ci;
                        sel_ti = tgt_ti;
                        history_push(state, "Merge FX bricks");
                    }
                    state.selected_track = sel_ti;
                    state.selected_clip  = sel_ci;
                    state.clip_selection.clear();
                    state.clip_selection.insert({sel_ti, sel_ci});
                    s_fx_flash.active = true;
                    s_fx_flash.t0     = ImGui::GetTime();
                    s_fx_flash.ti     = sel_ti;
                    s_fx_flash.ci     = sel_ci;
                    // End the drag — the welded brick stays selected, so
                    // re-grabbing it is one click.
                    drag_track = -1; drag_clip = -1;
                    drag_hot_track = -1; drag_hot_gap = -1;
                    g_tl.drag_merge_ti  = -1; g_tl.drag_merge_ci  = -1;
                    g_tl.drag_couple_ti = -1; g_tl.drag_couple_ci = -1;
                    s_fx_weld.target_ti = -1; s_fx_weld.target_ci = -1;
                    s_drag_moved = false;
                    s_body_snap_held_start = -1.f; s_body_snap_held_cand = -1.f;
                    g_tl.drag_multi.clear();
                }
            }
        }
    } else {
        s_snap_indicator = -1.f;
        g_tl.drag_merge_ti = -1;
        g_tl.drag_merge_ci = -1;
        g_tl.drag_couple_ti = -1;
        g_tl.drag_couple_ci = -1;
        s_fx_weld.target_ti = -1;
        s_fx_weld.target_ci = -1;
    }

    // Trim feedback: bright bar + inward arrow on the edge being dragged, and
    // a floating chip with the edge timecode (M:SS:FF) and resulting duration.
    if (drag_track >= 0 && drag_clip >= 0 && (drag_left || drag_right) &&
        s_drag_moved && drag_track < (int)state.tracks.size() &&
        drag_clip < (int)state.tracks[drag_track].clips.size()) {
        const Clip& dc = state.tracks[drag_track].clips[drag_clip];
        float edge_t = drag_left ? dc.start : dc.end;
        float ex = origin.x + TL_LABEL_W + edge_t * zoom - scroll;
        float ty = track_top_at(drag_track);
        float ecy0 = ty + 1.f, ecy1 = ty + TL_TRACK_H - 1.f;
        if (ex >= origin.x + TL_LABEL_W && ex <= origin.x + total_w) {
            dl->AddLine({ex, ecy0}, {ex, ecy1}, IM_COL32(255, 255, 255, 235), 2.5f);
            float adir = drag_left ? 1.f : -1.f;   // arrow points into the clip
            float amy  = (ecy0 + ecy1) * 0.5f;
            dl->AddTriangleFilled({ex + adir * 3.f, amy - 5.f},
                                  {ex + adir * 3.f, amy + 5.f},
                                  {ex + adir * 9.f, amy}, IM_COL32(255, 255, 255, 235));
        }
        int total_f = (int)roundf(edge_t * fps);
        int ifr     = (int)fmaxf(1.f, roundf(fps));
        int ff      = total_f % ifr, ssec = total_f / ifr;
        char chip[48];
        snprintf(chip, sizeof(chip), "%d:%02d:%02d  \xc2\xb7  %.2fs",
                 ssec / 60, ssec % 60, ff, dc.end - dc.start);
        ImVec2 csz = ImGui::CalcTextSize(chip);
        ImVec2 cp  = {mouse.x + 14.f, mouse.y - csz.y - 14.f};
        if (cp.x + csz.x + 12.f > origin.x + total_w) cp.x = mouse.x - csz.x - 26.f;
        dl->AddRectFilled({cp.x - 6.f, cp.y - 4.f},
                          {cp.x + csz.x + 6.f, cp.y + csz.y + 4.f},
                          IM_COL32(20, 20, 30, 235), 4.f);
        dl->AddRect({cp.x - 6.f, cp.y - 4.f},
                    {cp.x + csz.x + 6.f, cp.y + csz.y + 4.f},
                    IM_COL32(120, 120, 160, 200), 4.f);
        dl->AddText(cp, IM_COL32(230, 230, 245, 255), chip);
    }

    // ── Lyric brick body-drag (managed lyrics track) ─────────────────────────
    // Slides within its own track, clamped so it never overlaps a neighbor or
    // leaves the track. Neighbors are classified by the brick's ORIGINAL start
    // (fixed during the drag) so the clamp walls don't flip as it moves.
    if (s_lyric_dt >= 0 && s_lyric_dc >= 0 &&
        s_lyric_dt < (int)state.tracks.size() &&
        s_lyric_dc < (int)state.tracks[s_lyric_dt].clips.size()) {
        auto& lclips = state.tracks[s_lyric_dt].clips;
        Clip& ldc = lclips[s_lyric_dc];
        if (ImGui::IsMouseDragging(0)) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            s_drag_moved = true;
            float dur = s_lyric_o_end - s_lyric_o_start;
            float raw = (mouse.x - origin.x - TL_LABEL_W + scroll) / zoom - s_lyric_off;
            if (!ImGui::GetIO().KeyCtrl) raw = roundf(raw * fps) / fps;  // frame snap
            float lo = 0.f, hi = 1e9f;
            for (int k = 0; k < (int)lclips.size(); ++k) {
                if (k == s_lyric_dc) continue;
                if (lclips[k].start < s_lyric_o_start) lo = fmaxf(lo, lclips[k].end);
                else                                   hi = fminf(hi, lclips[k].start);
            }
            float ns = fmaxf(0.f, fmaxf(lo, fminf(raw, hi - dur)));
            ldc.start = ns; ldc.end = ns + dur;
        }
        if (ImGui::IsMouseReleased(0)) {
            float delta = ldc.start - s_lyric_o_start;
            if (fabsf(delta) > 0.5f / fps) {
                // Keep the per-word (karaoke) times in step with the moved brick.
                for (auto& w : ldc.words) { w.start += delta; w.end += delta; }
                history_push(state, "Move lyric");
            }
            s_lyric_dt = -1; s_lyric_dc = -1; s_drag_moved = false;
        }
    }

    if (ImGui::IsMouseReleased(0)) {
        if (drag_track >= 0 && drag_clip >= 0) {
            {
            // Alt+drop on a merge target (any track) = instant weld, skipping
            // the hold-to-weld dwell. Checked before the gap/cross-track
            // branches so a cross-track Alt drop merges instead of moving.
            if (!drag_left && !drag_right && g_tl.drag_merge_ci >= 0 &&
                ImGui::GetIO().KeyAlt) {
                int tgt_ti = g_tl.drag_merge_ti;
                int tgt_ci = g_tl.drag_merge_ci;
                Clip dragged_copy = state.tracks[drag_track].clips[drag_clip];
                merge_fx_clips(state.tracks[tgt_ti].clips[tgt_ci], dragged_copy);
                // Erase the dragged clip; adjust index if target sat after it
                state.tracks[drag_track].clips.erase(
                    state.tracks[drag_track].clips.begin() + drag_clip);
                int sel_ci = (tgt_ti == drag_track && tgt_ci > drag_clip)
                                 ? tgt_ci - 1 : tgt_ci;
                state.selected_track = tgt_ti;
                state.selected_clip  = sel_ci;
                state.clip_selection.clear();
                state.clip_selection.insert({tgt_ti, sel_ci});
                s_fx_flash.active = true;
                s_fx_flash.t0     = ImGui::GetTime();
                s_fx_flash.ti     = tgt_ti;
                s_fx_flash.ci     = sel_ci;
                history_push(state, "Merge FX bricks");
                goto drag_done;
            }
            if (!drag_left && !drag_right && drag_hot_gap >= 0) {
                // Drop into gap between tracks — insert new track there. The
                // host's coupled glass bricks travel with it.
                std::vector<Clip> grp = extract_host_group(state, drag_track, drag_clip);
                Track nt;
                char name[32];
                snprintf(name, sizeof(name), "Track %d", (int)state.tracks.size() + 1);
                nt.name = name;
                for (auto& c : grp) nt.clips.push_back(c);
                state.tracks.insert(state.tracks.begin() + drag_hot_gap, std::move(nt));
                state.selected_track = drag_hot_gap;
                state.selected_clip  = 0;
                state.clip_selection.clear();
                state.clip_selection.insert({state.selected_track, state.selected_clip});
                history_push(state, "Move clip to new track");
            } else if (!drag_left && !drag_right && s_drag_moved &&
                drag_hot_track >= 0 && drag_hot_track != drag_track) {
                std::vector<Clip> grp = extract_host_group(state, drag_track, drag_clip);
                Clip& moved = grp[0];
                if (drag_hot_track == (int)state.tracks.size()) {
                    // Drop below all tracks — create new track
                    Track nt;
                    char name[32];
                    snprintf(name, sizeof(name), "Track %d", (int)state.tracks.size() + 1);
                    nt.name = name;
                    for (auto& c : grp) nt.clips.push_back(c);
                    state.tracks.push_back(nt);
                    state.selected_track = (int)state.tracks.size() - 1;
                    state.selected_clip  = 0;
                    state.clip_selection.clear();
                    state.clip_selection.insert({state.selected_track, state.selected_clip});
                    history_push(state, "Move clip to new track");
                } else {
                    // Abort drop if the host conflicts with a clip on the target.
                    bool overlaps = false;
                    for (const Clip& oc : state.tracks[drag_hot_track].clips)
                        if (clips_conflict(moved, oc)) { overlaps = true; break; }
                    if (overlaps) {
                        // Put the whole group back on its original track.
                        moved.start = drag_origin_start;
                        moved.end   = drag_origin_end;
                        int at = drag_clip < (int)state.tracks[drag_track].clips.size()
                                 ? drag_clip : (int)state.tracks[drag_track].clips.size();
                        for (int gi = (int)grp.size()-1; gi >= 0; --gi)
                            state.tracks[drag_track].clips.insert(
                                state.tracks[drag_track].clips.begin() + at, grp[(size_t)gi]);
                        state.selected_track = drag_track;
                        state.selected_clip  = at;
                        state.clip_selection.clear();
                        state.clip_selection.insert({state.selected_track, state.selected_clip});
                    } else {
                        int base = (int)state.tracks[drag_hot_track].clips.size();
                        for (auto& c : grp) state.tracks[drag_hot_track].clips.push_back(c);
                        // Dropping an FX brick onto a track with hostable content
                        // is a deliberate placement — couple it on the spot (same
                        // as a library drop), no 1.5s dwell. couple_fx_now fires
                        // the weld flash + glow and is a no-op for content clips.
                        int sel = base;
                        const char* act = "Move clip to track";
                        if (is_fx_clip(grp[0])) {
                            sel = couple_fx_now(state, drag_hot_track, base);
                            if (sel != base || state.tracks[drag_hot_track]
                                    .clips[(size_t)sel].fx_coupled)
                                act = "Couple FX brick";
                        }
                        state.selected_track = drag_hot_track;
                        state.selected_clip  = sel;
                        state.clip_selection.clear();
                        state.clip_selection.insert({state.selected_track, state.selected_clip});
                        history_push(state, act);
                    }
                }
            } else {
                // Body drag on same track — validate with conflict predicate, restore on conflict
                bool overlaps = false;
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
                if (!drag_left && !drag_right) {
                    if (check_clip_conflicts(drag_track, drag_clip)) overlaps = true;
                    if (!overlaps) {
                        for (auto& orig : g_tl.drag_multi) {
                            if (orig.ti == drag_track && orig.ci == drag_clip) continue;
                            if (check_clip_conflicts(orig.ti, orig.ci)) { overlaps = true; break; }
                        }
                    }
                } else if (is_fx_clip(state.tracks[drag_track].clips[drag_clip]) &&
                           check_clip_conflicts(drag_track, drag_clip)) {
                    // Trimming an FX brick's edge over a neighboring FX brick
                    // would stack them — bounce the trim back too.
                    overlaps = true;
                }
                if (overlaps) {
                    // Restore focus clip
                    Clip& moved_clip = state.tracks[drag_track].clips[drag_clip];
                    if (drag_left) {
                        // Left trims also moved in_point and shifted keys;
                        // undo both along with the edge.
                        float d = drag_origin_start - moved_clip.start;
                        moved_clip.in_point = fmaxf(0.f, moved_clip.in_point +
                                                    d * fmaxf(0.01f, moved_clip.speed));
                        clip_keys_shift(moved_clip, -d);
                    }
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
        g_tl.drag_merge_ti = -1; g_tl.drag_merge_ci = -1;
        s_fx_weld.target_ti = -1; s_fx_weld.target_ci = -1;
        s_drag_moved = false;
        s_body_snap_held_start = -1.f; s_body_snap_held_cand = -1.f;
        g_tl.drag_multi.clear();
    }

    // Playhead. The transport parks on the last *whole* frame (a playhead at
    // the exclusive `duration` shows nothing), which leaves the marker one
    // sub-frame short of the final clip's right edge. When we're sitting on
    // that last frame, draw the marker at the true timeline end so it meets the
    // clip border — the preview still holds the last real frame.
    float ph_draw = state.playhead;
    if (state.duration > 0.f && state.playhead >= last_playable_time(state) - 1e-4f)
        ph_draw = state.duration;
    float ph_x = origin.x+TL_LABEL_W+ph_draw*zoom-scroll;
    if (ph_x >= origin.x+TL_LABEL_W && ph_x <= origin.x+total_w) {
        dl->AddLine({ph_x, origin.y}, {ph_x, origin.y+total_h}, to_u32(Col::fg));
        dl->AddTriangleFilled({ph_x-5.f,origin.y},{ph_x+5.f,origin.y},{ph_x,origin.y+10.f},to_u32(Col::fg));
    }

    // Click/drag ruler to seek. Once grabbed, mouse can roam outside the ruler strip.
    auto& s_ruler_drag = g_tl.ruler_drag;
    bool any_popup_global = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (ImGui::IsMouseReleased(0)) s_ruler_drag = false;
    if (!any_popup_global && drag_track < 0 && g_tl.loop_drag == 0 && g_tl.marker_drag < 0) {
        // Seek zone starts below the loop-brace strip so the two don't collide.
        bool in_ruler = mouse.y >= origin.y + LOOP_STRIP_H && mouse.y <= origin.y + TL_RULER_H &&
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
        float gap_y = track_top_at(drag_hot_gap);
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
                    float clip_end = cl.end;
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
                    // New clip longer than the current view → zoom out to fit
                    // it once placed (consumer only ever zooms out, next frame).
                    state.tl_zoom_to_fit_end = fmaxf(state.tl_zoom_to_fit_end, clip_end);
                    history_push(state, act);
                };
                float drop_t = snap_to_frame(state, fmaxf(0.f, (ImGui::GetMousePos().x - (origin.x + TL_LABEL_W) + scroll) / zoom));

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
                if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("TEXT_STYLE")) {
                    AnimStyle st = (AnimStyle)*(const int*)pay->Data;
                    make_new_track(make_text_brick(st, drop_t),
                                   (std::string("Drop text brick: ") + text_style_name(st)).c_str(), true);
                }
                if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("FX_CREATIVE")) {
                    FXType ft = (FXType)*(const int*)pay->Data;
                    Clip cl; cl.clip_type = ClipType::Effect; cl.fx_type = ft;
                    cl.start = drop_t; cl.end = drop_t + 5.f;
                    make_new_track(std::move(cl), (std::string("Drop FX: ") + fx_type_name(ft)).c_str(), false);
                }
                if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("FX_AUDIO")) {
                    FXType ft = (FXType)*(const int*)pay->Data;
                    Clip cl; cl.clip_type = ClipType::Effect; cl.fx_type = ft;
                    cl.start = drop_t; cl.end = drop_t + 5.f;
                    cl.audio_fx.autotune_on      = (ft == FXType::AudioAutotune);
                    cl.audio_fx.pitch_on         = (ft == FXType::AudioPitch);
                    cl.audio_fx.formant_on       = (ft == FXType::AudioFormant);
                    cl.audio_fx.delay_on         = (ft == FXType::AudioDelay);
                    cl.audio_fx.reverb_on        = (ft == FXType::AudioReverb);
                    cl.audio_fx.voice_convert_on = (ft == FXType::AudioVoiceConvert);
                    make_new_track(std::move(cl), (std::string("Drop audio FX: ") + fx_type_name(ft)).c_str(), false);
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

    // Super-diamond unpair menu — pulls members back out of a welded group by
    // nudging their keys one frame apart (just past the grouping tolerance).
    if (ImGui::BeginPopup("##kf_ctx")) {
        bool ctx_ok = s_kf_ctx.ti >= 0 && s_kf_ctx.ti < (int)state.tracks.size() &&
                      s_kf_ctx.ci >= 0 &&
                      s_kf_ctx.ci < (int)state.tracks[s_kf_ctx.ti].clips.size();
        if (!ctx_ok) {
            ImGui::CloseCurrentPopup();
        } else {
            Clip& kclip = state.tracks[s_kf_ctx.ti].clips[s_kf_ctx.ci];
            std::vector<std::string> mems;
            for (auto& [p, pt] : kclip.ktracks)
                if (pt.find_nearest(s_kf_ctx.time, 0.02f) >= 0) mems.push_back(p);
            std::sort(mems.begin(), mems.end());

            // First free timestamp stepping away from the group, frame-sized
            // steps so unpaired keys land on the grid and never re-group.
            auto free_spot = [&]() {
                float dur  = kclip.end - kclip.start;
                float step = fmaxf(1.f / (float)state.fps, 0.04f);
                auto occupied = [&](float t) {
                    for (auto& [p2, pt2] : kclip.ktracks)
                        if (pt2.find_nearest(t, 0.025f) >= 0) return true;
                    return false;
                };
                float nt = s_kf_ctx.time + step; int guard = 0;
                while (nt <= dur && occupied(nt) && guard++ < 64) nt += step;
                if (nt > dur) {
                    nt = s_kf_ctx.time - step; guard = 0;
                    while (nt >= 0.f && occupied(nt) && guard++ < 64) nt -= step;
                    if (nt < 0.f) nt = s_kf_ctx.time;  // nowhere to go
                }
                return nt;
            };
            auto unpair_one = [&](const std::string& prop) {
                auto it = kclip.ktracks.find(prop);
                if (it == kclip.ktracks.end()) return;
                int ki = it->second.find_nearest(s_kf_ctx.time, 0.02f);
                if (ki < 0) return;
                it->second.keys[ki].time = free_spot();
                std::sort(it->second.keys.begin(), it->second.keys.end(),
                          [](const Keyframe& a, const Keyframe& b){ return a.time < b.time; });
            };

            if (mems.size() < 2) {
                ImGui::CloseCurrentPopup();  // weld dissolved meanwhile
            } else {
                if (ImGui::MenuItem("Unpair all")) {
                    // Keep the first member in place, spread the rest out.
                    for (size_t i = 1; i < mems.size(); ++i) unpair_one(mems[i]);
                    state.kf_sel_idx = -1;  // selection index likely stale now
                    history_push(state, "Unpair keyframes");
                }
                ImGui::Separator();
                for (auto& p : mems) {
                    char lbl[64]; snprintf(lbl, sizeof(lbl), "Unpair %s", p.c_str());
                    if (ImGui::MenuItem(lbl)) {
                        unpair_one(p);
                        state.kf_sel_idx = -1;
                        history_push(state, "Unpair keyframe");
                    }
                }
            }
        }
        ImGui::EndPopup();
    }

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
                // Build the Subtitles track (ClipType::Subtitle) when transcription
                // finishes. Without this callback the pipeline ran but nothing
                // populated the timeline — same bug the Extract Subtitles button hit.
                state.pipeline_on_done = apply_subtitle_pipeline;
                kick_pipeline(state, cc->text, PipelineMode::TranscribeOnly);
            }
            if (ImGui::MenuItem("Separate vocals")) {
                kick_pipeline(state, cc->text, PipelineMode::SeparateOnly);
            }
            if (disabled) ImGui::EndDisabled();

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
            if (ImGui::MenuItem("Wrap in Video Multi-FX")) {
                Clip se    = *cc;
                cc->clip_type  = ClipType::MultiFX;
                cc->fx_chain.clear();
                cc->fx_chain_selected = 0;
                // Sub-clip inherits the parent's time span as full duration (rel_start=0, rel_end=0)
                se.rel_start = 0.f; se.rel_end = 0.f;
                cc->fx_chain.push_back(std::move(se));
                history_push(state, "Wrap in Video Multi-FX");
            }
            ImGui::Separator();
        }

        // ── Manage the FX welded to a CONTENT clip (right-clicked the host) ──
        if (cc && !is_fx_clip(*cc)) {
            std::vector<int> fxk = coupled_fx_of(state, ti, ci);
            if (!fxk.empty()) {
                bool many = fxk.size() > 1;
                if (ImGui::MenuItem(many ? "Decouple FX bricks" : "Decouple FX brick")) {
                    // Lift each freed brick onto its own fresh track just below
                    // the content — a clean, movable global brick. Process high
                    // index first so the earlier indices stay valid.
                    std::sort(fxk.begin(), fxk.end(), std::greater<int>());
                    for (int k : fxk) decouple_fx_to_new_track(state, ti, k);
                    history_push(state, "Decouple FX");
                }
                if (ImGui::MenuItem(many ? "Remove FX bricks" : "Remove FX brick")) {
                    s_ctx_fx_del_ti = ti; s_ctx_fx_del_cis = fxk;   // deferred erase
                }
                ImGui::Separator();
            }
        }

        // ── Decouple / remove a coupled Multi-FX brick (right-clicked the brick) ──
        if (cc && cc->fx_coupled && is_fx_clip(*cc)) {
            if (ImGui::MenuItem("Decouple Multi-FX brick")) {
                // Lift the freed brick onto a fresh track just below the content
                // so it becomes a clean, movable global brick (keeps its span).
                decouple_fx_to_new_track(state, ti, ci);
                history_push(state, "Decouple Multi-FX brick");
            }
            if (ImGui::MenuItem("Remove effect")) {
                s_ctx_fx_del_ti = ti; s_ctx_fx_del_cis = {ci};      // deferred erase
            }
            ImGui::Separator();
        }

        bool trk_locked = ct && ct->locked;
        if (trk_locked) ImGui::BeginDisabled();
        if (ImGui::MenuItem("Split at playhead", "S")) {
            if (valid) {
                float cut = state.playhead;
                if (cut > cc->start+0.02f && cut < cc->end-0.02f) {
                    clip_split_with_fx(state, ti, ci, cut);
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
        if (valid && cc->clip_type == ClipType::Video &&
            ImGui::MenuItem(cc->has_crop() ? "Crop clip *" : "Crop clip")) {
            state.selected_track  = ctx_track;
            state.selected_clip   = ci;
            state.crop_edit_track = ctx_track;
            state.crop_edit_clip  = ci;
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

    // Apply a deferred "Remove FX / Remove effect" requested above — erasing in
    // the popup body would dangle cc/ci for the menu items that follow.
    if (s_ctx_fx_del_ti >= 0 && s_ctx_fx_del_ti < (int)state.tracks.size() &&
        !s_ctx_fx_del_cis.empty()) {
        auto& cls = state.tracks[(size_t)s_ctx_fx_del_ti].clips;
        std::sort(s_ctx_fx_del_cis.rbegin(), s_ctx_fx_del_cis.rend());   // erase high→low
        for (int k : s_ctx_fx_del_cis)
            if (k >= 0 && k < (int)cls.size()) cls.erase(cls.begin() + k);
        state.selected_clip = -1;
        history_push(state, s_ctx_fx_del_cis.size() > 1 ? "Remove FX" : "Remove effect");
    }
    s_ctx_fx_del_ti = -1;
    s_ctx_fx_del_cis.clear();

    if (ImGui::BeginPopup("##track_ctx")) {
        open_track_ctx = false;
        int ti = ctx_track;
        bool valid = ti>=0 && ti<(int)state.tracks.size();
        Track* ct = valid ? &state.tracks[ti] : nullptr;

        if (ct) {
            // (Audio bus routing removed — grouping is now a Bus brick placed on
            // a track, submixing the tracks below it.)

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
        if (ImGui::MenuItem("Add Audio Record Brick"))
            add_record_brick(state);
        if (ImGui::MenuItem("Add Video Record Brick"))
            add_video_record_brick(state);
        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(2);
    (void)open_clip_ctx; (void)open_track_ctx; (void)open_tl_ctx;

    // Content-coupling timer — runs after all clip interaction so coupling
    // never invalidates indices the draw loop above is still using.
    couple_pending_tick(state);
}
