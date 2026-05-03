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
        ImGuiWindowFlags_NoScrollWithMouse
    );

    ui_topbar(state);

    if (!ImGui::IsAnyItemActive() && !ImGui::IsAnyItemFocused()) {
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            int next = (int)state.current_screen + 1;
            if (next <= (int)Screen::Export)
                state.go((Screen)next);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            int prev = (int)state.current_screen - 1;
            if (prev >= (int)Screen::Home)
                state.go((Screen)prev);
        }
    }

    if (state.playing) {
        state.playhead += io.DeltaTime;
        if (state.duration > 0.f && state.playhead >= state.duration) {
            state.playhead = 0.f;
            state.playing  = false;
        }
    }

    switch (state.current_screen) {
        case Screen::Home:   ui_screen_home(state);   break;
        case Screen::Upload: ui_screen_upload(state); break;
        case Screen::Editor: ui_screen_editor(state); break;
        case Screen::Styles: ui_screen_styles(state); break;
        case Screen::Export: ui_screen_export(state); break;
    }

    ImGui::End();
}

void app_shutdown(AppState& state) {
    audio_shutdown();
    video_close();
    transcribe_cancel();
    (void)state;
}
