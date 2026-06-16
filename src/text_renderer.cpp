#include "text_renderer.h"
#include "text_anim.h"
#include "ui/theme.h"
#include <cmath>

static ImU32 col_f4_alpha(const float c[4], float extra_alpha) {
    float a = c[3] * extra_alpha;
    return IM_COL32((int)(c[0]*255), (int)(c[1]*255), (int)(c[2]*255), (int)(a*255));
}

// Draw text at 8 angular offsets around (cx, cy) at radius r.
static void add_text_ring(ImDrawList* dl, ImFont* font, float fsz,
                          float cx, float cy, float r, ImU32 col, const char* text) {
    static const float kAngles[8] = {0.f, 0.7854f, 1.5708f, 2.3562f,
                                     3.1416f, 3.9270f, 4.7124f, 5.4978f};
    for (float ang : kAngles) {
        float ox = r * cosf(ang);
        float oy = r * sinf(ang);
        dl->AddText(font, fsz, {cx + ox, cy + oy}, col, text);
    }
}

// Line-level x position given anchor and line width.
static float line_x(int anchor_h, float block_cx, float lw, float anim_dx) {
    if (anchor_h == 0) return block_cx + anim_dx;
    if (anchor_h == 2) return block_cx - lw + anim_dx;
    return block_cx - lw * 0.5f + anim_dx;
}

// Bytes in the UTF-8 codepoint beginning at lead byte c.
static int utf8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6)  return 2;
    if ((c >> 4) == 0xE)  return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}

