#pragma once
#include "app.h"

// Opens the Unix domain socket and writes the lock file.
// Call once at app init (after GL is set up so validate_glsl works).
void ipc_server_start();

// Reads pending IPC messages and dispatches them against state.
// Non-blocking. Call every frame from app_frame.
void ipc_server_poll(AppState& state);
// Republish the saved (.pms v72) analysis request after project load: cached
// analysis publishes synchronously, cache miss starts the background run.
void audio_analysis_republish(AppState& state);
// Block up to timeout_ms waiting for IPC activity (new connection or bytes on
// any client). Used by the main-loop idle throttle so an IPC arrival wakes us
// immediately instead of waiting out the sleep. Zero fds / error → returns.
void ipc_wait_for_request(int timeout_ms);

// Closes the socket and removes the lock file.
// Call at app shutdown.
void ipc_server_stop();

// Feeds one queued synthetic mouse step (from the ui_input IPC method) into
// ImGui. Call from the main loop between ImGui_ImplGlfw_NewFrame() and
// ImGui::NewFrame() so injected events land after the backend's own.
void ipc_debug_input_tick();

// The lever chokepoint, socket-free: parse one JSON request
// ({"id","method","params"}), dispatch, return the JSON reply. This is what
// the pms_engine C ABI and the headless test target call; the socket server
// is a thin wrapper over the same dispatch.
std::string engine_command(AppState& state, const std::string& json_request);

// Live scene-analysis (describe_video) progress for the canvas banner. Returns
// true while a run is active; fills the counts (any pointer may be null).
bool scene_analysis_progress(int* vid_idx, int* vid_total, int* frame_idx, int* frame_total);
// th/perf-decode: process-start steady_clock seconds, shared by bench_tick()
// (ipc_server.cpp) and the draw_preview presented-frame hook (canvas.cpp) so
// seek-to-present latencies use one epoch.
double bench_now_s();
