#pragma once
#include "audio_fx.h"
#include "keyframe.h"
#include <string>
#include <vector>
#include <cstdint>

void  audio_init();
void  audio_shutdown();
bool  audio_load(const std::string& path);  // async — probe+enqueue decode in background
bool  audio_loading();                       // true while background decode is in progress
void  audio_play();
void  audio_pause();
void  audio_seek(float seconds);
void  audio_set_volume(float v);  // main-buffer gain (0–2)
float audio_duration();
float audio_position();
float audio_latency();
bool  audio_is_playing();

// ── Loop region (cycle playback for the record brick) ─────────────────────────
// While set, the master clock wraps from end back to start; audio_position()
// (and therefore the playhead and video preview) follows automatically.
void     audio_set_loop(float start_sec, float end_sec);
void     audio_clear_loop();
bool     audio_loop_active();
uint64_t audio_loop_cycles();  // increments once per wrap — recorder slices takes on this clock

// ── Mic capture (record brick) — performance mode ─────────────────────────────
// Opening the mic swaps the playback-only device for ONE duplex device with
// small periods (~128 frames): the same callback renders the timeline mix,
// sums the gated live input straight into the output (zero-buffer monitor
// path, ~6 ms round trip), and feeds the raw input to the capture ring.
// Interleaved stereo f32 @ 44100 throughout, so takes feed straight back
// into the clip system. The master clock is shared — playback continues
// seamlessly across the device swap.
bool  audio_capture_start();    // enter performance mode
void  audio_capture_stop();     // leave performance mode
bool  audio_capture_active();
float audio_input_peak();       // live mic peak 0–1 (0 when not capturing)

// Master output meter (BS.1770): momentary + integrated LUFS and sample peak
// of the final device buffer. Integrated resets each time playback starts.
#include "loudness.h"
LoudnessSnapshot audio_master_meter();
// Move all captured samples since the last drain into `out` (appends).
void  audio_capture_drain(std::vector<float>& out);
float audio_capture_latency();  // input period in seconds (0 if not capturing)

// Capture device picker. Index -1 = system default. The list refreshes on
// each devices() call; selection applies on the next audio_capture_start.
std::vector<std::string> audio_capture_devices();
void audio_capture_select(int index);
int  audio_capture_selected();
// PulseAudio/PipeWire source name for a capture-device index (from the last
// audio_capture_devices() enumeration), for feeding ffmpeg `-f pulse -i <name>`.
// Empty string for index < 0 (system default) or an unknown index.
std::string audio_capture_pulse_source(int index);

// Input monitoring: sum the live mic into the output inside the duplex
// callback. Latency = one input + one output period of the duplex device.
// Enters performance mode on demand.
void audio_monitor_set(bool on);
bool audio_monitor_get();

// Monitor noise gate (silvertune companion port): smoothed gate on the
// monitored signal. The recorded stream stays raw unless bake is on.
void audio_gate_set(bool on);
bool audio_gate_get();
void audio_gate_bake_set(bool on);  // also gate the recorded takes
bool audio_gate_bake_get();

// "Hear effects": run the record brick's audio FX chain on the MONITOR
// signal only — takes record dry, playback re-applies the same chain, so
// what you sing against is what the take becomes. The chain is built on the
// UI thread (audio_monitor_chain_set; empty clears) and processed per
// sample in the duplex input path.
void audio_monitor_fx_set(bool on);
bool audio_monitor_fx_get();

// Virtual microphone (PipeWire only): expose the PROCESSED record-brick
// signal (gate + noise reduction + live FX) as a system input device —
// "Pop Maker Studio Mic" in every app's mic picker. Keeps the capture
// device running while enabled, independent of local monitoring.
bool audio_vmic_set(bool on);   // false = start failed (no PipeWire)
bool audio_vmic_get();
void audio_monitor_chain_set(const std::vector<AudioFX>& stages);
// Windowed monitor chain: while the transport rolls, the live mic runs through
// these source-time FX windows via process_seg (matching playback/export
// exactly, edge crossfades included). brick_start = the host record brick's
// timeline start (seconds), to map the master clock into source time. Empty
// segs clears. Used while playing/recording; audio_monitor_chain_set is the
// parked/dial-in path.
void audio_monitor_chain_set_seg(const std::vector<AudioFXSegment>& segs,
                                 float brick_start);
bool audio_monitor_chain_active();

// Perf-mode health: true while the duplex device owns audio; xruns counts
// suspicious callback gaps since capture start (any nonzero = audible risk).
bool     audio_perf_mode();
uint32_t audio_perf_xruns();

// ── Processed-audio lookups (export bake) ─────────────────────────────────────
// Copy out the preview system's buffers so the exporter can bake audio FX
// without re-decoding/re-processing: raw decoded PCM for `path`, or the
// FX-processed buffer for (path, fx_hash). false = not cached/ready.
bool audio_source_cached(const std::string& path, std::vector<float>& out);
bool audio_fx_cached(const std::string& path, uint64_t fx_hash, std::vector<float>& out);

// Decode audio file metadata without full load
struct AudioMeta {
    float    duration_secs = 0.f;
    int      sample_rate   = 0;
    int      channels      = 0;
    uint64_t size_bytes    = 0;
};
bool audio_probe(const std::string& path, AudioMeta& meta);

// ── Clip-based audio ──────────────────────────────────────────────────────────

// Bus brick snapshot — one per ClipType::Bus clip. Submixes the audio of every
// track BELOW `track` (down to the next bus brick), gated by [start,end], then
// applies `stages` + `gain`. Stages flattened from the brick's fx_chain entries.
struct AudioBusBrick {
    int      track   = 0;    // the brick's track index (groups tracks > this)
    float    start   = 0.f;  // span on the timeline (seconds)
    float    end      = 0.f;
    std::vector<AudioFX> stages;
    float    gain    = 1.f;
    uint64_t hash    = 0;    // change detector for the live chain registry
};

// Push the bus-brick set — called every frame like clips.
void audio_bus_bricks_update(const std::vector<AudioBusBrick>& bricks);

struct AudioClipDesc {
    int         track     = 0;    // routing: the clip's track index (for bus-brick lookup)
    float       tl_start  = 0.f;  // clip start on timeline (seconds)
    float       tl_end    = 0.f;  // clip end on timeline
    float       in_point  = 0.f;  // source offset at tl_start
    float       speed     = 1.f;
    float       volume    = 1.f;
    float       pan       = 0.f;  // -1=L, 0=center, +1=R
    float       fade_in   = 0.f;
    float       fade_out  = 0.f;
    PropTrack   vol_keys;         // volume keyframes (times rel. tl_start); empty = use volume
    PropTrack   pan_keys;         // pan keyframes; empty = use pan
    std::string path;
    std::vector<AudioFXSegment> fx_segs;  // windowed FX; empty = dry
    uint64_t    fx_hash   = 0;            // audio_fx_segments_hash; 0 = no FX
};

// Kick off async PCM decode for a source file so it's ready when needed.
void audio_source_ensure(const std::string& path);

// Push a fresh snapshot of all Audio clips — called every frame.
void audio_clips_update(const std::vector<AudioClipDesc>& clips);

// Push a fresh snapshot of all Video clips (for embedded audio) — called every frame.
void video_audio_clips_update(const std::vector<AudioClipDesc>& clips);

// Free all per-clip source buffers (call on project close / new project).
void audio_clips_clear();
