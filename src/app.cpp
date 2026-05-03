#include "app.h"
#include "audio.h"
#include "video.h"
#include "transcribe.h"
#include "render.h"
#include "ui/theme.h"
#include "ui/screens.h"
#include <imgui.h>
#include <algorithm>
#include <chrono>

std::vector<std::pair<int,int>> AppState::subtitle_clip_indices() const {
    std::vector<std::pair<int,int>> out;
    for (int ti = 0; ti < (int)tracks.size(); ++ti) {
        if (tracks[ti].type != TrackType::Subtitle) continue;
        for (int ci = 0; ci < (int)tracks[ti].clips.size(); ++ci)
            out.push_back({ti, ci});
    }
    std::sort(out.begin(), out.end(), [&](auto& a, auto& b){
        return tracks[a.first].clips[a.second].start <
               tracks[b.first].clips[b.second].start;
    });
    return out;
}

void app_init(AppState& state) {
    theme_apply();
    audio_init();
    render_init_fonts();
    (void)state;
}

void app_frame(AppState& state) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowBgAlpha(1.f);
    ImGui::Begin("##root", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize   |
        ImGuiWindowFlags_NoMove     |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_MenuBar
    );

    // Update playhead BEFORE rendering so the video frame shown this cycle
    // matches the audio position this cycle, not last cycle's.
    if (state.playing) {
        float pos;
        if (!audio_loading() && audio_is_playing()) {
            pos = audio_position() - audio_latency();
            if (pos < 0.f) pos = 0.f;
        } else {
            using namespace std::chrono;
            double elapsed = duration<double>(steady_clock::now() - state.play_start_wall).count();
            if (elapsed < 0.0) elapsed = 0.0;
            pos = state.play_start_pos + (float)elapsed;
        }
        state.playhead = pos;
        if (state.duration > 0.f && state.playhead >= state.duration) {
            state.playhead = state.duration;
            state.playing  = false;
            audio_pause();
            audio_seek(0.f);
        }
    }

    if (state.splash_timer > 0.f) {
        state.splash_timer -= io.DeltaTime;
        ui_splash(state);
    } else {
        ui_studio(state);
    }

    ImGui::End();
}

void app_shutdown(AppState& state) {
    audio_shutdown();
    video_close();
    transcribe_cancel();
    (void)state;
}
