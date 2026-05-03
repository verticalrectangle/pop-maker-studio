#pragma once
#include <string>
#include <cstdint>

// Video background layer — FFmpeg decode, OpenGL texture per-frame.
// Designed for preview (real-time) and export (frame-accurate).

struct VideoFrame {
    uint8_t* data   = nullptr;  // RGBA packed, width*height*4 bytes
    int      width  = 0;
    int      height = 0;
    double   pts    = 0.0;      // presentation timestamp in seconds
};

struct VideoInfo {
    double duration = 0.0;
    int    width    = 0;
    int    height   = 0;
    double fps      = 0.0;
    bool   has_audio = false;
};

// Open a video file for preview playback.
// Spawns a background decode thread; call video_get_frame() each render frame.
bool  video_open(const std::string& path);
void  video_close();
bool  video_is_open();
VideoInfo video_info();

// Seek to timestamp (seconds). Non-blocking — frame arrives next poll.
void  video_seek(double seconds);

// Returns the most recently decoded frame at or before `playhead`.
// Returns nullptr if no frame is ready yet.
// The returned pointer is valid until the next call to video_get_frame().
const VideoFrame* video_get_frame(double playhead);

// Upload the current frame to an OpenGL texture.
// Creates the texture on first call; reuses it thereafter.
// Returns the OpenGL texture ID (cast to uintptr_t for ImGui).
uintptr_t video_get_texture(double playhead);

// Frame-accurate decode for export: returns the frame at exactly `seconds`.
// Blocks until the frame is decoded. Caller frees VideoFrame->data.
VideoFrame* video_decode_frame_at(double seconds);
