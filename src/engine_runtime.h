#pragma once
// engine_runtime.h — the engine's per-frame heartbeat and its event feed.
//
// engine_tick owns every engine worker pump that used to live scattered in
// the desktop frame loop (app_frame + the studio screen). The desktop app
// calls it once per frame with gl_ready=true; the C ABI's pms_tick calls it
// with gl_ready=false (no GL context — GL-dependent pumps are skipped until
// the RenderSurface seam lands in Phase 2).
//
// Events: engine subsystems emit JSON events (or engine_tick diffs state
// into events); pms_poll_events drains them. Event types match the Swift
// bridge in pms-ios (playhead, pipeline, loudness, face_track, takes).
#include "app.h"
#include <string>

void engine_tick(AppState& state, double dt, bool gl_ready);

void        engine_emit_event(const std::string& json_event);
std::string engine_drain_events();   // JSON array; empties the queue
