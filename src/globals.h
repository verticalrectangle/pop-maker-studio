#pragma once
#include <string>

// Path from GLFW drop callback
extern std::string g_dropped_file;
// ~/.local/share/pop-maker-studio
extern std::string g_managed_dir;

// Returns true if the whisper ggml model is present.
bool models_detect();
