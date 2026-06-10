#include "studio_types.h"
#include "studio_shared.h"
#include "panel_fx.h"
#include "app.h"
#include "body_fx.h"
#include "bg_remove.h"
#include "runtime_fx.h"
#include "video.h"
#include "history.h"
#include "filepicker.h"
#include "audio_fx.h"
#include "hf_api.h"
#include "vc_job.h"
#include "bg_presets.h"
#include "theme.h"
#include "presets.h"
#include "globals.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <filesystem>
#include <map>
#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;
extern ImFont* g_font_bold;

// ── FX preset helpers ─────────────────────────────────────────────────────────

void preset_apply(Clip& clip, const EffectPreset& p) {
    clip.fx_color_on    = p.fx_color_on;
    clip.fx_brightness  = p.fx_brightness;
    clip.fx_contrast    = p.fx_contrast;
    clip.fx_saturation  = p.fx_saturation;
    clip.fx_hue         = p.fx_hue;
    clip.fx_blur_on     = p.fx_blur_on;
    clip.fx_blur        = p.fx_blur;
    clip.fx_vignette_on = p.fx_vignette_on;
    clip.fx_vignette    = p.fx_vignette;
    clip.fx_text_on     = p.fx_text_on;
    clip.fx_opacity_mul = p.fx_opacity_mul;
    clip.fx_scale_mul   = p.fx_scale_mul;
}

static EffectPreset preset_from_clip(const Clip& clip, const std::string& name) {
    EffectPreset p;
    p.name           = name;
    p.category       = PresetCategory::User;
    p.fx_color_on    = clip.fx_color_on;
    p.fx_brightness  = clip.fx_brightness;
    p.fx_contrast    = clip.fx_contrast;
    p.fx_saturation  = clip.fx_saturation;
    p.fx_hue         = clip.fx_hue;
    p.fx_blur_on     = clip.fx_blur_on;
    p.fx_blur        = clip.fx_blur;
    p.fx_vignette_on = clip.fx_vignette_on;
    p.fx_vignette    = clip.fx_vignette;
    p.fx_text_on     = clip.fx_text_on;
    p.fx_opacity_mul = clip.fx_opacity_mul;
    p.fx_scale_mul   = clip.fx_scale_mul;
    return p;
}



// ── Shared FX card catalogue ──────────────────────────────────────────────────
struct FXCard { FXType type; const char* name; const char* tagline; ImU32 accent; };
static const FXCard g_fx_cards[] = {
    {FXType::ChromaKey, "Chroma Key",  "Color-range keyer  ·  green screen  ·  compositing", IM_COL32(50,220,120,255)},
    {FXType::Glitch,    "Glitch",      "RGB split  ·  row corruption  ·  digital tear",       IM_COL32(0,210,220,255)},
    {FXType::ZoomPunch, "Zoom Punch",  "Beat-synced scale spike  ·  shake",                   IM_COL32(255,135,40,255)},
    {FXType::LUT,       "LUT Grade",   "Load any .cube file  ·  cinematic color grade",       IM_COL32(255,205,55,255)},
    {FXType::LightLeak, "Light Leak",  "Film flare  ·  amplitude-driven  ·  Screen blend",    IM_COL32(255,90,160,255)},
    {FXType::VHS,       "VHS",         "Chroma bleed  ·  grain  ·  tracking glitch",          IM_COL32(110,195,95,255)},
    {FXType::Datamosh,  "Datamosh",    "Temporal ghost  ·  multi-key chaos  ·  total mosh",   IM_COL32(255,60,100,255)},
    {FXType::Grade,     "Grade",       "colour  ·  contrast  ·  saturation",                  IM_COL32(100,80,200,255)},
    {FXType::Blur,      "Blur",        "gaussian softness",                                    IM_COL32(100,80,200,255)},
    {FXType::Vignette,  "Vignette",    "radial darkening",                                    IM_COL32(100,80,200,255)},
#include "generated/fx_ui_picker.h"
};
static const int g_n_fx_cards = (int)(sizeof(g_fx_cards) / sizeof(g_fx_cards[0]));

// ── Right panel: Adjustment Library tab ──────────────────────────────────────

void panel_adjustment_library(AppState& state, float w) {
    ImGui::Dummy({0.f, 8.f});

    ImGui::TextUnformatted("Adjustment Library");
    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
    ImGui::TextWrapped("Click to create a new Grade clip with the preset applied. Drag to timeline to create one.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 8.f});

    float card_w  = w - 8.f;  // single column like FX cards
    float card_h  = 80.f;
    float thumb_w = card_h * (108.f / 192.f);

    auto draw_preset_card = [&](const EffectPreset& p, int unique_id) {
        ImGui::PushID(unique_id);
        ImVec2 cp = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        bool hov = ImGui::IsMouseHoveringRect(cp, {cp.x + card_w, cp.y + card_h});

        // Card background
        dl->AddRectFilled(cp, {cp.x+card_w, cp.y+card_h},
                          hov ? IM_COL32(28,28,40,255) : IM_COL32(18,18,28,255), 5.f);

        // Portrait thumbnail on the left
        uintptr_t prev_tex = video_adj_preview_texture(unique_id,
            p.fx_color_on ? p.fx_brightness : 0.f,
            p.fx_color_on ? p.fx_contrast   : 1.f,
            p.fx_color_on ? p.fx_saturation : 1.f,
            p.fx_color_on ? p.fx_hue        : 0.f,
            p.fx_blur_on     ? p.fx_blur     : 0.f,
            p.fx_vignette_on ? p.fx_vignette : 0.f);
        if (prev_tex) {
            dl->AddImageRounded((ImTextureID)(uintptr_t)prev_tex,
                                cp, {cp.x+thumb_w, cp.y+card_h},
                                {0,0}, {1,1},
                                hov ? IM_COL32(255,255,255,230) : IM_COL32(255,255,255,190),
                                5.f, ImDrawFlags_RoundCornersLeft);
        }

        // Border
        dl->AddRect(cp, {cp.x+card_w, cp.y+card_h},
                    hov ? IM_COL32(255,255,255,200) : IM_COL32(60,60,80,200), 5.f, 0, hov ? 2.f : 1.f);

        // Name to the right of thumbnail
        float tx = cp.x + thumb_w + 10.f;
        ImGui::PushFont(g_font_bold);
        dl->AddText(ImGui::GetFont(), 13.f, {tx, cp.y+14.f}, IM_COL32(255,255,255,240), p.name.c_str());
        ImGui::PopFont();

        // Invisible button over the card for click and drag-drop
        ImGui::SetCursorScreenPos(cp);
        ImGui::InvisibleButton("##card", {card_w, card_h});
        bool clicked = ImGui::IsItemClicked(0);

        // Drag-drop source — payload is index into the combined preset list
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload("FX_PRESET", &unique_id, sizeof(int));
            ImGui::TextUnformatted(p.name.c_str());
            ImGui::TextUnformatted("Drop onto timeline track");
            ImGui::EndDragDropSource();
        }

        ImGui::PopID();

        if (clicked) {
            // Always create a new Grade clip with the preset applied
            Clip cl;
            cl.clip_type     = ClipType::Effect;
            cl.fx_type       = FXType::Grade;
            cl.fx_color_on   = true;
            cl.start         = state.playhead;
            cl.end           = state.playhead + 2.f;
            preset_apply(cl, p);
            Track nt; nt.name = "Grade";
            nt.clips.push_back(cl);
            state.tracks.insert(state.tracks.begin(), std::move(nt));
            state.selected_track = 0;
            state.selected_clip  = 0;
            history_push(state, "Add Grade: " + p.name);
        }
    };

    // Combined preset list: built-ins first, then user presets.
    // Index 0..N-1 = built-ins, N.. = user.
    int builtin_count = (int)g_builtin_presets.size();

    auto draw_section = [&](const char* label, PresetCategory cat) {
        ui_separator();
        ImGui::Dummy({0.f, 4.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 4.f});

        for (int i = 0; i < (int)g_builtin_presets.size(); ++i) {
            if (g_builtin_presets[i].category != cat) continue;
            draw_preset_card(g_builtin_presets[i], i);
            ImGui::Dummy({0.f, 4.f});
        }
        ImGui::Dummy({0.f, 4.f});
    };

    draw_section("Color",    PresetCategory::Color);
    draw_section("Blur",     PresetCategory::Blur);
    draw_section("Vignette", PresetCategory::Vignette);
    draw_section("Combo",    PresetCategory::Combo);

    // User presets section
    if (!state.user_presets.empty()) {
        ui_separator();
        ImGui::Dummy({0.f, 4.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted("My Presets");
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 4.f});

        for (int i = 0; i < (int)state.user_presets.size(); ++i) {
            draw_preset_card(state.user_presets[i], builtin_count + i);
            // Right-click to delete user preset
            if (ImGui::BeginPopupContextItem(("##userpctx" + std::to_string(i)).c_str())) {
                if (ImGui::MenuItem("Delete preset")) {
                    state.user_presets.erase(state.user_presets.begin() + i);
                    presets_save_user(state.user_presets);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::Dummy({0.f, 4.f});
        }
    }

    // ── Color Grade & Tone ─────────────────────────────────────────────────────
    {
        ui_separator();
        ImGui::Dummy({0.f, 4.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted("Color Grade & Tone");
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
        ImGui::TextWrapped("Static color grades. Click to add as a color grade brick.");
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 4.f});

        float cg_card_w  = w - 8.f;
        float cg_card_h  = 80.f;
        float cg_thumb_w = cg_card_h * (108.f / 192.f);

        for (int i = 0; i < g_n_fx_cards; ++i) {
            if (!fx_type_is_adjustment_style(g_fx_cards[i].type)) continue;
            const FXCard& fc = g_fx_cards[i];
            ImGui::PushID(i + 19000);
            ImVec2 cp = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            bool hov = ImGui::IsMouseHoveringRect(cp, {cp.x+cg_card_w, cp.y+cg_card_h});
            dl->AddRectFilled(cp, {cp.x+cg_card_w, cp.y+cg_card_h},
                              hov ? IM_COL32(28,22,48,255) : IM_COL32(18,14,32,255), 5.f);
            dl->AddRectFilled(cp, {cp.x+3.f, cp.y+cg_card_h}, IM_COL32(100,80,200,200), 2.f);

            uintptr_t prev_tex = video_fx_preview_texture(fc.type, (float)ImGui::GetTime());
            if (prev_tex)
                dl->AddImageRounded((ImTextureID)(uintptr_t)prev_tex,
                                    cp, {cp.x+cg_thumb_w, cp.y+cg_card_h},
                                    {0,0},{1,1},
                                    hov ? IM_COL32(255,255,255,230) : IM_COL32(255,255,255,190),
                                    5.f, ImDrawFlags_RoundCornersLeft);

            dl->AddRect(cp, {cp.x+cg_card_w, cp.y+cg_card_h},
                        hov ? IM_COL32(160,130,255,220) : IM_COL32(80,60,160,180), 5.f, 0, hov ? 2.f : 1.f);

            float tx = cp.x + cg_thumb_w + 10.f;
            ImGui::PushFont(g_font_bold);
            dl->AddText(ImGui::GetFont(), 13.f, {tx, cp.y+10.f}, IM_COL32(255,255,255,240), fc.name);
            ImGui::PopFont();
            dl->AddText({tx, cp.y+27.f}, IM_COL32(160,160,170,200), fc.tagline);

            if (hov) {
                const char* al = "+ Add";
                ImVec2 sz = ImGui::CalcTextSize(al);
                dl->AddText({cp.x+cg_card_w-sz.x-10.f, cp.y+cg_card_h-16.f}, IM_COL32(255,255,255,200), al);
            }

            ImGui::SetCursorScreenPos(cp);
            ImGui::InvisibleButton("##cgcard", {cg_card_w, cg_card_h});
            if (ImGui::IsItemClicked()) {
                Clip cl;
                cl.clip_type = ClipType::Effect;
                cl.fx_type   = fc.type;
                cl.start     = state.playhead;
                cl.end       = state.playhead + 5.f;
                Track nt; nt.name = fc.name;
                nt.clips.push_back(cl);
                state.tracks.insert(state.tracks.begin(), std::move(nt));
                state.selected_track = 0;
                state.selected_clip  = 0;
                history_push(state, std::string("Add Color Grade: ") + fc.name);
            }

            ImGui::Dummy({0.f, 4.f});
            ImGui::PopID();
        }
    }

    ImGui::Dummy({0.f, 16.f});

    // ── Custom (runtime FX) section ───────────────────────────────────────────
    {
        const auto& defs = runtime_fx_list();
        if (!defs.empty()) {
            ui_separator();
            ImGui::Dummy({0.f, 4.f});
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted("Custom");
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextWrapped("Hot-reload effects — edit the JSON in effects/ and changes appear live.");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 4.f});

            float card_w = w - 8.f;
            float card_h = 64.f;
            bool has_sel = (state.selected_track >= 0 && state.selected_track < (int)state.tracks.size() &&
                            state.selected_clip  >= 0 && state.selected_clip  < (int)state.tracks[state.selected_track].clips.size());

            for (int i = 0; i < (int)defs.size(); ++i) {
                const RuntimeFXDef& def = defs[i];
                ImGui::PushID(20000 + i);
                ImVec2 cp  = ImGui::GetCursorScreenPos();
                ImDrawList* dl = ImGui::GetWindowDrawList();

                bool failed = (def.program == 0);
                bool hov    = !failed && ImGui::IsMouseHoveringRect(cp, {cp.x+card_w, cp.y+card_h});

                ImU32 bg  = hov ? IM_COL32(22,30,50,255) : IM_COL32(14,18,30,255);
                ImU32 bdr = failed ? IM_COL32(180,60,60,180) :
                            (hov   ? IM_COL32(80,160,255,220)  : IM_COL32(50,80,140,180));

                dl->AddRectFilled(cp, {cp.x+card_w, cp.y+card_h}, bg, 5.f);
                dl->AddRectFilled(cp, {cp.x+3.f, cp.y+card_h}, bdr, 2.f);
                dl->AddRect(cp, {cp.x+card_w, cp.y+card_h}, bdr, 5.f, 0, hov ? 2.f : 1.f);

                ImU32 name_col = failed ? IM_COL32(200,80,80,255) : IM_COL32(255,255,255,240);
                ImGui::PushFont(g_font_bold);
                dl->AddText(ImGui::GetFont(), 13.f, {cp.x+12.f, cp.y+10.f}, name_col, def.name.c_str());
                ImGui::PopFont();

                std::string meta = def.category;
                if (!def.description.empty()) meta += "  ·  " + def.description;
                if (failed) meta = "compile error — hover for details";
                dl->AddText({cp.x+12.f, cp.y+27.f}, IM_COL32(140,150,170,200), meta.c_str());

                if ((int)def.params.size() > 0) {
                    char ps[32]; snprintf(ps, sizeof(ps), "%d param%s", (int)def.params.size(), def.params.size()==1?"":"s");
                    dl->AddText({cp.x+12.f, cp.y+44.f}, IM_COL32(100,110,130,200), ps);
                }

                ImGui::SetCursorScreenPos(cp);
                ImGui::InvisibleButton("##rfxcard", {card_w, card_h});

                if (failed && ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255,120,120,255));
                    ImGui::TextUnformatted(def.compile_error.c_str());
                    ImGui::PopStyleColor();
                    ImGui::EndTooltip();
                }

                if (!failed && ImGui::IsItemClicked()) {
                    if (has_sel) {
                        Clip& cl = state.tracks[state.selected_track].clips[state.selected_clip];
                        cl.runtime_fx_id = def.id;
                        // Initialize params to defaults
                        cl.runtime_fx_params.resize(def.params.size());
                        for (int pi = 0; pi < (int)def.params.size(); ++pi)
                            cl.runtime_fx_params[pi] = def.params[pi].default_val;
                        cl.runtime_fx_amount = 1.f;
                        history_push(state, "Apply custom FX: " + def.name);
                    } else {
                        ImGui::SetTooltip("Select a clip first to apply a custom effect.");
                    }
                }

                ImGui::Dummy({0.f, 4.f});
                ImGui::PopID();
            }
        }
    }

    ImGui::Dummy({0.f, 16.f});

    // ── Body FX section ───────────────────────────────────────────────────────────
    {
        ui_separator();
        ImGui::Dummy({0.f, 4.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted("Body FX");
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
        ImGui::TextWrapped("Glass bricks — applied to person and background independently using AI masks.");
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 4.f});

        // Find selected video clip
        bool has_vid = (state.selected_track >= 0 &&
                        state.selected_track < (int)state.tracks.size() &&
                        state.selected_clip  >= 0 &&
                        state.selected_clip  < (int)state.tracks[state.selected_track].clips.size() &&
                        state.tracks[state.selected_track].clips[state.selected_clip].clip_type == ClipType::Video);

        float card_w = w - 8.f;
        float card_h = 68.f;
        int n = body_fx_info_count();
        const BodyFXInfo* infos = body_fx_info_list();

        for (int i = 0; i < n; ++i) {
            const BodyFXInfo& info = infos[i];
            ImGui::PushID(40000 + i);
            ImVec2 cp = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            bool hov = has_vid && ImGui::IsMouseHoveringRect(cp, {cp.x+card_w, cp.y+card_h});
            ImU32 bg  = hov ? IM_COL32(20,28,46,255) : IM_COL32(12,16,28,255);
            dl->AddRectFilled(cp, {cp.x+card_w, cp.y+card_h}, bg, 5.f);
            dl->AddRectFilled(cp, {cp.x+3.f, cp.y+card_h}, (ImU32)info.accent, 2.f);
            dl->AddRect(cp, {cp.x+card_w, cp.y+card_h},
                        hov ? (ImU32)info.accent : IM_COL32(40,50,80,200), 5.f, 0, hov ? 2.f : 1.f);

            ImGui::PushFont(g_font_bold);
            dl->AddText(ImGui::GetFont(), 13.f, {cp.x+12.f, cp.y+8.f},
                        has_vid ? IM_COL32(255,255,255,240) : IM_COL32(120,120,140,200), info.name);
            ImGui::PopFont();
            dl->AddText({cp.x+12.f, cp.y+25.f}, IM_COL32(130,140,160,200), info.tagline);
            dl->AddText({cp.x+12.f, cp.y+44.f}, IM_COL32(80,100,130,180), info.category);

            ImGui::SetCursorScreenPos(cp);
            ImGui::InvisibleButton("##bfxcard", {card_w, card_h});

            if (!has_vid && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Select a video clip first.");
            }

            if (has_vid && ImGui::IsItemClicked()) {
                int ti = state.selected_track;
                int ci = state.selected_clip;
                Clip& vid_cl = state.tracks[ti].clips[ci];

                // Create BodyFX brick on same track, same time range
                Clip bfx;
                bfx.clip_type    = ClipType::BodyFX;
                bfx.start        = vid_cl.start;
                bfx.end          = vid_cl.end;
                bfx.source_id    = vid_cl.source_id;
                bfx.body_fx_type = info.type;
                for (int pi = 0; pi < 4; ++pi)
                    bfx.body_fx_params[pi] = (pi < info.n_params) ? info.params[pi].default_val : 0.5f;
                bfx.body_fx_amount = 1.f;

                state.tracks[ti].clips.push_back(bfx);

                // Auto-trigger mask generation if not already done
                if (vid_cl.bg_remove_mask_dir.empty() ||
                    vid_cl.bg_remove_status != BgRemoveStatus::Ready) {
                    bg_remove_start(state, ti, ci);
                }

                state.selected_clip = (int)state.tracks[ti].clips.size() - 1;
                history_push(state, std::string("Add Body FX: ") + info.name);
            }

            ImGui::Dummy({0.f, 4.f});
            ImGui::PopID();
        }
    }

    ImGui::Dummy({0.f, 16.f});
}

