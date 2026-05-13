#pragma once
#include "app.h"
#include <string>

// Launch noise reduction as a background job using ffmpeg's afftdn filter.
// On completion, state.noise_reduce_output is set to the cleaned WAV path.
void noise_reduce_start(AppState& state, const std::string& input_path);

// Poll the running job; call once per frame from the main thread.
void noise_reduce_poll(AppState& state);
