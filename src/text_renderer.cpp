#include "text_renderer.h"
#include "text_anim.h"
#include <cmath>


// ── Typography font registry (hoisted from ui/theme.cpp) ─────────────────────
// The app's theme_apply() registers faces after building the atlas; the
// engine (text renderer, overlay renderer, typography pipeline) resolves
// faces by id. Storage engine-side so typo_font_get links into pms-engine.
ImFont* g_font_black = nullptr;   // assigned by the app after atlas build
ImFont* g_font_cjk   = nullptr;   // CJK-capable canvas face (app assigns after atlas build)
namespace {
    struct TypoFace { const char* name; ImFont* font; };
    std::vector<TypoFace> g_typo_faces;
}
void typo_font_clear() { g_typo_faces.clear(); }
void typo_font_register(const char* name, ImFont* font) {
    g_typo_faces.push_back({name, font});
}
ImFont* typo_font_get(const char* id) {
    if (id && *id)
        for (const auto& f : g_typo_faces)
            if (f.font && strcmp(f.name, id) == 0) return f.font;
    return g_font_black;   // default lyrics face / graceful fallback
}

// Does `s` contain CJK codepoints (kana, kanji, JP punctuation, fullwidth)?
// The Latin display faces have none of these, so such strings need the
// merged CJK face or every glyph renders blank.
static bool text_has_cjk(const char* s) {
    if (!s) return false;
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        unsigned cp = *p;
        int len = 1;
        if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; len = 4; }
        else if (*p < 0x80) { len = 1; }
        else return true;              // malformed UTF-8 — hand it to the CJK face
        if (len > 1 && p[1] != 0) {
            for (int i = 1; i < len; ++i) cp = (cp << 6) | (p[i] & 0x3F);
        }
        // CJK ideographs + compatibility, kana, CJK punctuation, fullwidth forms
        if ((cp >= 0x2E80 && cp <= 0x9FFF) ||
            (cp >= 0x3000 && cp <= 0x30FF) ||
            (cp >= 0xFF00 && cp <= 0xFFEF))
            return true;
        p += len;
    }
    return false;
}


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

// Width of `s` with letter-spacing `track` (fraction of fsz added after each
// glyph). track == 0 is the fast path (plain measure).
static float text_w_tracked(ImFont* f, float fsz, const char* s, float track) {
    float w = f->CalcTextSizeA(fsz, FLT_MAX, -1.f, s).x;
    if (track != 0.f) {
        int n = 0;
        for (const char* p = s; *p; ) { p += utf8_len((unsigned char)*p); ++n; }
        if (n > 1) w += track * fsz * (n - 1);
    }
    return w;
}

// Draw `s` glyph-by-glyph with letter-spacing. track == 0 → one AddText.
static void draw_text_tracked(ImDrawList* dl, ImFont* f, float fsz, float x, float y,
                              ImU32 col, const char* s, float track) {
    if (track == 0.f) { dl->AddText(f, fsz, {x, y}, col, s); return; }
    float cx = x;
    for (const char* p = s; *p; ) {
        int n = utf8_len((unsigned char)*p);
        char ch[5] = {0}; for (int k = 0; k < n; ++k) ch[k] = p[k];
        float gw = f->CalcTextSizeA(fsz, FLT_MAX, -1.f, ch).x;
        dl->AddText(f, fsz, {cx, y}, col, ch);
        cx += gw + track * fsz;
        p += n;
    }
}

static inline ImU32 lerp_rgb_keepa(ImU32 c1rgb, ImU32 c2rgb, float t, unsigned a) {
    if (t < 0.f) t = 0.f; if (t > 1.f) t = 1.f;
    int r = (int)(((c1rgb)      & 0xFF) * (1.f - t) + ((c2rgb)      & 0xFF) * t);
    int g = (int)(((c1rgb >> 8) & 0xFF) * (1.f - t) + ((c2rgb >> 8) & 0xFF) * t);
    int b = (int)(((c1rgb >> 16)& 0xFF) * (1.f - t) + ((c2rgb >> 16)& 0xFF) * t);
    return IM_COL32(r, g, b, a);
}

