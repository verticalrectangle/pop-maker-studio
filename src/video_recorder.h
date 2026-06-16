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

#include <vector>
#include <string>

bool vrecorder_start(AppState& state, int ti, int ci);  // async: warms up, then records
void vrecorder_stop(AppState& state, bool keep_partial = true);
void vrecorder_tick(AppState& state);            // call once per UI frame
void vrecorder_shutdown();                        // kill capture child on app exit (no orphan ffmpeg)
bool vrecorder_active();                         // warming or recording
bool vrecorder_recording();                      // takes are being sliced
bool vrecorder_warming();                        // camera starting, transport not yet running
bool vrecorder_is_target(int ti, int ci);
int  vrecorder_take_count();
bool vrecorder_using_test_pattern();             // no camera found
std::string vrecorder_error();                   // last failure, cleared on next start

// Camera monitor: run the capture child without recording so the live
// mirror shows the camera (the audio brick's 'Monitor input' equivalent).
void vrecorder_monitor_set(bool on);
bool vrecorder_monitor_get();

// Capture IMG: grab the current live frame as a single still and add it to the
// brick at (ti,ci) as a .jpg take. Returns true if a photo was written; returns
// false (and starts the monitor) when no live frame is available yet — the next
// call captures once the preview has warmed up.
bool vrecorder_capture_photo(AppState& state, int ti, int ci);

// Capture devices (V4L2 video-capture nodes + their card names).
struct VCamDevice { std::string path, name; };
std::vector<VCamDevice> vrecorder_devices();
void vrecorder_device_select(int idx);           // -1 = auto (first device)
int  vrecorder_device_selected();

// Best-guess index (into audio_capture_devices()) of the selected camera's own
// built-in mic, matched by device name, or -1 if there's no confident match.
// Caller should cache — this re-enumerates V4L2 + audio devices.
int  vrecorder_camera_mic(int cam_sel);

// Latest captured JPEG frame + a serial that bumps per frame (for the live
// mirror's texture upload). UI-thread only (same thread as the tick).
bool     vrecorder_latest_jpeg(std::vector<uint8_t>& out);
uint64_t vrecorder_frame_serial();
