#pragma once
#include <string>
#include <vector>

enum class PresetCategory { Color, Blur, Vignette, Combo, User };

struct EffectPreset {
    std::string    name;
    PresetCategory category    = PresetCategory::Color;
    // Color grade
    bool  fx_color_on    = false;
    float fx_brightness  = 0.f;
    float fx_contrast    = 1.f;
    float fx_saturation  = 1.f;
    float fx_hue         = 0.f;
    // Blur
    bool  fx_blur_on     = false;
    float fx_blur        = 0.f;
    // Vignette
    bool  fx_vignette_on = false;
    float fx_vignette    = 0.f;
    // Text overrides
    bool  fx_text_on     = false;
    float fx_opacity_mul = 1.f;
    float fx_scale_mul   = 1.f;
};

extern const std::vector<EffectPreset> g_builtin_presets;

// User preset persistence — stored in ~/.config/pop-maker-studio/presets.json
std::vector<EffectPreset> presets_load_user();
void presets_save_user(const std::vector<EffectPreset>& presets);
