#pragma once
#include "app.h"
#include <string>

void draw_terminal_panel(AppState& state, float panel_w, float panel_h);
void terminal_panel_shutdown();
void terminal_inject_path(const std::string& path);
bool terminal_is_focused();
