#include "render.h"
#include "inter_font.h"   // inter_black_ttf[], inter_black_ttf_size
#include "bg_remove.h"
#include "globals.h"
#include "overlay_renderer.h"
#include "video.h"
#include "fx_shader.h"
#include "runtime_fx.h"
#include "body_fx.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <imgui_impl_opengl3.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "stb_image_write.h"
#pragma GCC diagnostic pop

#include "stb_image.h"  // declarations only — implementation lives in video.cpp

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
#include <cstdarg>

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

// ── Helpers ───────────────────────────────────────────────────────────────────

// Find the first non-glass ken_burns Effect brick on any track that overlaps
// [cl_start, cl_end). Returns nullptr if none.
static const Clip* find_ken_burns_brick(const AppState& state, int video_ti,
                                        float cl_start, float cl_end) {
    for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
        for (auto& fc : state.tracks[ti].clips) {
            if (fc.clip_type != ClipType::Effect) continue;
            if (fc.fx_type   != FXType::KenBurns) continue;
            if (fc.end <= cl_start || fc.start >= cl_end) continue;
            if (fx_clip_is_glass(state, ti, fc)) continue;
            return &fc;
        }
    }
    return nullptr;
}

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
            // Ken Burns bricks override scale with a time-varying interpolation.
            {
                const Clip* kb = find_ken_burns_brick(state, rl.track_idx, cl.start, cl.end);
                bool has_sx_kf = cl.ktracks.count("scale_x") > 0;
                bool has_sy_kf = cl.ktracks.count("scale_y") > 0;
                bool need_scale = kb || has_sx_kf || has_sy_kf ||
                                  fabsf(cl.scale_x - 1.f) > 0.001f ||
                                  fabsf(cl.scale_y - 1.f) > 0.001f;
                if (need_scale) {
                    std::string sx, sy;
                    if (kb) {
                        float dur = kb->end - kb->start;
                        if (dur < 0.001f) dur = 0.001f;
                        char buf[256];
                        snprintf(buf, sizeof(buf),
                            "%.4f+%.4f*clip((t+%.4f-%.4f)/%.4f\\,0\\,1)",
                            (double)kb->fx_ken_burns_start_scale,
                            (double)(kb->fx_ken_burns_end_scale - kb->fx_ken_burns_start_scale),
                            (double)rl.vid_ss, (double)kb->start, (double)dur);
                        sx = buf; sy = buf;
                    } else {
                        sx = prop_expr(cl, "scale_x", 1.f, cl.scale_x, snap_eval_t);
                        sy = prop_expr(cl, "scale_y", 1.f, cl.scale_y, snap_eval_t);
                    }
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
                    if (fc.fx_type == FXType::Grade    ||
                        fc.fx_type == FXType::Blur     ||
                        fc.fx_type == FXType::Vignette) {
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
                    if (fc.fx_type == FXType::ZoomPunch && fc.fx_zoom_strength > 0.001f) {
                        const std::vector<float>* beat_src_vec = nullptr;
                        if (fc.beat_src_track >= 0 && fc.beat_src_clip >= 0 &&
                            fc.beat_src_track < (int)state.tracks.size()) {
                            const auto& btr = state.tracks[fc.beat_src_track];
                            if (fc.beat_src_clip < (int)btr.clips.size())
                                beat_src_vec = &btr.clips[fc.beat_src_clip].beats;
                        }
                        if (beat_src_vec && !beat_src_vec->empty()) {
                            // Build max-of-decays expression over all beats in the clip window
                            std::string zoom_e = "0";
                            float str   = fc.fx_zoom_strength;
                            float decay = fmaxf(0.05f, fc.fx_zoom_decay);
                            for (float bt : *beat_src_vec) {
                                if (bt < fc.start || bt >= fc.end) continue;
                                float bt_rel = fmaxf(0.f, bt - rl.vid_ss);
                                char term[128];
                                snprintf(term, sizeof(term),
                                    "if(gte(t,%.3f),%.4f*exp(-(t-%.3f)/%.4f),0)",
                                    (double)bt_rel, (double)str, (double)bt_rel, (double)decay);
                                zoom_e = "max(" + zoom_e + "," + term + ")";
                            }
                            if (zoom_e != "0") {
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
                } // for fc
            } // for fti

            // Position — all layers use pos_x/pos_y; eval=frame when KFs animate it.
            // Ken Burns bricks override position with a time-varying interpolation.
            // overlay filter uses overlay_w/overlay_h (not iw/ih) for the overlay input dimensions.
            std::string x_e, y_e;
            bool has_pos_kf;
            {
                const Clip* kb = find_ken_burns_brick(state, rl.track_idx, cl.start, cl.end);
                if (kb) {
                    float dur = kb->end - kb->start;
                    if (dur < 0.001f) dur = 0.001f;
                    char xbuf[256], ybuf[256];
                    snprintf(xbuf, sizeof(xbuf),
                        "((%.4f+%.4f*clip((t+%.4f-%.4f)/%.4f\\,0\\,1))*%d-overlay_w/2)",
                        (double)kb->fx_ken_burns_start_x,
                        (double)(kb->fx_ken_burns_end_x - kb->fx_ken_burns_start_x),
                        (double)rl.vid_ss, (double)kb->start, (double)dur, out_w);
                    snprintf(ybuf, sizeof(ybuf),
                        "((%.4f+%.4f*clip((t+%.4f-%.4f)/%.4f\\,0\\,1))*%d-overlay_h/2)",
                        (double)kb->fx_ken_burns_start_y,
                        (double)(kb->fx_ken_burns_end_y - kb->fx_ken_burns_start_y),
                        (double)rl.vid_ss, (double)kb->start, (double)dur, out_h);
                    x_e = xbuf; y_e = ybuf;
                    has_pos_kf = true;
                } else {
                    x_e = "(" + prop_expr(cl, "pos_x", (float)out_w, cl.pos_x, snap_eval_t) + "-overlay_w/2)";
                    y_e = "(" + prop_expr(cl, "pos_y", (float)out_h, cl.pos_y, snap_eval_t) + "-overlay_h/2)";
                    has_pos_kf = cl.ktracks.count("pos_x") > 0 || cl.ktracks.count("pos_y") > 0;
                }
            }

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
            else if (cl.sub_pos == 2) y_e = "h*0.08";          // SAFE_TOP
            else if (cl.sub_pos == 3) {
                char yb[64];
                snprintf(yb, sizeof(yb), "h*%.4f-text_h/2", (double)cl.sub_pos_y);
                y_e = yb;
            } else {
                y_e = "h*0.80-text_h";                          // 1 - SAFE_BOT
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

            // x expression: position text according to sub_anchor_h
            // 0=left edge at sub_pos_x, 1=center at sub_pos_x, 2=right edge at sub_pos_x
            char x_base_buf[64];
            if (cl.sub_anchor_h == 0)
                snprintf(x_base_buf, sizeof(x_base_buf), "(w*%.4f)", (double)cl.sub_pos_x);
            else if (cl.sub_anchor_h == 2)
                snprintf(x_base_buf, sizeof(x_base_buf), "(w*%.4f-text_w)", (double)cl.sub_pos_x);
            else
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
            if (cl.clip_type == ClipType::Effect || cl.clip_type == ClipType::MultiFX) {
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
        bg_remove_run_hires(cl.text, hdir);
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
            if (cl.clip_type == ClipType::Effect || cl.clip_type == ClipType::MultiFX) continue;
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
        std::string out_audio = (outdir / (vp.stem().string() + "_audio.webm")).string();

        std::string err = video_extract_segment(video_path, 0.0, 1e9, out_audio);
        bool ok = err.empty() && fs::exists(out_audio);
        if (ok) state.extract_wav_path = out_audio;
        state.extract_running = false;
        state.extract_done    = ok;
    }).detach();
}

// ── GL shared state — forward declarations used by snapshot + export ─────────

static struct GlExport {
    bool    active        = false;
    GLuint  fbo           = 0;
    GLuint  color_tex     = 0;
    GLuint  vid_tex[MAX_VIDEO_TRACKS * 2] = {};
    int     out_w         = 0;
    int     out_h         = 0;
    int     current_frame = 0;
    int     total_frames  = 0;
    float   fps_f         = 30.f;
    int     pipe_write    = -1;
    pid_t   ffmpeg_pid    = 0;
    std::vector<uint8_t> pixel_buf;
    // Double-buffered PBOs: GPU DMAs frame N into pbo[N%2] while CPU reads pbo[(N-1)%2].
    GLuint  pbo[2]        = {};
    bool    use_vaapi     = false;  // h264_vaapi encoder active
} g_gl_ex;

static void gl_cleanup_export();
static bool gl_render_vid_clip(ImDrawList& dl, const Clip* cl, float at_time,
                                float alpha_mul, GLuint tex_id, int fx_slot,
                                float W, float H, const AppState& state, int ti,
                                bool use_scene = false);

// ── GL snapshot — identical to preview ───────────────────────────────────────

void render_snapshot_gl(AppState& state, float snap_t) {
    if (state.snapshot_running) return;

    // Build output path (same logic as render_snapshot_start)
    std::string base_path = state.audio_path;
    if (base_path.empty()) {
        for (auto& tr : state.tracks)
            for (auto& cl : tr.clips)
                if (cl.clip_type == ClipType::Video && !cl.text.empty())
                    { base_path = cl.text; goto snap_found_base; }
    }
    snap_found_base:
    if (base_path.empty()) {
        state.snapshot_msg     = "Snapshot failed — no media loaded";
        state.snapshot_msg_new = true;
        return;
    }

    int total_ms = (int)(snap_t * 1000.f);
    int ms = total_ms % 1000, ss = (total_ms / 1000) % 60, mm = total_ms / 60000;
    char ts[32]; snprintf(ts, sizeof(ts), "%02dm%02ds%03dms", mm, ss, ms);
    std::string stem = fs::path(base_path).stem().string();
    std::string dir  = fs::path(base_path).parent_path().string();
    std::string out  = dir + "/" + stem + "_frame_" + ts + ".png";

    int out_w = 1080, out_h = 1920;
    switch (state.format) {
        case OutputFormat::Horizontal: out_w = 1920; out_h = 1080; break;
        case OutputFormat::Square:     out_w = 1080; out_h = 1080; break;
        default: break;
    }
    float W = (float)out_w, H = (float)out_h;
    float t = snap_t;

    // ── Create temporary FBO ──────────────────────────────────────────────────
    GLuint fbo = 0, col_tex = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &col_tex);
    glBindTexture(GL_TEXTURE_2D, col_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, out_w, out_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, col_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &col_tex);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        state.snapshot_msg     = "Snapshot failed — FBO error";
        state.snapshot_msg_new = true;
        return;
    }

    // Per-track video textures for this snapshot (freed at end)
    GLuint vid_texs[MAX_VIDEO_TRACKS * 2] = {};
    glGenTextures(MAX_VIDEO_TRACKS * 2, vid_texs);
    for (int i = 0; i < MAX_VIDEO_TRACKS * 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, vid_texs[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    // ── Render into FBO ───────────────────────────────────────────────────────
    GLint prev_fbo = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    GLint prev_vp[4]; glGetIntegerv(GL_VIEWPORT, prev_vp);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, out_w, out_h);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImDrawList dl(ImGui::GetDrawListSharedData());
    dl._ResetForNewFrame();
    dl.PushClipRect({0.f, 0.f}, {W, H});
    dl.PushTexture(ImGui::GetIO().Fonts->TexRef);

    // Reuse the same video-clip rendering path as render_tick_gl.
    // Per-slot decoders self-track their open file, so no path-cache bookkeeping needed.
    for (int ti = (int)state.tracks.size() - 1; ti >= 0; --ti) {
        const auto& track = state.tracks[ti];
        if (!track.visible) continue;
        int slot_pri = ti % MAX_VIDEO_TRACKS;
        int slot_sec = MAX_VIDEO_TRACKS + slot_pri;

        // ── Background (color-pattern) clips ─────────────────────────────────
        for (auto& bg_cl : track.clips) {
            if (bg_cl.clip_type != ClipType::Background) continue;
            if (t < bg_cl.start || t >= bg_cl.end) continue;
            if (bg_cl.text.empty()) break;

            int bg_slot_idx = ti % MAX_BG_SLOTS;
            uintptr_t bg_tex = bg_render_to_texture(
                bg_cl.text.c_str(), bg_slot_idx, out_w, out_h,
                t, bg_cl.bg_speed, bg_cl.bg_intensity,
                bg_cl.bg_c1, bg_cl.bg_c2, bg_cl.bg_c3);
            if (!bg_tex) break;

            float bpx = bg_cl.eval_prop("pos_x",    t);
            float bpy = bg_cl.eval_prop("pos_y",    t);
            float bsx = bg_cl.eval_prop("scale_x",  t);
            float bsy = bg_cl.eval_prop("scale_y",  t);
            float brt = bg_cl.eval_prop("rotation", t);
            float ba  = bg_cl.eval_prop("opacity",  t);
            float bcx = bpx * W, bcy = bpy * H;
            float bhw = W * bsx * 0.5f, bhh = H * bsy * 0.5f;
            float brad = brt * 3.14159265f / 180.f;
            float bcos = cosf(brad), bsin = sinf(brad);
            auto brot = [&](float ox, float oy) -> ImVec2 {
                return { bcx + ox*bcos - oy*bsin, bcy + ox*bsin + oy*bcos };
            };
            ImU32 bcol = IM_COL32(255, 255, 255,
                                  (int)(fmaxf(0.f, fminf(1.f, ba)) * 255.f));
            dl.AddImageQuad(ImTextureRef((ImTextureID)bg_tex),
                brot(-bhw, -bhh), brot(bhw, -bhh), brot(bhw, bhh), brot(-bhw, bhh),
                {0,1}, {1,1}, {1,0}, {0,0}, bcol);
            break;
        }

        // ── Video clips ───────────────────────────────────────────────────────
        const Clip* active = nullptr; int active_ci = -1;
        for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
            auto& cl = track.clips[ci];
            if (cl.clip_type == ClipType::Video && t >= cl.start && t < cl.end)
                { active = &cl; active_ci = ci; break; }
        }
        if (!active) {
            for (int ci = 1; ci < (int)track.clips.size(); ++ci) {
                const Clip& prev = track.clips[ci - 1];
                const Clip& cl   = track.clips[ci];
                if (prev.clip_type != ClipType::Video || cl.clip_type != ClipType::Video) continue;
                if (prev.transition_type == TransitionType::None || prev.transition_post <= 0.f) continue;
                if (t >= cl.start && t < cl.start + prev.transition_post)
                    { active = &track.clips[ci]; active_ci = ci; break; }
            }
        }
        if (!active) continue;

        bool in_trans_out = (active->transition_type != TransitionType::None &&
                             active->transition_pre > 0.f &&
                             t >= active->end - active->transition_pre);
        const Clip* next_cl = nullptr;
        if (in_trans_out && active_ci + 1 < (int)track.clips.size()) {
            const Clip& nc = track.clips[active_ci + 1];
            if (nc.clip_type == ClipType::Video) next_cl = &nc;
        }
        bool in_trans_in = false; const Clip* prev_cl = nullptr;
        if (!in_trans_out && active_ci > 0) {
            const Clip& pc = track.clips[active_ci - 1];
            if (pc.clip_type == ClipType::Video &&
                pc.transition_type != TransitionType::None &&
                pc.transition_post > 0.f &&
                t < active->start + pc.transition_post)
                { in_trans_in = true; prev_cl = &pc; }
        }

        if (in_trans_out && next_cl) {
            float pre = active->transition_pre, post = active->transition_post, cut = active->end;
            float t_a = fmaxf(0.f, fminf(1.f, (t-(cut-pre))/fmaxf(pre,1e-5f)));
            float t_b = fmaxf(0.f, fminf(1.f, (t-cut)/fmaxf(post,1e-5f)));
            if (active->transition_type == TransitionType::Dissolve) {
                gl_render_vid_clip(dl, active, t, 1.f-t_a, vid_texs[slot_pri], slot_pri, W, H, state, ti);
                gl_render_vid_clip(dl, next_cl, t, t_b>0.f?t_b:t_a, vid_texs[slot_sec], slot_sec, W, H, state, ti);
            } else if (active->transition_type == TransitionType::FadeBlack) {
                gl_render_vid_clip(dl, active, t, 1.f-t_a, vid_texs[slot_pri], slot_pri, W, H, state, ti);
                gl_render_vid_clip(dl, next_cl, t, t_b, vid_texs[slot_sec], slot_sec, W, H, state, ti);
            } else {
                gl_render_vid_clip(dl, active, t, 1.f-t_a, vid_texs[slot_pri], slot_pri, W, H, state, ti);
                float wa = t_a*(1.f-t_b);
                if (wa > 0.01f)
                    dl.AddRectFilled({0,0},{W,H},IM_COL32(255,255,255,(int)(wa*255)));
                gl_render_vid_clip(dl, next_cl, t, t_b, vid_texs[slot_sec], slot_sec, W, H, state, ti);
            }
        } else if (in_trans_in && prev_cl) {
            float tf = fmaxf(0.f, fminf(1.f,
                (t-active->start)/fmaxf(prev_cl->transition_post,1e-5f)));
            if (prev_cl->transition_type == TransitionType::Dissolve) {
                gl_render_vid_clip(dl, prev_cl, fminf(t,prev_cl->end-1e-4f),
                                   1.f-tf, vid_texs[slot_sec], slot_sec, W, H, state, ti);
                gl_render_vid_clip(dl, active, t, tf, vid_texs[slot_pri], slot_pri, W, H, state, ti);
            } else if (prev_cl->transition_type == TransitionType::FadeBlack) {
                gl_render_vid_clip(dl, active, t, tf, vid_texs[slot_pri], slot_pri, W, H, state, ti);
            } else {
                float wa = 1.f-tf;
                if (wa > 0.01f)
                    dl.AddRectFilled({0,0},{W,H},IM_COL32(255,255,255,(int)(wa*255)));
                gl_render_vid_clip(dl, active, t, tf, vid_texs[slot_pri], slot_pri, W, H, state, ti);
            }
        } else {
            gl_render_vid_clip(dl, active, t, 1.f, vid_texs[slot_pri], slot_pri, W, H, state, ti);
        }
    }
    video_close_export_all();

    draw_text_overlays(&dl, state, t, {0.f, 0.f}, W, H);
    dl.PopTexture();
    dl.PopClipRect();

    ImDrawData dd;
    dd.DisplayPos = {0,0}; dd.DisplaySize = {W,H}; dd.FramebufferScale = {1,1};
    // Use atlas->TexList directly — PlatformIO.Textures is rebuilt at the END
    // of each frame, so it misses new ImTextureData* objects created by
    // draw_text_overlays when first rendering at the export resolution (new
    // font size triggers ImFontAtlasTextureAdd → WantCreate + TexID=Invalid).
    dd.Textures = &ImGui::GetIO().Fonts->TexList;
    dd.AddDrawList(&dl);
    ImGui_ImplOpenGL3_RenderDrawData(&dd);

    // ── Read back + vertical flip ─────────────────────────────────────────────
    std::vector<uint8_t> raw((size_t)out_w * out_h * 4);
    glReadPixels(0, 0, out_w, out_h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
    std::vector<uint8_t> flipped((size_t)out_w * out_h * 4);
    int rb = out_w * 4;
    for (int y = 0; y < out_h; ++y)
        memcpy(flipped.data() + (size_t)y*rb, raw.data() + (size_t)(out_h-1-y)*rb, rb);

    // ── Restore GL state ──────────────────────────────────────────────────────
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &col_tex);
    glDeleteTextures(MAX_VIDEO_TRACKS * 2, vid_texs);

    // ── Save PNG (synchronous, small file — fine on main thread) ─────────────
    state.snapshot_running = true;
    bool ok = stbi_write_png(out.c_str(), out_w, out_h, 4, flipped.data(), rb) != 0;
    state.snapshot_running = false;
    if (ok) {
        state.snapshot_msg = "Saved " + fs::path(out).filename().string();
        system(("xdg-open \"" + dir + "\" &").c_str());
    } else {
        state.snapshot_msg = "Snapshot failed — PNG write error";
    }
    state.snapshot_msg_new = true;
}

// ── GL-based export ───────────────────────────────────────────────────────────
// One export frame is rendered per call to render_tick_gl(), driven from app_frame()
// on the main/GL thread. Each frame is decoded from the original source files via
// libavcodec, composited via ImDrawList into an offscreen FBO, read back with
// glReadPixels, and piped as rawvideo RGBA to ffmpeg for H.264 encoding.

// ── Render crash log ─────────────────────────────────────────────────────────
// Written incrementally; last flushed line shows where a crash occurred.
// Log lives at /tmp/pms_render_log.txt.
static FILE* g_render_log = nullptr;

static void rlog(const char* fmt, ...) {
    if (!g_render_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_render_log, fmt, ap); va_end(ap);
    fflush(g_render_log);
}

static void gl_cleanup_export() {
    if (g_gl_ex.fbo)       { glDeleteFramebuffers(1, &g_gl_ex.fbo);       g_gl_ex.fbo       = 0; }
    if (g_gl_ex.color_tex) { glDeleteTextures(1, &g_gl_ex.color_tex);     g_gl_ex.color_tex = 0; }
    glDeleteTextures(MAX_VIDEO_TRACKS * 2, g_gl_ex.vid_tex);
    memset(g_gl_ex.vid_tex, 0, sizeof(g_gl_ex.vid_tex));
    if (g_gl_ex.pbo[0]) { glDeleteBuffers(2, g_gl_ex.pbo); g_gl_ex.pbo[0] = g_gl_ex.pbo[1] = 0; }
    video_close_export_all();
    g_gl_ex.pixel_buf.clear();
    g_gl_ex.use_vaapi = false;
    g_gl_ex.active = false;
}

// Decode a video clip frame, upload to a GL texture, and AddImageQuad to dl.
// Returns false if the clip has no file or decode fails.
static bool is_still_ext(const std::string& path) {
    auto ext = fs::path(path).extension().string();
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext==".jpg"||ext==".jpeg"||ext==".png"||ext==".bmp"||ext==".webp"
        || ext==".tiff"||ext==".heic"||ext==".heif";
}

static bool gl_render_vid_clip(ImDrawList& dl, const Clip* cl, float at_time,
                                float alpha_mul, GLuint tex_id, int fx_slot,
                                float W, float H, const AppState& state, int ti,
                                bool use_scene)
{
    if (!cl || cl->text.empty()) return false;
    float src_t = cl->in_point + (at_time - cl->start) * cl->speed;

    // Still images (HEIC, JPEG, PNG…): FFmpeg can't reliably decode these,
    // especially HEIC without libheif. Use the proxy JPEG via stb_image instead.
    if (is_still_ext(cl->text)) {
        std::string still = proxy_still_path(cl->text);
        if (!fs::exists(still)) return false;
        int sw = 0, sh = 0, sc = 0;
        uint8_t* px = stbi_load(still.c_str(), &sw, &sh, &sc, 4);
        if (!px) return false;
        glBindTexture(GL_TEXTURE_2D, tex_id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        stbi_image_free(px);
        int vid_w = sw, vid_h = sh;

        uintptr_t cur_tex = (uintptr_t)tex_id;
        {
            EffectAccum glass_ea = collect_glass_effects(state, at_time, ti);
            CreativeFXAccum glass_cfx = collect_glass_fx(state, at_time, ti);
            if (glass_cfx.any_gen_fx || glass_cfx.any_cfx ||
                glass_ea.any_color || glass_ea.any_blur ||
                glass_ea.any_vignette || glass_ea.any_text)
                cur_tex = fx_apply(cur_tex, fx_slot, vid_w, vid_h, glass_ea, glass_cfx, at_time);
        }

        float px_ = cl->eval_prop("pos_x",    at_time);
        float py_ = cl->eval_prop("pos_y",    at_time);
        float sx  = cl->eval_prop("scale_x",  at_time);
        float sy_ = cl->eval_prop("scale_y",  at_time);
        float rot = cl->eval_prop("rotation", at_time);
        float alpha = cl->eval_prop("opacity", at_time) * alpha_mul;

        float fit_w = W, fit_h = H;
        if (vid_w > 0 && vid_h > 0) {
            float va = (float)vid_w / (float)vid_h, ca = W / H;
            if (va > ca) { fit_w = W; fit_h = W / va; }
            else         { fit_h = H; fit_w = H * va; }
        }
        float cx = px_ * W, cy = py_ * H;
        float hw = fit_w * sx * 0.5f, hh = fit_h * sy_ * 0.5f;
        float rad = rot * 3.14159265f / 180.f;
        float cos_r = cosf(rad), sin_r = sinf(rad);
        auto rot_pt = [&](float ox, float oy) -> ImVec2 {
            return { cx + ox*cos_r - oy*sin_r, cy + ox*sin_r + oy*cos_r };
        };
        if (use_scene) {
            scene_add_layer(cur_tex, cx, cy, hw, hh, cos_r, sin_r,
                            fmaxf(0.f, fminf(1.f, alpha)));
        } else {
            ImVec2 uv0{0,0}, uv1{1,1};
            ImU32 col = IM_COL32(255,255,255,(int)(alpha*255));
            dl.AddImageQuad((ImTextureID)(uintptr_t)cur_tex,
                rot_pt(-hw,-hh), rot_pt(hw,-hh), rot_pt(hw,hh), rot_pt(-hw,hh),
                uv0, {uv1.x,uv0.y}, uv1, {uv0.x,uv1.y}, col);
        }
        return true;
    }

    // Per-slot decoder: fx_slot (== decoder slot) self-tracks which file it has open.
    if (!video_open_export(fx_slot, cl->text)) return false;
    VideoFrame* vf = video_decode_frame_at(fx_slot, (double)src_t);
    if (!vf) return false;

    // CPU datamosh — must happen before GL upload.
    // Collect both global and glass FX now so glass datamosh also runs pre-upload.
    CreativeFXAccum cfx      = collect_creative_fx(state, at_time, ti);
    CreativeFXAccum glass_cfx = collect_glass_fx  (state, at_time, ti);
    if (cfx.datamosh_on && cfx.datamosh_intensity > 0.01f)
        video_apply_datamosh(vf, cfx.datamosh_intensity, (float)src_t);
    if (glass_cfx.datamosh_on && glass_cfx.datamosh_intensity > 0.01f)
        video_apply_datamosh(vf, glass_cfx.datamosh_intensity, (float)src_t);

    // AI bg-remove: apply mask alpha before GL upload so the compositor can
    // correctly composite over background layers.
    if (cl->bg_remove_on && cl->bg_remove_status == BgRemoveStatus::Ready
            && !cl->bg_remove_mask_dir.empty()) {
        float mask_fps = bg_remove_read_fps(cl->bg_remove_mask_dir);
        int   frame_i  = (int)(src_t * mask_fps);
        video_apply_bg_remove_export(vf, *cl, frame_i);
    }

    glBindTexture(GL_TEXTURE_2D, tex_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vf->width, vf->height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, vf->data);
    int vid_w = vf->width, vid_h = vf->height;
    video_free_frame(vf);

    // Pre-composite: glass FX/adjustments on the same track as this video clip.
    uintptr_t cur_tex = (uintptr_t)tex_id;
    {
        EffectAccum glass_ea = collect_glass_effects(state, at_time, ti);
        if (glass_cfx.any_gen_fx || glass_cfx.any_cfx ||
            glass_ea.any_color || glass_ea.any_blur ||
            glass_ea.any_vignette || glass_ea.any_text)
            cur_tex = fx_apply(cur_tex, fx_slot, vid_w, vid_h, glass_ea, glass_cfx, at_time);
    }

    // Glass BodyFX: standalone bricks and MultiFX sub-effects on this track.
    // Both use the video clip's own bg_remove masks (hires at export time).
    if (cl->bg_remove_status == BgRemoveStatus::Ready && !cl->bg_remove_mask_dir.empty()) {
        std::string mask_dir = cl->bg_remove_mask_dir;
        float mask_fps = bg_remove_read_fps(mask_dir);
        float bfx_src_t = cl->in_point + (at_time - cl->start) / cl->speed;
        int frame_i = (int)(bfx_src_t * mask_fps);

        // Standalone glass BodyFX bricks on this track
        for (auto& bfx_cl : state.tracks[ti].clips) {
            if (bfx_cl.clip_type != ClipType::BodyFX) continue;
            if (at_time < bfx_cl.start || at_time >= bfx_cl.end) continue;
            unsigned mask_tex = body_fx_mask_texture(mask_dir, frame_i);
            if (!mask_tex) continue;
            cur_tex = body_fx_apply(bfx_cl.body_fx_type, cur_tex, mask_tex,
                                    vid_w, vid_h, bfx_cl.body_fx_params,
                                    bfx_cl.body_fx_amount, at_time);
        }

        // BodyFX sub-effects inside glass MultiFX bricks on this track
        for (auto& mfx_cl : state.tracks[ti].clips) {
            if (mfx_cl.clip_type != ClipType::MultiFX) continue;
            if (at_time < mfx_cl.start || at_time >= mfx_cl.end) continue;
            if (!fx_clip_is_glass(state, ti, mfx_cl)) continue;
            float rel = at_time - mfx_cl.start;
            float parent_dur = mfx_cl.end - mfx_cl.start;
            for (auto& se : mfx_cl.fx_chain) {
                if (se.clip_type != ClipType::BodyFX) continue;
                float se_end = (se.rel_end <= 0.f) ? parent_dur : se.rel_end;
                if (rel < se.rel_start || rel >= se_end) continue;
                unsigned mask_tex = body_fx_mask_texture(mask_dir, frame_i);
                if (!mask_tex) continue;
                cur_tex = body_fx_apply(se.body_fx_type, cur_tex, mask_tex,
                                        vid_w, vid_h, se.body_fx_params,
                                        se.body_fx_amount, at_time);
            }
        }
    }

    // RuntimeFX — custom hot-reload shader on this clip.
    if (!cl->runtime_fx_id.empty())
        cur_tex = runtime_fx_apply(cl->runtime_fx_id, cur_tex, vid_w, vid_h,
                                   cl->runtime_fx_params, cl->runtime_fx_amount, at_time);

    // BodyFX is now a solid brick on its own track — applied post-composite (see below).

    // Global FX are applied once to the full composited frame after all clips are
    // rendered — not per-clip. See render_tick_gl post-process step.
    uintptr_t draw_tex = cur_tex;

    // ZoomPunch — beat-synced scale spike, same logic as preview
    float px    = cl->eval_prop("pos_x",    at_time);
    float py    = cl->eval_prop("pos_y",    at_time);
    float sx    = cl->eval_prop("scale_x",  at_time);
    float sy    = cl->eval_prop("scale_y",  at_time);
    float rot   = cl->eval_prop("rotation", at_time);
    float alpha = cl->eval_prop("opacity",  at_time) * alpha_mul;

    float fit_w = W, fit_h = H;
    if (vid_w > 0 && vid_h > 0) {
        float vid_asp = (float)vid_w / (float)vid_h;
        float can_asp = W / H;
        if (vid_asp > can_asp) { fit_w = W; fit_h = W / vid_asp; }
        else                   { fit_h = H; fit_w = H * vid_asp; }
    }
    float cx = px * W, cy = py * H;
    float hw = fit_w * sx * 0.5f, hh = fit_h * sy * 0.5f;

    if (cfx.zoom_on) {
        float pulse = beat_pulse_at(state, cfx.zoom_src_track, cfx.zoom_src_clip, at_time, cfx.zoom_decay);
        float punch = cfx.zoom_strength * pulse;
        if (punch > 0.001f) {
            float sf = 1.f + punch;
            hw *= sf; hh *= sf;
            if (cfx.zoom_shake > 0.f) {
                float tf = floorf(at_time * 60.f);
                float sa = cfx.zoom_shake * punch * W * 0.025f;
                cx += sinf(tf * 127.1f) * sa;
                cy += cosf(tf * 311.7f) * sa;
            }
        }
    }

    if (cfx.ken_burns_on) {
        float p   = cfx.ken_burns_progress;
        float kbs = cfx.ken_burns_start_scale + (cfx.ken_burns_end_scale - cfx.ken_burns_start_scale) * p;
        float kbx = cfx.ken_burns_start_x     + (cfx.ken_burns_end_x     - cfx.ken_burns_start_x)     * p;
        float kby = cfx.ken_burns_start_y     + (cfx.ken_burns_end_y     - cfx.ken_burns_start_y)     * p;
        hw = fit_w * kbs * 0.5f;
        hh = fit_h * kbs * 0.5f;
        cx = kbx * W;
        cy = kby * H;
    }

    float rad = rot * 3.14159265f / 180.f;
    float cos_r = cosf(rad), sin_r = sinf(rad);
    auto rot_pt = [&](float ox, float oy) -> ImVec2 {
        return { cx + ox*cos_r - oy*sin_r, cy + ox*sin_r + oy*cos_r };
    };
    if (use_scene) {
        scene_add_layer(draw_tex, cx, cy, hw, hh, cos_r, sin_r,
                        fmaxf(0.f, fminf(1.f, alpha)));
    } else {
        ImU32 col = IM_COL32(255, 255, 255, (int)(fmaxf(0.f, fminf(1.f, alpha)) * 255.f));
        dl.AddImageQuad(ImTextureRef((ImTextureID)draw_tex),
            rot_pt(-hw,-hh), rot_pt(hw,-hh), rot_pt(hw,hh), rot_pt(-hw,hh),
            {0,0}, {1,0}, {1,1}, {0,1}, col);
    }
    return true;
}

void render_start_gl(AppState& state) {
    g_cancel.store(false);

    // Open crash log — each line is flushed so the last line before a crash is visible.
    if (g_render_log) fclose(g_render_log);
    g_render_log = fopen("/tmp/pms_render_log.txt", "w");
    rlog("render_start_gl: out_mp4=%s duration=%.2f fps=%d\n",
         state.out_mp4.c_str(), (double)state.duration, state.fps);

    int out_w = 1080, out_h = 1920;
    switch (state.format) {
        case OutputFormat::Horizontal: out_w = 1920; out_h = 1080; break;
        case OutputFormat::Square:     out_w = 1080; out_h = 1080; break;
        default: break;
    }
    int fps          = state.fps;
    int total_frames = (int)(state.duration * fps + 0.5f);
    if (total_frames <= 0 || state.out_mp4.empty()) return;

    // ── Create FBO ────────────────────────────────────────────────────────────
    GLuint fbo = 0, col_tex = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glGenTextures(1, &col_tex);
    glBindTexture(GL_TEXTURE_2D, col_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, out_w, out_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, col_tex, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &col_tex);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        state.render.stage = "Error: FBO setup failed";
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // ── Create per-track video textures ───────────────────────────────────────
    GLuint vid_texs[MAX_VIDEO_TRACKS * 2] = {};
    glGenTextures(MAX_VIDEO_TRACKS * 2, vid_texs);
    for (int i = 0; i < MAX_VIDEO_TRACKS * 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, vid_texs[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    // GIF exports have no audio — skip collection entirely.
    const bool is_gif = state.render_settings.gif_export;

    // ── Collect audio inputs ──────────────────────────────────────────────────
    // Each audio clip gets its own ffmpeg input with correct -ss / -to / -itsoffset
    // so that:
    //   ss    = clip.in_point   (start position in the source file)
    //   to    = ss + (end-start)*speed   (end position in the source file)
    //   delay = clip.start      (-itsoffset: where it lands on the output timeline)
    struct AudioIn {
        std::string path;
        float vol   = 1.f;
        float ss    = 0.f;   // source seek (-ss before -i)
        float to    = -1.f;  // source end  (-to before -i, -1 = no limit)
        float delay = 0.f;   // timeline offset (-itsoffset before -i)
    };
    std::vector<AudioIn> audio_ins;
    if (!is_gif) {
        // state.audio_path is the primary audio file (extracted stem / uploaded track).
        // The preview audio callback does NOT play g_samples (audio_path content) directly —
        // it only mixes Audio brick clips.  To keep export consistent with preview, only
        // add audio_path as a background input when no Audio brick is already sourced
        // from the same file.  If a brick covers it, the bricks are the sole audio source.
        if (!state.audio_path.empty()) {
            bool covered_by_brick = false;
            for (auto& tr : state.tracks)
                for (auto& cl : tr.clips)
                    if (cl.clip_type == ClipType::Audio && cl.text == state.audio_path)
                        { covered_by_brick = true; break; }
            if (!covered_by_brick)
                audio_ins.push_back({state.audio_path, 1.f, 0.f, -1.f, 0.f});
        }
        for (int ti = (int)state.tracks.size() - 1; ti >= 0; --ti) {
            for (auto& cl : state.tracks[ti].clips) {
                if (cl.clip_type != ClipType::Audio || cl.text.empty()) continue;
                if (!fs::exists(cl.text)) continue;
                float vol   = state.tracks[ti].muted ? 0.f : cl.volume;
                float ss    = cl.in_point;
                float dur   = (cl.end - cl.start) * fmaxf(0.01f, cl.speed);
                float to    = ss + dur;
                // Modern FFmpeg keeps absolute timestamps after -ss (input option),
                // so the stream's pts starts at ~in_point, not 0.  To place audio at
                // cl.start on the output timeline we need itsoffset = cl.start - in_point,
                // not cl.start.  Clamped to 0 — negative itsoffset is unsupported.
                float delay = fmaxf(0.f, cl.start - cl.in_point);
                audio_ins.push_back({cl.text, vol, ss, to, delay});
            }
        }
    }

    // ── VAAPI detection ───────────────────────────────────────────────────────
    // Use AMD/Intel VAAPI hardware encoder when the render node exists and the
    // user hasn't disabled it.  Encodes on dedicated GPU silicon → ~10-20× faster
    // than libx264, freeing the CPU entirely for the next frame's GL work.
    bool use_vaapi = state.render_settings.use_vaapi &&
                     fs::exists("/dev/dri/renderD128");

    // ── Build ffmpeg command ──────────────────────────────────────────────────
    // Input 0: rawvideo from stdin (RGBA, GL bottom-up — vflip applied below)
    // Input 1..N: audio files
    std::vector<std::string> args;
    args.push_back("ffmpeg"); args.push_back("-hide_banner"); args.push_back("-y");
    if (use_vaapi) {
        args.push_back("-vaapi_device"); args.push_back("/dev/dri/renderD128");
    }
    args.push_back("-f");       args.push_back("rawvideo");
    args.push_back("-pix_fmt"); args.push_back("rgba");
    args.push_back("-s");       args.push_back(std::to_string(out_w) + "x" + std::to_string(out_h));
    args.push_back("-r");       args.push_back(std::to_string(fps));
    args.push_back("-i");       args.push_back("pipe:0");
    for (auto& ai : audio_ins) {
        // -itsoffset must come before -ss/-to/-i; a value of 0 is harmless.
        if (ai.delay > 0.001f) {
            char buf[64]; snprintf(buf, sizeof(buf), "%.6f", (double)ai.delay);
            args.push_back("-itsoffset"); args.push_back(buf);
        }
        if (ai.ss > 0.001f) {
            char buf[64]; snprintf(buf, sizeof(buf), "%.6f", (double)ai.ss);
            args.push_back("-ss"); args.push_back(buf);
        }
        if (ai.to > 0.001f) {
            char buf[64]; snprintf(buf, sizeof(buf), "%.6f", (double)ai.to);
            args.push_back("-to"); args.push_back(buf);
        }
        args.push_back("-i"); args.push_back(ai.path);
    }
    if (is_gif) {
        // Animated GIF: single-pass palettegen+paletteuse via filter_complex.
        // vflip corrects GL's bottom-up pixel order; fps downsamples to gif_fps.
        // split feeds the same stream to palettegen (palette analysis) and
        // paletteuse (dithered remapping).  bayer dithering hides banding well.
        // Do NOT also push -map 0:v — filter_complex auto-maps its unlabeled output.
        char gif_vf[320];
        snprintf(gif_vf, sizeof(gif_vf),
            "[0:v]vflip,fps=%d,"
            "scale=trunc(iw*%d/200)*2:trunc(ih*%d/200)*2,"
            "split[s0][s1];[s0]palettegen=stats_mode=full[p];"
            "[s1][p]paletteuse=dither=bayer:bayer_scale=5[out]",
            state.render_settings.gif_fps,
            state.render_settings.gif_scale,
            state.render_settings.gif_scale);
        args.push_back("-filter_complex"); args.push_back(gif_vf);
        args.push_back("-map");   args.push_back("[out]");
        args.push_back("-loop");  args.push_back("0");   // infinite loop
        args.push_back("-f");     args.push_back("gif");
        args.push_back(state.out_gif);
    } else {
        args.push_back("-map"); args.push_back("0:v");
        if (!audio_ins.empty()) {
            if (audio_ins.size() == 1) {
                args.push_back("-map"); args.push_back("1:a");
            } else {
                // Build filter_complex:
                //   Step 1: volume-adjust each non-unity stream  →  [aN_v]
                //   Step 2: amix all streams into [aout]
                std::string fc;
                std::vector<std::string> mix_ins;
                for (int i = 0; i < (int)audio_ins.size(); ++i) {
                    if (fabsf(audio_ins[i].vol - 1.f) > 0.001f) {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "[%d:a]volume=%.4f[a%dv];",
                                 i + 1, (double)audio_ins[i].vol, i + 1);
                        fc += buf;
                        char lbl[32]; snprintf(lbl, sizeof(lbl), "[a%dv]", i + 1);
                        mix_ins.push_back(lbl);
                    } else {
                        char lbl[32]; snprintf(lbl, sizeof(lbl), "[%d:a]", i + 1);
                        mix_ins.push_back(lbl);
                    }
                }
                for (auto& s : mix_ins) fc += s;
                char mixbuf[64];
                snprintf(mixbuf, sizeof(mixbuf), "amix=inputs=%d:duration=longest[aout]",
                         (int)audio_ins.size());
                fc += mixbuf;
                args.push_back("-filter_complex"); args.push_back(fc);
                args.push_back("-map"); args.push_back("[aout]");
            }
        }
        if (use_vaapi) {
            // RGBA → NV12 (CPU) → hwupload → h264_vaapi on the GPU's VCE engine.
            // vflip corrects GL's bottom-up pixel order.
            args.push_back("-vf");    args.push_back("vflip,format=nv12,hwupload");
            args.push_back("-c:v");   args.push_back("h264_vaapi");
            args.push_back("-global_quality"); args.push_back(std::to_string(state.render_settings.crf));
        } else {
            args.push_back("-c:v");     args.push_back("libx264");
            args.push_back("-pix_fmt"); args.push_back("yuv420p");
            args.push_back("-crf");     args.push_back(std::to_string(state.render_settings.crf));
            args.push_back("-preset");  args.push_back(state.render_settings.preset);
            if (state.render_settings.high_profile) {
                args.push_back("-profile:v"); args.push_back("high");
            }
        }
        if (!audio_ins.empty()) {
            args.push_back("-c:a");  args.push_back("aac");
            args.push_back("-b:a");  args.push_back(std::to_string(state.render_settings.audio_bitrate) + "k");
        }
        args.push_back("-shortest");
        args.push_back(state.out_mp4);
    }

    // Log the full ffmpeg command so failures can be diagnosed.
    rlog("ffmpeg cmd:");
    for (auto& a : args) rlog(" [%s]", a.c_str());
    rlog("\n");

    // ── Fork ffmpeg ───────────────────────────────────────────────────────────
    int stdin_pipe[2];
    if (pipe(stdin_pipe) != 0) {
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &col_tex);
        glDeleteTextures(MAX_VIDEO_TRACKS * 2, vid_texs);
        state.render.stage = "Error: pipe()";
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &col_tex);
        glDeleteTextures(MAX_VIDEO_TRACKS * 2, vid_texs);
        state.render.stage = "Error: fork()";
        return;
    }
    if (pid == 0) {
        dup2(stdin_pipe[0], STDIN_FILENO);
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        // Redirect ffmpeg stderr to a log file for post-mortem diagnosis.
        int errfd = open("/tmp/pms_ffmpeg_err.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (errfd >= 0) { dup2(errfd, STDERR_FILENO); close(errfd); }
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); close(devnull); }
        std::vector<char*> av;
        for (auto& s : args) av.push_back(const_cast<char*>(s.c_str()));
        av.push_back(nullptr);
        execvp("ffmpeg", av.data());
        _exit(127);
    }
    close(stdin_pipe[0]);

    // ── Allocate double-buffered PBOs for async GPU→CPU readback ─────────────
    // With VAAPI, ffmpeg handles the vflip so we don't need the flip memcpy;
    // pixel_buf is still used for the libx264 path.
    size_t frame_bytes = (size_t)out_w * out_h * 4;
    GLuint pbos[2] = {};
    glGenBuffers(2, pbos);
    for (int i = 0; i < 2; ++i) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)frame_bytes, nullptr, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    // ── Store GL export state ─────────────────────────────────────────────────
    g_gl_ex.active        = true;
    g_gl_ex.fbo           = fbo;
    g_gl_ex.color_tex     = col_tex;
    memcpy(g_gl_ex.vid_tex, vid_texs, sizeof(vid_texs));
    g_gl_ex.out_w         = out_w;
    g_gl_ex.out_h         = out_h;
    g_gl_ex.current_frame = 0;
    g_gl_ex.total_frames  = total_frames;
    g_gl_ex.fps_f         = (float)fps;
    g_gl_ex.pipe_write    = stdin_pipe[1];
    g_gl_ex.ffmpeg_pid    = pid;
    g_gl_ex.pixel_buf.resize(frame_bytes);
    g_gl_ex.pbo[0]        = pbos[0];
    g_gl_ex.pbo[1]        = pbos[1];
    g_gl_ex.use_vaapi     = use_vaapi;
    video_close_export_all();  // reset all decoder slots for the new render session
    g_ffmpeg_pid.store(pid);

    state.render.running      = true;
    state.render.progress     = 0.f;
    state.render.frame        = 0;
    state.render.total_frames = total_frames;
    state.render.eta_secs     = 0.f;
    state.render.stage        = "Encoding…";

    if (!state.out_srt.empty())
        render_export_srt(state, state.out_srt);
}

