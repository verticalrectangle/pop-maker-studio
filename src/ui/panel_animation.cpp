#include "studio_types.h"
#include "studio_shared.h"
#include "panel_animation.h"
#include "panel_clip.h"   // section_fade / section_text_style (shared style controls)
#include "text_styles.h"
#include "pipeline.h"
#include "app.h"
#include "audio.h"
#include "history.h"
#include "typography_script.h"
#include "script_clip.h"
#include "generated/typography_js_presets.h"
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
// ── Typography as Script clips ────────────────────────────────────────────────
// A typography layer is ONE Script clip (`pms:typography/<id>`, params = Tune
// overrides). The picker lists the 76 JS presets by category with live
// thumbnails rendered through the Script clip pipeline; Tune edits the
// selected layer's params JSON in place.

#include "typography_script.h"
#include "script_clip.h"
#include "generated/typography_js_presets.h"

// The selected typography layer, if any (a Script clip with a pms:typography/
// entry). Tune edits its params; the picker replaces its preset.
static bool typo_selected_layer(AppState& state, int& out_ti, int& out_ci) {
    if (state.selected_track >= 0 && state.selected_track < (int)state.tracks.size()) {
        auto& clips = state.tracks[state.selected_track].clips;
        if (state.selected_clip >= 0 && state.selected_clip < (int)clips.size()) {
            const Clip& c = clips[state.selected_clip];
            if (c.clip_type == ClipType::Script &&
                c.script_path.rfind("pms:typography/", 0) == 0) {
                out_ti = state.selected_track;
                out_ci = state.selected_clip;
                return true;
            }
        }
    }
    // Fall back to the first typography layer on the timeline.
    for (int ti = 0; ti < (int)state.tracks.size(); ++ti)
        for (int ci = 0; ci < (int)state.tracks[ti].clips.size(); ++ci) {
            const Clip& c = state.tracks[ti].clips[ci];
            if (c.clip_type == ClipType::Script &&
                c.script_path.rfind("pms:typography/", 0) == 0) {
                out_ti = ti;
                out_ci = ci;
                return true;
            }
        }
    return false;
}

// Sample time for thumbnails: middle of the transcript words mapped to the
// timeline (same mapping as lay_typography_script), else the playhead.
static float typo_thumb_t(const AppState& state) {
    if (!state.words_cache.empty() && !state.audio_path.empty()) {
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
        if (t1 > t0) return t0 + (t1 - t0) * 0.35f;
    }
    return state.playhead;
}

// Read a numeric param (or ts_* key) from the clip's params JSON.
static float typo_param_num(const Clip& c, const char* key, float dflt) {
    if (c.script_params.empty()) return dflt;
    try {
        auto j = nlohmann::json::parse(c.script_params);
        if (j.contains(key) && j[key].is_number()) return j[key].get<float>();
    } catch (...) {}
    return dflt;
}

static void typo_param_set(Clip& c, const char* key, float v) {
    nlohmann::json j = nlohmann::json::object();
    if (!c.script_params.empty()) {
        try { j = nlohmann::json::parse(c.script_params); } catch (...) {}
    }
    j[key] = v;
    c.script_params = j.dump();
}

static void typo_param_set4(Clip& c, const char* key, const float v[4]) {
    nlohmann::json j = nlohmann::json::object();
    if (!c.script_params.empty()) {
        try { j = nlohmann::json::parse(c.script_params); } catch (...) {}
    }
    j[key] = {v[0], v[1], v[2], v[3]};
    c.script_params = j.dump();
}

static void typo_params_get4(const Clip& c, const char* key, float v[4], const float dflt[4]) {
    for (int i = 0; i < 4; ++i) v[i] = dflt[i];
    if (c.script_params.empty()) return;
    try {
        auto j = nlohmann::json::parse(c.script_params);
        if (j.contains(key) && j[key].is_array() && j[key].size() == 4)
            for (int i = 0; i < 4; ++i) v[i] = j[key][i].get<float>();
    } catch (...) {}
}

