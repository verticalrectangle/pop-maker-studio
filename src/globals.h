#pragma once
#include <string>
#include <cstddef>

// Globals set by GLFW callbacks in main.cpp, read by UI screens
extern std::string g_dropped_file;     // path from GLFW drop callback
extern std::string g_pipeline_script;  // path to ml_pipeline.py  (extracted from binary)
extern std::string g_prefetch_script;  // path to ml_prefetch.py  (extracted from binary)
extern std::string g_voice_convert_script;  // path to voice_convert.py (extracted from binary)
extern std::string g_managed_dir;      // ~/.local/share/pop-maker-studio

// Returns true if faster-whisper and Demucs weights are present on disk.
bool models_detect();
