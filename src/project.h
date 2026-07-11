#pragma once
#include "app.h"
#include <string>

bool project_save(const AppState& state, const std::string& path);
// Where the home screen's card thumbnail for this .pms lives
// (~/.config/pop-maker-studio/thumbs/<hash>.png). Stable per path.
std::string project_thumb_path(const std::string& pms_path);
bool project_load(AppState& state, const std::string& path);

// Collect: copy every referenced media original (clip sources, LUTs, master
// audio, bin) into a `media/` folder beside `pms_path`, rewrite the project's
// references to the copies, and save the .pms there — producing a self-contained,
// backup-able project folder. Mutates `state` (paths now point at the copies).
// Returns false (and sets `err`) on failure. `copied` gets the file count.
bool collect_project(AppState& state, const std::string& pms_path,
                     std::string& err, int* copied = nullptr);
