#include "presets.h"
#include "../vendor/json.hpp"
#include <fstream>
#include <cstdlib>
#include <sys/stat.h>

using json = nlohmann::json;

// ── Built-in presets ──────────────────────────────────────────────────────────

const std::vector<EffectPreset> g_builtin_presets = {
    // ── Color ─────────────────────────────────────────────────────────────────
    { "Warm",         PresetCategory::Color, true,  0.06f, 1.05f, 1.15f,  12.f },
    { "Cool",         PresetCategory::Color, true, -0.03f, 1.05f, 0.90f, -18.f },
    { "Cinematic",    PresetCategory::Color, true,  0.00f, 1.15f, 0.72f,   0.f,
                                             false, 0.f,
                                             true,  0.38f },
    { "Vintage",      PresetCategory::Color, true, -0.05f, 0.90f, 0.60f,  18.f,
                                             false, 0.f,
                                             true,  0.30f },
    { "B&W",          PresetCategory::Color, true,  0.00f, 1.10f, 0.00f,   0.f },
    { "Neon",         PresetCategory::Color, true,  0.02f, 1.20f, 1.80f,   0.f },
    { "Golden Hour",  PresetCategory::Color, true,  0.08f, 1.05f, 1.25f,  22.f },
    { "Teal & Orange",PresetCategory::Color, true,  0.00f, 1.10f, 1.35f, -22.f },
    { "Overexposed",  PresetCategory::Color, true,  0.28f, 0.85f, 0.95f,   0.f },
    { "Underexposed", PresetCategory::Color, true, -0.28f, 1.15f, 0.95f,   0.f },
    // ── Blur ──────────────────────────────────────────────────────────────────
    { "Soft Focus",   PresetCategory::Blur,  false, 0.f,   1.f,   1.f,    0.f,
                                             true,  1.5f },
    { "Dream",        PresetCategory::Blur,  true,  0.05f, 0.95f, 1.05f,  0.f,
                                             true,  3.0f },
    { "Heavy Blur",   PresetCategory::Blur,  false, 0.f,   1.f,   1.f,    0.f,
                                             true,  7.0f },
    // ── Vignette ──────────────────────────────────────────────────────────────
    { "Subtle Frame", PresetCategory::Vignette, false, 0.f, 1.f, 1.f, 0.f,
                                                false, 0.f,
                                                true,  0.22f },
    { "Dark Border",  PresetCategory::Vignette, false, 0.f, 1.f, 1.f, 0.f,
                                                false, 0.f,
                                                true,  0.55f },
    { "Spotlight",    PresetCategory::Vignette, true,  0.10f, 1.05f, 1.f, 0.f,
                                                false, 0.f,
                                                true,  0.72f },
    // ── Combo ─────────────────────────────────────────────────────────────────
    { "Moody",        PresetCategory::Combo, true,  -0.04f, 1.20f, 0.65f, 0.f,
                                             false, 0.f,
                                             true,  0.45f },
    { "Film",         PresetCategory::Combo, true,   0.00f, 1.15f, 0.78f, 5.f,
                                             false, 0.f,
                                             true,  0.30f },
    { "Night Vision", PresetCategory::Combo, true,   0.00f, 1.20f, 0.80f, 120.f },
};

// ── User preset persistence ───────────────────────────────────────────────────

static std::string user_presets_path() {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    std::string dir = std::string(home) + "/.config/pop-maker-studio";
    mkdir(dir.c_str(), 0755);
    return dir + "/presets.json";
}

static const char* cat_to_str(PresetCategory c) {
    switch (c) {
        case PresetCategory::Color:   return "Color";
        case PresetCategory::Blur:    return "Blur";
        case PresetCategory::Vignette:return "Vignette";
        case PresetCategory::Combo:   return "Combo";
        default:                      return "User";
    }
}
static PresetCategory str_to_cat(const std::string& s) {
    if (s == "Color")    return PresetCategory::Color;
    if (s == "Blur")     return PresetCategory::Blur;
    if (s == "Vignette") return PresetCategory::Vignette;
    if (s == "Combo")    return PresetCategory::Combo;
    return PresetCategory::User;
}

std::vector<EffectPreset> presets_load_user() {
    std::ifstream f(user_presets_path());
    if (!f) return {};
    try {
        json j = json::parse(f);
        std::vector<EffectPreset> out;
        for (auto& e : j) {
            EffectPreset p;
            p.name           = e.value("name",           std::string{});
            p.category       = str_to_cat(e.value("category", std::string("User")));
            p.fx_color_on    = e.value("fx_color_on",    false);
            p.fx_brightness  = e.value("fx_brightness",  0.f);
            p.fx_contrast    = e.value("fx_contrast",    1.f);
            p.fx_saturation  = e.value("fx_saturation",  1.f);
            p.fx_hue         = e.value("fx_hue",         0.f);
            p.fx_blur_on     = e.value("fx_blur_on",     false);
            p.fx_blur        = e.value("fx_blur",        0.f);
            p.fx_vignette_on = e.value("fx_vignette_on", false);
            p.fx_vignette    = e.value("fx_vignette",    0.f);
            p.fx_text_on     = e.value("fx_text_on",     false);
            p.fx_opacity_mul = e.value("fx_opacity_mul", 1.f);
            p.fx_scale_mul   = e.value("fx_scale_mul",   1.f);
            out.push_back(p);
        }
        return out;
    } catch (...) { return {}; }
}

void presets_save_user(const std::vector<EffectPreset>& presets) {
    json j = json::array();
    for (auto& p : presets) {
        json e;
        e["name"]           = p.name;
        e["category"]       = cat_to_str(p.category);
        e["fx_color_on"]    = p.fx_color_on;
        e["fx_brightness"]  = p.fx_brightness;
        e["fx_contrast"]    = p.fx_contrast;
        e["fx_saturation"]  = p.fx_saturation;
        e["fx_hue"]         = p.fx_hue;
        e["fx_blur_on"]     = p.fx_blur_on;
        e["fx_blur"]        = p.fx_blur;
        e["fx_vignette_on"] = p.fx_vignette_on;
        e["fx_vignette"]    = p.fx_vignette;
        e["fx_text_on"]     = p.fx_text_on;
        e["fx_opacity_mul"] = p.fx_opacity_mul;
        e["fx_scale_mul"]   = p.fx_scale_mul;
        j.push_back(e);
    }
    std::ofstream f(user_presets_path());
    if (f) f << j.dump(2) << "\n";
}
