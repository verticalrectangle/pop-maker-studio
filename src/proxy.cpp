#include "proxy.h"
#include "platform.h"
#include "paths.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>

#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>

namespace fs = std::filesystem;

static bool file_has_alpha(const std::string& path);
static bool file_has_alpha_cached(const std::string& path);

// ── Ready-state cache ─────────────────────────────────────────────────────────
//
// Without this, proxy_is_ready() and proxy_job_status() are called from the
// timeline draw loop on every visible video clip every frame. With 20+ clips at
// 60 fps that's thousands of stat() syscalls per second, plus shared-mutex
// contention with the proxy worker thread.
//
// "Ready" is a terminal state in a session: once a proxy exists on disk it stays
// there until the user removes the file outside the app. So we cache positive
// hits forever (in-session) and throttle negative re-stats to ~4 Hz per path.
static std::mutex                              g_ready_cache_mu;
static std::unordered_set<std::string>         g_ready_cache;
static std::unordered_map<std::string, double> g_last_stat_ts;

static double mono_now_seconds() {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

static void mark_ready_cached(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_ready_cache_mu);
    g_ready_cache.insert(path);
}

// ── Path helpers ──────────────────────────────────────────────────────────────

std::string proxy_interm_path(const std::string& vp)     { return cache_path(vp, ".interm.mp4"); }
std::string proxy_interm_idx_path(const std::string& vp) { return cache_path(vp, ".interm.idx"); }
static std::string proxy_fail_path(const std::string& vp) { return proxy_interm_path(vp) + ".fail"; }

// Persist a proxy failure, keyed to the source's size+mtime. Writing the
// source file (a re-export to the same path) makes proxy_failure() see a
// stale marker, drop it, and allow a fresh attempt — a corrupt file left in
// place keeps failing fast instead of churning the worker forever.
static void mark_proxy_failed(const std::string& path, const std::string& err) {
    std::error_code ec;
    auto sz = fs::file_size(path, ec);
    if (ec) return;
    auto mt = fs::last_write_time(path, ec);
    if (ec) return;
    FILE* f = fopen(proxy_fail_path(path).c_str(), "w");
    if (!f) return;
    fprintf(f, "%llu %lld\n%s\n", (unsigned long long)sz,
            (long long)mt.time_since_epoch().count(), err.c_str());
    fclose(f);
    fprintf(stderr, "[proxy] FAILED %s: %s\n", path.c_str(), err.c_str());
}

// Non-empty error string when generation failed and the source hasn't been
// replaced since. Cheap for the common case (one stat of a missing file).
std::string proxy_failure(const std::string& video_path) {
    if (proxy_is_ready(video_path)) return {};   // succeeded — never "failed"
    std::string fp = proxy_fail_path(video_path);
    std::error_code ec;
    if (!fs::exists(fp, ec)) return {};
    FILE* f = fopen(fp.c_str(), "r");
    if (!f) return {};
    char head[128] = {0};
    char err[512]  = {0};
    if (!fgets(head, sizeof(head), f) || !fgets(err, sizeof(err), f)) {
        fclose(f);
        return {};
    }
    fclose(f);
    unsigned long long sz = 0;
    long long mt = 0;
    if (sscanf(head, "%llu %lld", &sz, &mt) != 2) return {};
    std::error_code sec;
    bool stale = !fs::exists(video_path, sec) ||
                 sz  != (unsigned long long)fs::file_size(video_path, sec) ||
                 mt  != (long long)fs::last_write_time(video_path, sec).time_since_epoch().count();
    if (stale) { fs::remove(fp); return {}; }
    std::string e = err;
    while (!e.empty() && (e.back() == '\n' || e.back() == '\r')) e.pop_back();
    return e;
}

// Does this file's CONTENT decode with stb_image (PNG/JPEG/BMP/GIF)? The
// EXTENSION lies — a ".png" is very often really WebP/HEIC/AVIF, which stb can't
// read, so loading the "original" renders blank. Magic-byte sniff, cached (this
// is hit per-frame for image clips).
static bool content_is_stbi_image(const std::string& path) {
    static std::mutex mu;
    static std::unordered_map<std::string, bool> cache;
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = cache.find(path);
        if (it != cache.end()) return it->second;
    }
    bool ok = false;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        unsigned char b[12] = {0};
        size_t n = fread(b, 1, sizeof(b), f);
        fclose(f);
        if (n >= 4)
            ok = (b[0]==0x89 && b[1]=='P' && b[2]=='N' && b[3]=='G') ||  // PNG
                 (b[0]==0xFF && b[1]==0xD8 && b[2]==0xFF)            ||  // JPEG
                 (b[0]=='B'  && b[1]=='M')                          ||  // BMP
                 (b[0]=='G'  && b[1]=='I' && b[2]=='F');                // GIF
    }
    std::lock_guard<std::mutex> lk(mu);
    cache[path] = ok;
    return ok;
}