// ── Right panel: Effect tab ───────────────────────────────────────────────────

void panel_adjustment(AppState& state, float w) {
    if (state.selected_track < 0 || state.selected_track >= (int)state.tracks.size()) return;
    Track& track = state.tracks[state.selected_track];
    if (state.selected_clip < 0 || state.selected_clip >= (int)track.clips.size()) return;
    Clip& clip = track.clips[state.selected_clip];

    ImGui::Dummy({0.f, 8.f});

    bool is_glass = fx_clip_is_glass(state, state.selected_track, clip);
    ImGui::TextUnformatted("Adjustment");
    ImGui::SameLine(0.f, 8.f);
    ImGui::PushStyleColor(ImGuiCol_Text, is_glass
        ? IM_COL32(130, 210, 255, 255) : IM_COL32(160, 110, 255, 255));
    ImGui::TextUnformatted(is_glass ? "GLASS" : "GLOBAL");
    ImGui::PopStyleColor();

    char info[128];
    snprintf(info, sizeof(info), "%s  ·  %.2fs – %.2fs  ·  %s",
        track.name.c_str(), clip.start, clip.end,
        is_glass ? "clip-specific pre-composite" : "post-composite all tracks below");
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextWrapped("%s", info);
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 4.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

    ImGui::PushStyleColor(ImGuiCol_SliderGrab,      IM_COL32(180,130,255,255));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,IM_COL32(210,170,255,255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,         Col::bg_soft);
    ImGui::PushStyleColor(ImGuiCol_CheckMark,       IM_COL32(180,130,255,255));

    auto fx_reset_btn = [&](const char* id, float* v, float def) {
        ImGui::SameLine(0.f, 6.f);
        char lbl[16]; snprintf(lbl, sizeof(lbl), "R##%s", id);
        if (ui_btn(lbl, false, true)) { *v = def; history_push(state, "Effect: reset"); }
    };
    float sw = w - 72.f;  // slider width leaving room for label + reset

    bool show_color    = (clip.fx_type == FXType::Grade);
    bool show_blur     = (clip.fx_type == FXType::Blur);
    bool show_vignette = (clip.fx_type == FXType::Vignette);

    // ── Color Grade ───────────────────────────────────────────────────────────
    if (show_color) {
        bool cg = clip.fx_color_on;
        if (ImGui::Checkbox("Color Grade##fx", &cg)) {
            clip.fx_color_on = cg;
            history_push(state, "Effect: color grade");
        }
        if (clip.fx_color_on) {
            ImGui::Dummy({0.f, 4.f});
            ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Brightness##fx",&clip.fx_brightness,-1.f,1.f,"%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Effect: brightness");
            fx_reset_btn("br", &clip.fx_brightness, 0.f);

            ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Contrast##fx",  &clip.fx_contrast, 0.5f,2.f,"%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Effect: contrast");
            fx_reset_btn("co", &clip.fx_contrast, 1.f);

            ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Saturation##fx",&clip.fx_saturation,0.f,2.f,"%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Effect: saturation");
            fx_reset_btn("sa", &clip.fx_saturation, 1.f);

            ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Hue##fx",       &clip.fx_hue,-180.f,180.f,"%.0f\xc2\xb0");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Effect: hue");
            fx_reset_btn("hu", &clip.fx_hue, 0.f);
            ImGui::Dummy({0.f, 4.f});
        }
        ImGui::Dummy({0.f, 4.f}); ui_separator(); ImGui::Dummy({0.f, 4.f});
    }

    // ── Blur ──────────────────────────────────────────────────────────────────
    if (show_blur) {
        bool bl = clip.fx_blur_on;
        if (ImGui::Checkbox("Blur##fx", &bl)) {
            clip.fx_blur_on = bl;
            history_push(state, "Effect: blur");
        }
        if (clip.fx_blur_on) {
            ImGui::Dummy({0.f, 4.f});
            ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Radius##fx", &clip.fx_blur, 0.f, 20.f, "%.1f px");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Effect: blur radius");
            fx_reset_btn("bl", &clip.fx_blur, 0.f);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextUnformatted("Live preview shows badge — rendered on export");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 4.f});
        }
        ImGui::Dummy({0.f, 4.f}); ui_separator(); ImGui::Dummy({0.f, 4.f});
    }

    // ── Vignette ──────────────────────────────────────────────────────────────
    if (show_vignette) {
        bool vi = clip.fx_vignette_on;
        if (ImGui::Checkbox("Vignette##fx", &vi)) {
            clip.fx_vignette_on = vi;
            history_push(state, "Effect: vignette");
        }
        if (clip.fx_vignette_on) {
            ImGui::Dummy({0.f, 4.f});
            ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Strength##fx", &clip.fx_vignette, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Effect: vignette strength");
            fx_reset_btn("vi", &clip.fx_vignette, 0.f);
            ImGui::Dummy({0.f, 4.f});
        }
    }

    ImGui::Dummy({0.f, 4.f}); ui_separator(); ImGui::Dummy({0.f, 4.f});

    // ── Text Overrides ────────────────────────────────────────────────────────
    bool tx = clip.fx_text_on;
    if (ImGui::Checkbox("Text Overrides##fx", &tx)) {
        clip.fx_text_on = tx;
        history_push(state, "Effect: text overrides");
    }
    if (clip.fx_text_on) {
        ImGui::Dummy({0.f, 4.f});
        ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Opacity mul##fx",&clip.fx_opacity_mul,0.f,1.f,"%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Effect: opacity mul");
        fx_reset_btn("op", &clip.fx_opacity_mul, 1.f);

        ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Scale mul##fx",  &clip.fx_scale_mul,0.5f,2.f,"%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Effect: scale mul");
        fx_reset_btn("sc", &clip.fx_scale_mul, 1.f);
        ImGui::Dummy({0.f, 4.f});
    }

    ImGui::PopStyleColor(4);

    ImGui::Dummy({0.f, 8.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
    ImGui::TextWrapped("Adjustments stack — multiple Adjustment clips at the same time compound (brightness adds, contrast/saturation multiply, blur adds).");
    ImGui::PopStyleColor();

    ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

    // ── Save as Preset ────────────────────────────────────────────────────────
    static char s_preset_name[64] = {};
    static bool s_naming = false;
    if (!s_naming) {
        if (ui_btn("Save as new preset", false, true)) {
            s_naming = true;
            s_preset_name[0] = '\0';
        }
    } else {
        ImGui::SetNextItemWidth(w - 80.f);
        bool enter = ImGui::InputText("##pname", s_preset_name, sizeof(s_preset_name),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ui_btn("Save", true, true) || enter) {
            if (s_preset_name[0] != '\0') {
                state.user_presets.insert(state.user_presets.begin(), preset_from_clip(clip, s_preset_name));
                presets_save_user(state.user_presets);
            }
            s_naming = false;
        }
        ImGui::SameLine();
        if (ui_btn("X", false, true)) s_naming = false;
    }

    // ── Runtime FX controls ───────────────────────────────────────────────────
    if (!clip.runtime_fx_id.empty()) {
        ImGui::Dummy({0.f, 4.f}); ui_separator(); ImGui::Dummy({0.f, 4.f});
        const RuntimeFXDef* rdef = runtime_fx_find(clip.runtime_fx_id);
        if (rdef) {
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::Text("Custom FX: %s", rdef->name.c_str());
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 4.f});

            clip.runtime_fx_params.resize(rdef->params.size());
            float sw = w - 72.f;

            for (int pi = 0; pi < (int)rdef->params.size(); ++pi) {
                const RuntimeFXParam& p = rdef->params[pi];
                ImGui::PushID(30000 + pi);
                ImGui::SetNextItemWidth(sw);
                ImGui::SliderFloat(p.label.c_str(), &clip.runtime_fx_params[pi], p.min_val, p.max_val, "%.3f");
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Runtime FX: " + p.label);
                ImGui::SameLine(0.f, 6.f);
                char rl[24]; snprintf(rl, sizeof(rl), "R##rfx%d", pi);
                if (ui_btn(rl, false, true)) {
                    clip.runtime_fx_params[pi] = p.default_val;
                    history_push(state, "Runtime FX: reset " + p.label);
                }
                ImGui::PopID();
            }

            ImGui::SetNextItemWidth(sw);
            ImGui::SliderFloat("Amount##rfx", &clip.runtime_fx_amount, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Runtime FX: amount");

            ImGui::Dummy({0.f, 4.f});
            if (ui_btn("Remove custom FX", false, true)) {
                clip.runtime_fx_id.clear();
                clip.runtime_fx_params.clear();
                clip.runtime_fx_amount = 1.f;
                history_push(state, "Remove custom FX");
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200,80,80,255));
            ImGui::Text("Custom FX '%s' not found", clip.runtime_fx_id.c_str());
            ImGui::PopStyleColor();
        }
    }

    ImGui::Dummy({0.f, 4.f});
    if (ui_btn("Delete clip", false, true)) {
        track.clips.erase(track.clips.begin() + state.selected_clip);
        state.selected_clip = -1;
        history_push(state, "Delete effect clip");
    }
}


