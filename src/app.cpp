#include "app.h"
#include "audio.h"
#include "video.h"
#include "transcribe.h"
#include "ui/theme.h"
#include "ui/screens.h"
#include <imgui.h>
#include <algorithm>

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

    if (state.splash_timer > 0.f) {
        state.splash_timer -= io.DeltaTime;
        ui_splash(state);
    } else {
        ui_studio(state);
    }

    if (state.playing) {
        state.playhead += io.DeltaTime;
        if (state.duration > 0.f && state.playhead >= state.duration) {
            state.playhead = 0.f;
            state.playing  = false;
        }
    }

    ImGui::End();
}

void app_shutdown(AppState& state) {
    audio_shutdown();
    video_close();
    transcribe_cancel();
    (void)state;
}
