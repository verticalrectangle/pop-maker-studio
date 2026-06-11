#pragma once
#include "../app.h"

// Terminal-like chat panel for the in-app agent (see AGENT_HARNESS.md).
void draw_agent_log(AppState& state, float panel_w, float panel_h);
void draw_agent_input(AppState& state, float panel_w);
