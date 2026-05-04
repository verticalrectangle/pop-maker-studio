#pragma once
#include <string>
#include <vector>

struct WaveformData {
    std::vector<float> samples;  // normalized RMS, WAVEFORM_FPS samples/sec
    bool ready   = false;
    bool loading = false;
};

static constexpr float WAVEFORM_FPS = 120.f;  // samples per second

// Returns cached waveform for path, or triggers async load and returns nullptr.
const WaveformData* waveform_get(const std::string& path);
void waveform_clear_cache();
