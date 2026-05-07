#include "render.h"
#include "inter_font.h"   // inter_black_ttf[], inter_black_ttf_size
#include "bg_remove.h"
#include "globals.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <map>
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>

#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>

namespace fs = std::filesystem;

static std::atomic<bool>  g_cancel{false};
static std::atomic<pid_t> g_ffmpeg_pid{0};
static std::string        g_font_path;

// ── Font extraction ───────────────────────────────────────────────────────────

void render_init_fonts() {
    g_font_path = "/tmp/pms_inter_black.ttf";
    FILE* f = fopen(g_font_path.c_str(), "wb");
    if (f) {
        fwrite(inter_black_ttf, 1, inter_black_ttf_size, f);
        fclose(f);
    } else {
        g_font_path.clear();
    }
}

const std::string& render_font_path() { return g_font_path; }

// ── SRT export ────────────────────────────────────────────────────────────────

static std::string srt_ts(float s) {
    int h  = (int)(s / 3600);
    int m  = (int)((s - h*3600) / 60);
    int sc = (int)s % 60;
    int ms = (int)(std::fmod(s, 1.f) * 1000.f);
    char buf[20];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d,%03d", h, m, sc, ms);
    return buf;
}

bool render_export_srt(const AppState& state, const std::string& out_path) {
    std::ofstream f(out_path);
    if (!f) return false;
    int idx = 1;
    for (auto& [ti, ci] : state.subtitle_clip_indices()) {
        const Clip& c = state.tracks[ti].clips[ci];
        f << idx++ << "\n" << srt_ts(c.start) << " --> " << srt_ts(c.end) << "\n"
          << c.text << "\n\n";
    }
    return true;
}

// ── FFmpeg option-value escaping ──────────────────────────────────────────────
// Within a filter_complex_script option value:
//   \  →  \\      (Level 1)
//   :  →  \:      (option separator)
//   '  →  \'      (quoting char)
//   ,  →  \,      (filter chain separator, Level 2)
//   ;  →  \;      (chain separator, Level 2)
//   [  →  \[      (link label, Level 2)
//   ]  →  \]
//   %  →  %%      (drawtext expansion)
static std::string esc(const std::string& s) {
    std::string o;
    o.reserve(s.size() * 2);
    for (unsigned char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case ':':  o += "\\:";  break;
            case '\'': o += "\\'";  break;
            case ',':  o += "\\,";  break;
            case ';':  o += "\\;";  break;
            case '[':  o += "\\[";  break;
            case ']':  o += "\\]";  break;
            case '%':  o += "%%";   break;
            default:   o += (char)c; break;
        }
    }
    return o;
}

// ── Keyframe expression builder ───────────────────────────────────────────────

// Returns a constant string (e.g. "0.500") for a static prop, or a piecewise
// linear if-chain expression over ffmpeg `t` (seconds from input start).
// `bias` is added to `t` before evaluation (e.g. clip.start when -ss is used).
// `scale` multiplies the output value (e.g. out_w for pos_x → pixel X).
static std::string prop_expr(const Clip& cl, const std::string& prop,
                              float scale_factor, float default_val = -999.f,
                              float eval_at = -1.f)
{
    // Snapshot mode: evaluate at a specific timeline time → constant output.
    if (eval_at >= 0.f) {
        float v = cl.eval_prop(prop, eval_at);
        char buf[32]; snprintf(buf, sizeof(buf), "%.4f", (double)(v * scale_factor));
        return buf;
    }

    auto it = cl.ktracks.find(prop);
    bool has_kf = (it != cl.ktracks.end() && !it->second.empty());

    float sv = (default_val > -998.f) ? default_val : cl.eval_prop(prop, cl.start);

    if (!has_kf) {
        char buf[32]; snprintf(buf, sizeof(buf), "%.4f", (double)(sv * scale_factor));
        return buf;
    }

    const auto& keys = it->second.keys;
    if (keys.size() == 1) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.4f", (double)(keys[0].value * scale_factor));
        return buf;
    }

    // Build piecewise linear expression (right-to-left nesting).
    // Each segment: lerp from keys[i] to keys[i+1] over [t0..t1].
    // Easing is baked as the alpha curve; Hold just clamps at start value.
    std::string expr;
    for (int i = (int)keys.size() - 2; i >= 0; --i) {
        float t0  = keys[i].time,   t1  = keys[i+1].time;
        float v0  = keys[i].value,  v1  = keys[i+1].value;
        float dt  = (t1 - t0 < 1e-5f) ? 1e-5f : (t1 - t0);

        std::string alpha_expr;
        switch (keys[i].interp) {
            case InterpType::Hold:
                alpha_expr = "0";
                break;
            case InterpType::EaseIn: {
                // a=clamp((t-t0)/dt,0,1); a^2
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "pow(clip((t-%.4f)/%.4f\\,0\\,1)\\,2)",
                    (double)t0, (double)dt);
                alpha_expr = buf;
                break;
            }
            case InterpType::EaseOut: {
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "(1-pow(1-clip((t-%.4f)/%.4f\\,0\\,1)\\,2))",
                    (double)t0, (double)dt);
                alpha_expr = buf;
                break;
            }
            case InterpType::EaseBoth: {
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "smoothstep(%.4f\\,%.4f\\,t)",
                    (double)t0, (double)(t0 + dt));
                alpha_expr = buf;
                break;
            }
            default: { // Linear
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "clip((t-%.4f)/%.4f\\,0\\,1)",
                    (double)t0, (double)dt);
                alpha_expr = buf;
                break;
            }
        }

        float vs = v0 * scale_factor;
        float vd = (v1 - v0) * scale_factor;
        char seg[256];
        snprintf(seg, sizeof(seg), "(%.4f+%.4f*%s)",
                 (double)vs, (double)vd, alpha_expr.c_str());

        if (expr.empty()) {
            // Last segment — value after last keyframe
            char tail[32];
            snprintf(tail, sizeof(tail), "%.4f",
                     (double)(keys.back().value * scale_factor));
            expr = std::string("if(lt(t,") + std::to_string(t1) + "),"
                   + seg + "," + tail + ")";
        } else {
            expr = std::string("if(lt(t,") + std::to_string(t1) + "),"
                   + seg + "," + expr + ")";
        }
    }

    // Clamp before first key
    char head[32];
    snprintf(head, sizeof(head), "%.4f",
             (double)(keys[0].value * scale_factor));
    expr = std::string("if(lt(t,") + std::to_string(keys[0].time)
           + ")," + head + "," + expr + ")";

    return expr;
}

// ── Unified render layer ──────────────────────────────────────────────────────
// One entry per clip that contributes to the visual output, ordered
// bottom-to-top (index 0 = background, last = frontmost).
struct RLayer {
    enum Kind { Vid, Txt } kind;
    int   track_idx, clip_idx;
    int   in_idx      = -1;   // Vid: ffmpeg input index
    float vid_ss      = 0.f;  // Vid: -ss seek of that input (for enable expr)
    int   mask_in_idx = -1;   // >= 0 if this layer has bg_remove hires masks
    float mask_fps    = 30.f; // fps of the mask image sequence
};

// ── Filter-complex script writer ──────────────────────────────────────────────

