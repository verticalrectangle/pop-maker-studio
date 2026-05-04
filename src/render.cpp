#include "render.h"
#include "inter_font.h"   // inter_black_ttf[], inter_black_ttf_size

#include <fstream>
#include <sstream>
#include <iomanip>
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
                              float scale_factor, float default_val = -999.f)
{
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
    int track_idx, clip_idx;
    int   in_idx  = -1;   // Vid: ffmpeg input index
    float vid_ss  = 0.f;  // Vid: -ss seek of that input (for enable expr)
};

// ── Filter-complex script writer ──────────────────────────────────────────────

static bool write_filter_script(
    const AppState& state,
    const std::string& path,
    const std::vector<RLayer>& layers,
    int aud_in,
    int out_w, int out_h,
    float sub_offset,
    std::string& vout_label,
    std::string& aout_label)
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
    bool base_done = false;  // true once first video overlay is composited

    // ── Black canvas base ─────────────────────────────────────────────────────
    line() << "color=c=black:s=" << out_w << "x" << out_h << ":r=30[vbase]";
    std::string vcur = "[vbase]";

    // ── Process all layers bottom-to-top ─────────────────────────────────────
    for (const RLayer& rl : layers) {
        const Track& tr = state.tracks[rl.track_idx];
        const Clip&  cl = tr.clips[rl.clip_idx];
        if (!tr.visible) continue;

        if (rl.kind == RLayer::Vid) {
            std::string scaled_tag = "[vsc"  + std::to_string(vid_idx) + "]";
            std::string layer_in   = scaled_tag;

            if (!base_done) {
                // Bottom-most video: scale+pad to fill canvas (letterbox).
                line() << "[" << rl.in_idx << ":v]"
                       << "scale=" << out_w << ":" << out_h
                       << ":force_original_aspect_ratio=decrease,"
                       << "pad=" << out_w << ":" << out_h
                       << ":(ow-iw)/2:(oh-ih)/2:color=black,"
                       << "setsar=1,format=rgba"
                       << scaled_tag;
                base_done = true;
            } else {
                // Upper video layers: scale to user-specified size.
                std::string sw = prop_expr(cl, "scale_x", (float)out_w, cl.scale_x);
                std::string sh = prop_expr(cl, "scale_y", (float)out_h, cl.scale_y);
                line() << "[" << rl.in_idx << ":v]"
                       << "scale=" << sw << ":" << sh
                       << ",format=rgba"
                       << scaled_tag;
            }

            // Optional opacity
            bool has_opa = (cl.ktracks.count("opacity") > 0 ||
                            fabsf(cl.opacity - 1.f) > 0.01f);
            if (has_opa) {
                std::string opa_tag = "[vopa" + std::to_string(vid_idx) + "]";
                line() << scaled_tag
                       << "colorchannelmixer=aa="
                       << prop_expr(cl, "opacity", 1.f, cl.opacity)
                       << opa_tag;
                layer_in = opa_tag;
            }

            // Position (base fills canvas at 0,0; upper layers use pos_x/pos_y)
            std::string x_e = base_done && vid_idx > 0
                ? "(" + prop_expr(cl, "pos_x", (float)out_w, cl.pos_x) + "-iw/2)"
                : "0";
            std::string y_e = base_done && vid_idx > 0
                ? "(" + prop_expr(cl, "pos_y", (float)out_h, cl.pos_y) + "-ih/2)"
                : "0";

            // Enable window
            float en0 = fmaxf(0.f, cl.start - rl.vid_ss);
            float en1 = fmaxf(0.f, cl.end   - rl.vid_ss);

            std::string vnext = "[vov" + std::to_string(vid_idx) + "]";
            line() << vcur << layer_in
                   << "overlay=x=" << x_e << ":y=" << y_e
                   << ":format=auto"
                   << ":enable='between(t,"
                   << std::fixed << std::setprecision(3)
                   << en0 << "," << en1 << ")'"
                   << vnext;
            vcur = vnext;
            ++vid_idx;

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

            // Collect words from cache that belong to this clip
            std::vector<const WordEntry*> clip_words;
            for (auto& we : state.words_cache)
                if (we.end > cl.start && we.start < cl.end)
                    clip_words.push_back(&we);

            bool has_karaoke = !clip_words.empty();

            // Base layer: dim color for the full clip duration
            char dim_col[32];
            if (cl.sub_color_override)
                snprintf(dim_col, sizeof(dim_col), "0x%02x%02x%02x%02x",
                    (int)(cl.sub_color[0]*255), (int)(cl.sub_color[1]*255),
                    (int)(cl.sub_color[2]*255),
                    has_karaoke ? (int)(cl.sub_color[3]*255*0.4f) : (int)(cl.sub_color[3]*255));
            else
                strcpy(dim_col, has_karaoke ? "white@0.4" : "white");

            {
                std::string vnext = "[vtxt" + std::to_string(txt_idx++) + "]";
                line() << vcur
                       << "drawtext="
                       << "fontfile=" << esc(g_font_path) << ":"
                       << "text="     << esc(cl.text)     << ":"
                       << "fontsize=" << font_sz           << ":"
                       << "fontcolor=" << dim_col          << ":"
                       << "borderw=3:bordercolor=black@0.7:"
                       << "shadowx=2:shadowy=2:shadowcolor=black@0.5:"
                       << "x=(w-text_w)/2:"
                       << "y=" << y_e                     << ":"
                       << "enable='between(t,"
                       << std::fixed << std::setprecision(3)
                       << fmaxf(0.f, cl.start - sub_offset) << ","
                       << fmaxf(0.f, cl.end   - sub_offset) << ")'"
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
                           << "fontsize=" << font_sz           << ":"
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

    // ── Audio volume ──────────────────────────────────────────────────────────
    if (aud_in >= 0) {
        float vol = 1.f;
        for (auto& tr : state.tracks) {
            if (tr.muted) continue;
            for (auto& cl : tr.clips) {
                if (cl.clip_type == ClipType::Text  || cl.clip_type == ClipType::Lyrics ||
                    cl.clip_type == ClipType::Subtitle) continue;
                vol = cl.volume; goto vol_done;
            }
        }
        vol_done:;
        char vbuf[32]; snprintf(vbuf, sizeof(vbuf), "%.3f", (double)vol);
        line() << "[" << aud_in << ":a]volume=" << vbuf << "[aout]";
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
    struct VidInput { std::string path; float ss=0.f, to=-1.f; int in_idx=-1; };
    struct AudioIn  { std::string path; float ss=0.f, to=-1.f; };
    std::vector<VidInput> vid_inputs;
    std::vector<RLayer>   layers;
    AudioIn audio_in;
    audio_in.path = state.audio_path;

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
            if (cl.clip_type == ClipType::Video) {
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
                audio_in.path = cl.text;
                audio_in.ss   = cl.start;
                audio_in.to   = cl.end;
            }
        }
    }

    if (!audio_in.path.empty() && !fs::exists(audio_in.path)) audio_in.path.clear();
    if (layers.empty() && audio_in.path.empty()) return {};

    // Assign real ffmpeg input indices: video files first, then audio.
    int n_in = 0;
    for (auto& vi : vid_inputs) vi.in_idx = n_in++;
    int aud_in = -1;
    if (!audio_in.path.empty()) aud_in = n_in++;

    // Resolve layer in_idx from vid_inputs array index → actual ffmpeg index,
    // and fill vid_ss for enable expression offset.
    for (auto& rl : layers) {
        if (rl.kind != RLayer::Vid) continue;
        int arr = rl.in_idx;
        rl.vid_ss = vid_inputs[arr].ss;
        rl.in_idx = vid_inputs[arr].in_idx;
    }

    // Flat ffmpeg input list: all video files, then audio.
    struct InSpec { std::string path; float ss=0.f, to=-1.f; };
    std::vector<InSpec> inputs;
    for (auto& vi : vid_inputs) inputs.push_back({vi.path, vi.ss, vi.to});
    if (aud_in >= 0) inputs.push_back({audio_in.path, audio_in.ss, audio_in.to});

    // sub_offset: project time at which the rendered video begins
    // (equals the -ss of the base video input, or audio start).
    float sub_offset = (!vid_inputs.empty() && vid_inputs[0].ss > 0.001f)
                     ? vid_inputs[0].ss
                     : (audio_in.ss > 0.001f ? audio_in.ss : 0.f);

    float out_duration = state.duration;
    if (!vid_inputs.empty() && vid_inputs[0].to > 0.001f)
        out_duration = vid_inputs[0].to - vid_inputs[0].ss;
    else if (audio_in.to > 0.001f)
        out_duration = audio_in.to - audio_in.ss;

    // Write filter script
    std::string script = "/tmp/pms_filter.txt";
    std::string vout, aout;
    if (!write_filter_script(state, script, layers, aud_in,
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
        if (inp.ss > 0.001f) { a.push_back("-ss"); a.push_back(std::to_string(inp.ss)); }
        if (inp.to > 0.001f) { a.push_back("-to"); a.push_back(std::to_string(inp.to)); }
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
