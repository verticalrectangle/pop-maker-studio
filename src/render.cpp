#include "render.h"
#include "inter_font.h"   // inter_black_ttf[], inter_black_ttf_size
#include "bg_remove.h"
#include "globals.h"
#include "overlay_renderer.h"
#include "video.h"
#include "fx_shader.h"
#include "runtime_fx.h"
#include "body_fx.h"
#include "face_filters.h"
#include "face_cache.h"

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
#include "proxy.h"      // proxy_load — the counted frame rate the bg masks are keyed by

#include <fstream>
#include <sstream>
#include <iomanip>
#include <map>
#include <set>
#include <unordered_map>
#include <cerrno>
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

#include "audio.h"             // audio_source_cached / audio_fx_cached (FX bake)
#include "ui/studio_shared.h"  // collect_audio_fx_for_clip

namespace fs = std::filesystem;

static std::atomic<bool>  g_cancel{false};
static std::atomic<pid_t> g_ffmpeg_pid{0};
static std::string        g_font_path;

// ── Audio FX bake ─────────────────────────────────────────────────────────────
// Preview applies AudioFX (autotune, reverb, delay…) by processing decoded
// PCM in memory; ffmpeg knows nothing about that. For export parity, a clip
// with an effective FX chain gets its source pre-rendered to a processed WAV
// that substitutes the ffmpeg input. Full-source bake, so ss/to/itsoffset
// math is untouched. Reuses the preview's caches when warm (the usual case —
// hearing the FX in preview is what computed them); cold paths decode and
// process synchronously at export start.

static bool write_wav_f32(const std::string& path, const float* smp, size_t n) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    uint32_t data_bytes = (uint32_t)(n * 4);
    uint32_t riff_sz    = 36 + data_bytes;
    uint16_t fmt = 3 /*IEEE float*/, ch = 2, bits = 32;
    uint32_t rate = 44100, byte_rate = rate * ch * 4;
    uint16_t block = ch * 4;
    uint32_t fmt_sz = 16;
    bool ok = true;
    auto put = [&](const void* p, size_t sz) { ok = ok && fwrite(p, 1, sz, f) == sz; };
    put("RIFF", 4); put(&riff_sz, 4); put("WAVE", 4);
    put("fmt ", 4); put(&fmt_sz, 4);
    put(&fmt, 2); put(&ch, 2); put(&rate, 4); put(&byte_rate, 4);
    put(&block, 2); put(&bits, 2);
    put("data", 4); put(&data_bytes, 4);
    put(smp, n * 4);
    fclose(f);
    if (!ok) ::unlink(path.c_str());
    return ok;
}

static bool decode_to_pcm(const std::string& src, std::vector<float>& out) {
    std::string tmp = "/tmp/pms_bake_" +
                      std::to_string(std::hash<std::string>{}(src)) + ".raw";
    const char* args[] = {
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-y", "-i", src.c_str(),
        "-vn", "-ar", "44100", "-ac", "2", "-f", "f32le", tmp.c_str(),
        nullptr
    };
    pid_t pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDOUT_FILENO); dup2(dn, STDERR_FILENO); close(dn); }
        execvp("ffmpeg", const_cast<char**>(args));
        _exit(127);
    }
    if (pid < 0) return false;
    int st = 0; waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return false;
    FILE* f = fopen(tmp.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); rewind(f);
    if (sz > 0) {
        out.resize((size_t)sz / sizeof(float));
        size_t got = fread(out.data(), sizeof(float), out.size(), f);
        out.resize(got);
    }
    fclose(f);
    ::unlink(tmp.c_str());
    return !out.empty();
}

static std::string bake_audio_fx_wav(const std::string& src,
                                     const std::vector<AudioFXSegment>& segs) {
    uint64_t fxh = audio_fx_segments_hash(segs);
    uint64_t key = fxh ^ (uint64_t)std::hash<std::string>{}(src);
    char out_path[160];
    snprintf(out_path, sizeof(out_path), "/tmp/pms_fxbake_%016llx.wav",
             (unsigned long long)key);
    if (fs::exists(out_path)) return out_path;

    std::vector<float> pcm;
    if (!audio_fx_cached(src, fxh, pcm)) {           // preview already processed?
        if (!audio_source_cached(src, pcm) &&        // raw PCM cached?
            !decode_to_pcm(src, pcm))                // cold: decode now
            return "";
        pcm = process_audio_fx_segments(pcm, segs, 44100.f);
        if (pcm.empty()) return "";
    }
    if (!write_wav_f32(out_path, pcm.data(), pcm.size())) return "";
    return out_path;
}

// Does a media file have an audio stream? Cached probe (file-scope mirror of the
// local lambda used in the per-stream builder).
static bool render_path_has_audio(const std::string& p) {
    static std::map<std::string, bool> cache;
    auto it = cache.find(p);
    if (it != cache.end()) return it->second;
    bool ok = video_probe_file(p).has_audio;
    cache[p] = ok;
    return ok;
}

// The bus brick a clip on track `ti` over [t0,t1] routes through: the brick on
// the highest track ABOVE it (lower index) whose span overlaps the clip. Mirrors
// the live mixer's owner_brick (nearest above, span-gated). null = ungrouped.
static const Clip* bus_brick_for(const AppState& state, int ti, float t0, float t1) {
    const Clip* best = nullptr; int best_track = -1;
    for (int bt = 0; bt < ti && bt < (int)state.tracks.size(); ++bt) {
        for (const auto& c : state.tracks[(size_t)bt].clips) {
            if (c.clip_type != ClipType::Bus) continue;
            if (c.start < t1 && c.end > t0 && bt > best_track) {   // spans overlap
                best = &c; best_track = bt;
            }
        }
    }
    return best;
}

// Cumulative bus-brick gain for a clip on track `ti` (the owning brick's gain;
// 1.0 when ungrouped). Applied at the per-stream volume stage. The brick's span
// is honored at the FX stage; for gain we apply the brick's gain to the clip
// (exact when the clip sits within the span — the intended grouping case).
static float bus_brick_gain(const AppState& state, int ti, const Clip& cl) {
    const Clip* bb = bus_brick_for(state, ti, cl.start, cl.end);
    return bb ? bb->volume : 1.f;
}