static bool write_filter_script(
    const AppState& state,
    const std::string& path,
    const std::vector<RLayer>& layers,
    const std::vector<int>& aud_ins,       // ffmpeg input indices for each audio stream
    const std::vector<float>& aud_vols,    // per-stream volume
    int out_w, int out_h,
    float sub_offset,
    std::string& vout_label,
    std::string& aout_label,
    float snap_eval_t = -1.f)             // >= 0 → snapshot mode: eval KFs at this time
{
    std::ostringstream fc;
    bool first = true;
    auto line = [&]() -> std::ostringstream& {
        if (!first) fc << ";\n";
        first = false;
        return fc;
    };

    int font_sz  = (out_h >= 1500) ? 96 : 72;
    int vid_idx  = 0;   // counter for unique filter labels
    int txt_idx  = 0;
    int fx_idx   = 0;   // counter for effect filter labels

    // ── Black canvas base ─────────────────────────────────────────────────────
    line() << "color=c=black:s=" << out_w << "x" << out_h << ":r=" << state.fps << "[vbase]";
    std::string vcur = "[vbase]";

    // ── Build transition info map (keyed by track_idx, clip_idx) ─────────────
    // For each video clip: does it fade out to the next? does it fade in from the previous?
    struct TransInfo {
        bool  is_out = false;
        bool  is_in  = false;
        float pre    = 0.f;   // fade duration inside outgoing clip
        float post   = 0.f;   // fade duration inside incoming clip
        TransitionType type = TransitionType::None;
    };
    std::map<std::pair<int,int>, TransInfo> trans_map;
    for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
        const auto& clips = state.tracks[ti].clips;
        for (int ci = 0; ci + 1 < (int)clips.size(); ++ci) {
            const Clip& A = clips[ci];
            const Clip& B = clips[ci + 1];
            if (A.clip_type != ClipType::Video || B.clip_type != ClipType::Video) continue;
            if (A.transition_type == TransitionType::None) continue;
            if (A.transition_pre <= 0.f && A.transition_post <= 0.f) continue;
            trans_map[{ti, ci}].is_out = true;
            trans_map[{ti, ci}].pre    = A.transition_pre;
            trans_map[{ti, ci}].post   = A.transition_post;
            trans_map[{ti, ci}].type   = A.transition_type;
            trans_map[{ti, ci+1}].is_in = true;
            trans_map[{ti, ci+1}].pre   = A.transition_pre;
            trans_map[{ti, ci+1}].post  = A.transition_post;
            trans_map[{ti, ci+1}].type  = A.transition_type;
        }
    }

    // ── Process all layers bottom-to-top ─────────────────────────────────────
    for (const RLayer& rl : layers) {
        const Track& tr = state.tracks[rl.track_idx];
        const Clip&  cl = tr.clips[rl.clip_idx];
        if (!tr.visible) continue;

        if (rl.kind == RLayer::Vid) {
            std::string layer_in;

            // Step 1: Letterbox-fit preserving AR (no padding — keeps actual video dims).
            // All layers go through the same pipeline so transforms are always respected.
            {
                std::string fit_tag = "[vfit" + std::to_string(vid_idx) + "]";
                line() << "[" << rl.in_idx << ":v]"
                       << "scale=" << out_w << ":" << out_h
                       << ":force_original_aspect_ratio=decrease"
                       << ",setsar=1,format=rgba"
                       << fit_tag;
                layer_in = fit_tag;
            }

            // Step 2: User scale relative to the letterbox-fitted size (KF-aware).
            {
                bool has_sx_kf = cl.ktracks.count("scale_x") > 0;
                bool has_sy_kf = cl.ktracks.count("scale_y") > 0;
                bool need_scale = has_sx_kf || has_sy_kf ||
                                  fabsf(cl.scale_x - 1.f) > 0.001f ||
                                  fabsf(cl.scale_y - 1.f) > 0.001f;
                if (need_scale) {
                    std::string sx = prop_expr(cl, "scale_x", 1.f, cl.scale_x, snap_eval_t);
                    std::string sy = prop_expr(cl, "scale_y", 1.f, cl.scale_y, snap_eval_t);
                    std::string scl_tag = "[vscl" + std::to_string(vid_idx) + "]";
                    line() << layer_in
                           << "scale=iw*(" << sx << "):ih*(" << sy << "):eval=frame"
                           << scl_tag;
                    layer_in = scl_tag;
                }
            }

            // Step 3: Rotation in degrees → radians for ffmpeg rotate filter (KF-aware).
            {
                bool has_rot_kf = cl.ktracks.count("rotation") > 0;
                bool need_rot = has_rot_kf || fabsf(cl.rotation) > 0.001f;
                if (need_rot) {
                    std::string rot_e = prop_expr(cl, "rotation",
                                                  (float)(M_PI / 180.0), cl.rotation, snap_eval_t);
                    std::string rot_tag = "[vrot" + std::to_string(vid_idx) + "]";
                    line() << layer_in
                           << "rotate=" << rot_e
                           << ":fillcolor=black@0:ow=iw:oh=ih"
                           << rot_tag;
                    layer_in = rot_tag;
                }
            }

            // Opacity (static, keyframed, and/or transition fade)
            {
                auto tit = trans_map.find({rl.track_idx, rl.clip_idx});
                bool has_trans = (tit != trans_map.end());
                bool has_opa = has_trans ||
                               cl.ktracks.count("opacity") > 0 ||
                               fabsf(cl.opacity - 1.f) > 0.01f;
                if (has_opa) {
                    std::string base_opa = prop_expr(cl, "opacity", 1.f, cl.opacity, snap_eval_t);
                    std::string aa_expr  = base_opa;

                    if (has_trans) {
                        const TransInfo& ti2 = tit->second;
                        float en1 = fmaxf(0.f, cl.end   - rl.vid_ss);
                        float en0 = fmaxf(0.f, cl.start - rl.vid_ss);

                        if (ti2.is_out && ti2.pre > 0.f) {
                            // Fade out over transition_pre seconds before cut
                            char fade[128];
                            snprintf(fade, sizeof(fade),
                                "(1-clip((t-%.3f)/%.3f\\,0\\,1))",
                                (double)(en1 - ti2.pre), (double)ti2.pre);
                            aa_expr = "(" + aa_expr + "*" + fade + ")";
                        }
                        if (ti2.is_in && ti2.post > 0.f) {
                            // Fade in over transition_post seconds after cut
                            char fade[128];
                            snprintf(fade, sizeof(fade),
                                "clip((t-%.3f)/%.3f\\,0\\,1)",
                                (double)en0, (double)ti2.post);
                            aa_expr = "(" + aa_expr + "*" + fade + ")";
                        }
                    }

                    std::string opa_tag = "[vopa" + std::to_string(vid_idx) + "]";
                    line() << layer_in
                           << "colorchannelmixer=aa=" << aa_expr
                           << opa_tag;
                    layer_in = opa_tag;
                }
            }

            // BG remove alphamerge — extract alpha from hires mask sequence, merge.
            if (rl.mask_in_idx >= 0) {
                std::string alpha_tag  = "[valpha" + std::to_string(vid_idx) + "]";
                std::string masked_tag = "[vbgm"   + std::to_string(vid_idx) + "]";
                line() << "[" << rl.mask_in_idx << ":v]"
                       << "scale=" << out_w << ":" << out_h << ":flags=bilinear,"
                       << "extractplanes=a"
                       << alpha_tag;
                line() << layer_in << alpha_tag << "alphamerge" << masked_tag;
                layer_in = masked_tag;
            }

            // Effect clip filters — color grade, blur, vignette
            // Collect all Effect clips on tracks above this video clip that overlap it.
            for (int fti = 0; fti < rl.track_idx; ++fti) {
                if (!state.tracks[fti].visible) continue;
                for (auto& fc : state.tracks[fti].clips) {
                    if (fc.clip_type != ClipType::Effect) continue;
                    if (fc.end <= cl.start || fc.start >= cl.end) continue;
                    // Compute enable window in output-timeline coordinates
                    float en0 = fmaxf(0.f, fc.start - rl.vid_ss);
                    float en1 = fmaxf(0.f, fminf(fc.end, cl.end) - rl.vid_ss);
                    if (en1 <= en0) continue;
                    char en[80];
                    snprintf(en, sizeof(en), "between(t,%.3f,%.3f)", (double)en0, (double)en1);

                    // ── Adjustment layer FX ───────────────────────────────
                    if (fc.fx_type == FXType::Adjustment) {
                        if (fc.fx_color_on) {
                            std::string ntag = "[vfx" + std::to_string(fx_idx++) + "]";
                            char eq[256];
                            snprintf(eq, sizeof(eq),
                                "eq=brightness=%.3f:contrast=%.3f:saturation=%.3f:enable='%s'",
                                (double)fc.fx_brightness, (double)fc.fx_contrast,
                                (double)fc.fx_saturation, en);
                            line() << layer_in << eq << ntag;
                            layer_in = ntag;
                            if (fabsf(fc.fx_hue) > 0.5f) {
                                std::string htag = "[vfx" + std::to_string(fx_idx++) + "]";
                                char hue[160];
                                snprintf(hue, sizeof(hue),
                                    "hue=h=%.1f:enable='%s'", (double)fc.fx_hue, en);
                                line() << layer_in << hue << htag;
                                layer_in = htag;
                            }
                        }
                        if (fc.fx_blur_on && fc.fx_blur > 0.1f) {
                            std::string ntag = "[vfx" + std::to_string(fx_idx++) + "]";
                            char blur[160];
                            snprintf(blur, sizeof(blur),
                                "gblur=sigma=%.1f:enable='%s'", (double)fc.fx_blur, en);
                            line() << layer_in << blur << ntag;
                            layer_in = ntag;
                        }
                        if (fc.fx_vignette_on && fc.fx_vignette > 0.01f) {
                            std::string ntag = "[vfx" + std::to_string(fx_idx++) + "]";
                            char vig[160];
                            snprintf(vig, sizeof(vig),
                                "vignette=angle=%.4f:enable='%s'",
                                (double)(fc.fx_vignette * 3.14159265f * 0.5f), en);
                            line() << layer_in << vig << ntag;
                            layer_in = ntag;
                        }
                    }

                    // ── Creative FX (rendered as closest FFmpeg approximation) ─
                    if (fc.fx_type == FXType::Glitch && fc.fx_glitch_chroma >= 0.1f) {
                        // RGB channel split via rgbashift (R right, B left)
                        int cr = (int)(fc.fx_glitch_chroma + 0.5f);
                        std::string ntag = "[vfx" + std::to_string(fx_idx++) + "]";
                        char flt[200];
                        snprintf(flt, sizeof(flt),
                            "rgbashift=rh=%d:bh=%d:enable='%s'", cr, -cr, en);
                        line() << layer_in << flt << ntag;
                        layer_in = ntag;
                    }
                    if (fc.fx_type == FXType::VHS) {
                        // Chroma bleed: R drifts right, B drifts left (less)
                        int bleed   = (int)(fc.fx_vhs_bleed + 0.5f);
                        int bleed_b = (int)(fc.fx_vhs_bleed * 0.4f + 0.5f);
                        if (bleed > 0) {
                            std::string ntag = "[vfx" + std::to_string(fx_idx++) + "]";
                            char flt[200];
                            snprintf(flt, sizeof(flt),
                                "rgbashift=rh=%d:bh=%d:enable='%s'", bleed, -bleed_b, en);
                            line() << layer_in << flt << ntag;
                            layer_in = ntag;
                        }
                        // Grain: temporal noise, strength mapped to noise seed range
                        if (fc.fx_vhs_noise >= 0.01f) {
                            int ns = (int)(fc.fx_vhs_noise * 60.f + 0.5f);
                            std::string ntag = "[vfx" + std::to_string(fx_idx++) + "]";
                            char flt[200];
                            snprintf(flt, sizeof(flt),
                                "noise=c0s=%d:c0f=t+u:enable='%s'", ns, en);
                            line() << layer_in << flt << ntag;
                            layer_in = ntag;
                        }
                    }
                    // ZoomPunch: scale spike per beat rendered via scale+crop chain
                    if (fc.fx_type == FXType::ZoomPunch && !state.beats.empty() &&
                        fc.fx_zoom_strength > 0.001f) {
                        // Build max-of-decays expression over all beats in the clip window
                        std::string zoom_e = "0";
                        float str   = fc.fx_zoom_strength;
                        float decay = fmaxf(0.05f, fc.fx_zoom_decay);
                        for (float bt : state.beats) {
                            if (bt < fc.start || bt >= fc.end) continue;
                            // Time relative to clip's ffmpeg input start
                            float bt_rel = fmaxf(0.f, bt - rl.vid_ss);
                            char term[128];
                            snprintf(term, sizeof(term),
                                "if(gte(t,%.3f),%.4f*exp(-(t-%.3f)/%.4f),0)",
                                (double)bt_rel, (double)str, (double)bt_rel, (double)decay);
                            zoom_e = "max(" + zoom_e + "," + term + ")";
                        }
                        if (zoom_e != "0") {
                            // Scale up from centre then crop back to canvas
                            std::string stag = "[vfx" + std::to_string(fx_idx++) + "]";
                            std::string ctag = "[vfx" + std::to_string(fx_idx++) + "]";
                            char scale_f[512];
                            snprintf(scale_f, sizeof(scale_f),
                                "scale=iw*(1+%s):ih*(1+%s):eval=frame:enable='%s'",
                                zoom_e.c_str(), zoom_e.c_str(), en);
                            line() << layer_in << scale_f << stag;
                            char crop_f[128];
                            snprintf(crop_f, sizeof(crop_f),
                                "crop=%d:%d:(iw-%d)/2:(ih-%d)/2:enable='%s'",
                                out_w, out_h, out_w, out_h, en);
                            line() << stag << crop_f << ctag;
                            layer_in = ctag;
                        }
                    }
                }
            }

            // Position — all layers use pos_x/pos_y; eval=frame when KFs animate it.
            // overlay filter uses overlay_w/overlay_h (not iw/ih) for the overlay input dimensions.
            std::string x_e = "(" + prop_expr(cl, "pos_x", (float)out_w, cl.pos_x, snap_eval_t) + "-overlay_w/2)";
            std::string y_e = "(" + prop_expr(cl, "pos_y", (float)out_h, cl.pos_y, snap_eval_t) + "-overlay_h/2)";
            bool has_pos_kf = cl.ktracks.count("pos_x") > 0 || cl.ktracks.count("pos_y") > 0;

            // Enable window
            float en0 = fmaxf(0.f, cl.start - rl.vid_ss);
            float en1 = fmaxf(0.f, cl.end   - rl.vid_ss);

            std::string vnext = "[vov" + std::to_string(vid_idx) + "]";
            line() << vcur << layer_in
                   << "overlay=x=" << x_e << ":y=" << y_e
                   << ":format=auto"
                   << (has_pos_kf ? ":eval=frame" : "")
                   << ":enable='between(t,"
                   << std::fixed << std::setprecision(3)
                   << en0 << "," << en1 << ")'"
                   << vnext;
            vcur = vnext;
            ++vid_idx;

            // DipWhite: overlay a white fill that peaks at transition midpoint
            {
                auto tit2 = trans_map.find({rl.track_idx, rl.clip_idx});
                if (tit2 != trans_map.end() && tit2->second.is_out &&
                    tit2->second.type == TransitionType::DipWhite) {
                    float pre  = tit2->second.pre;
                    float post = tit2->second.post;
                    float total = pre + post;
                    float t_start = en1 - pre;
                    // alpha peaks at the cut point (t = en1)
                    char wexpr[256];
                    snprintf(wexpr, sizeof(wexpr),
                        "(1-abs(clip((t-%.3f)/%.3f\\,0\\,1)-%.3f)*%.3f)",
                        (double)t_start,
                        (double)(total > 0.f ? total : 1e-5f),
                        (double)(total > 0.f ? pre / total : 0.5),
                        (double)(total > 0.f ? total / fmaxf(pre, post) : 2.0));
                    std::string w_src  = "[wfill"  + std::to_string(vid_idx) + "]";
                    std::string w_opa  = "[wopa"   + std::to_string(vid_idx) + "]";
                    std::string w_next = "[wov"    + std::to_string(vid_idx) + "]";
                    line() << "color=c=white:s=" << out_w << "x" << out_h
                           << ":r=" << state.fps << w_src;
                    line() << w_src << "colorchannelmixer=aa=" << wexpr << w_opa;
                    line() << vcur << w_opa
                           << "overlay=format=auto"
                           << ":enable='between(t,"
                           << std::fixed << std::setprecision(3)
                           << t_start << "," << en1 << ")'"
                           << w_next;
                    vcur = w_next;
                    ++vid_idx;
                }
            }

        } else { // Txt
            if (cl.text.empty()) continue;

            std::string y_e;
            if      (cl.sub_pos == 1) y_e = "(h-text_h)/2";
            else if (cl.sub_pos == 2) y_e = "h*0.10";
            else if (cl.sub_pos == 3) {
                char yb[64];
                snprintf(yb, sizeof(yb), "h*%.4f-text_h/2", (double)cl.sub_pos_y);
                y_e = yb;
            } else {
                y_e = "h*0.88-text_h";
            }

            // Per-word karaoke overlays only for clips created in Karaoke grouping mode
            std::vector<const WordEntry*> clip_words;
            if (cl.karaoke) {
                for (auto& we : state.words_cache)
                    if (we.end > cl.start && we.start < cl.end)
                        clip_words.push_back(&we);
            }

            bool has_karaoke = !clip_words.empty();

            // Kinetic typography: per-style x/y/alpha modifiers
            // `lt` = clip-relative time = t - clip_t0 (where clip_t0 = cl.start - sub_offset).
            // In snapshot mode sub_offset=snap_t, so clip_t0 may be negative; at t=0 lt=snap_t-cl.start.
            float clip_t0 = (snap_eval_t >= 0.f)
                            ? (cl.start - sub_offset)
                            : fmaxf(0.f, cl.start - sub_offset);
            float clip_dur = fmaxf(0.01f, cl.end - cl.start);
            float fade_in  = fminf(0.25f, clip_dur * 0.3f);
            float fade_out = fminf(0.25f, clip_dur * 0.2f);

            // lt = clip-relative time (0 at clip start)
            char lt_def[64];
            snprintf(lt_def, sizeof(lt_def), "(t-%.3f)", (double)clip_t0);
            std::string lt = lt_def;

            AnimStyle eff_style = (cl.clip_style != AnimStyle::None)
                                  ? cl.clip_style : state.style;

            // Collect text effects from Effect clips above this track
            float fx_opacity = 1.f, fx_scale = 1.f;
            for (int fti = 0; fti < rl.track_idx; ++fti) {
                if (!state.tracks[fti].visible) continue;
                for (auto& fc : state.tracks[fti].clips) {
                    if (fc.clip_type != ClipType::Effect || !fc.fx_text_on) continue;
                    if (fc.end <= cl.start || fc.start >= cl.end) continue;
                    fx_opacity *= fc.fx_opacity_mul;
                    fx_scale   *= fc.fx_scale_mul;
                }
            }
            int clip_font_sz = cl.font_size > 0.f
                               ? (int)(cl.font_size * out_h + 0.5f)
                               : font_sz;
            int effective_font_sz = (int)(clip_font_sz * fx_scale + 0.5f);

            // Alpha modifier (multiplied into fontcolor alpha)
            std::string alpha_mod = "1";
            // X offset added to base x=(w-text_w)/2
            std::string x_off = "0";
            // Y offset added to base y
            std::string y_off = "0";

            switch (eff_style) {
                case AnimStyle::Fade: {
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                        "if(lt(%s,%.3f)"
                        ",clip(%s/%.3f\\,0\\,1)"
                        ",if(gt(%s,%.3f)"
                        ",clip((%.3f-%s)/%.3f\\,0\\,1)"
                        ",1))",
                        lt.c_str(), (double)fade_in,
                        lt.c_str(), (double)fade_in,
                        lt.c_str(), (double)(clip_dur - fade_out),
                        (double)clip_dur, lt.c_str(), (double)fade_out);
                    alpha_mod = buf;
                    break;
                }
                case AnimStyle::Glitch: {
                    // Random-looking horizontal jitter using sin at high frequency, fades out
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                        "(sin(%s*97+sin(%s*53)*31)*12*clip(1-%s/0.5\\,0\\,1))",
                        lt.c_str(), lt.c_str(), lt.c_str());
                    x_off = buf;
                    break;
                }
                case AnimStyle::Typewriter: {
                    // Fade-in each character by revealing characters left-to-right
                    // Approximate with fontsize scaling alpha ramp (true char reveal needs per-char)
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                        "if(lt(%s,%.3f),clip(%s/%.3f\\,0\\,1),1)",
                        lt.c_str(), (double)fade_in,
                        lt.c_str(), (double)fade_in);
                    alpha_mod = buf;
                    // Also use a slight scale-up of font size via y squeeze
                    char yb[128];
                    snprintf(yb, sizeof(yb),
                        "(if(lt(%s,%.3f),(%.3f-%s)/%.3f*(-8)\\,0))",
                        lt.c_str(), (double)fade_in,
                        (double)fade_in, lt.c_str(), (double)fade_in);
                    y_off = yb;
                    break;
                }
                case AnimStyle::Bounce: {
                    // Spring enter from above, settles at 0
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                        "(if(lt(%s,%.3f)"
                        ",sin(clip(%s/%.3f\\,0\\,1)*3.14159)*(-60)*exp(-clip(%s/%.3f\\,0\\,1)*4)"
                        ",0))",
                        lt.c_str(), (double)fminf(0.6f, clip_dur),
                        lt.c_str(), (double)fminf(0.6f, clip_dur),
                        lt.c_str(), (double)fminf(0.6f, clip_dur));
                    y_off = buf;
                    break;
                }
                case AnimStyle::Slide: {
                    // Slide in from left, slide out to right
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                        "(if(lt(%s,%.3f)"
                        ",(clip(%s/%.3f\\,0\\,1)-1)*w*0.6"
                        ",if(gt(%s,%.3f)"
                        ",(clip((%s-%.3f)/%.3f\\,0\\,1))*w*0.6"
                        ",0)))",
                        lt.c_str(), (double)fade_in,
                        lt.c_str(), (double)fade_in,
                        lt.c_str(), (double)(clip_dur - fade_out),
                        lt.c_str(), (double)(clip_dur - fade_out), (double)fade_out);
                    x_off = buf;
                    break;
                }
                case AnimStyle::Stack: {
                    // Slide in from bottom
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                        "(if(lt(%s,%.3f),(1-clip(%s/%.3f\\,0\\,1))*80,0))",
                        lt.c_str(), (double)fade_in,
                        lt.c_str(), (double)fade_in);
                    y_off = buf;
                    break;
                }
                case AnimStyle::Block: {
                    // Box drawn via drawtext box=1 option (text_w/text_h not available in drawbox)
                    break;
                }
                default: break;
            }

            // Compose alpha into fontcolor if needed
            char dim_col[64];
            if (alpha_mod == "1") {
                if (cl.sub_color_override)
                    snprintf(dim_col, sizeof(dim_col), "0x%02x%02x%02x%02x",
                        (int)(cl.sub_color[0]*255), (int)(cl.sub_color[1]*255),
                        (int)(cl.sub_color[2]*255),
                        has_karaoke ? (int)(cl.sub_color[3]*255*0.4f) : (int)(cl.sub_color[3]*255));
                else
                    strcpy(dim_col, has_karaoke ? "white@0.4" : "white");
            } else {
                // Use alpha expression — embed as fontcolor=white@<expr>
                // (ffmpeg drawtext alpha= option, multiplied into fontcolor alpha)
                if (cl.sub_color_override)
                    snprintf(dim_col, sizeof(dim_col), "0x%02x%02x%02x%02x",
                        (int)(cl.sub_color[0]*255), (int)(cl.sub_color[1]*255),
                        (int)(cl.sub_color[2]*255),
                        has_karaoke ? (int)(cl.sub_color[3]*255*0.4f) : (int)(cl.sub_color[3]*255));
                else
                    strcpy(dim_col, has_karaoke ? "white@0.4" : "white");
            }

            // Word-wrap: break text into lines at sub_wrap_w * out_w pixels.
            // Use approximate character width (0.55 * font_size) since we don't
            // have ImGui font metrics here. Matches preview well for Inter Black.
            float max_line_px = cl.sub_wrap_w * (float)out_w;
            float char_w_approx = effective_font_sz * 0.55f;
            std::vector<std::string> render_lines;
            {
                const char* src = cl.text.c_str();
                const char* wp  = src;
                std::string cur;
                while (true) {
                    const char* ep = wp;
                    while (*ep && *ep != ' ') ++ep;
                    std::string word(wp, ep);
                    std::string test = cur.empty() ? word : cur + " " + word;
                    if (!cur.empty() && test.size() * char_w_approx > max_line_px) {
                        render_lines.push_back(cur);
                        cur = word;
                    } else {
                        cur = test;
                    }
                    if (!*ep) break;
                    wp = ep + 1;
                }
                if (!cur.empty()) render_lines.push_back(cur);
                if (render_lines.empty()) render_lines.push_back("");
            }
            float line_h_px = effective_font_sz * 1.25f;

            // x expression: centre at sub_pos_x fraction of canvas width
            char x_base_buf[64];
            snprintf(x_base_buf, sizeof(x_base_buf), "(w*%.4f-text_w/2)", (double)cl.sub_pos_x);
            std::string x_base = x_base_buf;
            std::string x_final = x_off == "0" ? x_base : "(" + x_base + "+" + x_off + ")";

            // Apply effect opacity
            std::string final_alpha = alpha_mod;
            if (fx_opacity < 0.999f) {
                char fa[64];
                if (alpha_mod == "1")
                    snprintf(fa, sizeof(fa), "%.4f", (double)fx_opacity);
                else
                    snprintf(fa, sizeof(fa), "(%.4f)*(%s)", (double)fx_opacity, alpha_mod.c_str());
                final_alpha = fa;
            }

            // Emit one drawtext per wrapped line
            for (int li = 0; li < (int)render_lines.size(); ++li) {
                std::string vnext = "[vtxt" + std::to_string(txt_idx++) + "]";
                auto& ln = line();

                // y for this line = base_y + line_index * line_height + y_off
                char y_line_buf[256];
                if (y_off == "0")
                    snprintf(y_line_buf, sizeof(y_line_buf), "(%s+%.1f)",
                             y_e.c_str(), (double)(li * line_h_px));
                else
                    snprintf(y_line_buf, sizeof(y_line_buf), "(%s+%.1f+(%s))",
                             y_e.c_str(), (double)(li * line_h_px), y_off.c_str());

                ln << vcur
                   << "drawtext="
                   << "fontfile=" << esc(g_font_path)       << ":"
                   << "text="     << esc(render_lines[li])   << ":"
                   << "fontsize=" << effective_font_sz        << ":"
                   << "fontcolor=" << dim_col                 << ":"
                   << "borderw=3:bordercolor=black@0.7:"
                   << "shadowx=2:shadowy=2:shadowcolor=black@0.5:"
                   << "x=" << x_final                        << ":"
                   << "y=" << y_line_buf                     << ":";
                if (final_alpha != "1")
                    ln << "alpha=" << final_alpha             << ":";
                if (eff_style == AnimStyle::Block)
                    ln << "box=1:boxcolor=white@1.0:boxborderw=8:fontcolor=black:borderw=0:shadowx=0:shadowy=0:";
                ln << "enable='between(t,"
                   << std::fixed << std::setprecision(3)
                   << clip_t0 << ","
                   << fmaxf(0.f, cl.end - sub_offset) << ")'"
                   << vnext;
                vcur = vnext;
            }

            // Karaoke overlays: one bright drawtext per word timed to its exact window
            if (has_karaoke) {
                char hi_col[32];
                if (cl.sub_color_override)
                    snprintf(hi_col, sizeof(hi_col), "0x%02x%02x%02x%02x",
                        (int)(cl.sub_color[0]*255), (int)(cl.sub_color[1]*255),
                        (int)(cl.sub_color[2]*255), (int)(cl.sub_color[3]*255));
                else
                    strcpy(hi_col, "white");

                for (auto* we : clip_words) {
                    std::string vnext = "[vtxt" + std::to_string(txt_idx++) + "]";
                    line() << vcur
                           << "drawtext="
                           << "fontfile=" << esc(g_font_path) << ":"
                           << "text="     << esc(we->text)    << ":"
                           << "fontsize=" << clip_font_sz      << ":"
                           << "fontcolor=" << hi_col           << ":"
                           << "borderw=3:bordercolor=black@0.9:"
                           << "shadowx=2:shadowy=2:shadowcolor=black@0.7:"
                           << "x=(w-text_w)/2:"
                           << "y=" << y_e                     << ":"
                           << "enable='between(t,"
                           << std::fixed << std::setprecision(3)
                           << fmaxf(0.f, we->start - sub_offset) << ","
                           << fmaxf(0.f, we->end   - sub_offset) << ")'"
                           << vnext;
                    vcur = vnext;
                }
            }
        }
    }
    vout_label = vcur;

    // ── Audio mix ─────────────────────────────────────────────────────────────
    if (!aud_ins.empty()) {
        if (aud_ins.size() == 1) {
            char vbuf[32]; snprintf(vbuf, sizeof(vbuf), "%.3f", (double)aud_vols[0]);
            line() << "[" << aud_ins[0] << ":a]volume=" << vbuf << "[aout]";
        } else {
            // Volume-adjust each stream, then amix
            for (int i = 0; i < (int)aud_ins.size(); ++i) {
                char vbuf[32]; snprintf(vbuf, sizeof(vbuf), "%.3f", (double)aud_vols[i]);
                line() << "[" << aud_ins[i] << ":a]volume=" << vbuf << "[amix" << i << "]";
            }
            std::ostringstream& lo = line();
            for (int i = 0; i < (int)aud_ins.size(); ++i) lo << "[amix" << i << "]";
            lo << "amix=inputs=" << aud_ins.size() << ":duration=longest[aout]";
        }
        aout_label = "[aout]";
    }

    std::ofstream f(path);
    if (!f) return false;
    f << fc.str() << "\n";
    return true;
}

