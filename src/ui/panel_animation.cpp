#include "studio_types.h"
#include "studio_shared.h"
#include "panel_animation.h"
#include "panel_clip.h"   // section_fade / section_text_style (shared style controls)
#include "text_styles.h"
#include "pipeline.h"
#include "app.h"
#include "audio.h"
#include "history.h"
#include "typography_presets.h"
#include "text_renderer.h"
#include "text_anim.h"
#include "theme.h"
#include <imgui.h>
#include <imgui_internal.h>
#include "json.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace fs = std::filesystem;
extern ImFont* g_font_bold;
extern ImFont* g_font_black;

// ── Typography generator + panel ──────────────────────────────────────────────

// Tag used on source_id of auto-generated FX clips so generate can find/clear them.
static constexpr const char* TYPO_FX_TAG = "__typo_fx__";

static void apply_typo_style(Clip& c, const TypographyPreset& pr, const AppState& state) {
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

    // Find first analyzed audio/video clip for beat source
    int beat_ti = -1, beat_ci = -1;
    for (int ti = 0; ti < (int)state.tracks.size() && beat_ti < 0; ++ti)
        for (int ci = 0; ci < (int)state.tracks[ti].clips.size() && beat_ti < 0; ++ci) {
            auto& cl = state.tracks[ti].clips[ci];
            if (!cl.beats.empty()) { beat_ti = ti; beat_ci = ci; }
        }

    // Find managed lyrics track for this source BEFORE clearing (save index).
    int typo_ti = -1;
    for (int i = 0; i < (int)state.tracks.size(); ++i) {
        if (!state.tracks[i].managed) continue;
        for (auto& c : state.tracks[i].clips)
            if (c.clip_type == ClipType::Lyrics && c.source_id == src) { typo_ti = i; break; }
        if (typo_ti >= 0) break;
    }

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
        Track lt; lt.name = "Lyrics"; lt.managed = true;
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

    typo_track->clips.clear();
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
            state.tracks[typo_ti + 1].managed &&
            state.tracks[typo_ti + 1].name == "Lyrics FX")
            fx_track = &state.tracks[typo_ti + 1];
        else {
            Track ft; ft.name = "Lyrics FX"; ft.managed = true;
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
    s_panel_view = PanelView::Typography;
    history_push(state, std::string("Generate typography — ") + pr->label);
}

// Live-update style on the typography target (no re-grouping). A selected
// standalone Text/Subtitle brick restyles in place; otherwise the whole managed
// Lyrics track for the source restyles together.
static bool typo_selected_is_standalone(const AppState& state) {
    if (state.selected_track < 0 || state.selected_track >= (int)state.tracks.size())
        return false;
    auto& clips = state.tracks[state.selected_track].clips;
    if (state.selected_clip < 0 || state.selected_clip >= (int)clips.size())
        return false;
    // Only a one-off Text brick styles in isolation. Lyrics AND Subtitle clips
    // are a managed group — they restyle together across the whole track.
    return clips[state.selected_clip].clip_type == ClipType::Text;
}

static void typo_restyle_live(AppState& state) {
    const TypographyPreset* pr = typo_preset_by_id(state.typo_preset_id.c_str());
    if (!pr) return;
    if (typo_selected_is_standalone(state)) {
        apply_typo_style(state.tracks[state.selected_track].clips[state.selected_clip],
                         *pr, state);
        return;
    }
    // Managed group: restyle every clip sharing the selected clip's type and
    // source (lyrics or subtitles), so a tweak lands on the whole track at once.
    // Fall back to the lyrics-for-the-current-audio set when there's no usable
    // selection (e.g. called right after generation).
    ClipType ct  = ClipType::Lyrics;
    std::string src = state.audio_path;
    if (state.selected_track >= 0 && state.selected_track < (int)state.tracks.size()) {
        auto& clips = state.tracks[state.selected_track].clips;
        if (state.selected_clip >= 0 && state.selected_clip < (int)clips.size()) {
            const Clip& sel = clips[state.selected_clip];
            if (sel.clip_type == ClipType::Lyrics || sel.clip_type == ClipType::Subtitle) {
                ct  = sel.clip_type;
                src = sel.source_id;
            }
        }
    }
    for (auto& t : state.tracks)
        for (auto& c : t.clips)
            if (c.clip_type == ct && c.source_id == src)
                apply_typo_style(c, *pr, state);
}

