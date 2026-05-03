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

// ── Filter-complex script writer ──────────────────────────────────────────────

static bool write_filter_script(
    const AppState& state,
    const std::string& path,
    int vid_in, int aud_in,
    int out_w, int out_h,
    std::string& vout_label,
    std::string& aout_label)
{
    std::ostringstream fc;
    bool first = true;
    // Helper: append a new filter line, preceded by ";\n" after the first
    auto line = [&]() -> std::ostringstream& {
        if (!first) fc << ";\n";
        first = false;
        return fc;
    };

    // ── Video source ──────────────────────────────────────────────────────────
    std::string vcur;
    if (vid_in >= 0) {
        line() << "[" << vid_in << ":v]"
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

    // ── Subtitle drawtext chain ───────────────────────────────────────────────
    int font_sz = (out_h >= 1500) ? 96 : 72;
    int si = 0;
    for (auto& [ti, ci] : state.subtitle_clip_indices()) {
        const Track& tr = state.tracks[ti];
        const Clip&  cl = tr.clips[ci];
        if (!tr.visible || cl.text.empty()) continue;

        // Y expression
        std::string y_expr;
        if      (cl.sub_pos == 1) y_expr = "(h-text_h)/2";
        else if (cl.sub_pos == 2) y_expr = "h*0.10";
        else if (cl.sub_pos == 3) {
            char yb[64];
            snprintf(yb, sizeof(yb), "h*%.4f-text_h/2", (double)cl.sub_pos_y);
            y_expr = yb;
        } else {
            y_expr = "h*0.88-text_h";
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
               << "y=" << y_expr                  << ":"
               << "enable='between(t,"
               << std::fixed << std::setprecision(3)
               << cl.start << "," << cl.end << ")'"
               << vnext;
        vcur = vnext;
    }
    vout_label = vcur;

    // ── Audio volume ──────────────────────────────────────────────────────────
    if (aud_in >= 0) {
        float vol = 1.f;
        for (auto& tr : state.tracks) {
            if ((tr.type == TrackType::Audio || tr.type == TrackType::Video)
                    && !tr.clips.empty() && tr.muted == false) {
                vol = tr.clips[0].volume;
                break;
            }
        }
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

    // Find best video and audio source files
    std::string video_file;
    for (auto& tr : state.tracks)
        if (tr.type == TrackType::Video && !tr.clips.empty()
                && !tr.clips[0].text.empty() && fs::exists(tr.clips[0].text)) {
            video_file = tr.clips[0].text; break;
        }

    std::string audio_file = state.audio_path;
    for (auto& tr : state.tracks)
        if (tr.type == TrackType::Audio && !tr.clips.empty()
                && !tr.clips[0].text.empty() && fs::exists(tr.clips[0].text)) {
            audio_file = tr.clips[0].text; break;
        }
    if (!audio_file.empty() && !fs::exists(audio_file)) audio_file.clear();

    int n_in = 0, vid_in = -1, aud_in = -1;
    std::vector<std::string> in_files;
    if (!video_file.empty()) { vid_in = n_in++; in_files.push_back(video_file); }
    if (!audio_file.empty()) { aud_in = n_in++; in_files.push_back(audio_file); }
    if (n_in == 0) return {};

    // Write filter script
    std::string script = "/tmp/pms_filter.txt";
    std::string vout, aout;
    if (!write_filter_script(state, script, vid_in, aud_in, out_w, out_h, vout, aout))
        return {};

    const auto& rs = state.render_settings;

    std::vector<std::string> a;
    a.push_back("ffmpeg");
    a.push_back("-hide_banner");
    a.push_back("-loglevel"); a.push_back("error");
    a.push_back("-progress"); a.push_back("pipe:1");
    a.push_back("-y");

    for (auto& f : in_files) { a.push_back("-i"); a.push_back(f); }

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

    if (state.duration > 0.f)
        { a.push_back("-t"); a.push_back(std::to_string(state.duration)); }

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
