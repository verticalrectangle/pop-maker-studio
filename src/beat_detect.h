#pragma once
#include <string>
#include <vector>

struct BeatResult {
    std::string    source_id;
    float          bpm      = 0.f;
    float          duration = 0.f;
    std::vector<float> beats; // beat timestamps in seconds
    std::vector<float> rms;   // per-second RMS energy, normalised 0–1
    bool           ok    = false;
};

// Decode audio from path and run aubio beat tracking.
// Blocking — call from a background thread.
BeatResult beat_detect(const std::string& path);