// Small "Hold" pin toggle drawn after a tweakable control's label. Filled when
// the field is pinned — pinned tweaks survive switching to another preset.
static void typo_hold_btn(AppState& state, TypoField f) {
    ImGui::SameLine(0.f, 6.f);
    bool held = state.typo.pinned(f);
    ImGui::PushID((int)f);
    if (ui_btn(held ? "Held" : "Hold", held, true))
        state.typo.pin(f, !held);
    ImGui::PopID();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(held ? "Pinned — kept when you switch presets (click to unpin)"
                               : "Pin so this setting survives switching presets");
}

// Category accent color
static ImU32 typo_category_dot(const char* cat) {
    if (strcmp(cat, "Hype")      == 0) return IM_COL32(255,  60,  80, 255);
    if (strcmp(cat, "Aesthetic") == 0) return IM_COL32(220, 130, 255, 255);
    if (strcmp(cat, "Editorial") == 0) return IM_COL32(255, 200,  50, 255);
    if (strcmp(cat, "Clean")     == 0) return IM_COL32( 80, 200, 255, 255);
    if (strcmp(cat, "Retro")     == 0) return IM_COL32(255, 140,  40, 255);
    return IM_COL32(180, 180, 180, 255);
}

void panel_typography(AppState& state, float w) {
    float full_w = w - 16.f;
    ImGui::Dummy({0.f, 8.f});

    // ── Resolve selected Lyrics/Text clip ─────────────────────────────────────
    bool valid_sel = state.selected_track >= 0
                     && state.selected_track < (int)state.tracks.size()
                     && state.selected_clip  >= 0
                     && state.selected_clip  < (int)state.tracks[state.selected_track].clips.size();
    const Clip* sel_clip = valid_sel ? &state.tracks[state.selected_track].clips[state.selected_clip] : nullptr;
    bool is_lyrics = sel_clip && (sel_clip->clip_type == ClipType::Lyrics
                                  || sel_clip->clip_type == ClipType::Text
                                  || sel_clip->clip_type == ClipType::Subtitle);

    if (!is_lyrics) {
        // ── Empty state ───────────────────────────────────────────────────────
        ImGui::Dummy({0.f, 40.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        const char* msg = "Select a text, subtitle, or lyrics clip to style it";
        float tw = ImGui::CalcTextSize(msg).x;
        ImGui::SetCursorPosX((w - tw) * 0.5f);
        ImGui::TextUnformatted(msg);
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 6.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
        ImGui::TextWrapped("Add a text brick from the Text library, or right-click an "
                           "audio/video clip and choose \"Make lyric video\".");
        ImGui::PopStyleColor();
        return;
    }


    // Which preset is currently active on this clip?
    // Try to find it by matching the clip's stored preset_id if we stamped it, else fall back to state.
    // We use state.typo_preset_id as the committed selection (updated on click).

    // ── Browse presets (collapsible) ──────────────────────────────────────────
    // The preset grid is a chooser, not the daily driver — collapse it so the
    // tune controls below are the first thing you see. The active preset's name
    // rides in the header so it's clear what's applied while collapsed.
    const TypographyPreset* apr = typo_preset_by_id(state.typo_preset_id.c_str());
    char blbl[96];
    snprintf(blbl, sizeof(blbl), "Browse presets  ·  %s###typo_browse",
             apr ? apr->label : "none");
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    bool browse_open = ImGui::TreeNodeEx(blbl,
        ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding);
    ImGui::PopStyleColor();
    if (browse_open) {
    ImGui::Dummy({0.f, 6.f});

    // Category filter pills (in place) — pick a category instead of scrolling
    // the whole catalogue. Shared with the FX / background libraries.
    static std::string s_typo_cat;   // empty = All
    {
        std::vector<const char*> cats;
        for (int i = 0; i < g_n_typo_presets; ++i) {
            const char* c = g_typo_presets[i].category;
            bool seen = false;
            for (auto* x : cats) if (strcmp(x, c) == 0) { seen = true; break; }
            if (!seen) cats.push_back(c);
        }
        category_pills("typocat", cats, s_typo_cat);
        ImGui::Dummy({0.f, 8.f});
    }

    const float gap    = 4.f;
    const float cell_w = (full_w - gap) * 0.5f;
    const float cell_h = 112.f;   // taller so the wrapped tagline + preview fit

    const char* cur_cat = nullptr;
    int col_idx = 0;

    for (int i = 0; i < g_n_typo_presets; ++i) {
        const TypographyPreset& pr = g_typo_presets[i];
        if (!s_typo_cat.empty() && s_typo_cat != pr.category) continue;
        bool selected = (state.typo_preset_id == pr.id);

        // Category label — full width, resets column. Only in "All" mode; when a
        // pill is active the pill already names the category, so it's dropped.
        if (!cur_cat || strcmp(cur_cat, pr.category) != 0) {
            if (col_idx == 1) { ImGui::NewLine(); col_idx = 0; }
            if (s_typo_cat.empty()) {
                if (cur_cat) ImGui::Dummy({0.f, 4.f});
                ImU32 dot_col = typo_category_dot(pr.category);
                ImDrawList* dl_cat = ImGui::GetWindowDrawList();
                ImVec2 lp = ImGui::GetCursorScreenPos();
                dl_cat->AddCircleFilled({lp.x + 4.f, lp.y + 7.f}, 4.f, dot_col);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.f);
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                ImGui::TextUnformatted(pr.category);
                ImGui::PopStyleColor();
                ImGui::Dummy({0.f, 2.f});
            }
            cur_cat = pr.category;
            col_idx = 0;
        }

        if (col_idx == 1) ImGui::SameLine(0.f, gap);

        ImVec2 cp = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        bool hov = ImGui::IsMouseHoveringRect(cp, {cp.x + cell_w, cp.y + cell_h});

        ImU32 bg_col  = selected ? IM_COL32(55, 48, 88, 255) : IM_COL32(26, 24, 36, 255);
        ImU32 brd_col = selected ? IM_COL32(140, 100, 255, 255) : IM_COL32(50, 47, 65, 200);
        float brd_w   = selected ? 2.f : 1.f;
        if (hov && !selected) { bg_col = IM_COL32(38, 34, 54, 255); brd_col = IM_COL32(100, 85, 150, 255); }

        dl->AddRectFilled(cp, {cp.x + cell_w, cp.y + cell_h}, bg_col, 6.f);

        // Category accent bar left edge
        dl->AddRectFilled({cp.x, cp.y + 10.f}, {cp.x + 3.f, cp.y + cell_h - 10.f},
            typo_category_dot(pr.category), 2.f);

        dl->AddRect(cp, {cp.x + cell_w, cp.y + cell_h}, brd_col, 6.f, 0, brd_w);

        // Color chip top-right
        ImU32 chip = IM_COL32((int)(pr.color[0]*220), (int)(pr.color[1]*220), (int)(pr.color[2]*220), 220);
        dl->AddRectFilled({cp.x + cell_w - 16.f, cp.y + 6.f},
                          {cp.x + cell_w - 6.f,  cp.y + 16.f}, chip, 3.f);

        float tx = cp.x + 10.f;
        ImGui::PushFont(g_font_bold);
        dl->AddText(ImGui::GetFont(), 12.f, {tx, cp.y + 10.f},
            selected ? IM_COL32(255,255,255,255) : IM_COL32(210,205,230,240), pr.label);
        ImGui::PopFont();

        // Tagline — wraps within the card (up to 2 lines) instead of being cut
        // off at the card edge. The middle-dot-separated full hint is shown.
        {
            float tag_w = cell_w - 16.f;
            ImVec4 tag_clip = {tx, cp.y + 24.f, cp.x + cell_w - 6.f, cp.y + 52.f};
            dl->AddText(ImGui::GetFont(), 11.f, {tx, cp.y + 25.f},
                        IM_COL32(120, 115, 145, 200), pr.tagline, nullptr,
                        tag_w, &tag_clip);
        }

        // ── Inline text preview ───────────────────────────────────────────────
        {
            float px0 = cp.x + 6.f, px1 = cp.x + cell_w - 6.f;
            float py0 = cp.y + 56.f, py1 = cp.y + cell_h - 6.f;
            float pw  = px1 - px0, ph = py1 - py0;

            dl->AddRectFilled({px0, py0}, {px1, py1}, IM_COL32(10, 8, 18, 220), 3.f);
            dl->AddRect({px0, py0}, {px1, py1}, IM_COL32(40, 36, 58, 180), 3.f);
            dl->PushClipRect({px0, py0}, {px1, py1}, true);

            // Sample string in the preset's own letter case. Presets that group
            // by phrase/line/segment shine on a full sentence, so preview them
            // with one; word-grouped presets get a single word. Rendered in the
            // preset's actual face so the card shows the real typeface.
            int pc = (pr.text_case >= 0) ? pr.text_case : (pr.all_caps ? 1 : 0);
            bool sentence = (pr.grouping != SubtitleMode::Word);
            std::string sample = sentence ? "I want to see you every night." : "Stay";
            if      (pc == 1) for (auto& ch : sample) ch = (char)toupper((unsigned char)ch);
            else if (pc == 2) for (auto& ch : sample) ch = (char)tolower((unsigned char)ch);

            ImFont* pfont = typo_font_get(pr.font);
            float pfsz = fmaxf(8.f, fminf(20.f, pr.font_size * ph * 4.5f));
            ImVec2 tsz2 = pfont->CalcTextSizeA(pfsz, FLT_MAX, -1.f, sample.c_str());
            // Shrink to fit the preview width — sentences would overflow otherwise.
            if (tsz2.x > pw - 4.f && tsz2.x > 0.f) {
                pfsz = fmaxf(7.f, pfsz * (pw - 4.f) / tsz2.x);
                tsz2 = pfont->CalcTextSizeA(pfsz, FLT_MAX, -1.f, sample.c_str());
            }

            // Animated preview: drive the REAL text renderer on a looping clock
            // so the card shows the preset's actual motion (per-element cascade,
            // typewriter, wave, gradient, glow, the real font) scaled to the box.
            // Only the on-screen cards animate (cheap offscreen).
            if (ImGui::IsRectVisible({px0, py0}, {px1, py1})) {
                Clip pc;
                pc.clip_type    = ClipType::Text;
                pc.text         = sample;
                pc.clip_style   = pr.style;
                pc.sub_font     = pr.font ? pr.font : "";
                pc.anim_unit    = pr.anim_unit;
                pc.anim_stagger = pr.anim_stagger > 0.f ? pr.anim_stagger : 0.06f;
                pc.ease         = pr.ease;
                pc.tracking     = pr.tracking;
                pc.karaoke      = false;       // no word timings to drive it in a card
                pc.grad_mode    = pr.grad_mode;
                memcpy(pc.grad_col2, pr.grad_col2, sizeof(pc.grad_col2));
                memcpy(pc.sub_color, pr.color, sizeof(pc.sub_color));
                pc.sub_color_override = true;
                pc.ts           = pr.ts;
                pc.sub_anchor_h = 1;

                // Loop: play the intro, hold, restart. Per-card phase offset so
                // the grid doesn't pulse in unison.
                const float loop_dur = 2.6f;
                float lt = fmodf((float)ImGui::GetTime() + (float)i * 0.18f, loop_dur);
                pc.start = 0.f; pc.end = loop_dur;

                float fade_in  = fminf(0.25f, loop_dur * 0.3f);
                float fade_out = fminf(0.25f, loop_dur * 0.2f);
                float a_dx = 0.f, a_dy = 0.f, a_alpha = 1.f, a_scale = 1.f;
                if (pc.anim_unit == 0 && pr.style != AnimStyle::None) {
                    BlockAnim ba = compute_block_anim(pr.style, lt, loop_dur,
                                                      fade_in, fade_out, pw, pc.ease);
                    a_dx = ba.dx; a_dy = ba.dy; a_alpha = ba.alpha; a_scale = ba.scale;
                }
                float dfsz    = pfsz * a_scale;
                float dline_h = dfsz * 1.25f;

                float bty;
                if (pr.sub_pos == 2)      bty = py0 + 2.f;
                else if (pr.sub_pos == 0) bty = py1 - dline_h - 2.f;
                else                       bty = py0 + ph * 0.5f - dline_h * 0.5f;

                TextRenderCtx trc{};
                trc.dl = dl; trc.font = pfont; trc.fsz = dfsz;
                trc.anim_alpha = a_alpha; trc.anim_dx = a_dx; trc.anim_dy = 0.f;
                trc.clip = &pc; trc.eff_style = pr.style; trc.anchor_h = 1;
                trc.block_cx = px0 + pw * 0.5f; trc.ty = bty + a_dy;
                trc.line_h = dline_h; trc.t = lt; trc.rotation = 0.f;
                trc.canvas_w = pw; trc.canvas_x0 = px0;
                trc.canvas_h = ph; trc.canvas_y0 = py0; trc.clip_words = nullptr;
                std::vector<std::string> plines{ sample };
                render_text_block(trc, plines);
            }

            dl->PopClipRect();
        }

        ImGui::SetCursorScreenPos(cp);
        char btn_id[64]; snprintf(btn_id, sizeof(btn_id), "##tycard_%s", pr.id);
        ImGui::InvisibleButton(btn_id, {cell_w, cell_h});
        if (ImGui::IsItemClicked()) {
            state.typo_preset_id = pr.id;
            // Keep only the pinned (held) tweaks; everything else reverts to the
            // new preset's own look — that's the sticky/hold UX.
            state.typo.keep_held();
            // A standalone Text/Subtitle brick just takes the preset's look
            // (font, colour, position, animation) on that one clip. Lyrics
            // regenerate the managed transcript track (regroup + lyrics FX).
            if (typo_selected_is_standalone(state)) {
                apply_typo_style(state.tracks[state.selected_track]
                                     .clips[state.selected_clip], pr, state);
                history_push(state, std::string("Typography — ") + pr.label);
            } else {
                generate_typography(state);
            }
        }

        col_idx++;
        if (col_idx >= 2) { col_idx = 0; ImGui::Dummy({0.f, gap}); }
    }
    if (col_idx == 1) ImGui::NewLine();
    ImGui::TreePop();
    }   // browse_open

    ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

    // ── Tune ──────────────────────────────────────────────────────────────────
    // Every control below tweaks the active preset. Its "Hold" pin keeps that
    // tweak when you switch to another preset (keep_held on the card click).
    const TypographyPreset* pr = typo_preset_by_id(state.typo_preset_id.c_str());
    auto& tw = state.typo;

    // Grouping — overrides the preset's word grouping for the managed lyrics track
    // (value lives in state.subtitle_mode/_n). Hidden for a standalone text brick,
    // where grouping doesn't apply. Changing it re-lays the transcript track.
    if (!typo_selected_is_standalone(state)) {
        ui_label("Grouping"); typo_hold_btn(state, TF_Grouping);
        SubtitleMode gm = tw.on(TF_Grouping) ? state.subtitle_mode
                                             : (pr ? pr->grouping : SubtitleMode::Phrase);
        struct GBtn { SubtitleMode m; const char* label; const char* tip; };
        static const GBtn gbtns[] = {
            {SubtitleMode::Word,    "Word",     "One word per brick"},
            {SubtitleMode::Phrase,  "Phrase",   "Sub-line phrases — a line split at its pauses"},
            {SubtitleMode::Segment, "Sentence", "One brick per transcript line"},
            {SubtitleMode::CustomN, "Custom",   "N words per brick"},
        };
        ImGui::PushID("ty_group");
        for (auto& gb : gbtns) {
            if (ui_btn(gb.label, gm == gb.m, true)) {
                state.subtitle_mode = gb.m; tw.tweak(TF_Grouping);
                generate_typography(state);
            }
            if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::TextUnformatted(gb.tip); ImGui::EndTooltip(); }
            ImGui::SameLine(0.f, 4.f);
        }
        ImGui::PopID();
        ImGui::NewLine();
        if (gm == SubtitleMode::CustomN) {
            ImGui::SetNextItemWidth(80.f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
            int n = state.subtitle_n;
            if (ImGui::InputInt("words/brick##tycn", &n)) {
                state.subtitle_n = (n < 1) ? 1 : (n > 20) ? 20 : n;
                tw.tweak(TF_Grouping); generate_typography(state);
            }
            ImGui::PopStyleColor();
        }
        ImGui::Dummy({0.f, 8.f});
    }

    ui_label("Font Size"); typo_hold_btn(state, TF_FontSize);
    float fs = tw.on(TF_FontSize) ? tw.font_size : (pr ? pr->font_size : 0.09f);
    ImGui::SetNextItemWidth(full_w);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
    if (ImGui::SliderFloat("##tyfo", &fs, 0.03f, 0.30f, "%.2f")) {
        tw.font_size = fs; tw.tweak(TF_FontSize);
        typo_restyle_live(state);
    }
    ImGui::PopStyleColor();

    ImGui::Dummy({0.f, 8.f});

    ui_label("Color"); typo_hold_btn(state, TF_Color);
    float col_buf[4];
    const float* src_col = tw.on(TF_Color) ? tw.color : (pr ? pr->color : tw.color);
    memcpy(col_buf, src_col, sizeof(col_buf));
    ImGui::SetNextItemWidth(full_w);
    if (ImGui::ColorEdit4("##tycol", col_buf,
            ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar)) {
        memcpy(tw.color, col_buf, sizeof(tw.color)); tw.tweak(TF_Color);
        typo_restyle_live(state);
    }
    palette_widget("##pal_typo", col_buf);
    // Only a palette-swatch click (which mutates col_buf in place) is a real
    // edit. Compare against what we loaded (src_col) so just opening the tab
    // doesn't fire a spurious restyle.
    if (memcmp(col_buf, src_col, sizeof(col_buf)) != 0) {
        memcpy(tw.color, col_buf, sizeof(tw.color)); tw.tweak(TF_Color);
        typo_restyle_live(state);
    }

    ImGui::Dummy({0.f, 8.f});

    // Karaoke highlight — only meaningful when the active preset does per-word
    // karaoke. The Color above is the base (unsung) word; this is the sung word.
    if ((pr && pr->karaoke) || tw.on(TF_KaraokeHi)) {
        ui_label("Karaoke Highlight"); typo_hold_btn(state, TF_KaraokeHi);
        float kh[4];
        const float* src_kh = tw.on(TF_KaraokeHi) ? tw.karaoke_hi
                            : (pr ? pr->karaoke_highlight_color : tw.karaoke_hi);
        memcpy(kh, src_kh, sizeof(kh));
        ImGui::SetNextItemWidth(full_w);
        if (ImGui::ColorEdit4("##tykhi", kh,
                ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar)) {
            memcpy(tw.karaoke_hi, kh, sizeof(tw.karaoke_hi)); tw.tweak(TF_KaraokeHi);
            typo_restyle_live(state);
        }
        ImGui::Dummy({0.f, 8.f});
    }

    // Horizontal alignment — left / center / right (writes sub_anchor_h, which
    // the renderer already honors; previously only presets could set it).
    ui_label("Alignment"); typo_hold_btn(state, TF_AnchorH);
    int cur_align = tw.on(TF_AnchorH) ? tw.anchor_h : (pr ? pr->sub_anchor_h : 1);
    struct AlignBtn { int v; const char* label; };
    AlignBtn abtns[] = {{0,"Left"},{1,"Center"},{2,"Right"}};
    ImGui::PushID("ty_align");   // scope: "Center" also exists in Vertical below
    for (auto& ab : abtns) {
        if (ui_btn(ab.label, cur_align == ab.v, true)) {
            tw.anchor_h = ab.v; tw.tweak(TF_AnchorH);
            typo_restyle_live(state);
        }
        ImGui::SameLine(0.f, 4.f);
    }
    ImGui::PopID();
    ImGui::NewLine();

    ImGui::Dummy({0.f, 8.f});

    // Vertical placement — bottom / center / top (sub_pos). Pairs with alignment.
    ui_label("Vertical"); typo_hold_btn(state, TF_PosV);
    int cur_pos = tw.on(TF_PosV) ? tw.pos_v : (pr ? pr->sub_pos : 1);
    struct VBtn { int v; const char* label; };
    VBtn vbtns[] = {{0,"Bottom"},{1,"Center"},{2,"Top"}};
    ImGui::PushID("ty_vert");
    for (auto& vb : vbtns) {
        if (ui_btn(vb.label, cur_pos == vb.v, true)) {
            tw.pos_v = vb.v; tw.tweak(TF_PosV);
            typo_restyle_live(state);
        }
        ImGui::SameLine(0.f, 4.f);
    }
    ImGui::PopID();
    ImGui::NewLine();

    ImGui::Dummy({0.f, 10.f});

    // ── Advanced ──────────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    bool adv_open = ImGui::TreeNodeEx("Advanced##typo_adv",
        ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding);
    ImGui::PopStyleColor();
    if (adv_open) {
        ImGui::Dummy({0.f, 6.f});

        // Letter case — 3-way (As typed / UPPER / lower).
        ui_label("Letter case"); typo_hold_btn(state, TF_Case);
        int cur_case = tw.on(TF_Case) ? tw.text_case
            : (pr ? (pr->text_case >= 0 ? pr->text_case : (pr->all_caps ? 1 : 0)) : 0);
        struct CaseBtn { int v; const char* label; };
        CaseBtn cbtns[] = {{0,"As typed"},{1,"AA"},{2,"aa"}};
        for (auto& cb : cbtns) {
            if (ui_btn(cb.label, cur_case == cb.v, true)) {
                tw.text_case = cb.v; tw.tweak(TF_Case);
                typo_restyle_live(state);
            }
            ImGui::SameLine(0.f, 4.f);
        }
        ImGui::NewLine();

        ImGui::Dummy({0.f, 8.f});
        ui_label("Letter spacing"); typo_hold_btn(state, TF_Tracking);
        float trk = tw.on(TF_Tracking) ? tw.tracking : (pr ? pr->tracking : 0.f);
        ImGui::SetNextItemWidth(full_w);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
        if (ImGui::SliderFloat("##tytrk", &trk, -0.1f, 0.5f, "%.2f")) {
            tw.tracking = trk; tw.tweak(TF_Tracking);
            typo_restyle_live(state);
        }
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 8.f});
        ui_label("Wrap width"); typo_hold_btn(state, TF_Wrap);
        float wrp = tw.on(TF_Wrap) ? tw.wrap_w : (pr ? pr->sub_wrap_w : 0.85f);
        ImGui::SetNextItemWidth(full_w);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
        if (ImGui::SliderFloat("##tywrap", &wrp, 0.2f, 1.0f, "%.2f")) {
            tw.wrap_w = wrp; tw.tweak(TF_Wrap);
            typo_restyle_live(state);
        }
        ImGui::PopStyleColor();

        // Fine horizontal / vertical offset.
        ImGui::Dummy({0.f, 8.f});
        ui_label("X offset"); typo_hold_btn(state, TF_PosX);
        float px = tw.on(TF_PosX) ? tw.pos_x : (pr ? pr->sub_pos_x : 0.5f);
        ImGui::SetNextItemWidth(full_w);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
        if (ImGui::SliderFloat("##typx", &px, 0.f, 1.f, "%.2f")) {
            tw.pos_x = px; tw.tweak(TF_PosX);
            typo_restyle_live(state);
        }
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 8.f});
        ui_label("Y offset"); typo_hold_btn(state, TF_PosY);
        float py = tw.on(TF_PosY) ? tw.pos_y : (pr ? pr->sub_pos_y : 0.85f);
        ImGui::SetNextItemWidth(full_w);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
        if (ImGui::SliderFloat("##typy", &py, 0.f, 1.f, "%.2f")) {
            // A manual Y offset means a custom vertical position (sub_pos=3).
            tw.pos_y = py; tw.tweak(TF_PosY);
            tw.pos_v = 3; tw.tweak(TF_PosV);
            typo_restyle_live(state);
        }
        ImGui::PopStyleColor();

        // Fade in/out — track-wide via the shared section. apply_typo_style only
        // applies these when the field is active, so existing fades aren't wiped.
        ImGui::Dummy({0.f, 10.f});
        ui_label("Fade"); typo_hold_btn(state, TF_FadeIn);
        if (!tw.on(TF_FadeIn)) tw.fade_in = 0.f;     // show 0 until the user sets it
        if (!tw.on(TF_FadeOut)) tw.fade_out = 0.f;
        if (section_fade(state, tw.fade_in, tw.fade_out, full_w)) {
            tw.tweak(TF_FadeIn); tw.tweak(TF_FadeOut);
            typo_restyle_live(state);
        }

        // Text style (shadow/stroke/glow/box) — shared with the Clip tab.
        ImGui::Dummy({0.f, 10.f});
        ui_label("Text style"); typo_hold_btn(state, TF_TextStyle);
        if (!tw.on(TF_TextStyle) && pr) tw.ts = pr->ts;   // seed from preset until tweaked
        if (section_text_style(state, tw.ts, full_w)) {
            tw.tweak(TF_TextStyle);
            typo_restyle_live(state);
        }

        ImGui::Dummy({0.f, 8.f});
        if (ui_btn("Reset all to preset", false, true)) {
            tw.active = 0; tw.held = 0;   // drop every tweak and pin
            typo_restyle_live(state);
        }

        ImGui::TreePop();
    }
}

