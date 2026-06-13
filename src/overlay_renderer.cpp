#include "overlay_renderer.h"
#include "app.h"
#include "ui/theme.h"
#include "ui/pipeline.h"  // SAFE_TOP, SAFE_BOT
#include "text_renderer.h"

#include <imgui.h>
#include <cmath>
#include <vector>
#include <string>

extern ImFont* g_font_black;

void draw_text_overlays(ImDrawList* dl, const AppState& state, float t,
                        ImVec2 p, float w, float h)
{
    if (state.tracks.empty()) return;

    // If any Lyrics clip is active at time t, suppress Subtitle clips.
    // This prevents the auto-generated subtitle track from double-rendering
    // when the user has applied typography on top of it.
    bool lyrics_active = false;
    for (auto& track : state.tracks) {
        if (!track.visible) continue;
        for (auto& cl : track.clips) {
            if (cl.clip_type == ClipType::Lyrics && t >= cl.start && t < cl.end)
                { lyrics_active = true; break; }
        }
        if (lyrics_active) break;
    }

    int text_rendered = 0;

    for (int ti = (int)state.tracks.size() - 1; ti >= 0; --ti) {
        const auto& track = state.tracks[ti];
        if (!track.visible) continue;

        const Clip* active = nullptr;
        for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
            auto ct = track.clips[ci].clip_type;
            if (ct != ClipType::Text && ct != ClipType::Lyrics && ct != ClipType::Subtitle)
                continue;
            // Suppress subtitle track when lyrics are present at this time.
            if (ct == ClipType::Subtitle && lyrics_active) continue;
            if (t >= track.clips[ci].start && t < track.clips[ci].end) {
                active = &track.clips[ci];
                break;
            }
        }
        if (!active) {
            // Only count this track toward the stacking offset when it actually
            // contains text/lyrics/subtitle clips — mirrors canvas.cpp's logic.
            bool has_text = false;
            for (auto& c : track.clips) {
                auto ct = c.clip_type;
                if (ct == ClipType::Text || ct == ClipType::Lyrics || ct == ClipType::Subtitle)
                    { has_text = true; break; }
            }
            if (has_text) ++text_rendered;
            continue;
        }

        ImFont* txt_font = g_font_black;
        // font_size is stored as a fraction of canvas height (0 = use default).
        // Default fraction chosen so the text looks the same proportion in the
        // preview canvas and the full-res FBO — do NOT use a fixed pixel size.
        static constexpr float kDefaultFontFrac = 0.055f;
        float fsz = active->font_size > 0.f ? active->font_size * h
                                            : h * kDefaultFontFrac;
        float line_h = fsz * 1.25f;

        float max_line_w = fmaxf(40.f, active->sub_wrap_w * w);
        std::vector<std::string> txt_lines;
        {
            const char* src = active->text.c_str();
            const char* wp  = src;
            std::string cur;
            while (true) {
                const char* ep = wp;
                while (*ep && *ep != ' ') ++ep;
                std::string word(wp, ep);
                std::string test = cur.empty() ? word : cur + " " + word;
                if (!cur.empty() &&
                    txt_font->CalcTextSizeA(fsz, FLT_MAX, -1.f, test.c_str()).x > max_line_w) {
                    txt_lines.push_back(cur);
                    cur = word;
                } else {
                    cur = test;
                }
                if (!*ep) break;
                wp = ep + 1;
            }
            if (!cur.empty()) txt_lines.push_back(cur);
            if (txt_lines.empty()) txt_lines.push_back("");
        }

        // Post-wrap font scale: ensure no rendered line overflows the canvas given the anchor.
        // Mirrors canvas.cpp lines ~1052-1073.
        {
            float max_fit_w;
            if (active->sub_anchor_h == 0)
                max_fit_w = (1.f - active->sub_pos_x - SAFE_SIDE) * w;
            else if (active->sub_anchor_h == 2)
                max_fit_w = (active->sub_pos_x - SAFE_SIDE) * w;
            else
                max_fit_w = 2.f * fminf(active->sub_pos_x - SAFE_SIDE,
                                        1.f - active->sub_pos_x - SAFE_SIDE) * w;
            max_fit_w = fminf(max_fit_w, max_line_w);   // also cap at wrap column
            max_fit_w = fmaxf(max_fit_w, 40.f);          // safety floor

            float max_rendered_w = 0.f;
            for (auto& ln : txt_lines)
                max_rendered_w = fmaxf(max_rendered_w,
                    txt_font->CalcTextSizeA(fsz, FLT_MAX, -1.f, ln.c_str()).x);

            if (max_rendered_w > max_fit_w && max_rendered_w > 0.f) {
                fsz    *= max_fit_w / max_rendered_w;
                line_h  = fsz * 1.25f;
            }
        }

        float block_h = txt_lines.size() * line_h;
        float slot_h  = fmaxf(40.f, block_h);
        // Safe-zone margins — proportional to canvas height, identical to canvas.cpp.
        float sz_top = SAFE_TOP * h;
        float sz_bot = SAFE_BOT * h;
        float slot_y;
        if (active->sub_pos == 1)
            slot_y = p.y + h * 0.5f - block_h * 0.5f;
        else if (active->sub_pos == 2)
            slot_y = p.y + sz_top + text_rendered * slot_h;
        else if (active->sub_pos == 3)
            slot_y = p.y + active->sub_pos_y * h - block_h * 0.5f;
        else
            slot_y = p.y + h - sz_bot - block_h - text_rendered * slot_h;
        // Clamp to safe zone so text never lands under platform UI chrome.
        slot_y = fmaxf(p.y + sz_top, fminf(p.y + h - sz_bot - block_h, slot_y));

        float local_t  = t - active->start;
        float clip_dur = active->end - active->start;
        float fade_in  = fminf(0.25f, clip_dur * 0.3f);
        float fade_out = fminf(0.25f, clip_dur * 0.2f);

        EffectAccum text_fx = collect_effects(state, t, ti);

        float anim_dx    = 0.f;
        float anim_dy    = 0.f;
        float anim_alpha = 1.f;

        AnimStyle eff_style = (active->clip_style != AnimStyle::None)
                              ? active->clip_style : state.style;

        switch (eff_style) {
            case AnimStyle::Fade:
                if (local_t < fade_in)       anim_alpha = local_t / fade_in;
                else if (local_t > clip_dur - fade_out)
                                              anim_alpha = (clip_dur - local_t) / fade_out;
                break;
            case AnimStyle::Glitch: {
                float decay = fmaxf(0.f, 1.f - local_t / 0.5f);
                anim_dx = sinf(local_t * 97.f + sinf(local_t * 53.f) * 31.f) * 12.f * decay;
                break;
            }
            case AnimStyle::Typewriter:
                if (local_t < fade_in) {
                    anim_alpha = local_t / fade_in;
                    anim_dy    = (fade_in - local_t) / fade_in * (-8.f);
                }
                break;
            case AnimStyle::Bounce: {
                float bd = fminf(0.6f, clip_dur);
                if (local_t < bd) {
                    float p2 = local_t / bd;
                    anim_dy = sinf(p2 * 3.14159f) * (-60.f) * expf(-p2 * 4.f);
                }
                break;
            }
            case AnimStyle::Slide:
                if (local_t < fade_in)
                    anim_dx = (local_t / fade_in - 1.f) * w * 0.6f;
                else if (local_t > clip_dur - fade_out)
                    anim_dx = ((local_t - (clip_dur - fade_out)) / fade_out) * w * 0.6f;
                break;
            case AnimStyle::Stack:
                if (local_t < fade_in)
                    anim_dy = (1.f - local_t / fade_in) * 80.f;
                break;
            default: break;
        }

        if (text_fx.any_text) {
            anim_alpha *= text_fx.opacity_mul;
            fsz        *= text_fx.scale_mul;
            line_h      = fsz * 1.25f;
            block_h     = txt_lines.size() * line_h;
        }

        float block_cx = p.x + active->sub_pos_x * w;
        float ty_anim  = slot_y + anim_dy;

        std::vector<const WordEntry*> clip_words;
        bool has_karaoke = (active->karaoke && !state.words_cache.empty());
        if (has_karaoke) {
            for (auto& we : state.words_cache)
                if (we.end > active->start && we.start < active->end)
                    clip_words.push_back(&we);
            if (clip_words.empty()) has_karaoke = false;
        }

        {
            TextRenderCtx trc;
            trc.dl         = dl;
            trc.font       = txt_font;
            trc.fsz        = fsz;
            trc.anim_alpha = anim_alpha;
            trc.anim_dx    = anim_dx;
            trc.anim_dy    = 0.f;
            trc.clip       = active;
            trc.eff_style  = eff_style;
            trc.anchor_h   = active->sub_anchor_h;
            trc.block_cx   = block_cx;
            trc.ty         = ty_anim;
            trc.line_h     = line_h;
            trc.t          = t;
            trc.rotation   = active->eval_prop("rotation", t);
            trc.clip_words = has_karaoke ? &clip_words : nullptr;
            render_text_block(trc, txt_lines);
        }

        // Callout arrow: triangle from text box edge toward arrow target point
        if (active->callout_arrow) {
            float max_rw = 0.f;
            for (auto& ln : txt_lines)
                max_rw = fmaxf(max_rw, txt_font->CalcTextSizeA(fsz, FLT_MAX, -1.f, ln.c_str()).x);
            const auto& cts = active->ts;
            float pad_x = cts.bg_pad_x, pad_y = cts.bg_pad_y;
            float bx_anchor;
            if (active->sub_anchor_h == 0)      bx_anchor = block_cx;
            else if (active->sub_anchor_h == 2) bx_anchor = block_cx - max_rw;
            else                                bx_anchor = block_cx - max_rw * 0.5f;
            float bx0 = bx_anchor - pad_x,  bx1 = bx_anchor + max_rw + pad_x;
            float by0 = ty_anim   - pad_y,  by1 = ty_anim   + block_h + pad_y;
            float atx = p.x + active->arrow_tx * w;
            float aty = p.y + active->arrow_ty * h;
            float cx = (bx0 + bx1) * 0.5f, cy = (by0 + by1) * 0.5f;
            float dx = atx - cx, dy = aty - cy;
            float hw = (bx1 - bx0) * 0.5f, hh_box = (by1 - by0) * 0.5f;
            float ex, ey;
            if (fabsf(dx) < 0.001f && fabsf(dy) < 0.001f) {
                ex = cx; ey = by1;
            } else {
                float te_x = (fabsf(dx) > 0.001f) ? hw     / fabsf(dx) : FLT_MAX;
                float te_y = (fabsf(dy) > 0.001f) ? hh_box / fabsf(dy) : FLT_MAX;
                float te   = fminf(te_x, te_y);
                ex = cx + dx * te; ey = cy + dy * te;
            }
            float len = sqrtf(dx*dx + dy*dy);
            float nx = (len > 0.001f) ? -dy / len * 8.f : 8.f;
            float ny = (len > 0.001f) ?  dx / len * 8.f : 0.f;
            ImU32 acol = IM_COL32((int)(cts.bg_col[0]*255), (int)(cts.bg_col[1]*255),
                                   (int)(cts.bg_col[2]*255), (int)(cts.bg_col[3]*255));
            dl->AddTriangleFilled({ex + nx, ey + ny}, {ex - nx, ey - ny}, {atx, aty}, acol);
        }

        ++text_rendered;
    }
}