// ── Build execvp argument list ────────────────────────────────────────────────

static std::vector<std::string> build_args(AppState& state) {
    if (state.out_mp4.empty()) return {};

    int out_w = 1080, out_h = 1920;
    switch (state.format) {
        case OutputFormat::Horizontal: out_w = 1920; out_h = 1080; break;
        case OutputFormat::Square:     out_w = 1080; out_h = 1080; break;
        default: break;
    }

    // ── Collect unified layer list (bottom-to-top = background first) ────────
    struct VidInput  { std::string path; float ss=0.f, to=-1.f; int in_idx=-1; };
    struct AudioIn   { std::string path; float ss=0.f, to=-1.f; float vol=1.f; };
    std::vector<VidInput> vid_inputs;
    std::vector<RLayer>   layers;
    std::vector<AudioIn>  audio_ins;
    // Primary audio track always included first if set
    if (!state.audio_path.empty()) audio_ins.push_back({state.audio_path, 0.f, -1.f, 1.f});

    // Helper: find or register a video file; returns array index into vid_inputs.
    auto get_vid_input = [&](const std::string& p, float ss, float to) -> int {
        for (int i = 0; i < (int)vid_inputs.size(); ++i)
            if (vid_inputs[i].path == p) return i;
        VidInput vi; vi.path = p; vi.ss = ss; vi.to = to;
        vid_inputs.push_back(vi);
        return (int)vid_inputs.size() - 1;
    };

    for (int ti = (int)state.tracks.size() - 1; ti >= 0; --ti) {
        for (int ci = 0; ci < (int)state.tracks[ti].clips.size(); ++ci) {
            const Clip& cl = state.tracks[ti].clips[ci];
            if (cl.clip_type == ClipType::Effect) {
                continue;  // applied per-layer via collect_effects, not as a render layer
            } else if (cl.clip_type == ClipType::Video) {
                if (cl.text.empty() || !fs::exists(cl.text)) continue;
                int arr_idx = get_vid_input(cl.text, cl.start, cl.end);
                RLayer rl; rl.kind = RLayer::Vid;
                rl.track_idx = ti; rl.clip_idx = ci;
                rl.in_idx    = arr_idx;  // resolved to real ffmpeg idx below
                layers.push_back(rl);
            } else if (cl.clip_type == ClipType::Text   ||
                       cl.clip_type == ClipType::Lyrics ||
                       cl.clip_type == ClipType::Subtitle) {
                RLayer rl; rl.kind = RLayer::Txt;
                rl.track_idx = ti; rl.clip_idx = ci;
                layers.push_back(rl);
            } else if (cl.clip_type == ClipType::Audio) {
                if (cl.text.empty() || !fs::exists(cl.text)) continue;
                float vol = state.tracks[ti].muted ? 0.f : cl.volume;
                // Replace primary audio entry if same path, else add new stream
                bool found = false;
                for (auto& ai : audio_ins) {
                    if (ai.path == cl.text) { ai.ss = cl.start; ai.to = cl.end; ai.vol = vol; found = true; break; }
                }
                if (!found) audio_ins.push_back({cl.text, cl.start, cl.end, vol});
            }
        }
    }

    // Drop invalid audio entries
    audio_ins.erase(std::remove_if(audio_ins.begin(), audio_ins.end(),
        [](const AudioIn& ai){ return ai.path.empty() || !fs::exists(ai.path); }),
        audio_ins.end());
    if (layers.empty() && audio_ins.empty()) return {};

    // Assign real ffmpeg input indices: video files first, then audio streams.
    int n_in = 0;
    for (auto& vi : vid_inputs) vi.in_idx = n_in++;
    std::vector<int>   aud_in_idxs;
    std::vector<float> aud_in_vols;
    for (auto& ai : audio_ins) { aud_in_idxs.push_back(n_in++); aud_in_vols.push_back(ai.vol); }

    // Resolve layer in_idx from vid_inputs array index → actual ffmpeg index,
    // and fill vid_ss for enable expression offset.
    for (auto& rl : layers) {
        if (rl.kind != RLayer::Vid) continue;
        int arr = rl.in_idx;
        rl.vid_ss = vid_inputs[arr].ss;
        rl.in_idx = vid_inputs[arr].in_idx;
    }

    // ── BG remove hires masks (runs synchronously here in render thread) ────────
    struct MaskSpec { std::string dir; float fps; };
    std::map<std::string, MaskSpec> hires_masks;  // keyed by video_path
    for (auto& rl : layers) {
        if (rl.kind != RLayer::Vid) continue;
        const Clip& cl = state.tracks[rl.track_idx].clips[rl.clip_idx];
        if (!cl.bg_remove_on || cl.text.empty()) continue;
        if (hires_masks.count(cl.text)) continue;
        std::string hdir = bg_remove_hires_dir(cl.text);
        bg_remove_run_hires(cl.text, hdir, state.python_path, g_rembg_script);
        float fps = bg_remove_read_fps(hdir);
        hires_masks[cl.text] = {hdir, fps};
    }

    // Assign mask sequence input indices (after video/audio).
    for (auto& rl : layers) {
        if (rl.kind != RLayer::Vid) continue;
        const Clip& cl = state.tracks[rl.track_idx].clips[rl.clip_idx];
        if (!cl.bg_remove_on || cl.text.empty()) continue;
        auto it = hires_masks.find(cl.text);
        if (it == hires_masks.end()) continue;
        if (!fs::exists(it->second.dir + "/fps.txt")) continue;
        rl.mask_in_idx = n_in++;
        rl.mask_fps    = it->second.fps;
    }

    // Flat ffmpeg input list: all video files, then audio streams, then mask sequences.
    struct InSpec { std::string path; float ss=0.f, to=-1.f;
                    bool is_img_seq=false; float img_fps=30.f; };
    std::vector<InSpec> inputs;
    for (auto& vi : vid_inputs) inputs.push_back({vi.path, vi.ss, vi.to});
    for (auto& ai : audio_ins)  inputs.push_back({ai.path, ai.ss, ai.to});
    // Mask sequences: one entry per unique video with bg_remove, in the order assigned above.
    for (auto& rl : layers) {
        if (rl.kind != RLayer::Vid || rl.mask_in_idx < 0) continue;
        const Clip& cl = state.tracks[rl.track_idx].clips[rl.clip_idx];
        auto it = hires_masks.find(cl.text);
        if (it == hires_masks.end()) continue;
        std::string seq_path = it->second.dir + "/%06d.png";
        // Avoid duplicate entries (multiple clips from same video share one proxy).
        bool dup = false;
        for (auto& inp : inputs) { if (inp.path == seq_path) { dup = true; break; } }
        if (!dup) inputs.push_back({seq_path, 0.f, -1.f, true, it->second.fps});
    }

    // sub_offset: project time at which the rendered video begins.
    float sub_offset = (!vid_inputs.empty() && vid_inputs[0].ss > 0.001f)
                     ? vid_inputs[0].ss
                     : (!audio_ins.empty() && audio_ins[0].ss > 0.001f ? audio_ins[0].ss : 0.f);

    float out_duration = state.duration;
    if (!vid_inputs.empty() && vid_inputs[0].to > 0.001f)
        out_duration = vid_inputs[0].to - vid_inputs[0].ss;
    else if (!audio_ins.empty() && audio_ins[0].to > 0.001f)
        out_duration = audio_ins[0].to - audio_ins[0].ss;

    // Write filter script
    std::string script = "/tmp/pms_filter.txt";
    std::string vout, aout;
    if (!write_filter_script(state, script, layers, aud_in_idxs, aud_in_vols,
                             out_w, out_h, sub_offset, vout, aout))
        return {};

    const auto& rs = state.render_settings;

    std::vector<std::string> a;
    a.push_back("ffmpeg");
    a.push_back("-hide_banner");
    a.push_back("-loglevel"); a.push_back("error");
    a.push_back("-progress"); a.push_back("pipe:1");
    a.push_back("-y");

    for (auto& inp : inputs) {
        if (inp.is_img_seq) {
            char fps_buf[32]; snprintf(fps_buf, sizeof(fps_buf), "%.6g", (double)inp.img_fps);
            a.push_back("-f");         a.push_back("image2");
            a.push_back("-framerate"); a.push_back(fps_buf);
        } else {
            if (inp.ss > 0.001f) { a.push_back("-ss"); a.push_back(std::to_string(inp.ss)); }
            if (inp.to > 0.001f) { a.push_back("-to"); a.push_back(std::to_string(inp.to)); }
        }
        a.push_back("-i"); a.push_back(inp.path);
    }

    a.push_back("-filter_complex_script"); a.push_back(script);
    a.push_back("-map"); a.push_back(vout);
    if (!aout.empty()) { a.push_back("-map"); a.push_back(aout); }
    else               { a.push_back("-an"); }

    a.push_back("-c:v");      a.push_back("libx264");
    a.push_back("-crf");      a.push_back(std::to_string(rs.crf));
    a.push_back("-preset");   a.push_back(rs.preset);
    a.push_back("-profile:v"); a.push_back(rs.high_profile ? "high" : "main");
    a.push_back("-level");    a.push_back("4.0");
    a.push_back("-pix_fmt");  a.push_back("yuv420p");
    a.push_back("-movflags"); a.push_back("+faststart");

    if (!aout.empty()) {
        a.push_back("-c:a"); a.push_back("aac");
        a.push_back("-b:a"); a.push_back(std::to_string(rs.audio_bitrate) + "k");
    }

    if (out_duration > 0.f)
        { a.push_back("-t"); a.push_back(std::to_string(out_duration)); }

    a.push_back(state.out_mp4);
    return a;
}

