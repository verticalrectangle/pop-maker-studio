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

// ── Mic capture (record brick) ────────────────────────────────────────────────
// Separate miniaudio capture device: interleaved stereo f32 @ 44100, same
// format the mixer uses, so takes feed straight back into the clip system.
bool  audio_capture_start();
void  audio_capture_stop();
bool  audio_capture_active();
// Move all captured samples since the last drain into `out` (appends).
void  audio_capture_drain(std::vector<float>& out);
float audio_capture_latency();  // input period in seconds (0 if not capturing)

// Capture device picker. Index -1 = system default. The list refreshes on
// each devices() call; selection applies on the next audio_capture_start.
std::vector<std::string> audio_capture_devices();
void audio_capture_select(int index);
int  audio_capture_selected();

// Input monitoring: route live mic into the playback mix (capture must be
// running). Round-trip latency = input + output period — fine for VO, audible
// for tight singing, hence default off.
void audio_monitor_set(bool on);
bool audio_monitor_get();

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

struct AudioClipDesc {
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