// Effective FX segments for an exported clip — same rules as the preview:
// the clip's own chain covers its whole range, otherwise track bricks apply
// windowed to their overlap. Voice conversion is an ML job handled via
// vc_out_path substitution, never offline-baked.
static std::vector<AudioFXSegment> export_fx_segments(const AppState& state,
                                                      int ti, const Clip& cl) {
    std::vector<AudioFXSegment> segs;
    if (cl.audio_fx.any_active()) {
        AudioFX own = cl.audio_fx;
        own.voice_convert_on = false;
        if (own.any_active()) {
            float spd = fmaxf(0.01f, cl.speed);
            segs.push_back({cl.in_point,
                            cl.in_point + (cl.end - cl.start) * spd, own});
        }
    } else {
        segs = collect_audio_fx_segments(state, ti, cl);
    }
    // Bus brick: the clip routes through the nearest bus brick above it (lower
    // track index) whose span overlaps the clip. Its FX chain bakes over the
    // overlap window (clip↔span, mapped to source time). Linear inserts
    // (EQ/comp/reverb/gain) match the summed-stem result; grain FX differ
    // marginally on overlapping clips. The brick's gain is applied separately at
    // the per-stream volume stage (bus_brick_gain).
    if (const Clip* bb = bus_brick_for(state, ti, cl.start, cl.end)) {
        float spd = fmaxf(0.01f, cl.speed);
        float ov0 = fmaxf(cl.start, bb->start);
        float ov1 = fminf(cl.end,   bb->end);
        float s0  = cl.in_point + (ov0 - cl.start) * spd;
        float s1  = cl.in_point + (ov1 - cl.start) * spd;
        for (const auto& se : bb->fx_chain) {
            AudioFX fx;
            if (audio_fx_from_brick_pub(se, fx)) segs.push_back({s0, s1, fx});
        }
    }
    return segs;
}

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
// t_scale/t_bias map keyframe times (timeline seconds relative to clip start)
// into the time base the consuming filter actually sees: T = t_bias + t_scale*k.
// Video overlay expressions use the default identity mapping; audio filters run
// before atempo on streams whose pts = itsoffset + source time, so they pass
// t_bias = delay + in_point and t_scale = clip speed.
static std::string prop_expr(const Clip& cl, const std::string& prop,
                              float scale_factor, float default_val = -999.f,
                              float eval_at = -1.f,
                              float t_scale = 1.f, float t_bias = 0.f)
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
        float t0  = t_bias + t_scale * keys[i].time;
        float t1  = t_bias + t_scale * keys[i+1].time;
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
    expr = std::string("if(lt(t,") + std::to_string(t_bias + t_scale * keys[0].time)
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
                case AnimStyle::Scale: {
                    // drawtext fontsize isn't a per-frame expression, so the
                    // scale-pop can't be reproduced in this legacy filter-graph
                    // path (the GL overlay export does the real thing). Fall back
                    // to the Fade alpha ramp so the entrance isn't a hard cut.
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
                // Match the preview / GL export: the highlight is the per-clip
                // karaoke_highlight_color (set by typography presets), not sub_color
                // (which is the BASE line). Falls back to white when unset. The GL
                // MP4 export already renders this correctly via the C++ text path;
                // this is the ffmpeg-filter/GIF path keeping the same colour.
                char hi_col[32];
                const float* hc = cl.karaoke_highlight_color;
                if (hc[3] > 0.01f)
                    snprintf(hi_col, sizeof(hi_col), "0x%02x%02x%02x%02x",
                        (int)(hc[0]*255), (int)(hc[1]*255), (int)(hc[2]*255), (int)(hc[3]*255));
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
            // normalize=0: sum the streams (matching the additive preview mixer in
            // audio.cpp). amix's default normalize=1 divides by the input count, so
            // the export comes out ~10 dB quieter than what's heard in the project.
            lo << "amix=inputs=" << aud_ins.size()
               << ":duration=longest:normalize=0[aout]";
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
                std::string vsrc = clip_video_src(state, cl);  // conformed copy if ready
                if (vsrc.empty() || !fs::exists(vsrc)) continue;
                int arr_idx = get_vid_input(vsrc, cl.start, cl.end);
                RLayer rl; rl.kind = RLayer::Vid;
                rl.track_idx = ti; rl.clip_idx = ci;
                rl.in_idx    = arr_idx;  // resolved to real ffmpeg idx below
                layers.push_back(rl);
                // The video's own audio must respect the bus brick gain too. It
                // usually arrives via state.audio_path (added above at vol 1.0);
                // replace that entry's volume with the bus-gained value (mirrors
                // the Audio-clip dedup below), else add it as its own stream.
                if (render_path_has_audio(cl.text)) {
                    float vvol = ((state.tracks[ti].muted || cl.muted) ? 0.f : cl.volume)
                                 * bus_brick_gain(state, ti, cl);
                    bool found = false;
                    for (auto& ai : audio_ins)
                        if (ai.path == cl.text) { ai.vol = vvol; found = true; break; }
                    if (!found) audio_ins.push_back({cl.text, cl.start, cl.end, vvol});
                }
            } else if (cl.clip_type == ClipType::Text   ||
                       cl.clip_type == ClipType::Lyrics ||
                       cl.clip_type == ClipType::Subtitle) {
                RLayer rl; rl.kind = RLayer::Txt;
                rl.track_idx = ti; rl.clip_idx = ci;
                layers.push_back(rl);
            } else if (cl.clip_type == ClipType::Audio) {
                if (cl.text.empty() || !fs::exists(cl.text)) continue;
                float vol = ((state.tracks[ti].muted || cl.muted) ? 0.f : cl.volume)
                            * bus_brick_gain(state, ti, cl);
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
                std::string vsrc = clip_video_src(state, cl);  // conformed copy if ready
                if (vsrc.empty() || !fs::exists(vsrc)) continue;
                int arr_idx = get_vid_input(vsrc);
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
                if (clip_is_videolike_type(cl.clip_type) && !cl.text.empty())
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
        // .mka (Matroska audio) holds the source audio codec via stream-copy for
        // basically any codec; .webm rejected H.264/AAC so the rip silently failed.
        std::string out_audio = (outdir / (vp.stem().string() + "_audio.mka")).string();

        std::string err = video_extract_segment(video_path, 0.0, 1e9, out_audio, /*audio_only=*/true);
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
    // Triple-buffered PBOs: GPU DMAs frame N into pbo[N%3]; CPU reads frame N-2
    // from pbo[(N-2)%3] two ticks later. The extra slot keeps two readbacks
    // in flight at once so the GPU DMA can fully overlap with CPU encode work
    // — shaves the per-frame glMapBuffer wait when GPU runs faster than CPU.
    GLuint  pbo[3]        = {};
    bool    use_vaapi     = false;  // h264_vaapi encoder active
    // Face-filter landmark caches still building — frames wait on these.
    std::vector<std::pair<std::string, int>> face_waits;   // (take path, rot_q)
} g_gl_ex;

static void gl_cleanup_export();
static void clear_ex_still_tex();
static bool gl_render_vid_clip(ImDrawList& dl, const Clip* cl, float at_time,
                                float alpha_mul, GLuint tex_id, int fx_slot,
                                float W, float H, const AppState& state, int ti,
                                bool use_scene = false, float shake = 0.f);

// Render one track's active text overlay to a texture and composite it into the
// current scene at that track's z (shared by preview canvas + GL export).
void scene_add_text_layer(const AppState& state, float t, int ti, int w, int h) {
    static GLuint fbo = 0, tex = 0; static int fw = 0, fh = 0;
    if (!fbo) glGenFramebuffers(1, &fbo);
    if (!tex) glGenTextures(1, &tex);
    if (fw != w || fh != h) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        fw = w; fh = h;
    }
    // Build just this track's text into a scratch draw list.
    ImDrawList tdl(ImGui::GetDrawListSharedData());
    tdl._ResetForNewFrame();
    tdl.PushClipRect({0.f, 0.f}, {(float)w, (float)h});
    tdl.PushTexture(ImGui::GetIO().Fonts->TexRef);
    draw_text_overlays(&tdl, state, t, {0.f, 0.f}, (float)w, (float)h, ti);
    tdl.PopTexture(); tdl.PopClipRect();
    if (tdl.VtxBuffer.Size == 0) return;   // no active text on this track

    GLint prev_fbo = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    GLint vp[4];        glGetIntegerv(GL_VIEWPORT, vp);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImDrawData dd;
    dd.DisplayPos       = {0.f, 0.f};
    dd.DisplaySize      = {(float)w, (float)h};
    dd.FramebufferScale = {1.f, 1.f};
    dd.Textures         = &ImGui::GetIO().Fonts->TexList;
    dd.AddDrawList(&tdl);
    ImGui_ImplOpenGL3_RenderDrawData(&dd);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);

    // Composite full-frame at this track's z. The scene samples bottom-up vs
    // ImGui's top-down render, so flip V (v0=1, v1=0) to keep the text upright.
    scene_add_layer((uintptr_t)tex, w * 0.5f, h * 0.5f, w * 0.5f, h * 0.5f,
                    1.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.f);
}

// ── GL snapshot — identical to preview ───────────────────────────────────────

