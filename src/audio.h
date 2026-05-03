#pragma once
#include <string>
#include <cstdint>

void  audio_init();
void  audio_shutdown();
bool  audio_load(const std::string& path);  // load audio file for playback
void  audio_play();
void  audio_pause();
void  audio_seek(float seconds);
float audio_duration();
float audio_position();
bool  audio_is_playing();

// Decode audio file metadata without full load
struct AudioMeta {
    float    duration_secs = 0.f;
    int      sample_rate   = 0;
    int      channels      = 0;
    uint64_t size_bytes    = 0;
};
bool audio_probe(const std::string& path, AudioMeta& meta);
