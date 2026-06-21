#pragma once
#include "../app.h"

// Terminal-like chat panel for the in-app agent (see AGENT_HARNESS.md).
void draw_agent_log(AppState& state, float panel_w, float panel_h);
void draw_agent_input(AppState& state, float panel_w);

// True when the agent input held keyboard focus last frame — the studio/terminal
// OS-drop handlers stand down so dropped files stage into the agent chat instead.
bool agent_input_is_focused();
// Height of the input zone, grown to fit the drop-staging chip tray when present.
float agent_input_height();
// Drop-target border accent strength (0..1): steady while focused, flashes on drop.
float agent_drop_highlight();
