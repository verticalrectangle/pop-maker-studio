#pragma once
#include "app.h"
#include <string>

// Start a voice conversion job on an audio clip using a .pth model.
// Requires hubert.onnx to be present (see hubert_onnx_path() in vc_onnx.h).
// Updates clip.vc_status / vc_progress / vc_out_path via vc_poll().
void vc_start(AppState& state, int track_idx, int clip_idx,
              const std::string& model_path, int f0_semitones = 0);

// Poll all running jobs and flush results back to matching clips.
void vc_poll(AppState& state);

// Cancel all running jobs (detaches threads).
void vc_cancel_all();