std::string proxy_still_path(const std::string& vp) {
    // Load the original directly only when its BYTES are a stb-readable image —
    // full quality, alpha intact. The extension can't be trusted (".png" files
    // are routinely WebP/HEIC); those get a CONVERTED still, written as PNG so a
    // transparent source keeps its alpha (a JPEG still would flatten it).
    if (content_is_stbi_image(vp)) return vp;
    return cache_path(vp, ".still.png");
}

// ── Queue + worker pool state ─────────────────────────────────────────────────
//
// Multiple worker threads pull from g_queue in parallel so an import of 20+
// clips actually fans out across CPU cores instead of serializing on a single
// ffmpeg subprocess. Each worker holds at most one in-flight path; tracking
// is per-path via the g_active / g_progress_map / g_pid_map containers so two
// workers never grab the same source. PROXY_MAX_WORKERS caps concurrency so
// we don't trash a busy machine; per-process -threads is sized so the workers
// × threads product stays at ~hardware_concurrency without oversubscription.
static std::mutex                                  g_mu;
static std::deque<std::string>                     g_queue;        // pending
static std::unordered_set<std::string>             g_active;       // in-flight
static std::unordered_map<std::string, float>      g_progress_map; // per-active 0-1
static std::unordered_map<std::string, pid_t>      g_pid_map;      // per-active pid
static int                                         g_workers_alive = 0;

static int proxy_max_workers() {
    int hw = (int)std::thread::hardware_concurrency();
    if (hw < 2) hw = 2;
    int n = hw / 2;
    if (n < 1) n = 1;
    if (n > 4) n = 4;
    return n;
}

static int proxy_threads_per_job() {
    int hw = (int)std::thread::hardware_concurrency();
    if (hw < 2) hw = 2;
    int n = proxy_max_workers();
    int k = hw / n;
    if (k < 2) k = 2;
    if (k > 4) k = 4;
    return k;
}

// ── Subprocess helper ─────────────────────────────────────────────────────────

static pid_t spawn_ffmpeg(const char** args) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        // Lower priority so the UI stays smooth while the worker pool churns.
        // Best-effort: ignore failure (we don't have CAP_SYS_NICE for negatives,
        // but going positive is unprivileged on Linux).
        (void)nice(10);
        execvp("ffmpeg", const_cast<char**>(args));
        _exit(127);
    }
    return pid;
}

// ── Seek-table builder ────────────────────────────────────────────────────────
//
// The intermediate is all-intra (-g 1), so every packet is a keyframe and the
// seek table is just the byte offset of each frame's packet inside the mp4,
// read straight from ffprobe's packet table (the `pos` of every packet whose
// flags contain "K_"). Layout is identical to the old MJPEG index: u32 frame
// count + u64 byte offsets, host byte order.
static bool build_seek_table(const std::string& interm_path,
                              const std::string& idx_path) {
    const char* args[] = {"ffprobe", "-v", "error", "-select_streams", "v:0",
                          "-show_entries", "packet=pos,flags", "-of", "json",
                          interm_path.c_str(), nullptr};
    int pfd[2];
    if (pipe(pfd) != 0) return false;
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, STDIN_FILENO); dup2(dn, STDERR_FILENO); close(dn); }
        execvp("ffprobe", const_cast<char**>(args));
        _exit(127);
    }
    close(pfd[1]);

    std::vector<uint64_t> offsets;
    offsets.reserve(8192);
    FILE* f = fdopen(pfd[0], "r");
    if (f) {
        char line[512];
        uint64_t cur_pos = 0;
        bool have_pos = false, cur_key = false;
        // ffprobe -of json emits one object per packet; -show_entries order is
        // pos then flags, and each "pos" line starts a new packet, so reset the
        // key state there. A packet with no "K_" flag simply never pushes.
        while (fgets(line, sizeof(line), f)) {
            unsigned long long p = 0;
            if (sscanf(line, " \"pos\": \"%llu\"", &p) == 1) {
                cur_pos = (uint64_t)p;
                have_pos = true;
                cur_key = false;   // new packet
            } else if (strstr(line, "\"flags\": \"") && strstr(line, "K_")) {
                cur_key = true;
            }
            if (have_pos && cur_key) {
                offsets.push_back(cur_pos);
                have_pos = false;
                cur_key = false;
            }
        }
        fclose(f);
    } else {
        close(pfd[0]);
    }
    waitpid(pid, nullptr, 0);

    if (offsets.empty()) return false;

    FILE* idx = fopen(idx_path.c_str(), "wb");
    if (!idx) return false;
    uint32_t count = (uint32_t)offsets.size();
    fwrite(&count, sizeof(count), 1, idx);
    fwrite(offsets.data(), sizeof(uint64_t), count, idx);
    fclose(idx);
    return true;
}

