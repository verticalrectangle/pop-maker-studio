#pragma once
#include <string>

// Globals set by GLFW callbacks in main.cpp, read by UI screens
extern std::string g_dropped_file;     // path from GLFW drop callback
extern std::string g_pipeline_script;  // path to ml_pipeline.py  (extracted from binary)
extern std::string g_prefetch_script;  // path to ml_prefetch.py  (extracted from binary)

// Returns true if Whisper large-v2 and Demucs htdemucs weights exist in cache.
bool models_detect();
