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
