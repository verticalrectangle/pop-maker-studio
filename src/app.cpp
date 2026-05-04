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
#include <cmath>

// ── PropTrack ─────────────────────────────────────────────────────────────────

static float apply_interp(float t, InterpType it) {
    switch (it) {
        case InterpType::EaseIn:   return t * t;
        case InterpType::EaseOut:  return t * (2.f - t);
        case InterpType::EaseBoth: return t < 0.5f ? 2.f*t*t : -1.f + (4.f - 2.f*t)*t;
        case InterpType::Hold:     return 0.f;
        default:                   return t;
    }
}

float PropTrack::eval(float t) const {
    if (keys.empty()) return 0.f;
    if ((int)keys.size() == 1 || t <= keys.front().time) return keys.front().value;
    if (t >= keys.back().time) return keys.back().value;
    for (int i = 0; i < (int)keys.size() - 1; ++i) {
        if (t >= keys[i].time && t < keys[i+1].time) {
            if (keys[i].interp == InterpType::Hold) return keys[i].value;
            float alpha = (t - keys[i].time) / (keys[i+1].time - keys[i].time);
            alpha = apply_interp(alpha, keys[i].interp);
            return keys[i].value + alpha * (keys[i+1].value - keys[i].value);
        }
    }
    return keys.back().value;
}

void PropTrack::set(float t, float v, InterpType it) {
    for (auto& k : keys) {
        if (fabsf(k.time - t) < 0.02f) { k.value = v; k.interp = it; return; }
    }
    keys.push_back({t, v, it});
    std::sort(keys.begin(), keys.end(),
              [](const Keyframe& a, const Keyframe& b){ return a.time < b.time; });
}

void PropTrack::remove_at(float t, float tol) {
    keys.erase(std::remove_if(keys.begin(), keys.end(),
        [&](const Keyframe& k){ return fabsf(k.time - t) < tol; }), keys.end());
}

int PropTrack::find_nearest(float t, float tol) const {
    int best = -1; float bd = tol;
    for (int i = 0; i < (int)keys.size(); ++i) {
        float d = fabsf(keys[i].time - t);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

// ── Clip::eval_prop ───────────────────────────────────────────────────────────

float Clip::eval_prop(const std::string& name, float playhead) const {
    float t = playhead - start;
    auto it = ktracks.find(name);
    if (it != ktracks.end() && !it->second.empty())
        return it->second.eval(t);
    if (name == "pos_x")     return pos_x;
    if (name == "pos_y")     return pos_y;
    if (name == "scale_x")   return scale_x;
    if (name == "scale_y")   return scale_y;
    if (name == "rotation")  return rotation;
    if (name == "opacity")   return opacity;
    if (name == "volume")    return volume;
    if (name == "sub_pos_y") return sub_pos_y;
    return 0.f;
}

// ── AppState ──────────────────────────────────────────────────────────────────

std::vector<std::pair<int,int>> AppState::subtitle_clip_indices() const {
    std::vector<std::pair<int,int>> out;
    for (int ti = 0; ti < (int)tracks.size(); ++ti) {
        if (!tracks[ti].visible) continue;
        for (int ci = 0; ci < (int)tracks[ti].clips.size(); ++ci)
            if (tracks[ti].clips[ci].clip_type == ClipType::Text     ||
                tracks[ti].clips[ci].clip_type == ClipType::Lyrics   ||
                tracks[ti].clips[ci].clip_type == ClipType::Subtitle)
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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.f, 0.f});
    ImGui::Begin("##root", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize   |
        ImGuiWindowFlags_NoMove     |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_MenuBar
    );
    ImGui::PopStyleVar();  // WindowPadding

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
    } else if (!state.models_ready && !state.models_skipped) {
        ui_setup(state);
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
