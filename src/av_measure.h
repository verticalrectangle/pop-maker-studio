#pragma once
// A/V offset measurement for the Video Record brick. Asynchronously records a
// few seconds of TWO mics at once — the clean mic the take keeps, and the
// camera's own scratch mic, which rides the camera pipeline and so carries the
// same latency as the video frames — then runs GCC-PHAT (av_sync.h) to estimate
// how far the clean audio leads or lags the video. The result drops straight
// into a clip's rec_av_offset_ms so a separately-mic'd take lip-syncs without a
// clapperboard.
//
// Both mics are captured from ONE ffmpeg process so they share a start instant
// (the same trick video_recorder.cpp uses for cam+mic). The run is non-blocking:
// start it, poll once per UI frame, apply the result when it lands.
#include <string>

struct AVMeasureResult {
    bool        ok = false;
    float       offset_ms   = 0.f;  // → clip.rec_av_offset_ms (+ delays the audio)
    float       confidence  = 0.f;  // 0..1 peak prominence; < ~0.5 is shaky
    std::string error;              // populated when ok == false
};

// Begin a measurement: capture_s seconds of clean_pulse_src + cam_pulse_src
// (PulseAudio source names, as from audio_capture_pulse_source). Returns false
// if a measurement is already running or the sources are empty. Non-blocking —
// the capture + analysis run on a background thread.
bool av_measure_start(const std::string& clean_pulse_src,
                      const std::string& cam_pulse_src,
                      float capture_s);

bool av_measure_active();   // capture or analysis in progress

// True once when a started measurement has finished; fills `out` and clears the
// done flag (so a subsequent poll returns false until the next run). Call once
// per frame from the UI thread.
bool av_measure_poll(AVMeasureResult& out);

// Seconds elapsed in the current capture (0 when idle) — for a progress readout.
float av_measure_elapsed();

void av_measure_shutdown();  // join/abandon a running measurement on app exit
