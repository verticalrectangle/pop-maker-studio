#include "app.h"
#include "audio.h"
#include "video.h"
#include "transcribe.h"
#include "render.h"
#include "fx_shader.h"
#include "presets.h"
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
    if (name == "opacity") {
        float base = opacity;
        float dur  = end - start;
        if (fade_in  > 0.f && t < fade_in)
            base *= (t / fade_in);
        if (fade_out > 0.f && dur > 0.f && t > dur - fade_out)
            base *= ((dur - t) / fade_out);
        return fmaxf(0.f, fminf(1.f, base));
    }
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

EffectAccum collect_effects(const AppState& state, float t, int below_track_idx) {
    EffectAccum acc;
    for (int ti = 0; ti < below_track_idx && ti < (int)state.tracks.size(); ++ti) {
        for (auto& cl : state.tracks[ti].clips) {
            if (cl.clip_type != ClipType::Effect) continue;
            if (t < cl.start || t >= cl.end) continue;
            if (cl.fx_color_on) {
                acc.brightness += cl.fx_brightness;
                acc.contrast   *= cl.fx_contrast;
                acc.saturation *= cl.fx_saturation;
                acc.hue        += cl.fx_hue;
                acc.any_color   = true;
            }
            if (cl.fx_blur_on) {
                acc.blur     += cl.fx_blur;
                acc.any_blur  = true;
            }
            if (cl.fx_vignette_on) {
                acc.vignette     = fminf(1.f, acc.vignette + cl.fx_vignette);
                acc.any_vignette = true;
            }
            if (cl.fx_text_on) {
                acc.opacity_mul *= cl.fx_opacity_mul;
                acc.scale_mul   *= cl.fx_scale_mul;
                acc.any_text     = true;
            }
        }
    }
    return acc;
}

CreativeFXAccum collect_creative_fx(const AppState& state, float t, int below_track_idx) {
    CreativeFXAccum acc;
    for (int ti = 0; ti < below_track_idx && ti < (int)state.tracks.size(); ++ti) {
        for (auto& cl : state.tracks[ti].clips) {
            if (cl.clip_type != ClipType::Effect) continue;
            if (cl.fx_type == FXType::Adjustment)  continue;
            if (t < cl.start || t >= cl.end)       continue;
            switch (cl.fx_type) {
                case FXType::Glitch:
                    acc.glitch_on         = true;
                    acc.glitch_chroma     = fmaxf(acc.glitch_chroma,     cl.fx_glitch_chroma);
                    acc.glitch_jitter     = fmaxf(acc.glitch_jitter,     cl.fx_glitch_jitter);
                    acc.glitch_corruption       = fmaxf(acc.glitch_corruption,       cl.fx_glitch_corruption);
                    acc.glitch_corruption_bleed = fmaxf(acc.glitch_corruption_bleed, cl.fx_glitch_corruption_bleed);
                    break;
                case FXType::ZoomPunch:
                    acc.zoom_on       = true;
                    acc.zoom_strength = fmaxf(acc.zoom_strength, cl.fx_zoom_strength);
                    acc.zoom_decay    = fmaxf(acc.zoom_decay,    cl.fx_zoom_decay);
                    acc.zoom_shake    = fmaxf(acc.zoom_shake,    cl.fx_zoom_shake);
                    break;
                case FXType::LightLeak:
                    acc.leak_on        = true;
                    acc.leak_intensity = fmaxf(acc.leak_intensity, cl.fx_leak_intensity);
                    acc.leak_speed     = fmaxf(acc.leak_speed,     cl.fx_leak_speed);
                    break;
                case FXType::VHS:
                    acc.vhs_on       = true;
                    acc.vhs_noise    = fmaxf(acc.vhs_noise,    cl.fx_vhs_noise);
                    acc.vhs_bleed    = fmaxf(acc.vhs_bleed,    cl.fx_vhs_bleed);
                    acc.vhs_tracking = fmaxf(acc.vhs_tracking, cl.fx_vhs_tracking);
                    break;
                case FXType::Datamosh:
                    acc.datamosh_on         = true;
                    acc.datamosh_intensity  = fmaxf(acc.datamosh_intensity,  cl.fx_datamosh_intensity);
                    acc.datamosh_decay      = fmaxf(acc.datamosh_decay,      cl.fx_datamosh_decay);
                    acc.datamosh_block_size = cl.fx_datamosh_block_size;
                    acc.datamosh_clip_start = cl.start;
                    break;
                default: break;
            }
        }
    }
    return acc;
}

void app_init(AppState& state) {
    theme_apply();
    audio_init();
    render_init_fonts();
    fx_shader_init();
    state.user_presets = presets_load_user();
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
    fx_shader_shutdown();
    (void)state;
}