void render_snapshot_gl(AppState& state, float snap_t, bool open_folder) {
    if (state.snapshot_running) return;

    // Build output path (same logic as render_snapshot_start)
    std::string base_path = state.audio_path;
    if (base_path.empty()) {
        for (auto& tr : state.tracks)
            for (auto& cl : tr.clips)
                if (clip_is_videolike_type(cl.clip_type) && !cl.text.empty())
                    { base_path = cl.text; goto snap_found_base; }
    }
    snap_found_base:
    if (base_path.empty()) {
        // No video/audio media — a text / background / image / FX composition,
        // i.e. someone making a still image. base_path is only used to name the
        // output file, so derive one from the project (else HOME) and render the
        // frame full-res anyway instead of refusing.
        if (!state.project_path.empty()) {
            base_path = state.project_path;
        } else {
            const char* home = std::getenv("HOME");
            base_path = std::string(home ? home : ".") + "/pms_snapshot";
        }
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
    clear_ex_still_tex();   // free the previous render's per-path still textures
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
    // Clear transparent (not black) so a still export keeps its alpha: a
    // text/image/FX composition with empty regions saves as a transparent PNG
    // (stickers, overlays), while a full-frame video stays fully opaque. The
    // mp4 export path (render_tick_gl) is unchanged — it still clears black.
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImDrawList dl(ImGui::GetDrawListSharedData());
    dl._ResetForNewFrame();
    dl.PushClipRect({0.f, 0.f}, {W, H});
    dl.PushTexture(ImGui::GetIO().Fonts->TexRef);

    // Per-track scene FX: standalone FX bricks process the tracks below them.
    // Flush the accumulated draw list into the FBO, then run the brick's FX on
    // it in place, then keep building (tracks above composite over the result).
    auto flush_dl = [&]() {
        dl.PopTexture(); dl.PopClipRect();
        ImDrawData fdd; fdd.DisplayPos = {0,0}; fdd.DisplaySize = {W,H};
        fdd.FramebufferScale = {1,1}; fdd.Textures = &ImGui::GetIO().Fonts->TexList;
        fdd.AddDrawList(&dl);
        ImGui_ImplOpenGL3_RenderDrawData(&fdd);
        dl._ResetForNewFrame();
        dl.PushClipRect({0.f, 0.f}, {W, H});
        dl.PushTexture(ImGui::GetIO().Fonts->TexRef);
    };
    auto apply_track_fx = [&](int ti) {
        EffectAccum     ea  = collect_effects_for_track(state, t, ti);
        CreativeFXAccum cfx = collect_creative_fx_for_track(state, t, ti);
        if (!(ea.any_color || ea.any_blur || ea.any_vignette || ea.any_text ||
              cfx.any_cfx || cfx.any_gen_fx)) return;
        flush_dl();                                   // below-tracks → FBO
        glBindFramebuffer(GL_FRAMEBUFFER, 0);         // detach so col_tex is readable
        uintptr_t out = fx_apply((uintptr_t)col_tex, kSceneFxSlot, out_w, out_h, ea, cfx, t);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, out_w, out_h);
        if (out != (uintptr_t)col_tex) fx_blit(out, fbo, out_w, out_h);
    };

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
            if (clip_is_videolike_type(cl.clip_type) && !cl.text.empty() &&
                t >= cl.start && t < cl.end)
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
        if (!active) {  // text-only / FX-only track — still draw text + run its FX
            draw_text_overlays(&dl, state, t, {0.f, 0.f}, W, H, ti);
            apply_track_fx(ti);
            continue;
        }

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
            } else if (active->transition_type == TransitionType::Shake) {
                // Hard cut at the cut point; the shake ramps up into it and
                // decays out of it, so it whips rather than blends.
                if (t_b <= 0.f)
                    gl_render_vid_clip(dl, active, t, 1.f, vid_texs[slot_pri], slot_pri, W, H, state, ti, false, t_a);
                else
                    gl_render_vid_clip(dl, next_cl, t, 1.f, vid_texs[slot_sec], slot_sec, W, H, state, ti, false, 1.f-t_b);
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
            } else if (prev_cl->transition_type == TransitionType::Shake) {
                gl_render_vid_clip(dl, active, t, 1.f, vid_texs[slot_pri], slot_pri, W, H, state, ti, false, 1.f-tf);
            } else {
                float wa = 1.f-tf;
                if (wa > 0.01f)
                    dl.AddRectFilled({0,0},{W,H},IM_COL32(255,255,255,(int)(wa*255)));
                gl_render_vid_clip(dl, active, t, tf, vid_texs[slot_pri], slot_pri, W, H, state, ti);
            }
        } else {
            gl_render_vid_clip(dl, active, t, 1.f, vid_texs[slot_pri], slot_pri, W, H, state, ti);
        }
        // This track's text, drawn right after its video so it layers at the
        // track's z-order (foreground tracks composite on top of it next).
        draw_text_overlays(&dl, state, t, {0.f, 0.f}, W, H, ti);
        apply_track_fx(ti);   // standalone FX bricks process the tracks below
    }
    video_close_export_all();

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

    // (Scene FX are applied per-track during the loop above — each standalone FX
    // brick processes the tracks below it — so there's no whole-frame FX pass.)

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
        state.snapshot_msg       = "Saved " + fs::path(out).filename().string();
        state.snapshot_done_path = out;
        state.snapshot_done_err.clear();
        if (open_folder)
            system(("xdg-open \"" + dir + "\" &").c_str());
    } else {
        state.snapshot_msg      = "Snapshot failed — PNG write error";
        state.snapshot_done_err = "PNG write failed";
    }
    state.snapshot_msg_new = true;
    state.snapshot_done    = true;
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

// Per-stage timing accumulator. Resets every kPerfWindow frames so the printed
// numbers reflect recent throughput, not the long-running average.
static constexpr int kPerfWindow = 60;
static struct PerfAccum {
    double collect_us = 0, compose_us = 0, render_us = 0;
    double fx_us = 0, text_us = 0, kick_us = 0, total_us = 0;
    int    count     = 0;
    void reset() { *this = {}; }
} g_perf;

using perf_clock = std::chrono::steady_clock;
static inline double us_since(perf_clock::time_point t0) {
    using namespace std::chrono;
    return duration<double, std::micro>(perf_clock::now() - t0).count();
}

