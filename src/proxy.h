#pragma once
#include <string>
#include <cstdint>
#include <vector>

// Proxy generation and management.
//
// On video import, call proxy_start().  A background ffmpeg subprocess
// generates a full-resolution all-intra H.264 "intermediate" file — every
// frame is an independent keyframe (-g 1, no frame chains), CFR at the
// source's counted rate capped at 30 fps, rotation baked, no audio — plus a
// binary seek table (.interm.idx) alongside it. While that runs,
// proxy_still_path() provides an instant single-frame JPEG so the preview
// panel always shows something.
//
// Once proxy_is_ready() returns true, open it with video_open_intermediate()
// and scrubbing becomes: byte-seek to the frame's offset (each frame starts
// at a keyframe) + decode a single frame. No frame-chain chasing, no MJPEG.

struct ProxyInfo {
    std::string interm_path;       // full-res all-intra H.264 intermediate (.interm.mp4)
    std::string interm_idx_path;   // u32 frame count + u64 byte offset per frame
    std::string still_path;        // single JPEG shown while proxy is generating
    int         frame_count = 0;
    double      fps         = 0.0;   // derived from fps_num/fps_den for convenience
    int64_t     fps_num     = 0;     // exact rational numerator   (e.g. 30000)
    int64_t     fps_den     = 1;     // exact rational denominator (e.g. 1001)
    int         width       = 0;
    int         height      = 0;
    std::vector<uint64_t> offsets;  // byte offset per frame in the intermediate
                                    // (every frame starts at a keyframe)
};

// Start background proxy generation for `video_path`.
// Extracts a preview still immediately (blocking, < 1 s), then launches a
// background ffmpeg process to build the all-intra intermediate and its seek
// table.
// Safe to call multiple times — no-op if proxy already exists.
void proxy_start(const std::string& video_path);

// Returns true while a background generation subprocess is running.
bool proxy_is_generating();

// Returns true when the .interm.mp4 + .interm.idx files are on disk and valid.
bool proxy_is_ready(const std::string& video_path);

// Load the ProxyInfo (offsets, dimensions, fps) for an existing proxy.
// Returns false if the proxy files are missing or corrupt.
bool proxy_load(const std::string& video_path, ProxyInfo& out);

// Path helpers — deterministic, no state needed.
std::string proxy_interm_path(const std::string& video_path);
std::string proxy_interm_idx_path(const std::string& video_path);
std::string proxy_still_path(const std::string& video_path);

// Cancel any running background generation and drain the queue (called on shutdown).
void proxy_cancel();

// Generate a preview still for video_path without starting the intermediate
// proxy. Blocking (< 1 s). No-op if the still already exists on disk.
void proxy_ensure_still(const std::string& video_path);

// Per-file proxy status, returned by proxy_job_status() and proxy_status_all().
struct ProxyJobStatus {
    std::string path;
    enum class State { Ready, Generating, Queued, Idle, Failed } state = State::Idle;
    float progress = 0.f;  // 0–1, only meaningful when state == Generating
    std::string error;     // only meaningful when state == Failed
};

// Non-empty when proxy generation FAILED for this source (unreadable/corrupt
// file, transcode error). The failure is keyed to the source's size+mtime, so
// overwriting the file (re-export to the same path) clears it automatically.
std::string proxy_failure(const std::string& video_path);

// Status for a single path.
ProxyJobStatus proxy_job_status(const std::string& video_path);

// Batch status for a list of paths (same order as input).
std::vector<ProxyJobStatus> proxy_status_all(const std::vector<std::string>& paths);

// Number of paths waiting in the queue (not counting the active job).
int proxy_queue_depth();