// ── ScratchRaw: render a glyph as hand-scratched hatch lines ──────────────────────
// Samples the font atlas to determine glyph coverage, then draws parallel
// hatch lines (vertical by default, horizontal for wide glyphs) only where
// the glyph has alpha. Additionally scatters 6-10 low-alpha scratch lines
// in an area slightly larger than the glyph bounding box — these overshoot
// the letter edges into surrounding space, adding raw "film scratch debris"
// energy without crowding the letterform. All strokes re-randomize per
// frame at 24fps for the "boiling" hand-scratched feel.
static float glyph_coverage_at(ImTextureData* tex, int x, int y, int bpp) {
    if (x < 0 || y < 0 || x >= tex->Width || y >= tex->Height) return 0.f;
    unsigned char* px = (unsigned char*)tex->GetPixelsAt(x, y);
    return (bpp == 1) ? (float)*px / 255.f : (float)px[3] / 255.f;
}

static void draw_scratch_glyph(ImDrawList* dl, ImFont* font, float es,
                                float ex, float ey, const char* utf8,
                                float alpha, ImU32 base_rgb, int gi, int frame_i) {
    // Decode first UTF-8 codepoint
    unsigned char c0 = (unsigned char)utf8[0];
    ImWchar cp;
    if (c0 < 0x80) cp = c0;
    else if ((c0 & 0xE0) == 0xC0)
        cp = ((ImWchar)(c0 & 0x1F) << 6) | ((ImWchar)(unsigned char)utf8[1] & 0x3F);
    else if ((c0 & 0xF0) == 0xE0)
        cp = ((ImWchar)(c0 & 0x0F) << 12) | ((ImWchar)(unsigned char)utf8[1] & 0x3F) << 6
             | ((ImWchar)(unsigned char)utf8[2] & 0x3F);
    else cp = c0;

    ImFontBaked* baked = font->GetFontBaked(es);
    if (!baked) return;
    ImFontGlyph* glyph = baked->FindGlyph(cp);
    if (!glyph || !glyph->Visible) return;

    ImTextureData* tex = font->OwnerAtlas->TexData;
    if (!tex || !tex->Pixels) return;
    int bpp = tex->BytesPerPixel;
    int texW = tex->Width, texH = tex->Height;

    // Glyph rect in atlas pixels
    int gx0 = (int)(glyph->U0 * texW), gy0 = (int)(glyph->V0 * texH);
    int gx1 = (int)(glyph->U1 * texW), gy1 = (int)(glyph->V1 * texH);
    int gw = gx1 - gx0, gh = gy1 - gy0;
    if (gw <= 2 || gh <= 2) return;

    // Glyph render rect on canvas
    float scale = es / baked->Size;
    float rx0 = ex + glyph->X0 * scale;
    float ry0 = ey + glyph->Y0 * scale;
    float rw = (glyph->X1 - glyph->X0) * scale;
    float rh = (glyph->Y1 - glyph->Y0) * scale;
    float sx = rw / (float)gw, sy = rh / (float)gh;

    // Adaptive direction: vertical by default, horizontal for wide glyphs
    bool horizontal = (float)gw > (float)gh * 1.3f;

    const float thresh = 0.2f;
    const int spacing = 3;       // atlas px between hatch lines
    ImU32 sc = (base_rgb & 0x00FFFFFF) | ((unsigned)(alpha * 255) << 24);

    // ── Interior hatch ───────────────────────────────────────────────────
    // Walk parallel lines through the glyph. For each line, find covered
    // segments and draw them with rough strokes. 3px spacing fills the
    // letter shape enough to read; jitter + gaps add the hand-scratched feel.
    if (horizontal) {
        for (int y = 0; y < gh; y += spacing) {
            int segStart = -1;
            for (int x = 0; x <= gw; ++x) {
                float c = (x < gw) ? glyph_coverage_at(tex, gx0 + x, gy0 + y, bpp) : 0.f;
                if (c > thresh) {
                    if (segStart < 0) segStart = x;
                } else {
                    if (segStart >= 0) {
                        int si = y / spacing;
                        if (hash01(gi * 17 + si, frame_i) > 0.08f) {
                            float thick = 1.f + hash01(gi * 31 + si, frame_i + 13) * 1.f;
                            float jy = (hash01(gi * 43 + si, frame_i + 27) - 0.5f) * 2.f;
                            dl->AddLine({rx0 + segStart * sx, ry0 + y * sy + jy},
                                        {rx0 + (x - 1) * sx, ry0 + y * sy + jy}, sc, thick);
                        }
                        segStart = -1;
                    }
                }
            }
        }
    } else {
        for (int x = 0; x < gw; x += spacing) {
            int segStart = -1;
            for (int y = 0; y <= gh; ++y) {
                float c = (y < gh) ? glyph_coverage_at(tex, gx0 + x, gy0 + y, bpp) : 0.f;
                if (c > thresh) {
                    if (segStart < 0) segStart = y;
                } else {
                    if (segStart >= 0) {
                        int si = x / spacing;
                        if (hash01(gi * 17 + si, frame_i) > 0.08f) {
                            float thick = 1.f + hash01(gi * 31 + si, frame_i + 13) * 1.f;
                            float jx = (hash01(gi * 43 + si, frame_i + 27) - 0.5f) * 2.f;
                            dl->AddLine({rx0 + x * sx + jx, ry0 + segStart * sy},
                                        {rx0 + x * sx + jx, ry0 + (y - 1) * sy}, sc, thick);
                        }
                        segStart = -1;
                    }
                }
            }
        }
    }

    // ── Scattered low-alpha debris scratches ─────────────────────────────
    // 6-10 short faint lines in an area slightly larger than the glyph
    // bounding box. These overshoot the letter edges into surrounding space,
    // adding raw "film scratch debris" energy. Re-randomized every frame.
    float pad = fmaxf(rw, rh) * 0.15f;   // overshoot area
    int n_debris = 6 + (int)(hash01(gi, frame_i + 99) * 5.f);
    for (int di = 0; di < n_debris; ++di) {
        float dx = (hash01(gi * 61 + di, frame_i + 3) - 0.5f) * (rw + pad * 2.f);
        float dy = (hash01(gi * 67 + di, frame_i + 11) - 0.5f) * (rh + pad * 2.f);
        float cx = rx0 + rw * 0.5f + dx;
        float cy = ry0 + rh * 0.5f + dy;
        float ang = (hash01(gi * 73 + di, frame_i + 23) - 0.5f) * 1.2f;
        float len = fmaxf(rw, rh) * (0.1f + hash01(gi * 79 + di, frame_i + 37) * 0.25f);
        float da = (0.2f + hash01(gi * 83 + di, frame_i + 41) * 0.2f) * alpha;
        ImU32 dc = (base_rgb & 0x00FFFFFF) | ((unsigned)(da * 255) << 24);
        float thick = 0.5f + hash01(gi * 89 + di, frame_i + 53) * 1.f;
        dl->AddLine({cx, cy},
                    {cx + cosf(ang) * len, cy + sinf(ang) * len}, dc, thick);
    }
}

