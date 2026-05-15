#pragma once
#include "app.h"
#include <string>

// Start a voice conversion job on an audio clip using system Python + .pth model.
// On first use, bootstraps a venv and installs torch/torchaudio automatically.
// Updates clip.vc_status / vc_progress / vc_out_path via vc_poll().
void vc_start(AppState& state, int track_idx, int clip_idx,
              const std::string& model_path, int f0_semitones = 0);

// Poll all running jobs and flush results back to matching clips.
void vc_poll(AppState& state);

// Cancel all running jobs (detaches threads).
void vc_cancel_all();

// True if the ML Python venv is already installed.
bool vc_venv_ready();

// Path to the venv python3 binary, or empty if not installed.
std::string vc_venv_python_path();