// ── Text brick library ────────────────────────────────────────────────────────
// Human entry point: add a PLAIN text brick, then style + animate it in the
// Typography tab (the single styling surface for all text-like clips). The
// per-animation picker that used to live here was folded into Typography —
// AnimStyle now comes from a typography preset, not a pre-add choice.

// Animation display names — kept for the timeline drop-history label and any
// other AnimStyle → text lookups; this is no longer a visible card list.
static const TextStyleCard TEXT_STYLES[] = {
    {AnimStyle::None,       "Plain",      "Static — no animation", "plain"},
    {AnimStyle::Fade,       "Fade",       "Opacity in/out — clean and invisible", "soft"},
    {AnimStyle::Glitch,     "Glitch",     "Digital artefact noise — corrupt feel", "glitch"},
    {AnimStyle::Typewriter, "Typewriter", "Character-by-character reveal", "retro"},
    {AnimStyle::Bounce,     "Bounce",     "Drops in with spring overshoot", "lively"},
    {AnimStyle::Scale,      "Scale",      "Punches in from small — zoom", "punchy"},
    {AnimStyle::Slide,      "Slide",      "Enters from the left, holds, exits right", "motion"},
    {AnimStyle::Stack,      "Stack",      "Lines pile — previous dims on entry", "dense"},
    {AnimStyle::Block,      "Block",      "White background fill — high contrast", "sharp"},
};