static void gl_cleanup_export() {
    if (g_gl_ex.fbo)       { glDeleteFramebuffers(1, &g_gl_ex.fbo);       g_gl_ex.fbo       = 0; }
    if (g_gl_ex.color_tex) { glDeleteTextures(1, &g_gl_ex.color_tex);     g_gl_ex.color_tex = 0; }
    glDeleteTextures(MAX_VIDEO_TRACKS * 2, g_gl_ex.vid_tex);
    memset(g_gl_ex.vid_tex, 0, sizeof(g_gl_ex.vid_tex));
    if (g_gl_ex.pbo[0]) {
        glDeleteBuffers(3, g_gl_ex.pbo);
        g_gl_ex.pbo[0] = g_gl_ex.pbo[1] = g_gl_ex.pbo[2] = 0;
    }
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

// One GL texture per DISTINCT still image, keyed by source path, for the whole
// render. The per-track tex_id slots (vid_texs[ti % MAX_VIDEO_TRACKS]) alias when
// a project has more image tracks than slots, and the draws are deferred — so a
// still that loaded into a slot a later clip overwrote came out as that other
// image (or black). A per-path texture is stable; repeated uses share it. Freed
// in each render's teardown via clear_ex_still_tex().
struct ExStillTex { GLuint tex = 0; int w = 0, h = 0; };
static std::unordered_map<std::string, ExStillTex> g_ex_still_tex;
static void clear_ex_still_tex() {
    for (auto& kv : g_ex_still_tex)
        if (kv.second.tex) glDeleteTextures(1, &kv.second.tex);
    g_ex_still_tex.clear();
}

// Map an export source-time to the proxy frame index the bg-removal masks are keyed
// by. The masks are indexed by the proxy's COUNTED rate (frames÷duration, e.g. 29.97),
// the same rate the preview uses (video.cpp:1002 / playhead_to_frame_idx) and that
// start_frame is computed in. fps.txt instead records the raw proxy MJPEG's CONTAINER
// rate (a forced 30, see proxy.cpp:238) — using it drifts the mask out of sync as time
// grows. Cache the rational rate per source path (proxy_load spawns ffprobe).
static int export_proxy_frame_idx(const std::string& video_path, double t) {
    static std::map<std::string, std::pair<int64_t,int64_t>> s_cache;  // path → (num, den)
    auto it = s_cache.find(video_path);
    if (it == s_cache.end()) {
        int64_t num = 0, den = 1;
        ProxyInfo pi;
        if (proxy_load(video_path, pi) && pi.fps_num > 0 && pi.fps_den > 0) {
            num = pi.fps_num; den = pi.fps_den;
        }
        it = s_cache.insert({video_path, {num, den}}).first;
    }
    int64_t num = it->second.first, den = it->second.second;
    return (num > 0 && den > 0) ? (int)((int64_t)(t * (double)num) / den)
                                : (int)(t * 30.0);
}

static bool gl_render_vid_clip(ImDrawList& dl, const Clip* cl, float at_time,
                                float alpha_mul, GLuint tex_id, int fx_slot,
                                float W, float H, const AppState& state, int ti,
                                bool use_scene, float shake)
{
    if (!cl || cl->text.empty()) return false;
    float src_t = clip_src_time(*cl, at_time);

    // One-shot per-clip diagnostic: prints once on the first frame for each
    // distinct (clip.start, clip.end, speed) tuple seen during the export so
    // we can confirm the render path is actually reading the speed you set.
    {
        static std::set<uint64_t> seen;
        uint64_t key = 0;
        key ^= (uint64_t)(int)(cl->start * 1000.f) * 0x9E3779B97F4A7C15ULL;
        key ^= (uint64_t)(int)(cl->end   * 1000.f) * 0xBF58476D1CE4E5B9ULL;
        key ^= (uint64_t)(int)(cl->speed * 1000.f) * 0x94D049BB133111EBULL;
        if (!seen.count(key)) {
            seen.insert(key);
            fprintf(stderr,
                "[render diag] vid_clip ti=%d start=%.3f end=%.3f in_point=%.3f "
                "speed=%.4f at_t=%.3f src_t=%.3f path=%s\n",
                ti, (double)cl->start, (double)cl->end, (double)cl->in_point,
                (double)cl->speed, (double)at_time, (double)src_t,
                cl->text.c_str());
        }
    }

    // Still images: a PNG/JPEG is already a renderable image — load the ORIGINAL
    // at full quality (PNG keeps alpha) via stb_image, no lossy JPEG still. Only
    // formats stb can't read (HEIC/WEBP/TIFF — FFmpeg can't reliably decode them
    // either) fall back to the converted still proxy. We do NOT gate on the proxy
    // existing anymore: that silently dropped stills from the export whenever the
    // background still generator hadn't finished (or wasn't needed at all).
    if (is_still_ext(cl->text)) {
        // One texture per distinct image (keyed by path), decoded once for the
        // whole render — NOT the shared per-track tex_id, which aliases across
        // clips and corrupted repeated images on export. Repeated uses of the
        // same image share this texture; it's freed in clear_ex_still_tex().
        ExStillTex& st = g_ex_still_tex[cl->text];
        if (st.tex == 0) {
            int sw = 0, sh = 0, sc = 0;
            uint8_t* px = stbi_load(cl->text.c_str(), &sw, &sh, &sc, 4);
            if (!px) {   // exotic format → converted still proxy
                std::string still = proxy_still_path(cl->text);
                px = stbi_load(still.c_str(), &sw, &sh, &sc, 4);
            }
            if (!px) return false;
            glGenTextures(1, &st.tex);
            glBindTexture(GL_TEXTURE_2D, st.tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            stbi_image_free(px);
            st.w = sw; st.h = sh;
        }
        int vid_w = st.w, vid_h = st.h;

        uintptr_t cur_tex = (uintptr_t)st.tex;
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
            // Fit box follows the cropped region's aspect (matches preview).
            float va = cl->cropped_aspect(vid_w, vid_h), ca = W / H;
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
        // Non-destructive crop: sample only the clip's UV window.
        ImVec2 uv0{cl->crop_l,       cl->crop_t};
        ImVec2 uv1{1.f - cl->crop_r, 1.f - cl->crop_b};
        // Camera-record takes export MIRRORED — matches the live preview and
        // playback (front-facing-cam convention). Swap the horizontal window.
        if (cl->clip_type == ClipType::VideoRecord) {
            float t = uv0.x; uv0.x = uv1.x; uv1.x = t;
        }
        if (cl->flip_h) { float t = uv0.x; uv0.x = uv1.x; uv1.x = t; }
        if (cl->flip_v) { float t = uv0.y; uv0.y = uv1.y; uv1.y = t; }
        if (use_scene) {
            scene_add_layer(cur_tex, cx, cy, hw, hh, cos_r, sin_r,
                            fmaxf(0.f, fminf(1.f, alpha)),
                            uv0.x, uv0.y, uv1.x, uv1.y);
        } else {
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

    // AI bg-removal is applied solely by the RemoveBackground BodyFX brick (below) —
    // no CPU alpha-bake here. The frame uploads OPAQUE and the brick shader cuts it
    // once (α = orig.a · mask); baking here too would square the mask.

    glBindTexture(GL_TEXTURE_2D, tex_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vf->width, vf->height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, vf->data);
    int vid_w = vf->width, vid_h = vf->height;
    double vid_pts = vf->pts;   // the decoded frame's ACTUAL timestamp; the bg mask is
                                // indexed off this (not the requested src_t) so the cutout
                                // tracks the exact frame rendered — no temporal lag.
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
        // Index by the proxy's COUNTED rate (matches the preview + start_frame), NOT
        // fps.txt's container rate. Use the DECODED frame's pts, not the requested
        // src_t: the decoder snaps to the nearest source frame, and keying the mask off
        // the request rather than the delivered frame is what made the export mask lag.
        int frame_i = export_proxy_frame_idx(cl->text, vid_pts);
        // Bounding box (keep-region in v_uv — y is bottom-up, so flip t/b) + softness,
        // fed into the RemoveBackground brick shader (was the CPU alpha-bake's job).
        float bg_box[4] = {0.f, 1.f, 0.f, 1.f};
        if (cl->bg_remove_box_on) {
            bg_box[0] = cl->bg_remove_box_l;        bg_box[1] = cl->bg_remove_box_r;
            bg_box[2] = 1.f - cl->bg_remove_box_b;  bg_box[3] = 1.f - cl->bg_remove_box_t;
        }
        float bg_soft = cl->bg_remove_softness;

        // At most ONE RemoveBackground may apply per frame across both loops below:
        // stacking the cutout multiplies alpha by the mask twice (mask^2), eroding the
        // soft edges into a smudge. Other body-FX still stack freely (they preserve alpha).
        bool bg_removed = false;

        // Standalone glass BodyFX bricks on this track
        for (auto& bfx_cl : state.tracks[ti].clips) {
            if (bfx_cl.clip_type != ClipType::BodyFX) continue;
            if (at_time < bfx_cl.start || at_time >= bfx_cl.end) continue;
            bool is_rmbg = (bfx_cl.body_fx_type == BodyFXType::RemoveBackground);
            if (is_rmbg && bg_removed) continue;
            unsigned mask_tex = body_fx_mask_texture(mask_dir, frame_i);
            if (!mask_tex) continue;
            cur_tex = body_fx_apply(bfx_cl.body_fx_type, cur_tex, mask_tex,
                                    vid_w, vid_h, bfx_cl.body_fx_params,
                                    bfx_cl.body_fx_amount, at_time, bg_box, bg_soft);
            if (is_rmbg) bg_removed = true;
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
                bool is_rmbg = (se.body_fx_type == BodyFXType::RemoveBackground);
                if (is_rmbg && bg_removed) continue;
                unsigned mask_tex = body_fx_mask_texture(mask_dir, frame_i);
                if (!mask_tex) continue;
                cur_tex = body_fx_apply(se.body_fx_type, cur_tex, mask_tex,
                                        vid_w, vid_h, se.body_fx_params,
                                        se.body_fx_amount, at_time, bg_box, bg_soft);
                if (is_rmbg) bg_removed = true;
            }
        }
    }

    // RuntimeFX — custom hot-reload shader on this clip.
    if (!cl->runtime_fx_id.empty())
        cur_tex = runtime_fx_apply(cl->runtime_fx_id, cur_tex, vid_w, vid_h,
                                   cl->runtime_fx_params,
                                   cl->eval_prop("runtime_fx_amount", at_time), at_time);

    // Face filter on the take — same cached-landmark helper as preview.
    // Export prep blocks until the cache is built (see export start).
    if (cl->face_filter != 0)
        cur_tex = face_filter_apply_take(*cl, (double)src_t, cur_tex,
                                         fx_slot, vid_w, vid_h);

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
        // Fit box follows the cropped region's aspect (matches preview).
        float vid_asp = cl->cropped_aspect(vid_w, vid_h);
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

    // Transition shake: jitter the clip's screen centre. shake² gives a punchy
    // peak; multi-frequency per axis reads as a handheld whip, not a slide.
    if (shake > 0.f) {
        float seed = floorf(at_time * 60.f);
        float amp  = shake * shake * fminf(W, H) * 0.05f;
        cx += (sinf(seed * 127.1f) + 0.5f * sinf(seed * 57.7f)) * amp;
        cy += (cosf(seed * 311.7f) + 0.5f * cosf(seed * 91.3f)) * amp;
    }

    float rad = rot * 3.14159265f / 180.f;
    float cos_r = cosf(rad), sin_r = sinf(rad);
    auto rot_pt = [&](float ox, float oy) -> ImVec2 {
        return { cx + ox*cos_r - oy*sin_r, cy + ox*sin_r + oy*cos_r };
    };
    // Non-destructive crop: sample only the clip's UV window.
    float cu0 = cl->crop_l,       cv0 = cl->crop_t;
    float cu1 = 1.f - cl->crop_r, cv1 = 1.f - cl->crop_b;
    // Camera-record takes export MIRRORED — matches the live preview and
    // playback (front-facing-cam convention). Swap the horizontal window.
    if (cl->clip_type == ClipType::VideoRecord) { float t = cu0; cu0 = cu1; cu1 = t; }
    if (cl->flip_h) { float t = cu0; cu0 = cu1; cu1 = t; }
    if (cl->flip_v) { float t = cv0; cv0 = cv1; cv1 = t; }
    if (use_scene) {
        scene_add_layer(draw_tex, cx, cy, hw, hh, cos_r, sin_r,
                        fmaxf(0.f, fminf(1.f, alpha)),
                        cu0, cv0, cu1, cv1);
    } else {
        ImU32 col = IM_COL32(255, 255, 255, (int)(fmaxf(0.f, fminf(1.f, alpha)) * 255.f));
        dl.AddImageQuad(ImTextureRef((ImTextureID)draw_tex),
            rot_pt(-hw,-hh), rot_pt(hw,-hh), rot_pt(hw,hh), rot_pt(-hw,hh),
            {cu0,cv0}, {cu1,cv0}, {cu1,cv1}, {cu0,cv1}, col);
    }
    return true;
}

void render_start_gl(AppState& state) {
    g_cancel.store(false);

    // Open crash log — each line is flushed so the last line before a crash is visible.
    if (g_render_log) fclose(g_render_log);
    g_render_log = fopen("/tmp/pms_render_log.txt", "w");
    g_perf.reset();
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

    // Face filters: kick landmark-cache builds NOW so they run while the rest
    // of the export sets up; render_tick_gl waits on the stragglers before
    // the first frame (filters silently missing from an export would read as
    // "export is broken").
    std::vector<std::pair<std::string, int>> face_waits;
    for (auto& tr : state.tracks)
        for (auto& cl : tr.clips)
            if (cl.face_filter != 0 && !cl.text.empty()) {
                int rq = ((int)lroundf(cl.rotation / 90.f) % 4 + 4) % 4;
                face_cache_request(cl.text, rq);
                face_waits.push_back({cl.text, rq});
            }

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
    clear_ex_still_tex();   // free the previous render's per-path still textures
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
        float speed = 1.f;   // playback speed; non-unity → atempo in filter_complex
        // Keyframed gain/pan + fades, applied pre-atempo. The stream's pts as
        // the filter sees them = itsoffset + source time, so all times below
        // are already mapped into that base (delay + in_point + rel*speed).
        std::string vol_e;          // volume expression over t — overrides vol
        float pan = 0.f;            // static pan (-1=L .. +1=R)
        std::string pan_e;          // pan expression over t — overrides pan
        float fade_in = 0.f,  fade_in_st = 0.f;   // afade in duration / start pts
        float fade_out = 0.f, fade_out_st = 0.f;  // afade out duration / start pts
    };
    std::vector<AudioIn> audio_ins;
    if (!is_gif) {
        // Per-clip audio: each Audio brick AND each Video clip with audio contributes
        // its own ffmpeg input with proper -ss / -to / -itsoffset and a per-stream
        // atempo so cuts, in_point, and speed all survive the export. Video clips
        // need this too because the source video's audio track is the user's audio.
        std::set<std::string> covered_paths;  // sources already mixed in via clips
        // Cache probe results: skip Video clips whose source has no audio track
        // (ffmpeg would error trying to map a non-existent audio stream).
        std::map<std::string, bool> has_audio_cache;
        auto path_has_audio = [&](const std::string& p) {
            auto it = has_audio_cache.find(p);
            if (it != has_audio_cache.end()) return it->second;
            MediaFileInfo info = video_probe_file(p);
            bool ok = info.has_audio;
            has_audio_cache[p] = ok;
            return ok;
        };
        for (int ti = (int)state.tracks.size() - 1; ti >= 0; --ti) {
            for (auto& cl : state.tracks[ti].clips) {
                // Record brick: its selected take is its audio. Same math as
                // an Audio clip at speed 1 — in_point is honored, so a left
                // trim seeks into the take (the FX bake windows, built by
                // export_fx_segments, are mapped through in_point too).
                if (cl.clip_type == ClipType::Record) {
                    if (cl.muted || cl.rec_take_sel < 0 ||
                        cl.rec_take_sel >= (int)cl.rec_takes.size()) continue;
                    std::string tp = cl.rec_takes[cl.rec_take_sel];
                    if (!fs::exists(tp)) continue;
                    // Converted voice substitutes the take (preview parity)
                    if (cl.vc_status == VcStatus::Ready && !cl.vc_out_path.empty() &&
                        fs::exists(cl.vc_out_path))
                        tp = cl.vc_out_path;
                    // Autotune-over-takes etc: bake the take's effective FX.
                    auto segs = export_fx_segments(state, ti, cl);
                    if (!segs.empty()) {
                        std::string baked = bake_audio_fx_wav(tp, segs);
                        if (!baked.empty()) tp = baked;
                    }
                    // Fold a past-t=0 overhang into in_point (take speed 1),
                    // so ss/to seek into the take exactly like the Audio path.
                    float cstart  = cl.start;
                    float inpoint = cl.in_point;
                    if (cstart < 0.f) { inpoint += -cstart; cstart = 0.f; }
                    float dur = cl.end - cstart;
                    AudioIn ai;
                    ai.path  = tp;
                    ai.vol   = (state.tracks[ti].muted ? 0.f : cl.volume)
                               * bus_brick_gain(state, ti, cl);
                    ai.ss    = inpoint;
                    ai.to    = inpoint + dur;
                    ai.delay = fmaxf(0.f, cstart);
                    ai.pan   = state.tracks[ti].muted ? 0.f : cl.pan;
                    // Keyframed volume/pan (take plays at 1x, pts base 0) — mirror
                    // the general clip path so a record take exports as it previews.
                    if (!state.tracks[ti].muted) {
                        if (auto kv = cl.ktracks.find("volume");
                            kv != cl.ktracks.end() && !kv->second.empty())
                            ai.vol_e = prop_expr(cl, "volume", 1.f, cl.volume, -1.f, 1.f, 0.f);
                        if (auto kp = cl.ktracks.find("pan");
                            kp != cl.ktracks.end() && !kp->second.empty())
                            ai.pan_e = prop_expr(cl, "pan", 1.f, cl.pan, -1.f, 1.f, 0.f);
                    }
                    if (cl.fade_in > 0.f)  { ai.fade_in = cl.fade_in;  ai.fade_in_st = 0.f; }
                    if (cl.fade_out > 0.f) {
                        ai.fade_out    = cl.fade_out;
                        ai.fade_out_st = fmaxf(0.f, dur - cl.fade_out);
                    }
                    audio_ins.push_back(std::move(ai));
                    covered_paths.insert(tp);
                    continue;
                }
                if (cl.text.empty()) continue;
                // Audio clips and any video-like clip whose source carries audio
                // (imported video, or a camera A/V take recorded with the mic).
                if (cl.clip_type != ClipType::Audio &&
                    !clip_is_videolike_type(cl.clip_type)) continue;
                if (!fs::exists(cl.text)) continue;
                if (clip_is_videolike_type(cl.clip_type) && !path_has_audio(cl.text)) continue;
                float speed = fmaxf(0.01f, cl.speed);
                float vol   = ((state.tracks[ti].muted || cl.muted) ? 0.f : cl.volume)
                              * bus_brick_gain(state, ti, cl);
                // Clips dragged left past t=0 (start < 0): only the part from
                // timeline 0 is audible. Fold the overhang into in_point so
                // the ss/to/itsoffset math below needs no negative offsets —
                // preview parity (the live mixer handles negative start).
                float cstart  = cl.start;
                float inpoint = cl.in_point;
                if (cstart < 0.f) {
                    inpoint += -cstart * speed;
                    cstart   = 0.f;
                }
                float ss    = inpoint;
                float dur   = (cl.end - cstart) * speed;
                float to    = ss + dur;
                // Each audio stream is reset to pts 0 by asetpts=PTS-STARTPTS in the
                // filter graph (see below), so timeline placement is the FULL clip
                // start — adelay = cl.start, same as the Record-take path. (It used
                // to subtract in_point, which silently collapsed to 0 for clips whose
                // in_point equals their start — e.g. sequential slices of one source —
                // so every such clip played at t=0 at once instead of in sequence.)
                float delay = fmaxf(0.f, cstart);
                // Preview parity: converted voice substitutes the source, and
                // any effective AudioFX chain is baked into a processed WAV.
                // Full-source bake — ss/to/itsoffset math below is unchanged.
                std::string apath = cl.text;
                if (cl.clip_type == ClipType::Audio &&
                    cl.vc_status == VcStatus::Ready && !cl.vc_out_path.empty() &&
                    fs::exists(cl.vc_out_path))
                    apath = cl.vc_out_path;
                {
                    auto segs = export_fx_segments(state, ti, cl);
                    if (!segs.empty()) {
                        std::string baked = bake_audio_fx_wav(apath, segs);
                        if (!baked.empty()) apath = baked;
                    }
                }
                AudioIn ai;
                ai.path = apath; ai.vol = vol;
                ai.ss = ss; ai.to = to; ai.delay = delay; ai.speed = speed;
                // Pre-atempo filter time base. Streams are normalised with
                // asetpts=PTS-STARTPTS in the filter graph (amix ignores input
                // start timestamps — measured, not folklore), so the clip's
                // first sample is always pts 0; placement happens via adelay.
                float pts0 = 0.f;
                if (!state.tracks[ti].muted && !cl.muted) {
                    if (auto kv = cl.ktracks.find("volume");
                        kv != cl.ktracks.end() && !kv->second.empty())
                        ai.vol_e = prop_expr(cl, "volume", 1.f, cl.volume, -1.f,
                                             speed, pts0);
                    ai.pan = cl.pan;
                    if (auto kp = cl.ktracks.find("pan");
                        kp != cl.ktracks.end() && !kp->second.empty())
                        ai.pan_e = prop_expr(cl, "pan", 1.f, cl.pan, -1.f,
                                             speed, pts0);
                }
                // Audio fades, mirroring the preview mixer's clip_fade().
                if (cl.fade_in > 0.f) {
                    ai.fade_in    = cl.fade_in * speed;
                    ai.fade_in_st = pts0;
                }
                if (cl.fade_out > 0.f) {
                    ai.fade_out    = cl.fade_out * speed;
                    ai.fade_out_st = pts0 + fmaxf(0.f, dur - cl.fade_out * speed);
                }
                audio_ins.push_back(std::move(ai));
                covered_paths.insert(cl.text);
            }
        }
        // state.audio_path fallback: only when no clip already contributes its audio.
        // For lyric-video workflows there's just an audio file and Text clips, so this
        // path still produces audio. For video-editing workflows the per-clip entries
        // above own the audio and the fallback would just produce an un-edited
        // duplicate that ignores cuts/speed.
        //
        // ...BUT never when the source clip is muted. If the user muted the original
        // (e.g. replaced it with converted-voice segments on other tracks), re-adding
        // it here plays the raw take under everything — the mute has to win.
        bool src_muted = false;
        if (!state.audio_path.empty())
            for (int ti = 0; ti < (int)state.tracks.size() && !src_muted; ++ti)
                for (const auto& cl : state.tracks[ti].clips)
                    if (cl.text == state.audio_path &&
                        (state.tracks[ti].muted || cl.muted)) { src_muted = true; break; }
        if (!state.audio_path.empty() && !covered_paths.count(state.audio_path) && !src_muted) {
            AudioIn ai; ai.path = state.audio_path; ai.to = -1.f;
            audio_ins.push_back(std::move(ai));
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
        // No -itsoffset: amix ignores input start timestamps entirely
        // (verified empirically), so timeline placement is done with adelay
        // inside the filter graph instead.
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
            // Per-stream needs: gain (static or keyframed), pan, fades, atempo.
            auto stream_needs_work = [](const AudioIn& ai) {
                return fabsf(ai.speed - 1.f) > 0.001f ||
                       fabsf(ai.vol   - 1.f) > 0.001f ||
                       !ai.vol_e.empty() || !ai.pan_e.empty() ||
                       fabsf(ai.pan) > 0.001f ||
                       ai.fade_in > 0.f || ai.fade_out > 0.f;
            };
            // Single stream with no edits → direct map (zero filter overhead).
            bool simple_passthrough = (audio_ins.size() == 1) &&
                                      !stream_needs_work(audio_ins[0]) &&
                                      audio_ins[0].delay <= 0.001f;
            if (simple_passthrough) {
                // Trailing '?' → optional stream: if the source turns out to
                // have no audio (probe false positives on some AVIs), ffmpeg
                // skips it instead of aborting the whole export.
                args.push_back("-map"); args.push_back("1:a?");
            } else {
                // Per-stream chain: volume → afade → pan → atempo, then amix.
                // Volume/fade/pan run BEFORE atempo so their `t` is the
                // pre-tempo pts the keyframe expressions were mapped into.
                // atempo accepts 0.5..100 in modern ffmpeg so a single
                // instance covers the speed slider — clamp at its lower bound.
                std::string fc;
                std::vector<std::string> mix_ins;
                for (int i = 0; i < (int)audio_ins.size(); ++i) {
                    const auto& ai = audio_ins[i];
                    if (!stream_needs_work(ai) && ai.delay <= 0.001f) {
                        char lbl[32]; snprintf(lbl, sizeof(lbl), "[%d:a]", i + 1);
                        mix_ins.push_back(lbl);
                        continue;
                    }
                    char head[16]; snprintf(head, sizeof(head), "[%d:a]", i + 1);
                    fc += head;
                    std::vector<std::string> chain;
                    // Normalise to pts 0: input -ss leaves absolute timestamps
                    // and itsoffset is ignored by amix, so every downstream
                    // time (fades, keyframe exprs) is clip-relative.
                    chain.push_back("asetpts=PTS-STARTPTS");
                    if (!ai.vol_e.empty())
                        chain.push_back("volume='" + ai.vol_e + "':eval=frame");
                    else if (fabsf(ai.vol - 1.f) > 0.001f) {
                        char buf[64]; snprintf(buf, sizeof(buf), "volume=%.4f", (double)ai.vol);
                        chain.push_back(buf);
                    }
                    if (ai.fade_in > 0.f) {
                        char buf[96]; snprintf(buf, sizeof(buf),
                            "afade=t=in:st=%.4f:d=%.4f",
                            (double)ai.fade_in_st, (double)ai.fade_in);
                        chain.push_back(buf);
                    }
                    if (ai.fade_out > 0.f) {
                        char buf[96]; snprintf(buf, sizeof(buf),
                            "afade=t=out:st=%.4f:d=%.4f",
                            (double)ai.fade_out_st, (double)ai.fade_out);
                        chain.push_back(buf);
                    }
                    bool has_pan = !ai.pan_e.empty() || fabsf(ai.pan) > 0.001f;
                    bool has_speed = fabsf(ai.speed - 1.f) > 0.001f;
                    if (has_pan) {
                        // Constant-gain pan law matching the preview mixer:
                        // L gain = min(1, 1-pan), R gain = min(1, 1+pan).
                        // Per-channel volume via channelsplit/join because the
                        // `pan` filter can't take time expressions. aformat
                        // first so mono mic sources don't break channelsplit.
                        std::string gl, gr;
                        if (!ai.pan_e.empty()) {
                            gl = "'min(1,1-(" + ai.pan_e + "))':eval=frame";
                            gr = "'min(1,1+(" + ai.pan_e + "))':eval=frame";
                        } else {
                            char bl[32], br[32];
                            snprintf(bl, sizeof(bl), "%.4f", (double)fminf(1.f, 1.f - ai.pan));
                            snprintf(br, sizeof(br), "%.4f", (double)fminf(1.f, 1.f + ai.pan));
                            gl = bl; gr = br;
                        }
                        for (auto& f : chain) { fc += f; fc += ","; }
                        char seg[256];
                        snprintf(seg, sizeof(seg),
                            "aformat=channel_layouts=stereo,"
                            "channelsplit=channel_layout=stereo[l%d][r%d];", i, i);
                        fc += seg;
                        snprintf(seg, sizeof(seg), "[l%d]volume=%s[lo%d];", i, gl.c_str(), i);
                        fc += seg;
                        snprintf(seg, sizeof(seg), "[r%d]volume=%s[ro%d];", i, gr.c_str(), i);
                        fc += seg;
                        snprintf(seg, sizeof(seg),
                            "[lo%d][ro%d]join=inputs=2:channel_layout=stereo", i, i);
                        fc += seg;
                        if (has_speed) {
                            char buf[64]; snprintf(buf, sizeof(buf), ",atempo=%.5f",
                                                   (double)fmaxf(0.5f, fminf(100.f, ai.speed)));
                            fc += buf;
                        }
                        if (ai.delay > 0.001f) {
                            char buf[64]; snprintf(buf, sizeof(buf),
                                ",adelay=%d:all=1", (int)lroundf(ai.delay * 1000.f));
                            fc += buf;
                        }
                    } else {
                        if (has_speed) {
                            char buf[64]; snprintf(buf, sizeof(buf), "atempo=%.5f",
                                                   (double)fmaxf(0.5f, fminf(100.f, ai.speed)));
                            chain.push_back(buf);
                        }
                        if (ai.delay > 0.001f) {
                            char buf[64]; snprintf(buf, sizeof(buf),
                                "adelay=%d:all=1", (int)lroundf(ai.delay * 1000.f));
                            chain.push_back(buf);
                        }
                        for (size_t k = 0; k < chain.size(); ++k) {
                            if (k) fc += ",";
                            fc += chain[k];
                        }
                    }
                    // Regenerate timestamps from the sample count so the chain
                    // emits strictly monotonic pts with no NOPTS flush packet.
                    // adelay (and input-seeked matroska/AAC sources with encoder
                    // priming) can leave a NOPTS final packet that a lone aac
                    // encoder tolerates but amix propagates — the muxer then sees
                    // a non-monotonic dts (NOPTS) and aborts the whole export.
                    // N/SR/TB rewrites pts = sample_index/sample_rate, killing it.
                    fc += ",asetpts=N/SR/TB";
                    char tail[32]; snprintf(tail, sizeof(tail), "[a%df];", i + 1);
                    fc += tail;
                    char lbl[32]; snprintf(lbl, sizeof(lbl), "[a%df]", i + 1);
                    mix_ins.push_back(lbl);
                }
                if (mix_ins.size() == 1) {
                    // One stream, no amix needed — map the processed label directly.
                    // The trailing ';' from the filter chain above is harmless before EOF.
                    args.push_back("-filter_complex"); args.push_back(fc);
                    args.push_back("-map"); args.push_back(mix_ins[0]);
                } else {
                    for (auto& s : mix_ins) fc += s;
                    char mixbuf[80];
                    // normalize=0: sum the streams to match the additive preview
                    // mixer (audio.cpp). amix defaults to normalize=1, which divides
                    // by the input count and makes the export ~10 dB quieter than
                    // what's heard in the project.
                    snprintf(mixbuf, sizeof(mixbuf),
                             "amix=inputs=%d:duration=longest:normalize=0[aout]",
                             (int)mix_ins.size());
                    fc += mixbuf;
                    args.push_back("-filter_complex"); args.push_back(fc);
                    args.push_back("-map"); args.push_back("[aout]");
                }
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

    // ── Allocate triple-buffered PBOs for async GPU→CPU readback ─────────────
    // With VAAPI, ffmpeg handles the vflip so we don't need the flip memcpy;
    // pixel_buf is still used for the libx264 path.
    size_t frame_bytes = (size_t)out_w * out_h * 4;
    GLuint pbos[3] = {};
    glGenBuffers(3, pbos);
    for (int i = 0; i < 3; ++i) {
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
    g_gl_ex.pbo[2]        = pbos[2];
    g_gl_ex.use_vaapi     = use_vaapi;
    g_gl_ex.face_waits    = std::move(face_waits);
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
    int slot = frame_idx % 3;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, g_gl_ex.pbo[slot]);
    uint8_t* src = (uint8_t*)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    int out_w     = g_gl_ex.out_w;
    int out_h     = g_gl_ex.out_h;
    size_t total  = (size_t)out_w * out_h * 4;
    const uint8_t* write_src = nullptr;
    if (src) {
        int row_bytes = out_w * 4;
        if (g_gl_ex.use_vaapi) {
            // VAAPI: ffmpeg applies vflip — write directly from the mapped PBO,
            // skip the intermediate full-frame memcpy entirely (~5 ms at 1080p).
            write_src = src;
        } else {
            // libx264: flip rows here (GL bottom-up → top-down for MP4). We still
            // need the staging buffer because the flip can't be done in-place.
            for (int y = 0; y < out_h; ++y) {
                int src_y = out_h - 1 - y;
                memcpy(g_gl_ex.pixel_buf.data() + (size_t)y * row_bytes,
                       src + (size_t)src_y * row_bytes, row_bytes);
            }
            write_src = g_gl_ex.pixel_buf.data();
        }
    }

    if (write_src) {
        const uint8_t* buf = write_src;
        size_t left = total;
        while (left > 0) {
            ssize_t n = write(g_gl_ex.pipe_write, buf, left);
            if (n < 0) {
                // ffmpeg died — note it and stop trying. Render loop will
                // notice on next finalize attempt and clean up.
                if (errno == EPIPE) {
                    rlog("pipe_write: EPIPE — ffmpeg gone, abandoning frame %d\n", frame_idx);
                    g_cancel.store(true);
                }
                break;
            }
            if (n == 0) break;
            buf += n; left -= (size_t)n;
        }
    }

    if (src) glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
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

    // Face-filter caches still building: hold the first frame until they're
    // ready (a filter silently missing from an export reads as "broken").
    // Failed builds are dropped — those clips export unfiltered.
    if (!g_gl_ex.face_waits.empty() && g_gl_ex.current_frame == 0) {
        float worst = 1.f;
        auto& fw = g_gl_ex.face_waits;
        for (auto it = fw.begin(); it != fw.end();) {
            float p = 0.f;
            FaceCacheStatus st = face_cache_status(it->first, &p);
            if (st == FaceCacheStatus::Ready || st == FaceCacheStatus::Failed ||
                st == FaceCacheStatus::None) {
                it = fw.erase(it);
            } else {
                worst = std::min(worst, p);
                ++it;
            }
        }
        if (!fw.empty()) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Tracking faces… %d%%",
                     (int)(worst * 100.f));
            state.render.stage = buf;
            return;
        }
        state.render.stage = "Encoding…";
    }

    auto tick_t0 = perf_clock::now();

    // Triple-buffer pipeline: at tick N we collect frame N-2 (kicked at tick
    // N-2). First two ticks skip collection because nothing has finished DMA
    // yet. After the last frame is rendered we keep ticking for one more tick
    // to drain the final two pending PBOs (handled by the finalize threshold
    // bumping to total_frames + 1).
    {
        auto t0 = perf_clock::now();
        if (g_gl_ex.current_frame > 1)
            gl_collect_pbo_frame(g_gl_ex.current_frame - 2);
        g_perf.collect_us += us_since(t0);
    }

    rlog("  readpixels_done\n");   // previous frame collected (or first frame skipped)

    // Drain tick: all frames have been rendered/kicked, just collect what's
    // left in flight on subsequent ticks. Skip the render+kick block below.
    if (g_gl_ex.current_frame >= g_gl_ex.total_frames &&
        g_gl_ex.current_frame <  g_gl_ex.total_frames + 1) {
        g_gl_ex.current_frame++;
        return;
    }

    // All frames rendered + last frames drained → signal ffmpeg and wait.
    if (g_gl_ex.current_frame >= g_gl_ex.total_frames + 1) {
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
    auto compose_t0 = perf_clock::now();
    ImDrawList dl(ImGui::GetDrawListSharedData());
    dl._ResetForNewFrame();
    dl.PushClipRect({0.f, 0.f}, {W, H});
    dl.PushTexture(ImGui::GetIO().Fonts->TexRef);

    // Bind + clear the export FBO up front so standalone FX bricks can flush the
    // tracks-below into it and run their FX mid-loop (group-bus scoping).
    glBindFramebuffer(GL_FRAMEBUFFER, g_gl_ex.fbo);
    glViewport(0, 0, g_gl_ex.out_w, g_gl_ex.out_h);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    auto flush_dl = [&]() {
        dl.PopTexture(); dl.PopClipRect();
        ImDrawData fdd; fdd.DisplayPos = {0,0}; fdd.DisplaySize = {W,H};
        fdd.FramebufferScale = {1,1}; fdd.Textures = &ImGui::GetIO().Fonts->TexList;
        fdd.AddDrawList(&dl);
        glBindFramebuffer(GL_FRAMEBUFFER, g_gl_ex.fbo);  // per-clip FX may have rebound
        glViewport(0, 0, g_gl_ex.out_w, g_gl_ex.out_h);
        ImGui_ImplOpenGL3_RenderDrawData(&fdd);
        dl._ResetForNewFrame();
        dl.PushClipRect({0.f, 0.f}, {W, H});
        dl.PushTexture(ImGui::GetIO().Fonts->TexRef);
    };
    auto apply_track_fx = [&](int ti) {
        EffectAccum     ea  = collect_effects_for_track(state, t, ti);
        CreativeFXAccum cfx = collect_creative_fx_for_track(state, t, ti);
        if (!(ea.any_color || ea.any_blur || ea.any_vignette || ea.any_text ||
              cfx.any_cfx || cfx.any_gen_fx)) return;
        flush_dl();                                   // below-tracks → FBO
        glBindFramebuffer(GL_FRAMEBUFFER, 0);         // detach so color_tex is readable
        uintptr_t out = fx_apply((uintptr_t)g_gl_ex.color_tex, kSceneFxSlot,
                                 g_gl_ex.out_w, g_gl_ex.out_h, ea, cfx, t);
        glBindFramebuffer(GL_FRAMEBUFFER, g_gl_ex.fbo);
        glViewport(0, 0, g_gl_ex.out_w, g_gl_ex.out_h);
        if (out != (uintptr_t)g_gl_ex.color_tex)
            fx_blit(out, g_gl_ex.fbo, g_gl_ex.out_w, g_gl_ex.out_h);
    };

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
            if (clip_is_videolike_type(cl.clip_type) && !cl.text.empty() &&
                t >= cl.start && t < cl.end)
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
        if (!active) {  // text-only / FX-only track — still draw text + run its FX
            draw_text_overlays(&dl, state, t, {0.f, 0.f}, W, H, ti);
            apply_track_fx(ti);   // standalone FX bricks process the tracks below
            continue;
        }

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
            } else if (active->transition_type == TransitionType::Shake) {
                if (t_b <= 0.f)
                    gl_render_vid_clip(dl, active,  t, 1.f, tex_pri, slot_pri, W, H, state, ti, false, t_a);
                else
                    gl_render_vid_clip(dl, next_cl, t, 1.f, tex_sec, slot_sec, W, H, state, ti, false, 1.f-t_b);
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
            } else if (prev_cl->transition_type == TransitionType::Shake) {
                gl_render_vid_clip(dl, active, t, 1.f, tex_pri, slot_pri, W, H, state, ti, false, 1.f-tf);
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
        // This track's text at its z-order (foreground tracks composite over it).
        draw_text_overlays(&dl, state, t, {0.f, 0.f}, W, H, ti);
        apply_track_fx(ti);   // standalone FX bricks process the tracks below
    }

    g_perf.compose_us += us_since(compose_t0);
    rlog("  vid_clips_done\n");

    // ── Phase 2: Render remaining video clips to export FBO ───────────────────
    // The FBO was bound + cleared up front (before the loop); standalone FX
    // bricks flushed the tracks below them into it mid-loop. Here we render any
    // dl content accumulated after the last flush — it blends over the FBO at
    // the correct z-order. No clear: that would erase the per-track FX results.
    auto render_t0 = perf_clock::now();
    glBindFramebuffer(GL_FRAMEBUFFER, g_gl_ex.fbo);  // per-track FX may have rebound
    glViewport(0, 0, g_gl_ex.out_w, g_gl_ex.out_h);
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
    g_perf.render_us += us_since(render_t0);
    rlog("  vid_render_done\n");

    // (Scene FX are applied per-track during the loop above — each standalone FX
    // brick processes the tracks below it — so there's no whole-frame FX pass.)

    // (Text overlays are now drawn per-track inside Phase 1's video loop so they
    // composite at each track's z-order instead of always on top. The old
    // always-on-top Phase 4 pass is gone.)

    // ── Kick async GPU→PBO DMA (non-blocking — returns immediately) ───────────
    // The GPU will fill pbo[current_frame % 3] while the CPU processes the next
    // two frames; collection happens 2 ticks later.
    auto kick_t0 = perf_clock::now();
    glBindBuffer(GL_PIXEL_PACK_BUFFER, g_gl_ex.pbo[g_gl_ex.current_frame % 3]);
    glReadPixels(0, 0, g_gl_ex.out_w, g_gl_ex.out_h,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr); // nullptr = async into PBO
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    g_perf.kick_us += us_since(kick_t0);

    rlog("  pipe_write_done\n");  // will be written at start of next tick

    // ── Restore GL state ──────────────────────────────────────────────────────
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);

    // ── Update progress ───────────────────────────────────────────────────────
    ++g_gl_ex.current_frame;
    state.render.frame    = g_gl_ex.current_frame;
    state.render.progress = (float)g_gl_ex.current_frame / (float)g_gl_ex.total_frames;

    g_perf.total_us += us_since(tick_t0);
    g_perf.count++;
    if (g_perf.count >= kPerfWindow) {
        double n = (double)g_perf.count;
        double tot_ms = g_perf.total_us / n / 1000.0;
        double fps    = (tot_ms > 0.0) ? 1000.0 / tot_ms : 0.0;
        fprintf(stderr,
            "[render perf %d-frame avg] %.2f ms/tick (~%.1f fps)  "
            "collect=%.2f compose=%.2f render=%.2f fx=%.2f text=%.2f kick=%.2f ms\n",
            kPerfWindow, tot_ms, fps,
            g_perf.collect_us / n / 1000.0,
            g_perf.compose_us / n / 1000.0,
            g_perf.render_us  / n / 1000.0,
            g_perf.fx_us      / n / 1000.0,
            g_perf.text_us    / n / 1000.0,
            g_perf.kick_us    / n / 1000.0);
        rlog("perf60 collect=%.2f compose=%.2f render=%.2f fx=%.2f text=%.2f kick=%.2f total=%.2f ms (%.1f fps)\n",
            g_perf.collect_us / n / 1000.0,
            g_perf.compose_us / n / 1000.0,
            g_perf.render_us  / n / 1000.0,
            g_perf.fx_us      / n / 1000.0,
            g_perf.text_us    / n / 1000.0,
            g_perf.kick_us    / n / 1000.0,
            tot_ms, fps);
        g_perf.reset();
    }
}