// ── Public API — path/ready/load (unchanged) ──────────────────────────────────

bool proxy_is_ready(const std::string& video_path) {
    // Alpha video must never use the yuv420p proxy — it would discard transparency
    // and preview would show black where export shows the BG. Fall through to
    // native decode which now preserves yuva→RGBA.
    if (file_has_alpha_cached(video_path)) {
        // Clean up any stale yuv420p proxy left from before the fix
        std::error_code ec;
        fs::remove(proxy_interm_path(video_path), ec);
        fs::remove(proxy_interm_idx_path(video_path), ec);
        // Also clear from ready cache so callers don't hit a stale positive
        {
            std::lock_guard<std::mutex> lk(g_ready_cache_mu);
            g_ready_cache.erase(video_path);
            g_last_stat_ts.erase(video_path);
        }
        return false;
    }
    double now = mono_now_seconds();
    {
        std::lock_guard<std::mutex> lk(g_ready_cache_mu);
        if (g_ready_cache.count(video_path)) {
            // Positive hits are normally terminal, but the proxy files can vanish
            // under us (the user clears the cache dir, a disk-full run truncates
            // them, an old session left a half-written pair). Re-verify existence
            // at a low rate (~0.5 Hz per path — negligible next to the 60 fps draw
            // loop) so a missing proxy SELF-HEALS into a regenerate instead of a
            // permanent blank that only a full app restart clears.
            auto it = g_last_stat_ts.find(video_path);
            if (it != g_last_stat_ts.end() && now - it->second < 2.0) return true;
            g_last_stat_ts[video_path] = now;
            bool still_there = fs::exists(proxy_interm_path(video_path)) &&
                               fs::exists(proxy_interm_idx_path(video_path));
            if (still_there) return true;
            g_ready_cache.erase(video_path);   // evicted → callers re-queue proxy_start
            return false;
        }
        auto it = g_last_stat_ts.find(video_path);
        if (it != g_last_stat_ts.end() && now - it->second < 0.25) return false;
        g_last_stat_ts[video_path] = now;
    }
    bool ready = fs::exists(proxy_interm_path(video_path)) &&
                 fs::exists(proxy_interm_idx_path(video_path));
    if (ready) mark_ready_cached(video_path);
    return ready;
}

