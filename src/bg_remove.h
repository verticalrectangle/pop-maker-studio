#pragma once
#include "app.h"
#include <string>

// Spawn background ONNX rembg job on the clip's MJPEG proxy.
// Updates clip.bg_remove_status / mask_dir immediately; progress via bg_remove_poll.
void bg_remove_start(AppState& state, int track_idx, int clip_idx);

// Poll running jobs and push progress/status back onto matching clips.
// Call once per frame from the main thread.
void bg_remove_poll(AppState& state);

// Cancel all running jobs and clean up.
void bg_remove_cancel_all();

// Run rembg synchronously on the original video at full resolution.
// Used at export time (blocking — call from render thread only).
// Returns true on success.  Skips if fps.txt already present in output_dir.
bool bg_remove_run_hires(const std::string& video_path,
                          const std::string& output_dir);

// Deterministic hires mask dir for a video path (used at export time).
std::string bg_remove_hires_dir(const std::string& video_path);

// Deterministic proxy mask dir for a video path (used at preview time).
std::string bg_remove_proxy_dir(const std::string& video_path);

// Read fps.txt written by the script; returns 30.0 on failure.
float bg_remove_read_fps(const std::string& mask_dir);

// Read start_frame.txt; returns 0 on failure.
int bg_remove_read_start_frame(const std::string& mask_dir);

// Return number of mask frames stored in bg_masks.idx (builds .idx if missing).
// Returns 0 if no masks exist.
int bg_remove_read_frame_count(const std::string& mask_dir);

// ── u2net model management ────────────────────────────────────────────────────

// Returns true if the u2net ONNX model is present on disk.
// Cached after first check; call rembg_install_reset() to force re-check.
bool rembg_is_installed(const std::string& python_path = "");
void rembg_install_reset();

enum class RembgInstallStatus { Idle, Running, Done, Failed };
void              rembg_install_start(const std::string& python_path = "");
RembgInstallStatus rembg_install_status();
std::string       rembg_install_error();
