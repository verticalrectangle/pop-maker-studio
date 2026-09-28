#pragma once
// Per-clip Script compositing (docs/SCRIPT_API.md): owns one ScriptRuntime +
// ScriptSurface per timeline Script clip (key "track:clip"), renders frames
// on the GL thread, skips render when nothing changed (§9), and hands the
// compositors a layer texture: canvas-sized, straight alpha, row 0 = top.
//
// Errors never throw: a failing clip renders a red error card and keeps its
// errors for get_script_errors; export paths must treat a non-empty error
// list as fatal.
#include "script_runtime.h"  // ScriptError

#include <string>
#include <vector>

struct AppState;
struct Clip;

// "track:clip" — the key used by every entry point below.
std::string script_clip_key(int track, int clip);

// Layer texture for `clip` at timeline time `t` (0 = GL/Skia unavailable).
// The script sees a canvas_w×canvas_h canvas (the project/export size,
// f.width×f.height); the texture is surface_w×surface_h (smaller in the
// preview — Skia rasterises at that size). `exporting` blocks on requested
// face tracks before rendering and sets f.exporting. This call's errors are
// appended to `errors`.
unsigned script_clip_texture(const AppState& state, const Clip& clip, const std::string& key,
                             float t, int canvas_w, int canvas_h, int surface_w, int surface_h,
                             bool exporting, std::vector<ScriptError>& errors);

// Drop state for Script clips no longer on the timeline (call once per frame).
void script_clip_gc(const AppState& state);

// Force a rebuild on the next render (path/params edited).
void script_clip_invalidate(const std::string& key);

struct ScriptClipReport {
    std::string key;
    std::vector<ScriptError> errors;
    std::vector<std::string> log;  // pms.log / console.* tail
    double render_ms = 0.0;        // last render: JS + Skia recording (CPU)
    double flush_ms = 0.0;         // last render: Skia flush + post/unpremultiply submit (CPU)
    int builds = 0;                // runtime (re)builds so far: 1 + hot reloads / edits
};

// One report per Script clip on the timeline that has been rendered.
std::vector<ScriptClipReport> script_clip_reports(const AppState& state);
