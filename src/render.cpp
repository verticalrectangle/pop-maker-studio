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

// ── Filter-complex script writer ──────────────────────────────────────────────

// Represents one video input to ffmpeg (path + seek boundaries).
struct VidSpec { std::string path; float ss=0.f; float to=-1.f; int in_idx=-1; };

static bool write_filter_script(
    const AppState& state,
    const std::string& path,
    const std::vector<VidSpec>& vid_specs,  // ordered video inputs
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

    // ── Base (first video or black) ───────────────────────────────────────────
    std::string vcur;
    if (!vid_specs.empty()) {
        const VidSpec& v0 = vid_specs[0];
        line() << "[" << v0.in_idx << ":v]"
               << "scale=" << out_w << ":" << out_h
               << ":force_original_aspect_ratio=decrease,"
               << "pad=" << out_w << ":" << out_h
               << ":(ow-iw)/2:(oh-ih)/2:color=black,"
               << "setsar=1,format=yuv420p"
               << "[vbase]";
    } else {
        line() << "color=c=black:s=" << out_w << "x" << out_h
               << ":r=30,format=yuv420p[vbase]";
    }
    vcur = "[vbase]";

    // ── Overlay extra video layers (indices 1+) ───────────────────────────────
    for (int vi = 1; vi < (int)vid_specs.size(); ++vi) {
        const VidSpec& vs   = vid_specs[vi];
        // Find matching video track in state (by path)
        const Clip* cl_ptr = nullptr;
        for (auto& tr : state.tracks) {
            for (auto& cl : tr.clips)
                if (cl.clip_type == ClipType::Video && cl.text == vs.path)
                    { cl_ptr = &cl; break; }
            if (cl_ptr) break;
        }
        if (!cl_ptr) continue;
        const Clip& cl = *cl_ptr;

        // Scale expressions (pixels)
        std::string sw_expr = prop_expr(cl, "scale_x", (float)out_w, cl.scale_x);
        std::string sh_expr = prop_expr(cl, "scale_y", (float)out_h, cl.scale_y);

        std::string layer_tag = "[vlayer" + std::to_string(vi) + "]";
        std::string layer_rgba = "[vlr" + std::to_string(vi) + "]";

        // Scale to desired size
        line() << "[" << vs.in_idx << ":v]"
               << "scale=" << sw_expr << ":" << sh_expr
               << ",format=rgba"
               << layer_tag;

        // Opacity via colorchannelmixer (only if not fully opaque / keyframed)
        bool has_opa = (cl_ptr->ktracks.count("opacity") > 0 ||
                        fabsf(cl.opacity - 1.f) > 0.01f);
        std::string layer_in = layer_tag;
        if (has_opa) {
            std::string opa_expr = prop_expr(cl, "opacity", 1.f, cl.opacity);
            std::string opa_tag  = "[vlopa" + std::to_string(vi) + "]";
            line() << layer_tag
                   << "colorchannelmixer=aa=" << opa_expr
                   << opa_tag;
            layer_in = opa_tag;
        }

        // Overlay position expressions (pixels from top-left corner of canvas,
        // centred on pos_x * out_w, pos_y * out_h).
        std::string x_expr = "(" + prop_expr(cl, "pos_x", (float)out_w, cl.pos_x) + "-iw/2)";
        std::string y_expr = "(" + prop_expr(cl, "pos_y", (float)out_h, cl.pos_y) + "-ih/2)";

        // Enable expression — only show while the clip is active.
        float en_start = fmaxf(0.f, cl.start - vs.ss);
        float en_end   = fmaxf(0.f, cl.end   - vs.ss);

        std::string vnext = "[vov" + std::to_string(vi) + "]";
        line() << vcur << layer_in
               << "overlay=x=" << x_expr << ":y=" << y_expr
               << ":format=auto"
               << ":enable='between(t,"
               << std::fixed << std::setprecision(3) << en_start << "," << en_end << ")'"
               << vnext;
        vcur = vnext;
    }

    // ── Subtitle drawtext chain ───────────────────────────────────────────────
    int font_sz = (out_h >= 1500) ? 96 : 72;
    int si = 0;
    for (auto& [ti, ci] : state.subtitle_clip_indices()) {
        const Track& tr = state.tracks[ti];
        const Clip&  cl = tr.clips[ci];
        if (!tr.visible || cl.text.empty()) continue;

        // Y expression
        std::string y_e;
        if      (cl.sub_pos == 1) y_e = "(h-text_h)/2";
        else if (cl.sub_pos == 2) y_e = "h*0.10";
        else if (cl.sub_pos == 3) {
            char yb[64]; snprintf(yb, sizeof(yb), "h*%.4f-text_h/2", (double)cl.sub_pos_y);
            y_e = yb;
        } else {
            y_e = "h*0.88-text_h";
        }

        // Color
        char col[32];
        if (cl.sub_color_override)
            snprintf(col, sizeof(col), "0x%02x%02x%02x%02x",
                (int)(cl.sub_color[0]*255), (int)(cl.sub_color[1]*255),
                (int)(cl.sub_color[2]*255), (int)(cl.sub_color[3]*255));
        else
            strcpy(col, "white");

        std::string vnext = "[vsub" + std::to_string(si++) + "]";
        line() << vcur
               << "drawtext="
               << "fontfile=" << esc(g_font_path) << ":"
               << "text="     << esc(cl.text)     << ":"
               << "fontsize=" << font_sz           << ":"
               << "fontcolor=" << col              << ":"
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
    vout_label = vcur;

    // ── Audio volume ──────────────────────────────────────────────────────────
    if (aud_in >= 0) {
        float vol = 1.f;
        for (auto& tr : state.tracks) {
            if (tr.muted) continue;
            for (auto& cl : tr.clips) {
                if (cl.clip_type == ClipType::Text) continue;
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

    // Collect all video track clips as separate inputs (ordered = layer order).
    struct InSpec { std::string path; float ss=0.f; float to=-1.f; };
    std::vector<VidSpec> vid_specs;
    InSpec audio_in;

    // Collect video clips across all tracks (in track order = z-order),
    // deduplicated by path so the same file on two tracks uses one ffmpeg input.
    for (auto& tr : state.tracks) {
        for (auto& cl : tr.clips) {
            if (cl.clip_type != ClipType::Video) continue;
            if (cl.text.empty() || !fs::exists(cl.text)) continue;
            // Dedup: skip if path already registered.
            bool dup = false;
            for (auto& v : vid_specs) if (v.path == cl.text) { dup=true; break; }
            if (dup) continue;
            VidSpec vs; vs.path = cl.text; vs.ss = cl.start; vs.to = cl.end;
            vid_specs.push_back(vs);
        }
    }

    audio_in.path = state.audio_path;
    for (auto& tr : state.tracks) {
        bool found = false;
        for (auto& cl : tr.clips) {
            if (cl.clip_type != ClipType::Audio) continue;
            if (cl.text.empty() || !fs::exists(cl.text)) continue;
            audio_in.path = cl.text;
            audio_in.ss   = cl.start;
            audio_in.to   = cl.end;
            found = true; break;
        }
        if (found) break;
    }
    if (!audio_in.path.empty() && !fs::exists(audio_in.path)) audio_in.path.clear();

    if (vid_specs.empty() && audio_in.path.empty()) return {};

    // Assign input indices: video tracks first, then audio.
    int n_in = 0;
    for (auto& vs : vid_specs) vs.in_idx = n_in++;
    int aud_in = -1;
    if (!audio_in.path.empty()) { aud_in = n_in++; }

    // Build flat ffmpeg input list (same order).
    std::vector<InSpec> inputs;
    for (auto& vs : vid_specs) inputs.push_back({vs.path, vs.ss, vs.to});
    if (aud_in >= 0) inputs.push_back(audio_in);

    // Subtitle offset = start of first video clip (or audio clip).
    float sub_offset = (!vid_specs.empty() && vid_specs[0].ss > 0.001f)
                     ? vid_specs[0].ss
                     : (audio_in.ss > 0.001f ? audio_in.ss : 0.f);

    // Output duration from primary video/audio clip.
    float out_duration = state.duration;
    if (!vid_specs.empty() && vid_specs[0].to > 0.001f)
        out_duration = vid_specs[0].to - vid_specs[0].ss;
    else if (audio_in.to > 0.001f)
        out_duration = audio_in.to - audio_in.ss;

    // Write filter script
    std::string script = "/tmp/pms_filter.txt";
    std::string vout, aout;
    if (!write_filter_script(state, script, vid_specs, aud_in,
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
