#pragma once
#include "app.h"
#include <string>

// Generate a Blender Python script that, when run inside Blender with the
// lyric-video-blender addon enabled, recreates the current lyric session
// as a fully animated scene using the addon's operators.
bool blender_export_script(const AppState& state, const std::string& out_path);
