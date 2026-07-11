#include "conform.h"
#include "video.h"
#include "paths.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>

namespace fs = std::filesystem;

// ── ffmpeg launcher (own copy; proxy.cpp's is static) ─────────────────────────
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
        (void)nice(10);                       // keep the UI responsive
        execvp("ffmpeg", const_cast<char**>(args));
        _exit(127);
    }
    return pid;
}

// ── Worker state ──────────────────────────────────────────────────────────────
struct Job { std::string src, out; int fps; bool smooth, loop; };

static std::mutex                       g_mu;
static std::condition_variable          g_cv;
static std::deque<Job>                  g_queue;
static std::unordered_set<std::string>  g_queued;     // out paths pending
static std::string                      g_active_out;  // out path in flight ("" = none)
static pid_t                            g_active_pid = -1;
static std::atomic<bool>                g_cf_shutdown{false};
static bool                             g_worker_started = false;
static std::unordered_set<std::string>  g_ready_cache;  // out paths known complete

// ── Path helpers ──────────────────────────────────────────────────────────────
std::string conform_path(const std::string& src, int project_fps, bool smooth, bool loop) {
    char suffix[48];
    snprintf(suffix, sizeof(suffix), ".conform_%d_s%d_l%d.mp4",
             project_fps, smooth ? 1 : 0, loop ? 1 : 0);
    return cache_path(src, suffix);
}

// ── Native-fps probe ──────────────────────────────────────────────────────────
bool conform_needed(const std::string& src, int project_fps) {
    if (src.empty() || project_fps <= 0) return false;
    MediaFileInfo mi = video_probe_file(src);
    double nf = mi.fps;
    if (!(nf > 0.0)) return false;                 // still / unreadable → no conform
    // More than ~1% off the project grid is worth conforming.
    return std::fabs(nf - (double)project_fps) > (double)project_fps * 0.01;
}

