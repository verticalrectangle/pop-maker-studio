#pragma once
#include "app.h"
#include <string>

// Spawn background rembg job on the clip's MJPEG proxy.
// Updates clip.bg_remove_status / mask_dir immediately; progress via bg_remove_poll.
void bg_remove_start(AppState& state, int track_idx, int clip_idx,
                     const std::string& python_path,
                     const std::string& rembg_script);

// Poll running jobs and push progress/status back onto matching clips.
// Call once per frame from the main thread.
void bg_remove_poll(AppState& state);

// Cancel all running jobs and clean up.
void bg_remove_cancel_all();

// Run rembg synchronously on the original video at full resolution.
// Used at export time (blocking — call from render thread only).
// Returns true on success.  Skips if fps.txt already present in output_dir.
bool bg_remove_run_hires(const std::string& video_path,
                          const std::string& output_dir,
                          const std::string& python_path,
                          const std::string& rembg_script);

// Deterministic hires mask dir for a video path (used at export time).
std::string bg_remove_hires_dir(const std::string& video_path);

// Deterministic proxy mask dir for a video path (used at preview time).
std::string bg_remove_proxy_dir(const std::string& video_path);

// Read fps.txt written by the script; returns 30.0 on failure.
float bg_remove_read_fps(const std::string& mask_dir);