const char* text_style_name(AnimStyle st) {
    for (auto& sc : TEXT_STYLES)
        if (sc.style == st) return sc.name;
    return "Text";
}

// Build the clip a card creates/drops. Centered on the canvas so it's never
// off-screen (the Clip defaults put text at the bottom subtitle slot).
// Also used by the timeline's TEXT_STYLE drop handlers.
Clip make_text_brick(AnimStyle style, float start) {
    Clip c;
    c.clip_type  = ClipType::Text;
    c.text       = "Your text";
    c.clip_style = style;
    c.sub_pos    = 1;            // canvas center
    c.start      = start;
    c.end        = start + 4.f;
    return c;
}

// Drop a plain text brick onto a track / at the playhead, then select it.
static void add_text_brick_here(AppState& state) {
    Clip c = make_text_brick(AnimStyle::None, state.playhead);
    int target = find_empty_track(state);
    if (target < 0) {
        Track t; t.name = "Text";
        state.tracks.insert(state.tracks.begin(), std::move(t));
        target = 0;
    }
    state.tracks[target].clips.push_back(std::move(c));
    state.selected_track = target;
    state.selected_clip  = (int)state.tracks[target].clips.size() - 1;
    clip_flash(state, target, state.selected_clip, /*reveal=*/true);
    s_panel_view = PanelView::Typography;   // jump straight to styling
    history_push(state, "Add text brick");
}

