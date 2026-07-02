#include "studio_types.h"
#include "studio_shared.h"
#include "panel_media.h"
#include "app.h"
#include "audio.h"
#include "video.h"
#include "proxy.h"
#include "conform.h"
#include "history.h"
#include "project.h"
#include "fx_shader.h"
#include "theme.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <filesystem>
#include <algorithm>
#include <set>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>

namespace fs = std::filesystem;

extern double s_scrub_until;  // owned by canvas.cpp (used in ui_studio scrub expiry)

// ── Shared source-duration cache ──────────────────────────────────────────────
std::unordered_map<std::string, float> s_source_durations;

// ── Time formatting ───────────────────────────────────────────────────────────
std::string fmt_time(float s) {
    int m  = (int)(s / 60);
    int sc = (int)s % 60;
    int cs = (int)((s - floorf(s)) * 100.f);
    char buf[20]; snprintf(buf, sizeof(buf), "%d:%02d.%02d", m, sc, cs);
    return buf;
}

std::string fmt_time_short(float s) {
    int m  = (int)(s / 60);
    int sc = (int)s % 60;
    char buf[12]; snprintf(buf, sizeof(buf), "%d:%02d", m, sc);
    return buf;
}

int find_empty_track(const AppState& state) {
    for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
        const Track& t = state.tracks[ti];
        if (is_group_head(t)) continue;   // folder rows hold no clips
        if (t.clips.empty() && t.visible && !t.locked && !t.managed) return ti;
    }
    return -1;
}

// ── Multi-selection ops ───────────────────────────────────────────────────────
bool delete_selected_clips(AppState& state) {
    auto is_fx = [](const Clip& c) {
        return c.clip_type == ClipType::Effect  || c.clip_type == ClipType::MultiFX ||
               c.clip_type == ClipType::BodyFX  || c.clip_type == ClipType::AudioMultiFX;
    };
    // A content clip's welded FX bricks must die with it — otherwise deleting the
    // content (selecting a glass brick selects its host) leaves the FX orphaned.
    auto add_coupled = [&](int ti, int ci, std::vector<int>& out) {
        auto& cls = state.tracks[(size_t)ti].clips;
        for (int k = 0; k < (int)cls.size(); ++k)
            if (k != ci && cls[(size_t)k].fx_coupled && is_fx(cls[(size_t)k]) &&
                fx_coupled_host(state, ti, cls[(size_t)k]) == ci)
                out.push_back(k);
    };
    // Deleting a "Remove Background" body-FX brick turns the cutout back off on
    // its host clip — otherwise the decode-time bg_remove_on flag lingers and the
    // background stays removed even though the effect is gone.
    auto clear_bg_for_deleted = [&](int ti, const std::vector<int>& dels) {
        auto& cls = state.tracks[(size_t)ti].clips;
        for (int k : dels) {
            if (k < 0 || k >= (int)cls.size()) continue;
            const Clip& d = cls[(size_t)k];
            if (d.clip_type != ClipType::BodyFX ||
                d.body_fx_type != BodyFXType::RemoveBackground) continue;
            for (auto& h : cls)
                if (clip_is_videolike_type(h.clip_type) && !h.source_id.empty() &&
                    h.source_id == d.source_id) {
                    h.bg_remove_on     = false;
                    h.bg_remove_status = BgRemoveStatus::Idle;
                }
        }
    };

    if (state.clip_selection.size() <= 1) {
        if (state.selected_track < 0 || state.selected_clip < 0) return false;
        if (state.selected_track >= (int)state.tracks.size()) return false;
        Track& tr = state.tracks[state.selected_track];
        if (state.selected_clip >= (int)tr.clips.size()) return false;
        std::vector<int> dels{state.selected_clip};
        add_coupled(state.selected_track, state.selected_clip, dels);
        std::sort(dels.begin(), dels.end(), std::greater<int>());
        clear_bg_for_deleted(state.selected_track, dels);
        for (int k : dels)
            if (k >= 0 && k < (int)tr.clips.size()) tr.clips.erase(tr.clips.begin() + k);
        state.selected_clip = -1;
        state.clip_selection.clear();
        return true;
    }
    // Group by track; delete descending so erase() doesn't shift indices
    // out from under us within a track.
    std::vector<std::vector<int>> by_track(state.tracks.size());
    for (auto& [ti, ci] : state.clip_selection)
        if (ti >= 0 && ti < (int)state.tracks.size()) {
            by_track[ti].push_back(ci);
            add_coupled(ti, ci, by_track[ti]);
        }
    for (int ti = 0; ti < (int)by_track.size(); ++ti) {
        auto& cis = by_track[ti];
        std::sort(cis.begin(), cis.end(), std::greater<int>());
        cis.erase(std::unique(cis.begin(), cis.end()), cis.end());
        clear_bg_for_deleted(ti, cis);
        for (int ci : cis)
            if (ci >= 0 && ci < (int)state.tracks[ti].clips.size())
                state.tracks[ti].clips.erase(state.tracks[ti].clips.begin() + ci);
    }
    state.clip_selection.clear();
    state.selected_clip = -1;
    return true;
}

bool duplicate_selected_clips(AppState& state) {
    auto valid = [&](int ti, int ci) {
        return ti >= 0 && ti < (int)state.tracks.size() &&
               ci >= 0 && ci < (int)state.tracks[ti].clips.size();
    };
    if (state.clip_selection.size() <= 1) {
        if (!valid(state.selected_track, state.selected_clip)) return false;
        Track& tr = state.tracks[state.selected_track];
        Clip dup = tr.clips[state.selected_clip];
        if (dup.clip_type == ClipType::Lyrics) dup.source_id.clear();  // duplicated lyric is freestanding — survives a typography regen
        float len = dup.end - dup.start;
        dup.start = dup.end;
        dup.end   = dup.start + len;
        tr.clips.insert(tr.clips.begin() + state.selected_clip + 1, dup);
        state.selected_clip = state.selected_clip + 1;
        state.clip_selection.clear();
        state.clip_selection.insert({state.selected_track, state.selected_clip});
        return true;
    }
    float gmin =  1e30f, gmax = -1e30f;
    for (auto& [ti, ci] : state.clip_selection) {
        if (!valid(ti, ci)) continue;
        const Clip& c = state.tracks[ti].clips[ci];
        gmin = fminf(gmin, c.start);
        gmax = fmaxf(gmax, c.end);
    }
    if (!(gmax > gmin)) return false;
    float shift = gmax - gmin;
    // Snapshot per-track originals first; append copies after so we don't
    // duplicate clips we just inserted.
    std::vector<std::vector<Clip>> dups(state.tracks.size());
    for (auto& [ti, ci] : state.clip_selection) {
        if (!valid(ti, ci)) continue;
        Clip d = state.tracks[ti].clips[ci];
        if (d.clip_type == ClipType::Lyrics) d.source_id.clear();  // duplicated lyric is freestanding
        d.start += shift;
        d.end   += shift;
        dups[ti].push_back(std::move(d));
    }
    state.clip_selection.clear();
    int new_primary_ti = -1, new_primary_ci = -1;
    for (int ti = 0; ti < (int)dups.size(); ++ti) {
        for (auto& d : dups[ti]) {
            state.tracks[ti].clips.push_back(std::move(d));
            int nci = (int)state.tracks[ti].clips.size() - 1;
            state.clip_selection.insert({ti, nci});
            if (new_primary_ti < 0) { new_primary_ti = ti; new_primary_ci = nci; }
        }
    }
    if (new_primary_ti >= 0) {
        state.selected_track = new_primary_ti;
        state.selected_clip  = new_primary_ci;
    }
    return true;
}

// ── Playback helpers ──────────────────────────────────────────────────────────
float snap_to_frame(const AppState& state, float t) {
    float fps = tl_fps(state);
    if (!(fps > 0.f)) fps = 30.f;
    return roundf(t * fps) / fps;
}

void seek_to(AppState& state, float t) {
    // Quantize to the timeline frame grid (falls back to 30 fps). A hard-coded
    // 30 fps grid eats sub-frame steps for >30 fps content, which made the
    // back-arrow stick at high zoom on 60 fps footage.
    t = snap_to_frame(state, t);
    // Never past the last playable frame (a playhead at `duration` shows nothing).
    t = fmaxf(0.f, fminf(t, last_playable_time(state)));
    state.playhead = t;
    audio_seek(t);
    if (state.playing) {
        state.play_start_pos  = t;
        state.play_start_wall = std::chrono::steady_clock::now();
    } else {
        audio_play();
        s_scrub_until = ImGui::GetTime() + 0.08;
    }
}

void toggle_play(AppState& state) {
    state.playing = !state.playing;
    if (state.playing) {
        state.play_start_pos  = state.playhead;
        state.play_start_wall = std::chrono::steady_clock::now();
        audio_seek(state.playhead);
        audio_play();
    } else {
        audio_pause();
    }
}

float tl_fps(const AppState& state) {
    // The timeline frame grid = the project/export fps — the ONE grid that the ruler
    // ticks, all snapping, and the normalize pass land on, so nothing ever sits
    // between frames. A source video's higher native rate does NOT raise the grid:
    // you cut and export at the project fps, so that's the grid you see and snap to.
    float tf = (float)state.fps;
    if (!(tf > 0.f)) tf = 30.f;
    return tf;
}

