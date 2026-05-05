#pragma once
#include <cstdint>

void      fx_shader_init();
void      fx_shader_shutdown();

// Apply color grade + Gaussian blur to src_tex (GL texture ID).
// Writes into an internal ping-pong FBO pair and returns the output texture ID.
// Returns src_tex unchanged when no effects are active.
uintptr_t fx_apply(uintptr_t src_tex,
                   float brightness, float contrast,
                   float saturation, float hue_deg,
                   float blur_sigma);

// Creative FX — each writes into a dedicated FBO and returns that texture.
// Returns src_tex unchanged when params are at neutral.
uintptr_t fx_apply_glitch(uintptr_t src_tex, float chroma_px, float jitter, float time);
uintptr_t fx_apply_vhs   (uintptr_t src_tex, float noise, float bleed_px, float tracking, float time);