bool proxy_load(const std::string& video_path, ProxyInfo& out) {
    out.interm_path     = proxy_interm_path(video_path);
    out.interm_idx_path = proxy_interm_idx_path(video_path);
    out.still_path      = proxy_still_path(video_path);

    FILE* idx = fopen(out.interm_idx_path.c_str(), "rb");
    if (!idx) return false;
    uint32_t count = 0;
    if (fread(&count, sizeof(count), 1, idx) != 1 || count == 0) {
        fclose(idx); return false;
    }
    out.offsets.resize(count);
    size_t got = fread(out.offsets.data(), sizeof(uint64_t), count, idx);
    fclose(idx);
    if (got != count) return false;

    out.frame_count = (int)count;

    // Probe sidecar: fps + dims from a previous proxy_load, keyed to the
    // proxy's frame count. Without it, EVERY slot open forked two synchronous
    // ffprobes (fps + dims on the intermediate) — ~250 ms of blocked main
    // thread per source, which is what made opening a media-heavy project
    // freeze for seconds.
    std::string cache_path = out.interm_idx_path + ".probe";
    {
        FILE* c = fopen(cache_path.c_str(), "r");
        if (c) {
            long long cc = 0, fn = 0, fd = 0;
            int cw = 0, ch = 0;
            if (fscanf(c, "%lld %lld %lld %d %d", &cc, &fn, &fd, &cw, &ch) == 5 &&
                cc == (long long)count && fd > 0 && cw > 0 && ch > 0) {
                out.fps_num = (int64_t)fn;
                out.fps_den = (int64_t)fd;
                out.fps     = (double)fn / (double)fd;
                out.width   = cw;
                out.height  = ch;
                fclose(c);
                return true;
            }
            fclose(c);
        }
    }

    // Determine the intermediate's frame rate. Unlike raw MJPEG, the
    // transcode carries real fps metadata: it's CFR at min(source, 30) via
    // "-r", so the interm's own r_frame_rate IS the proxy rate — and the
    // index offsets are into THIS file, so frames must be timed at THIS rate.
    // Fall back to frame_count / container duration if the stream rate is
    // missing (keeps the frame-to-time mapping exact).
    {
        const char* pargv[] = {"ffprobe", "-v", "error", "-select_streams", "v:0",
                               "-show_entries", "stream=r_frame_rate:format=duration",
                               "-of", "default=nw=1", out.interm_path.c_str(), nullptr};
        int pfd[2];
        if (pipe(pfd) == 0) {
            pid_t pid = fork();
            if (pid == 0) {
                close(pfd[0]);
                dup2(pfd[1], STDOUT_FILENO);
                close(pfd[1]);
                int dn = open("/dev/null", O_RDWR);
                if (dn >= 0) { dup2(dn, STDIN_FILENO); dup2(dn, STDERR_FILENO); close(dn); }
                execvp("ffprobe", const_cast<char**>(pargv));
                _exit(127);
            }
            close(pfd[1]);
            FILE* probe = fdopen(pfd[0], "r");
            double dur = 0.0;
            if (probe) {
                char line[256];
                long long fn = 0, fd = 1;
                while (fgets(line, sizeof(line), probe)) {
                    if (sscanf(line, "r_frame_rate=%lld/%lld", &fn, &fd) == 2 && fd > 0) {
                        out.fps_num = (int64_t)fn;
                        out.fps_den = (int64_t)fd;
                        out.fps     = (double)fn / (double)fd;
                    }
                    sscanf(line, "duration=%lf", &dur);
                }
                fclose(probe);
            } else { close(pfd[0]); }
            waitpid(pid, nullptr, 0);

            if (out.fps <= 0.0 && dur > 0.0 && out.frame_count > 0) {
                out.fps_num = (int64_t)out.frame_count * 1000;
                out.fps_den = (int64_t)std::llround(dur * 1000.0);
                if (out.fps_den <= 0) out.fps_den = 1;
                out.fps = (double)out.fps_num / (double)out.fps_den;
            }
        }
    }

    // Probe dimensions from the intermediate itself (actual full-res pixel size).
    {
        const char* dargv[] = {"ffprobe", "-v", "error", "-select_streams", "v:0",
                               "-show_entries", "stream=width,height",
                               "-of", "default=nw=1", out.interm_path.c_str(), nullptr};
        int dfd[2];
        if (pipe(dfd) == 0) {
            pid_t pid = fork();
            if (pid == 0) {
                close(dfd[0]);
                dup2(dfd[1], STDOUT_FILENO);
                close(dfd[1]);
                int dn = open("/dev/null", O_RDWR);
                if (dn >= 0) { dup2(dn, STDIN_FILENO); dup2(dn, STDERR_FILENO); close(dn); }
                execvp("ffprobe", const_cast<char**>(dargv));
                _exit(127);
            }
            close(dfd[1]);
            FILE* dim_probe = fdopen(dfd[0], "r");
            if (dim_probe) {
                char line[256];
                int w = 0, h = 0;
                while (fgets(line, sizeof(line), dim_probe)) {
                    if (sscanf(line, "width=%d",  &w) == 1) out.width  = w;
                    if (sscanf(line, "height=%d", &h) == 1) out.height = h;
                }
                fclose(dim_probe);
            } else { close(dfd[0]); }
            waitpid(pid, nullptr, 0);
        }
    }

    bool ok = out.width > 0 && out.height > 0 && out.fps > 0.0;
    if (ok) {
        // Persist the probe so the next open of this proxy skips both forks.
        FILE* c = fopen(cache_path.c_str(), "w");
        if (c) {
            fprintf(c, "%lld %lld %lld %d %d\n", (long long)out.frame_count,
                    (long long)out.fps_num, (long long)out.fps_den,
                    out.width, out.height);
            fclose(c);
        }
    }
    return ok;
}

// ── Image detection ───────────────────────────────────────────────────────────

static bool is_image_ext(const std::string& path) {
    auto ext = fs::path(path).extension().string();
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext==".jpg"||ext==".jpeg"||ext==".png"||ext==".bmp"||ext==".webp"||ext==".tiff"
        || ext==".heic"||ext==".heif"||ext==".svg";
}

