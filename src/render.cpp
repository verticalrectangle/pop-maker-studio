#include "render.h"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <atomic>
#include <cmath>

static std::atomic<bool> g_cancel{false};

static std::string srt_timestamp(float seconds) {
    int h  = (int)(seconds / 3600);
    int m  = (int)((seconds - h * 3600) / 60);
    int s  = (int)(seconds) % 60;
    int ms = (int)(std::fmod(seconds, 1.f) * 1000.f);
    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(2) << h  << ":"
        << std::setw(2) << m  << ":"
        << std::setw(2) << s  << ","
        << std::setw(3) << ms;
    return oss.str();
}

bool render_export_srt(const AppState& state, const std::string& out_path) {
    std::ofstream f(out_path);
    if (!f) return false;
    int idx = 1;
    auto indices = state.subtitle_clip_indices();
    for (auto& [ti, ci] : indices) {
        const Clip& clip = state.tracks[ti].clips[ci];
        f << idx++ << "\n"
          << srt_timestamp(clip.start) << " --> " << srt_timestamp(clip.end) << "\n"
          << clip.text << "\n\n";
    }
    return true;
}

void render_start(AppState& state) {
    g_cancel.store(false);
    state.render.running  = true;
    state.render.progress = 0.f;
    state.render.frame    = 0;
    state.render.stage    = "Initialising…";

    std::thread([&state]() {
        if (!state.out_srt.empty())
            render_export_srt(state, state.out_srt);

        state.render.total_frames = 5040;
        for (int i = 0; i <= state.render.total_frames && !g_cancel.load(); ++i) {
            state.render.frame    = i;
            state.render.progress = (float)i / state.render.total_frames;
            state.render.stage    = i < 500  ? "Writing header…"  :
                                    i < 4500 ? "Encoding frames…" :
                                               "Muxing audio…";
            state.render.eta_secs = (state.render.total_frames - i) / 1400.f;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        state.render.running  = false;
        state.render.progress = 1.f;
        state.render.stage    = g_cancel.load() ? "Cancelled" : "Done";
        if (!g_cancel.load())
            state.render_done = true;
    }).detach();
}

void render_cancel() {
    g_cancel.store(true);
}
