#include "app.h"
#include "audio.h"
#include "transcribe.h"
#include "ui/theme.h"
#include "ui/screens.h"
#include <imgui.h>
#include <sstream>

std::string LyricLine::full_text() const {
    std::string s;
    for (size_t i = 0; i < words.size(); ++i) {
        if (i) s += ' ';
        s += words[i].text;
    }
    return s;
}

void app_init(AppState& state) {
    theme_apply();
    audio_init();
    (void)state;
}

void app_frame(AppState& state) {
    // Full-screen dockspace window
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

    // ── Top nav bar ──────────────────────────────────────────────────────────
    ui_topbar(state);

    // ── Arrow key navigation ─────────────────────────────────────────────────
    if (!ImGui::IsAnyItemActive()) {
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

    // ── Tick audio playhead ──────────────────────────────────────────────────
    if (state.playing) {
        state.playhead += io.DeltaTime;
        if (state.duration > 0.f && state.playhead >= state.duration) {
            state.playhead = 0.f;
            state.playing  = false;
        }
        // update active word
        state.active_line = -1;
        state.active_word = -1;
        for (int li = 0; li < (int)state.lines.size(); ++li) {
            const auto& line = state.lines[li];
            for (int wi = 0; wi < (int)line.words.size(); ++wi) {
                if (state.playhead >= line.words[wi].start &&
                    state.playhead <  line.words[wi].end) {
                    state.active_line = li;
                    state.active_word = wi;
                }
            }
        }
    }

    // ── Active screen ────────────────────────────────────────────────────────
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
    transcribe_cancel();
    (void)state;
}
