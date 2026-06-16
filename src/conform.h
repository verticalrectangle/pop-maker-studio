#pragma once
#include <string>

// ── Frame-rate conform ────────────────────────────────────────────────────────
//
// When a clip's native frame rate differs from the project fps, the preview
// proxy (a 30 fps dup/drop transcode) and the export (native-fps nearest-frame
// sampling) can land on different source frames, and 24/60 fps content judders.
//
// A conform transcodes the source ONCE into a project-fps copy with temporal
// blending (ffmpeg `framerate` filter), so both the proxy and the export derive
// from the same project-fps frames — preview == export, judder smoothed. The
// caller swaps the clip's decode source to the conformed file once it's ready.
//
// `smooth`  : true → blend adjacent frames (smooth motion); false → dup/drop
//             (keep the original cadence, still rate-aligned for consistency).
// `loop`    : true → conform cyclically (loop the source, blend the seam, keep
//             one fully-surrounded loop) so a perfect-loop GIF stays seamless
//             AND frame-aligned.
//
// Cached next to the source, deterministic by fps + flags. Background worker,
// modelled on the proxy pipeline.

// Native fps of `src` differs from `project_fps` by more than ~1% → worth a
// conform. Probes the source. Returns false for stills / unreadable files.
bool conform_needed(const std::string& src, int project_fps);

// Deterministic output path (encodes fps + flags so settings never collide).
std::string conform_path(const std::string& src, int project_fps, bool smooth, bool loop);

// Start a background conform if the file isn't already present or in flight.
// No-op if the conformed file already exists.
void conform_start(const std::string& src, int project_fps, bool smooth, bool loop);

// True once the conformed file is on disk and complete.
bool conform_is_ready(const std::string& src, int project_fps, bool smooth, bool loop);

enum class ConformState { Ready, Working, Queued, Idle };
struct ConformStatus { ConformState state = ConformState::Idle; float progress = 0.f; };
ConformStatus conform_status(const std::string& src, int project_fps, bool smooth, bool loop);

// Kill any running/queued conform jobs (app shutdown).
void conform_cancel();