// ── Snapshot arg builder ──────────────────────────────────────────────────────
// No input seeking — the filter runs from t=0 with standard expressions.
// An output-side -ss snap_t discards frames until exactly t=snap_t, so all
// enable windows, lt animation offsets, and KF expressions are correct.

static std::vector<std::string> build_snapshot_args(AppState& state,
                                                     float snap_t,
                                                     const std::string& out_path) {
    int out_w = 1080, out_h = 1920;
    switch (state.format) {
        case OutputFormat::Horizontal: out_w = 1920; out_h = 1080; break;
        case OutputFormat::Square:     out_w = 1080; out_h = 1080; break;
        default: break;
    }

    struct VidInput { std::string path; int in_idx = -1; };
    std::vector<VidInput> vid_inputs;
    std::vector<RLayer>   layers;

    auto get_vid_input = [&](const std::string& p) -> int {
        for (int i = 0; i < (int)vid_inputs.size(); ++i)
            if (vid_inputs[i].path == p) return i;
        VidInput vi; vi.path = p;
        vid_inputs.push_back(vi);
        return (int)vid_inputs.size() - 1;
    };

    for (int ti = (int)state.tracks.size() - 1; ti >= 0; --ti) {
        if (!state.tracks[ti].visible) continue;
        for (int ci = 0; ci < (int)state.tracks[ti].clips.size(); ++ci) {
            const Clip& cl = state.tracks[ti].clips[ci];
            if (cl.clip_type == ClipType::Effect) continue;
            if (cl.clip_type == ClipType::Video) {
                if (cl.text.empty() || !fs::exists(cl.text)) continue;
                int arr_idx = get_vid_input(cl.text);
                RLayer rl; rl.kind = RLayer::Vid;
                rl.track_idx = ti; rl.clip_idx = ci;
                rl.in_idx    = arr_idx;
                layers.push_back(rl);
            } else if (cl.clip_type == ClipType::Text   ||
                       cl.clip_type == ClipType::Lyrics ||
                       cl.clip_type == ClipType::Subtitle) {
                RLayer rl; rl.kind = RLayer::Txt;
                rl.track_idx = ti; rl.clip_idx = ci;
                layers.push_back(rl);
            }
        }
    }
    if (layers.empty()) return {};

    int n_in = 0;
    for (auto& vi : vid_inputs) vi.in_idx = n_in++;

    // vid_ss = 0: no input seeking, so enable expressions use project-absolute times
    for (auto& rl : layers) {
        if (rl.kind != RLayer::Vid) continue;
        rl.vid_ss = 0.f;
        rl.in_idx = vid_inputs[rl.in_idx].in_idx;
    }

    std::string script = "/tmp/pms_snap_filter.txt";
    std::string vout, aout;
    // sub_offset=0, snap_eval_t=-1: standard filter, t=snap_t at the captured frame
    if (!write_filter_script(state, script, layers, {}, {},
                             out_w, out_h, 0.f, vout, aout))
        return {};

    std::vector<std::string> a;
    a.push_back("ffmpeg");
    a.push_back("-hide_banner");
    a.push_back("-loglevel"); a.push_back("error");
    a.push_back("-y");

    for (auto& vi : vid_inputs)
        { a.push_back("-i"); a.push_back(vi.path); }

    a.push_back("-filter_complex_script"); a.push_back(script);
    a.push_back("-map"); a.push_back(vout);
    a.push_back("-an");
    // Output-side seek: ffmpeg decodes from t=0 and discards until exactly snap_t
    a.push_back("-ss"); a.push_back(std::to_string(snap_t));
    a.push_back("-vframes"); a.push_back("1");
    a.push_back("-f"); a.push_back("image2");
    a.push_back(out_path);
    return a;
}

