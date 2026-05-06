#pragma once
#include "app.h"
#include <string>

// Launch noise reduction as a background subprocess.
// On completion, state.noise_reduce_output is set to the cleaned WAV path.
void noise_reduce_start(AppState& state, const std::string& input_path,
                        const std::string& python_path,
                        const std::string& script_path);

// Poll the running job; call once per frame from the main thread.
void noise_reduce_poll(AppState& state);

// Returns true if noisereduce + soundfile are importable in python_path.
// Cached after first check.
bool noise_reduce_is_installed(const std::string& python_path);
