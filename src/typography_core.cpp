// typography_core.cpp — lyric typography generation hoisted from
// ui/panel_animation.cpp during the iOS engine extraction (Phase 0):
// generate_typography rebuilds the lyrics track's text bricks from the word
// cache using the active preset; apply_typo_style stamps a preset onto one
// clip. The Typography PANEL (styling UI) stays app-side and calls these.
#include "engine_seams.h"
#include "pipeline_core.h"
#include "app.h"
#include "audio.h"
#include "history.h"
#include "typography_presets.h"
#include "json.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;

// Provenance tag: typography-generated bricks carry this in source_id so a
// regenerate can find and replace exactly its own output.
constexpr const char* TYPO_FX_TAG = "__typo_fx__";

// App hook: surface the Typography tab after a rebuild. Registered by the
// desktop app; headless/iOS engine builds leave it null (no-op).
static void (*g_focus_typo_hook)() = nullptr;
void set_focus_typography_hook(void (*fn)()) { g_focus_typo_hook = fn; }
void app_focus_typography_panel() { if (g_focus_typo_hook) g_focus_typo_hook(); }

void apply_typo_style(Clip& c, const TypographyPreset& pr, const AppState& state) {
    // Each styled field takes the user's tweak when that field is active in the
    // hold store, otherwise the preset's value. Letter case: tweak wins; else the
    // preset's text_case (or, for older presets, derived from all_caps).
    const TypoTweaks& tw = state.typo;
    int   tcase = tw.on(TF_Case) ? tw.text_case
                : (pr.text_case >= 0 ? pr.text_case : (pr.all_caps ? 1 : 0));

    c.font_size         = tw.on(TF_FontSize) ? tw.font_size : pr.font_size;
    c.sub_pos           = tw.on(TF_PosV)     ? tw.pos_v     : pr.sub_pos;
    c.sub_pos_y         = tw.on(TF_PosY)     ? tw.pos_y     : pr.sub_pos_y;
    c.sub_pos_x         = tw.on(TF_PosX)     ? tw.pos_x     : pr.sub_pos_x;
    c.sub_anchor_h      = tw.on(TF_AnchorH)  ? tw.anchor_h  : pr.sub_anchor_h;
    c.sub_wrap_w        = tw.on(TF_Wrap)     ? tw.wrap_w    : pr.sub_wrap_w;
    c.sub_color_override = true;
    if (tw.on(TF_Color))
        memcpy(c.sub_color, tw.color, sizeof(c.sub_color));
    else
        memcpy(c.sub_color, pr.color, sizeof(c.sub_color));
    c.karaoke           = pr.karaoke;
    c.karaoke_mode      = pr.karaoke_mode;
    if (tw.on(TF_KaraokeHi))
        memcpy(c.karaoke_highlight_color, tw.karaoke_hi, sizeof(c.karaoke_highlight_color));
    else
        memcpy(c.karaoke_highlight_color, pr.karaoke_highlight_color, sizeof(c.karaoke_highlight_color));
    c.clip_style        = pr.style;
    c.sub_font          = pr.font ? pr.font : "";
    c.ease              = pr.ease;
    c.tracking          = tw.on(TF_Tracking) ? tw.tracking : pr.tracking;
    c.anim_unit         = pr.anim_unit;
    c.anim_stagger      = pr.anim_stagger > 0.f ? pr.anim_stagger : 0.06f;
    c.grad_mode         = pr.grad_mode;
    memcpy(c.grad_col2, pr.grad_col2, sizeof(c.grad_col2));
    // Letter case: lyrics regenerate from the transcript each time (and karaoke
    // word widths depend on the stored text), so they fold case in-place. A
    // one-off Text/Subtitle brick stores a render-time flag instead — that's
    // non-destructive, so switching case back to "as-typed" restores the typed
    // text.
    if (c.clip_type == ClipType::Lyrics) {
        c.text_case = 0;
        if      (tcase == 1) for (auto& ch : c.text) ch = (char)toupper((unsigned char)ch);
        else if (tcase == 2) for (auto& ch : c.text) ch = (char)tolower((unsigned char)ch);
    } else {
        c.text_case = tcase;
    }

    // Styling (shadow/stroke/glow/box) travels with the preset unless the user
    // pinned/overrode it in the tweak store.
    c.ts = tw.on(TF_TextStyle) ? tw.ts : pr.ts;

    // Fade isn't a preset property — only apply it when the user set it, so an
    // existing per-clip fade is left alone otherwise (and a tweak lands track-wide).
    if (tw.on(TF_FadeIn))  c.fade_in  = tw.fade_in;
    if (tw.on(TF_FadeOut)) c.fade_out = tw.fade_out;
}