// ── Render thread ─────────────────────────────────────────────────────────────

void render_start(AppState& state) {
    g_cancel.store(false);
    g_ffmpeg_pid.store(0);
    state.render.running  = true;
    state.render.progress = 0.f;
    state.render.frame    = 0;
    state.render.eta_secs = 0.f;
    state.render.stage    = "Building…";

    std::thread([&state]() {
        if (!state.out_srt.empty())
            render_export_srt(state, state.out_srt);

        auto args = build_args(state);
        if (args.empty()) {
            state.render.stage   = "Error: no input files";
            state.render.running = false;
            return;
        }

        // Create pipe for ffmpeg progress output
        int pfd[2];
        if (pipe(pfd) != 0) {
            state.render.stage   = "Error: pipe()";
            state.render.running = false;
            return;
        }

        pid_t pid = fork();
        if (pid < 0) {
            close(pfd[0]); close(pfd[1]);
            state.render.stage   = "Error: fork()";
            state.render.running = false;
            return;
        }

        if (pid == 0) {
            // Child: pipe write end → stdout; stderr → /dev/null
            dup2(pfd[1], STDOUT_FILENO);
            close(pfd[0]);
            close(pfd[1]);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }

            std::vector<char*> argv_ptrs;
            for (auto& s : args) argv_ptrs.push_back(const_cast<char*>(s.c_str()));
            argv_ptrs.push_back(nullptr);
            execvp("ffmpeg", argv_ptrs.data());
            _exit(127);
        }

        // Parent
        close(pfd[1]);
        g_ffmpeg_pid.store(pid);
        state.render.stage = "Encoding…";

        float total_us = state.duration * 1e6f;
        FILE* fp = fdopen(pfd[0], "r");
        char  line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (g_cancel.load()) break;
            if (strncmp(line, "out_time_ms=", 12) == 0 && total_us > 0.f) {
                // Despite the name, FFmpeg outputs microseconds here
                long us = atol(line + 12);
                state.render.progress = fminf(0.99f, (float)us / total_us);
            } else if (strncmp(line, "speed=", 6) == 0 && state.render.progress > 0.f) {
                float spd = atof(line + 6);
                if (spd > 0.01f) {
                    float rem = (1.f - state.render.progress) * state.duration;
                    state.render.eta_secs = rem / spd;
                }
            }
        }
        fclose(fp);

        int wstat = 0;
        waitpid(pid, &wstat, 0);
        g_ffmpeg_pid.store(0);

        bool cancelled = g_cancel.load();
        bool ok = !cancelled && WIFEXITED(wstat) && WEXITSTATUS(wstat) == 0;
        state.render.running  = false;
        state.render.progress = ok ? 1.f : state.render.progress;
        state.render.stage    = cancelled ? "Cancelled" : (ok ? "Done" : "Error — ffmpeg failed");
        if (ok) state.render_done = true;
    }).detach();
}

