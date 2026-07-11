#pragma once
#include "app.h"
#include <imgui.h>
#include <vector>
#include <string>

struct TextRenderCtx {
    ImDrawList* dl;
    ImFont*     font;
    float       fsz;
    float       anim_alpha;
    float       anim_dx, anim_dy;
    const Clip* clip;
    AnimStyle   eff_style;
    int         anchor_h;   // 0=left 1=center 2=right
    float       block_cx;   // horizontal anchor in canvas px
    float       ty;         // top-y of first line (already animated)
    float       line_h;
    float       t;          // playhead / render time
    float       rotation;   // clip rotation in degrees (rotates the whole block)
    float       canvas_w = 0.f;  // canvas width px (per-element slide distance)
    float       canvas_x0 = 0.f; // canvas left edge, same px space as block_cx
    float       canvas_h = 0.f;  // canvas height px (0 = no vertical safe-zone clamp)
    float       canvas_y0 = 0.f; // canvas top edge, same px space as ty
    // Non-null when clip->karaoke is active
    const std::vector<const WordEntry*>* clip_words;
};

// Takes ctx BY VALUE: it clamps block_cx so the resting block stays on-canvas.
void render_text_block(TextRenderCtx ctx, const std::vector<std::string>& lines);