void generate_typography(AppState& state) {
    // Need either cached words or JSON on disk
    bool has_cache    = !state.words_cache.empty();
    bool has_word_json = !state.words_json_path.empty() && fs::exists(state.words_json_path);
    bool has_seg_json  = !state.segments_json_path.empty() && fs::exists(state.segments_json_path);
    if (!has_cache && !has_word_json && !has_seg_json) return;

    const TypographyPreset* pr = typo_preset_by_id(state.typo_preset_id.c_str());
    if (!pr) pr = &g_typo_presets[0];

    // Grouping comes from the preset unless the user overrode it in the Typography
    // tab (TF_Grouping) — then it's state.subtitle_mode/_n. The preset still
    // supplies pause_gap/max_words as the tuning defaults.
    SubtitleMode grouping = state.typo.on(TF_Grouping) ? state.subtitle_mode : pr->grouping;
    int          group_n  = state.typo.on(TF_Grouping) ? state.subtitle_n   : pr->custom_n;

    const std::string src = state.audio_path;
    const std::string fx_tag = TYPO_FX_TAG + src;
    // Provenance keys on source_id==src; an empty src would match (and wipe) the
    // freestanding manual bricks below, so refuse to regenerate without a real source.
    if (src.empty()) return;

    // Find first analyzed audio/video clip for beat source
    int beat_ti = -1, beat_ci = -1;
    for (int ti = 0; ti < (int)state.tracks.size() && beat_ti < 0; ++ti)
        for (int ci = 0; ci < (int)state.tracks[ti].clips.size() && beat_ti < 0; ++ci) {
            auto& cl = state.tracks[ti].clips[ci];
            if (!cl.beats.empty()) { beat_ti = ti; beat_ci = ci; }
        }

    // Find the lyrics track BEFORE clearing (save index). Prefer one already holding
    // this source's clips; else fall back to the first lyrics track — kind is durable,
    // so an empty / manual / freshly-reloaded track is still found (managed isn't saved).
    int typo_ti = -1;
    for (int i = 0; i < (int)state.tracks.size() && typo_ti < 0; ++i)
        if (is_lyrics_track(state.tracks[i]))
            for (auto& c : state.tracks[i].clips)
                if (c.clip_type == ClipType::Lyrics && c.source_id == src) { typo_ti = i; break; }
    if (typo_ti < 0)
        for (int i = 0; i < (int)state.tracks.size(); ++i)
            if (is_lyrics_track(state.tracks[i])) { typo_ti = i; break; }

    // Clear previously generated typography clips and FX clips from all tracks.
    for (auto& t : state.tracks) {
        t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(),
            [&](const Clip& c) {
                return (c.clip_type == ClipType::Lyrics && c.source_id == src) ||
                       (c.clip_type == ClipType::Effect && c.source_id == fx_tag);
            }), t.clips.end());
    }

    // Create managed Lyrics track if none found.
    if (typo_ti < 0) {
        Track lt; lt.name = "Lyrics"; lt.managed = true; lt.kind = TrackKind::Lyrics;
        state.tracks.insert(state.tracks.begin(), std::move(lt));
        typo_ti = 0;
        if (beat_ti >= 0) beat_ti++;  // index shifted by insert
    }
    Track* typo_track = &state.tracks[typo_ti];

    // Build word clips
    auto stamp = [&](Clip& c) {
        c.clip_type = ClipType::Lyrics;
        c.source_id = src;
        apply_typo_style(c, *pr, state);
    };

    // Strobe: alternate color per word
    bool strobe = (strcmp(pr->id, "strobe") == 0);
    int  strobe_idx = 0;

    // Rave: random positions
    bool rave = (strcmp(pr->id, "rave") == 0);

    // Build raw word clips — prefer in-memory cache, fall back to JSON on disk.
    std::vector<Clip> raw;
    if (has_cache) {
        for (auto& we : state.words_cache) {
            Clip c; c.text = we.text; c.start = we.start; c.end = we.end;
            raw.push_back(c);
        }
    } else if (has_word_json) {
        std::ifstream f(state.words_json_path);
        if (f) {
            try {
                auto j = nlohmann::json::parse(f);
                for (auto& w : j) {
                    Clip c;
                    c.text  = w["word"].get<std::string>();
                    c.start = w["start"].get<float>();
                    c.end   = w["end"].get<float>();
                    raw.push_back(c);
                }
            } catch (...) {}
        }
    }

    // Whisper segments are the real line boundaries — Phrase/Line/Segment grouping
    // uses them instead of guessing lines from silence gaps. Read in source time;
    // offset to match `raw` below.
    std::vector<Clip> segs;
    const bool line_mode = (grouping == SubtitleMode::Phrase || grouping == SubtitleMode::Line ||
                            grouping == SubtitleMode::Segment || grouping == SubtitleMode::Karaoke);
    if (line_mode && has_seg_json) segs = read_segment_clips(state.segments_json_path);

    // No word list but we do have segments → the segments themselves are the lines.
    const bool seg_only = raw.empty() && !segs.empty();
    if (seg_only) { raw = std::move(segs); segs.clear(); }

    if (raw.empty()) return;

    // Apply the same timeline offset as apply_subtitle_mode:
    // tl_offset = clip.start - clip.in_point shifts source-relative whisper
    // timestamps to the clip's position on the timeline.  Without this,
    // lyrics are placed at their source positions regardless of where the
    // clip brick sits on the timeline.
    {
        float tl_offset = 0.f;
        for (auto& t : state.tracks) {
            bool found = false;
            for (auto& c : t.clips) {
                if ((c.clip_type == ClipType::Audio || c.clip_type == ClipType::Video) &&
                    c.source_id == src) {
                    tl_offset = c.start - c.in_point;
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
        if (tl_offset != 0.f) {
            for (auto& c : raw)  { c.start += tl_offset; c.end += tl_offset; }
            for (auto& c : segs) { c.start += tl_offset; c.end += tl_offset; }
        }
    }

    // Compensate for the playhead step-function lag: audio_position() is
    // advanced at the START of each audio callback, so the displayed playhead
    // always lags behind the actually-heard audio by up to one buffer period.
    // app.cpp subtracts audio_latency() from audio_position() to get the
    // playhead.  We subtract the same amount from clip timestamps at placement
    // time so the lyric clip becomes visible precisely when its word is heard.
    {
        float latency = audio_latency();
        if (latency > 0.f) {
            for (auto& c : raw)  { c.start -= latency; c.end -= latency; }
            for (auto& c : segs) { c.start -= latency; c.end -= latency; }
        }
    }

    // Drop clips that land entirely before the timeline start (words before
    // in_point shifted to negative time after applying tl_offset).
    raw.erase(std::remove_if(raw.begin(), raw.end(),
        [](const Clip& c){ return c.end < 0.f; }), raw.end());

    // Snap all raw word clips to frame boundaries now that offsets are final.
    for (auto& c : raw) {
        c.start = snap_to_frame(c.start, state.fps);
        c.end   = snap_end_to_frame(c.end, state.fps);
    }

    std::vector<Clip> grouped;
    if (seg_only)           grouped = raw;  // segments already are the lines
    else if (!segs.empty()) grouped = group_words_segmented(raw, segs, grouping, pr->pause_gap, pr->max_words);
    else                    grouped = group_words(raw, grouping, group_n, pr->pause_gap, pr->max_words);

    // Per-clip word data (always needed for edit replay; karaoke also uses it)
    std::vector<WordEntry> all_words;
    if (!seg_only) {
        for (auto& w : raw) {
            WordEntry we; we.text = w.text; we.start = w.start; we.end = w.end;
            all_words.push_back(we);
        }
    }

    // Do NOT clear the whole track — the scoped erase above already removed this
    // source's generated bricks. Freestanding manual bricks (empty source_id) must
    // survive a regen; the loop below appends the freshly-grouped clips around them.
    for (int i = 0; i < (int)grouped.size(); ++i) {
        Clip c = grouped[i];
        stamp(c);

        if (strobe && (strobe_idx++ % 2 == 1)) {
            // invert: black text, white would need bg — just tint yellow for now
            c.sub_color[0] = 0.f; c.sub_color[1] = 0.f; c.sub_color[2] = 0.f; c.sub_color[3] = 1.f;
        }
        if (rave) {
            // pseudo-random position per clip
            float hash = sinf((float)i * 127.1f + 311.7f) * 43758.5f;
            hash = hash - floorf(hash);
            float hash2 = sinf((float)i * 269.5f + 183.3f) * 43758.5f;
            hash2 = hash2 - floorf(hash2);
            c.sub_pos   = 3;
            c.sub_pos_y = 0.15f + hash  * 0.7f;
            c.sub_pos_x = 0.1f  + hash2 * 0.8f;
        }

        if (!all_words.empty()) {
            c.words.clear();
            for (auto& we : all_words)
                if (we.start >= c.start - 0.001f && we.end <= c.end + 0.001f)
                    c.words.push_back(we);
        }

        apply_lyrics_edits(state, c);

        typo_track->clips.push_back(c);
    }

    std::sort(typo_track->clips.begin(), typo_track->clips.end(),
              [](const Clip& a, const Clip& b){ return a.start < b.start; });

    // Auto-generate FX clips on a managed FX track directly above the lyrics track.
    if (typo_ti >= 0 && pr->n_fx > 0) {
        Track* fx_track = nullptr;
        if (typo_ti + 1 < (int)state.tracks.size() &&
            state.tracks[typo_ti + 1].kind == TrackKind::LyricsFX)
            fx_track = &state.tracks[typo_ti + 1];
        else {
            Track ft; ft.name = "Lyrics FX"; ft.managed = true; ft.kind = TrackKind::LyricsFX;
            state.tracks.insert(state.tracks.begin() + typo_ti + 1, std::move(ft));
            fx_track = &state.tracks[typo_ti + 1];
        }
        // Clear old generated FX
        fx_track->clips.erase(std::remove_if(fx_track->clips.begin(), fx_track->clips.end(),
            [&](const Clip& c){ return c.source_id == fx_tag; }), fx_track->clips.end());

        float dur = raw.empty() ? 0.f : raw.back().end;
        for (int fi = 0; fi < pr->n_fx; ++fi) {
            const TypoFXDesc& fd = pr->fx[fi];
            Clip fc;
            fc.clip_type    = ClipType::Effect;
            fc.fx_type      = fd.type;
            fc.source_id    = fx_tag;
            fc.start        = 0.f;
            fc.end          = fmaxf(dur, 10.f);
            fc.beat_src_track = beat_ti;
            fc.beat_src_clip  = beat_ci;
            // set default params + beat intensity on first beat-syncable param via hack:
            // store beat_intensity in beat_decay as a signal, generator will apply per-type
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
            fx_track->clips.push_back(fc);
        }
    }

    // Auto-select the first generated Lyrics clip so the Typography inspector opens immediately.
    int sel_ti = -1, sel_ci = -1;
    for (int ti2 = 0; ti2 < (int)state.tracks.size() && sel_ti < 0; ++ti2)
        for (int ci2 = 0; ci2 < (int)state.tracks[ti2].clips.size() && sel_ti < 0; ++ci2)
            if (state.tracks[ti2].clips[ci2].clip_type == ClipType::Lyrics
                && state.tracks[ti2].clips[ci2].source_id == src)
                { sel_ti = ti2; sel_ci = ci2; }
    state.selected_track = sel_ti;
    state.selected_clip  = sel_ci;
    // Desktop UX nicety (jump to the Typography tab) — app-provided; a
    // headless/iOS engine build supplies a no-op.
    app_focus_typography_panel();
    history_push(state, std::string("Generate typography — ") + pr->label);
}