void panel_text_library(AppState& state, float w) {
    ImGui::Dummy({0.f, 6.f});
    ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted("Text");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, to_u32(Col::muted));
    ImGui::TextWrapped("Add a text brick, then style and animate it in the "
                       "Typography tab. Click to add at the playhead, or drag "
                       "onto the timeline.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 8.f});

    float card_w = w - 8.f, card_h = 64.f;
    ImVec2 cp = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool hov = ImGui::IsMouseHoveringRect(cp, {cp.x + card_w, cp.y + card_h});

    dl->AddRectFilled(cp, {cp.x + card_w, cp.y + card_h},
                      hov ? IM_COL32(34, 40, 58, 255) : IM_COL32(22, 22, 28, 255), 6.f);
    dl->AddRect(cp, {cp.x + card_w, cp.y + card_h},
                hov ? IM_COL32(80, 140, 220, 220) : IM_COL32(50, 50, 62, 200), 6.f, 0, 1.2f);
    ImGui::PushFont(g_font_bold);
    dl->AddText(ImGui::GetFont(), 16.f, {cp.x + 14.f, cp.y + 13.f}, to_u32(Col::fg), "+ Add Text");
    ImGui::PopFont();
    dl->AddText({cp.x + 14.f, cp.y + 37.f}, IM_COL32(140, 140, 160, 220),
                "Plain brick \xe2\x80\x94 style it in Typography");

    ImGui::InvisibleButton("##add_text_brick", {card_w, card_h});
    if (ImGui::IsItemClicked()) add_text_brick_here(state);
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        int style_int = (int)AnimStyle::None;   // plain brick; styled in Typography
        ImGui::SetDragDropPayload("TEXT_STYLE", &style_int, sizeof(int));
        ImDrawList* gdl = ImGui::GetForegroundDrawList();
        ImVec2 gp = ImGui::GetMousePos();
        gdl->AddRectFilled({gp.x + 8.f, gp.y + 8.f}, {gp.x + 148.f, gp.y + 44.f},
                           IM_COL32(20, 40, 80, 230), 6.f);
        gdl->AddRect({gp.x + 8.f, gp.y + 8.f}, {gp.x + 148.f, gp.y + 44.f},
                     IM_COL32(80, 140, 220, 200), 6.f, 0, 1.2f);
        gdl->AddText({gp.x + 20.f, gp.y + 20.f}, IM_COL32(255, 255, 255, 240), "Text brick");
        ImGui::EndDragDropSource();
    }
    if (hov) ImGui::SetTooltip("Add a plain text brick (style in the Typography tab)");

    ImGui::Dummy({0.f, 12.f});
}
