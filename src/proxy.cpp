#include "proxy.h"
#include "paths.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <filesystem>

#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>

namespace fs = std::filesystem;

// ── Path helpers ──────────────────────────────────────────────────────────────

std::string proxy_mjpeg_path(const std::string& vp) { return vp + ".pms_proxy.mjpeg"; }
std::string proxy_idx_path  (const std::string& vp) { return vp + ".pms_proxy.idx";   }
std::string proxy_still_path(const std::string& vp) { return vp + ".pms_still.jpg";   }

// ── Queue state ───────────────────────────────────────────────────────────────

static std::mutex          g_mu;
static std::deque<std::string> g_queue;     // paths waiting (not yet started)
static std::string         g_active;        // path currently being generated
static std::atomic<float>  g_progress{0.f}; // 0–1 for the active job
static std::atomic<pid_t>  g_pid{-1};       // ffmpeg PID for the active job
static std::atomic<bool>   g_worker_alive{false};

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
    return fs::exists(proxy_mjpeg_path(video_path)) &&
           fs::exists(proxy_idx_path(video_path));
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

    // Probe fps from the ORIGINAL video — raw MJPEG has no reliable fps metadata.
    {
        std::string file_arg = "file:" + video_path;
        const char* pargv[] = {"ffprobe", "-v", "error", "-select_streams", "v:0",
                               "-show_entries", "stream=r_frame_rate",
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
            if (probe) {
                char line[256];
                long long fn = 0, fd = 1;
                while (fgets(line, sizeof(line), probe)) {
                    if (sscanf(line, "r_frame_rate=%lld/%lld", &fn, &fd) == 2 && fd > 0) {
                        out.fps_num = (int64_t)fn;
                        out.fps_den = (int64_t)fd;
                        out.fps     = (double)fn / (double)fd;
                    }
                }
                fclose(probe);
            } else { close(pfd[0]); }
            waitpid(pid, nullptr, 0);
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
        || ext==".heic"||ext==".heif";
}

// ── Still generation ──────────────────────────────────────────────────────────

void proxy_ensure_still(const std::string& video_path) {
    std::string still = proxy_still_path(video_path);
    if (fs::exists(still)) return;

    if (is_image_ext(video_path)) {
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
    while (true) {
        std::string path;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_queue.empty()) {
                g_active.clear();
                g_worker_alive.store(false);
                return;
            }
            path = g_queue.front();
            g_queue.pop_front();
            g_active = path;
            g_progress.store(0.f);
        }

        if (proxy_is_ready(path)) continue;
        if (g_shutdown.load()) break;

        // Sweep stale incomplete mjpeg (mjpeg exists but no idx)
        std::string mj  = proxy_mjpeg_path(path);
        std::string idx = proxy_idx_path(path);
        if (fs::exists(mj) && !fs::exists(idx)) fs::remove(mj);

        // Generate still so the preview panel shows something immediately
        proxy_ensure_still(path);

        // Probe total frame count for accurate progress percentage
        int total_frames = probe_total_frames(path);

        // Use ffmpeg -progress to write key=value updates to a temp file
        std::string prog_file = mj + ".prog";
        std::string src = "file:" + path;
        const char* proxy_args[] = {
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-y", "-i", src.c_str(),
            "-vf", "scale=min(iw/2\\,960):-2",
            "-r", "30",
            "-c:v", "mjpeg", "-q:v", "13",
            "-an",
            "-progress", prog_file.c_str(),
            mj.c_str(), nullptr
        };

        pid_t pp = spawn_ffmpeg(proxy_args);
        g_pid.store(pp);

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
                    if (last_frame > 0)
                        g_progress.store(std::min(0.98f, (float)last_frame / (float)total_frames));
                }
            }
            usleep(100'000);  // 100 ms poll
        }

        g_pid.store(-1);
        fs::remove(prog_file);

        if (!ok) {
            fs::remove(mj);
        } else if (!build_seek_table(mj, idx)) {
            fs::remove(mj);
        } else {
            g_progress.store(1.f);
        }

        if (g_shutdown.load()) break;
    }

    std::lock_guard<std::mutex> lk(g_mu);
    g_active.clear();
    g_worker_alive.store(false);
}

// ── Public API ────────────────────────────────────────────────────────────────

bool proxy_is_generating() { return g_worker_alive.load(); }

void proxy_cancel() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_queue.clear();
        g_active.clear();
    }
    pid_t pp = g_pid.exchange(-1);
    if (pp > 0) { kill(pp, SIGTERM); waitpid(pp, nullptr, 0); }
    g_worker_alive.store(false);
}

void proxy_start(const std::string& video_path) {
    if (is_image_ext(video_path)) {
        // Images: just generate a still, no MJPEG needed
        std::string still = proxy_still_path(video_path);
        if (fs::exists(still)) return;
        std::string img_src = "file:" + video_path;
        // Run in background thread so we don't block the render loop
        std::thread([video_path, img_src, still]() {
            const char* args[] = {
                "ffmpeg", "-hide_banner", "-loglevel", "error",
                "-y", "-i", img_src.c_str(), "-vf", "scale=iw/2:ih/2",
                still.c_str(), nullptr
            };
            pid_t p = spawn_ffmpeg(args);
            if (p > 0) { int st; waitpid(p, &st, 0); }

            if (!fs::exists(still)) {
                std::string tmp = still + ".tmp.jpg";
                std::string cmd = "heif-convert \"" + video_path + "\" \"" + tmp + "\" 2>/dev/null";
                if (system(cmd.c_str()) == 0 && fs::exists(tmp)) { // NOLINT
                    const char* a2[] = {"ffmpeg","-hide_banner","-loglevel","error",
                                        "-y","-i",tmp.c_str(),"-vf","scale=iw/2:ih/2",
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

    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_active == video_path) return;   // already generating this one
        for (auto& q : g_queue)
            if (q == video_path) return;      // already queued
        g_queue.push_back(video_path);
    }

    // Kick off the worker if it isn't already running
    bool was_alive = g_worker_alive.exchange(true);
    if (!was_alive)
        std::thread(proxy_worker_fn).detach();
}

// ── Status API ────────────────────────────────────────────────────────────────

ProxyJobStatus proxy_job_status(const std::string& video_path) {
    ProxyJobStatus st;
    st.path = video_path;
    if (proxy_is_ready(video_path)) { st.state = ProxyJobStatus::State::Ready; return st; }
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_active == video_path) {
        st.state    = ProxyJobStatus::State::Generating;
        st.progress = g_progress.load();
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
