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
#include <fstream>

// ── Preset FX as global Effect bricks ─────────────────────────────────────
// Presets with FX entries (VHS/FilmGrain/Scanlines/ChromaticAberration) lay
// real Effect bricks that affect everything below the lyrics track — the same
// bricks typography_core.cpp laid (same types, params, beat sync, track
// placement/tagging). The Script clip itself applies no post pass, so nothing
// is applied twice.
constexpr const char* TYPO_FX_TAG = "__typo_fx__";

struct TypoFXDesc {
    FXType type;
    float  beat_intensity = 0.f;
};

// Read the preset's "fx" array from its shipped JS module config. Returns
// false only when the module can't be read (unknown preset is reported by the
// caller); a missing/empty fx array yields zero entries.
static bool typo_fx_for_preset(const std::string& preset, std::vector<TypoFXDesc>& out) {
    out.clear();
    std::string resolved = script_resolve_spec("pms:typography/" + preset, "", "");
    if (resolved.empty()) return false;
    std::ifstream f(resolved, std::ios::binary);
    if (!f) return false;
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t k = src.find("\"fx\"");
    if (k == std::string::npos) return true;
    k = src.find('[', k);
    if (k == std::string::npos) return true;
    int depth = 0;
    size_t e = k;
    for (; e < src.size(); ++e) {
        if (src[e] == '[') ++depth;
        else if (src[e] == ']') { if (--depth == 0) break; }
    }
    std::string arr = src.substr(k, e - k + 1);
    size_t p = 0;
    while ((p = arr.find("\"type\"", p)) != std::string::npos) {
        size_t c = arr.find(':', p);
        size_t q1 = arr.find('"', c);
        if (q1 == std::string::npos) break;
        size_t q2 = arr.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string type = arr.substr(q1 + 1, q2 - q1 - 1);
        // The beat value belongs to this object only (bounded by the next
        // "type" so a missing beat can't steal the next object's).
        size_t nt = arr.find("\"type\"", q2 + 1);
        float beat = 0.f;
        size_t b = arr.find("\"beat\"", q2);
        if (b != std::string::npos && (nt == std::string::npos || b < nt)) {
            size_t bc = arr.find(':', b);
            if (bc != std::string::npos) beat = (float)atof(arr.c_str() + bc + 1);
        }
        TypoFXDesc d;
        if (type == "VHS") d.type = FXType::VHS;
        else if (type == "FilmGrain") d.type = FXType::FilmGrain;
        else if (type == "Scanlines") d.type = FXType::Scanlines;
        else if (type == "ChromaticAberration") d.type = FXType::ChromaticAberration;
        else { p = q2 + 1; continue; }  // unknown entry: skip, don't fail
        d.beat_intensity = beat;
        out.push_back(d);
        p = q2 + 1;
    }
    return true;
}

// Lay (or clear) the preset's global FX bricks on a managed LyricsFX track
// directly above the lyrics track. Re-applying a preset replaces its bricks;
// a preset without FX clears any previously laid ones so no stale global
// effect survives a preset swap.
static void lay_typo_fx_clips(AppState& state, const std::vector<TypoFXDesc>& fx,
                              int typo_ti, float t1) {
    const std::string fx_tag = std::string(TYPO_FX_TAG) + state.audio_path;
    for (auto& t : state.tracks)
        t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(),
            [&](const Clip& c) { return c.source_id == fx_tag; }), t.clips.end());
    if (fx.empty()) return;

    // Beat source: first clip with analyzed beats (same rule as native).
    int beat_ti = -1, beat_ci = -1;
    for (int ti = 0; ti < (int)state.tracks.size() && beat_ti < 0; ++ti)
        for (int ci = 0; ci < (int)state.tracks[ti].clips.size() && beat_ti < 0; ++ci) {
            auto& cl = state.tracks[ti].clips[ci];
            if (!cl.beats.empty()) { beat_ti = ti; beat_ci = ci; }
        }

    int fx_ti = -1;
    if (typo_ti + 1 < (int)state.tracks.size() &&
        state.tracks[typo_ti + 1].kind == TrackKind::LyricsFX)
        fx_ti = typo_ti + 1;
    else {
        Track ft; ft.name = "Lyrics FX"; ft.managed = true; ft.kind = TrackKind::LyricsFX;
        state.tracks.insert(state.tracks.begin() + typo_ti + 1, std::move(ft));
        fx_ti = typo_ti + 1;
        if (beat_ti > typo_ti) beat_ti++;  // insert shifted tracks below
    }

    for (auto& fd : fx) {
        Clip fc;
        fc.clip_type = ClipType::Effect;
        fc.fx_type   = fd.type;
        fc.source_id = fx_tag;
        fc.start     = 0.f;
        fc.end       = fmaxf(t1, 10.f);
        fc.beat_src_track = beat_ti;
        fc.beat_src_clip  = beat_ci;
        switch (fd.type) {
            case FXType::ChromaticAberration:
                fc.fx_chromatic_aberration_amount = 0.6f;
                break;
            case FXType::FilmGrain:
                fc.fx_film_grain_amount    = 1.f;
                fc.fx_film_grain_intensity = fd.beat_intensity > 0.001f ? 0.8f : 0.35f;
                fc.fx_film_grain_size      = 1.2f;
                if (fd.beat_intensity > 0.001f)
                    fc.fx_film_grain_intensity_beat = fd.beat_intensity;
                break;
            case FXType::Scanlines:
                fc.fx_scanlines_amount  = 0.5f;
                fc.fx_scanlines_density = 0.5f;
                if (fd.beat_intensity > 0.001f)
                    fc.fx_scanlines_density_beat = fd.beat_intensity;
                break;
            case FXType::VHS:
                fc.fx_vhs_noise    = 0.35f;
                fc.fx_vhs_bleed    = 4.f;
                fc.fx_vhs_tracking = 0.15f;
                break;
            default: break;
        }
        state.tracks[fx_ti].clips.push_back(std::move(fc));
    }
}


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
    // One frame of headroom before the first word: frame-quantised renders
    // (f.t = frame/fps) and raw-t clip activity otherwise disagree exactly on
    // the boundary, blanking enter frames. Mirrors the native generator,
    // whose latency shift + snapping also started clips ~a frame early.
    t0 = std::max(0.f, t0 - 1.f / std::max(1, state.fps));

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
    std::vector<TypoFXDesc> fx;
    typo_fx_for_preset(preset, fx);
    lay_typo_fx_clips(state, fx, typo_ti, t1);
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
