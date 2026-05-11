#include "vc_job.h"
#include <filesystem>
#include <map>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <functional>

namespace fs = std::filesystem;

// ── Per-job shared state ──────────────────────────────────────────────────────

struct VcJobData {
    std::atomic<float> progress{0.f};
    std::atomic<int>   status{0};   // 0=running 1=done 2=error
    std::string        out_path;
    std::string        error_msg;
    std::mutex         mu;
};

struct VcJob {
    std::shared_ptr<VcJobData> data;
    int    track_idx = -1;
    int    clip_idx  = -1;
    std::thread thread;
};

static std::vector<VcJob> g_jobs;
static std::mutex          g_jobs_mu;

// ── Background worker ─────────────────────────────────────────────────────────

static void run_job(std::shared_ptr<VcJobData> data,
                    const std::string& source_path,
                    const std::string& model_path,
                    const std::string& python_path,
                    const std::string& script_path,
                    const std::string& out_path)
{
    // Step 1: decode source audio to a temp WAV (ffmpeg → 44100 Hz stereo PCM)
    std::string wav_in = out_path + ".src.wav";
    std::string dec_cmd = "ffmpeg -hide_banner -loglevel error -y"
                          " -i \"" + source_path + "\""
                          " -vn -ar 44100 -ac 2 \"" + wav_in + "\" 2>/dev/null";
    if (system(dec_cmd.c_str()) != 0 || !fs::exists(wav_in)) {  // NOLINT
        std::lock_guard<std::mutex> lk(data->mu);
        data->error_msg = "Failed to decode source audio";
        data->status.store(2);
        return;
    }

    // Step 2: run voice_convert.py, reading PROGRESS lines
    std::string cmd = "\"" + python_path + "\" \"" + script_path + "\""
                    + " \"" + wav_in + "\""
                    + " \"" + model_path + "\""
                    + " \"" + out_path + "\" 2>&1";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        fs::remove(wav_in);
        std::lock_guard<std::mutex> lk(data->mu);
        data->error_msg = "Failed to launch voice_convert.py";
        data->status.store(2);
        return;
    }

    char buf[512];
    std::string last_lines;
    while (fgets(buf, sizeof(buf), pipe)) {
        std::string line(buf);
        if (!line.empty() && line.back() == '\n') line.pop_back();

        float p = 0.f;
        if (sscanf(line.c_str(), "PROGRESS %f", &p) == 1) {
            data->progress.store(p);
            continue;
        }
        if (line.substr(0, 5) == "ERROR") {
            std::lock_guard<std::mutex> lk(data->mu);
            data->error_msg = line.substr(6);
            continue;
        }
        if (!line.empty()) {
            last_lines += line + "\n";
            if (last_lines.size() > 400)
                last_lines = last_lines.substr(last_lines.size() - 400);
        }
    }

    int ret = pclose(pipe);
    fs::remove(wav_in);

    if (ret != 0 || !fs::exists(out_path)) {
        std::lock_guard<std::mutex> lk(data->mu);
        if (data->error_msg.empty())
            data->error_msg = last_lines.empty()
                ? "voice_convert.py failed (no output)"
                : last_lines;
        data->status.store(2);
        return;
    }

    data->progress.store(1.f);
    data->status.store(1);
}

// ── Public API ────────────────────────────────────────────────────────────────

void vc_start(AppState& state, int track_idx, int clip_idx,
              const std::string& model_path,
              const std::string& python_path,
              const std::string& script_path)
{
    if (track_idx < 0 || track_idx >= (int)state.tracks.size()) return;
    auto& track = state.tracks[track_idx];
    if (clip_idx < 0 || clip_idx >= (int)track.clips.size()) return;
    Clip& clip = track.clips[clip_idx];
    if (clip.clip_type != ClipType::Audio || clip.text.empty()) return;

    // Deterministic output path based on source + model
    uint64_t h = std::hash<std::string>{}(clip.text + model_path);
    std::string out_path = "/tmp/pms_vc_" + std::to_string(h) + ".wav";

    clip.vc_status    = VcStatus::Processing;
    clip.vc_progress  = 0.f;
    clip.vc_out_path.clear();
    clip.vc_model_used = model_path;
    clip.vc_error.clear();

    auto data = std::make_shared<VcJobData>();
    data->out_path = out_path;

    VcJob job;
    job.data      = data;
    job.track_idx = track_idx;
    job.clip_idx  = clip_idx;
    job.thread    = std::thread(run_job, data,
                                clip.text, model_path,
                                python_path, script_path, out_path);

    std::lock_guard<std::mutex> lk(g_jobs_mu);
    g_jobs.push_back(std::move(job));
}

void vc_poll(AppState& state)
{
    std::lock_guard<std::mutex> lk(g_jobs_mu);
    for (auto it = g_jobs.begin(); it != g_jobs.end(); ) {
        auto& job  = *it;
        auto& data = *job.data;
        int st = data.status.load();

        if (job.track_idx < 0 || job.track_idx >= (int)state.tracks.size()) {
            if (st != 0 && job.thread.joinable()) job.thread.join();
            it = (st != 0) ? g_jobs.erase(it) : ++it;
            continue;
        }
        auto& track = state.tracks[job.track_idx];
        if (job.clip_idx < 0 || job.clip_idx >= (int)track.clips.size()) {
            if (st != 0 && job.thread.joinable()) job.thread.join();
            it = (st != 0) ? g_jobs.erase(it) : ++it;
            continue;
        }
        Clip& clip = track.clips[job.clip_idx];

        if (st == 0) {
            clip.vc_progress = data.progress.load();
            ++it;
        } else if (st == 1) {
            clip.vc_progress = 1.f;
            clip.vc_out_path = data.out_path;
            clip.vc_status   = VcStatus::Ready;
            if (job.thread.joinable()) job.thread.join();
            it = g_jobs.erase(it);
        } else {
            {
                std::lock_guard<std::mutex> lk2(data.mu);
                clip.vc_error = data.error_msg;
            }
            clip.vc_status = VcStatus::Error;
            if (job.thread.joinable()) job.thread.join();
            it = g_jobs.erase(it);
        }
    }
}

void vc_cancel_all()
{
    std::lock_guard<std::mutex> lk(g_jobs_mu);
    for (auto& job : g_jobs)
        if (job.thread.joinable()) job.thread.detach();
    g_jobs.clear();
}
