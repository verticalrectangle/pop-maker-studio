#include "proxy.h"
#include "paths.h"

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

std::string proxy_mjpeg_path(const std::string& vp) { return cache_path(vp, ".proxy.mjpeg"); }
std::string proxy_idx_path  (const std::string& vp) { return cache_path(vp, ".proxy.idx");   }
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

static bool build_seek_table(const std::string& mjpeg_path,
                              const std::string& idx_path) {
    FILE* f = fopen(mjpeg_path.c_str(), "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);

    std::vector<uint64_t> offsets;
    offsets.reserve(8192);

    static const size_t BUF = 1 << 20;
    std::vector<uint8_t> buf(BUF + 2);

    uint8_t carry[2] = {0, 0};
    long pos = 0;

    while (pos < file_size) {
        size_t to_read = BUF;
        if (pos + (long)to_read > file_size)
            to_read = (size_t)(file_size - pos);

        buf[0] = carry[0];
        buf[1] = carry[1];
        size_t got = fread(buf.data() + 2, 1, to_read, f);
        if (got == 0) break;

        for (size_t i = 0; i + 2 < got + 2; ++i) {
            if (buf[i] == 0xFF && buf[i+1] == 0xD8 && buf[i+2] == 0xFF) {
                long abs_off = pos + (long)i - 2;
                if (abs_off >= 0)
                    offsets.push_back((uint64_t)abs_off);
            }
        }

        carry[0] = buf[got];
        carry[1] = buf[got + 1];
        pos += (long)got;
    }
    fclose(f);

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
    {
        std::lock_guard<std::mutex> lk(g_ready_cache_mu);
        if (g_ready_cache.count(video_path)) return true;
        double now = mono_now_seconds();
        auto it = g_last_stat_ts.find(video_path);
        if (it != g_last_stat_ts.end() && now - it->second < 0.25) return false;
        g_last_stat_ts[video_path] = now;
    }
    bool ready = fs::exists(proxy_mjpeg_path(video_path)) &&
                 fs::exists(proxy_idx_path(video_path));
    if (ready) mark_ready_cached(video_path);
    return ready;
}

bool proxy_load(const std::string& video_path, ProxyInfo& out) {
    out.mjpeg_path = proxy_mjpeg_path(video_path);
    out.idx_path   = proxy_idx_path(video_path);
    out.still_path = proxy_still_path(video_path);

    FILE* idx = fopen(out.idx_path.c_str(), "rb");
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

    // Determine the proxy's frame rate. Raw MJPEG has no reliable fps
    // metadata, and the original's r_frame_rate is NOT the proxy's rate
    // either: the transcode forces "-r 30" (CFR), so a 60 fps screencast
    // yields a 30 fps proxy — indexing that with the original's rate shows
    // frames from 2× too deep into the source and diverges from the export.
    // The proxy's true rate is frame_count / source duration; probe the
    // ORIGINAL's container duration and derive it, keeping the original's
    // r_frame_rate only as a fallback when the duration probe fails.
    {
        std::string file_arg = "file:" + video_path;
        const char* pargv[] = {"ffprobe", "-v", "error", "-select_streams", "v:0",
                               "-show_entries", "stream=r_frame_rate:format=duration",
                               "-of", "default=nw=1", file_arg.c_str(), nullptr};
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
            double src_dur = 0.0;
            if (probe) {
                char line[256];
                long long fn = 0, fd = 1;
                while (fgets(line, sizeof(line), probe)) {
                    if (sscanf(line, "r_frame_rate=%lld/%lld", &fn, &fd) == 2 && fd > 0) {
                        out.fps_num = (int64_t)fn;
                        out.fps_den = (int64_t)fd;
                        out.fps     = (double)fn / (double)fd;
                    }
                    sscanf(line, "duration=%lf", &src_dur);
                }
                fclose(probe);
            } else { close(pfd[0]); }
            waitpid(pid, nullptr, 0);

            if (src_dur > 0.0 && out.frame_count > 0) {
                out.fps_num = (int64_t)out.frame_count * 1000;
                out.fps_den = (int64_t)std::llround(src_dur * 1000.0);
                if (out.fps_den <= 0) out.fps_den = 1;
                out.fps = (double)out.fps_num / (double)out.fps_den;
            }
        }
    }

    // Probe dimensions from the proxy itself (actual half-res pixel size).
    {
        const char* dargv[] = {"ffprobe", "-v", "error", "-select_streams", "v:0",
                               "-show_entries", "stream=width,height",
                               "-of", "default=nw=1", out.mjpeg_path.c_str(), nullptr};
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

    return out.width > 0 && out.height > 0 && out.fps > 0.0;
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
    return system(cmd.c_str()) == 0 && fs::exists(dst_png);  // NOLINT
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

static int probe_total_frames(const std::string& video_path) {
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
    return (int)(dur * proxy_fps + 0.5);
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

        // Sweep stale incomplete mjpeg (mjpeg exists but no idx)
        std::string mj  = proxy_mjpeg_path(path);
        std::string idx = proxy_idx_path(path);
        if (fs::exists(mj) && !fs::exists(idx)) fs::remove(mj);

        // Generate still so the preview panel shows something immediately
        proxy_ensure_still(path);

        // Probe total frame count for accurate progress percentage
        int total_frames = probe_total_frames(path);

        // Use ffmpeg -progress to write key=value updates to a temp file.
        // -hwaccel auto uses GPU decode when available (vaapi/nvdec/qsv/etc.),
        // silently falls back to software when not. -threads K caps per-process
        // thread count so N workers × K threads stays near hardware_concurrency.
        // fast_bilinear scaler trades a little chroma fidelity for ~20% faster
        // scaling — fine for preview-quality output. -q:v 16 is a touch coarser
        // than the prior 13 with no visible difference at half-res, shaves bytes
        // and encode time.
        std::string prog_file = mj + ".prog";
        std::string src = "file:" + path;
        const char* proxy_args[] = {
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-hwaccel", "auto",
            "-threads", threads_str.c_str(),
            "-y", "-i", src.c_str(),
            "-vf", "scale=min(iw/2\\,960):-2:flags=fast_bilinear",
            "-r", "30",
            "-c:v", "mjpeg", "-q:v", "16",
            "-an",
            "-progress", prog_file.c_str(),
            mj.c_str(), nullptr
        };

        pid_t pp = spawn_ffmpeg(proxy_args);
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_pid_map[path] = pp;
        }

        // Poll progress file while ffmpeg runs
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

        fs::remove(prog_file);

        if (!ok) {
            fs::remove(mj);
        } else if (!build_seek_table(mj, idx)) {
            fs::remove(mj);
        } else {
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

void proxy_start(const std::string& video_path) {
    if (is_image_ext(video_path)) {
        // Images: just generate a still, no MJPEG needed
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
                if (system(cmd.c_str()) == 0 && fs::exists(tmp)) { // NOLINT
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
