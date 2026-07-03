#pragma once
// Native PipeWire backend for performance mode (low-latency monitor/record).
//
// The pulse shim quantizes miniaudio's 128-frame request up to ~236/384
// frames per direction (~14 ms round trip). A native pw_stream pair gets the
// real quantum: both streams tick the SAME graph clock (constant phase, no
// drift — the thing the old two-device path could never have), and the
// stream adapter resamples app-side 44.1 kHz to the graph rate with its
// proper resampler, so the engine stays 44.1 k end to end.
//
// Callbacks run on PipeWire's RT data thread — same rules as a miniaudio
// callback: no locks, no allocations.
#ifdef HAVE_PIPEWIRE
#include <cstdint>

using PwInProcess  = void (*)(const float* in, uint32_t frames);  // interleaved stereo
using PwOutProcess = void (*)(float* out, uint32_t frames);       // fill fully

// Connect the pair. capture_node = PipeWire node name to record from
// (miniaudio's pulse device id works verbatim), or null for the default
// source. Returns false if PipeWire is unreachable or the streams fail to
// reach STREAMING within the timeout — caller falls back to miniaudio.
bool  audio_pw_start(PwInProcess in_cb, PwOutProcess out_cb, const char* capture_node);
void  audio_pw_stop();
bool  audio_pw_active();
float audio_pw_period_s();   // achieved frames-per-cycle / 44100 (one direction)

// ── Virtual microphone ────────────────────────────────────────────────────────
// A PipeWire node with media.class Audio/Source: every app's input-device
// list shows "Pop Maker Studio Mic". Feed it the PROCESSED record-brick
// monitor signal (gate + noise reduction + live FX chain) and calls hear you
// with the studio chain. Independent lifecycle from the perf-mode pair — it
// works with either audio backend. Writer side is audio-thread safe
// (lock-free ring; underruns emit silence).
bool audio_pw_vmic_start();
void audio_pw_vmic_stop();
bool audio_pw_vmic_active();
void audio_pw_vmic_push(const float* interleaved_lr, uint32_t frames);
#endif
