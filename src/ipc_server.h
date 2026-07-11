#pragma once
#include "app.h"

// Opens the Unix domain socket and writes the lock file.
// Call once at app init (after GL is set up so validate_glsl works).
void ipc_server_start();

// Reads pending IPC messages and dispatches them against state.
// Non-blocking. Call every frame from app_frame.
void ipc_server_poll(AppState& state);

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