static bool is_svg_ext(const std::string& path) {
    auto ext = fs::path(path).extension().string();
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext==".svg" || ext==".svgz";
}

// SVG is vector XML — neither ffmpeg nor stb_image can decode it, so rasterize it
// to a PNG still with librsvg (rsvg-convert). Fixed 2000px width, aspect + alpha
// kept: tiny icons upscale crisply and oversized art downsizes, independent of the
// SVG's declared size (vectors stay sharp at any scale). Writes dst_png directly.
static bool rasterize_svg(const std::string& src, const std::string& dst_png) {
    std::string cmd = "rsvg-convert --width=2000 --keep-aspect-ratio --format=png -o \""
                      + dst_png + "\" \"" + src + "\" 2>/dev/null";
    return pms_system(cmd.c_str()) == 0 && fs::exists(dst_png);  // NOLINT
}

// ── Still generation ──────────────────────────────────────────────────────────

void proxy_ensure_still(const std::string& video_path) {
    std::string still = proxy_still_path(video_path);
    if (fs::exists(still)) return;

    if (is_svg_ext(video_path)) {
        rasterize_svg(video_path, still);
    } else if (is_image_ext(video_path)) {
        std::string img_src = "file:" + video_path;
        const char* args[] = {
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-y", "-i", img_src.c_str(),
            "-vf", "scale=iw/2:-2,format=yuvj420p",
            still.c_str(), nullptr
        };
        pid_t p = spawn_ffmpeg(args);
        int st; waitpid(p, &st, 0);
    } else {
        std::string src = "file:" + video_path;
        const char* args[] = {
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-y", "-i", src.c_str(),
            "-ss", "0", "-vframes", "1",
            "-vf", "scale=iw/4:-2,format=yuvj420p",
            still.c_str(), nullptr
        };
        pid_t p = spawn_ffmpeg(args);
        int st; waitpid(p, &st, 0);
    }
}

// ── Frame count probe (for progress %) ───────────────────────────────────────
//
// Also exposes the target intermediate rate (min(source fps, 30)) as a double
// plus an exact rational — the worker feeds the rational to "-r" so a
// 24000/1001 source stays exactly 24000/1001 in the intermediate.

static int probe_total_frames(const std::string& video_path,
                              double* out_fps = nullptr,
                              int64_t* out_fps_num = nullptr,
                              int64_t* out_fps_den = nullptr) {
    std::string src = "file:" + video_path;
    const char* args[] = {"ffprobe", "-v", "error", "-select_streams", "v:0",
                          "-show_entries", "stream=r_frame_rate,duration",
                          "-of", "default=nw=1", src.c_str(), nullptr};
    int pfd[2];
    if (pipe(pfd) != 0) return 0;
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, STDIN_FILENO); dup2(dn, STDERR_FILENO); close(dn); }
        execvp("ffprobe", const_cast<char**>(args));
        _exit(127);
    }
    close(pfd[1]);
    FILE* f = fdopen(pfd[0], "r");
    double dur = 0.0; long long fn = 30, fd2 = 1;
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            sscanf(line, "duration=%lf", &dur);
            sscanf(line, "r_frame_rate=%lld/%lld", &fn, &fd2);
        }
        fclose(f);
    }
    waitpid(pid, nullptr, 0);
    if (dur <= 0.0 || fd2 <= 0) return 0;
    double src_fps = (double)fn / (double)fd2;
    double proxy_fps = std::min(src_fps, 30.0);  // proxy is capped at 30
    if (out_fps) *out_fps = proxy_fps;
    if (out_fps_num && out_fps_den) {
        if (src_fps > 30.0) { *out_fps_num = 30; *out_fps_den = 1; }   // clamped
        else                { *out_fps_num = (int64_t)fn; *out_fps_den = (int64_t)fd2; }
    }
    return (int)(dur * proxy_fps + 0.5);
}

