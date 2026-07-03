// timeline_core.cpp — engine-side timeline/project logic hoisted out of the
// desktop UI layer (ui/studio_shared.cpp, ui/timeline.cpp) during the iOS
// engine extraction (docs/IOS_PORT_PLAN.md Phase 0). Behavior is unchanged;
// only the home of the code moved. Declarations: engine_seams.h (until the
// seam empties into a real engine header).
#include "engine_seams.h"
#include "app.h"
#include "audio.h"
#include "audio_fx.h"
#include "history.h"
#include "paths.h"
#include "proxy.h"
#include "conform.h"
#include "project.h"
#include "video.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <system_error>
namespace fs = std::filesystem;

// Forward decls: hoist order kept callers above callees.
bool audio_fx_from_brick_impl(const Clip& cl, AudioFX& out);
static bool audio_fx_from_brick(const Clip& cl, AudioFX& out);

static bool is_chain_brick(const Clip& c) {
    return c.clip_type == ClipType::MultiFX ||
           c.clip_type == ClipType::AudioMultiFX;
}


// ── Playback helpers ──────────────────────────────────────────────────────────
float snap_to_frame(const AppState& state, float t) {
    float fps = tl_fps(state);
    if (!(fps > 0.f)) fps = 30.f;
    return roundf(t * fps) / fps;
}

int group_head_of(const AppState& state, int ti) {
    for (int h = 0; h < (int)state.tracks.size(); ++h) {
        const Track& t = state.tracks[(size_t)h];
        if (!is_group_head(t)) continue;
        if (ti > h && ti <= h + t.group_children) return h;
    }
    return -1;
}

std::string clip_video_src(const AppState& state, const Clip& cl) {
    if (clip_needs_conform(cl, state.fps) &&
        conform_is_ready(cl.text, state.fps, cl.conform_smooth, cl.clip_loop))
        return conform_path(cl.text, state.fps, cl.conform_smooth, cl.clip_loop);
    return cl.text;
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

void mark_project_saved(AppState& state, const std::string& path) {
    state.saved_history_pos = history_pos();
    if (!path.empty()) state.thumb_request = project_thumb_path(path);
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

float tl_fps(const AppState& state) {
    // The timeline frame grid = the project/export fps — the ONE grid that the ruler
    // ticks, all snapping, and the normalize pass land on, so nothing ever sits
    // between frames. A source video's higher native rate does NOT raise the grid:
    // you cut and export at the project fps, so that's the grid you see and snap to.
    float tf = (float)state.fps;
    if (!(tf > 0.f)) tf = 30.f;
    return tf;
}

static bool audio_fx_from_brick(const Clip& cl, AudioFX& out) {
    return audio_fx_from_brick_impl(cl, out);
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

int find_empty_track(const AppState& state) {
    for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
        const Track& t = state.tracks[ti];
        if (is_group_head(t)) continue;   // folder rows hold no clips
        if (t.clips.empty() && t.visible && !t.locked && !t.managed) return ti;
    }
    return -1;
}

// ── Clip / slot helpers ───────────────────────────────────────────────────────
std::string clip_slot_key(const std::string& src, float /*start*/) {
    return src;
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

void mark_project_clean(AppState& state) {
    state.saved_history_pos = history_pos();
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

void queue_video_slot_opens(AppState& state) {
    state.slot_open_queue = collect_slot_opens(state);
    state.slot_open_total = (int)state.slot_open_queue.size();
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

void reopen_video_slots(AppState& state) {
    for (auto& [slot, src] : collect_slot_opens(state))
        open_video_slot_now(state, slot, src);
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

float last_playable_time(const AppState& state) {
    if (state.duration <= 0.f) return 0.f;
    float fps = tl_fps(state);
    if (!(fps > 0.f)) fps = 30.f;
    // Start time of the last whole frame: ceil(duration*fps) is the frame count,
    // minus one → last index, /fps → its start. Always strictly inside [0,duration).
    float lf = (ceilf(state.duration * fps) - 1.f) / fps;
    return fmaxf(0.f, lf);
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
