#pragma once
// Typography-as-Script-clips (replaces the native preset system, removed in
// .pms v71): a typography layer is ONE Script clip whose script_path is
// `pms:typography/<id>` and whose script_params carry the per-layer Tune
// overrides. Declared here (engine-side) so IPC, the pipeline completion
// callback and the UI share one implementation.
#include <string>

struct AppState;

// Lay (or replace) the typography Script clip for the current transcript:
// spans the words' time range mapped to timeline seconds. Returns false +
// err when there are no words or the preset id is unknown.
bool lay_typography_script(AppState& state, const std::string& preset,
                           const std::string& params_json, std::string& err);

// Preset id currently on the typography Script clip ("" when none).
std::string active_typography_preset(const AppState& state);
