#pragma once
#include <string>
#include <vector>

// Path from GLFW drop callback
extern std::string g_dropped_file;
// All paths from the most recent OS drop this frame (single or multi), for
// focus-aware consumers like the agent chat that stage drops as chips. Filled
// by the drop callback, consumed during app_frame, cleared at end of frame.
extern std::vector<std::string> g_drop_batch;
// ~/.local/share/pop-maker-studio
extern std::string g_managed_dir;

// Returns true if the whisper ggml model is present.
bool models_detect();
