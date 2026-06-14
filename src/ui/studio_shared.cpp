#include "studio_types.h"
#include "studio_shared.h"
#include "panel_media.h"
#include "app.h"
#include "audio.h"
#include "video.h"
#include "proxy.h"
#include "conform.h"
#include "history.h"
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
    // The timeline frame grid. Default to the project (timeline/export) fps —
    // that's the rate the playhead, frame-stepping and last_playable_time should
    // snap to. The MJPEG preview proxy is fps-capped (and some are coarse — a
    // 10 fps proxy would otherwise drag the playhead onto a 0.1 s grid and park
    // it short of the clip's true end), so only let the proxy's rate RAISE the
    // grid when it's genuinely finer, never lower it.
    float tf = (float)state.fps;
    if (!(tf > 0.f)) tf = 30.f;
    if (state.proxy_ready) {
        double vf = video_info(0).fps;
        if (vf > (double)tf) tf = (float)vf;
    }
    return tf;
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

bool kf_slider(AppState& state, Clip& clip, int sel_ti, int sel_ci, float w,
               const char* prop, const char* label, float* val_ptr,
               float vmin, float vmax, const char* fmt,
               float disp, const char* prop2) {
    float t_local = state.playhead - clip.start;
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
            history_push(state, std::string("Add KF ") + prop);
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
    if (ImGui::SliderFloat(sid, &dv, vmin, vmax, fmt)) {
        changed = true;
        float raw = dv / disp;
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
    }
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, std::string("Edit ") + prop);
    return changed;
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
    for (int i = 0; i < MAX_VIDEO_TRACKS; ++i)
        if (state.proxy_paths[i] == key) return i;
    for (int i = 0; i < MAX_VIDEO_TRACKS; ++i)
        if (state.proxy_paths[i].empty()) { state.proxy_paths[i] = key; return i; }
    fprintf(stderr, "[video] slot table full (%d slots) — proxy will not load for: %s\n",
            MAX_VIDEO_TRACKS, key.c_str());
    return -1;
}

void gc_video_slots(AppState& state) {
    std::set<std::string> live;
    for (auto& tr : state.tracks)
        for (auto& cl : tr.clips)
            if (clip_is_videolike_type(cl.clip_type) && !cl.text.empty())
                live.insert(clip_slot_key(clip_video_src(state, cl), cl.start));
    for (int i = 0; i < MAX_VIDEO_TRACKS; ++i) {
        if (!state.proxy_paths[i].empty() && !live.count(state.proxy_paths[i])) {
            video_close(i);
            state.proxy_paths[i].clear();
        }
    }
}

void reopen_video_slots(AppState& state) {
    for (auto& tr : state.tracks) {
        for (auto& cl : tr.clips) {
            if (!clip_is_videolike_type(cl.clip_type) || cl.text.empty()) continue;
            // Decode the conformed copy when ready, else the original. (Stills
            // are never conformed, so `src` keeps the .png/.gif extension and the
            // image branch below still fires for them; a conformed clip's `src`
            // is a .mp4 and falls through to the proxy/native video path.)
            std::string src = clip_video_src(state, cl);
            std::string key = clip_slot_key(src, cl.start);
            int slot = slot_for_video(state, key, src);
            if (slot < 0) continue;
            // Still images go straight to Still — never native. libav happily
            // opens a PNG as a one-frame video, but then any decode past t=0
            // fails and the clip renders blank (this is how MCP-added images
            // vanished from the preview: add_clip → proxy_scan → native PNG).
            // Animated images (.gif) fall through to the proxy path below.
            if (is_image_path(src) && !is_animated_image(src)) {
                if (video_source(slot) != PreviewSource::Still)
                    video_open_still(slot, proxy_still_path(src));
                continue;
            }
            if (proxy_is_ready(src)) {
                ProxyInfo pi;
                if (proxy_load(src, pi)) {
                    video_open_proxy(slot, pi);
                    if (slot == 0) state.proxy_ready = true;
                    continue;
                }
            }
            // No proxy yet — try native (libav direct decode) for instant
            // preview. Falls back to the still placeholder if libav can't open
            // the file (unusual codec, corrupt container).
            if (!video_open_native(slot, src))
                video_open_still(slot, proxy_still_path(src));
        }
    }
}