void normalize_timeline_to_grid(AppState& state) {
    float fps = tl_fps(state);
    if (!(fps > 0.f)) return;
    auto q = [fps](float t){ return std::roundf(t * fps) / fps; };
    for (auto& tr : state.tracks)
        for (auto& c : tr.clips) {
            c.start = q(c.start);
            c.end   = q(c.end);
            if (c.end <= c.start) c.end = c.start + 1.f / fps;   // never collapse below a frame
            c.in_point        = fmaxf(0.f, q(c.in_point));
            c.transition_pre  = fmaxf(0.f, q(c.transition_pre));
            c.transition_post = fmaxf(0.f, q(c.transition_post));
            c.fade_in         = fmaxf(0.f, q(c.fade_in));
            c.fade_out        = fmaxf(0.f, q(c.fade_out));
            for (auto& kv : c.ktracks)
                for (auto& k : kv.second.keys) k.time = fmaxf(0.f, q(k.time));
        }
    for (auto& m : state.markers) m.time = fmaxf(0.f, q(m.time));
    if (state.loop_in  >= 0.f) state.loop_in  = q(state.loop_in);
    if (state.loop_out >= 0.f) state.loop_out = q(state.loop_out);
}

float last_playable_time(const AppState& state) {
    if (state.duration <= 0.f) return 0.f;
    float fps = tl_fps(state);
    if (!(fps > 0.f)) fps = 30.f;
    // Start time of the last whole frame: ceil(duration*fps) is the frame count,
    // minus one → last index, /fps → its start. Always strictly inside [0,duration).
    float lf = (ceilf(state.duration * fps) - 1.f) / fps;
    return fmaxf(0.f, lf);
}

bool loop_region(const AppState& state, float& lo, float& hi) {
    bool custom = state.loop_in >= 0.f && state.loop_out > state.loop_in;
    lo = custom ? fmaxf(0.f, state.loop_in) : 0.f;
    hi = custom ? state.loop_out : state.duration;
    // The brace edges are the loop's wrap points — force them onto the frame
    // grid so a loop can never cycle mid-frame (set paths already snap; this is
    // the belt-and-braces guard for older saved regions).
    if (custom) { lo = snap_to_frame(state, lo); hi = snap_to_frame(state, hi); }
    if (hi > state.duration) hi = state.duration;   // never cycle past content
    if (hi <= lo) { lo = 0.f; hi = state.duration; return false; }  // degenerate → whole
    return custom;
}

int marker_add(AppState& state, float time, const char* label) {
    Marker m;
    m.time = snap_to_frame(state, fmaxf(0.f, time));
    if (label && *label) {
        m.label = label;
    } else {
        char buf[24];
        snprintf(buf, sizeof(buf), "Marker %d", (int)state.markers.size() + 1);
        m.label = buf;
    }
    // Keep markers sorted by time so prev/next jumps and the ruler walk are linear.
    auto it = std::lower_bound(state.markers.begin(), state.markers.end(), m.time,
                               [](const Marker& a, float t){ return a.time < t; });
    int idx = (int)(it - state.markers.begin());
    state.markers.insert(it, std::move(m));
    history_push(state, "Add marker");
    return idx;
}

// ── Cross-surface FX clipboard ──────────────────────────────────────────────────
Clip s_fx_clipboard;
bool s_fx_clipboard_has   = false;
bool s_fx_clipboard_audio = false;

void fx_clip_copy(const Clip& se, bool audio) {
    s_fx_clipboard = se; s_fx_clipboard_has = true; s_fx_clipboard_audio = audio;
}
bool fx_clip_can_paste(const Clip& brick) {
    return s_fx_clipboard_has && s_fx_clipboard_audio == fx_brick_is_audio_kind(brick);
}
void fx_chain_duplicate(AppState& state, Clip& brick, int idx) {
    if (idx < 0 || idx >= (int)brick.fx_chain.size()) return;
    Clip cp = brick.fx_chain[(size_t)idx];                  // copies params + keyframes
    brick.fx_chain.insert(brick.fx_chain.begin() + idx + 1, std::move(cp));
    brick.fx_chain_selected = idx + 1;
    history_push(state, std::string(fx_brick_is_audio_kind(brick) ? "Audio Multi-FX: " : "Multi-FX: ") + "duplicate effect");
}
void fx_chain_paste(AppState& state, Clip& brick, int after_idx) {
    if (!fx_clip_can_paste(brick)) return;
    int n  = (int)brick.fx_chain.size();
    int at = (after_idx >= 0 && after_idx < n) ? after_idx + 1 : n;
    brick.fx_chain.insert(brick.fx_chain.begin() + at, s_fx_clipboard);
    brick.fx_chain_selected = at;
    history_push(state, std::string(fx_brick_is_audio_kind(brick) ? "Audio Multi-FX: " : "Multi-FX: ") + "paste effect");
}
void fx_chain_delete(AppState& state, Clip& brick, int idx) {
    if (idx < 0 || idx >= (int)brick.fx_chain.size()) return;
    brick.fx_chain.erase(brick.fx_chain.begin() + idx);
    int n = (int)brick.fx_chain.size();
    if (n == 0)                             brick.fx_chain_selected = -1;
    else if (brick.fx_chain_selected > idx) brick.fx_chain_selected--;
    else if (brick.fx_chain_selected >= n)  brick.fx_chain_selected = n - 1;
    history_push(state, std::string(fx_brick_is_audio_kind(brick) ? "Audio Multi-FX: " : "Multi-FX: ") + "remove effect");
}