// Collect one PBO frame (map → flip rows → write to pipe). Called at the start
// of tick N+1 to retrieve the pixels kicked during tick N.
// With VAAPI, ffmpeg's vflip handles row inversion so we pipe raw bottom-up RGBA.
static void gl_collect_pbo_frame(int frame_idx) {
    int slot = frame_idx % 2;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, g_gl_ex.pbo[slot]);
    uint8_t* src = (uint8_t*)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    if (src) {
        int out_w     = g_gl_ex.out_w;
        int out_h     = g_gl_ex.out_h;
        int row_bytes = out_w * 4;
        if (g_gl_ex.use_vaapi) {
            // VAAPI: ffmpeg applies vflip — pipe raw GL bottom-up order directly.
            memcpy(g_gl_ex.pixel_buf.data(), src, (size_t)out_w * out_h * 4);
        } else {
            // libx264: flip rows here (GL bottom-up → top-down for MP4).
            for (int y = 0; y < out_h; ++y) {
                int src_y = out_h - 1 - y;
                memcpy(g_gl_ex.pixel_buf.data() + (size_t)y * row_bytes,
                       src + (size_t)src_y * row_bytes, row_bytes);
            }
        }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    const uint8_t* buf = g_gl_ex.pixel_buf.data();
    size_t total = (size_t)g_gl_ex.out_w * g_gl_ex.out_h * 4;
    while (total > 0) {
        ssize_t n = write(g_gl_ex.pipe_write, buf, total);
        if (n <= 0) break;
        buf += n; total -= (size_t)n;
    }
}

