#include "overlay_renderer.h"
#include "app.h"
#include "ui/theme.h"

#include <imgui.h>
#include <cmath>
#include <vector>
#include <string>

extern ImFont* g_font_black;

void draw_text_overlays(ImDrawList* dl, const AppState& state, float t,
                        ImVec2 p, float w, float h)
{
    if (state.tracks.empty()) return;

    int text_rendered = 0;

    for (int ti = (int)state.tracks.size() - 1; ti >= 0; --ti) {
        const auto& track = state.tracks[ti];
        if (!track.visible) continue;

        const Clip* active = nullptr;
        for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
            auto ct = track.clips[ci].clip_type;
            if (ct != ClipType::Text && ct != ClipType::Lyrics && ct != ClipType::Subtitle)
                continue;
            if (t >= track.clips[ci].start && t < track.clips[ci].end) {
                active = &track.clips[ci];
                break;
            }
        }
        if (!active) { ++text_rendered; continue; }

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

        float block_h = txt_lines.size() * line_h;
        float slot_h  = fmaxf(40.f, block_h);
        float slot_y;
        if (active->sub_pos == 1)
            slot_y = p.y + h * 0.5f - block_h * 0.5f;
        else if (active->sub_pos == 2)
            slot_y = p.y + 24.f + text_rendered * slot_h;
        else if (active->sub_pos == 3)
            slot_y = p.y + active->sub_pos_y * h - block_h * 0.5f;
        else
            slot_y = p.y + h - 24.f - block_h - text_rendered * slot_h;

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
        ImU32 shad_col = IM_COL32(0, 0, 0, (int)(180.f * anim_alpha));

        std::vector<const WordEntry*> clip_words;
        bool has_karaoke = (active->karaoke && !state.words_cache.empty());
        if (has_karaoke) {
            for (auto& we : state.words_cache)
                if (we.end > active->start && we.start < active->end)
                    clip_words.push_back(&we);
            if (clip_words.empty()) has_karaoke = false;
        }
        int kw_idx = 0;

        float block_max_w = 0.f;
        for (auto& ln : txt_lines)
            block_max_w = fmaxf(block_max_w, txt_font->CalcTextSizeA(fsz, FLT_MAX, -1.f, ln.c_str()).x);

        if (eff_style == AnimStyle::Block) {
            float pad_x = 8.f, pad_y = 4.f;
            float bx0 = block_cx - block_max_w * 0.5f - pad_x + anim_dx;
            dl->AddRectFilled(
                {bx0, ty_anim - pad_y},
                {bx0 + block_max_w + pad_x * 2.f, ty_anim + block_h + pad_y},
                to_u32(Col::fg), 2.f);
        }

        for (int li = 0; li < (int)txt_lines.size(); ++li) {
            const std::string& ln = txt_lines[li];
            ImVec2 lsz = txt_font->CalcTextSizeA(fsz, FLT_MAX, -1.f, ln.c_str());
            float lx   = block_cx - lsz.x * 0.5f + anim_dx;
            float ly   = ty_anim + li * line_h;

            dl->AddText(txt_font, fsz, {lx + 2.f, ly + 2.f}, shad_col, ln.c_str());

            if (has_karaoke) {
                const char* lp = ln.c_str();
                float cur_x = lx;
                while (*lp) {
                    const char* ep = lp;
                    while (*ep && *ep != ' ') ++ep;
                    std::string lword(lp, ep);
                    bool has_space = (*ep == ' ');
                    std::string lword_sp = lword + (has_space ? " " : "");

                    bool is_active_word = false;
                    if (kw_idx < (int)clip_words.size()) {
                        const WordEntry* we = clip_words[kw_idx];
                        is_active_word = (t >= we->start && t < we->end);
                        ++kw_idx;
                    }

                    ImU32 wcol;
                    if (active->sub_color_override) {
                        float a = (is_active_word ? active->sub_color[3] : active->sub_color[3] * 0.45f) * anim_alpha;
                        wcol = IM_COL32((int)(active->sub_color[0]*255), (int)(active->sub_color[1]*255),
                                        (int)(active->sub_color[2]*255), (int)(a*255));
                    } else {
                        wcol = is_active_word ? IM_COL32(255,255,255,(int)(255*anim_alpha))
                                              : IM_COL32(255,255,255,(int)(100*anim_alpha));
                    }
                    float word_w = txt_font->CalcTextSizeA(fsz, FLT_MAX, -1.f, lword_sp.c_str()).x;
                    dl->AddText(txt_font, fsz, {cur_x, ly}, wcol, lword_sp.c_str());
                    cur_x += word_w;
                    lp = has_space ? ep + 1 : ep;
                }
            } else {
                ImU32 tcol;
                if (active->sub_color_override) {
                    float a = active->sub_color[3] * anim_alpha;
                    tcol = IM_COL32((int)(active->sub_color[0]*255), (int)(active->sub_color[1]*255),
                                    (int)(active->sub_color[2]*255), (int)(a*255));
                } else if (eff_style == AnimStyle::Block) {
                    tcol = to_u32(Col::bg);
                } else {
                    tcol = IM_COL32(255, 255, 255, (int)(255.f * anim_alpha));
                }
                dl->AddText(txt_font, fsz, {lx, ly}, tcol, ln.c_str());
            }
        }

        ++text_rendered;
    }
}