// ── Right panel: Background ───────────────────────────────────────────────────

void panel_background(AppState& state, float w, bool clip_only) {
    ImGui::Dummy({0.f, 8.f});

    // Resolve the selected Background clip (may be null if panel opened from BG tab without selection)
    Clip* bgclip = nullptr;
    if (state.selected_track >= 0 && state.selected_track < (int)state.tracks.size() &&
        state.selected_clip  >= 0 && state.selected_clip  < (int)state.tracks[state.selected_track].clips.size()) {
        Clip& c = state.tracks[state.selected_track].clips[state.selected_clip];
        if (c.clip_type == ClipType::Background) bgclip = &c;
    }

    if (!clip_only) {
        // ── Add Background brick button ──────────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
        ImGui::TextWrapped("Click to apply. Drag onto a timeline track.");
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 4.f});
        if (ImGui::Button("+ Add Background Track", {w, 0.f})) {
            // A background brick is just a generated-texture layer — place it
            // like any other content clip (empty track if free, else a new
            // track at the top, at the playhead) instead of push_back to the
            // bottom, which made every new background a full-composition
            // backdrop behind everything.
            float dur = 7.f;
            Clip c;
            c.clip_type = ClipType::Background;
            c.start = state.playhead; c.end = c.start + dur;
            c.text  = "blob";  // default preset
            int target = find_empty_track(state);
            if (target < 0) {
                Track t; t.name = "Background";
                state.tracks.insert(state.tracks.begin(), std::move(t));
                target = 0;
            }
            state.tracks[target].clips.push_back(std::move(c));
            state.selected_track = target;
            state.selected_clip  = (int)state.tracks[target].clips.size() - 1;
            bgclip = &state.tracks[target].clips[state.selected_clip];
            history_push(state, "Add background");
        }
        ImGui::Dummy({0.f, 8.f});
    }

    // ── Per-clip controls (only when a bg clip is selected) ───────────────────
    if (bgclip) {
        ImGui::PushStyleColor(ImGuiCol_Text, to_u32(Col::muted));
        ImGui::TextUnformatted("PRESET");
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 4.f});

        // Solid fill has no animation — speed/intensity would be dead sliders.
        if (bgclip->text != "solid") {
            ImGui::SetNextItemWidth(w);
            ImGui::SliderFloat("##bg_speed", &bgclip->bg_speed, 0.1f, 4.f, "Speed %.1fx");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "BG speed");
            ImGui::SetNextItemWidth(w);
            {
                float pct = bgclip->bg_intensity * 100.f;
                if (ImGui::SliderFloat("##bg_int", &pct, 0.f, 100.f, "Intensity %.0f%%"))
                    bgclip->bg_intensity = pct / 100.f;
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "BG intensity");
            }
        }
        ImGui::Dummy({0.f, 4.f});

        const BgPreset* active_pr = bgclip->text.empty() ? nullptr
                                    : bg_preset_by_id(bgclip->text.c_str());
        if (active_pr && active_pr->n_colors >= 1) {
            ImGui::TextUnformatted("Colors");
            ImGui::SameLine();
            if (ImGui::ColorEdit4("##bgc1", bgclip->bg_c1,
                ImGuiColorEditFlags_NoLabel|ImGuiColorEditFlags_NoInputs|ImGuiColorEditFlags_NoBorder))
                history_push(state, "BG color");
            if (active_pr->n_colors >= 2) {
                ImGui::SameLine();
                if (ImGui::ColorEdit4("##bgc2", bgclip->bg_c2,
                    ImGuiColorEditFlags_NoLabel|ImGuiColorEditFlags_NoInputs|ImGuiColorEditFlags_NoBorder))
                    history_push(state, "BG color");
            }
            if (active_pr->n_colors >= 3) {
                ImGui::SameLine();
                if (ImGui::ColorEdit4("##bgc3", bgclip->bg_c3,
                    ImGuiColorEditFlags_NoLabel|ImGuiColorEditFlags_NoInputs|ImGuiColorEditFlags_NoBorder))
                    history_push(state, "BG color");
            }
            // Palette fills all active gradient slots simultaneously
            {
                int ns = active_pr->n_colors;
                float* bg_slots[3] = { bgclip->bg_c1, bgclip->bg_c2, bgclip->bg_c3 };
                palette_widget("##pal_bg", bg_slots, ns, true);
            }
            ImGui::Dummy({0.f, 4.f});
        }
    }

    if (clip_only) return;

    // ── Preset grid ───────────────────────────────────────────────────────────
    const char* cur_cat = nullptr;
    float cell_w = (w - 8.f) * 0.5f;
    float cell_h = 80.f;
    int col_idx  = 0;
    float t_anim = (float)ImGui::GetTime();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (int pi = 0; pi < g_n_bg_presets; ++pi) {
        const BgPreset& pr = g_bg_presets[pi];

        if (!cur_cat || strcmp(cur_cat, pr.category) != 0) {
            if (col_idx == 1) { ImGui::NewLine(); col_idx = 0; }
            if (cur_cat) ImGui::Dummy({0.f, 6.f});
            cur_cat = pr.category;
            ImGui::PushStyleColor(ImGuiCol_Text, to_u32(Col::muted));
            ImGui::TextUnformatted(cur_cat);
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 2.f});
        }

        ImVec2 cp = ImGui::GetCursorScreenPos();
        bool selected = bgclip && (bgclip->text == pr.id);

        ImU32 card_bg = selected ? IM_COL32(40,40,60,255) : IM_COL32(22,22,28,255);
        dl->AddRectFilled(cp, {cp.x+cell_w, cp.y+cell_h}, card_bg, 4.f);
        if (selected) dl->AddRect(cp, {cp.x+cell_w, cp.y+cell_h}, IM_COL32(130,100,255,220), 4.f, 0, 1.5f);

        // Animated mini-preview
        float pad = 5.f;
        ImVec2 pp = {cp.x+pad, cp.y+pad};
        float pw2 = cell_w-pad*2, ph2 = cell_h*0.58f;
        dl->PushClipRect(pp, {pp.x+pw2, pp.y+ph2}, true);
        draw_bg_preset(pr.id, dl, pp, pw2, ph2, t_anim, pr.default_speed, 1.f,
            pr.dc1, pr.dc2, pr.dc3);
        dl->PopClipRect();

        float lx = cp.x+6.f, ly = cp.y+cell_h*0.62f;
        dl->AddText({lx,ly}, to_u32(Col::fg), pr.label);

        ImGui::SetCursorScreenPos(cp);
        ImGui::InvisibleButton(pr.id, {cell_w, cell_h});
        if (ImGui::IsItemClicked()) {
            if (!bgclip) {
                // No bg clip selected — create one, placed like any content
                // clip (empty track or new track on top, at the playhead).
                float dur = 7.f;
                Clip c; c.clip_type = ClipType::Background;
                c.start = state.playhead; c.end = c.start + dur;
                c.text = pr.id;
                memcpy(c.bg_c1, pr.dc1, sizeof(float)*4);
                memcpy(c.bg_c2, pr.dc2, sizeof(float)*4);
                memcpy(c.bg_c3, pr.dc3, sizeof(float)*4);
                int target = find_empty_track(state);
                if (target < 0) {
                    Track t; t.name = "Background";
                    state.tracks.insert(state.tracks.begin(), std::move(t));
                    target = 0;
                }
                state.tracks[target].clips.push_back(std::move(c));
                state.selected_track = target;
                state.selected_clip  = (int)state.tracks[target].clips.size()-1;
            } else {
                bgclip->text = pr.id;
                memcpy(bgclip->bg_c1, pr.dc1, sizeof(float)*4);
                memcpy(bgclip->bg_c2, pr.dc2, sizeof(float)*4);
                memcpy(bgclip->bg_c3, pr.dc3, sizeof(float)*4);
            }
            history_push(state, "Background preset");
        }
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload("BG_PRESET", pr.id, strlen(pr.id)+1);
            // Ghost chip
            ImDrawList* gdl = ImGui::GetWindowDrawList();
            ImVec2 gp = ImGui::GetCursorScreenPos();
            float gw = 140.f, gh = 36.f;
            gdl->AddRectFilled(gp, {gp.x+gw, gp.y+gh}, IM_COL32(90,15,105,230), 6.f);
            draw_bg_preset(pr.id, gdl, gp, gw, gh, (float)ImGui::GetTime(),
                           pr.default_speed, 0.6f, pr.dc1, pr.dc2, pr.dc3);
            gdl->AddRect(gp, {gp.x+gw, gp.y+gh}, IM_COL32(180,80,200,200), 6.f, 0, 1.2f);
            ImVec2 tsz = ImGui::CalcTextSize(pr.label);
            gdl->AddText({gp.x+(gw-tsz.x)*0.5f, gp.y+(gh-13.f)*0.5f},
                         IM_COL32(255,255,255,240), pr.label);
            ImGui::Dummy({gw, gh});
            ImGui::EndDragDropSource();
        }

        if (col_idx == 0) { ImGui::SameLine(0.f, 8.f); col_idx = 1; }
        else              { col_idx = 0; }
    }
    if (col_idx == 1) ImGui::NewLine();
    ImGui::Dummy({0.f, 12.f});
}

// ── Right panel: Creative FX library ─────────────────────────────────────────

void panel_fx_creative(AppState& state, float w) {
    ImGui::Dummy({0.f, 8.f});
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(180,240,255,255));
    ImGui::TextUnformatted("FX");
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
    ImGui::TextWrapped("Click to add at playhead. Drag onto a timeline track.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 8.f});

    float card_w  = w - 8.f;
    float card_h  = 96.f;
    float thumb_w = card_h * (108.f / 192.f);

    for (int i = 0; i < g_n_fx_cards; ++i) {
        if (fx_type_is_adjustment_style(g_fx_cards[i].type)) continue;
        const FXCard& fc = g_fx_cards[i];
        ImGui::PushID(i + 9000);
        ImVec2 cp = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        bool hov = ImGui::IsMouseHoveringRect(cp, {cp.x+card_w, cp.y+card_h});
        dl->AddRectFilled(cp, {cp.x+card_w, cp.y+card_h},
                          hov ? IM_COL32(28,28,40,255) : IM_COL32(18,18,28,255), 5.f);

        uintptr_t prev_tex = video_fx_preview_texture(fc.type, (float)ImGui::GetTime());
        if (prev_tex)
            dl->AddImageRounded((ImTextureID)(uintptr_t)prev_tex,
                                cp, {cp.x+thumb_w, cp.y+card_h},
                                {0,0},{1,1},
                                hov ? IM_COL32(255,255,255,230) : IM_COL32(255,255,255,190),
                                5.f, ImDrawFlags_RoundCornersLeft);

        dl->AddRect(cp, {cp.x+card_w, cp.y+card_h},
                    hov ? IM_COL32(255,255,255,200) : IM_COL32(60,60,80,200), 5.f, 0, hov ? 2.f : 1.f);

        float tx = cp.x + thumb_w + 10.f;
        ImGui::PushFont(g_font_bold);
        dl->AddText(ImGui::GetFont(), 13.f, {tx, cp.y+14.f}, IM_COL32(255,255,255,240), fc.name);
        ImGui::PopFont();
        dl->AddText({tx, cp.y+33.f}, IM_COL32(160,160,170,200), fc.tagline);

        if (hov) {
            const char* al = "+ Add";
            ImVec2 sz = ImGui::CalcTextSize(al);
            dl->AddText({cp.x+card_w-sz.x-10.f, cp.y+card_h-18.f}, IM_COL32(255,255,255,200), al);
        }

        ImGui::SetCursorScreenPos(cp);
        ImGui::InvisibleButton("##fxcard", {card_w, card_h});
        if (ImGui::IsItemClicked()) {
            Clip cl;
            cl.clip_type = ClipType::Effect;
            cl.fx_type   = fc.type;
            cl.start     = state.playhead;
            cl.end       = state.playhead + 5.f;
            // Set defaults for each adjustment FX type
            if (fc.type == FXType::Grade) {
                cl.fx_color_on   = true;
                cl.fx_brightness = 0.f;
                cl.fx_contrast   = 1.f;
                cl.fx_saturation = 1.f;
                cl.fx_hue        = 0.f;
            } else if (fc.type == FXType::Blur) {
                cl.fx_blur_on = true;
                cl.fx_blur    = 4.f;
            } else if (fc.type == FXType::Vignette) {
                cl.fx_vignette_on = true;
                cl.fx_vignette    = 0.5f;
            }
            Track nt; nt.name = fc.name;
            nt.clips.push_back(cl);
            state.tracks.insert(state.tracks.begin(), std::move(nt));
            state.selected_track = 0;
            state.selected_clip  = 0;
            history_push(state, std::string("Add FX: ") + fc.name);
        }
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            int ft = (int)fc.type;
            ImGui::SetDragDropPayload("FX_CREATIVE", &ft, sizeof(int));
            ImGui::Text("%s", fc.name);
            ImGui::TextDisabled("Drop onto timeline track");
            ImGui::EndDragDropSource();
        }

        ImGui::Dummy({0.f, 5.f});
        ImGui::PopID();
    }
}

