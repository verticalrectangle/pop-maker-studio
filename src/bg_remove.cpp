#include "bg_remove.h"
#include "proxy.h"
#include <filesystem>
#include <map>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <sstream>
#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;

// ── Job data shared between dispatch thread and poll ──────────────────────────

struct JobData {
    std::atomic<float> progress{0.f};
    std::atomic<int>   status{0};   // 0=running 1=done 2=error
    std::string        error_msg;
    std::mutex         mu;
};

struct BgJob {
    std::shared_ptr<JobData> data;
    std::thread              thread;
};

static std::map<std::string, BgJob> g_jobs;   // keyed by mask_dir
static std::mutex                   g_jobs_mu;

// ── Path helpers ──────────────────────────────────────────────────────────────

std::string bg_remove_proxy_dir(const std::string& video_path) {
    fs::path p(video_path);
    return (p.parent_path() / (p.stem().string() + "_bg_masks")).string();
}

std::string bg_remove_hires_dir(const std::string& video_path) {
    fs::path p(video_path);
    return (p.parent_path() / (p.stem().string() + "_bg_hires")).string();
}

float bg_remove_read_fps(const std::string& mask_dir) {
    std::string fp = mask_dir + "/fps.txt";
    FILE* f = fopen(fp.c_str(), "r");
    if (!f) return 30.f;
    float fps = 30.f;
    fscanf(f, "%f", &fps);
    fclose(f);
    return (fps > 0.f && fps < 1000.f) ? fps : 30.f;
}

// ── Internal: run the rembg script as a child process ────────────────────────

static void run_job(std::shared_ptr<JobData> data,
                    const std::string& python_path,
                    const std::string& rembg_script,
                    const std::string& input_path,
                    const std::string& output_dir) {
    std::ostringstream cmd;
    cmd << "\"" << python_path   << "\" "
        << "\"" << rembg_script  << "\" "
        << "--input  \"" << input_path  << "\" "
        << "--output \"" << output_dir  << "\" 2>&1";

    fprintf(stderr, "[bg_remove] cmd: %s\n", cmd.str().c_str());
    FILE* pipe = popen(cmd.str().c_str(), "r");
    if (!pipe) {
        std::lock_guard<std::mutex> lk(data->mu);
        data->error_msg = "popen failed — could not launch: " + cmd.str();
        data->status.store(2);
        return;
    }

    char buf[512];
    std::string last_lines;  // accumulate non-JSON output for error reporting
    while (fgets(buf, sizeof(buf), pipe)) {
        std::string line(buf);
        if (!line.empty() && line.back() == '\n') line.pop_back();

        // {"progress": 0.45}
        auto pp = line.find("\"progress\":");
        if (pp != std::string::npos) {
            float p = 0.f;
            sscanf(line.c_str() + pp + 11, "%f", &p);
            data->progress.store(p);
            continue;
        }
        // {"done": true}
        if (line.find("\"done\"") != std::string::npos) continue;

        // {"error": "..."} — structured error from script
        auto ep = line.find("\"error\":");
        if (ep != std::string::npos) {
            std::lock_guard<std::mutex> lk(data->mu);
            data->error_msg = line;
            continue;
        }

        // Unstructured output (tracebacks, import errors, etc.) — keep last 400 chars
        if (!line.empty()) {
            last_lines += line + "\n";
            if (last_lines.size() > 400) last_lines = last_lines.substr(last_lines.size() - 400);
        }
    }

    int ret = pclose(pipe);
    if (ret != 0) {
        std::lock_guard<std::mutex> lk(data->mu);
        if (data->error_msg.empty())
            data->error_msg = last_lines.empty() ? "Script exited with error (no output)" : last_lines;
    }
    data->status.store(ret == 0 ? 1 : 2);
}

// ── Public API ────────────────────────────────────────────────────────────────

void bg_remove_start(AppState& state, int track_idx, int clip_idx,
                     const std::string& python_path,
                     const std::string& rembg_script) {
    if (track_idx < 0 || track_idx >= (int)state.tracks.size()) return;
    auto& track = state.tracks[track_idx];
    if (clip_idx < 0 || clip_idx >= (int)track.clips.size()) return;
    Clip& clip = track.clips[clip_idx];
    if (clip.clip_type != ClipType::Video || clip.text.empty()) return;

    std::string mjpeg = proxy_mjpeg_path(clip.text);
    if (!fs::exists(mjpeg)) {
        clip.bg_remove_status = BgRemoveStatus::Error;
        clip.bg_remove_error  = "Proxy not ready — wait for proxy generation to finish.";
        return;
    }

    std::string mdir = bg_remove_proxy_dir(clip.text);
    clip.bg_remove_mask_dir = mdir;
    clip.bg_remove_status   = BgRemoveStatus::Processing;
    clip.bg_remove_progress = 0.f;
    clip.bg_remove_error.clear();

    // Cancel any existing job for this mask dir.
    {
        std::lock_guard<std::mutex> lk(g_jobs_mu);
        auto it = g_jobs.find(mdir);
        if (it != g_jobs.end()) {
            // Thread is detached via shared_ptr; just remove the map entry.
            g_jobs.erase(it);
        }
    }

    auto data = std::make_shared<JobData>();

    BgJob job;
    job.data   = data;
    job.thread = std::thread(run_job, data, python_path, rembg_script, mjpeg, mdir);
    job.thread.detach();

    {
        std::lock_guard<std::mutex> lk(g_jobs_mu);
        g_jobs[mdir] = std::move(job);
    }
}