bool kf_slider(AppState& state, Clip& clip, int sel_ti, int sel_ci, float w,
               const char* prop, const char* label, float* val_ptr,
               float vmin, float vmax, const char* fmt,
               float disp, const char* prop2) {
    // Keyframes must land ON a frame — snap the playhead to the frame grid before
    // taking the clip-local time, so you can't drop one between two frames (the
    // FX-brick sliders read straight off the playhead, which can sit sub-frame).
    float t_local = snap_to_frame(state.playhead, (int)tl_fps(state)) - clip.start;
    float kf_tol  = 0.5f / fmaxf(1.f, tl_fps(state));   // half a frame: exact-frame match
    bool changed = false;
    auto it_pt = clip.ktracks.find(prop);
    PropTrack* pt = (it_pt != clip.ktracks.end()) ? &it_pt->second : nullptr;
    bool has_keys = pt && !pt->empty();
    bool has_kf   = pt && pt->find_nearest(t_local, kf_tol) >= 0;

    auto toggle_key_here = [&]() {
        if (has_kf) {
            pt->remove_at(t_local, kf_tol);
            if (pt->empty()) clip.ktracks.erase(prop);
            if (prop2) {
                auto it2 = clip.ktracks.find(prop2);
                if (it2 != clip.ktracks.end()) {
                    it2->second.remove_at(t_local, kf_tol);
                    if (it2->second.empty()) clip.ktracks.erase(it2);
                }
            }
            history_push(state, std::string("Remove KF ") + prop);
        } else {
            clip.ktracks[prop].set(t_local, clip.eval_prop(prop, state.playhead));
            if (prop2) clip.ktracks[prop2].set(t_local, clip.eval_prop(prop2, state.playhead));
            state.kf_sel_track = sel_ti; state.kf_sel_clip = sel_ci;
            state.kf_sel_prop  = prop;
            state.kf_sel_idx   = clip.ktracks[prop].find_nearest(t_local, kf_tol);
            state.kf_sel_source = -1;
            history_push(state, std::string("Add KF ") + prop);
            // Auto-expose: if `clip` is a MultiFX sub-effect (not the selected clip
            // itself), reveal it on the timeline — open this effect's per-param rows
            // and the host's coupled-chain lanes.
            if (sel_ti >= 0 && sel_ti < (int)state.tracks.size() &&
                sel_ci >= 0 && sel_ci < (int)state.tracks[(size_t)sel_ti].clips.size()) {
                Clip& owner = state.tracks[(size_t)sel_ti].clips[(size_t)sel_ci];
                if (&clip != &owner) {
                    // Record which chain entry this is, so the timeline diamond
                    // highlights the right key (source-correct selection).
                    for (int i = 0; i < (int)owner.fx_chain.size(); ++i)
                        if (&owner.fx_chain[(size_t)i] == &clip) { state.kf_sel_source = i; break; }
                    clip.params_expanded = true;
                    int host = fx_coupled_host(state, sel_ti, owner);
                    if (host >= 0) state.tracks[(size_t)sel_ti].clips[(size_t)host].fx_expanded = true;
                    else owner.fx_expanded = true;
                }
            }
        }
    };
    {
        ImDrawList* kdl = ImGui::GetWindowDrawList();
        float rh = ImGui::GetFrameHeight();
        const ImU32 gold = IM_COL32(255,200,60,255), gold_dim = IM_COL32(190,160,70,220),
                    faint = IM_COL32(120,120,130,200), off = IM_COL32(70,70,78,150);
        ImGui::InvisibleButton((std::string("##kfprev_") + prop).c_str(), {12.f, rh});
        {
            ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            float cx = (a.x+b.x)*0.5f, cy = (a.y+b.y)*0.5f;
            ImU32 c = !has_keys ? off : ImGui::IsItemHovered() ? gold : faint;
            kdl->AddTriangleFilled({cx+3,cy-4},{cx+3,cy+4},{cx-3,cy}, c);
            if (has_keys && ImGui::IsItemClicked()) {
                float best = -1.f;
                for (auto& k : pt->keys) if (k.time < t_local - 1e-4f) best = k.time;
                if (best >= 0.f) seek_to(state, clip.start + best);
            }
        }
        ImGui::SameLine(0.f, 1.f);
        ImGui::InvisibleButton((std::string("##kftog_") + prop).c_str(), {16.f, rh});
        {
            ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            float cx = (a.x+b.x)*0.5f, cy = (a.y+b.y)*0.5f, r = 5.5f;
            bool hv = ImGui::IsItemHovered();
            ImU32 c = has_kf ? gold : has_keys ? gold_dim : (hv ? gold_dim : faint);
            if (hv) c = gold;
            ImVec2 top{cx,cy-r}, rt{cx+r,cy}, bot{cx,cy+r}, lf{cx-r,cy};
            if (has_kf) kdl->AddQuadFilled(top, rt, bot, lf, c);
            else        kdl->AddQuad(top, rt, bot, lf, c, 1.6f);
            if (ImGui::IsItemClicked()) toggle_key_here();
            if (hv) ImGui::SetTooltip(has_kf ? "Remove keyframe here"
                                   : has_keys ? "Add keyframe here"
                                              : "Animate this property");
        }
        ImGui::SameLine(0.f, 1.f);
        ImGui::InvisibleButton((std::string("##kfnext_") + prop).c_str(), {12.f, rh});
        {
            ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            float cx = (a.x+b.x)*0.5f, cy = (a.y+b.y)*0.5f;
            ImU32 c = !has_keys ? off : ImGui::IsItemHovered() ? gold : faint;
            kdl->AddTriangleFilled({cx-3,cy-4},{cx-3,cy+4},{cx+3,cy}, c);
            if (has_keys && ImGui::IsItemClicked()) {
                float nxt = -1.f;
                for (auto& k : pt->keys) if (k.time > t_local + 1e-4f) { nxt = k.time; break; }
                if (nxt >= 0.f) seek_to(state, clip.start + nxt);
            }
        }
    }
    ImGui::SameLine(0.f, 6.f);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted(label); ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, has_keys ? IM_COL32(255,200,60,255) : to_u32(Col::fg));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
    ImGui::SetNextItemWidth(w - 16.f);
    char sid[80]; snprintf(sid, sizeof(sid), "##kfs_%s", prop);
    float dv = (has_keys ? clip.eval_prop(prop, state.playhead) : *val_ptr) * disp;
    // Applies a new raw value through the same path an edit takes: static
    // field always, plus the key at the playhead when the prop is animated.
    auto apply_value = [&](float raw) {
        changed  = true;
        *val_ptr = raw;
        if (has_keys) {
            int ki = pt->find_nearest(t_local, kf_tol);
            if (ki >= 0) pt->keys[ki].value = raw; else pt->set(t_local, raw);
            if (prop2) {
                PropTrack& p2 = clip.ktracks[prop2];
                int k2 = p2.find_nearest(t_local, kf_tol);
                if (k2 >= 0) p2.keys[k2].value = raw; else p2.set(t_local, raw);
            }
        }
    };
    if (ImGui::SliderFloat(sid, &dv, vmin, vmax, fmt))
        apply_value(dv / disp);
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, std::string("Edit ") + prop);
    // Homebase: double-click snaps the prop back to its fresh-Clip default
    // (keyframed props get the default keyed at the playhead, like any edit).
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        apply_value(clip_prop_default(prop, *val_ptr));
        ImGui::ClearActiveID();   // release the drag the first click started
        history_push(state, std::string("Reset ") + prop);
    }
    // Navigate-to-param: a timeline keyframe click set focus_prop → scroll this
    // slider into view + flash it briefly. One-shot, self-clearing.
    if (state.focus_prop == prop) {
        double el = ImGui::GetTime() - state.focus_prop_t;
        if (el < 0.15) ImGui::SetScrollHereY(0.5f);   // settle the scroll, then release
        float a = 1.f - (float)(el / 0.6);
        if (a > 0.f) {
            ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect({mn.x-2.f, mn.y-2.f}, {mx.x+2.f, mx.y+2.f},
                IM_COL32(255,200,60,(int)(200.f*a)), 3.f, 0, 2.f);
        }
        if (el > 0.6) { state.focus_prop.clear(); state.focus_prop_t = 0.0; }
    }
    return changed;
}

void kf_color_diamond(AppState& state, Clip& clip, int sel_ti, int sel_ci, const char* prefix) {
    char pr[64], pg[64], pb[64];
    snprintf(pr, sizeof(pr), "%s_r", prefix);
    snprintf(pg, sizeof(pg), "%s_g", prefix);
    snprintf(pb, sizeof(pb), "%s_b", prefix);
    float t_local = snap_to_frame(state.playhead, (int)tl_fps(state)) - clip.start;
    float kf_tol  = 0.5f / fmaxf(1.f, tl_fps(state));
    auto it_r = clip.ktracks.find(pr);
    PropTrack* pt = (it_r != clip.ktracks.end()) ? &it_r->second : nullptr;
    bool has_keys = pt && !pt->empty();
    bool has_kf   = pt && pt->find_nearest(t_local, kf_tol) >= 0;

    ImDrawList* kdl = ImGui::GetWindowDrawList();
    float rh = ImGui::GetFrameHeight();
    const ImU32 gold = IM_COL32(255,200,60,255), gold_dim = IM_COL32(190,160,70,220),
                faint = IM_COL32(120,120,130,200);
    ImGui::InvisibleButton((std::string("##kfcol_") + prefix).c_str(), {16.f, rh});
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    float cx = (a.x+b.x)*0.5f, cy = (a.y+b.y)*0.5f, r = 5.5f;
    bool hv = ImGui::IsItemHovered();
    ImU32 c = has_kf ? gold : has_keys ? gold_dim : (hv ? gold_dim : faint);
    if (hv) c = gold;
    ImVec2 top{cx,cy-r}, rt{cx+r,cy}, bot{cx,cy+r}, lf{cx-r,cy};
    if (has_kf) kdl->AddQuadFilled(top, rt, bot, lf, c);
    else        kdl->AddQuad(top, rt, bot, lf, c, 1.6f);
    if (hv) ImGui::SetTooltip(has_kf ? "Remove colour keyframe"
                            : has_keys ? "Add colour keyframe here"
                                       : "Keyframe this colour");
    if (ImGui::IsItemClicked()) {
        const char* props[3] = {pr, pg, pb};
        if (has_kf) {
            for (const char* p : props) {
                auto it = clip.ktracks.find(p);
                if (it != clip.ktracks.end()) {
                    it->second.remove_at(t_local, kf_tol);
                    if (it->second.empty()) clip.ktracks.erase(it);
                }
            }
            history_push(state, std::string("Remove KF ") + prefix + " colour");
        } else {
            for (const char* p : props)
                clip.ktracks[p].set(t_local, clip.eval_prop(p, state.playhead));
            state.kf_sel_track = sel_ti; state.kf_sel_clip = sel_ci;
            state.kf_sel_prop  = pr;
            state.kf_sel_idx   = clip.ktracks[pr].find_nearest(t_local, kf_tol);
            state.kf_sel_source = -1;
            // Auto-expose if `clip` is a MultiFX sub-effect (mirror kf_slider).
            if (sel_ti >= 0 && sel_ti < (int)state.tracks.size() &&
                sel_ci >= 0 && sel_ci < (int)state.tracks[(size_t)sel_ti].clips.size()) {
                Clip& owner = state.tracks[(size_t)sel_ti].clips[(size_t)sel_ci];
                if (&clip != &owner) {
                    for (int i = 0; i < (int)owner.fx_chain.size(); ++i)
                        if (&owner.fx_chain[(size_t)i] == &clip) { state.kf_sel_source = i; break; }
                    clip.params_expanded = true;
                    int host = fx_coupled_host(state, sel_ti, owner);
                    if (host >= 0) state.tracks[(size_t)sel_ti].clips[(size_t)host].fx_expanded = true;
                    else owner.fx_expanded = true;
                }
            }
            history_push(state, std::string("Add KF ") + prefix + " colour");
        }
    }
}

void kf_color_edit(AppState& state, Clip& clip, const char* prefix, float r, float g, float b) {
    float t_local = snap_to_frame(state.playhead, (int)tl_fps(state)) - clip.start;
    float kf_tol  = 0.5f / fmaxf(1.f, tl_fps(state));
    const char* sfx[3] = {"_r", "_g", "_b"};
    float vals[3] = {r, g, b};
    char nm[64];
    for (int i = 0; i < 3; ++i) {
        snprintf(nm, sizeof(nm), "%s%s", prefix, sfx[i]);
        auto it = clip.ktracks.find(nm);
        if (it == clip.ktracks.end() || it->second.empty()) continue;
        int ki = it->second.find_nearest(t_local, kf_tol);
        if (ki >= 0) it->second.keys[(size_t)ki].value = vals[i];
    }
}