// Category accent color
static ImU32 typo_category_dot(const char* cat) {
    if (strcmp(cat, "Hype")      == 0) return IM_COL32(255,  60,  80, 255);
    if (strcmp(cat, "Aesthetic") == 0) return IM_COL32(220, 130, 255, 255);
    if (strcmp(cat, "Editorial") == 0) return IM_COL32(255, 200,  50, 255);
    if (strcmp(cat, "Clean")     == 0) return IM_COL32( 80, 200, 255, 255);
    if (strcmp(cat, "Retro")     == 0) return IM_COL32(255, 140,  40, 255);
    if (strcmp(cat, "Kinetic")   == 0) return IM_COL32(140, 255, 150, 255);
    if (strcmp(cat, "Karaoke")   == 0) return IM_COL32(255, 220,  80, 255);
    if (strcmp(cat, "Gradient")  == 0) return IM_COL32(255, 130, 255, 255);
    if (strcmp(cat, "Script")    == 0) return IM_COL32(255, 170, 220, 255);
    if (strcmp(cat, "Mono")      == 0) return IM_COL32(120, 255, 230, 255);
    return IM_COL32(180, 180, 180, 255);
}

void panel_typography(AppState& state, float w) {
    float full_w = w - 16.f;
    ImGui::Dummy({0.f, 8.f});

    int lti = -1, lci = -1;
    bool has_layer = typo_selected_layer(state, lti, lci);
    std::string active = active_typography_preset(state);
    const char* active_name = active.c_str();
    for (int i = 0; i < kNTypoJsPresets; ++i)
        if (active == kTypoJsPresets[i].id) { active_name = kTypoJsPresets[i].name; break; }

    if (state.words_cache.empty()) {
        ImGui::Dummy({0.f, 40.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        const char* msg = "Transcribe or set a transcript to lay typography";
        float tw = ImGui::CalcTextSize(msg).x;
        ImGui::SetCursorPosX((w - tw) * 0.5f);
        ImGui::TextUnformatted(msg);
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 6.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
        ImGui::TextWrapped("Right-click an audio/video clip and choose "
                           "\"Make lyric video\", or run trigger_pipeline.");
        ImGui::PopStyleColor();
        return;
    }

    // ── Browse presets (collapsible) ──────────────────────────────────────────
    char blbl[128];
    snprintf(blbl, sizeof(blbl), "Browse presets  ·  %s###typo_browse",
             active.empty() ? "none" : active_name);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    bool browse_open = ImGui::TreeNodeEx(blbl,
        ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding);
    ImGui::PopStyleColor();
    if (browse_open) {
    ImGui::Dummy({0.f, 6.f});

    static std::string s_typo_cat;   // empty = All
    static std::string* s_typo_q = nullptr;
    {
        std::vector<const char*> cats;
        for (int i = 0; i < kNTypoJsPresets; ++i) {
            const char* c = kTypoJsPresets[i].category;
            bool seen = false;
            for (auto* x : cats) if (strcmp(x, c) == 0) { seen = true; break; }
            if (!seen) cats.push_back(c);
        }
        static std::string s_typo_query;
        category_pills("typocat", cats, s_typo_cat, s_typo_query);
        ImGui::Dummy({0.f, 8.f});
        s_typo_q = &s_typo_query;
    }

    // Keep the current layer's Tune params when swapping presets ({} = none).
    std::string keep_params = "{}";
    if (has_layer) {
        const Clip& lc = state.tracks[lti].clips[lci];
        if (!lc.script_params.empty()) keep_params = lc.script_params;
    }

    // Thumbnail geometry: project canvas size, card-sized surface.
    int cw = 1080, ch = 1920;
    output_format_px(state.format, cw, ch);
    float sample_t = typo_thumb_t(state);

    const float gap    = 4.f;
    const float cell_w = (full_w - gap) * 0.5f;
    const float cell_h = 112.f;

    const char* cur_cat = nullptr;
    int col_idx = 0;

    for (int i = 0; i < kNTypoJsPresets; ++i) {
        const TypoJsPreset& pr = kTypoJsPresets[i];
        if (!s_typo_cat.empty() && s_typo_cat != pr.category) continue;
        if (s_typo_q && !lib_search_match(*s_typo_q, pr.name, pr.tagline)) continue;
        bool selected = (active == pr.id);

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
        dl->AddRectFilled({cp.x, cp.y + 10.f}, {cp.x + 3.f, cp.y + cell_h - 10.f},
            typo_category_dot(pr.category), 2.f);
        dl->AddRect(cp, {cp.x + cell_w, cp.y + cell_h}, brd_col, 6.f, 0, brd_w);

        float tx = cp.x + 10.f;
        ImGui::PushFont(g_font_bold);
        dl->AddText(ImGui::GetFont(), 12.f, {tx, cp.y + 10.f},
            selected ? IM_COL32(255,255,255,255) : IM_COL32(210,205,230,240), pr.name);
        ImGui::PopFont();

        {
            float tag_w = cell_w - 16.f;
            ImVec4 tag_clip = {tx, cp.y + 24.f, cp.x + cell_w - 6.f, cp.y + 52.f};
            dl->AddText(ImGui::GetFont(), 11.f, {tx, cp.y + 25.f},
                        IM_COL32(120, 115, 145, 200), pr.tagline, nullptr,
                        tag_w, &tag_clip);
        }

        // ── Live thumbnail through the Script clip pipeline ───────────────────
        {
            float px0 = cp.x + 6.f, px1 = cp.x + cell_w - 6.f;
            float py0 = cp.y + 56.f, py1 = cp.y + cell_h - 6.f;
            float pw  = px1 - px0, ph = py1 - py0;

            dl->AddRectFilled({px0, py0}, {px1, py1}, IM_COL32(10, 8, 18, 220), 3.f);
            dl->AddRect({px0, py0}, {px1, py1}, IM_COL32(40, 36, 58, 180), 3.f);
            if (ImGui::IsRectVisible({px0, py0}, {px1, py1})) {
                Clip tmp;
                tmp.clip_type = ClipType::Script;
                tmp.start = 0.f;
                tmp.end = sample_t + 1.f;
                tmp.script_path = std::string("pms:typography/") + pr.id;
                tmp.script_params = keep_params;
                std::vector<ScriptError> serr;
                char keybuf[64];
                snprintf(keybuf, sizeof(keybuf), "typo_preview:%s", pr.id);
                unsigned tex = script_clip_texture(state, tmp, keybuf, sample_t,
                    cw, ch, (int)pw, (int)ph, false, serr);
                if (tex)
                    dl->AddImage((ImTextureID)(uintptr_t)tex,
                                 {px0, py0}, {px1, py1}, {0, 0}, {1, 1});
            }
        }

        ImGui::SetCursorScreenPos(cp);
        char btn_id[64]; snprintf(btn_id, sizeof(btn_id), "##tycard_%s", pr.id);
        ImGui::InvisibleButton(btn_id, {cell_w, cell_h});
        if (ImGui::IsItemClicked()) {
            std::string err;
            // Swap keeps the layer's Tune params; Reset (below) clears them.
            if (!lay_typography_script(state, pr.id, keep_params, err) && !err.empty()) {
                state.snapshot_msg = err;
                state.snapshot_msg_new = true;
            }
        }

        col_idx++;
        if (col_idx >= 2) { col_idx = 0; ImGui::Dummy({0.f, gap}); }
    }
    if (col_idx == 1) ImGui::NewLine();
    ImGui::TreePop();
    }   // browse_open

    ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

    if (!has_layer) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
        ImGui::TextWrapped("Pick a preset above to lay the typography layer.");
        ImGui::PopStyleColor();
        return;
    }

    // ── Tune: edit the layer's params ─────────────────────────────────────────
    Clip& lc = state.tracks[lti].clips[lci];
    auto touch = [&](const char* action) {
        script_clip_invalidate(script_clip_key(lti, lci));
        history_push(state, action);
    };

    {
        ui_label("Font Size");
        float out_h  = output_px_height(state);
        float fs = typo_param_num(lc, "fontSize", 0.09f);
        float fs_px  = fs * out_h;
        ImGui::SetNextItemWidth(full_w);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
        if (ImGui::SliderFloat("##tyfo", &fs_px, 0.03f * out_h, 0.30f * out_h, "%.0f px")) {
            typo_param_set(lc, "fontSize", fs_px / out_h);
            touch("Typography font size");
        }
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 8.f});
    }

    {
        ui_label("Color");
        float col_buf[4]; const float dcol[4] = {1, 1, 1, 1};
        typo_params_get4(lc, "color", col_buf, dcol);
        ImGui::SetNextItemWidth(full_w);
        if (ImGui::ColorEdit4("##tycol", col_buf,
                ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar)) {
            typo_param_set4(lc, "color", col_buf);
            touch("Typography color");
        }
        palette_widget("##pal_typo", col_buf);
        ImGui::SameLine(0.f, 6.f);
        {
            float dp[3];
            if (ui_dropper_button("##dp_typo", dp)) {
                col_buf[0] = dp[0]; col_buf[1] = dp[1]; col_buf[2] = dp[2];
                typo_param_set4(lc, "color", col_buf);
                touch("Typography color");
            }
        }
        ImGui::Dummy({0.f, 8.f});
    }

    {
        ui_label("Alignment");
        int cur = (int)typo_param_num(lc, "anchorH", 1.f);
        struct AlignBtn { int v; const char* label; };
        AlignBtn abtns[] = {{0,"Left"},{1,"Center"},{2,"Right"}};
        ImGui::PushID("ty_align");
        for (auto& ab : abtns) {
            if (ui_btn(ab.label, cur == ab.v, true)) {
                typo_param_set(lc, "anchorH", (float)ab.v);
                touch("Typography alignment");
            }
            ImGui::SameLine(0.f, 4.f);
        }
        ImGui::PopID();
        ImGui::NewLine();
        ImGui::Dummy({0.f, 8.f});
    }

    {
        ui_label("Vertical");
        int cur = (int)typo_param_num(lc, "pos", 1.f);
        struct VBtn { int v; const char* label; };
        VBtn vbtns[] = {{0,"Bottom"},{1,"Center"},{2,"Top"}};
        ImGui::PushID("ty_vert");
        for (auto& vb : vbtns) {
            if (ui_btn(vb.label, cur == vb.v, true)) {
                typo_param_set(lc, "pos", (float)vb.v);
                touch("Typography vertical");
            }
            ImGui::SameLine(0.f, 4.f);
        }
        ImGui::PopID();
        ImGui::NewLine();
        ImGui::Dummy({0.f, 10.f});
    }

    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    bool adv_open = ImGui::TreeNodeEx("Advanced##typo_adv",
        ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding);
    ImGui::PopStyleColor();
    if (adv_open) {
        ImGui::Dummy({0.f, 6.f});

        ui_label("Letter case");
        int cur_case = (int)typo_param_num(lc, "textCase", 0.f);
        struct CaseBtn { int v; const char* label; };
        CaseBtn cbtns[] = {{0,"As typed"},{1,"AA"},{2,"aa"}};
        for (auto& cb : cbtns) {
            if (ui_btn(cb.label, cur_case == cb.v, true)) {
                typo_param_set(lc, "textCase", (float)cb.v);
                touch("Typography case");
            }
            ImGui::SameLine(0.f, 4.f);
        }
        ImGui::NewLine();

        ImGui::Dummy({0.f, 8.f});
        ui_label("Letter spacing");
        float trk = typo_param_num(lc, "tracking", 0.f);
        ImGui::SetNextItemWidth(full_w);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
        if (ImGui::SliderFloat("##tytrk", &trk, -0.1f, 0.5f, "%.2f")) {
            typo_param_set(lc, "tracking", trk);
            touch("Typography tracking");
        }
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 8.f});
        ui_label("Wrap width");
        float wrp = typo_param_num(lc, "wrapW", 0.85f);
        ImGui::SetNextItemWidth(full_w);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
        if (ImGui::SliderFloat("##tywrap", &wrp, 0.2f, 1.0f, "%.2f")) {
            typo_param_set(lc, "wrapW", wrp);
            touch("Typography wrap");
        }
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 8.f});
        ui_label("X offset");
        float px = typo_param_num(lc, "posX", 0.5f);
        ImGui::SetNextItemWidth(full_w);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
        if (ImGui::SliderFloat("##typx", &px, 0.f, 1.f, "%.2f")) {
            typo_param_set(lc, "posX", px);
            touch("Typography X offset");
        }
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 8.f});
        ui_label("Karaoke highlight");
        float kh[4]; const float dkh[4] = {1, 0.85f, 0.1f, 1};
        typo_params_get4(lc, "karaokeHi", kh, dkh);
        ImGui::SetNextItemWidth(full_w);
        if (ImGui::ColorEdit4("##tykhi", kh,
                ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar)) {
            typo_param_set4(lc, "karaokeHi", kh);
            touch("Typography karaoke highlight");
        }
        ImGui::Dummy({0.f, 8.f});

        ImGui::Dummy({0.f, 10.f});
        ui_label("Fade");
        float fi = typo_param_num(lc, "fadeIn", 0.f);
        float fo = typo_param_num(lc, "fadeOut", 0.f);
        if (section_fade(state, fi, fo, full_w)) {
            typo_param_set(lc, "fadeIn", fi);
            typo_param_set(lc, "fadeOut", fo);
            touch("Typography fade");
        }

        ImGui::Dummy({0.f, 10.f});
        ui_label("Text style");
        {
            float se = typo_param_num(lc, "ts_shadow_enabled", 1.f);
            float sw = typo_param_num(lc, "ts_stroke_enabled", 0.f);
            float gl = typo_param_num(lc, "ts_glow_enabled", 0.f);
            float bg = typo_param_num(lc, "ts_bg_enabled", 0.f);
            bool sh = se > 0.5f, st = sw > 0.5f, go = gl > 0.5f, bo = bg > 0.5f;
            bool changed = false;
            if (ImGui::Checkbox("Shadow##tyts_sh", &sh)) { typo_param_set(lc, "ts_shadow_enabled", sh ? 1.f : 0.f); changed = true; }
            ImGui::SameLine();
            if (ImGui::Checkbox("Stroke##tyts_st", &st)) { typo_param_set(lc, "ts_stroke_enabled", st ? 1.f : 0.f); changed = true; }
            ImGui::SameLine();
            if (ImGui::Checkbox("Glow##tyts_gl", &go)) { typo_param_set(lc, "ts_glow_enabled", go ? 1.f : 0.f); changed = true; }
            ImGui::SameLine();
            if (ImGui::Checkbox("Box##tyts_bg", &bo)) { typo_param_set(lc, "ts_bg_enabled", bo ? 1.f : 0.f); changed = true; }
            if (changed) touch("Typography text style");
        }

        ImGui::Dummy({0.f, 8.f});
        if (ui_btn("Reset all to preset", false, true)) {
            lc.script_params = "{}";
            touch("Typography reset");
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

// A 2 s freestanding lyric brick at `start` (empty source_id so a
// transcript regen never wipes it). Manual Text-clip styling applies.
Clip make_lyric_brick(AppState& state, float start) {
    (void)state;
    Clip c;
    c.clip_type = ClipType::Lyrics;
    c.text      = "Lyric";
    c.start     = start;
    c.end       = start + 2.f;
    c.sub_pos   = 1;
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

// Drop a lyric brick onto the lyrics track at the playhead, then select it.
static void add_lyric_brick_here(AppState& state) {
    Clip c = make_lyric_brick(state, state.playhead);
    int target = -1;
    for (int i = 0; i < (int)state.tracks.size(); ++i)
        if (is_lyrics_track(state.tracks[i])) { target = i; break; }
    if (target < 0) {
        Track t; t.name = "Lyrics"; t.kind = TrackKind::Lyrics;
        state.tracks.insert(state.tracks.begin(), std::move(t));
        target = 0;
    }
    state.tracks[target].clips.push_back(std::move(c));
    state.selected_track = target;
    state.selected_clip  = (int)state.tracks[target].clips.size() - 1;
    clip_flash(state, target, state.selected_clip, /*reveal=*/true);
    s_panel_view = PanelView::Clip;   // jump to the lyric text editor
    history_push(state, "Add lyric brick");
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

void panel_lyric_library(AppState& state, float w) {
    ImGui::Dummy({0.f, 6.f});
    ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted("Lyric");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, to_u32(Col::muted));
    ImGui::TextWrapped("Add a lyric brick — the only brick a lyrics track holds. "
                       "Click to add at the playhead, or drag onto the timeline. "
                       "Edit its text in the Clip tab; skin it in Typography.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 8.f});

    float card_w = w - 8.f, card_h = 64.f;
    ImVec2 cp = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool hov = ImGui::IsMouseHoveringRect(cp, {cp.x + card_w, cp.y + card_h});

    dl->AddRectFilled(cp, {cp.x + card_w, cp.y + card_h},
                      hov ? IM_COL32(34, 44, 34, 255) : IM_COL32(22, 22, 28, 255), 6.f);
    dl->AddRect(cp, {cp.x + card_w, cp.y + card_h},
                hov ? IM_COL32(120, 180, 90, 220) : IM_COL32(50, 50, 62, 200), 6.f, 0, 1.2f);
    ImGui::PushFont(g_font_bold);
    dl->AddText(ImGui::GetFont(), 16.f, {cp.x + 14.f, cp.y + 13.f}, to_u32(Col::fg), "+ Add Lyric");
    ImGui::PopFont();
    dl->AddText({cp.x + 14.f, cp.y + 37.f}, IM_COL32(140, 140, 160, 220),
                "Durable line \xe2\x80\x94 typography skins it");

    ImGui::InvisibleButton("##add_lyric_brick", {card_w, card_h});
    if (ImGui::IsItemClicked()) add_lyric_brick_here(state);
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        int dummy = 0;
        ImGui::SetDragDropPayload("LYRIC_BRICK", &dummy, sizeof(int));
        ImDrawList* gdl = ImGui::GetForegroundDrawList();
        ImVec2 gp = ImGui::GetMousePos();
        gdl->AddRectFilled({gp.x + 8.f, gp.y + 8.f}, {gp.x + 148.f, gp.y + 44.f},
                           IM_COL32(30, 60, 30, 230), 6.f);
        gdl->AddRect({gp.x + 8.f, gp.y + 8.f}, {gp.x + 148.f, gp.y + 44.f},
                     IM_COL32(120, 180, 90, 200), 6.f, 0, 1.2f);
        gdl->AddText({gp.x + 20.f, gp.y + 20.f}, IM_COL32(255, 255, 255, 240), "Lyric brick");
        ImGui::EndDragDropSource();
    }
    if (hov) ImGui::SetTooltip("Add a lyric brick (text in the Clip tab, skin in Typography)");

    ImGui::Dummy({0.f, 12.f});
}