// ── Audio FX brick catalogue ──────────────────────────────────────────────────

struct AudioFXCard {
    FXType      type;
    const char* name;
    const char* tagline;
    ImU32       accent;
};

static const AudioFXCard g_audio_fx_cards[] = {
    { FXType::AudioAutotune,     "Autotune",       "YIN pitch detection  ·  scale snap  ·  grain shift",      IM_COL32(30,220,180,255) },
    { FXType::AudioPitch,        "Pitch Shift",    "Grain pitch shift  ·  -24 to +24 semitones",              IM_COL32(30,180,140,255) },
    { FXType::AudioFormant,      "Formant",        "Voice character shift  ·  chipmunk / deep",               IM_COL32(40,200,160,255) },
    { FXType::AudioDelay,        "Delay",          "Stereo delay  ·  feedback  ·  wet/dry",                   IM_COL32(20,160,120,255) },
    { FXType::AudioReverb,       "Reverb",         "Freeverb room reverb  ·  damp  ·  wet/dry",               IM_COL32(20,140,110,255) },
    { FXType::AudioVoiceConvert, "Voice Convert",  "RVC voice conversion  ·  load .pth model",                IM_COL32(50,220,150,255) },
};
static const int g_n_audio_fx_cards = (int)(sizeof(g_audio_fx_cards) / sizeof(g_audio_fx_cards[0]));

void panel_fx_audio(AppState& state, float w) {
    ImGui::Dummy({0.f, 8.f});
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(30,220,180,255));
    ImGui::TextUnformatted("Audio FX");
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
    ImGui::TextWrapped("Click to add to selected audio track.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 8.f});

    float card_w = w - 8.f;
    float card_h = 72.f;

    for (int i = 0; i < g_n_audio_fx_cards; ++i) {
        const AudioFXCard& fc = g_audio_fx_cards[i];
        ImGui::PushID(i + 20000);
        ImVec2 cp = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        bool hov = ImGui::IsMouseHoveringRect(cp, {cp.x+card_w, cp.y+card_h});
        dl->AddRectFilled(cp, {cp.x+card_w, cp.y+card_h},
                          hov ? IM_COL32(18,38,34,255) : IM_COL32(12,26,22,255), 5.f);

        // Accent bar on left
        dl->AddRectFilled(cp, {cp.x+3.f, cp.y+card_h}, fc.accent, 5.f);

        dl->AddRect(cp, {cp.x+card_w, cp.y+card_h},
                    hov ? IM_COL32(30,220,180,160) : IM_COL32(30,100,80,160), 5.f, 0, hov ? 2.f : 1.f);

        float tx = cp.x + 14.f;
        ImGui::PushFont(g_font_bold);
        dl->AddText(ImGui::GetFont(), 13.f, {tx, cp.y+12.f}, IM_COL32(255,255,255,240), fc.name);
        ImGui::PopFont();
        dl->AddText({tx, cp.y+30.f}, IM_COL32(130,200,180,200), fc.tagline);

        if (hov) {
            const char* al = "+ Add";
            ImVec2 sz = ImGui::CalcTextSize(al);
            dl->AddText({cp.x+card_w-sz.x-10.f, cp.y+card_h-16.f}, IM_COL32(30,220,180,220), al);
        }

        ImGui::SetCursorScreenPos(cp);
        ImGui::InvisibleButton("##afxcard", {card_w, card_h});
        if (ImGui::IsItemClicked()) {
            // Find selected audio track, or first audio track, or create one
            int target_ti = -1;
            if (state.selected_track >= 0 && state.selected_track < (int)state.tracks.size()) {
                auto& tr = state.tracks[state.selected_track];
                for (auto& cl : tr.clips)
                    if (cl.clip_type == ClipType::Audio) { target_ti = state.selected_track; break; }
            }
            if (target_ti < 0) {
                for (int ti = 0; ti < (int)state.tracks.size(); ++ti)
                    for (auto& cl : state.tracks[ti].clips)
                        if (cl.clip_type == ClipType::Audio) { target_ti = ti; break; }
            }
            if (target_ti < 0) {
                Track nt; nt.name = fc.name;
                state.tracks.push_back(std::move(nt));
                target_ti = (int)state.tracks.size() - 1;
            }

            Clip cl;
            cl.clip_type = ClipType::Effect;
            cl.fx_type   = fc.type;
            cl.start     = state.playhead;
            cl.end       = state.playhead + 5.f;
            // Set sensible defaults
            cl.audio_fx.autotune_on      = (fc.type == FXType::AudioAutotune);
            cl.audio_fx.pitch_on         = (fc.type == FXType::AudioPitch);
            cl.audio_fx.formant_on       = (fc.type == FXType::AudioFormant);
            cl.audio_fx.delay_on         = (fc.type == FXType::AudioDelay);
            cl.audio_fx.reverb_on        = (fc.type == FXType::AudioReverb);
            cl.audio_fx.voice_convert_on = (fc.type == FXType::AudioVoiceConvert);

            state.tracks[target_ti].clips.push_back(cl);
            state.selected_track = target_ti;
            state.selected_clip  = (int)state.tracks[target_ti].clips.size() - 1;
            history_push(state, std::string("Add audio FX: ") + fc.name);
        }

        ImGui::Dummy({0.f, 5.f});
        ImGui::PopID();
    }

    ImGui::Dummy({0.f, 8.f});
    if (ui_btn("Browse Voices…", false, true)) {
        extern PanelView s_panel_view;
        s_panel_view = PanelView::LibAFX;
    }
}

// ── Voice model data (used by panel_audio_fx_clip's VC inspector) ────────────

struct PinnedVoice { const char* label; const char* repo; const char* file; };
static const PinnedVoice k_pinned[] = {
    { "Drake",        "binant/Drake_RVC",                                        "model.pth"       },
    { "Travis Scott", "binant/Travis_Scott_-_RVC_-_1000_Epoch_48k",              "model.pth"       },
    { "The Weeknd",   "binant/The_Weeknd__RVC__1000_Epochs",                     "model.pth"       },
    { "Playboi Carti","Shadow-AI/Playboi_Carti_Deep_Voice_300_Epochs_RVC_V2",    "PBCDeepVoice.zip"},
};

static std::map<std::string, HFDownload> s_dl;

static std::string dl_key(const std::string& repo, const std::string& file) {
    return repo + "::" + file;
}

// ── Right panel: Creative FX clip controls ────────────────────────────────────

