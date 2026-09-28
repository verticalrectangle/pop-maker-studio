#pragma once
// Per-clip Script compositing (SCRIPT_API.md): owns the ScriptRuntime +
// ScriptCanvas for one timeline clip, renders frames on the main/GL thread,
// caches the last frame per §9, and exposes the GL texture for the preview /
// export / snapshot compositors.
//
// All methods run on the main/GL thread except script_clip_poll() (stat-only,
// cheap) which the app may call from anywhere. Export calls render_export()
// which blocks on face tracks (face_cache_ensure_sync).
#include <string>
#include <vector>

struct AppState;
struct Clip;
struct ScriptError;

// Opaque per-clip state, keyed by clip identity. Managed by script_clip_*.
struct ScriptClipState;

// Render (or reuse the cached) frame for this clip at timeline time `t`.
// Returns the GL texture (0 = nothing to composite: error card still returns
// its texture). `canvas_w/h` = project canvas px. `exporting` selects the
// blocking face-track path + sets f.exporting. Errors appended to `errors`
// (capped); the error card is rendered into the texture on failure.
unsigned script_clip_texture(AppState& state, const Clip& clip,
                             const std::string& clip_key,
                             float t, int canvas_w, int canvas_h,
                             bool exporting, std::vector<ScriptError>& errors);

// Drop cached state for clips no longer on the timeline (call per frame with
// the live key set).
void script_clip_gc(const std::vector<std::string>& live_keys);

// Invalidate one clip (path/params changed via set_script_clip).
void script_clip_invalidate(const std::string& clip_key);

// Poll file watchers: true if any live clip's modules/data changed.
bool script_clip_poll();

// Last-frame render time (ms) for the perf readout, "" when unknown.
std::string script_clip_perf(const std::string& clip_key);

// Errors for get_script_errors {clip?}: per-clip list, or all clips.
std::vector<std::pair<std::string, std::vector<ScriptError>>> script_clip_errors(
    const AppState& state);
