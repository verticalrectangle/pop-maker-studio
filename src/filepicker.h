#pragma once
#include <string>

// Open a native OS file picker dialog.
// Returns the selected path, or empty string if cancelled.
// filter_name: human label e.g. "Audio & Video"
// filter_patterns: space-separated globs e.g. "*.wav *.mp3 *.mp4"
std::string filepicker_open(const char* title,
                             const char* filter_name,
                             const char* filter_patterns);