void bg_remove_poll(AppState& state) {
    std::lock_guard<std::mutex> lk(g_jobs_mu);

    for (auto& [mdir, job] : g_jobs) {
        int   st   = job.data->status.load();
        float prog = job.data->progress.load();

        for (auto& track : state.tracks) {
            for (auto& clip : track.clips) {
                if (clip.bg_remove_mask_dir != mdir) continue;
                clip.bg_remove_progress = prog;
                if (st == 1) {
                    clip.bg_remove_status = BgRemoveStatus::Ready;
                } else if (st == 2) {
                    clip.bg_remove_status = BgRemoveStatus::Error;
                    std::lock_guard<std::mutex> lk2(job.data->mu);
                    clip.bg_remove_error = job.data->error_msg;
                }
            }
        }
    }

    // Remove completed jobs.
    for (auto it = g_jobs.begin(); it != g_jobs.end(); ) {
        if (it->second.data->status.load() != 0)
            it = g_jobs.erase(it);
        else
            ++it;
    }
}

void bg_remove_cancel_all() {
    std::lock_guard<std::mutex> lk(g_jobs_mu);
    g_jobs.clear();
}

bool bg_remove_run_hires(const std::string& video_path,
                          const std::string& output_dir,
                          const std::string& python_path,
                          const std::string& rembg_script) {
    // Skip if already done.
    if (fs::exists(output_dir + "/fps.txt")) return true;

    std::ostringstream cmd;
    cmd << "\"" << python_path   << "\" "
        << "\"" << rembg_script  << "\" "
        << "--input  \"" << video_path   << "\" "
        << "--output \"" << output_dir   << "\"";

    int ret = system(cmd.str().c_str());
    return (ret == 0);
}

// ── rembg package install ─────────────────────────────────────────────────────

static std::atomic<int>  s_rembg_ok{-1};        // -1=unknown 0=no 1=yes
static std::atomic<int>  s_install_status{0};   // 0=idle 1=running 2=done 3=failed
static std::string       s_install_error;
static std::mutex        s_install_mu;

bool rembg_is_installed(const std::string& python_path) {
    int cached = s_rembg_ok.load();
    if (cached >= 0) return cached == 1;
    std::string cmd = "\"" + python_path + "\" -c \"import rembg\" 2>/dev/null";
    int ret = system(cmd.c_str());
    s_rembg_ok.store(ret == 0 ? 1 : 0);
    return ret == 0;
}

void rembg_install_reset() {
    s_rembg_ok.store(-1);
}

void rembg_install_start(const std::string& python_path) {
    if (s_install_status.load() == 1) return;
    s_install_status.store(1);
    { std::lock_guard<std::mutex> lk(s_install_mu); s_install_error.clear(); }

    std::thread([python_path]() {
        std::string cmd = "\"" + python_path + "\" -m pip install --progress-bar off rembg 2>&1";
        FILE* pipe = popen(cmd.c_str(), "r");
        std::string out;
        if (pipe) {
            char buf[256];
            while (fgets(buf, sizeof(buf), pipe)) out += buf;
            int ret = pclose(pipe);
            if (ret != 0) {
                std::lock_guard<std::mutex> lk(s_install_mu);
                s_install_error = out.size() > 300 ? out.substr(out.size() - 300) : out;
                s_install_status.store(3);
            } else {
                rembg_install_reset();
                s_install_status.store(2);
            }
        } else {
            std::lock_guard<std::mutex> lk(s_install_mu);
            s_install_error = "popen failed";
            s_install_status.store(3);
        }
    }).detach();
}

RembgInstallStatus rembg_install_status() {
    switch (s_install_status.load()) {
        case 1: return RembgInstallStatus::Running;
        case 2: return RembgInstallStatus::Done;
        case 3: return RembgInstallStatus::Failed;
        default: return RembgInstallStatus::Idle;
    }
}

std::string rembg_install_error() {
    std::lock_guard<std::mutex> lk(s_install_mu);
    return s_install_error;
}