int decouple_fx_to_new_track(AppState& state, int ti, int ci) {
    if (ti < 0 || ti >= (int)state.tracks.size()) return -1;
    auto& clips = state.tracks[ti].clips;
    if (ci < 0 || ci >= (int)clips.size()) return -1;
    Clip brick = std::move(clips[(size_t)ci]);
    brick.fx_coupled = false;
    brick.fx_host_sid.clear();
    clips.erase(clips.begin() + ci);
    Track nt;
    nt.name = "FX";
    nt.clips.push_back(std::move(brick));
    int new_ti = ti + 1;
    state.tracks.insert(state.tracks.begin() + new_ti, std::move(nt));
    return new_ti;
}

// ── Eyedropper ────────────────────────────────────────────────────────────────
static struct {
    std::string owner;      // dropper button id that armed the pick
    bool  active = false;   // waiting for a canvas click
    bool  has    = false;   // a sample landed, owner hasn't consumed it yet
    float rgb[3] = {};
} g_dropper;

bool ui_dropper_active() { return g_dropper.active; }
void ui_dropper_cancel() { g_dropper.active = false; g_dropper.has = false; g_dropper.owner.clear(); }
void ui_dropper_feed(float r, float g, float b) {
    if (!g_dropper.active) return;
    g_dropper.rgb[0] = r; g_dropper.rgb[1] = g; g_dropper.rgb[2] = b;
    g_dropper.active = false;
    g_dropper.has    = true;
}

bool ui_dropper_button(const char* id, float rgb_out[3]) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool armed = g_dropper.active && g_dropper.owner == id;
    ImGui::InvisibleButton(id, {18.f, 18.f});
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    bool hov = ImGui::IsItemHovered();
    ImU32 bg = armed ? IM_COL32(70, 110, 200, 255)
             : hov   ? IM_COL32(60, 60, 78, 255) : IM_COL32(40, 40, 52, 255);
    dl->AddRectFilled(a, b, bg, 4.f);
    // Pipette glyph: diagonal barrel + tip dot.
    ImU32 fg = armed ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 205, 220, 230);
    float x0 = a.x + 4.5f, y0 = b.y - 4.5f, x1 = b.x - 5.f, y1 = a.y + 5.f;
    dl->AddLine({x0 + 1.5f, y0 - 1.5f}, {x1, y1}, fg, 2.f);
    dl->AddCircleFilled({x0, y0}, 1.8f, fg);
    dl->AddLine({x1 - 1.f, y1 - 2.f}, {x1 + 2.f, y1 + 1.f}, fg, 3.f);
    if (hov) ImGui::SetTooltip(armed ? "Click the preview to sample (Esc cancels)"
                                     : "Pick a color from the preview");
    if (ImGui::IsItemClicked()) {
        if (armed) { ui_dropper_cancel(); }
        else { g_dropper.owner = id; g_dropper.active = true; g_dropper.has = false; }
    }
    if (g_dropper.has && g_dropper.owner == id) {
        rgb_out[0] = g_dropper.rgb[0];
        rgb_out[1] = g_dropper.rgb[1];
        rgb_out[2] = g_dropper.rgb[2];
        g_dropper.has = false;
        g_dropper.owner.clear();
        return true;
    }
    return false;
}

bool lib_search_match(const std::string& query, const char* hay1, const char* hay2) {
    if (query.empty()) return true;
    auto contains = [](const char* hay, const std::string& needle) {
        if (!hay) return false;
        std::string h(hay);
        for (auto& c : h) c = (char)std::tolower((unsigned char)c);
        return h.find(needle) != std::string::npos;
    };
    std::string q(query);
    for (auto& c : q) c = (char)std::tolower((unsigned char)c);
    return contains(hay1, q) || contains(hay2, q);
}

bool category_pills(const char* id, const std::vector<const char*>& cats,
                    std::string& sel, std::string& query) {
    ImGui::PushID(id);
    // Search row above the pills: field + clear button.
    char buf[96];
    strncpy(buf, query.c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
    float cw = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(query.empty() ? cw : cw - 26.f);
    if (ImGui::InputTextWithHint("##libsearch", "Search\xe2\x80\xa6", buf, sizeof(buf)))
        query = buf;
    ImGui::PopStyleColor();
    if (!query.empty()) {
        ImGui::SameLine(0.f, 4.f);
        if (ui_btn("\xc3\x97", false, true)) query.clear();
    }
    ImGui::Dummy({0.f, 2.f});
    ImGui::PopID();
    bool changed = category_pills(id, cats, sel);
    return changed;
}

bool category_pills(const char* id, const std::vector<const char*>& cats, std::string& sel) {
    // Drop a stale filter (category no longer present) back to All.
    if (!sel.empty()) {
        bool ok = false;
        for (auto* c : cats) if (sel == c) { ok = true; break; }
        if (!ok) sel.clear();
    }
    bool changed = false;
    ImGui::PushID(id);
    std::vector<const char*> pills{"All"};
    for (auto* c : cats) pills.push_back(c);
    // Screen-space right edge so the wrap test matches GetItemRectMax.
    float x2 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    for (size_t i = 0; i < pills.size(); ++i) {
        bool s = (i == 0) ? sel.empty() : (sel == pills[i]);
        if (ui_btn(pills[i], s, true)) {
            sel = (i == 0) ? std::string() : std::string(pills[i]);
            changed = true;
        }
        if (i + 1 < pills.size()) {
            float lx = ImGui::GetItemRectMax().x;
            float nw = ImGui::CalcTextSize(pills[i + 1]).x +
                       ImGui::GetStyle().FramePadding.x * 2.f + 8.f;
            if (lx + ImGui::GetStyle().ItemSpacing.x + nw < x2) ImGui::SameLine();
        }
    }
    ImGui::PopID();
    return changed;
}

void marker_jump(AppState& state, int dir) {
    if (state.markers.empty()) return;
    const float eps = 1e-3f;
    float here = state.playhead;
    float best = -1.f;
    if (dir < 0) {
        for (auto& m : state.markers) if (m.time < here - eps) best = m.time;  // sorted → last match
    } else {
        for (auto& m : state.markers) if (m.time > here + eps) { best = m.time; break; }
    }
    if (best >= 0.f) seek_to(state, best);
}

int group_head_of(const AppState& state, int ti) {
    for (int h = 0; h < (int)state.tracks.size(); ++h) {
        const Track& t = state.tracks[(size_t)h];
        if (!is_group_head(t)) continue;
        if (ti > h && ti <= h + t.group_children) return h;
    }
    return -1;
}

void normalize_track_groups(AppState& state) {
    int nt = (int)state.tracks.size();
    for (int h = 0; h < nt; ++h) {
        Track& t = state.tracks[(size_t)h];
        if (!is_group_head(t)) continue;
        // A head must never hold clips (it's a folder row, not a lane).
        if (!t.clips.empty()) t.clips.clear();
        int max_run = nt - 1 - h;
        if (t.group_children > max_run) t.group_children = max_run;
        // No nesting: the run stops before the next head.
        for (int j = h + 1; j <= h + t.group_children; ++j)
            if (is_group_head(state.tracks[(size_t)j])) {
                t.group_children = j - h - 1;
                break;
            }
        // Empty folder dissolves back into a plain (invisible-value) track —
        // delete it outright so it doesn't linger as a dead row.
        if (t.group_children <= 0) {
            if (state.selected_track == h) { state.selected_track = -1; state.selected_clip = -1; }
            state.tracks.erase(state.tracks.begin() + h);
            nt = (int)state.tracks.size();
            --h;
        }
    }
}

void mark_project_saved(AppState& state, const std::string& path) {
    state.saved_history_pos = history_pos();
    if (!path.empty()) state.thumb_request = project_thumb_path(path);
}
void mark_project_clean(AppState& state) {
    state.saved_history_pos = history_pos();
}
bool project_dirty(const AppState& state) {
    return history_pos() != state.saved_history_pos;
}

float output_px_height(const AppState& state) {
    switch (state.format) {
        case OutputFormat::Horizontal: return 1080.f;
        case OutputFormat::Square:     return 1080.f;
        default:                       return 1920.f;   // Vertical
    }
}

// ── Splitter capture ──────────────────────────────────────────────────────────
static bool g_splitter_capture = false;
void ui_set_splitter_capture(bool on) { g_splitter_capture = on; }
bool ui_splitter_capture()            { return g_splitter_capture; }

// ── Slider homebase (double-click reset) ──────────────────────────────────────
bool ui_slider_home(AppState& state, float* v, float defv, const char* hist_label) {
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        *v = defv;
        // The first click of the double-click grabbed the slider and jumped the
        // value — release it so the reset sticks instead of resuming the drag.
        ImGui::ClearActiveID();
        history_push(state, std::string("Reset ") + hist_label);
        return true;
    }
    return false;
}

// Default ("homebase") value for a keyframable clip prop: what a freshly
// constructed Clip carries in that field. Falls back to cur when the prop
// isn't in the registry (then a reset is a no-op rather than a surprise).
float clip_prop_default(const char* prop, float cur) {
    static const Clip s_def{};
    for (int i = 0; i < kClipKfFieldCount; ++i)
        if (!strcmp(prop, kClipKfFields[i].name)) return s_def.*(kClipKfFields[i].f);
    return cur;
}

