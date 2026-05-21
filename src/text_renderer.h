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
    // Non-null when clip->karaoke is active
    const std::vector<const WordEntry*>* clip_words;
};

void render_text_block(const TextRenderCtx& ctx, const std::vector<std::string>& lines);
