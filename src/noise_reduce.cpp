#include "noise_reduce.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <filesystem>
#include <cstdio>
#include <cstdlib>

namespace fs = std::filesystem;

struct NrJob {
    std::atomic<int>  status{0};   // 0=running 1=done 2=error
    std::mutex        mu;
    std::string       output;
    std::string       error;
};

static std::shared_ptr<NrJob> g_job;
static std::mutex              g_job_mu;

static void run_nr(std::shared_ptr<NrJob> job,
                   const std::string& input_path,
                   const std::string& output_path) {
    std::string cmd = "ffmpeg -hide_banner -loglevel error -y"
                      " -i \"" + input_path + "\""
                      " -af \"afftdn=nf=-25\""
                      " \"" + output_path + "\" 2>&1";

    int ret = system(cmd.c_str());  // NOLINT

    std::lock_guard<std::mutex> lk(job->mu);
    if (ret == 0 && fs::exists(output_path)) {
        job->output = output_path;
        job->status.store(1);
    } else {
        job->error = "ffmpeg afftdn failed (exit " + std::to_string(ret) + ")";
        job->status.store(2);
    }
}

void noise_reduce_start(AppState& state, const std::string& input_path) {
    std::lock_guard<std::mutex> lk(g_job_mu);

    std::string out = input_path;
    auto dot = out.rfind('.');
    if (dot != std::string::npos)
        out = out.substr(0, dot) + "_denoised.wav";
    else
        out += "_denoised.wav";

    state.noise_reduce_running  = true;
    state.noise_reduce_progress = 0.f;
    state.noise_reduce_output.clear();
    state.noise_reduce_error.clear();

    g_job = std::make_shared<NrJob>();
    std::thread t(run_nr, g_job, input_path, out);
    t.detach();
}

void noise_reduce_poll(AppState& state) {
    std::lock_guard<std::mutex> lk(g_job_mu);
    if (!g_job) return;

    int s = g_job->status.load();
    if (s == 0) return;

    std::lock_guard<std::mutex> jlk(g_job->mu);
    if (s == 1) {
        state.noise_reduce_output = g_job->output;
    } else {
        state.noise_reduce_error  = g_job->error;
    }
    state.noise_reduce_running  = false;
    state.noise_reduce_progress = 1.f;
    g_job.reset();
}
