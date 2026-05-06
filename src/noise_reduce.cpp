#include "noise_reduce.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <sstream>
#include <cstdio>

struct NrJob {
    std::atomic<float>       progress{0.f};
    std::atomic<int>         status{0};   // 0=running 1=done 2=error
    std::mutex               mu;
    std::string              output;
    std::string              error;
};

static std::shared_ptr<NrJob> g_job;
static std::mutex              g_job_mu;

static void run_nr(std::shared_ptr<NrJob> job,
                   const std::string& python_path,
                   const std::string& script_path,
                   const std::string& input_path,
                   const std::string& output_path) {
    std::ostringstream cmd;
    cmd << "\"" << python_path << "\" "
        << "\"" << script_path << "\" "
        << "--input  \"" << input_path  << "\" "
        << "--output \"" << output_path << "\" 2>&1";

    FILE* pipe = popen(cmd.str().c_str(), "r");
    if (!pipe) {
        std::lock_guard<std::mutex> lk(job->mu);
        job->error = "popen failed";
        job->status.store(2);
        return;
    }

    char buf[1024];
    std::string tail;
    while (fgets(buf, sizeof(buf), pipe)) {
        std::string line(buf);
        if (!line.empty() && line.back() == '\n') line.pop_back();

        auto pp = line.find("\"progress\":");
        if (pp != std::string::npos) {
            float p = 0.f;
            sscanf(line.c_str() + pp + 11, "%f", &p);
            job->progress.store(p);
            continue;
        }
        if (line.find("\"done\"") != std::string::npos) continue;

        auto ep = line.find("\"error\":");
        if (ep != std::string::npos) {
            std::lock_guard<std::mutex> lk(job->mu);
            job->error = line;
            continue;
        }
        if (!line.empty()) {
            tail += line + "\n";
            if (tail.size() > 600) tail = tail.substr(tail.size() - 600);
        }
    }

    int ret = pclose(pipe);
    std::lock_guard<std::mutex> lk(job->mu);
    if (ret == 0) {
        job->output = output_path;
    } else {
        if (job->error.empty())
            job->error = tail.empty() ? "Script failed (no output)" : tail;
    }
    job->status.store(ret == 0 ? 1 : 2);
}

void noise_reduce_start(AppState& state, const std::string& input_path,
                        const std::string& python_path,
                        const std::string& script_path) {
    std::lock_guard<std::mutex> lk(g_job_mu);

    // Build output path next to the input file
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
    std::thread t(run_nr, g_job, python_path, script_path, input_path, out);
    t.detach();
}

void noise_reduce_poll(AppState& state) {
    std::lock_guard<std::mutex> lk(g_job_mu);
    if (!g_job) return;

    state.noise_reduce_progress = g_job->progress.load();
    int s = g_job->status.load();
    if (s == 0) return;  // still running

    std::lock_guard<std::mutex> jlk(g_job->mu);
    if (s == 1) {
        state.noise_reduce_output  = g_job->output;
    } else {
        state.noise_reduce_error   = g_job->error;
    }
    state.noise_reduce_running  = false;
    g_job.reset();
}

bool noise_reduce_is_installed(const std::string& python_path) {
    static int s_cached = -1;
    static std::string s_python;
    if (s_cached >= 0 && s_python == python_path) return s_cached == 1;
    s_python = python_path;
    std::string cmd = "\"" + python_path + "\" -c \"import noisereduce, soundfile\" 2>/dev/null";
    s_cached = (system(cmd.c_str()) == 0) ? 1 : 0;
    return s_cached == 1;
}
