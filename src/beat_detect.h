#pragma once
#include <string>
#include <vector>

struct BeatResult {
    std::string    source_id;
    float          bpm   = 0.f;
    std::vector<float> beats; // beat timestamps in seconds
    bool           ok    = false;
};

// Decode audio from path and run aubio beat tracking.
// Blocking — call from a background thread.
BeatResult beat_detect(const std::string& path);