// ── Clip / slot helpers ───────────────────────────────────────────────────────
std::string clip_slot_key(const std::string& src, float /*start*/) {
    return src;
}

std::string source_from_key(const std::string& key) {
    auto pos = key.find('\x01');
    return pos == std::string::npos ? key : key.substr(0, pos);
}

int slot_for_video(AppState& state, const std::string& key, const std::string& /*src*/) {
    if (key.empty()) return -1;
    for (int i = 0; i < MAX_VIDEO_SLOTS; ++i)
        if (state.proxy_paths[i] == key) return i;
    for (int i = 0; i < MAX_VIDEO_SLOTS; ++i)
        if (state.proxy_paths[i].empty()) { state.proxy_paths[i] = key; return i; }
    fprintf(stderr, "[video] slot table full (%d slots) — proxy will not load for: %s\n",
            MAX_VIDEO_SLOTS, key.c_str());
    return -1;
}

void gc_video_slots(AppState& state) {
    std::set<std::string> live;
    for (auto& tr : state.tracks)
        for (auto& cl : tr.clips)
            if (clip_is_videolike_type(cl.clip_type) && !cl.text.empty())
                live.insert(clip_slot_key(clip_video_src(state, cl), cl.start));
    for (int i = 0; i < MAX_VIDEO_SLOTS; ++i) {
        if (!state.proxy_paths[i].empty() && !live.count(state.proxy_paths[i])) {
            video_close(i);
            state.proxy_paths[i].clear();
        }
    }
}

// Open ONE slot for `src` — the per-source body of reopen_video_slots. This is
// the expensive part (proxy_load spawns ffprobe; GIFs decode all frames), so
// the project-open path runs it incrementally via the queue below.
static void open_video_slot_now(AppState& state, int slot, const std::string& src) {
    // Still images go straight to Still — never native. libav happily
    // opens a PNG as a one-frame video, but then any decode past t=0
    // fails and the clip renders blank (this is how MCP-added images
    // vanished from the preview: add_clip → proxy_scan → native PNG).
    // Animated images (.gif) fall through to the proxy path below.
    if (is_animated_image(src)) {
        // GIF: decode to full-res RGBA frames once (lossless + alpha) and
        // show the frame at the playhead — no lossy mp4 conform / MJPEG.
        if (!video_is_gif(slot)) video_open_gif(slot, src);
        return;
    }
    if (is_image_path(src) && !is_animated_image(src)) {
        if (video_source(slot) != PreviewSource::Still)
            video_open_still(slot, proxy_still_path(src));
        return;
    }
    if (proxy_is_ready(src)) {
        ProxyInfo pi;
        if (proxy_load(src, pi)) {
            video_open_proxy(slot, pi);
            if (slot == 0) state.proxy_ready = true;
            return;
        }
    }
    // No proxy yet — try native (libav direct decode) for instant
    // preview. Falls back to the still placeholder if libav can't open
    // the file (unusual codec, corrupt container).
    if (!video_open_native(slot, src))
        video_open_still(slot, proxy_still_path(src));
}

// Assign a slot for every video-like clip source. Returns unique (slot, src)
// pairs — the work list; sources sharing a slot key appear once.
static std::vector<std::pair<int, std::string>> collect_slot_opens(AppState& state) {
    std::vector<std::pair<int, std::string>> items;
    std::set<int> seen;
    for (auto& tr : state.tracks) {
        for (auto& cl : tr.clips) {
            if (!clip_is_videolike_type(cl.clip_type) || cl.text.empty()) continue;
            // Decode the conformed copy when ready, else the original. (Stills
            // are never conformed, so `src` keeps the .png/.gif extension and the
            // image branch of the opener still fires for them; a conformed clip's
            // `src` is a .mp4 and falls through to the proxy/native video path.)
            std::string src = clip_video_src(state, cl);
            std::string key = clip_slot_key(src, cl.start);
            int slot = slot_for_video(state, key, src);
            if (slot < 0 || seen.count(slot)) continue;
            seen.insert(slot);
            items.push_back({slot, src});
        }
    }
    return items;
}

void reopen_video_slots(AppState& state) {
    for (auto& [slot, src] : collect_slot_opens(state))
        open_video_slot_now(state, slot, src);
}

void queue_video_slot_opens(AppState& state) {
    state.slot_open_queue = collect_slot_opens(state);
    state.slot_open_total = (int)state.slot_open_queue.size();
}

void tick_video_slot_opens(AppState& state, double budget_ms) {
    if (state.slot_open_queue.empty()) return;
    // steady_clock, NOT ImGui::GetTime(): ImGui's clock only advances at
    // NewFrame, so a budget measured with it never trips inside a frame — the
    // "incremental" drain processed the ENTIRE queue in one frame and froze
    // the UI for the sum of every source's open cost.
    auto t0 = std::chrono::steady_clock::now();
    auto ms_since = [](std::chrono::steady_clock::time_point s) {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - s).count();
    };
    size_t i = 0;
    for (; i < state.slot_open_queue.size(); ++i) {
        auto& [slot, src] = state.slot_open_queue[i];
        auto it0 = std::chrono::steady_clock::now();
        open_video_slot_now(state, slot, src);
        double it_ms = ms_since(it0);
        if (it_ms > 50.0)
            fprintf(stderr, "[open] slot %d took %.0f ms: %s\n",
                    slot, it_ms, src.c_str());
        if (ms_since(t0) > budget_ms) { ++i; break; }
    }
    state.slot_open_queue.erase(state.slot_open_queue.begin(),
                                state.slot_open_queue.begin() + (long)i);
    if (state.slot_open_queue.empty()) state.slot_open_total = 0;
}

// ── Frame-rate conform ────────────────────────────────────────────────────────
bool clip_needs_conform(const Clip& cl, int project_fps) {
    if (project_fps <= 0 || cl.src_fps <= 0.f) return false;  // 0=unprobed, -1=still
    // No image is conformed. Stills report a phantom frame rate from ffprobe (a
    // PNG comes back as 25/1) and would be re-encoded into an alpha-stripped,
    // softened mp4; GIFs now render as full-res RGBA frames (video_open_gif) in
    // preview and via libav on export, so the lossy mp4 conform is unwanted there
    // too — it was the main thing degrading GIF quality.
    if (is_image_path(cl.text)) return false;
    return std::fabs(cl.src_fps - (float)project_fps) > (float)project_fps * 0.01f;
}

std::string clip_video_src(const AppState& state, const Clip& cl) {
    if (clip_needs_conform(cl, state.fps) &&
        conform_is_ready(cl.text, state.fps, cl.conform_smooth, cl.clip_loop))
        return conform_path(cl.text, state.fps, cl.conform_smooth, cl.clip_loop);
    return cl.text;
}

// One-time migration: remove the exact old-format sidecars that earlier
// versions wrote next to a source file (now centralized in the media cache).
// Only the unambiguous PMS-generated patterns are touched.
static void migrate_clean_sidecars(const std::string& path) {
    std::error_code ec;
    for (const char* sfx : {".pms_proxy.mjpeg", ".pms_proxy.idx",
                            ".pms_proxy.mjpeg.prog", ".pms_still.jpg"})
        fs::remove(path + sfx, ec);
    fs::path p(path), dir = p.parent_path();
    std::string fname = p.filename().string(), stem = p.stem().string();
    if (fs::exists(dir, ec)) {
        for (auto& e : fs::directory_iterator(dir, ec)) {
            std::string n = e.path().filename().string();
            if (n.rfind(fname + ".pms_conform_", 0) == 0) fs::remove(e.path(), ec);
        }
    }
    fs::remove_all(dir / (stem + "_bg_masks"), ec);
    fs::remove_all(dir / (stem + "_bg_hires"), ec);
}

void conform_tick(AppState& state) {
    bool reopen = false, probed_one = false;
    for (auto& tr : state.tracks) {
        for (auto& cl : tr.clips) {
            if (cl.clip_type != ClipType::Video || cl.text.empty()) continue;
            // Lazy native-fps probe — at most one ffprobe per frame so a load of
            // many clips doesn't hitch.
            // Probe when unprobed (src_fps 0). Also re-probe a loaded looping
            // clip that's missing its duration: src_fps is serialized but
            // src_duration (v46 projects) is not, so a saved GIF needs its loop
            // length recovered. -1 = probed-but-none, so we don't retry forever.
            if ((cl.src_fps == 0.f ||
                 (cl.clip_loop && cl.src_duration == 0.f)) && !probed_one) {
                MediaFileInfo mi = video_probe_file(cl.text);
                cl.src_fps = (mi.fps > 0.0) ? (float)mi.fps : -1.f;
                cl.src_duration = (mi.duration > 0.0) ? (float)mi.duration : -1.f;
                // Animated GIFs are loops by nature — default to seamless conform.
                if (mi.fps > 0.0 && is_animated_image(cl.text)) cl.clip_loop = true;
                migrate_clean_sidecars(cl.text);   // sweep old scattered files
                probed_one = true;
            }
            if (!clip_needs_conform(cl, state.fps)) continue;
            conform_start(cl.text, state.fps, cl.conform_smooth, cl.clip_loop);
            bool ready = conform_is_ready(cl.text, state.fps, cl.conform_smooth, cl.clip_loop);
            if (ready) proxy_start(conform_path(cl.text, state.fps, cl.conform_smooth, cl.clip_loop));
            // Edge-trigger a slot reopen so the preview swaps to the conform the
            // moment it lands (and back to the original if it ever goes away).
            if (ready != cl.conform_ready_cache) { cl.conform_ready_cache = ready; reopen = true; }
        }
    }
    if (reopen) { gc_video_slots(state); reopen_video_slots(state); }
}

