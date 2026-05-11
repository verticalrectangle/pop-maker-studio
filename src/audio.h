#pragma once
#include "audio_fx.h"
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
    std::string path;
    AudioFX     fx;
    uint64_t    fx_hash   = 0;    // audio_fx_hash(fx); 0 = no FX
};

// Kick off async PCM decode for a source file so it's ready when needed.
void audio_source_ensure(const std::string& path);

// Push a fresh snapshot of all Audio clips — called every frame.
void audio_clips_update(const std::vector<AudioClipDesc>& clips);

// Push a fresh snapshot of all Video clips (for embedded audio) — called every frame.
void video_audio_clips_update(const std::vector<AudioClipDesc>& clips);

// Free all per-clip source buffers (call on project close / new project).
void audio_clips_clear();
