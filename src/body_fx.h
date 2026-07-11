#pragma once
#include <string>
#include <cstdint>

enum class BodyFXType : int {
    // Mask utility (must stay first — index 0)
    RemoveBackground = 0,
    // Retro / 80s
    NeonOutline, VHSBody, Scanlines, ChromaticBody, Retrowave,
    TVStaticBg, Arcade, FilmGrainBody,
    // Depth / Focus
    DepthBlur, TiltShiftBg, Spotlight, FogBg, BokehBg,
    // Glitch
    GlitchBody, GlitchBg, SplitReality, DataCorruption, SignalLossBg, Chromafall,
    // Color
    ColorIsolation, InvertBg, DuotoneBody, DuotoneBg, ThermalBody, XRay,
    // Light / Glow
    ElectricAura, RimLight, GodRays, Halo, BloomBody, Bioluminescent,
    // Abstract
    Hologram, MatrixRainBg, BodyMosaic, WireframeBg, Vaporwave,
    // Party
    StrobeBg, DiscoBody, UVPaint, FireAura,
    // Beauty
    BeautySmooth,
    Count
};

struct BodyFXParamDef {
    const char* label;
    float min_val, max_val, default_val;
};

struct BodyFXInfo {
    BodyFXType      type;
    const char*     name;
    const char*     tagline;
    const char*     category;
    unsigned        accent;   // ImU32
    int             n_params;
    BodyFXParamDef  params[4];
    const char*     frag_body; // inserted into main() after preamble
};

// Compile all 40 programs. Call after GL context is ready.
void body_fx_init();

// Free all GL resources.
void body_fx_shutdown();

// Returns info for all 40 effects.
const BodyFXInfo* body_fx_info_list();
int               body_fx_info_count();
const BodyFXInfo* body_fx_find_info(BodyFXType t);

// Load (and cache) the per-frame mask PNG as a GL texture.
// Returns 0 if the mask file doesn't exist yet.
unsigned body_fx_mask_texture(const std::string& mask_dir, int frame_idx);

// Apply body FX to src_tex using mask_tex. Returns output GL texture id.
// Returns src_tex if any error (missing program, bad tex, etc).
uintptr_t body_fx_apply(BodyFXType type,
                         uintptr_t src_tex, unsigned mask_tex,
                         int w, int h,
                         const float params[4],
                         float amount, float t,
                         const float bg_box[4] = nullptr, float bg_softness = 0.f);

// Render a body FX onto the built-in preview portrait + its mask, for the picker
// card thumbnails. t animates the effect; each type caches its own output texture.
uintptr_t body_fx_preview_texture(BodyFXType type, float t);

// Free any GL textures cached for this mask dir (call when clip is deleted).
void body_fx_evict_mask_cache(const std::string& mask_dir);

// Drop the cached mjpeg seek table for a mask dir — call after the masks are wiped /
// regenerated so the render rebuilds byte offsets from the NEW file. A stale seek table
// indexes the old file's offsets into the new mjpeg → wrong byte ranges = smudge. No GL.
void body_fx_invalidate_mask_index(const std::string& mask_dir);