float project_end(const AppState& state) {
    float end = 0.f;
    for (auto& tr : state.tracks)
        for (auto& cl : tr.clips)
            if (cl.end > end) end = cl.end;
    return fmaxf(end, 0.01f);
}

bool is_audio_file(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    return ext==".wav"||ext==".mp3"||ext==".m4a"||
           ext==".flac"||ext==".mp4"||ext==".mov"||ext==".aac"||
           ext==".mkv"||ext==".webm";
}

void add_clip_to_track(AppState& state, int ti, const std::string& path, ClipType ct, bool reveal) {
    if (ti < 0 || ti >= (int)state.tracks.size()) return;
    Track& tr = state.tracks[ti];

    // Every video/audio that lands on the timeline is also in the project, so
    // mirror it into the bin. No-op for duplicates, so Browse-then-drag flows
    // don't double-list.
    if ((ct == ClipType::Video || ct == ClipType::Audio) && !path.empty())
        bin_add(state, path);

    Clip cl;
    cl.clip_type = ct;
    cl.start     = state.playhead;
    cl.text      = path;
    cl.source_id = path;

    if (ct == ClipType::Video) {
        float dur = video_probe_duration(path);
        if (dur <= 0.f) dur = 4.f;
        cl.end = cl.start + dur;
        s_source_durations[path] = dur;
        int slot = slot_for_video(state, clip_slot_key(path, cl.start), path);
        proxy_start(path);
        if (slot >= 0) {
            if (proxy_is_ready(path)) {
                ProxyInfo pi;
                if (proxy_load(path, pi)) video_open_proxy(slot, pi);
            } else if (!video_open_native(slot, path)) {
                video_open_still(slot, proxy_still_path(path));
            }
        }
        state.video_loaded = true;
    } else if (ct == ClipType::Audio) {
        AudioMeta meta;
        float dur = audio_probe(path, meta) ? meta.duration_secs : 0.f;
        if (dur <= 0.f) dur = 4.f;
        cl.end = cl.start + dur;
        s_source_durations[path] = dur;
        audio_source_ensure(path);
    } else {
        cl.end = cl.start + 2.f;
    }

    tr.clips.push_back(cl);
    std::sort(tr.clips.begin(), tr.clips.end(),
              [](const Clip& a, const Clip& b){ return a.start < b.start; });
    // Glow the just-placed clip (the sort shifted its index). reveal=false for
    // drops — the user dropped it where they're already looking.
    for (int i = 0; i < (int)tr.clips.size(); ++i)
        if (tr.clips[i].start == cl.start && tr.clips[i].text == cl.text) {
            clip_flash(state, ti, i, reveal); break;
        }

    // Ask draw_timeline to zoom out if the clip extends past the visible right edge.
    // Deferred so it always runs with a valid clip_area_w even on the very first frame.
    state.tl_zoom_to_fit_end = cl.end;

    state.selected_track = ti;
    for (int ci = 0; ci < (int)tr.clips.size(); ++ci)
        if (&tr.clips[ci] == &tr.clips.back() ||
            (fabsf(tr.clips[ci].start - cl.start) < 0.01f &&
             tr.clips[ci].clip_type == ct))
            { state.selected_clip = ci; break; }
    s_panel_view = PanelView::Clip;
    history_push(state, std::string("Add ") +
                        (ct==ClipType::Video ? "video" :
                         ct==ClipType::Audio ? "audio" : "text") + " clip");
}

// ── Panel-view helpers ────────────────────────────────────────────────────────
bool pv_is_lib(PanelView v) {
    return v == PanelView::LibBG    || v == PanelView::LibFX  || v == PanelView::LibAdj ||
           v == PanelView::LibBFX   || v == PanelView::LibAFX || v == PanelView::LibVID ||
           v == PanelView::LibIMG   || v == PanelView::LibAUD || v == PanelView::LibBin ||
           v == PanelView::LibText  || v == PanelView::LibLyric;
}

bool pv_is_override(PanelView v) {
    return v == PanelView::OverrideFX    || v == PanelView::OverrideAdj ||
           v == PanelView::OverrideBG    || v == PanelView::OverrideAudioFX ||
           v == PanelView::OverrideMultiFX ||
           v == PanelView::OverrideAudioMultiFX;
}

bool fx_type_is_audio_fx(FXType ft) {
    return ft == FXType::AudioAutotune   || ft == FXType::AudioPitch  ||
           ft == FXType::AudioFormant    || ft == FXType::AudioDelay  ||
           ft == FXType::AudioReverb     || ft == FXType::AudioVoiceConvert;
}

// One brick's contribution as a standalone AudioFX. VoiceConvert is excluded
// — it's an ML job handled via vc_out_path substitution, not offline DSP.
bool audio_fx_from_brick_pub(const Clip& cl, AudioFX& out) {
    extern bool audio_fx_from_brick_impl(const Clip&, AudioFX&);
    return audio_fx_from_brick_impl(cl, out);
}
bool audio_fx_from_brick_impl(const Clip& cl, AudioFX& out);
static bool audio_fx_from_brick(const Clip& cl, AudioFX& out) {
    return audio_fx_from_brick_impl(cl, out);
}
bool audio_fx_from_brick_impl(const Clip& cl, AudioFX& out) {
    out.mix = cl.audio_fx.mix;   // brick dry/wet applies to every audio FX kind
    switch (cl.fx_type) {
        case FXType::AudioAutotune:
            out.autotune_on    = cl.audio_fx.autotune_on;
            out.autotune_key   = cl.audio_fx.autotune_key;
            out.autotune_scale = cl.audio_fx.autotune_scale;
            out.autotune_speed = cl.audio_fx.autotune_speed;
            return out.autotune_on;
        case FXType::AudioPitch:
            out.pitch_on        = cl.audio_fx.pitch_on;
            out.pitch_semitones = cl.audio_fx.pitch_semitones;
            return out.pitch_on;
        case FXType::AudioFormant:
            out.formant_on    = cl.audio_fx.formant_on;
            out.formant_shift = cl.audio_fx.formant_shift;
            return out.formant_on;
        case FXType::AudioDelay:
            out.delay_on       = cl.audio_fx.delay_on;
            out.delay_time     = cl.audio_fx.delay_time;
            out.delay_feedback = cl.audio_fx.delay_feedback;
            out.delay_mix      = cl.audio_fx.delay_mix;
            return out.delay_on;
        case FXType::AudioReverb:
            out.reverb_on   = cl.audio_fx.reverb_on;
            out.reverb_room = cl.audio_fx.reverb_room;
            out.reverb_damp = cl.audio_fx.reverb_damp;
            out.reverb_mix  = cl.audio_fx.reverb_mix;
            return out.reverb_on;
        default: return false;
    }
}

std::vector<AudioFXSegment> collect_audio_fx_segments(const AppState& state,
                                                      int track_idx,
                                                      const Clip& ac) {
    std::vector<AudioFXSegment> segs;
    if (track_idx < 0 || track_idx >= (int)state.tracks.size()) return segs;
    const float spd = fmaxf(0.01f, ac.speed);

    // Map a timeline window onto the clip's SOURCE time — the FX brick only
    // applies where it overlaps the clip, not to the whole file.
    auto add_window = [&](float tl0, float tl1, const AudioFX& fx) {
        tl0 = fmaxf(tl0, ac.start);
        tl1 = fminf(tl1, ac.end);
        if (tl1 - tl0 < 0.005f) return;
        AudioFXSegment s;
        s.t0 = ac.in_point + (tl0 - ac.start) * spd;
        s.t1 = ac.in_point + (tl1 - ac.start) * spd;
        s.fx = fx;
        segs.push_back(s);
    };

    for (const auto& cl : state.tracks[track_idx].clips) {
        if (&cl == &ac) continue;
        if (cl.end <= ac.start || cl.start >= ac.end) continue;
        if (cl.clip_type == ClipType::Effect && fx_type_is_audio_fx(cl.fx_type)) {
            // Transitional single audio brick (pre-coupling) — legacy overlap.
            AudioFX fx;
            if (audio_fx_from_brick(cl, fx)) add_window(cl.start, cl.end, fx);
        } else if (cl.clip_type == ClipType::AudioMultiFX) {
            // Audio chain brick: one segment per entry, honoring the entry's
            // rel sub-window. Coupled bricks apply only to their host.
            if (cl.fx_coupled) {
                int host = fx_coupled_host(state, track_idx, cl);
                if (host < 0 ||
                    &state.tracks[track_idx].clips[(size_t)host] != &ac) continue;
            }
            for (const auto& se : cl.fx_chain) {
                if (!fx_type_is_audio_fx(se.fx_type)) continue;
                AudioFX fx;
                if (!audio_fx_from_brick(se, fx)) continue;
                float s0 = cl.start + se.rel_start;
                float s1 = (se.rel_end > 0.f) ? cl.start + se.rel_end : cl.end;
                add_window(s0, fminf(s1, cl.end), fx);
            }
        }
    }
    std::sort(segs.begin(), segs.end(),
              [](const AudioFXSegment& a, const AudioFXSegment& b) { return a.t0 < b.t0; });
    return segs;
}

