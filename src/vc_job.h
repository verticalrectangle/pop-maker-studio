#pragma once
#include "app.h"
#include <string>

// Start a voice conversion job on an audio clip.
// Decodes the clip's source to WAV then runs voice_convert.py with the given
// model. Updates clip.vc_status / vc_progress / vc_out_path via vc_poll().
void vc_start(AppState& state, int track_idx, int clip_idx,
              const std::string& model_path,
              const std::string& python_path,
              const std::string& script_path);

// Poll all running vc jobs and flush results back to matching clips.
// Call once per frame from the main thread.
void vc_poll(AppState& state);

// Cancel all running jobs.
void vc_cancel_all();