// ── Worker ────────────────────────────────────────────────────────────────────
static void run_job(const Job& j) {
    std::string tmp = j.out + ".tmp.mp4";   // atomic: rename only on success
    fs::remove(tmp);

    // Filter chain. `framerate` temporally BLENDS to the target rate; `fps`
    // dup/drops (keeps original cadence). A loop is conformed cyclically: loop
    // the source 3×, blend, and keep the fully-surrounded middle copy so the
    // seam blends correctly and the result is one seamless, frame-aligned loop.
    const char* rate = j.smooth ? "framerate=fps=" : "fps=";
    std::string vf = std::string(rate) + std::to_string(j.fps);
    if (j.loop) {
        float dur = video_probe_duration(j.src);
        int nf = (int)std::lround((double)dur * (double)j.fps);
        if (nf < 1) nf = 1;
        vf += ",trim=start_frame=" + std::to_string(nf) +
              ":end_frame=" + std::to_string(2 * nf) + ",setpts=PTS-STARTPTS";
    }

    std::string srcarg = "file:" + j.src;
    std::string loops = "2";

    // One transcode attempt. GPU decode first; on failure the caller retries in
    // pure software. -hwaccel auto selects a backend (VAAPI/NVDEC/…) that fails
    // outright on some codecs/profiles/GPUs, and a failed conform means the clip
    // never gets its project-fps source — retrying in software is deterministic
    // and always works.
    auto attempt = [&](bool use_hwaccel) -> bool {
        std::vector<const char*> args = {
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        };
        if (use_hwaccel) { args.push_back("-hwaccel"); args.push_back("auto"); }
        if (j.loop) { args.push_back("-stream_loop"); args.push_back(loops.c_str()); }
        args.push_back("-i");      args.push_back(srcarg.c_str());
        args.push_back("-vf");     args.push_back(vf.c_str());
        args.push_back("-c:v");    args.push_back("libx264");
        args.push_back("-crf");    args.push_back("18");
        args.push_back("-preset"); args.push_back("veryfast");
        args.push_back("-pix_fmt");args.push_back("yuv420p");
        // Keep audio for plain conforms (same duration); a looped/trimmed conform
        // drops it (loops are GIFs / silent in practice).
        if (j.loop) { args.push_back("-an"); }
        else        { args.push_back("-c:a"); args.push_back("copy"); }
        args.push_back(tmp.c_str());
        args.push_back(nullptr);

        pid_t pp = spawn_ffmpeg(args.data());
        { std::lock_guard<std::mutex> lk(g_mu); g_active_pid = pp; }

        int wst = 0;
        while (true) {
            pid_t r = waitpid(pp, &wst, WNOHANG);
            if (r == pp) break;
            if (r < 0)  { wst = -1; break; }
            if (g_cf_shutdown.load()) { kill(pp, SIGTERM); waitpid(pp, nullptr, 0); wst = -1; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        { std::lock_guard<std::mutex> lk(g_mu); g_active_pid = -1; }
        return (wst >= 0) && WIFEXITED(wst) && WEXITSTATUS(wst) == 0;
    };

    bool ok = attempt(/*use_hwaccel=*/true);
    if (!ok && !g_cf_shutdown.load()) {
        fs::remove(tmp);
        fprintf(stderr, "[conform] hw decode failed, retrying software: %s\n", j.src.c_str());
        ok = attempt(/*use_hwaccel=*/false);
    }

    if (ok && fs::exists(tmp)) {
        std::error_code ec;
        fs::rename(tmp, j.out, ec);
        if (ec) fs::remove(tmp);
        else    { std::lock_guard<std::mutex> lk(g_mu); g_ready_cache.insert(j.out); }
    } else {
        if (!ok && !g_cf_shutdown.load())
            fprintf(stderr, "[conform] FAILED (both hw+sw): %s\n", j.src.c_str());
        fs::remove(tmp);
    }
}

static void worker_loop() {
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return g_cf_shutdown.load() || !g_queue.empty(); });
            if (g_cf_shutdown.load()) return;
            j = g_queue.front(); g_queue.pop_front();
            g_queued.erase(j.out);
            g_active_out = j.out;
        }
        if (!fs::exists(j.out)) run_job(j);
        { std::lock_guard<std::mutex> lk(g_mu); g_active_out.clear(); }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────
bool conform_is_ready(const std::string& src, int fps, bool smooth, bool loop) {
    std::string out = conform_path(src, fps, smooth, loop);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_ready_cache.count(out)) return true;
    }
    if (fs::exists(out)) { std::lock_guard<std::mutex> lk(g_mu); g_ready_cache.insert(out); return true; }
    return false;
}

void conform_start(const std::string& src, int fps, bool smooth, bool loop) {
    if (src.empty() || fps <= 0) return;
    std::string out = conform_path(src, fps, smooth, loop);
    if (conform_is_ready(src, fps, smooth, loop)) return;

    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_worker_started) {
        g_worker_started = true;
        std::thread(worker_loop).detach();
    }
    if (g_active_out == out || g_queued.count(out)) return;   // already in flight
    g_queue.push_back({src, out, fps, smooth, loop});
    g_queued.insert(out);
    g_cv.notify_one();
}

ConformStatus conform_status(const std::string& src, int fps, bool smooth, bool loop) {
    std::string out = conform_path(src, fps, smooth, loop);
    if (conform_is_ready(src, fps, smooth, loop)) return {ConformState::Ready, 1.f};
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_active_out == out)   return {ConformState::Working, 0.f};
    if (g_queued.count(out))   return {ConformState::Queued,  0.f};
    return {ConformState::Idle, 0.f};
}

void conform_cancel() {
    g_cf_shutdown.store(true);
    g_cv.notify_all();
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_active_pid > 0) { kill(g_active_pid, SIGTERM); }
    g_queue.clear();
    g_queued.clear();
}