// ── Intermediate encode (one attempt) ────────────────────────────────────────
//
// Runs ffmpeg to build the full-res all-intra H.264 intermediate, polling
// -progress for the % bar. Returns true on a clean exit. Split out so the
// worker can retry:
//   • use_hwaccel=true  → -hwaccel auto (GPU decode: vaapi/nvdec/qsv/…)
//   • use_hwaccel=false → pure software decode
// -hwaccel auto picks a backend that *fails outright* on some codecs/profiles/
// GPUs (VAAPI on AMD chokes on assorted H.264/HEVC profiles), and when it fails
// ffmpeg exits non-zero and the proxy silently never appears. Retrying in
// software is deterministic and always available — that's the fix for "some
// clips never get a proxy." -g 1 makes every frame an independent keyframe (no
// frame chains, so each frame's byte offset is a clean seek point); -r <fps>
// forces the constant rate (ffmpeg's default fps_mode is cfr) so VFR / NTSC
// sources (24000/1001, 2997003/125000, 90000/2999 …) can't emit a broken,
// unindexable intermediate.
static bool proxy_encode_once(const std::string& path, const std::string& src,
                              const std::string& interm, const std::string& prog_file,
                              const std::string& threads_str, const std::string& fps_arg,
                              int total_frames, bool use_hwaccel) {
    std::vector<const char*> a;
    a.push_back("ffmpeg"); a.push_back("-hide_banner");
    a.push_back("-loglevel"); a.push_back("error");
    if (use_hwaccel) { a.push_back("-hwaccel"); a.push_back("auto"); }
    a.push_back("-threads"); a.push_back(threads_str.c_str());
    a.push_back("-y"); a.push_back("-i"); a.push_back(src.c_str());
    a.push_back("-map"); a.push_back("0:v:0");
    a.push_back("-an");
    a.push_back("-c:v"); a.push_back("libx264");
    a.push_back("-g"); a.push_back("1");
    a.push_back("-crf"); a.push_back("16");
    a.push_back("-preset"); a.push_back("veryfast");
    a.push_back("-pix_fmt"); a.push_back("yuv420p");
    a.push_back("-r"); a.push_back(fps_arg.c_str());
    a.push_back("-progress"); a.push_back(prog_file.c_str());
    a.push_back(interm.c_str());
    a.push_back(nullptr);

    pid_t pp = spawn_ffmpeg(a.data());
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pid_map[path] = pp;
    }

    bool ok = false;
    while (true) {
        int wst = 0;
        pid_t r = waitpid(pp, &wst, WNOHANG);
        if (r == pp) {
            ok = WIFEXITED(wst) && WEXITSTATUS(wst) == 0;
            break;
        }
        if (r < 0) break;
        if (g_shutdown.load()) { kill(pp, SIGTERM); waitpid(pp, nullptr, 0); break; }

        if (total_frames > 0) {
            FILE* pf = fopen(prog_file.c_str(), "r");
            if (pf) {
                char line[128]; int last_frame = 0;
                while (fgets(line, sizeof(line), pf)) {
                    int fr; if (sscanf(line, "frame=%d", &fr) == 1) last_frame = fr;
                }
                fclose(pf);
                if (last_frame > 0) {
                    float p = std::min(0.98f, (float)last_frame / (float)total_frames);
                    std::lock_guard<std::mutex> lk(g_mu);
                    auto it = g_progress_map.find(path);
                    if (it != g_progress_map.end()) it->second = p;
                }
            }
        }
        usleep(100'000);  // 100 ms poll
    }
    return ok;
}

// ── Worker thread ─────────────────────────────────────────────────────────────

