#pragma once
// Loop recorder for ClipType::VideoRecord bricks — the video twin of
// recorder.h. The transport loops over the brick's range, an ffmpeg child
// streams MJPEG frames from the camera, and every pass over the loop becomes
// one take (an MJPEG AVI in the managed takes dir, appended to rec_takes).
// The newest take is auto-selected so the brick plays it back immediately.
//
// Takes are stream-copied (no re-encode): JPEG frames are bucketed by their
// arrival time mapped onto the loop clock, and each take is muxed with
// framerate = frames/loop_len, so its duration is exactly the loop length.
//
// Capture source: /dev/video0 (V4L2) when present; otherwise an ffmpeg
// lavfi test pattern so the whole pipeline works on camera-less machines.
#include "app.h"

bool vrecorder_start(AppState& state, int ti, int ci);
void vrecorder_stop(AppState& state, bool keep_partial = true);
void vrecorder_tick(AppState& state);            // call once per UI frame
bool vrecorder_active();
bool vrecorder_is_target(int ti, int ci);
int  vrecorder_take_count();
bool vrecorder_using_test_pattern();             // no camera found

// Latest captured JPEG frame (for the live preview); returns false when
// nothing has arrived yet. UI-thread only (same thread as the tick).
bool vrecorder_latest_jpeg(std::vector<uint8_t>& out);
