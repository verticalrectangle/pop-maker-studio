#pragma once
#include <string>
#include <cstdint>
#include "proxy.h"

// Video preview — two separate paths:
//
// PREVIEW (proxy-based, stb_image, main thread only):
//   video_open_still()  — show a single JPEG while proxy is generating
//   video_open_proxy()  — open MJPEG + seek table; scrub is fseek + stb_image
//   video_get_texture() — decode current frame, upload to GL, return texture ID
//   video_close()
//
// EXPORT (FFmpeg, original file, frame-accurate):
//   video_open_export()         — open original for decode
//   video_decode_frame_at()     — decode exact frame at timestamp (blocks)
//   video_close_export()
//   VideoFrame                  — raw RGBA pixel data returned by decode_frame_at

struct VideoFrame {
    uint8_t* data   = nullptr;  // RGBA, width*height*4 bytes — caller must av_free()
    int      width  = 0;
    int      height = 0;
    double   pts    = 0.0;
};

struct VideoInfo {
    double duration  = 0.0;
    int    width     = 0;
    int    height    = 0;
    double fps       = 0.0;
    bool   has_audio = false;
};

// ── Preview path ──────────────────────────────────────────────────────────────

// Show a static JPEG until the proxy is ready.
void video_open_still(const std::string& jpeg_path);

// Open a proxy for interactive scrubbing.
bool video_open_proxy(const ProxyInfo& proxy);

// Close whichever preview is open.
void video_close();

bool      video_is_open();
VideoInfo video_info();    // from the most recently opened proxy or export context

// Upload the frame nearest to `playhead` to a GL texture.
// Re-decodes only when the frame index changes.  Returns 0 if nothing is open.
uintptr_t video_get_texture(double playhead);

// Decode the frame at `t` into a separate thumbnail texture (never clobbers the
// main preview texture).  out_w / out_h receive the proxy pixel dimensions.
uintptr_t video_get_thumbnail(double t, int* out_w, int* out_h);

// Probe original video container for duration without full stream scan.
// Reads container header only — safe to call on the main thread, < 100 ms.
float video_probe_duration(const std::string& path);

// ── Export path ───────────────────────────────────────────────────────────────

bool  video_open_export(const std::string& path);
void  video_close_export();

// Frame-accurate single-frame decode.  Caller must av_free(result->data)
// and delete result when done.
VideoFrame* video_decode_frame_at(double seconds);