static void proxy_worker_fn() {
    const std::string threads_str = std::to_string(proxy_threads_per_job());

    while (true) {
        std::string path;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_queue.empty() || g_shutdown.load()) {
                if (--g_workers_alive < 0) g_workers_alive = 0;
                return;
            }
            path = g_queue.front();
            g_queue.pop_front();
            g_active.insert(path);
            g_progress_map[path] = 0.f;
        }

        auto release_active = [&]{
            std::lock_guard<std::mutex> lk(g_mu);
            g_active.erase(path);
            g_pid_map.erase(path);
            g_progress_map.erase(path);
        };

        if (proxy_is_ready(path)) { release_active(); continue; }
        if (g_shutdown.load())    { release_active(); break;    }

        // Sweep stale incomplete intermediate (interm exists but no idx)
        std::string interm = proxy_interm_path(path);
        std::string idx    = proxy_interm_idx_path(path);
        if (fs::exists(interm) && !fs::exists(idx)) fs::remove(interm);

        // Probe total frame count for accurate progress percentage AND to fail
        // fast on unreadable sources: ffprobe prints nothing for a corrupt or
        // truncated file (no moov atom, etc.), so a 0 result means "cannot
        // decode" — spawn neither the still extractor nor the encoder, and
        // record a persistent failure instead of churning the queue forever
        // (the per-frame re-queue loop was re-launching ffmpeg against dead
        // files endlessly, flashing "building preview…"). The probe also
        // yields the target rate (min(source, 30)) as an exact rational for
        // "-r".
        double proxy_fps = 0.0;
        int64_t fps_num = 0, fps_den = 1;
        int total_frames = probe_total_frames(path, &proxy_fps, &fps_num, &fps_den);
        if (total_frames <= 0 || proxy_fps <= 0.0) {
            mark_proxy_failed(path, "source file corrupt or truncated (unreadable) — replace or re-export it");
            release_active();
            continue;
        }

        // Generate still so the preview panel shows something immediately
        proxy_ensure_still(path);

        // Encode the intermediate, GPU-decode first. -threads K caps
        // per-process thread count so N workers × K threads stays near
        // hardware_concurrency. Full-res all-intra H.264 (-g 1 → every frame
        // is a keyframe, so each byte offset in the seek table is a clean
        // decode point) at min(source fps, 30) CFR, rotation baked, no audio.
        // If hardware decode fails (bad VAAPI/NVDEC support for this codec)
        // retry ONCE in pure software — deterministic and always available, so
        // a clip can't be left permanently proxy-less.
        std::string fps_arg = fps_den == 1 ? std::to_string(fps_num)
                                           : std::to_string(fps_num) + "/" + std::to_string(fps_den);
        std::string prog_file = interm + ".prog";
        std::string src = "file:" + path;

        bool ok = proxy_encode_once(path, src, interm, prog_file, threads_str,
                                    fps_arg, total_frames, /*use_hwaccel=*/true);
        if (!ok && !g_shutdown.load()) {
            fs::remove(interm);
            fprintf(stderr, "[proxy] hw decode failed, retrying software: %s\n", path.c_str());
            ok = proxy_encode_once(path, src, interm, prog_file, threads_str,
                                   fps_arg, total_frames, /*use_hwaccel=*/false);
        }

        fs::remove(prog_file);

        if (!ok) {
            if (!g_shutdown.load())
                fprintf(stderr, "[proxy] generation FAILED (both hw+sw): %s\n", path.c_str());
            fs::remove(interm);
            mark_proxy_failed(path, "transcode failed (hardware and software decode both errored)");
        } else if (!build_seek_table(interm, idx)) {
            fprintf(stderr, "[proxy] seek-table build failed: %s\n", path.c_str());
            fs::remove(interm);
            mark_proxy_failed(path, "seek-table build failed");
        } else {
            fs::remove(proxy_fail_path(path));   // clear any earlier failure
            mark_ready_cached(path);
        }

        release_active();

        if (g_shutdown.load()) break;
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

bool proxy_is_generating() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_workers_alive > 0;
}

void proxy_cancel() {
    std::vector<pid_t> pids;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_queue.clear();
        for (auto& [_p, pid] : g_pid_map) pids.push_back(pid);
        // Don't wipe g_active / g_pid_map here — workers themselves erase their
        // own entries when their ffmpeg exits. Killing the pid (below) makes
        // waitpid return so the worker exits its inner poll loop promptly.
    }
    for (pid_t pp : pids)
        if (pp > 0) { kill(pp, SIGTERM); waitpid(pp, nullptr, 0); }
}

static bool file_has_alpha(const std::string& path) {
    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(ctx, nullptr) < 0) { avformat_close_input(&ctx); return false; }
    bool has = false;
    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
        if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            AVPixelFormat fmt = (AVPixelFormat)ctx->streams[i]->codecpar->format;
            if (fmt != AV_PIX_FMT_NONE) {
                const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
                if (desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA)) has = true;
            } else {
                // Fallback: ffprobe pix_fmt string may be more reliable for some containers
                // where codecpar format is NONE; try average pix_fmt via codecpar->profile? Skip
            }
            break;
        }
    }
    avformat_close_input(&ctx);
    return has;
}

static std::unordered_map<std::string, bool> g_alpha_cache;
static std::mutex g_alpha_cache_mu;
static bool file_has_alpha_cached(const std::string& path) {
    {
        std::lock_guard<std::mutex> lk(g_alpha_cache_mu);
        auto it = g_alpha_cache.find(path);
        if (it != g_alpha_cache.end()) return it->second;
    }
    bool v = file_has_alpha(path);
    {
        std::lock_guard<std::mutex> lk(g_alpha_cache_mu);
        g_alpha_cache[path] = v;
    }
    return v;
}