void render_cancel() {
    g_cancel.store(true);
    pid_t pid = g_ffmpeg_pid.load();
    if (pid > 0) kill(pid, SIGTERM);
}

// ── Frame snapshot ────────────────────────────────────────────────────────────
// Renders one PNG frame at snap_t using the full filter pipeline.

void render_snapshot_start(AppState& state, float snap_t) {
    if (state.snapshot_running) return;

    // Build output path: <stem>_frame_<MM>m<SS>s<mmm>ms.png next to first audio/video file
    std::string base_path = state.audio_path;
    if (base_path.empty()) {
        for (auto& tr : state.tracks)
            for (auto& cl : tr.clips)
                if (cl.clip_type == ClipType::Video && !cl.text.empty())
                    { base_path = cl.text; goto found_base; }
    }
    found_base:
    if (base_path.empty()) return;

    int total_ms = (int)(snap_t * 1000.f);
    int ms = total_ms % 1000, ss = (total_ms / 1000) % 60, mm = total_ms / 60000;
    char ts[32]; snprintf(ts, sizeof(ts), "%02dm%02ds%03dms", mm, ss, ms);
    std::string stem  = fs::path(base_path).stem().string();
    std::string dir   = fs::path(base_path).parent_path().string();
    std::string out   = dir + "/" + stem + "_frame_" + ts + ".png";

    auto args = build_snapshot_args(state, snap_t, out);
    if (args.empty()) {
        state.snapshot_msg     = "Snapshot failed — no active clips";
        state.snapshot_msg_new = true;
        return;
    }

    state.snapshot_running = true;

    std::thread([&state, args, out]() {
        // Write args to log so errors can be diagnosed
        {
            std::ofstream log("/tmp/pms_snap_cmd.txt");
            for (auto& s : args) log << s << " ";
            log << "\n";
        }

        pid_t pid = fork();
        if (pid == 0) {
            // Keep stderr → /tmp/pms_snap_err.txt for diagnostics
            int errfd = open("/tmp/pms_snap_err.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644);
            if (errfd >= 0) { dup2(errfd, STDERR_FILENO); close(errfd); }
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); close(devnull); }
            std::vector<char*> av;
            for (auto& s : args) av.push_back(const_cast<char*>(s.c_str()));
            av.push_back(nullptr);
            execvp("ffmpeg", av.data());
            _exit(127);
        }
        int wstat = 0;
        if (pid > 0) waitpid(pid, &wstat, 0);
        state.snapshot_running = false;
        bool ok = pid > 0 && WIFEXITED(wstat) && WEXITSTATUS(wstat) == 0;
        if (ok) {
            state.snapshot_msg = "Saved " + fs::path(out).filename().string();
            std::string folder = fs::path(out).parent_path().string();
            system(("xdg-open \"" + folder + "\" &").c_str());
        } else {
            state.snapshot_msg = "Snapshot failed";
        }
        state.snapshot_msg_new = true;
    }).detach();
}