void render_text_block(TextRenderCtx ctx, const std::vector<std::string>& lines) {
    if (lines.empty()) return;

    ImDrawList* dl        = ctx.dl;
    ImFont*     font      = ctx.font;
    float       fsz       = ctx.fsz;
    float       alpha     = ctx.anim_alpha;
    float       anim_dx   = ctx.anim_dx;
    const Clip* clip      = ctx.clip;
    const TextStyle& ts   = clip->ts;

    // Japanese text (or any CJK string) has no glyphs in the Latin display
    // faces — swap in the CJK-capable face so it renders instead of blank.
    // Mixed blocks (Latin + kana) render fully from the CJK face, which also
    // carries Latin. Measured widths below use the same font, so layout stays
    // consistent with what's actually drawn.
    if (g_font_cjk) {
        for (const auto& ln : lines)
            if (text_has_cjk(ln.c_str())) { font = g_font_cjk; break; }
    }

    float track = clip->tracking;

    // Pre-compute per-line x positions and block width (letter-spacing aware).
    std::vector<float> lwidths(lines.size());
    float block_max_w = 0.f;
    for (size_t i = 0; i < lines.size(); ++i) {
        lwidths[i] = text_w_tracked(font, fsz, lines[i].c_str(), track);
        if (lwidths[i] > block_max_w) block_max_w = lwidths[i];
    }

    // Keep the resting block on-canvas. An edge/random anchor (e.g. the rave
    // preset's per-word positions) on a wide word would otherwise spill off the
    // frame — block_cx scales with the canvas but never accounted for the text's
    // own width. Shift block_cx minimally so the block's bbox fits inside
    // [canvas_x0, canvas_x0+canvas_w]; per-element animation (anim_dx) is applied
    // afterward and can still travel off-frame intentionally. Text wider than the
    // canvas is centred (symmetric overflow is unavoidable).
    if (ctx.canvas_w > 0.f && block_max_w > 0.f) {
        float left = (ctx.anchor_h == 0) ? ctx.block_cx
                   : (ctx.anchor_h == 2) ? ctx.block_cx - block_max_w
                                         : ctx.block_cx - block_max_w * 0.5f;
        float right = left + block_max_w;
        // Shortform safe zone: TikTok/Reels/IG zoom-crop the frame to fill and lay
        // UI over the edges, so inset the clamp bounds instead of pinning flush to
        // the literal edge — text at the boundary still survives on-platform.
        float margin = ctx.canvas_w * 0.05f;   // 5% each side; bump if clips still clip
        float lo = ctx.canvas_x0 + margin, hi = ctx.canvas_x0 + ctx.canvas_w - margin;
        if (block_max_w <= hi - lo) {
            if (left < lo)       ctx.block_cx += lo - left;
            else if (right > hi) ctx.block_cx += hi - right;
        } else {
            ctx.block_cx += (lo + hi) * 0.5f - (left + right) * 0.5f;
        }
    }

    float block_h = lines.size() * ctx.line_h;

    // Vertical safe zone: the top handle/tabs and (taller) the bottom caption
    // strip eat into shortform frames, so keep the block out of those bands too.
    // ty already includes anim_dy (the caller folds it in), so this clamps the
    // displayed Y — text won't even dip into the caption mid-animation.
    if (ctx.canvas_h > 0.f) {
        float top_m = ctx.canvas_h * 0.08f;   // 8%  top    (handle / tabs / status)
        float bot_m = ctx.canvas_h * 0.15f;   // 15% bottom (caption strip is taller)
        float lo = ctx.canvas_y0 + top_m, hi = ctx.canvas_y0 + ctx.canvas_h - bot_m;
        if (block_h <= hi - lo) {
            if (ctx.ty < lo)                ctx.ty = lo;
            else if (ctx.ty + block_h > hi) ctx.ty = hi - block_h;
        } else {
            ctx.ty = (lo + hi) * 0.5f - block_h * 0.5f;   // taller than zone — centre
        }
    }

    bool has_karaoke = ctx.clip_words && !ctx.clip_words->empty() && clip->karaoke;

    // Per-element (per-word / per-letter) kinetic mode: each word or glyph runs
    // its own staggered motion. Requires an actual animation style and rules out
    // karaoke (which owns the per-word path) and the Block background style.
    bool per_elem = (clip->anim_unit != 0 ||
                     ctx.eff_style == AnimStyle::ScratchFilm ||
                     ctx.eff_style == AnimStyle::ScratchRaw) &&
                    !has_karaoke &&
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
            draw_text_tracked(dl, font, fsz, lx + ts.shadow_ox, ly + ts.shadow_oy, scol, lines[li].c_str(), track);
        }
    }

    // Vertex range where the fill is emitted — a non-zero grad_mode recolours it
    // afterwards (post-process, same trick as the rotation spin) for gradient
    // text fill without per-glyph drawing.
    int vtx_grad0 = (clip->grad_mode > 0) ? dl->VtxBuffer.Size : -1;

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
            // ── ScratchRaw: letters ARE scratches — render glyph as
            // hand-scratched hatch lines, no normal text fill ─────────────
            if (ctx.eff_style == AnimStyle::ScratchRaw) {
                int frame_i = (int)(local_t * 24.f);
                draw_scratch_glyph(dl, font, es, ex, ey, s.c_str(),
                                   a, base_rgb, gi, frame_i);
                return;
            }
            if (ts.shadow_enabled)
                dl->AddText(font, es, {ex + ts.shadow_ox, ey + ts.shadow_oy},
                            col_f4_alpha(ts.shadow_col, a), s.c_str());
            if (ts.stroke_enabled)
                add_text_ring(dl, font, es, ex, ey, ts.stroke_w,
                              col_f4_alpha(ts.stroke_col, a), s.c_str());
            dl->AddText(font, es, {ex, ey}, col, s.c_str());
            // ── Scratch-on-film overlay ───────────────────────────────
            if (ctx.eff_style == AnimStyle::ScratchFilm) {
                int frame_i = (int)(local_t * 24.f);
                float gw = ws;          // letter glyph width
                float gh = es * 1.2f;   // letter glyph height (approx)
                dl->PushClipRect({ex, ey}, {ex + gw, ey + gh}, true);
                int n_scratch = 3 + (int)(hash01(gi, frame_i + 99) * 4.f);
                for (int si = 0; si < n_scratch; ++si) {
                    float sy  = hash01(gi * 17 + si, frame_i) * gh;
                    float sx0 = hash01(gi * 31 + si, frame_i + 13) * gw;
                    float ang = (hash01(gi * 43 + si, frame_i + 27) - 0.5f) * 0.6f;
                    float len = gw * (0.3f + hash01(gi * 53 + si, frame_i + 41) * 0.7f);
                    float dx2 = cosf(ang) * len;
                    float dy2 = sinf(ang) * len;
                    ImU32 sc = IM_COL32(0, 0, 0, (unsigned)(200.f * a));
                    dl->AddLine({ex + sx0, ey + sy},
                                {ex + sx0 + dx2, ey + sy + dy2}, sc, 1.5f);
                }
                dl->PopClipRect();
            }
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
            // Highlight (sung) and base (unsung) colours.
            float hl[4]; for (int k=0;k<4;++k) hl[k] = clip->karaoke_highlight_color[k];
            float bs[4];
            if (clip->sub_color_override) { for (int k=0;k<4;++k) bs[k]=clip->sub_color[k]; }
            else { bs[0]=bs[1]=bs[2]=1.f; bs[3]=0.4f; }
            auto colf = [&](const float c[4], float am) {
                return IM_COL32((int)(c[0]*255),(int)(c[1]*255),(int)(c[2]*255),(int)(c[3]*am*255)); };

            while (*lp) {
                const char* ep = lp;
                while (*ep && *ep != ' ') ++ep;
                std::string lword(lp, ep);
                bool has_space  = (*ep == ' ');
                std::string lword_sp = lword + (has_space ? " " : "");

                bool  is_active = false;
                float wprog = 0.f;   // 0..1 progress through the active word
                if (kw_idx < (int)ctx.clip_words->size()) {
                    const WordEntry* we = (*ctx.clip_words)[kw_idx];
                    is_active = (ctx.t >= we->start && ctx.t < we->end);
                    bool sung = ctx.t >= we->end;
                    if (is_active && we->end > we->start)
                        wprog = (ctx.t - we->start) / (we->end - we->start);
                    else if (sung) wprog = 1.f;
                    ++kw_idx;
                    float word_w = font->CalcTextSizeA(fsz, FLT_MAX, -1.f, lword_sp.c_str()).x;

                    if (clip->karaoke_mode == 2) {
                        // Pop: active word scales up + lifts; base/sung swap colour.
                        float s = is_active ? 1.f + 0.18f * sinf(wprog * 3.14159f) : 1.f;
                        float es = fsz * s;
                        float ws = font->CalcTextSizeA(es, FLT_MAX, -1.f, lword_sp.c_str()).x;
                        float ox = (word_w - ws) * 0.5f;
                        float oy = is_active ? -ctx.line_h * 0.12f * sinf(wprog * 3.14159f) : 0.f;
                        // Active or already-sung words stay lit; upcoming stay dim.
                        ImU32 c = (is_active || wprog >= 1.f) ? colf(hl, alpha) : colf(bs, alpha);
                        dl->AddText(font, es, {cur_x + ox, ly + oy}, c, lword_sp.c_str());
                    } else if (clip->karaoke_mode == 1) {
                        // Fill-wipe: base colour underneath, highlight clipped to a
                        // left-to-right wipe across the active word over its span.
                        dl->AddText(font, fsz, {cur_x, ly}, colf(bs, alpha), lword_sp.c_str());
                        float fillw = (ctx.t >= 0.f && wprog > 0.f) ? word_w * wprog : 0.f;
                        if (wprog >= 1.f) fillw = word_w + fsz;   // fully sung
                        if (fillw > 0.f) {
                            dl->PushClipRect({cur_x, ly - fsz}, {cur_x + fillw, ly + fsz * 1.6f}, true);
                            dl->AddText(font, fsz, {cur_x, ly}, colf(hl, alpha), lword_sp.c_str());
                            dl->PopClipRect();
                        }
                    } else {
                        // Mode 0: plain colour swap (active = highlight, else base).
                        ImU32 c = is_active ? colf(hl, alpha)
                                : (wprog >= 1.f ? colf(hl, alpha) : colf(bs, alpha));
                        dl->AddText(font, fsz, {cur_x, ly}, c, lword_sp.c_str());
                    }
                    cur_x += word_w;
                } else {
                    float word_w = font->CalcTextSizeA(fsz, FLT_MAX, -1.f, lword_sp.c_str()).x;
                    dl->AddText(font, fsz, {cur_x, ly}, colf(bs, alpha), lword_sp.c_str());
                    cur_x += word_w;
                }
                lp = has_space ? ep + 1 : ep;
            }
        } else {
            ImU32 tcol;
            if (clip->sub_color_override) {
                float a = clip->sub_color[3] * alpha;
                tcol = IM_COL32((int)(clip->sub_color[0]*255), (int)(clip->sub_color[1]*255),
                                 (int)(clip->sub_color[2]*255), (int)(a*255));
            } else if (ctx.eff_style == AnimStyle::Block) {
                // App-theme background color, inlined: Block style knocks the
                // text out of its plate. (Was to_u32(Col::bg) — theme.h is
                // app-side; the engine renders with the literal, Col::bg = black.)
                tcol = IM_COL32(0, 0, 0, 255);
            } else {
                tcol = IM_COL32(255, 255, 255, (int)(255.f * alpha));
            }
            draw_text_tracked(dl, font, fsz, lx, ly, tcol, lines[li].c_str(), track);
        }
    }

    // ── Gradient fill (post-process the fill vertices) ─────────────────────────
    if (vtx_grad0 >= 0 && vtx_grad0 < dl->VtxBuffer.Size) {
        ImU32 c1 = clip->sub_color_override
            ? IM_COL32((int)(clip->sub_color[0]*255),(int)(clip->sub_color[1]*255),(int)(clip->sub_color[2]*255),255)
            : IM_COL32(255,255,255,255);
        ImU32 c2 = IM_COL32((int)(clip->grad_col2[0]*255),(int)(clip->grad_col2[1]*255),
                            (int)(clip->grad_col2[2]*255),255);
        float top = ctx.ty, bh = fmaxf(1.f, block_h);
        float left = ctx.block_cx - block_max_w * 0.5f, span = fmaxf(1.f, block_max_w + bh);
        for (int i = vtx_grad0; i < dl->VtxBuffer.Size; ++i) {
            ImDrawVert& v = dl->VtxBuffer[i];
            unsigned a = (v.col >> IM_COL32_A_SHIFT) & 0xFF;
            if (a == 0) continue;
            float tg;
            if (clip->grad_mode == 2)        tg = ((v.pos.x - left) + (v.pos.y - top)) / span;
            else if (clip->grad_mode == 3) { // hue cycle by x + time
                float hh = (v.pos.x - left) / fmaxf(1.f, block_max_w) + ctx.t * 0.15f;
                float r,g,b; ImGui::ColorConvertHSVtoRGB(hh - floorf(hh), 0.85f, 1.f, r,g,b);
                v.col = IM_COL32((int)(r*255),(int)(g*255),(int)(b*255), a); continue;
            }
            else                              tg = (v.pos.y - top) / bh;   // vertical
            v.col = lerp_rgb_keepa(c1, c2, tg, a);
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