void proxy_start(const std::string& video_path) {
    // Synthetic timeline clips and media deleted outside the app have no
    // decodable source. Do not launch ffmpeg workers for them; callers may
    // still submit their own layer frames (as the Metal renderer tests do).
    std::error_code source_error;
    if (video_path.empty() || !fs::is_regular_file(video_path, source_error)) return;
    // Alpha video (ProRes 4444, VP9 yuva, PNG video) must stay native — the
    // intermediate is all-intra H.264 yuv420p which cannot store alpha, so a
    // proxied copy would permanently lose transparency.
    if (file_has_alpha_cached(video_path)) return;
    if (is_image_ext(video_path)) {
        std::string still = proxy_still_path(video_path);
        if (fs::exists(still)) return;
        if (is_svg_ext(video_path)) {   // vector → librsvg rasterize, off-thread
            std::thread([video_path, still]() { rasterize_svg(video_path, still); }).detach();
            return;
        }
        std::string img_src = "file:" + video_path;
        // Convert to a FULL-RES PNG so alpha (e.g. a transparent WebP) survives
        // and the image isn't softened — it's already a finished image, not a
        // video that needs a lightweight proxy.
        std::thread([video_path, img_src, still]() {
            const char* args[] = {
                "ffmpeg", "-hide_banner", "-loglevel", "error",
                "-y", "-i", img_src.c_str(), "-frames:v", "1",
                still.c_str(), nullptr
            };
            pid_t p = spawn_ffmpeg(args);
            if (p > 0) { int st; waitpid(p, &st, 0); }

            if (!fs::exists(still)) {
                std::string tmp = still + ".tmp.png";
                std::string cmd = "heif-convert \"" + video_path + "\" \"" + tmp + "\" 2>/dev/null";
                if (pms_system(cmd.c_str()) == 0 && fs::exists(tmp)) { // NOLINT
                    const char* a2[] = {"ffmpeg","-hide_banner","-loglevel","error",
                                        "-y","-i",tmp.c_str(),"-frames:v","1",
                                        still.c_str(), nullptr};
                    pid_t p2 = spawn_ffmpeg(a2);
                    if (p2 > 0) { int st; waitpid(p2, &st, 0); }
                    fs::remove(tmp);
                }
            }
        }).detach();
        return;
    }

    if (proxy_is_ready(video_path)) return;

    // A source whose generation failed (corrupt/truncated file) stays skipped
    // until the file itself changes — proxy_failure() returns empty once the
    // source is overwritten, allowing a fresh attempt.
    if (!proxy_failure(video_path).empty()) return;

    int workers_to_spawn = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_active.count(video_path)) return;     // already generating
        for (auto& q : g_queue)
            if (q == video_path) return;            // already queued
        g_queue.push_back(video_path);

        // Spin up parallel workers up to PROXY_MAX_WORKERS, capped by how much
        // work is actually pending (queue + in-flight). New workers exit on
        // their own when the queue drains, so this is steady-state correct.
        int max_w = proxy_max_workers();
        int pending = (int)g_queue.size() + (int)g_active.size();
        int target = std::min(max_w, pending);
        if (g_workers_alive < target) {
            workers_to_spawn = target - g_workers_alive;
            g_workers_alive += workers_to_spawn;
        }
    }

    for (int i = 0; i < workers_to_spawn; ++i)
        std::thread(proxy_worker_fn).detach();
}

// ── Status API ────────────────────────────────────────────────────────────────

ProxyJobStatus proxy_job_status(const std::string& video_path) {
    ProxyJobStatus st;
    st.path = video_path;
    if (proxy_is_ready(video_path)) { st.state = ProxyJobStatus::State::Ready; return st; }
    std::string fail = proxy_failure(video_path);
    if (!fail.empty()) {
        st.state = ProxyJobStatus::State::Failed;
        st.error = fail;
        return st;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_active.count(video_path)) {
        st.state = ProxyJobStatus::State::Generating;
        auto it = g_progress_map.find(video_path);
        if (it != g_progress_map.end()) st.progress = it->second;
        return st;
    }
    for (auto& q : g_queue) {
        if (q == video_path) { st.state = ProxyJobStatus::State::Queued; return st; }
    }
    st.state = ProxyJobStatus::State::Idle;
    return st;
}

std::vector<ProxyJobStatus> proxy_status_all(const std::vector<std::string>& paths) {
    std::vector<ProxyJobStatus> out;
    out.reserve(paths.size());
    for (auto& p : paths) out.push_back(proxy_job_status(p));
    return out;
}

int proxy_queue_depth() {
    std::lock_guard<std::mutex> lk(g_mu);
    return (int)g_queue.size();
}
