#pragma once
#include <string>
#include <vector>

struct KeyFrame {
    float       timestamp;
    std::string jpeg_path;
};

// Extract up to max_frames keyframes from video using scene-change detection.
// Saves JPEGs to {video_path}.pms_frames/ directory.
// Sets *capped=true if natural scene count exceeded max_frames (even sampling applied).
// Returns empty vector on failure.
std::vector<KeyFrame> extract_keyframes(const std::string& video_path,
                                        int   max_frames = 60,
                                        bool* capped     = nullptr);
