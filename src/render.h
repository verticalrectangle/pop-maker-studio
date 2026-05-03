#pragma once
#include "app.h"
#include <string>

void render_start(AppState& state);
void render_cancel();
bool render_export_srt(const AppState& state, const std::string& out_path);