void render_tick_gl(AppState& state) {
    if (!g_gl_ex.active) return;

    // Cancel: discard any pending PBO, close pipe immediately.
    if (g_cancel.load()) {
        close(g_gl_ex.pipe_write); g_gl_ex.pipe_write = -1;
        waitpid(g_gl_ex.ffmpeg_pid, nullptr, 0);
        g_ffmpeg_pid.store(0);
        gl_cleanup_export();
        state.render.running = false;
        state.render.stage   = "Cancelled";
        if (g_render_log) { fclose(g_render_log); g_render_log = nullptr; }
        return;
    }

    // Collect the previous tick's PBO frame and write it to the pipe.
    // On tick 0 there is no previous frame yet.
    if (g_gl_ex.current_frame > 0)
        gl_collect_pbo_frame(g_gl_ex.current_frame - 1);

    rlog("  readpixels_done\n");   // previous frame collected (or first frame skipped)

    // All frames rendered + last frame written → signal ffmpeg and wait.
    if (g_gl_ex.current_frame >= g_gl_ex.total_frames) {
        close(g_gl_ex.pipe_write); g_gl_ex.pipe_write = -1;
        int wstat = 0;
        waitpid(g_gl_ex.ffmpeg_pid, &wstat, 0);
        g_ffmpeg_pid.store(0);
        bool ok = WIFEXITED(wstat) && WEXITSTATUS(wstat) == 0;
        gl_cleanup_export();
        state.render.running  = false;
        state.render.progress = ok ? 1.f : state.render.progress;
        state.render.stage    = ok ? "Done" : "Error — ffmpeg failed";
        if (ok) state.render_done = true;
        if (g_render_log) {
            rlog("render_tick_gl: finished ok=%d wstatus=%d\n", ok, wstat);
            fclose(g_render_log); g_render_log = nullptr;
        }
        return;
    }

    float t = (float)g_gl_ex.current_frame / g_gl_ex.fps_f;
    float W = (float)g_gl_ex.out_w;
    float H = (float)g_gl_ex.out_h;

    rlog("frame %d  t=%.3f\n", g_gl_ex.current_frame, (double)t);

    // Save previous GL state (restored at end of tick)
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    GLint prev_vp[4];
    glGetIntegerv(GL_VIEWPORT, prev_vp);

    rlog("  state_saved ok\n");

    // ── Phase 1: Collect video clips into ImDrawList ──────────────────────────
    // Per-clip glass FX (fx_apply) write to g_out[slot].tex before the draw list
    // is submitted, so all texture references stored in the list are stable when
    // ImGui_ImplOpenGL3_RenderDrawData samples them.
    //
    // Slot assignment:
    //   primary  = ti % MAX_VIDEO_TRACKS          (0 .. MAX_VIDEO_TRACKS-1)
    //   secondary = MAX_VIDEO_TRACKS + slot_pri   (MAX_VIDEO_TRACKS .. 2*MAX_VIDEO_TRACKS-1)
    // Global FX use kSceneFxSlot (= MAX_VIDEO_TRACKS*2-2) — that slot is only
    // touched in Phase 3, *after* RenderDrawData has consumed the draw list, so
    // any overlap between slot_sec and kSceneFxSlot is harmless.
    ImDrawList dl(ImGui::GetDrawListSharedData());
    dl._ResetForNewFrame();
    dl.PushClipRect({0.f, 0.f}, {W, H});
    dl.PushTexture(ImGui::GetIO().Fonts->TexRef);

    for (int ti = (int)state.tracks.size() - 1; ti >= 0; --ti) {
        const auto& track = state.tracks[ti];
        if (!track.visible) continue;

        int slot_pri = ti % MAX_VIDEO_TRACKS;
        int slot_sec = MAX_VIDEO_TRACKS + slot_pri;
        GLuint tex_pri = g_gl_ex.vid_tex[slot_pri];
        GLuint tex_sec = g_gl_ex.vid_tex[slot_sec];

        // ── Background (color-pattern) clips ───────────────────────────────────
        // Rendered before video clips so they appear beneath chroma-keyed footage.
        // Mirrors canvas.cpp: continue past non-active BG clips, break after render.
        for (auto& bg_cl : track.clips) {
            if (bg_cl.clip_type != ClipType::Background) continue;
            if (t < bg_cl.start || t >= bg_cl.end) continue;  // not active yet/any more
            if (bg_cl.text.empty()) break;                     // no preset assigned

            // Each track uses its own BG slot (0..MAX_BG_SLOTS-1).
            int bg_slot_idx = ti % MAX_BG_SLOTS;
            uintptr_t bg_tex = bg_render_to_texture(
                bg_cl.text.c_str(), bg_slot_idx, g_gl_ex.out_w, g_gl_ex.out_h,
                t, bg_cl.bg_speed, bg_cl.bg_intensity,
                bg_cl.bg_c1, bg_cl.bg_c2, bg_cl.bg_c3);
            if (!bg_tex) break;

            float bpx = bg_cl.eval_prop("pos_x",    t);
            float bpy = bg_cl.eval_prop("pos_y",    t);
            float bsx = bg_cl.eval_prop("scale_x",  t);
            float bsy = bg_cl.eval_prop("scale_y",  t);
            float brt = bg_cl.eval_prop("rotation", t);
            float ba  = bg_cl.eval_prop("opacity",  t);
            float bcx = bpx * W, bcy = bpy * H;
            float bhw = W * bsx * 0.5f, bhh = H * bsy * 0.5f;
            float brad = brt * 3.14159265f / 180.f;
            float bcos = cosf(brad), bsin = sinf(brad);
            auto brot = [&](float ox, float oy) -> ImVec2 {
                return { bcx + ox*bcos - oy*bsin, bcy + ox*bsin + oy*bcos };
            };
            ImU32 bcol = IM_COL32(255, 255, 255,
                                  (int)(fmaxf(0.f, fminf(1.f, ba)) * 255.f));
            // Y-flipped UVs: bg_render_to_texture produces a GL-orientation texture
            // (row 0 = bottom). Flip so the top of the pattern maps to the top of the FBO.
            dl.AddImageQuad(ImTextureRef((ImTextureID)bg_tex),
                brot(-bhw, -bhh), brot(bhw, -bhh), brot(bhw, bhh), brot(-bhw, bhh),
                {0,1}, {1,1}, {1,0}, {0,0}, bcol);
            break;  // one BG clip per track per frame
        }

        const Clip* active = nullptr;
        int active_ci = -1;
        for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
            auto& cl = track.clips[ci];
            if (cl.clip_type == ClipType::Video && t >= cl.start && t < cl.end)
                { active = &cl; active_ci = ci; break; }
        }
        // Check incoming transition clip (visible before its own start time)
        if (!active) {
            for (int ci = 1; ci < (int)track.clips.size(); ++ci) {
                const Clip& prev = track.clips[ci - 1];
                const Clip& cl   = track.clips[ci];
                if (prev.clip_type != ClipType::Video || cl.clip_type != ClipType::Video) continue;
                if (prev.transition_type == TransitionType::None || prev.transition_post <= 0.f) continue;
                if (t >= cl.start && t < cl.start + prev.transition_post)
                    { active = &track.clips[ci]; active_ci = ci; break; }
            }
        }
        if (!active) continue;

        rlog("  vid_clip track=%d clip=%d path=%s\n", ti, active_ci, active->text.c_str());

        bool in_trans_out = (active->transition_type != TransitionType::None &&
                             active->transition_pre > 0.f &&
                             t >= active->end - active->transition_pre);
        const Clip* next_cl = nullptr;
        if (in_trans_out && active_ci + 1 < (int)track.clips.size()) {
            const Clip& nc = track.clips[active_ci + 1];
            if (nc.clip_type == ClipType::Video) next_cl = &nc;
        }
        bool in_trans_in = false;
        const Clip* prev_cl = nullptr;
        if (!in_trans_out && active_ci > 0) {
            const Clip& pc = track.clips[active_ci - 1];
            if (pc.clip_type == ClipType::Video &&
                pc.transition_type != TransitionType::None &&
                pc.transition_post > 0.f &&
                t < active->start + pc.transition_post) {
                in_trans_in = true;
                prev_cl = &pc;
            }
        }

        if (in_trans_out && next_cl) {
            float pre  = active->transition_pre, post = active->transition_post;
            float cut  = active->end;
            float t_a  = fmaxf(0.f, fminf(1.f, (t - (cut - pre))  / fmaxf(pre,  1e-5f)));
            float t_b  = fmaxf(0.f, fminf(1.f, (t - cut)          / fmaxf(post, 1e-5f)));

            if (active->transition_type == TransitionType::Dissolve) {
                gl_render_vid_clip(dl, active,  t, 1.f-t_a, tex_pri, slot_pri, W, H, state, ti);
                gl_render_vid_clip(dl, next_cl, t, t_b>0.f?t_b:t_a, tex_sec, slot_sec, W, H, state, ti);
            } else if (active->transition_type == TransitionType::FadeBlack) {
                gl_render_vid_clip(dl, active,  t, 1.f-t_a, tex_pri, slot_pri, W, H, state, ti);
                gl_render_vid_clip(dl, next_cl, t, t_b, tex_sec, slot_sec, W, H, state, ti);
            } else { // DipWhite
                gl_render_vid_clip(dl, active, t, 1.f-t_a, tex_pri, slot_pri, W, H, state, ti);
                float white_a = t_a * (1.f - t_b);
                if (white_a > 0.01f)
                    dl.AddRectFilled({0.f, 0.f}, {W, H},
                                     IM_COL32(255, 255, 255, (int)(white_a * 255.f)));
                gl_render_vid_clip(dl, next_cl, t, t_b, tex_sec, slot_sec, W, H, state, ti);
            }
        } else if (in_trans_in && prev_cl) {
            float tf = fmaxf(0.f, fminf(1.f,
                (t - active->start) / fmaxf(prev_cl->transition_post, 1e-5f)));
            if (prev_cl->transition_type == TransitionType::Dissolve) {
                gl_render_vid_clip(dl, prev_cl, fminf(t, prev_cl->end-1e-4f),
                                   1.f-tf, tex_sec, slot_sec, W, H, state, ti);
                gl_render_vid_clip(dl, active, t, tf, tex_pri, slot_pri, W, H, state, ti);
            } else if (prev_cl->transition_type == TransitionType::FadeBlack) {
                gl_render_vid_clip(dl, active, t, tf, tex_pri, slot_pri, W, H, state, ti);
            } else { // DipWhite
                float white_a = 1.f - tf;
                if (white_a > 0.01f)
                    dl.AddRectFilled({0.f, 0.f}, {W, H},
                                     IM_COL32(255, 255, 255, (int)(white_a * 255.f)));
                gl_render_vid_clip(dl, active, t, tf, tex_pri, slot_pri, W, H, state, ti);
            }
        } else {
            gl_render_vid_clip(dl, active, t, 1.f, tex_pri, slot_pri, W, H, state, ti);
        }
    }

    rlog("  vid_clips_done\n");

    // ── Phase 2: Render video clips to export FBO ─────────────────────────────
    // Bind and clear the export FBO, then render the ImDrawList into it.
    glBindFramebuffer(GL_FRAMEBUFFER, g_gl_ex.fbo);
    glViewport(0, 0, g_gl_ex.out_w, g_gl_ex.out_h);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    {
        dl.PopTexture();
        dl.PopClipRect();
        ImDrawData dd;
        dd.DisplayPos       = {0.f, 0.f};
        dd.DisplaySize      = {W, H};
        dd.FramebufferScale = {1.f, 1.f};
        dd.Textures         = &ImGui::GetIO().Fonts->TexList;
        dd.AddDrawList(&dl);
        ImGui_ImplOpenGL3_RenderDrawData(&dd);
    }

    rlog("  vid_render_done\n");

    // ── Phase 3: Global FX ────────────────────────────────────────────────────
    // MUST run after RenderDrawData so per-clip fx slot textures are no longer
    // referenced by a live draw list.  Unbind the export FBO before calling
    // fx_apply so g_gl_ex.color_tex (the FBO's colour attachment) can be safely
    // sampled without an undefined read-while-attached feedback loop.
    {
        EffectAccum     global_ea  = collect_effects    (state, t, (int)state.tracks.size());
        CreativeFXAccum global_cfx = collect_creative_fx(state, t, (int)state.tracks.size());
        if (global_ea.any_color || global_ea.any_blur || global_ea.any_vignette ||
            global_ea.any_text  || global_cfx.any_cfx || global_cfx.any_gen_fx) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);  // detach so color_tex is readable
            uintptr_t out = fx_apply((uintptr_t)g_gl_ex.color_tex, kSceneFxSlot,
                                     g_gl_ex.out_w, g_gl_ex.out_h,
                                     global_ea, global_cfx, t);
            glBindFramebuffer(GL_FRAMEBUFFER, g_gl_ex.fbo);
            glViewport(0, 0, g_gl_ex.out_w, g_gl_ex.out_h);
            if (out != (uintptr_t)g_gl_ex.color_tex)
                fx_blit(out, g_gl_ex.fbo, g_gl_ex.out_w, g_gl_ex.out_h);
        }
    }

    rlog("  fx_done\n");

    // ── Phase 4: Text overlays (ImDrawList on top of the composited frame) ────
    // Export FBO must be bound — it is, either from Phase 2 (no global FX) or
    // re-bound explicitly in Phase 3 / Phase 3b.
    glBindFramebuffer(GL_FRAMEBUFFER, g_gl_ex.fbo);
    glViewport(0, 0, g_gl_ex.out_w, g_gl_ex.out_h);
    {
        ImDrawList text_dl(ImGui::GetDrawListSharedData());
        text_dl._ResetForNewFrame();
        text_dl.PushClipRect({0.f, 0.f}, {W, H});
        text_dl.PushTexture(ImGui::GetIO().Fonts->TexRef);

        draw_text_overlays(&text_dl, state, t, {0.f, 0.f}, W, H);

        rlog("  text_overlays_done  vtx=%d idx=%d cmd=%d\n",
             text_dl.VtxBuffer.Size, text_dl.IdxBuffer.Size, text_dl.CmdBuffer.Size);

        text_dl.PopTexture();
        text_dl.PopClipRect();

        ImDrawData tdd;
        tdd.DisplayPos       = {0.f, 0.f};
        tdd.DisplaySize      = {W, H};
        tdd.FramebufferScale = {1.f, 1.f};
        tdd.Textures         = &ImGui::GetIO().Fonts->TexList;
        tdd.AddDrawList(&text_dl);
        ImGui_ImplOpenGL3_RenderDrawData(&tdd);
        rlog("  imgui_render_done\n");
    }

    // ── Kick async GPU→PBO DMA (non-blocking — returns immediately) ───────────
    // The GPU will fill pbo[current_frame % 2] while the CPU processes the next
    // frame. We collect these pixels at the top of the NEXT render_tick_gl call.
    glBindBuffer(GL_PIXEL_PACK_BUFFER, g_gl_ex.pbo[g_gl_ex.current_frame % 2]);
    glReadPixels(0, 0, g_gl_ex.out_w, g_gl_ex.out_h,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr); // nullptr = async into PBO
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    rlog("  pipe_write_done\n");  // will be written at start of next tick

    // ── Restore GL state ──────────────────────────────────────────────────────
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);

    // ── Update progress ───────────────────────────────────────────────────────
    ++g_gl_ex.current_frame;
    state.render.frame    = g_gl_ex.current_frame;
    state.render.progress = (float)g_gl_ex.current_frame / (float)g_gl_ex.total_frames;
}