void render_text_block(const TextRenderCtx& ctx, const std::vector<std::string>& lines) {
    if (lines.empty()) return;

    ImDrawList* dl        = ctx.dl;
    ImFont*     font      = ctx.font;
    float       fsz       = ctx.fsz;
    float       alpha     = ctx.anim_alpha;
    float       anim_dx   = ctx.anim_dx;
    const Clip* clip      = ctx.clip;
    const TextStyle& ts   = clip->ts;

    // Pre-compute per-line x positions and block width.
    std::vector<float> lwidths(lines.size());
    float block_max_w = 0.f;
    for (size_t i = 0; i < lines.size(); ++i) {
        lwidths[i] = font->CalcTextSizeA(fsz, FLT_MAX, -1.f, lines[i].c_str()).x;
        if (lwidths[i] > block_max_w) block_max_w = lwidths[i];
    }

    float block_h = lines.size() * ctx.line_h;

    bool has_karaoke = ctx.clip_words && !ctx.clip_words->empty() && clip->karaoke;

    // Per-element (per-word / per-letter) kinetic mode: each word or glyph runs
    // its own staggered motion. Requires an actual animation style and rules out
    // karaoke (which owns the per-word path) and the Block background style.
    bool per_elem = clip->anim_unit != 0 && !has_karaoke &&
                    ctx.eff_style != AnimStyle::None &&
                    ctx.eff_style != AnimStyle::Block;

    // Record the vertex range so a non-zero clip rotation can spin the whole
    // composed block (glow + shadow + fill + outline) around its centre after
    // it's drawn — ImGui's AddText can't rotate, but rotating the emitted
    // glyph quads gives the same result for a uniform block rotation.
    int vtx_rot0 = (fabsf(ctx.rotation) > 0.01f) ? dl->VtxBuffer.Size : -1;

    // ── 1. Glow (block only) ───────────────────────────────────────────────────
    if (ts.glow_enabled && !per_elem) {
        for (int pass = 0; pass < 3; ++pass) {
            float r         = ts.glow_r * (pass + 1) / 3.f;
            float pass_a    = ts.glow_col[3] * (1.f - pass / 3.f) * 0.5f * alpha;
            ImU32 gcol      = IM_COL32((int)(ts.glow_col[0]*255),
                                       (int)(ts.glow_col[1]*255),
                                       (int)(ts.glow_col[2]*255),
                                       (int)(pass_a*255));
            for (size_t li = 0; li < lines.size(); ++li) {
                float lx = line_x(ctx.anchor_h, ctx.block_cx, lwidths[li], anim_dx);
                float ly = ctx.ty + li * ctx.line_h;
                add_text_ring(dl, font, fsz, lx, ly, r, gcol, lines[li].c_str());
            }
        }
    }

    // ── 2. Background box ─────────────────────────────────────────────────────
    bool draw_bg = ts.bg_enabled || (ctx.eff_style == AnimStyle::Block);
    if (draw_bg) {
        float pad_x = ts.bg_pad_x, pad_y = ts.bg_pad_y;
        float bx_anchor;
        if (ctx.anchor_h == 0)      bx_anchor = ctx.block_cx;
        else if (ctx.anchor_h == 2) bx_anchor = ctx.block_cx - block_max_w;
        else                        bx_anchor = ctx.block_cx - block_max_w * 0.5f;
        float bx0 = bx_anchor - pad_x + anim_dx;
        float bx1 = bx_anchor + block_max_w + pad_x + anim_dx;
        ImU32 bgcol = IM_COL32((int)(ts.bg_col[0]*255), (int)(ts.bg_col[1]*255),
                                (int)(ts.bg_col[2]*255), (int)(ts.bg_col[3]*255));
        dl->AddRectFilled({bx0, ctx.ty - pad_y},
                          {bx1, ctx.ty + block_h + pad_y},
                          bgcol, ts.bg_corner);
    }

    // ── 3. Shadow (block only) ─────────────────────────────────────────────────
    if (ts.shadow_enabled && !per_elem) {
        ImU32 scol = col_f4_alpha(ts.shadow_col, alpha);
        for (size_t li = 0; li < lines.size(); ++li) {
            float lx = line_x(ctx.anchor_h, ctx.block_cx, lwidths[li], anim_dx);
            float ly = ctx.ty + li * ctx.line_h;
            dl->AddText(font, fsz, {lx + ts.shadow_ox, ly + ts.shadow_oy}, scol, lines[li].c_str());
        }
    }

    // ── 4. Stroke (block only) ─────────────────────────────────────────────────
    if (ts.stroke_enabled && !per_elem) {
        ImU32 stkcol = col_f4_alpha(ts.stroke_col, alpha);
        for (size_t li = 0; li < lines.size(); ++li) {
            float lx = line_x(ctx.anchor_h, ctx.block_cx, lwidths[li], anim_dx);
            float ly = ctx.ty + li * ctx.line_h;
            add_text_ring(dl, font, fsz, lx, ly, ts.stroke_w, stkcol, lines[li].c_str());
        }
    }

    // ── 5a. Per-element kinetic path ───────────────────────────────────────────
    if (per_elem) {
        float local_t  = ctx.t - clip->start;
        float clip_dur = clip->end - clip->start;
        float fade_in  = fminf(0.25f, clip_dur * 0.3f);
        float fade_out = fminf(0.25f, clip_dur * 0.2f);
        bool  by_word  = (clip->anim_unit == 1);

        // Count animated elements (words, or non-space glyphs) for index/total.
        int total = 0;
        for (auto& ln : lines) {
            if (by_word) {
                const char* lp = ln.c_str();
                while (*lp) {
                    while (*lp == ' ') ++lp;
                    if (!*lp) break;
                    while (*lp && *lp != ' ') ++lp;
                    ++total;
                }
            } else {
                const char* lp = ln.c_str();
                while (*lp) { int n = utf8_len((unsigned char)*lp); if (*lp != ' ') ++total; lp += n; }
            }
        }
        if (total < 1) total = 1;

        ImU32 base_rgb = clip->sub_color_override
            ? IM_COL32((int)(clip->sub_color[0]*255), (int)(clip->sub_color[1]*255),
                       (int)(clip->sub_color[2]*255), 255)
            : IM_COL32(255, 255, 255, 255);

        auto draw_elem = [&](const std::string& s, float bx, float by, int gi) {
            ElemAnim ea = compute_elem_anim(ctx.eff_style, local_t, clip_dur,
                                            fade_in, fade_out, ctx.canvas_w,
                                            clip->ease, gi, total,
                                            clip->anim_stagger, ctx.line_h);
            float a = alpha * ea.alpha;
            if (a <= 0.003f) return;
            float es = fsz * ea.scale;
            float w0 = font->CalcTextSizeA(fsz, FLT_MAX, -1.f, s.c_str()).x;
            float ws = font->CalcTextSizeA(es,  FLT_MAX, -1.f, s.c_str()).x;
            float ex = bx + (w0 - ws) * 0.5f + ea.dx;
            float ey = by + (ctx.line_h - ctx.line_h * ea.scale) * 0.5f + ea.dy;
            ImU32 col = (base_rgb & 0x00FFFFFF) | ((unsigned)(a * 255) << 24);
            if (clip->sub_color_override) {
                float ca = clip->sub_color[3] * a;
                col = (base_rgb & 0x00FFFFFF) | ((unsigned)(ca * 255) << 24);
            }
            if (ts.shadow_enabled)
                dl->AddText(font, es, {ex + ts.shadow_ox, ey + ts.shadow_oy},
                            col_f4_alpha(ts.shadow_col, a), s.c_str());
            if (ts.stroke_enabled)
                add_text_ring(dl, font, es, ex, ey, ts.stroke_w,
                              col_f4_alpha(ts.stroke_col, a), s.c_str());
            dl->AddText(font, es, {ex, ey}, col, s.c_str());
        };

        int gi = 0;
        for (size_t li = 0; li < lines.size(); ++li) {
            float lx = line_x(ctx.anchor_h, ctx.block_cx, lwidths[li], anim_dx);
            float ly = ctx.ty + li * ctx.line_h;
            const std::string& ln = lines[li];
            float cur_x = lx;
            if (by_word) {
                const char* lp = ln.c_str();
                while (*lp) {
                    const char* sp = lp;
                    while (*sp == ' ') ++sp;
                    float lead = font->CalcTextSizeA(fsz, FLT_MAX, -1.f,
                                    std::string(lp, sp).c_str()).x;
                    cur_x += lead;
                    if (!*sp) break;
                    const char* ep = sp;
                    while (*ep && *ep != ' ') ++ep;
                    std::string word(sp, ep);
                    float ww = font->CalcTextSizeA(fsz, FLT_MAX, -1.f, word.c_str()).x;
                    draw_elem(word, cur_x, ly, gi++);
                    cur_x += ww;
                    lp = ep;
                }
            } else {
                const char* lp = ln.c_str();
                while (*lp) {
                    int n = utf8_len((unsigned char)*lp);
                    std::string ch(lp, lp + n);
                    float cw = font->CalcTextSizeA(fsz, FLT_MAX, -1.f, ch.c_str()).x;
                    if (*lp != ' ') draw_elem(ch, cur_x, ly, gi++);
                    cur_x += cw;
                    lp += n;
                }
            }
        }
    }

    // ── 5b. Block / karaoke path ───────────────────────────────────────────────
    int kw_idx = 0;
    if (!per_elem)
    for (size_t li = 0; li < lines.size(); ++li) {
        float lx = line_x(ctx.anchor_h, ctx.block_cx, lwidths[li], anim_dx);
        float ly = ctx.ty + li * ctx.line_h;

        if (has_karaoke) {
            const char* lp  = lines[li].c_str();
            float       cur_x = lx;
            while (*lp) {
                const char* ep = lp;
                while (*ep && *ep != ' ') ++ep;
                std::string lword(lp, ep);
                bool has_space  = (*ep == ' ');
                std::string lword_sp = lword + (has_space ? " " : "");

                bool is_active = false;
                if (kw_idx < (int)ctx.clip_words->size()) {
                    const WordEntry* we = (*ctx.clip_words)[kw_idx];
                    is_active = (ctx.t >= we->start && ctx.t < we->end);
                    ++kw_idx;
                }

                ImU32 wcol;
                if (clip->sub_color_override) {
                    float a = (is_active ? clip->sub_color[3] : clip->sub_color[3] * 0.45f) * alpha;
                    wcol = IM_COL32((int)(clip->sub_color[0]*255), (int)(clip->sub_color[1]*255),
                                    (int)(clip->sub_color[2]*255), (int)(a*255));
                } else if (is_active) {
                    ImU32 hcol = IM_COL32((int)(clip->karaoke_highlight_color[0]*255),
                                          (int)(clip->karaoke_highlight_color[1]*255),
                                          (int)(clip->karaoke_highlight_color[2]*255),
                                          (int)(clip->karaoke_highlight_color[3]*alpha*255));
                    wcol = hcol;
                } else {
                    wcol = IM_COL32(255, 255, 255, (int)(100 * alpha));
                }

                float word_w = font->CalcTextSizeA(fsz, FLT_MAX, -1.f, lword_sp.c_str()).x;
                dl->AddText(font, fsz, {cur_x, ly}, wcol, lword_sp.c_str());
                cur_x += word_w;
                lp = has_space ? ep + 1 : ep;
            }
        } else {
            ImU32 tcol;
            if (clip->sub_color_override) {
                float a = clip->sub_color[3] * alpha;
                tcol = IM_COL32((int)(clip->sub_color[0]*255), (int)(clip->sub_color[1]*255),
                                 (int)(clip->sub_color[2]*255), (int)(a*255));
            } else if (ctx.eff_style == AnimStyle::Block) {
                tcol = to_u32(Col::bg);
            } else {
                tcol = IM_COL32(255, 255, 255, (int)(255.f * alpha));
            }
            dl->AddText(font, fsz, {lx, ly}, tcol, lines[li].c_str());
        }
    }

    // Spin the whole emitted block around its centre by the clip rotation.
    if (vtx_rot0 >= 0 && vtx_rot0 < dl->VtxBuffer.Size) {
        float cx = ctx.anchor_h == 0 ? ctx.block_cx + block_max_w * 0.5f
                 : ctx.anchor_h == 2 ? ctx.block_cx - block_max_w * 0.5f
                                     : ctx.block_cx;
        float cy = ctx.ty + block_h * 0.5f;
        float rad = ctx.rotation * 3.14159265f / 180.f;
        float cs = cosf(rad), sn = sinf(rad);
        for (int i = vtx_rot0; i < dl->VtxBuffer.Size; ++i) {
            ImVec2& p = dl->VtxBuffer[i].pos;
            float dx = p.x - cx, dy = p.y - cy;
            p.x = cx + dx * cs - dy * sn;
            p.y = cy + dx * sn + dy * cs;
        }
    }
}
