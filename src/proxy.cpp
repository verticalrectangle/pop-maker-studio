#include "proxy.h"
#include "paths.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
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

// ── Internal state ────────────────────────────────────────────────────────────

static std::atomic<bool> g_generating{false};
static std::atomic<pid_t> g_proxy_pid{-1};
static std::atomic<pid_t> g_still_pid{-1};

// ── Subprocess helpers ────────────────────────────────────────────────────────

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

static bool wait_ok(pid_t pid) {
    if (pid <= 0) return false;
    while (true) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return WIFEXITED(st) && WEXITSTATUS(st) == 0;
        if (r < 0)    return false;
        if (g_shutdown.load()) { kill(pid, SIGTERM); waitpid(pid, nullptr, 0); return false; }
        usleep(50'000); // 50 ms poll
    }
}

// ── Seek-table builder ────────────────────────────────────────────────────────
// Scans the MJPEG file for JPEG SOI markers (FF D8 FF) and records each frame's
// byte offset.  In a valid MJPEG stream every FF in entropy-coded data is
// followed by 00 (byte stuffing), so FF D8 FF always means a new frame start.

static bool build_seek_table(const std::string& mjpeg_path,
                              const std::string& idx_path) {
    FILE* f = fopen(mjpeg_path.c_str(), "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);

    std::vector<uint64_t> offsets;
    offsets.reserve(8192);

    static const size_t BUF = 1 << 20;  // 1 MB read chunks
    std::vector<uint8_t> buf(BUF + 2);

    // Carry-over: last 2 bytes of previous chunk to detect markers spanning chunks
    uint8_t carry[2] = {0, 0};
    long pos = 0;

    while (pos < file_size) {
        size_t to_read = BUF;
        if (pos + (long)to_read > file_size)
            to_read = (size_t)(file_size - pos);

        // Place carry bytes at the front
        buf[0] = carry[0];
        buf[1] = carry[1];
        size_t got = fread(buf.data() + 2, 1, to_read, f);
        if (got == 0) break;

        // Scan for FF D8 FF (JPEG SOI + first marker prefix)
        for (size_t i = 0; i + 2 < got + 2; ++i) {
            if (buf[i] == 0xFF && buf[i+1] == 0xD8 && buf[i+2] == 0xFF) {
                long abs_off = pos + (long)i - 2;  // -2 for the carry bytes
                if (abs_off >= 0)
                    offsets.push_back((uint64_t)abs_off);
            }
        }

        carry[0] = buf[got];      // last two bytes of this chunk
        carry[1] = buf[got + 1];
        pos += (long)got;
    }
    fclose(f);

    if (offsets.empty()) return false;

    // Write binary idx: [uint32 count][uint64 offset] * count
    FILE* idx = fopen(idx_path.c_str(), "wb");
    if (!idx) return false;
    uint32_t count = (uint32_t)offsets.size();
    fwrite(&count, sizeof(count), 1, idx);
    fwrite(offsets.data(), sizeof(uint64_t), count, idx);
    fclose(idx);
    return true;
}

// ── Public API ────────────────────────────────────────────────────────────────

bool proxy_is_generating() { return g_generating.load(); }

bool proxy_is_ready(const std::string& video_path) {
    return fs::exists(proxy_mjpeg_path(video_path)) &&
           fs::exists(proxy_idx_path(video_path));
}

bool proxy_load(const std::string& video_path, ProxyInfo& out) {
    out.mjpeg_path = proxy_mjpeg_path(video_path);
    out.idx_path   = proxy_idx_path(video_path);
    out.still_path = proxy_still_path(video_path);

    // Load seek table
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

    // Probe fps from the ORIGINAL video — raw MJPEG streams have no reliable
    // fps metadata so ffprobe on the proxy frequently returns a wrong default.
    // Width/height come from the proxy itself (it's half-res).
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

    // Probe dimensions from the proxy (actual half-res pixel size).
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

void proxy_cancel() {
    g_generating.store(false);
    pid_t pp = g_proxy_pid.exchange(-1);
    pid_t sp = g_still_pid.exchange(-1);
    if (pp > 0) kill(pp, SIGTERM);
    if (sp > 0) kill(sp, SIGTERM);
    if (pp > 0) waitpid(pp, nullptr, 0);
    if (sp > 0) waitpid(sp, nullptr, 0);
}

// Returns true for still-image extensions that never need an MJPEG proxy.
static bool is_image_ext(const std::string& path) {
    auto ext = fs::path(path).extension().string();
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext==".jpg"||ext==".jpeg"||ext==".png"||ext==".bmp"||ext==".webp"||ext==".tiff"
        || ext==".heic"||ext==".heif";
}

void proxy_start(const std::string& video_path) {
    // Images: generate a scaled still and return — no MJPEG proxy needed.
    // A single-frame MJPEG would cause proxy_load to spawn ffprobe every frame
    // (fps comes back 0 → load fails → loop retries indefinitely).
    if (is_image_ext(video_path)) {
        std::string still = proxy_still_path(video_path);
        if (!fs::exists(still)) {
            std::string img_src = "file:" + video_path;
            const char* args[] = {
                "ffmpeg", "-hide_banner", "-loglevel", "error",
                "-y", "-i", img_src.c_str(),
                "-vf", "scale=iw/2:ih/2",
                still.c_str(), nullptr
            };
            pid_t sp = spawn_ffmpeg(args);
            wait_ok(sp);

            // Fallback for formats ffmpeg can't decode (e.g. HEIC without libheif):
            // try heif-convert → produce a JPEG → re-run ffmpeg to scale it.
            if (!fs::exists(still)) {
                std::string tmp = still + ".tmp.jpg";
                std::string cmd = "heif-convert " + std::string("\"") + video_path + "\" \"" + tmp + "\" 2>/dev/null";
                if (system(cmd.c_str()) == 0 && fs::exists(tmp)) { // NOLINT
                    const char* scale_args[] = {
                        "ffmpeg", "-hide_banner", "-loglevel", "error",
                        "-y", "-i", tmp.c_str(),
                        "-vf", "scale=iw/2:ih/2",
                        still.c_str(), nullptr
                    };
                    pid_t sp2 = spawn_ffmpeg(scale_args);
                    wait_ok(sp2);
                    fs::remove(tmp);
                }
            }
        }
        return;
    }

    if (proxy_is_ready(video_path)) return;

    // Sweep up any zombie mjpeg left by a previous interrupted generation
    // (mjpeg exists but idx is missing). Only safe when not currently generating.
    if (!g_generating.load()) {
        std::string mj = proxy_mjpeg_path(video_path);
        if (fs::exists(mj) && !fs::exists(proxy_idx_path(video_path)))
            fs::remove(mj);
    }

    if (g_generating.load()) proxy_cancel();

    g_generating.store(true);

    // Extract preview still synchronously — blocks for < 1 s.
    // This gives the preview panel something to show immediately.
    std::string still = proxy_still_path(video_path);
    if (!fs::exists(still)) {
        std::string still_src = "file:" + video_path;
        const char* still_args[] = {
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-y", "-i", still_src.c_str(),
            "-ss", "0", "-vframes", "1",
            "-vf", "scale=iw/4:ih/4",
            still.c_str(), nullptr
        };
        pid_t sp = spawn_ffmpeg(still_args);
        g_still_pid.store(sp);
        wait_ok(sp);
        g_still_pid.store(-1);
    }

    // Proxy generation runs in a background thread.
    std::thread([video_path]() {
        std::string mjpeg = proxy_mjpeg_path(video_path);
        std::string idx   = proxy_idx_path(video_path);

        // Quarter-resolution MJPEG, no audio.
        // -q:v 5 gives good quality/size balance for MJPEG (1=best, 31=worst).
        // We do NOT specify a frame rate — the proxy matches the original fps.
        std::string proxy_src = "file:" + video_path;
        const char* proxy_args[] = {
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-y", "-i", proxy_src.c_str(),
            "-vf", "scale=min(iw/2\\,960):-2",
            "-r", "30",   // cap proxy at 30 fps — MJPEG muxer can't handle >30 fps raw streams
            "-c:v", "mjpeg", "-q:v", "13",
            "-an",
            mjpeg.c_str(), nullptr
        };

        pid_t pp = spawn_ffmpeg(proxy_args);
        g_proxy_pid.store(pp);

        if (!wait_ok(pp)) {
            g_proxy_pid.store(-1);
            g_generating.store(false);
            fs::remove(mjpeg);  // don't leave a partial mjpeg without an idx
            return;
        }
        g_proxy_pid.store(-1);

        // Build the seek table from the written MJPEG.
        if (!build_seek_table(mjpeg, idx)) {
            // Remove corrupt files so proxy_is_ready() stays false.
            fs::remove(mjpeg);
        }

        g_generating.store(false);
    }).detach();
}
