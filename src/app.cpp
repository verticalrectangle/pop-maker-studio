// app.cpp — the desktop app frame: root window + screen dispatch. All core
// state machinery moved to src/app_state.cpp (engine) in the Phase 0 split.
#include "app.h"
#include "history.h"
#include "paths.h"
#include "audio.h"
#include "video.h"
#include "transcribe.h"
#include "render.h"
#include "fx_shader.h"
#include "body_fx.h"
#include "presets.h"
#include "runtime_fx.h"
#include "recorder.h"
#include "video_recorder.h"
#include "av_measure.h"
#include "ipc_server.h"
#include "agent_harness.h"
#include "globals.h"
#include "ui/theme.h"
#include "ui/screens.h"
#include "ui/panel_terminal.h"
#include "ui/studio_shared.h"   // last_playable_time
#include "conform.h"            // conform_cancel
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <cmath>


void app_init(AppState& state) {
    app_register_engine_hooks();
    theme_apply();
    audio_init();
    render_init_fonts();
    fx_shader_init();
    body_fx_init();   // compile the body-FX shaders (RemoveBackground, etc.) — was
                      // never called, so every body-FX brick silently no-op'd (prog=0)
    state.user_presets = presets_load_user();
    std::string effects_dir = g_managed_dir + "/effects";
    runtime_fx_init(effects_dir);
    ipc_server_start();
    // Baseline snapshot: history_undo() can't step back past entry 0, so
    // without this the first edit of a session is never undoable.
    history_push(state, "Session start");
}

void app_frame(AppState& state) {
    // Coupled FX bricks track their host every frame — drags, trims, splits
    // and deletions all resolve here instead of in each edit path.
    fx_coupling_tick(state);

    // Frame-rate conform: probe native fps, transcode mismatched clips to the
    // project rate in the background, and swap the preview/export to the
    // conformed copy when it lands.
    conform_tick(state);

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

    // Transport loop: cycle the loop brace region (or the whole timeline when no
    // region is set) seamlessly while playing. The record bricks own the audio
    // loop region when active, so defer to them.
    float loop_lo = 0.f, loop_hi = state.duration;
    loop_region(state, loop_lo, loop_hi);
    bool transport_loop = state.loop_play && loop_hi > loop_lo &&
                          !recorder_active() && !vrecorder_active();
    if (transport_loop && state.playing) {
        // Drive the audio engine's sample-accurate wrap so the master clock
        // (and the playhead derived from it) loops with no seam. Re-set each
        // frame so a region/duration change mid-loop is picked up immediately.
        audio_set_loop(loop_lo, loop_hi);
    } else if (audio_loop_active() && !recorder_active() && !vrecorder_active()) {
        // Loop turned off / playback stopped → drop our transport loop region
        // (never a recorder's — those clear it themselves on stop).
        audio_clear_loop();
    }

    // Keep state.duration covering all content. It's set ad-hoc on import /
    // typography and goes stale when a clip is dragged to a later start or a
    // second clip extends past it — which pinned the playhead at the old length
    // (couldn't scrub or play past ~30s) and made exports mismatch the real
    // content. Grow only, so deleting a clip never silently truncates the project.
    for (const auto& tr : state.tracks)
        for (const auto& c : tr.clips)
            if (c.end > state.duration) state.duration = c.end;

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
        if (transport_loop) {
            // The audio clock already wraps when sound is playing; this also
            // covers the silent (wall-clock) path, where pos grows unbounded.
            // Wrap at the region end → region start (playback before the region
            // plays in, then cycles — matches Ableton). Re-anchor the wall-clock
            // origin on wrap so drift can't accumulate.
            if (pos >= loop_hi) {
                pos = loop_lo + fmodf(pos - loop_lo, loop_hi - loop_lo);
                state.play_start_pos  = pos;
                state.play_start_wall = std::chrono::steady_clock::now();
                audio_seek(pos);
            }
            state.playhead = pos;
        } else {
            state.playhead = pos;
            // No end-of-project auto-stop while a loop region cycles (record
            // brick): the brick may extend past current content, and the wrap
            // keeps the playhead inside the loop anyway.
            if (!audio_loop_active() &&
                state.duration > 0.f && state.playhead >= state.duration) {
                // Park on the last real frame, not the exclusive end (which shows
                // nothing) — so playing to the end leaves the last frame on screen.
                state.playhead = last_playable_time(state);
                state.playing  = false;
                audio_pause();
                audio_seek(0.f);
            }
        }
    }
    // Single source of truth when paused: the playhead can never sit past the
    // last frame (replaces the old render-time nudge in canvas.cpp). Left alone
    // while a record-brick loop cycles past current content.
    if (!state.playing && !audio_loop_active() && state.duration > 0.f)
        state.playhead = fmaxf(0.f, fminf(state.playhead, last_playable_time(state)));

    // IPC-requested export: pick up on GL thread before ticking the render.
    if (state.export_request && !state.render.running) {
        state.export_request = false;
        if (!state.export_out_path.empty())
            state.out_mp4 = state.export_out_path;
        // sync gif path
        std::string mp4 = state.out_mp4;
        size_t dot = mp4.rfind('.');
        state.out_gif = (dot != std::string::npos ? mp4.substr(0, dot) : mp4) + ".gif";
        render_start_gl(state);
    }

    // Drive one export frame per app frame (GL calls must be on main thread).
    render_tick_gl(state);

    // Hot-reload custom effects and dispatch IPC messages.
    runtime_fx_poll(g_managed_dir + "/effects");
    ipc_server_poll(state);

    if (state.splash_timer > 0.f) {
        state.splash_timer -= io.DeltaTime;
        ui_splash(state);
    } else if (!state.models_ready && !state.models_skipped) {
        ui_setup(state);
    } else if (!state.in_studio) {
        ui_home(state);
    } else {
        autosave_tick(state, io.DeltaTime);   // periodic crash-recovery write
        ui_studio(state);
    }

    ImGui::End();
}

void app_shutdown(AppState& state) {
    g_shutdown.store(true);
    recovery_clear();       // clean exit → no crash-recovery file to offer next launch
    agent_shutdown();
    ipc_server_stop();
    vrecorder_shutdown();   // kill the camera-capture child so it isn't orphaned
    av_measure_shutdown();  // join any in-flight A/V-offset measurement
    conform_cancel();       // stop background conform transcodes (no orphan ffmpeg)
    runtime_fx_shutdown();
    audio_shutdown();
    video_close();
    transcribe_cancel();
    fx_shader_shutdown();
    terminal_panel_shutdown();
    (void)state;
}