// ── Extract raw audio from video ──────────────────────────────────────────────

void extract_audio_start(AppState& state, const std::string& video_path) {
    if (state.extract_running) return;
    state.extract_running = true;
    state.extract_done    = false;
    state.extract_wav_path.clear();

    std::thread([&state, video_path]() {
        fs::path vp(video_path);
        fs::path outdir = vp.parent_path() / vp.stem();
        fs::create_directories(outdir);
        std::string out_wav = (outdir / (vp.stem().string() + "_audio.wav")).string();

        std::vector<std::string> args = {
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-y", "-i", video_path,
            "-vn", "-acodec", "pcm_s16le", out_wav
        };

        pid_t pid = fork();
        if (pid == 0) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
            std::vector<char*> argv_ptrs;
            for (auto& s : args) argv_ptrs.push_back(const_cast<char*>(s.c_str()));
            argv_ptrs.push_back(nullptr);
            execvp("ffmpeg", argv_ptrs.data());
            _exit(127);
        }

        int wstat = 0;
        waitpid(pid, &wstat, 0);

        bool ok = WIFEXITED(wstat) && WEXITSTATUS(wstat) == 0 && fs::exists(out_wav);
        if (ok) state.extract_wav_path = out_wav;
        state.extract_running = false;
        state.extract_done    = ok;
    }).detach();
}
