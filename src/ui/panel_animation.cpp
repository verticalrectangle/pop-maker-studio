#include "studio_types.h"
#include "studio_shared.h"
#include "panel_animation.h"
#include "pipeline.h"
#include "app.h"
#include "audio.h"
#include "history.h"
#include "typography_presets.h"
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
    float fs   = (state.typo_font_size  > 0.001f) ? state.typo_font_size  : pr.font_size;
    bool  caps = state.typo_all_caps_override ? state.typo_all_caps : pr.all_caps;
    bool  has_color_override = (state.typo_color[3] > 0.001f);

    c.font_size         = fs;
    c.sub_pos           = pr.sub_pos;
    c.sub_pos_y         = pr.sub_pos_y;
    c.sub_pos_x         = pr.sub_pos_x;
    c.sub_anchor_h      = pr.sub_anchor_h;
    c.sub_wrap_w        = pr.sub_wrap_w;
    c.sub_color_override = true;
    if (has_color_override)
        memcpy(c.sub_color, state.typo_color, sizeof(c.sub_color));
    else
        memcpy(c.sub_color, pr.color, sizeof(c.sub_color));
    c.karaoke           = pr.karaoke;
    c.clip_style        = pr.style;
    if (caps) {
        for (auto& ch : c.text) ch = (char)toupper((unsigned char)ch);
    }

    c.ts = TextStyle{};
    if (strcmp(pr.id, "neon") == 0) {
        c.ts.glow_enabled = true; c.ts.glow_r = 10.f;
        c.ts.glow_col[0] = 1.f; c.ts.glow_col[1] = 0.2f; c.ts.glow_col[2] = 0.8f; c.ts.glow_col[3] = 0.7f;
    } else if (strcmp(pr.id, "cyberpunk") == 0) {
        c.ts.stroke_enabled = true; c.ts.stroke_w = 1.5f;
        c.ts.stroke_col[0] = 0.f; c.ts.stroke_col[1] = 1.f; c.ts.stroke_col[2] = 1.f; c.ts.stroke_col[3] = 0.8f;
    }
}