// ── Frame-rate conform ────────────────────────────────────────────────────────
bool clip_needs_conform(const Clip& cl, int project_fps) {
    if (project_fps <= 0 || cl.src_fps <= 0.f) return false;  // 0=unprobed, -1=still
    return std::fabs(cl.src_fps - (float)project_fps) > (float)project_fps * 0.01f;
}

std::string clip_video_src(const AppState& state, const Clip& cl) {
    if (clip_needs_conform(cl, state.fps) &&
        conform_is_ready(cl.text, state.fps, cl.conform_smooth, cl.clip_loop))
        return conform_path(cl.text, state.fps, cl.conform_smooth, cl.clip_loop);
    return cl.text;
}

void conform_tick(AppState& state) {
    bool reopen = false, probed_one = false;
    for (auto& tr : state.tracks) {
        for (auto& cl : tr.clips) {
            if (cl.clip_type != ClipType::Video || cl.text.empty()) continue;
            // Lazy native-fps probe — at most one ffprobe per frame so a load of
            // many clips doesn't hitch.
            if (cl.src_fps == 0.f && !probed_one) {
                MediaFileInfo mi = video_probe_file(cl.text);
                cl.src_fps = (mi.fps > 0.0) ? (float)mi.fps : -1.f;
                // Animated GIFs are loops by nature — default to seamless conform.
                if (mi.fps > 0.0 && is_animated_image(cl.text)) cl.clip_loop = true;
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

void add_clip_to_track(AppState& state, int ti, const std::string& path, ClipType ct) {
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
           v == PanelView::LibText;
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

std::vector<AudioBusDesc> collect_bus_descs(const AppState& state) {
    std::vector<AudioBusDesc> out;
    for (const auto& b : state.buses) {
        AudioBusDesc d;
        d.gain = b.gain;
        uint64_t h = 14695981039346656037ull;
        for (const auto& se : b.fx_chain) {
            if (!fx_type_is_audio_fx(se.fx_type)) continue;
            AudioFX fx;
            if (!audio_fx_from_brick(se, fx)) continue;
            d.stages.push_back(fx);
            h = (h ^ audio_fx_hash(fx)) * 1099511628211ull;
        }
        d.hash = d.stages.empty() ? 0 : h;
        out.push_back(d);
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
        case FXType::ChromaKey:  return IM_COL32(50,220,120,255);
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
        case FXType::ChromaKey: return "Chroma Key";
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
    // If the brick lands past the current view, zoom out to fit it.
    state.tl_zoom_to_fit_end = fmaxf(state.tl_zoom_to_fit_end, clip_end);
    history_push(state, "Add Record Brick");
}

void add_video_record_brick(AppState& state) {
    float qfps = tl_fps(state);
    if (!(qfps > 0.f)) qfps = 30.f;
    Clip cl;
    cl.clip_type = ClipType::VideoRecord;
    cl.start     = snap_to_frame(state.playhead, (int)qfps);
    cl.end       = snap_end_to_frame(cl.start + 8.f, (int)qfps);
    Track t;
    char n[32];
    snprintf(n, sizeof(n), "Camera %d", (int)state.tracks.size() + 1);
    t.name = n;
    float clip_end = cl.end;
    t.clips.push_back(std::move(cl));
    state.tracks.insert(state.tracks.begin(), std::move(t));
    state.selected_track = 0;
    state.selected_clip  = 0;
    // If the brick lands past the current view, zoom out to fit it.
    state.tl_zoom_to_fit_end = fmaxf(state.tl_zoom_to_fit_end, clip_end);
    history_push(state, "Add Record Brick");
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
