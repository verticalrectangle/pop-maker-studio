#pragma once
#include "app.h"
#include <string>

// Start a voice conversion job on an audio clip using a .pth model.
// Requires hubert.onnx to be present (see hubert_onnx_path() in vc_onnx.h).
// Updates clip.vc_status / vc_progress / vc_out_path via vc_poll().
void vc_start(AppState& state, int track_idx, int clip_idx,
              const std::string& model_path, int f0_semitones = 0,
              bool f0_auto = true);

// Poll all running jobs and flush results back to matching clips.
void vc_poll(AppState& state);

// Drop a clip's converted-voice substitution once no active voice-convert FX
// targets it (brick deleted, chain entry removed, decoupled, model cleared).
// Call every frame right after vc_poll(). The conversion is sticky clip state
// with no link to the FX that made it, so without this the changed vocals stay
// on the track after its brick is gone.
void vc_reconcile(AppState& state);

// Cancel all running jobs (detaches threads).
void vc_cancel_all();