void generate_typography(AppState& state) {
    // Need either cached words or JSON on disk
    bool has_cache    = !state.words_cache.empty();
    bool has_word_json = !state.words_json_path.empty() && fs::exists(state.words_json_path);
    bool has_seg_json  = !state.segments_json_path.empty() && fs::exists(state.segments_json_path);
    if (!has_cache && !has_word_json && !has_seg_json) return;

    const TypographyPreset* pr = typo_preset_by_id(state.typo_preset_id.c_str());
    if (!pr) pr = &g_typo_presets[0];

    // Grouping and word count come entirely from the preset — no user override.
    SubtitleMode grouping = pr->grouping;

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
    bool from_segments = false;

    if (grouping == SubtitleMode::Segment && has_seg_json) {
        std::ifstream f(state.segments_json_path);
        if (f) {
            try {
                auto j = nlohmann::json::parse(f);
                for (auto& seg : j) {
                    Clip c;
                    c.text  = seg["text"].get<std::string>();
                    c.start = seg["start"].get<float>();
                    c.end   = seg["end"].get<float>();
                    raw.push_back(c);
                }
                from_segments = true;
            } catch (...) {}
        }
    }

    if (!from_segments) {
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
    }

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
            for (auto& c : raw) {
                c.start += tl_offset;
                c.end   += tl_offset;
            }
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
            for (auto& c : raw) {
                c.start -= latency;
                c.end   -= latency;
            }
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

    auto grouped = from_segments ? raw
                                 : group_words(raw, grouping, pr->custom_n, pr->pause_gap, pr->max_words);

    // Per-clip word data (always needed for edit replay; karaoke also uses it)
    std::vector<WordEntry> all_words;
    if (!from_segments) {
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

// Live-update style on all existing generated typography clips (no re-grouping).
static void typo_restyle_live(AppState& state) {
    const TypographyPreset* pr = typo_preset_by_id(state.typo_preset_id.c_str());
    if (!pr) return;
    const std::string src = state.audio_path;
    for (auto& t : state.tracks)
        for (auto& c : t.clips)
            if (c.clip_type == ClipType::Lyrics && c.source_id == src)
                apply_typo_style(c, *pr, state);
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
        float tw = ImGui::CalcTextSize("Select a lyrics clip to style it").x;
        ImGui::SetCursorPosX((w - tw) * 0.5f);
        ImGui::TextUnformatted("Select a lyrics clip to style it");
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 6.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
        ImGui::TextWrapped("Right-click an audio or video clip and choose \"Make lyric video\" to get started.");
        ImGui::PopStyleColor();
        return;
    }


    // Which preset is currently active on this clip?
    // Try to find it by matching the clip's stored preset_id if we stamped it, else fall back to state.
    // We use state.typo_preset_id as the committed selection (updated on click).

    // ── Preset grid (2 columns) ───────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextUnformatted("STYLE");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 4.f});

    const float gap    = 4.f;
    const float cell_w = (full_w - gap) * 0.5f;
    const float cell_h = 92.f;

    const char* cur_cat = nullptr;
    int col_idx = 0;

    for (int i = 0; i < g_n_typo_presets; ++i) {
        const TypographyPreset& pr = g_typo_presets[i];
        bool selected = (state.typo_preset_id == pr.id);

        // Category label — full width, resets column
        if (!cur_cat || strcmp(cur_cat, pr.category) != 0) {
            if (col_idx == 1) { ImGui::NewLine(); col_idx = 0; }
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

        // Tagline — clip at first middle-dot
        char tagbuf[48]; snprintf(tagbuf, sizeof(tagbuf), "%s", pr.tagline);
        for (int k = 0; tagbuf[k]; ++k)
            if ((unsigned char)tagbuf[k] == 0xc2 && (unsigned char)tagbuf[k+1] == 0xb7)
                { tagbuf[k > 0 ? k-1 : 0] = '\0'; break; }
        dl->AddText({tx, cp.y + 27.f}, IM_COL32(120, 115, 145, 200), tagbuf);

        // ── Inline text preview ───────────────────────────────────────────────
        {
            float px0 = cp.x + 6.f, px1 = cp.x + cell_w - 6.f;
            float py0 = cp.y + 42.f, py1 = cp.y + cell_h - 6.f;
            float pw  = px1 - px0, ph = py1 - py0;

            dl->AddRectFilled({px0, py0}, {px1, py1}, IM_COL32(10, 8, 18, 220), 3.f);
            dl->AddRect({px0, py0}, {px1, py1}, IM_COL32(40, 36, 58, 180), 3.f);
            dl->PushClipRect({px0, py0}, {px1, py1}, true);

            // Sample string with preset's caps setting
            char sample[8] = "stay";
            if (pr.all_caps) { sample[0]='S'; sample[1]='T'; sample[2]='A'; sample[3]='Y'; }

            ImFont* pfont = g_font_black;
            float pfsz = fmaxf(8.f, fminf(20.f, pr.font_size * ph * 4.5f));
            ImVec2 tsz2 = pfont->CalcTextSizeA(pfsz, FLT_MAX, -1.f, sample);

            // Horizontal anchor
            float pax = px0 + pr.sub_pos_x * pw;
            float plx;
            if (pr.sub_anchor_h == 0)      plx = pax;
            else if (pr.sub_anchor_h == 2) plx = pax - tsz2.x;
            else                            plx = pax - tsz2.x * 0.5f;
            plx = fmaxf(px0, fminf(px1 - tsz2.x, plx));

            // Vertical slot
            float ply;
            if (pr.sub_pos == 1)      ply = py0 + ph * 0.5f - tsz2.y * 0.5f;
            else if (pr.sub_pos == 2) ply = py0 + 2.f;
            else if (pr.sub_pos == 3) ply = py0 + pr.sub_pos_y * ph - tsz2.y * 0.5f;
            else                       ply = py1 - tsz2.y - 2.f;
            ply = fmaxf(py0, fminf(py1 - tsz2.y, ply));

            ImU32 tcol = IM_COL32((int)(pr.color[0]*255), (int)(pr.color[1]*255),
                                   (int)(pr.color[2]*255), (int)(pr.color[3]*255));
            dl->AddText(pfont, pfsz, {plx, ply}, tcol, sample);

            // Anim style badge bottom-right inside preview
            const char* style_tag = nullptr;
            switch (pr.style) {
                case AnimStyle::Fade:   style_tag = "fade";   break;
                case AnimStyle::Slide:  style_tag = "slide";  break;
                case AnimStyle::Scale:  style_tag = "scale";  break;
                case AnimStyle::Block:  style_tag = "block";  break;
                case AnimStyle::Glitch: style_tag = "glitch"; break;
                default: break;
            }
            if (style_tag) {
                ImVec2 bsz = ImGui::GetFont()->CalcTextSizeA(9.f, FLT_MAX, -1.f, style_tag);
                dl->AddText(ImGui::GetFont(), 9.f,
                    {px1 - bsz.x - 4.f, py1 - bsz.y - 2.f},
                    IM_COL32(100, 90, 140, 180), style_tag);
            }

            dl->PopClipRect();
        }

        ImGui::SetCursorScreenPos(cp);
        char btn_id[64]; snprintf(btn_id, sizeof(btn_id), "##tycard_%s", pr.id);
        ImGui::InvisibleButton(btn_id, {cell_w, cell_h});
        if (ImGui::IsItemClicked()) {
            state.typo_preset_id = pr.id;
            state.typo_font_size = 0.f;
            memset(state.typo_color, 0, sizeof(state.typo_color));
            state.typo_all_caps_override = false;
            generate_typography(state);
        }

        col_idx++;
        if (col_idx >= 2) { col_idx = 0; ImGui::Dummy({0.f, gap}); }
    }
    if (col_idx == 1) ImGui::NewLine();

    ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

    // ── Tune ──────────────────────────────────────────────────────────────────
    const TypographyPreset* pr = typo_preset_by_id(state.typo_preset_id.c_str());

    ui_label("Font Size");
    float fs = (state.typo_font_size > 0.001f) ? state.typo_font_size : (pr ? pr->font_size : 0.09f);
    ImGui::SetNextItemWidth(full_w);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
    if (ImGui::SliderFloat("##tyfo", &fs, 0.03f, 0.30f, "%.2f")) {
        state.typo_font_size = fs;
        typo_restyle_live(state);
    }
    ImGui::PopStyleColor();

    ImGui::Dummy({0.f, 8.f});

    ui_label("Color");
    float col_buf[4];
    const float* src_col = (state.typo_color[3] > 0.001f) ? state.typo_color
                           : (pr ? pr->color : state.typo_color);
    memcpy(col_buf, src_col, sizeof(col_buf));
    ImGui::SetNextItemWidth(full_w);
    if (ImGui::ColorEdit4("##tycol", col_buf,
            ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar)) {
        memcpy(state.typo_color, col_buf, sizeof(state.typo_color));
        typo_restyle_live(state);
    }
    palette_widget("##pal_typo", col_buf);
    if (memcmp(col_buf, state.typo_color, sizeof(col_buf)) != 0) {
        memcpy(state.typo_color, col_buf, sizeof(state.typo_color));
        typo_restyle_live(state);
    }

    ImGui::Dummy({0.f, 10.f});

    // ── Advanced ──────────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    bool adv_open = ImGui::TreeNodeEx("Advanced##typo_adv",
        ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding);
    ImGui::PopStyleColor();
    if (adv_open) {
        ImGui::Dummy({0.f, 6.f});

        bool caps = state.typo_all_caps_override ? state.typo_all_caps : (pr ? pr->all_caps : false);
        if (ImGui::Checkbox("ALL CAPS##tycaps", &caps)) {
            state.typo_all_caps_override = true;
            state.typo_all_caps = caps;
            typo_restyle_live(state);
        }

        ImGui::Dummy({0.f, 8.f});
        if (ui_btn("Reset font & color to preset", false, true)) {
            state.typo_font_size         = 0.f;
            state.typo_color[3]          = 0.f;
            state.typo_all_caps_override = false;
            typo_restyle_live(state);
        }

        ImGui::TreePop();
    }
}
