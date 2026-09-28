// Typography-as-Script-clips: lay/replace the ONE Script clip
// (`pms:typography/<id>`) that renders the transcript words.
#include "typography_script.h"

// App hook: surface the Typography tab after laying the layer. Registered by
// the desktop app; headless/iOS engine builds leave it null (no-op).
static void (*g_focus_typography_hook)() = nullptr;
void set_focus_typography_hook(void (*fn)()) { g_focus_typography_hook = fn; }
void app_focus_typography_panel() { if (g_focus_typography_hook) g_focus_typography_hook(); }
#include "app.h"
#include "engine_seams.h"
#include "history.h"
#include "script_runtime.h"  // script_resolve_spec
#include "script_clip.h"     // script_clip_key/invalidate
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

bool lay_typography_script(AppState& state, const std::string& preset,
                           const std::string& params_json, std::string& err) {
    // Validate the preset id against the shipped JS modules (must exist on
    // disk — script_resolve_spec maps any pms:typography/<id> syntactically).
    std::string resolved = script_resolve_spec("pms:typography/" + preset, "", "");
    if (resolved.empty() || !std::filesystem::exists(resolved)) {
        err = "unknown typography preset '" + preset + "'";
        return false;
    }
    if (state.words_cache.empty()) { err = "no transcript words — transcribe or set_transcript first"; return false; }
    if (state.audio_path.empty()) { err = "no audio source on project"; return false; }

    // Words are source seconds; map to timeline seconds through the timeline
    // clip that plays the source (same mapping as pms.audio: offset =
    // in_point - start of the first Audio/Video clip whose text/source_id
    // equals the source).
    double off = 0.0;
    for (auto& tr : state.tracks) {
        bool found = false;
        for (auto& cl : tr.clips) {
            if (cl.clip_type != ClipType::Audio && cl.clip_type != ClipType::Video)
                continue;
            if (cl.text == state.audio_path || cl.source_id == state.audio_path) {
                off = (double)cl.in_point - (double)cl.start;
                found = true;
                break;
            }
        }
        if (found) break;
    }
    float t0 = 1e9f, t1 = -1e9f;
    for (auto& we : state.words_cache) {
        t0 = std::min(t0, (float)((double)we.start - off));
        t1 = std::max(t1, (float)((double)we.end - off));
    }
    if (!(t1 > t0)) { err = "transcript words have an empty time range"; return false; }
    t0 = snap_to_frame(t0, state.fps);
    t1 = snap_end_to_frame(t1, state.fps);
    if (!(t1 > t0)) t1 = t0 + 1.f / std::max(1, state.fps);

    // The typography layer lives on a dedicated track (kind Lyrics so the
    // timeline treats it as the lyrics lane). Reuse it across preset swaps;
    // replace any previous typography Script clip (keep foreign clips alone).
    int typo_ti = -1;
    for (int i = 0; i < (int)state.tracks.size(); ++i)
        if (is_lyrics_track(state.tracks[i])) { typo_ti = i; break; }
    if (typo_ti < 0) {
        Track lt;
        lt.name = "Typography";
        lt.kind = TrackKind::Lyrics;
        state.tracks.insert(state.tracks.begin(), std::move(lt));
        typo_ti = 0;
    }
    Track& track = state.tracks[typo_ti];
    track.clips.erase(std::remove_if(track.clips.begin(), track.clips.end(),
        [](const Clip& c) {
            return c.clip_type == ClipType::Script &&
                   c.script_path.rfind("pms:typography/", 0) == 0;
        }), track.clips.end());

    Clip cl;
    cl.clip_type = ClipType::Script;
    cl.start = t0;
    cl.end = t1;
    cl.script_path = "pms:typography/" + preset;
    cl.script_params = params_json.empty() ? "{}" : params_json;
    track.clips.push_back(std::move(cl));
    std::sort(track.clips.begin(), track.clips.end(),
              [](const Clip& a, const Clip& b) { return a.start < b.start; });
    int ci = -1;
    for (int i = 0; i < (int)track.clips.size(); ++i)
        if (track.clips[i].clip_type == ClipType::Script &&
            track.clips[i].script_path == "pms:typography/" + preset) ci = i;
    state.selected_track = typo_ti;
    state.selected_clip = ci;
    script_clip_invalidate(script_clip_key(typo_ti, ci));
    app_focus_typography_panel();
    history_push(state, std::string("Typography — ") + preset);
    return true;
}

std::string active_typography_preset(const AppState& state) {
    for (auto& tr : state.tracks)
        for (auto& c : tr.clips)
            if (c.clip_type == ClipType::Script &&
                c.script_path.rfind("pms:typography/", 0) == 0)
                return c.script_path.substr(strlen("pms:typography/"));
    return "";
}
