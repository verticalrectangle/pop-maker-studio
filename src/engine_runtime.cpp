// engine_runtime.cpp — see engine_runtime.h.
#include "engine_runtime.h"
#include "engine_seams.h"
#include "audio.h"
#include "loudness.h"
#include "face_track.h"
#include "recorder.h"
#include "video_recorder.h"
#include "runtime_fx.h"
#include "render.h"
#include "pipeline_core.h"
#include "globals.h"
#include "json.hpp"
#include <cmath>
#include <mutex>
#include <vector>

using nlohmann::json;

// ── Event queue ───────────────────────────────────────────────────────────────
static std::mutex        g_ev_mtx;
static std::vector<json> g_ev_queue;

void engine_emit_event(const std::string& ev) {
    json j = json::parse(ev, nullptr, false);
    if (j.is_discarded()) return;
    std::lock_guard<std::mutex> lk(g_ev_mtx);
    if (g_ev_queue.size() < 512) g_ev_queue.push_back(std::move(j));
}

std::string engine_drain_events() {
    std::lock_guard<std::mutex> lk(g_ev_mtx);
    json arr = json::array();
    for (auto& e : g_ev_queue) arr.push_back(std::move(e));
    g_ev_queue.clear();
    return arr.dump();
}

static void emit(json j) {
    std::lock_guard<std::mutex> lk(g_ev_mtx);
    if (g_ev_queue.size() < 512) g_ev_queue.push_back(std::move(j));
}

// ── State-diff event emission ─────────────────────────────────────────────────
// Pull-model: engine_tick compares cheap snapshots against the last emitted
// values instead of threading emit calls through every subsystem.
static void emit_state_events(AppState& state, double dt) {
    static float  last_playhead = -1.f;
    static bool   last_playing  = false;
    static int    last_stage    = -1;
    static float  last_progress = -1.f;
    static bool   last_face     = false;
    static int    last_takes    = -1;
    static double lufs_accum    = 0.0;

    if (fabsf(state.playhead - last_playhead) > 1e-4f ||
        state.playing != last_playing) {
        last_playhead = state.playhead;
        last_playing  = state.playing;
        emit({{"type", "playhead"}, {"t", state.playhead},
              {"playing", state.playing}});
    }
    int stage = (int)state.pipeline.stage;
    if (stage != last_stage ||
        fabsf(state.pipeline.progress - last_progress) > 0.01f) {
        last_stage    = stage;
        last_progress = state.pipeline.progress;
        static const char* kStageNames[] =
            {"idle", "extract", "transcribe", "align", "done", "error"};
        int si = stage >= 0 && stage < 6 ? stage : 0;
        emit({{"type", "pipeline"}, {"stage", kStageNames[si]},
              {"progress", state.pipeline.progress},
              {"message", state.pipeline.message}});
    }
    lufs_accum += dt;
    if (lufs_accum >= 0.15) {                       // ~7 Hz meter feed
        lufs_accum = 0.0;
        LoudnessSnapshot s = audio_master_meter();
        if (s.measured_secs > 0.05)
            emit({{"type", "loudness"},
                  {"momentary", s.momentary_lufs},
                  {"integrated", s.integrated_lufs},
                  {"peak_l", s.peak_l}, {"peak_r", s.peak_r},
                  {"clipped", s.clipped}});
    }
    if (face_track_available()) {
        FaceObs obs;
        bool valid = face_track_latest(obs) && obs.valid;
        if (valid != last_face) {
            last_face = valid;
            emit({{"type", "face_track"}, {"valid", valid}});
        }
    }
    int takes = vrecorder_take_count();
    if (takes != last_takes) {
        bool first = last_takes < 0;
        last_takes = takes;
        if (!first) emit({{"type", "takes"}, {"count", takes}});
    }
}

// ── The heartbeat ─────────────────────────────────────────────────────────────
void engine_tick(AppState& state, double dt, bool gl_ready) {
    // Coupled FX bricks track their host every frame — drags, trims, splits
    // and deletions all resolve here instead of in each edit path.
    fx_coupling_tick(state);

    // Lazy per-clip source probe (src_fps / src_duration). The proxy pipeline
    // generates intermediates; clip_video_src() picks them up when ready.
    clip_probe_tick(state);

    // Hot-reload custom effects.
    runtime_fx_poll(g_managed_dir + "/effects");

    // Beat analysis results land on their clips.
    poll_clip_beat_analysis(state);

    // Loop recorders: drain mic/camera, slice takes on the loop-cycle clock.
    recorder_tick(state);
    vrecorder_tick(state);

    // Single source of truth when paused: the playhead can never sit past
    // the last frame. Left alone while a record-brick loop cycles.
    if (!state.playing && !audio_loop_active() && state.duration > 0.f)
        state.playhead = fmaxf(0.f, fminf(state.playhead, last_playable_time(state)));

    if (gl_ready) {
        // IPC-requested export: pick up on the GL thread, then drive one
        // export frame per tick (GL calls must be on this thread).
        if (state.export_request && !state.render.running) {
            state.export_request = false;
            if (!state.export_out_path.empty())
                state.out_mp4 = state.export_out_path;
            std::string mp4 = state.out_mp4;
            size_t dot = mp4.rfind('.');
            state.out_gif = (dot != std::string::npos ? mp4.substr(0, dot) : mp4) + ".gif";
            render_start_gl(state);
        }
        render_tick_gl(state);

        // Project open in progress: open a few decoder slots per tick (GL
        // texture uploads) — the UI shows the progress banner off the queue.
        if (!state.slot_open_queue.empty())
            tick_video_slot_opens(state);
    }

    emit_state_events(state, dt);
}