void panel_audio_fx_clip(AppState& state, float w) {
    if (state.selected_track < 0 || state.selected_track >= (int)state.tracks.size()) return;
    Track& track = state.tracks[state.selected_track];
    if (state.selected_clip < 0 || state.selected_clip >= (int)track.clips.size()) return;
    Clip& clip = track.clips[state.selected_clip];
    AudioFX& afx = clip.audio_fx;

    float bar_w = w - 16.f;
    ImGui::Dummy({0.f, 8.f});
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(30,220,180,255));
    ImGui::TextUnformatted(fx_type_display(clip.fx_type));
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
    char info[128];
    snprintf(info, sizeof(info), "%s  ·  %.2fs – %.2fs", track.name.c_str(), clip.start, clip.end);
    ImGui::TextUnformatted(info);
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 8.f});
    ui_separator();
    ImGui::Dummy({0.f, 8.f});

    auto pct_slider = [&](const char* id, const char* lbl, float* v, float mn, float mx, const char* fmt) {
        ImGui::SetNextItemWidth(bar_w);
        float pct = *v * 100.f;
        if (ImGui::SliderFloat(id, &pct, mn*100.f, mx*100.f, fmt))
            *v = pct / 100.f;
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, lbl);
    };

    switch (clip.fx_type) {
        case FXType::AudioAutotune: {
            static const char* keys[]   = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
            static const char* scales[] = {"Major","Minor","Chromatic"};
            ImGui::SetNextItemWidth(60.f);
            if (ImGui::Combo("Key##at_k", &afx.autotune_key, keys, 12))
                history_push(state, "Autotune key");
            ImGui::SameLine(0.f, 8.f);
            ImGui::SetNextItemWidth(90.f);
            if (ImGui::Combo("Scale##at_sc", &afx.autotune_scale, scales, 3))
                history_push(state, "Autotune scale");
            ImGui::SetNextItemWidth(bar_w);
            ImGui::SliderFloat("##at_sp", &afx.autotune_speed, 0.f, 200.f, "Snap speed %.0f ms");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Autotune speed");
            ImGui::Dummy({0.f, 6.f});
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextWrapped("Voice presets:");
            ImGui::PopStyleColor();
            for (int pi = 0; pi < k_voice_preset_count; ++pi) {
                const VoicePresetDef& pd = k_voice_presets[pi];
                if (ui_btn(pd.name, false, true)) {
                    afx = pd.fx; history_push(state, "Voice preset");
                }
                if (pi < k_voice_preset_count-1) ImGui::SameLine(0.f, 4.f);
            }
            break;
        }
        case FXType::AudioPitch: {
            ImGui::SetNextItemWidth(bar_w);
            ImGui::SliderFloat("##pt_st", &afx.pitch_semitones, -24.f, 24.f, "%.0f semitones");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Pitch semitones");
            break;
        }
        case FXType::AudioFormant: {
            ImGui::SetNextItemWidth(bar_w);
            ImGui::SliderFloat("##fm_sh", &afx.formant_shift, -1.f, 1.f, "Shift %.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Formant shift");
            ImGui::Dummy({0.f, 6.f});
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextWrapped("Voice presets:");
            ImGui::PopStyleColor();
            for (int pi = 0; pi < k_voice_preset_count; ++pi) {
                const VoicePresetDef& pd = k_voice_presets[pi];
                if (ui_btn(pd.name, false, true)) {
                    afx = pd.fx; history_push(state, "Voice preset");
                }
                if (pi < k_voice_preset_count-1) ImGui::SameLine(0.f, 4.f);
            }
            break;
        }
        case FXType::AudioDelay: {
            ImGui::SetNextItemWidth(bar_w);
            ImGui::SliderFloat("##dl_t", &afx.delay_time, 0.01f, 2.f, "Time %.2f s");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Delay time");
            pct_slider("##dl_fb", "Delay feedback", &afx.delay_feedback, 0.f, 0.95f, "Feedback %.0f%%");
            pct_slider("##dl_mx", "Delay mix",      &afx.delay_mix,      0.f, 1.f,   "Mix %.0f%%");
            break;
        }
        case FXType::AudioReverb: {
            pct_slider("##rv_rm", "Reverb room", &afx.reverb_room, 0.f, 1.f, "Room %.0f%%");
            pct_slider("##rv_dp", "Reverb damp", &afx.reverb_damp, 0.f, 1.f, "Damp %.0f%%");
            pct_slider("##rv_mx", "Reverb mix",  &afx.reverb_mix,  0.f, 1.f, "Mix %.0f%%");
            break;
        }
        case FXType::AudioVoiceConvert: {
            // ── Model picker ──────────────────────────────────────────────────
            if (!afx.voice_model_path.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(30,220,150,255));
                ImGui::TextUnformatted(fs::path(afx.voice_model_path).filename().string().c_str());
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                ImGui::TextUnformatted("No model loaded");
                ImGui::PopStyleColor();
            }
            ImGui::SameLine(0.f, 8.f);
            if (ui_btn("Browse…##vc_fp", false, true)) {
                std::string p = filepicker_open("Voice model", "Model", "*.pth *.onnx");
                if (!p.empty()) { afx.voice_model_path = p; afx.voice_convert_on = true;
                                  history_push(state, "Voice model"); }
            }
            ImGui::Dummy({0.f, 6.f});

            // ── Find overlapping audio clips on this track ────────────────────
            // We need them to show conversion state and to trigger vc_start.
            int ti = state.selected_track;
            struct AudioRef { int ci; };
            std::vector<AudioRef> audio_refs;
            for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
                const Clip& ac = track.clips[ci];
                if (ac.clip_type != ClipType::Audio || ac.text.empty()) continue;
                if (ac.end <= clip.start || ac.start >= clip.end) continue;
                audio_refs.push_back({ci});
            }

            // ── Conversion job progress / controls ────────────────────────────
            if (!afx.voice_model_path.empty() && !audio_refs.empty()) {
                // Aggregate status: Processing if any clip is processing
                VcStatus agg = VcStatus::Idle;
                float    agg_prog = 0.f;
                std::string agg_err;
                for (auto& ar : audio_refs) {
                    const Clip& ac = track.clips[ar.ci];
                    if (ac.vc_status == VcStatus::Processing) { agg = VcStatus::Processing; agg_prog = ac.vc_progress; }
                    else if (ac.vc_status == VcStatus::Error && agg != VcStatus::Processing) { agg = VcStatus::Error; agg_err = ac.vc_error; }
                    else if (ac.vc_status == VcStatus::Ready  && agg == VcStatus::Idle)       agg = VcStatus::Ready;
                }

                ImVec2 bp = ImGui::GetCursorScreenPos();
                ImDrawList* dl = ImGui::GetWindowDrawList();

                if (agg == VcStatus::Processing) {
                    ImU32 green     = IM_COL32(30, 200, 140, 255);
                    ImU32 green_dim = IM_COL32(30, 200, 140, 50);
                    float fill = fmaxf(0.01f, fminf(1.f, agg_prog));
                    dl->AddRectFilled(bp, {bp.x+bar_w, bp.y+6.f}, green_dim, 3.f);
                    dl->AddRectFilled(bp, {bp.x+bar_w*fill, bp.y+6.f}, green, 3.f);
                    ImGui::Dummy({0.f, 10.f});
                    char pct[64];
                    snprintf(pct, sizeof(pct), "Converting voice…  %d%%", (int)(fill * 100.f));
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(30,220,150,255));
                    ImGui::TextUnformatted(pct);
                    ImGui::PopStyleColor();
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                    ImGui::TextWrapped("Running RVC. This may take a minute.");
                    ImGui::PopStyleColor();
                } else if (agg == VcStatus::Ready) {
                    dl->AddRectFilled(bp, {bp.x+bar_w, bp.y+6.f}, IM_COL32(30, 200, 80, 255), 3.f);
                    ImGui::Dummy({0.f, 10.f});
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(40,220,100,255));
                    ImGui::TextUnformatted("Voice converted");
                    ImGui::PopStyleColor();
                    ImGui::Dummy({0.f, 4.f});
                    if (ui_btn("Re-convert##vcr", false, true)) {
                        for (auto& ar : audio_refs)
                            vc_start(state, ti, ar.ci, afx.voice_model_path);
                    }
                } else if (agg == VcStatus::Error) {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(220,80,80,255));
                    ImGui::TextWrapped("Error: %s", agg_err.c_str());
                    ImGui::PopStyleColor();
                    ImGui::Dummy({0.f, 4.f});
                    if (ui_btn("Retry##vcretry", false, true)) {
                        for (auto& ar : audio_refs)
                            vc_start(state, ti, ar.ci, afx.voice_model_path);
                    }
                } else {
                    // Idle — show Convert button
                    if (ui_btn("Convert##vc_go", false, false)) {
                        for (auto& ar : audio_refs)
                            vc_start(state, ti, ar.ci, afx.voice_model_path);
                    }
                }
            } else if (afx.voice_model_path.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                ImGui::TextUnformatted("Load a model below, then hit Convert.");
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                ImGui::TextWrapped("No audio clips overlap this brick on the timeline.");
                ImGui::PopStyleColor();
            }

            ImGui::Dummy({0.f, 10.f});
            ui_separator();
            ImGui::Dummy({0.f, 8.f});

            // ── HuggingFace model browser ─────────────────────────────────────
            static HFSearch s_vc_search;
            static char     s_vc_query[64] = {};
            static char     s_vc_prev[64]  = {};
            static float    s_vc_debounce  = 0.f;

            ImGui::SetNextItemWidth(w - 8.f);
            if (ImGui::InputTextWithHint("##vcq", "Search HuggingFace RVC…",
                                         s_vc_query, sizeof(s_vc_query))) {
                if (strcmp(s_vc_query, s_vc_prev) != 0) {
                    memcpy(s_vc_prev, s_vc_query, sizeof(s_vc_query));
                    s_vc_debounce = s_vc_query[0] ? 0.8f : 0.f;
                    if (!s_vc_query[0]) hf_search_cancel(s_vc_search);
                }
            }
            if (s_vc_debounce > 0.f) {
                s_vc_debounce -= ImGui::GetIO().DeltaTime;
                if (s_vc_debounce <= 0.f) {
                    hf_search_cancel(s_vc_search);
                    hf_search(s_vc_query, s_vc_search);
                    s_vc_debounce = 0.f;
                }
            }

            auto ss = s_vc_search.status.load(std::memory_order_acquire);

            // Card: Download + Use (sets model path; user then hits Convert above)
            auto draw_vc_card = [&](int id, const char* lbl,
                                    const char* repo, const char* file) {
                std::string key = dl_key(repo, file);
                HFDownload& hfdl = s_dl[key];
                hf_download_poll(hfdl);
                // Download completed → set model path
                if (hfdl.status.load(std::memory_order_acquire) == HFDownload::Status::Done) {
                    afx.voice_model_path = hfdl.out_path;
                    afx.voice_convert_on = true;
                    history_push(state, std::string("Voice: ") + lbl);
                    hfdl.status.store(HFDownload::Status::Idle, std::memory_order_release);
                }
                bool installed = hf_rvc_installed(repo, file);
                ImGui::PushID(id);
                ImVec2 cp = ImGui::GetCursorScreenPos();
                float cw = w - 8.f, ch = 44.f;
                ImDrawList* cdl = ImGui::GetWindowDrawList();
                bool hov = ImGui::IsMouseHoveringRect(cp, {cp.x+cw, cp.y+ch});
                cdl->AddRectFilled(cp, {cp.x+cw, cp.y+ch},
                    hov ? IM_COL32(20,36,32,255) : IM_COL32(14,24,20,255), 4.f);
                cdl->AddRect(cp, {cp.x+cw, cp.y+ch},
                    installed ? IM_COL32(30,200,150,180) : IM_COL32(40,60,50,160), 4.f, 0, 1.f);
                ImGui::PushFont(g_font_bold);
                cdl->AddText(ImGui::GetFont(), 11.f, {cp.x+8.f, cp.y+8.f},
                             IM_COL32(255,255,255,220), lbl);
                ImGui::PopFont();

                auto dst = hfdl.status.load(std::memory_order_acquire);
                if (dst == HFDownload::Status::Running) {
                    float prog = hfdl.progress();
                    float bx0 = cp.x+cw-94.f, bx1 = cp.x+cw-8.f, by = cp.y+18.f;
                    cdl->AddRectFilled({bx0,by},{bx1,by+5.f},IM_COL32(20,60,45,255),2.f);
                    cdl->AddRectFilled({bx0,by},{bx0+(bx1-bx0)*prog,by+5.f},IM_COL32(30,200,150,255),2.f);
                    cdl->AddText({bx0, by+8.f}, IM_COL32(80,180,140,180), "Downloading…");
                } else if (installed) {
                    bool active = (afx.voice_model_path == hf_rvc_model_path(repo, file));
                    cdl->AddText({cp.x+8.f, cp.y+28.f},
                                 active ? IM_COL32(30,220,150,255) : IM_COL32(60,140,100,180),
                                 active ? "Selected" : "Installed");
                    ImGui::SetCursorScreenPos({cp.x+cw-44.f, cp.y+8.f});
                    if (ImGui::SmallButton("Use##vu")) {
                        afx.voice_model_path = hf_rvc_model_path(repo, file);
                        afx.voice_convert_on = true;
                        history_push(state, std::string("Voice: ") + lbl);
                    }
                } else {
                    ImGui::SetCursorScreenPos({cp.x+cw-70.f, cp.y+ch/2.f-8.f});
                    if (ImGui::SmallButton("Download##vd"))
                        hf_download_model(repo, file, hf_rvc_model_path(repo, file), hfdl);
                    if (dst == HFDownload::Status::Error) {
                        cdl->AddText({cp.x+8.f, cp.y+28.f}, IM_COL32(220,80,80,200), "Failed");
                        if (ImGui::IsItemHovered() && !hfdl.error_msg.empty())
                            ImGui::SetTooltip("%s", hfdl.error_msg.c_str());
                    }
                }
                ImGui::SetCursorScreenPos(cp);
                ImGui::InvisibleButton("##vcc", {cw-76.f, ch});
                ImGui::Dummy({0.f, ch + 3.f});
                ImGui::PopID();
            };

            if (!s_vc_query[0]) {
                ImGui::Dummy({0.f, 2.f});
                for (int i = 0; i < 4; ++i)
                    draw_vc_card(60000+i, k_pinned[i].label,
                                 k_pinned[i].repo, k_pinned[i].file);
            } else if (ss == HFSearch::Status::Running || s_vc_debounce > 0.f) {
                ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                ImGui::TextUnformatted("Searching…");
                ImGui::PopStyleColor();
            } else if (ss == HFSearch::Status::Error) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(220,80,80,255));
                ImGui::TextWrapped("%s", s_vc_search.error.c_str());
                ImGui::PopStyleColor();
            } else if (ss == HFSearch::Status::Done) {
                if (s_vc_search.results.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                    ImGui::TextUnformatted("No models found.");
                    ImGui::PopStyleColor();
                }
                for (int i = 0; i < (int)s_vc_search.results.size(); ++i) {
                    const HFModel& m = s_vc_search.results[i];
                    std::string disp = m.repo;
                    auto sl = disp.rfind('/');
                    if (sl != std::string::npos) disp = disp.substr(sl+1);
                    for (char& c : disp) if (c=='_'||c=='-') c=' ';
                    draw_vc_card(61000+i, disp.c_str(),
                                 m.repo.c_str(), m.model_file.c_str());
                }
            }
            break;
        }
        default: break;
    }
}


