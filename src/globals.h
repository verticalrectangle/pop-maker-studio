#pragma once
#include <string>
#include <imgui.h>

// Path from GLFW drop callback
extern std::string g_dropped_file;
// Screen position of the cursor at the time of the last OS file drop
extern ImVec2      g_drop_pos;
// ~/.local/share/pop-maker-studio
extern std::string g_managed_dir;

// Returns true if the whisper ggml model is present.
bool models_detect();