std::vector<AudioBusBrick> collect_bus_bricks(const AppState& state) {
    std::vector<AudioBusBrick> out;
    for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
        const Track& tr = state.tracks[(size_t)ti];
        for (const auto& cl : tr.clips) {
            if (cl.clip_type != ClipType::Bus) continue;
            AudioBusBrick d;
            d.track = ti;
            d.start = cl.start;
            d.end   = cl.end;
            d.gain  = cl.volume;          // the brick's gain
            uint64_t h = 14695981039346656037ull;
            for (const auto& se : cl.fx_chain) {   // audio FX entries
                if (!fx_type_is_audio_fx(se.fx_type)) continue;
                AudioFX fx;
                if (!audio_fx_from_brick(se, fx)) continue;
                d.stages.push_back(fx);
                h = (h ^ audio_fx_hash(fx)) * 1099511628211ull;
            }
            d.hash = d.stages.empty() ? 0 : h;
            out.push_back(d);
        }
    }
    return out;
}

PanelView pv_derive(const AppState& state) {
    bool hs = state.selected_track >= 0 &&
              state.selected_track < (int)state.tracks.size() &&
              state.selected_clip  >= 0 &&
              state.selected_clip  < (int)state.tracks[state.selected_track].clips.size();
    if (!hs) return PanelView::Project;
    const Clip& cl = state.tracks[state.selected_track].clips[state.selected_clip];
    if (cl.clip_type == ClipType::Background) return PanelView::OverrideBG;
    // Coupled bricks have no standalone panel — their chain lives in the
    // host content's FX tab (left-click can't even select them anymore;
    // this guard covers stale selections).
    if (cl.clip_type == ClipType::MultiFX && !cl.fx_coupled)
        return PanelView::OverrideMultiFX;
    if (cl.clip_type == ClipType::MultiFX) return PanelView::Clip;
    if (cl.clip_type == ClipType::AudioMultiFX && !cl.fx_coupled)
        return PanelView::OverrideAudioMultiFX;
    if (cl.clip_type == ClipType::AudioMultiFX) return PanelView::Clip;
    if (cl.clip_type == ClipType::Effect) {
        if (cl.fx_type == FXType::Grade    ||
            cl.fx_type == FXType::Blur     ||
            cl.fx_type == FXType::Vignette)
            return PanelView::OverrideAdj;
        if (fx_type_is_audio_fx(cl.fx_type))
            return PanelView::OverrideAudioFX;
        return PanelView::OverrideFX;
    }
    if (cl.clip_type == ClipType::Text || cl.clip_type == ClipType::Lyrics ||
        cl.clip_type == ClipType::Subtitle) return PanelView::Typography;
    return PanelView::Clip;
}

// ── FX / clip display helpers ─────────────────────────────────────────────────
ImVec4 clip_type_badge_color(ClipType ct) {
    switch (ct) {
        case ClipType::Text:       return {80.f/255,140.f/255,220.f/255,1.f};
        case ClipType::Lyrics:     return {220.f/255,160.f/255,40.f/255,1.f};
        case ClipType::Subtitle:   return {40.f/255,190.f/255,190.f/255,1.f};
        case ClipType::Video:      return {140.f/255,60.f/255,220.f/255,1.f};
        case ClipType::Audio:      return {50.f/255,180.f/255,100.f/255,1.f};
        case ClipType::Background: return {180.f/255,60.f/255,160.f/255,1.f};
        case ClipType::BodyFX:     return {255.f/255,80.f/255,160.f/255,1.f};
        case ClipType::Record:     return {220.f/255,50.f/255,50.f/255,1.f};
        case ClipType::VideoRecord: return {235.f/255,90.f/255,40.f/255,1.f};
        default:                   return {120.f/255,80.f/255,220.f/255,1.f};
    }
}

const char* clip_type_name(ClipType ct) {
    switch (ct) {
        case ClipType::Text:       return "TEXT";
        case ClipType::Lyrics:     return "LYRICS";
        case ClipType::Subtitle:   return "SUBTITLE";
        case ClipType::Video:      return "VIDEO";
        case ClipType::Audio:      return "AUDIO";
        case ClipType::Background: return "BG";
        case ClipType::BodyFX:     return "BODY FX";
        case ClipType::Record:     return "REC";
        case ClipType::VideoRecord: return "CAM";
        default:                   return "ADJUST";
    }
}

ImU32 fx_type_accent(FXType ft) {
    switch (ft) {
        case FXType::Glitch:     return IM_COL32(0,210,220,255);
        case FXType::ZoomPunch:  return IM_COL32(255,135,40,255);
        case FXType::LUT:        return IM_COL32(255,205,55,255);
        case FXType::LightLeak:  return IM_COL32(255,90,160,255);
        case FXType::VHS:        return IM_COL32(110,195,95,255);
        case FXType::Datamosh:   return IM_COL32(255,60,100,255);
        case FXType::ChromaKey:  return IM_COL32(90,200,180,255);   // the clean keyer
        case FXType::ChromaMelt: return IM_COL32(180,90,230,255);   // trippy melt — keyed feedback smear
        case FXType::ChromaEcho: return IM_COL32(230,120,90,255);   // keyed feedback echo — stacked frames
        case FXType::ChromaFrame: return IM_COL32(255,180,60,255);  // keyed multi-tap frame echoes
#include "generated/fx_ui_color.h"
        default:                return IM_COL32(120,80,220,255);
    }
}

const char* fx_type_name(FXType ft) {
    switch (ft) {
        case FXType::Glitch:    return "GLITCH";
        case FXType::ZoomPunch: return "ZOOM";
        case FXType::LUT:       return "LUT";
        case FXType::LightLeak: return "LEAK";
        case FXType::VHS:       return "VHS";
        case FXType::Datamosh:  return "MOSH";
        case FXType::ChromaKey: return "KEY";
        case FXType::ChromaMelt: return "MELT";
        case FXType::ChromaEcho: return "ECHO";
        case FXType::ChromaFrame: return "FRAME";
        case FXType::Grade:              return "Grade";
        case FXType::Blur:               return "Blur";
        case FXType::Vignette:           return "Vignette";
        case FXType::AudioAutotune:      return "TUNE";
        case FXType::AudioPitch:         return "PITCH";
        case FXType::AudioFormant:       return "FORM";
        case FXType::AudioDelay:         return "DELAY";
        case FXType::AudioReverb:        return "VERB";
        case FXType::AudioVoiceConvert:  return "VOICE";
#include "generated/fx_ui_abbrev.h"
        default:                         return "FX";
    }
}

const char* fx_type_display(FXType ft) {
    switch (ft) {
        case FXType::Glitch:    return "Glitch";
        case FXType::ZoomPunch: return "Zoom Punch";
        case FXType::LUT:       return "LUT Grade";
        case FXType::LightLeak: return "Light Leak";
        case FXType::VHS:       return "VHS";
        case FXType::Datamosh:  return "Datamosh";
        case FXType::ChromaKey:  return "Chroma Key";
        case FXType::ChromaMelt: return "Chroma Melt";
        case FXType::ChromaEcho: return "Chroma Echo";
        case FXType::ChromaFrame: return "Chroma Frame";
#include "generated/fx_ui_label.h"
        default:                return "FX";
    }
}

ImU32 clip_badge_color(const Clip& c) {
    if (c.clip_type == ClipType::Effect)  return fx_type_accent(c.fx_type);
    if (c.clip_type == ClipType::MultiFX) return IM_COL32(210, 110, 30, 220);
    return ImGui::ColorConvertFloat4ToU32(clip_type_badge_color(c.clip_type));
}

const char* clip_display_name(const Clip& c) {
    if (c.clip_type == ClipType::Effect)  return fx_type_name(c.fx_type);
    if (c.clip_type == ClipType::MultiFX) return "MULTI";
    // Capture IMG brick is a camera brick in photo mode — badge it as IMG so it
    // reads distinctly from the Video Record "CAM" brick.
    if (c.clip_type == ClipType::VideoRecord && c.rec_photo) return "IMG";
    return clip_type_name(c.clip_type);
}

bool fx_type_is_adjustment_style(FXType ft) {
    switch (ft) {
        case FXType::Duotone:          case FXType::Posterize:      case FXType::BleachBypass:
        case FXType::ColorBurn:        case FXType::Solarize:       case FXType::Daguerreotype:
        case FXType::XRay:             case FXType::MiamiVice:      case FXType::HorrorGrade:
        case FXType::SplitToning:      case FXType::DesertGold:     case FXType::GradientMap:
        case FXType::CrossProcess:     case FXType::Technicolor:    case FXType::Kodachrome:
        case FXType::SepiaRich:        case FXType::ColorDodge:     case FXType::InfraredFilm:
        case FXType::Thermal:          case FXType::ThermalMap:     case FXType::VintageNegative:
        case FXType::GoldenHour:       case FXType::CyberpunkGrade: case FXType::ZoneSystemBw:
        case FXType::WarholPop:        case FXType::DitherBayer:    case FXType::BitCrush:
            return true;
        default: return false;
    }
}