void panel_fx_clip(AppState& state, float w) {
    if (state.selected_track < 0 || state.selected_track >= (int)state.tracks.size()) return;
    Track& track = state.tracks[state.selected_track];
    if (state.selected_clip < 0 || state.selected_clip >= (int)track.clips.size()) return;
    Clip& clip = track.clips[state.selected_clip];

    ImGui::Dummy({0.f, 8.f});

    ImU32 ac = fx_type_accent(clip.fx_type);
    bool is_glass = fx_clip_is_glass(state, state.selected_track, clip);
    ImGui::TextUnformatted(fx_type_display(clip.fx_type));
    ImGui::SameLine(0.f, 8.f);
    ImGui::PushStyleColor(ImGuiCol_Text, is_glass
        ? IM_COL32(130, 210, 255, 255) : IM_COL32(160, 110, 255, 255));
    ImGui::TextUnformatted(is_glass ? "GLASS" : "GLOBAL");
    ImGui::PopStyleColor();

    char info[128];
    snprintf(info, sizeof(info), "%s  ·  %.2fs – %.2fs  ·  %s",
        track.name.c_str(), clip.start, clip.end,
        is_glass ? "clip-specific pre-composite" : "post-composite all tracks below");
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextWrapped("%s", info);
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 4.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

    ImGui::PushStyleColor(ImGuiCol_SliderGrab,       ac);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ac);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,          Col::bg_soft);
    float sw = w - 16.f;

    switch (clip.fx_type) {
        case FXType::Glitch:
            ui_label("Chroma Shift");
            ImGui::SetNextItemWidth(sw);
            ImGui::SliderFloat("##gchroma", &clip.fx_glitch_chroma, 0.f, 30.f, "%.1f px");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Glitch: chroma shift");
            ImGui::Dummy({0.f, 4.f});
            ui_label("Row Jitter");
            ImGui::SetNextItemWidth(sw);
            ImGui::SliderFloat("##gjitter", &clip.fx_glitch_jitter, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Glitch: row jitter");
            ImGui::Dummy({0.f, 4.f});
            ui_label("Block Corruption");
            ImGui::SetNextItemWidth(sw);
            ImGui::SliderFloat("##gcorrupt", &clip.fx_glitch_corruption, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Glitch: block corruption");
            ImGui::Dummy({0.f, 4.f});
            ui_label("Layer Bleed");
            ImGui::SetNextItemWidth(sw);
            ImGui::SliderFloat("##gbleed", &clip.fx_glitch_corruption_bleed, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Glitch: layer bleed");
            break;

        case FXType::ZoomPunch:
            ui_label("Punch Strength");
            ImGui::SetNextItemWidth(sw);
            ImGui::SliderFloat("##zstr", &clip.fx_zoom_strength, 0.f, 0.5f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "ZoomPunch: strength");
            ImGui::Dummy({0.f, 4.f});
            ui_label("Decay");
            ImGui::SetNextItemWidth(sw);
            ImGui::SliderFloat("##zdec", &clip.fx_zoom_decay, 0.05f, 0.5f, "%.2fs");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "ZoomPunch: decay");
            ImGui::Dummy({0.f, 4.f});
            ui_label("Shake");
            ImGui::SetNextItemWidth(sw);
            ImGui::SliderFloat("##zshk", &clip.fx_zoom_shake, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "ZoomPunch: shake");
            if (state.beats.empty()) {
                ImGui::Dummy({0.f, 6.f});
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                ImGui::TextWrapped("No beats loaded — run Beat Sync first for music-driven punches.");
                ImGui::PopStyleColor();
            }
            break;

        case FXType::LUT:
            ui_label("LUT File (.cube)");
            ImGui::PushStyleColor(ImGuiCol_Text, clip.fx_lut_path.empty() ? Col::muted : Col::fg);
            ImGui::TextWrapped("%s", clip.fx_lut_path.empty() ? "(none)" : clip.fx_lut_path.c_str());
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 4.f});
            if (ui_btn("Browse…", false, true)) {
                std::string p = filepicker_open("LUT file", "CUBE file", "*.cube");
                if (!p.empty()) { clip.fx_lut_path = p; history_push(state, "LUT: set file"); }
            }
            ImGui::Dummy({0.f, 8.f});
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextWrapped("Standard 3D .cube LUTs — thousands of free packs available online.");
            ImGui::PopStyleColor();
            break;

        case FXType::LightLeak:
            ui_label("Intensity");
            ImGui::SetNextItemWidth(sw);
            ImGui::SliderFloat("##lint", &clip.fx_leak_intensity, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "LightLeak: intensity");
            ImGui::Dummy({0.f, 4.f});
            ui_label("Speed");
            ImGui::SetNextItemWidth(sw);
            ImGui::SliderFloat("##lspd", &clip.fx_leak_speed, 0.f, 4.f, "%.2f\xc3\x97");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "LightLeak: speed");
            if (state.amplitude_envelope.empty()) {
                ImGui::Dummy({0.f, 6.f});
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                ImGui::TextWrapped("No amplitude data — run Analyse Amplitude for music-driven intensity.");
                ImGui::PopStyleColor();
            }
            break;

        case FXType::VHS:
            ui_label("Noise");
            ImGui::SetNextItemWidth(sw);
            ImGui::SliderFloat("##vnoi", &clip.fx_vhs_noise, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "VHS: noise");
            ImGui::Dummy({0.f, 4.f});
            ui_label("Chroma Bleed");
            ImGui::SetNextItemWidth(sw);
            ImGui::SliderFloat("##vble", &clip.fx_vhs_bleed, 0.f, 20.f, "%.1f px");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "VHS: chroma bleed");
            ImGui::Dummy({0.f, 4.f});
            ui_label("Tracking Glitch");
            ImGui::SetNextItemWidth(sw);
            ImGui::SliderFloat("##vtrk", &clip.fx_vhs_tracking, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "VHS: tracking");
            break;

        case FXType::Datamosh: {
            float sw2 = w - 16.f;
            ui_label("Intensity");
            ImGui::SetNextItemWidth(sw2);
            ImGui::SliderFloat("##dmint", &clip.fx_datamosh_intensity, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Datamosh: intensity");
            ImGui::Dummy({0.f, 4.f});
            ui_label("Spread");
            ImGui::SetNextItemWidth(sw2);
            ImGui::SliderFloat("##dmspread", &clip.fx_datamosh_spread, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Datamosh: spread");
            break;
        }

        case FXType::ChromaKey: {
            float sw2 = w - 16.f;
            ui_label("Key Color");
            float col3[3] = { clip.fx_chroma_key_r, clip.fx_chroma_key_g, clip.fx_chroma_key_b };
            ImGui::SetNextItemWidth(sw2);
            if (ImGui::ColorEdit3("##ckbcol", col3, ImGuiColorEditFlags_NoInputs |
                                                     ImGuiColorEditFlags_PickerHueWheel)) {
                clip.fx_chroma_key_r = col3[0];
                clip.fx_chroma_key_g = col3[1];
                clip.fx_chroma_key_b = col3[2];
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Chroma Key: color");
            palette_widget("##pal_ck", col3);
            if (col3[0] != clip.fx_chroma_key_r || col3[1] != clip.fx_chroma_key_g || col3[2] != clip.fx_chroma_key_b) {
                clip.fx_chroma_key_r = col3[0]; clip.fx_chroma_key_g = col3[1]; clip.fx_chroma_key_b = col3[2];
                history_push(state, "Chroma Key: color");
            }
            ImGui::Dummy({0.f, 4.f});
            ui_label("Threshold");
            ImGui::SetNextItemWidth(sw2);
            ImGui::SliderFloat("##ckbthresh", &clip.fx_chroma_key_threshold, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Chroma Key: threshold");
            ImGui::Dummy({0.f, 4.f});
            ui_label("Softness");
            ImGui::SetNextItemWidth(sw2);
            ImGui::SliderFloat("##ckbsoft", &clip.fx_chroma_key_softness, 0.f, 0.5f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Chroma Key: softness");
            break;
        }

#include "generated/fx_ui_inspector.h"

        default: break;
    }

    ImGui::PopStyleColor(3);

    // ── Beat Sync Source ────────────────────────────────────────────────────
    ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 210, 60, 255));
    ImGui::TextUnformatted("Beat Sync Source");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 4.f});

    // Build list of analyzed Audio/Video clips
    struct BeatSrcEntry { int ti, ci; std::string label; };
    std::vector<BeatSrcEntry> beat_srcs;
    beat_srcs.push_back({-1, -1, "None"});
    for (int ti2 = 0; ti2 < (int)state.tracks.size(); ++ti2) {
        const auto& tr2 = state.tracks[ti2];
        for (int ci2 = 0; ci2 < (int)tr2.clips.size(); ++ci2) {
            const Clip& c2 = tr2.clips[ci2];
            if (c2.clip_type != ClipType::Audio && c2.clip_type != ClipType::Video) continue;
            char lbl[128];
            if (!c2.beats.empty())
                snprintf(lbl, sizeof(lbl), "%s  ·  %d beats @ %.1f BPM",
                    fs::path(c2.source_id).filename().string().c_str(), (int)c2.beats.size(), c2.beat_bpm);
            else if (c2.beats_analyzing)
                snprintf(lbl, sizeof(lbl), "%s  ·  analysing…",
                    fs::path(c2.source_id).filename().string().c_str());
            else
                snprintf(lbl, sizeof(lbl), "%s  ·  no beats yet",
                    fs::path(c2.source_id).filename().string().c_str());
            beat_srcs.push_back({ti2, ci2, lbl});
        }
    }

    // Find current selection index
    int sel_idx = 0;
    for (int i = 1; i < (int)beat_srcs.size(); ++i)
        if (beat_srcs[i].ti == clip.beat_src_track && beat_srcs[i].ci == clip.beat_src_clip)
            { sel_idx = i; break; }

    const char* preview = beat_srcs[sel_idx].label.c_str();
    ImGui::SetNextItemWidth(w - 16.f);
    if (ImGui::BeginCombo("##bsrc", preview)) {
        for (int i = 0; i < (int)beat_srcs.size(); ++i) {
            bool selected = (i == sel_idx);
            if (ImGui::Selectable(beat_srcs[i].label.c_str(), selected)) {
                clip.beat_src_track = beat_srcs[i].ti;
                clip.beat_src_clip  = beat_srcs[i].ci;
                history_push(state, "FX: set beat sync source");
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    if (clip.beat_src_track >= 0) {
        ImGui::Dummy({0.f, 4.f});
        ui_label("Beat Decay");
        ImGui::SetNextItemWidth(w - 16.f);
        ImGui::SliderFloat("##bdecay", &clip.beat_decay, 0.02f, 1.0f, "%.2fs");
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "FX: beat decay");
    }

    ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
    if (track.locked) ImGui::BeginDisabled();
    if (ui_btn("Delete clip", false, true)) {
        if (track.locked) ImGui::EndDisabled();
        track.clips.erase(track.clips.begin() + state.selected_clip);
        state.selected_clip = -1;
        history_push(state, "Delete FX clip");
        return;
    }
    if (track.locked) ImGui::EndDisabled();
}

// ── Shader FX catalogue for MultiFX picker ───────────────────────────────────
struct ShaderFXCard { FXType type; const char* name; const char* category; };
static const ShaderFXCard k_shader_fx[] = {
    // Glitch & Distortion
    { FXType::Pixelate,            "Pixelate",           "Glitch" },
    { FXType::GlitchBlock,         "Glitch Block",       "Glitch" },
    { FXType::InterlaceGlitch,     "Interlace Glitch",   "Glitch" },
    { FXType::DataCorrupt,         "Data Corrupt",       "Glitch" },
    { FXType::DoubleGhost,         "Double Ghost",       "Glitch" },
    { FXType::RgbSplitWave,        "RGB Split Wave",     "Glitch" },
    { FXType::BitCrush,            "Bit Crush",          "Glitch" },
    { FXType::TVStatic,            "TV Static",          "Glitch" },
    { FXType::DitherBayer,         "Dither",             "Glitch" },
    { FXType::VHSDrop,             "VHS Drop",           "Glitch" },
    // Film & Texture
    { FXType::FilmGrain,           "Film Grain",         "Film" },
    { FXType::OldFilm,             "Old Film",           "Film" },
    { FXType::Lomo,                "Lomo",               "Film" },
    { FXType::Super8Film,          "Super 8",            "Film" },
    { FXType::Daguerreotype,       "Daguerreotype",      "Film" },
    { FXType::BleachBypass,        "Bleach Bypass",      "Film" },
    { FXType::FilmHalation,        "Film Halation",      "Film" },
    { FXType::FilmBurn,            "Film Burn",          "Film" },
    // Color
    { FXType::ChromaticAberration, "Chromatic Aberration","Color" },
    { FXType::Duotone,             "Duotone",            "Color" },
    { FXType::GradientMap,         "Gradient Map",       "Color" },
    { FXType::CrossProcess,        "Cross Process",      "Color" },
    { FXType::Technicolor,         "Technicolor",        "Color" },
    { FXType::Kodachrome,          "Kodachrome",         "Color" },
    { FXType::MiamiVice,           "Miami Vice",         "Color" },
    { FXType::GoldenHour,          "Golden Hour",        "Color" },
    { FXType::SplitToning,         "Split Toning",       "Color" },
    { FXType::Solarize,            "Solarize",           "Color" },
    // Glow & Light
    { FXType::NeonGlow,            "Neon Glow",          "Light" },
    { FXType::GodRays,             "God Rays",           "Light" },
    { FXType::AuroraBorealis,      "Aurora",             "Light" },
    { FXType::StarburstSpike,      "Starburst",          "Light" },
    { FXType::BokehDream,          "Bokeh Dream",        "Light" },
    { FXType::NeonEdgeGlow,        "Neon Edge",          "Light" },
    { FXType::NeonSign,            "Neon Sign",          "Light" },
    // Distort & Warp
    { FXType::Fisheye,             "Fisheye",            "Warp" },
    { FXType::Twirl,               "Twirl",              "Warp" },
    { FXType::Ripple,              "Ripple",             "Warp" },
    { FXType::WaveWarp,            "Wave Warp",          "Warp" },
    { FXType::Kaleidoscope,        "Kaleidoscope",       "Warp" },
    { FXType::MirrorFold,          "Mirror Fold",        "Warp" },
    { FXType::VortexDistort,       "Vortex",             "Warp" },
    { FXType::BarrelWarp,          "Barrel Warp",        "Warp" },
    { FXType::TiltShift,           "Tilt Shift",         "Warp" },
    { FXType::MirrorTunnel,        "Mirror Tunnel",      "Warp" },
    { FXType::LiquidChrome,        "Liquid Chrome",      "Warp" },
    // Scan & Pattern
    { FXType::Scanlines,           "Scanlines",          "Pattern" },
    { FXType::Halftone,            "Halftone",           "Pattern" },
    { FXType::Posterize,           "Posterize",          "Pattern" },
    { FXType::CRT,                 "CRT",                "Pattern" },
    { FXType::CrtBarrel,           "CRT Barrel",         "Pattern" },
    { FXType::LaserGrid,           "Laser Grid",         "Pattern" },
    { FXType::PixelMosaic,         "Pixel Mosaic",       "Pattern" },
    { FXType::AsciiArt,            "ASCII Art",          "Pattern" },
    { FXType::ComicDots,           "Comic Dots",         "Pattern" },
    { FXType::Crosshatch,          "Crosshatch",         "Pattern" },
};
static const int k_n_shader_fx = (int)(sizeof(k_shader_fx)/sizeof(k_shader_fx[0]));

// ── Multi-FX chain panel ──────────────────────────────────────────────────────

void panel_multifx(AppState& state, float w) {
    if (state.selected_track < 0 || state.selected_track >= (int)state.tracks.size()) return;
    Track& track = state.tracks[state.selected_track];
    if (state.selected_clip < 0 || state.selected_clip >= (int)track.clips.size()) return;
    Clip& brick = track.clips[state.selected_clip];
    if (brick.clip_type != ClipType::MultiFX) return;

    ImGui::Dummy({0.f, 8.f});
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(210,110,30,255));
    ImGui::TextUnformatted("Multi-FX");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.f, 8.f);
    bool is_glass = fx_clip_is_glass(state, state.selected_track, brick);
    ImGui::PushStyleColor(ImGuiCol_Text, is_glass ? IM_COL32(130,210,255,255) : IM_COL32(160,110,255,255));
    ImGui::TextUnformatted(is_glass ? "GLASS" : "GLOBAL");
    ImGui::PopStyleColor();

    char info[128];
    snprintf(info, sizeof(info), "%s  ·  %.2fs – %.2fs  ·  %d effect%s",
        track.name.c_str(), brick.start, brick.end,
        (int)brick.fx_chain.size(), brick.fx_chain.size() == 1 ? "" : "s");
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextWrapped("%s", info);
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 4.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

    float brick_dur = fmaxf(0.01f, brick.end - brick.start);

    // ── Add Effect dropdown ────────────────────────────────────────────────
    if (ImGui::BeginCombo("##mfx_add", "+ Add Effect", ImGuiComboFlags_NoArrowButton)) {
        for (int k = 0; k < g_n_fx_cards; ++k) {
            if (ImGui::Selectable(g_fx_cards[k].name)) {
                Clip se;
                se.clip_type = ClipType::Effect;
                se.fx_type   = g_fx_cards[k].type;
                se.rel_start = 0.f; se.rel_end = 0.f;
                brick.fx_chain.push_back(std::move(se));
                brick.fx_chain_selected = (int)brick.fx_chain.size() - 1;
                history_push(state, std::string("Multi-FX: add ") + g_fx_cards[k].name);
            }
        }
        // Body FX section — always glass within MultiFX
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(20, 200, 180, 160));
        ImGui::TextUnformatted("Body FX  (glass only)");
        ImGui::PopStyleColor();
        {
            int n_bfx = body_fx_info_count();
            const BodyFXInfo* bfx_list = body_fx_info_list();
            const char* last_cat = nullptr;
            for (int k = 0; k < n_bfx; ++k) {
                const BodyFXInfo& info = bfx_list[k];
                if (!last_cat || strcmp(last_cat, info.category) != 0) {
                    last_cat = info.category;
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(140, 140, 140, 180));
                    ImGui::TextUnformatted(info.category);
                    ImGui::PopStyleColor();
                }
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(20, 200, 180, 255));
                if (ImGui::Selectable(info.name)) {
                    Clip se;
                    se.clip_type      = ClipType::BodyFX;
                    se.body_fx_type   = info.type;
                    se.body_fx_amount = 1.f;
                    for (int pi = 0; pi < 4; ++pi)
                        se.body_fx_params[pi] = (pi < info.n_params) ? info.params[pi].default_val : 0.5f;
                    se.rel_start = 0.f; se.rel_end = 0.f;
                    brick.fx_chain.push_back(std::move(se));
                    brick.fx_chain_selected = (int)brick.fx_chain.size() - 1;
                    history_push(state, std::string("Multi-FX: add Body FX ") + info.name);
                }
                ImGui::PopStyleColor();
            }
        }
        // Shader FX section
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 160, 255, 160));
        ImGui::TextUnformatted("Shader FX");
        ImGui::PopStyleColor();
        {
            const char* last_shader_cat = nullptr;
            for (int k = 0; k < k_n_shader_fx; ++k) {
                const ShaderFXCard& sc = k_shader_fx[k];
                if (!last_shader_cat || strcmp(last_shader_cat, sc.category) != 0) {
                    last_shader_cat = sc.category;
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(130, 130, 150, 180));
                    ImGui::TextUnformatted(sc.category);
                    ImGui::PopStyleColor();
                }
                if (ImGui::Selectable(sc.name)) {
                    Clip se;
                    se.clip_type = ClipType::Effect;
                    se.fx_type   = sc.type;
                    se.rel_start = 0.f; se.rel_end = 0.f;
                    brick.fx_chain.push_back(std::move(se));
                    brick.fx_chain_selected = (int)brick.fx_chain.size() - 1;
                    history_push(state, std::string("Multi-FX: add shader ") + sc.name);
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Dummy({0.f, 8.f});

    // ── Chain list ─────────────────────────────────────────────────────────
    int del_idx = -1, swap_a = -1, swap_b = -1;
    const float del_btn_reserve = 22.f;  // SmallButton "x" + SameLine gap
    for (int i = 0; i < (int)brick.fx_chain.size(); ++i) {
        Clip& se = brick.fx_chain[i];
        bool sel = (brick.fx_chain_selected == i);
        ImGui::PushID(i);

        // Row background
        ImVec2 row_pos = ImGui::GetCursorScreenPos();
        float row_h   = 28.f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (sel) {
            ImU32 sel_bg = (se.clip_type == ClipType::BodyFX)
                ? IM_COL32(10, 60, 55, 180) : IM_COL32(60, 40, 20, 180);
            dl->AddRectFilled(row_pos, {row_pos.x + w, row_pos.y + row_h}, sel_bg, 3.f);
        }

        // Up/down reorder buttons (compact)
        ImGui::SetCursorScreenPos({row_pos.x + 2.f, row_pos.y + 5.f});
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80,60,20,160));
        if (ImGui::SmallButton("^") && i > 0)                        { swap_a = i-1; swap_b = i; }
        ImGui::SameLine(0.f, 2.f);
        if (ImGui::SmallButton("v") && i < (int)brick.fx_chain.size()-1) { swap_a = i; swap_b = i+1; }
        ImGui::PopStyleColor(2);
        ImGui::EndGroup();

        // Effect name (click to select)
        ImGui::SameLine(0.f, 6.f);
        const char* entry_name = nullptr;
        ImU32 name_col;
        if (se.clip_type == ClipType::BodyFX) {
            const BodyFXInfo* bfi = body_fx_find_info(se.body_fx_type);
            entry_name = bfi ? bfi->name : "Body FX";
            name_col = sel ? IM_COL32(80, 255, 230, 255) : IM_COL32(20, 200, 180, 255);
        } else {
            entry_name = fx_type_display(se.fx_type);
            name_col = sel ? IM_COL32(255, 200, 80, 255) : IM_COL32(220, 220, 220, 255);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, name_col);
        if (ImGui::Selectable(entry_name, sel,
                              ImGuiSelectableFlags_DontClosePopups, {80.f, row_h}))
            brick.fx_chain_selected = i;
        ImGui::PopStyleColor();

        // Duration mini-bar
        ImGui::SameLine(0.f, 6.f);
        {
            float rel_end_eff = (se.rel_end <= 0.f) ? brick_dur : se.rel_end;
            float x0  = ImGui::GetCursorScreenPos().x;
            float y0  = ImGui::GetCursorScreenPos().y + 8.f;
            float bh  = row_h - 16.f;
            float bar_w = ImGui::GetContentRegionAvail().x - del_btn_reserve;
            if (bar_w < 10.f) bar_w = 10.f;
            // Track background
            dl->AddRectFilled({x0, y0}, {x0 + bar_w, y0 + bh}, IM_COL32(40,40,40,200), 2.f);
            // Active region
            float rx0 = x0 + (se.rel_start / brick_dur) * bar_w;
            float rx1 = x0 + (rel_end_eff  / brick_dur) * bar_w;
            ImU32 bar_col = sel ? IM_COL32(210,130,30,220) : IM_COL32(150,90,20,180);
            dl->AddRectFilled({rx0, y0}, {rx1, y0 + bh}, bar_col, 2.f);

            // Draggable left handle
            ImGui::SetCursorScreenPos({rx0 - 3.f, y0});
            ImGui::InvisibleButton("##hleft", {8.f, bh});
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
                float dx = ImGui::GetIO().MouseDelta.x / bar_w * brick_dur;
                se.rel_start = fmaxf(0.f, fminf(se.rel_start + dx, rel_end_eff - 0.1f));
                history_push(state, "Multi-FX: adjust timing");
            }
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

            // Draggable right handle (only if rel_end != 0, i.e. not "full")
            ImGui::SetCursorScreenPos({rx1 - 3.f, y0});
            ImGui::InvisibleButton("##hright", {8.f, bh});
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
                float dx = ImGui::GetIO().MouseDelta.x / bar_w * brick_dur;
                float new_end = rel_end_eff + dx;
                // Snap to full-duration when near the end
                if (new_end >= brick_dur * 0.97f) new_end = 0.f;
                else new_end = fmaxf(se.rel_start + 0.1f, fminf(new_end, brick_dur));
                se.rel_end = new_end;
                history_push(state, "Multi-FX: adjust timing");
            }
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

            // Advance cursor past bar
            ImGui::SetCursorScreenPos({x0 + bar_w, y0 - 8.f});
            ImGui::Dummy({0.f, row_h});
        }

        // Delete button
        ImGui::SameLine(0.f, 4.f);
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(180,40,40,180));
        if (ImGui::SmallButton("x")) del_idx = i;
        ImGui::PopStyleColor(2);

        ImGui::PopID();
    }

    // Apply pending swap / delete
    if (swap_a >= 0) {
        std::swap(brick.fx_chain[swap_a], brick.fx_chain[swap_b]);
        if (brick.fx_chain_selected == swap_a) brick.fx_chain_selected = swap_b;
        else if (brick.fx_chain_selected == swap_b) brick.fx_chain_selected = swap_a;
        history_push(state, "Multi-FX: reorder");
    }
    if (del_idx >= 0) {
        brick.fx_chain.erase(brick.fx_chain.begin() + del_idx);
        brick.fx_chain_selected = fmaxf(-1, (float)(brick.fx_chain_selected - (brick.fx_chain_selected >= del_idx ? 1 : 0)));
        history_push(state, "Multi-FX: remove effect");
    }

    if (brick.fx_chain.empty()) {
        ImGui::Dummy({0.f, 12.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextWrapped("No effects yet — use 'Add Effect' above.");
        ImGui::PopStyleColor();
        return;
    }

    // ── Parameters for selected sub-effect ────────────────────────────────
    int si = brick.fx_chain_selected;
    if (si < 0 || si >= (int)brick.fx_chain.size()) { ImGui::Dummy({0.f, 8.f}); return; }
    Clip& clip = brick.fx_chain[si];   // named 'clip' so generated includes work

    ImGui::Dummy({0.f, 8.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

    if (clip.clip_type == ClipType::BodyFX) {
        // ── BodyFX sub-effect params ──────────────────────────────────────
        const BodyFXInfo* bfi = body_fx_find_info(clip.body_fx_type);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(20, 200, 180, 255));
        ImGui::TextUnformatted(bfi ? bfi->name : "Body FX");
        ImGui::PopStyleColor();
        ImGui::SameLine(0.f, 12.f);
        {
            float re = (clip.rel_end <= 0.f) ? brick_dur : clip.rel_end;
            char dlbl[32]; snprintf(dlbl, sizeof(dlbl), "%.1fs – %.1fs", clip.rel_start, re);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted(dlbl);
            ImGui::PopStyleColor();
        }
        ImGui::Dummy({0.f, 4.f});
        if (bfi) {
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted(bfi->tagline);
            ImGui::PopStyleColor();
        }
        ImGui::Dummy({0.f, 6.f});
        float sw = w - 16.f;
        ImGui::PushStyleColor(ImGuiCol_SliderGrab,       IM_COL32(20, 200, 180, 255));
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(80, 255, 230, 255));
        ImGui::PushStyleColor(ImGuiCol_FrameBg,          Col::bg_soft);
        ui_label("Amount");
        ImGui::SetNextItemWidth(sw);
        ImGui::SliderFloat("##mfx_bfx_amt", &clip.body_fx_amount, 0.f, 1.f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX Body FX: amount");
        if (bfi && bfi->n_params > 0) {
            ImGui::Dummy({0.f, 4.f});
            for (int pi = 0; pi < bfi->n_params && pi < 4; ++pi) {
                const BodyFXParamDef& pd = bfi->params[pi];
                char pid[32]; snprintf(pid, sizeof(pid), "##mfxbfxp%d", pi);
                ui_label(pd.label);
                ImGui::SetNextItemWidth(sw);
                ImGui::SliderFloat(pid, &clip.body_fx_params[pi], pd.min_val, pd.max_val, "%.3f");
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX Body FX: param");
                ImGui::Dummy({0.f, 2.f});
            }
        }
        ImGui::PopStyleColor(3);
        ImGui::Dummy({0.f, 6.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted("Masks are sourced from the video clip on this track.");
        ImGui::TextUnformatted("Run 'Process Masks' on the video clip if the effect is invisible.");
        ImGui::PopStyleColor();
    } else {
        // ── Regular Effect params ─────────────────────────────────────────
        ImU32 ac = fx_type_accent(clip.fx_type);
        ImGui::PushStyleColor(ImGuiCol_Text, ac);
        ImGui::TextUnformatted(fx_type_display(clip.fx_type));
        ImGui::PopStyleColor();
        ImGui::SameLine(0.f, 12.f);
        {
            float rel_end_eff = (clip.rel_end <= 0.f) ? brick_dur : clip.rel_end;
            char dur_lbl[32];
            snprintf(dur_lbl, sizeof(dur_lbl), "%.1fs – %.1fs", clip.rel_start, rel_end_eff);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted(dur_lbl);
            ImGui::PopStyleColor();
        }
        ImGui::Dummy({0.f, 4.f});

        float sw = w - 16.f;
        ImGui::PushStyleColor(ImGuiCol_SliderGrab,       ac);
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ac);
        ImGui::PushStyleColor(ImGuiCol_FrameBg,          Col::bg_soft);

        switch (clip.fx_type) {
            case FXType::Glitch:
                ui_label("Chroma Shift");
                ImGui::SetNextItemWidth(sw);
                ImGui::SliderFloat("##mgchroma", &clip.fx_glitch_chroma, 0.f, 30.f, "%.1f px");
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX Glitch: chroma");
                ImGui::Dummy({0.f, 4.f});
                ui_label("Row Jitter");
                ImGui::SetNextItemWidth(sw);
                ImGui::SliderFloat("##mgjitter", &clip.fx_glitch_jitter, 0.f, 1.f, "%.2f");
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX Glitch: jitter");
                ImGui::Dummy({0.f, 4.f});
                ui_label("Block Corruption");
                ImGui::SetNextItemWidth(sw);
                ImGui::SliderFloat("##mgcorrupt", &clip.fx_glitch_corruption, 0.f, 1.f, "%.2f");
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX Glitch: corruption");
                ImGui::Dummy({0.f, 4.f});
                ui_label("Layer Bleed");
                ImGui::SetNextItemWidth(sw);
                ImGui::SliderFloat("##mgbleed", &clip.fx_glitch_corruption_bleed, 0.f, 1.f, "%.2f");
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX Glitch: bleed");
                break;

            case FXType::ZoomPunch:
                ui_label("Punch Strength");
                ImGui::SetNextItemWidth(sw);
                ImGui::SliderFloat("##mzstr", &clip.fx_zoom_strength, 0.f, 0.5f, "%.2f");
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX Zoom: strength");
                ImGui::Dummy({0.f, 4.f});
                ui_label("Decay");
                ImGui::SetNextItemWidth(sw);
                ImGui::SliderFloat("##mzdec", &clip.fx_zoom_decay, 0.05f, 0.5f, "%.2fs");
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX Zoom: decay");
                ImGui::Dummy({0.f, 4.f});
                ui_label("Shake");
                ImGui::SetNextItemWidth(sw);
                ImGui::SliderFloat("##mzshk", &clip.fx_zoom_shake, 0.f, 1.f, "%.2f");
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX Zoom: shake");
                break;

            case FXType::LightLeak:
                ui_label("Intensity");
                ImGui::SetNextItemWidth(sw);
                ImGui::SliderFloat("##mllint", &clip.fx_leak_intensity, 0.f, 1.f, "%.2f");
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX Leak: intensity");
                ImGui::Dummy({0.f, 4.f});
                ui_label("Speed");
                ImGui::SetNextItemWidth(sw);
                ImGui::SliderFloat("##mllspd", &clip.fx_leak_speed, 0.f, 4.f, "%.1f");
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX Leak: speed");
                break;

            case FXType::VHS:
                ui_label("Noise");
                ImGui::SetNextItemWidth(sw);
                ImGui::SliderFloat("##mvhsnoise", &clip.fx_vhs_noise, 0.f, 1.f, "%.2f");
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX VHS: noise");
                ImGui::Dummy({0.f, 4.f});
                ui_label("Chroma Bleed");
                ImGui::SetNextItemWidth(sw);
                ImGui::SliderFloat("##mvhsbleed", &clip.fx_vhs_bleed, 0.f, 20.f, "%.1f px");
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX VHS: bleed");
                ImGui::Dummy({0.f, 4.f});
                ui_label("Tracking");
                ImGui::SetNextItemWidth(sw);
                ImGui::SliderFloat("##mvhstrk", &clip.fx_vhs_tracking, 0.f, 1.f, "%.2f");
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX VHS: tracking");
                break;

            case FXType::Datamosh:
                ui_label("Intensity");
                ImGui::SetNextItemWidth(sw);
                ImGui::SliderFloat("##mdmint", &clip.fx_datamosh_intensity, 0.f, 1.f, "%.2f");
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX Datamosh: intensity");
                ImGui::Dummy({0.f, 4.f});
                ui_label("Spread");
                ImGui::SetNextItemWidth(sw);
                ImGui::SliderFloat("##mdmspread", &clip.fx_datamosh_spread, 0.f, 1.f, "%.2f");
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX Datamosh: spread");
                break;

            case FXType::ChromaKey: {
                float sw2 = sw - 20.f;
                ui_label("Key Color  R");
                ImGui::SetNextItemWidth(sw2);
                ImGui::SliderFloat("##mckr", &clip.fx_chroma_key_r, 0.f, 1.f, "%.2f");
                ui_label("           G");
                ImGui::SetNextItemWidth(sw2);
                ImGui::SliderFloat("##mckg", &clip.fx_chroma_key_g, 0.f, 1.f, "%.2f");
                ui_label("           B");
                ImGui::SetNextItemWidth(sw2);
                ImGui::SliderFloat("##mckb", &clip.fx_chroma_key_b, 0.f, 1.f, "%.2f");
                ImGui::Dummy({0.f, 4.f});
                ui_label("Threshold");
                ImGui::SetNextItemWidth(sw2);
                ImGui::SliderFloat("##mckthr", &clip.fx_chroma_key_threshold, 0.f, 1.f, "%.2f");
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX ChromaKey");
                ImGui::Dummy({0.f, 4.f});
                ui_label("Softness");
                ImGui::SetNextItemWidth(sw2);
                ImGui::SliderFloat("##mcksoft", &clip.fx_chroma_key_softness, 0.f, 0.5f, "%.2f");
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX ChromaKey: softness");
                break;
            }

            case FXType::Grade:
                if (ImGui::Checkbox("Color Grade##mfx", &clip.fx_color_on))
                    history_push(state, "Multi-FX Grade");
                if (clip.fx_color_on) {
                    ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Brightness##mfx", &clip.fx_brightness, -1.f, 1.f, "%.2f");
                    if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX Grade: brightness");
                    ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Contrast##mfx",   &clip.fx_contrast, 0.5f, 2.f, "%.2f");
                    if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX Grade: contrast");
                    ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Saturation##mfx", &clip.fx_saturation, 0.f, 2.f, "%.2f");
                    if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX Grade: saturation");
                    ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Hue##mfx",        &clip.fx_hue, -180.f, 180.f, "%.0f\xc2\xb0");
                    if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX Grade: hue");
                }
                break;

            case FXType::Blur:
                if (ImGui::Checkbox("Blur##mfx", &clip.fx_blur_on))
                    history_push(state, "Multi-FX Blur");
                if (clip.fx_blur_on) {
                    ImGui::SetNextItemWidth(sw);
                    ImGui::SliderFloat("Radius##mfx", &clip.fx_blur, 0.f, 20.f, "%.1f px");
                    if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX Blur: radius");
                }
                break;

            case FXType::Vignette:
                if (ImGui::Checkbox("Vignette##mfx", &clip.fx_vignette_on))
                    history_push(state, "Multi-FX Vignette");
                if (clip.fx_vignette_on) {
                    ImGui::SetNextItemWidth(sw);
                    ImGui::SliderFloat("Strength##mfx", &clip.fx_vignette, 0.f, 1.f, "%.2f");
                    if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Multi-FX Vignette: strength");
                }
                break;

            // Shader FX — generated inspector covers these
#include "generated/fx_ui_inspector.h"

            default:
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                ImGui::TextWrapped("No parameters for this effect.");
                ImGui::PopStyleColor();
                break;
        }

        ImGui::PopStyleColor(3);
    }

    ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
    if (track.locked) ImGui::BeginDisabled();
    if (ui_btn("Delete Multi-FX brick", false, true)) {
        if (track.locked) ImGui::EndDisabled();
        track.clips.erase(track.clips.begin() + state.selected_clip);
        state.selected_clip = -1;
        history_push(state, "Delete Multi-FX clip");
        return;
    }
    if (track.locked) ImGui::EndDisabled();
}

// ── Body FX library / toolbox ─────────────────────────────────────────────────

void panel_body_fx_library(AppState& state, float w) {
    // Find selected BodyFX brick (if any)
    Clip* bfx_clip = nullptr;
    int   bfx_ti   = state.selected_track;
    int   bfx_ci   = state.selected_clip;
    if (bfx_ti >= 0 && bfx_ti < (int)state.tracks.size() &&
        bfx_ci >= 0 && bfx_ci < (int)state.tracks[bfx_ti].clips.size()) {
        Clip& c = state.tracks[bfx_ti].clips[bfx_ci];
        if (c.clip_type == ClipType::BodyFX) bfx_clip = &c;
    }

    // Find selected video clip (target track for adding BodyFX bricks)
    Clip* vid_clip = nullptr;
    int   vid_ti   = state.selected_track;
    int   vid_ci   = state.selected_clip;
    if (!bfx_clip &&
        vid_ti >= 0 && vid_ti < (int)state.tracks.size() &&
        vid_ci >= 0 && vid_ci < (int)state.tracks[vid_ti].clips.size()) {
        Clip& c = state.tracks[vid_ti].clips[vid_ci];
        if (c.clip_type == ClipType::Video) vid_clip = &c;
    }

    float bar_w = w - 16.f;

    // ── Selected brick controls (only shown when a BodyFX brick is selected) ───
    if (bfx_clip) {
        ImGui::Dummy({0.f, 8.f});
        {
            const BodyFXInfo* info = body_fx_find_info(bfx_clip->body_fx_type);
            if (info) {
                ImGui::PushFont(g_font_bold);
                ImGui::TextUnformatted(info->name);
                ImGui::PopFont();
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                ImGui::TextUnformatted(info->tagline);
                ImGui::PopStyleColor();
            }
        }

        auto plain_slider = [&](const char* id, const char* label, float* v,
                                float vmin, float vmax, const char* fmt) {
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, to_u32(Col::fg));
            ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
            ImGui::SetNextItemWidth(bar_w);
            ImGui::SliderFloat(id, v, vmin, vmax, fmt);
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, label);
        };

        ImGui::Dummy({0.f, 6.f});
        plain_slider("##bfx_amount", "Amount", &bfx_clip->body_fx_amount, 0.f, 1.f, "%.2f");

        const BodyFXInfo* info = body_fx_find_info(bfx_clip->body_fx_type);
        if (info && info->n_params > 0) {
            ImGui::Dummy({0.f, 4.f});
            for (int pi = 0; pi < info->n_params && pi < 4; ++pi) {
                const BodyFXParamDef& pd = info->params[pi];
                char id[32]; snprintf(id, sizeof(id), "##lbfxp%d", pi);
                plain_slider(id, pd.label, &bfx_clip->body_fx_params[pi],
                             pd.min_val, pd.max_val, "%.3f");
                ImGui::Dummy({0.f, 2.f});
            }
        }

        ImGui::Dummy({0.f, 10.f});
        ui_separator();
    }

    // ── Effect browser ────────────────────────────────────────────────────────
    ImGui::Dummy({0.f, 8.f});
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(80, 240, 210, 255));
    ImGui::TextUnformatted("Body FX");
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
    if (bfx_clip)
        ImGui::TextWrapped("Click to change the selected brick's effect type.");
    else if (vid_clip)
        ImGui::TextWrapped("Click to add a Body FX brick to the selected video clip's track.");
    else
        ImGui::TextWrapped("Select a video clip in the timeline to add Body FX bricks.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 8.f});

    int n_infos         = body_fx_info_count();
    const BodyFXInfo* infos = body_fx_info_list();
    const char* cur_cat = nullptr;

    for (int i = 0; i < n_infos; ++i) {
        const BodyFXInfo& info = infos[i];

        // Category header when it changes
        if (!cur_cat || strcmp(cur_cat, info.category) != 0) {
            if (cur_cat) ImGui::Dummy({0.f, 4.f});
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted(info.category);
            ImGui::PopStyleColor();
            cur_cat = info.category;
        }

        float card_h = 44.f;
        float card_w = w - 8.f;
        bool selected = bfx_clip && (bfx_clip->body_fx_type == info.type);

        ImGui::PushID(70000 + i);
        ImVec2 cp = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        bool can_act = bfx_clip || vid_clip;
        bool hov = can_act && ImGui::IsMouseHoveringRect(cp, {cp.x + card_w, cp.y + card_h});
        ImU32 fill = selected  ? IM_COL32(10, 90, 80, 255)
                   : !can_act  ? IM_COL32(10, 20, 18, 180)
                   : hov       ? IM_COL32(20, 50, 46, 255)
                               : IM_COL32(14, 28, 26, 255);
        ImU32 border = selected  ? IM_COL32(30, 200, 170, 200)
                     : !can_act  ? IM_COL32(20, 35, 32, 120)
                     : hov       ? IM_COL32(40, 140, 120, 150)
                                 : IM_COL32(30, 60, 55, 180);
        dl->AddRectFilled(cp, {cp.x + card_w, cp.y + card_h}, fill, 4.f);
        dl->AddRect(cp, {cp.x + card_w, cp.y + card_h}, border, 4.f, 0, selected ? 1.5f : 1.f);

        ImGui::PushFont(g_font_bold);
        ImU32 nc = selected   ? IM_COL32(80, 240, 210, 255)
                 : !can_act   ? IM_COL32(80, 100, 95, 160)
                              : IM_COL32(200, 230, 225, 220);
        dl->AddText(ImGui::GetFont(), 13.f, {cp.x + 10.f, cp.y + 8.f}, nc, info.name);
        ImGui::PopFont();
        ImU32 tc = can_act ? to_u32(Col::dim) : IM_COL32(60, 75, 70, 140);
        dl->AddText({cp.x + 10.f, cp.y + 24.f}, tc, info.tagline);

        ImGui::SetCursorScreenPos(cp);
        ImGui::InvisibleButton("##bfx_card", {card_w, card_h});
        if (ImGui::IsItemClicked() && can_act) {
            if (bfx_clip) {
                // Change selected brick's effect type
                bfx_clip->body_fx_type = info.type;
                for (int pi = 0; pi < 4; ++pi)
                    bfx_clip->body_fx_params[pi] = pi < info.n_params
                                                   ? info.params[pi].default_val : 0.5f;
                history_push(state, std::string("Body FX: ") + info.name);
            } else {
                // Add BodyFX brick to the selected video clip's track
                Clip cl;
                cl.clip_type     = ClipType::BodyFX;
                cl.body_fx_type  = info.type;
                cl.start         = state.playhead;
                cl.end           = fminf(state.duration, state.playhead + 5.f);
                for (int pi = 0; pi < 4; ++pi)
                    cl.body_fx_params[pi] = pi < info.n_params
                                            ? info.params[pi].default_val : 0.5f;
                state.tracks[vid_ti].clips.push_back(cl);
                state.selected_track = vid_ti;
                state.selected_clip  = (int)state.tracks[vid_ti].clips.size() - 1;
                history_push(state, std::string("Add Body FX: ") + info.name);
                // Auto-start bg removal on the video clip if not already ready
                if (vid_clip->bg_remove_status != BgRemoveStatus::Ready)
                    bg_remove_start(state, vid_ti, vid_ci);
            }
        }

        ImGui::PopID();
        ImGui::Dummy({0.f, 2.f});
    }
}
