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

// Extract one frame per requested timestamp (seek + decode). Saves JPEGs to
// the same {video_path}.pms_frames/ directory. Times past the end of the
// video are skipped; the result keeps the caller's order.
std::vector<KeyFrame> extract_frames_at(const std::string& video_path,
                                        const std::vector<float>& times);
