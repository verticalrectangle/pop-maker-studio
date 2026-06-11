#pragma once
// Loop recorder for ClipType::Record bricks. Logic-style cycle recording:
// the transport loops over the brick's timeline range, the mic records
// continuously, and every pass over the loop becomes one take (a WAV in the
// managed takes dir, appended to the brick's rec_takes). The newest take is
// auto-selected so the brick plays it back immediately.
//
// Capture and playback are separate miniaudio devices, so takes are aligned
// to the loop grid by a fixed offset of output latency + input latency —
// the performer sings in time with what they hear, which is one output
// period late, and the mic's samples arrive one input period after that.
#include "app.h"

bool recorder_start(AppState& state, int ti, int ci);  // begin cycle record on a Record brick
void recorder_stop(AppState& state, bool keep_partial = true);
void recorder_tick(AppState& state);                   // call once per UI frame
bool recorder_active();
bool recorder_is_target(int ti, int ci);
int  recorder_take_count();   // takes completed since recorder_start
float recorder_input_level(); // smoothed mic peak 0–1 for the panel meter

// Duration in seconds of a WAV written by the recorder (header probe; 0 on error).
float recorder_wav_duration(const std::string& path);
