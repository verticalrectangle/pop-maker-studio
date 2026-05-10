#include "studio_types.h"
#include "studio_shared.h"
#include "panel_fx.h"
#include "pipeline.h"
#include "app.h"
#include "audio.h"
#include "video.h"
#include "history.h"
#include "filepicker.h"
#include "fx_shader.h"
#include "bg_presets.h"
#include "theme.h"
#include "presets.h"
#include "waveform.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <filesystem>
#include <algorithm>
#include <cmath>
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
#include "generated/fx_ui_picker.h"
};
static const int g_n_fx_cards = (int)(sizeof(g_fx_cards) / sizeof(g_fx_cards[0]));

// ── Right panel: Adjustment Library tab ──────────────────────────────────────

void panel_adjustment_library(AppState& state, float w) {
    ImGui::Dummy({0.f, 8.f});

    ImGui::TextUnformatted("Adjustment Library");
    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
    ImGui::TextWrapped("Click to apply to selected Adjustment clip. Drag to timeline to create one.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 8.f});

    // Determine if an Adjustment clip is currently selected (for click-to-apply).
    bool has_fx_clip = (state.selected_track >= 0 &&
                        state.selected_track < (int)state.tracks.size() &&
                        state.selected_clip  >= 0 &&
                        state.selected_clip  < (int)state.tracks[state.selected_track].clips.size() &&
                        state.tracks[state.selected_track].clips[state.selected_clip].clip_type == ClipType::Effect &&
                        state.tracks[state.selected_track].clips[state.selected_clip].fx_type == FXType::Adjustment);

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
            if (has_fx_clip) {
                preset_apply(state.tracks[state.selected_track].clips[state.selected_clip], p);
                history_push(state, "Apply preset: " + p.name);
            } else {
                // Always create a new track above everything for the adjustment clip
                Clip cl;
                cl.clip_type = ClipType::Effect;
                cl.fx_type   = FXType::Adjustment;
                cl.start     = state.playhead;
                cl.end       = state.playhead + 2.f;
                preset_apply(cl, p);
                Track nt; nt.name = "Adjust";
                nt.clips.push_back(cl);
                state.tracks.insert(state.tracks.begin(), std::move(nt));
                state.selected_track = 0;
                state.selected_clip  = 0;
                history_push(state, "Add Adjustment: " + p.name);
            }
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

    // ── Color Grade ───────────────────────────────────────────────────────────
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

    // ── Blur ──────────────────────────────────────────────────────────────────
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

    // ── Vignette ──────────────────────────────────────────────────────────────
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
            float dur = state.duration > 0.f ? state.duration : 30.f;
            Track t;
            t.name = "Background";
            Clip c;
            c.clip_type = ClipType::Background;
            c.start = 0.f; c.end = dur;
            c.text  = "blob";  // default preset
            t.clips.push_back(c);
            state.tracks.push_back(t);
            state.selected_track = (int)state.tracks.size() - 1;
            state.selected_clip  = 0;
            bgclip = &state.tracks[state.selected_track].clips[0];
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

        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderFloat("##bg_speed", &bgclip->bg_speed, 0.1f, 4.f, "Speed %.1fx"))
            history_push(state, "BG speed");
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderFloat("##bg_int", &bgclip->bg_intensity, 0.f, 1.f, "Intensity %.0f%%"))
            history_push(state, "BG intensity");
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
                // No bg clip selected — create one
                float dur = state.duration > 0.f ? state.duration : 30.f;
                Track t; t.name = "Background";
                Clip c; c.clip_type = ClipType::Background; c.start = 0.f; c.end = dur;
                c.text = pr.id;
                memcpy(c.bg_c1, pr.dc1, sizeof(float)*4);
                memcpy(c.bg_c2, pr.dc2, sizeof(float)*4);
                memcpy(c.bg_c3, pr.dc3, sizeof(float)*4);
                t.clips.push_back(c);
                state.tracks.push_back(t);
                state.selected_track = (int)state.tracks.size()-1;
                state.selected_clip  = 0;
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

// ── Right panel: Creative FX clip controls ────────────────────────────────────

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