void add_record_brick(AppState& state) {
    float qfps = tl_fps(state);
    if (!(qfps > 0.f)) qfps = 30.f;
    Clip cl;
    cl.clip_type = ClipType::Record;
    cl.start     = snap_to_frame(state.playhead, (int)qfps);
    cl.end       = snap_end_to_frame(cl.start + 8.f, (int)qfps);
    Track t;
    char n[32];
    snprintf(n, sizeof(n), "Record %d", (int)state.tracks.size() + 1);
    t.name = n;
    float clip_end = cl.end;
    t.clips.push_back(std::move(cl));
    state.tracks.insert(state.tracks.begin(), std::move(t));
    state.selected_track = 0;
    state.selected_clip  = 0;
    clip_flash(state, 0, 0, /*reveal=*/true);
    // If the brick lands past the current view, zoom out to fit it.
    state.tl_zoom_to_fit_end = fmaxf(state.tl_zoom_to_fit_end, clip_end);
    history_push(state, "Add Record Brick");
}

void add_video_record_brick(AppState& state) {
    float qfps = tl_fps(state);
    if (!(qfps > 0.f)) qfps = 30.f;

    // Record A/V pair: the camera brick arrives WITH a mic twin on the track
    // below, both stamped with a fresh pair id. Recording the camera runs both
    // recorders on the shared loop clock — cycle N of one is cycle N of the
    // other, so their take trays pair by index (and the best video take can
    // ship with the best audio take). Don't want sound? Delete the MIC brick.
    int pair_id = 0;
    for (auto& tr : state.tracks)
        for (auto& c : tr.clips)
            if (c.rec_pair_id > pair_id) pair_id = c.rec_pair_id;
    ++pair_id;
    // The pair is also a selection GROUP: clicking either brick selects both,
    // and dragging moves them together — they record in lockstep, so they
    // should stay aligned on the timeline too.
    int gid = 0;
    for (auto& tr : state.tracks)
        for (auto& c : tr.clips)
            if (c.group_id > gid) gid = c.group_id;
    ++gid;

    Clip cl;
    cl.clip_type   = ClipType::VideoRecord;
    cl.rec_pair_id = pair_id;
    cl.group_id    = gid;
    cl.start       = snap_to_frame(state.playhead, (int)qfps);
    cl.end         = snap_end_to_frame(cl.start + 8.f, (int)qfps);
    Clip mic;
    mic.clip_type   = ClipType::Record;
    mic.rec_pair_id = pair_id;
    mic.group_id    = gid;
    mic.start       = cl.start;
    mic.end         = cl.end;

    int idx = (int)state.tracks.size() + 1;
    Track t;
    char n[32];
    snprintf(n, sizeof(n), "Camera %d", idx);
    t.name = n;
    float clip_end = cl.end;
    t.clips.push_back(std::move(cl));
    Track tm;
    snprintf(n, sizeof(n), "Cam Mic %d", idx);
    tm.name = n;
    tm.clips.push_back(std::move(mic));
    // The pair lives in a folder row (track group) named after the camera —
    // collapse it to one line, mute/lock both from the header, drag as one.
    Track head;
    snprintf(n, sizeof(n), "Camera %d (A/V)", idx);
    head.name           = n;
    head.kind           = TrackKind::GroupHead;
    head.group_children = 2;
    state.tracks.insert(state.tracks.begin(), std::move(tm));
    state.tracks.insert(state.tracks.begin(), std::move(t));
    state.tracks.insert(state.tracks.begin(), std::move(head));
    state.selected_track = 1;
    state.selected_clip  = 0;
    clip_flash(state, 1, 0, /*reveal=*/true);
    // If the brick lands past the current view, zoom out to fit it.
    state.tl_zoom_to_fit_end = fmaxf(state.tl_zoom_to_fit_end, clip_end);
    history_push(state, "Add Record A/V Bricks");
}

void add_photo_capture_brick(AppState& state) {
    float qfps = tl_fps(state);
    if (!(qfps > 0.f)) qfps = 30.f;
    Clip cl;
    cl.clip_type = ClipType::VideoRecord;   // camera brick, in photo mode
    cl.rec_photo = true;
    cl.start     = snap_to_frame(state.playhead, (int)qfps);
    cl.end       = snap_end_to_frame(cl.start + 4.f, (int)qfps);
    Track t;
    char n[32];
    snprintf(n, sizeof(n), "Photo %d", (int)state.tracks.size() + 1);
    t.name = n;
    float clip_end = cl.end;
    t.clips.push_back(std::move(cl));
    state.tracks.insert(state.tracks.begin(), std::move(t));
    state.selected_track = 0;
    state.selected_clip  = 0;
    clip_flash(state, 0, 0, /*reveal=*/true);
    state.tl_zoom_to_fit_end = fmaxf(state.tl_zoom_to_fit_end, clip_end);
    history_push(state, "Add Capture IMG Brick");
}

void add_bus_brick(AppState& state) {
    float qfps = tl_fps(state);
    if (!(qfps > 0.f)) qfps = 30.f;
    Clip cl;
    cl.clip_type = ClipType::Bus;
    cl.start     = 0.f;
    cl.end       = snap_end_to_frame(fmaxf(8.f, state.duration), (int)qfps);
    cl.volume    = 1.f;                       // gain (1.0 = unity)
    Track t;
    char n[32];
    snprintf(n, sizeof(n), "Bus %d", (int)state.tracks.size() + 1);
    t.name = n;
    float clip_end = cl.end;
    t.clips.push_back(std::move(cl));
    state.tracks.insert(state.tracks.begin(), std::move(t));  // top → groups every track below
    state.selected_track = 0;
    state.selected_clip  = 0;
    clip_flash(state, 0, 0, /*reveal=*/true);
    state.tl_zoom_to_fit_end = fmaxf(state.tl_zoom_to_fit_end, clip_end);
    history_push(state, "Add Bus Brick");
}

// Rescale a media clip's own timeline width AND any FX bricks (Effect / BodyFX
// / MultiFX / Background) that overlap it, anchored at the clip's start. Speed
// ratio = new_speed / old_speed; widths divide by it so the clip plays the
// same source content in less/more wall-clock time and brick boundaries stay
// locked to the same source-content moments. MultiFX sub-effects'
// rel_start/rel_end scale with their parent so internal timing stays coherent.
void rescale_glass_bricks(AppState& state, int media_ti, int media_ci, float speed_ratio) {
    if (speed_ratio <= 0.f || !std::isfinite(speed_ratio)) return;
    if (fabsf(speed_ratio - 1.f) < 1e-5f) return;
    if (media_ti < 0 || media_ti >= (int)state.tracks.size()) return;
    Track& mtr = state.tracks[media_ti];
    if (media_ci < 0 || media_ci >= (int)mtr.clips.size()) return;
    const Clip media = mtr.clips[media_ci];  // snapshot — old start/end used for overlap test
    float anchor = media.start;
    for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
        Track& tr = state.tracks[ti];
        for (auto& cl : tr.clips) {
            if (ti == media_ti && &cl == &mtr.clips[media_ci]) continue;
            if (cl.clip_type != ClipType::Effect &&
                cl.clip_type != ClipType::BodyFX &&
                cl.clip_type != ClipType::MultiFX &&
                cl.clip_type != ClipType::Background) continue;
            if (cl.start >= media.end || cl.end <= media.start) continue;
            cl.start = anchor + (cl.start - anchor) / speed_ratio;
            cl.end   = anchor + (cl.end   - anchor) / speed_ratio;
            for (auto& se : cl.fx_chain) {
                se.rel_start /= speed_ratio;
                if (se.rel_end > 0.f) se.rel_end /= speed_ratio;
            }
        }
    }
    // Resize the media clip itself so its timeline width matches the new
    // playback duration (source_dur / speed). Anchored at the start so nothing
    // upstream shifts.
    Clip& m = mtr.clips[media_ci];
    m.end = anchor + (m.end - anchor) / speed_ratio;
}

FxBrickColors fx_brick_colors(FXType ft, bool sel) {
    int r, g, b;
    if (ft == FXType::Grade || ft == FXType::Blur || ft == FXType::Vignette ||
        fx_type_is_adjustment_style(ft)) { r=100; g=80;  b=200; }
    else if (fx_type_is_audio_fx(ft))    { r=30;  g=180; b=140; }  // teal — audio
    else                                 { r=210; g=110; b=30;  }
    if (sel) {
        int rb = (int)fminf(r*1.25f,255), gb2 = (int)fminf(g*1.25f,255), bb = (int)fminf(b*1.25f,255);
        return { IM_COL32(r,g,b,210), IM_COL32(rb,gb2,bb,255), IM_COL32(240,240,255,240) };
    }
    return { IM_COL32(r/4,g/4,b/4,130), IM_COL32(r*2/3,g*2/3,b*2/3,200), IM_COL32(r,g,b,220) };
}
