#pragma once
// Script clip runtime (docs/SCRIPT_API.md): QuickJS-ng host per Script clip.
// One ScriptRuntime per clip: isolated JSRuntime+JSContext, ES module loader
// with file watching, the `pms` global (§3) with the Context2D from
// script_context2d.h, purity guards, and the per-frame render entry used by
// script_clip.cpp.
//
// Threading: main/GL thread only (preview, export tick, snapshot).
#include "script_gpu.h"  // ScriptUniforms

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct AppState;
struct Clip;
class SkCanvas;

struct ScriptError {
    std::string message;   // exception text (+ stack) / compile error
    std::string file;      // module path ("" = runtime-level)
    int line = 0;          // 1-based; 0 = unknown
};

struct ScriptFrame {
    double t = 0.0;        // timeline seconds
    double local = 0.0;    // seconds since clip start
    int frame = 0;         // round(t * fps)
    double fps = 30.0;
    int width = 0, height = 0;  // canvas px for this render
    double duration = 0.0;
    bool exporting = false;
};

class ScriptRuntime {
public:
    ScriptRuntime();
    ~ScriptRuntime();

    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    // (Re)build from the clip's entry module and run setup({width, height,
    // fps, params}). Destroys any previous runtime. Returns false + errors
    // when the entry is missing, evaluation throws, or render is not
    // exported.
    bool build(const AppState& state, const Clip& clip, int width, int height,
               std::vector<ScriptError>& errors);

    // Run render(f) against `canvas` (cleared; scaled from the logical
    // f.width×f.height to the canvas' own size). Returns false + errors when
    // it throws or queues promise jobs.
    bool render(const AppState& state, const ScriptFrame& f, SkCanvas* canvas,
                std::vector<ScriptError>& errors);

    // True when any watched file (modules, pms.json/image/font files) changed.
    bool poll_dirty() const;

    // Script log (pms.log / console.*), last 200 lines.
    const std::vector<std::string>& log_tail() const;
    // Post shader requested by the last render ("" = none) + its uniforms.
    const std::string& post_frag() const;
    const ScriptUniforms& post_uniforms() const;
    // Face tracks requested via pms.face(path) during the last render.
    const std::vector<std::string>& face_requests() const;

    struct Impl;

private:
    void teardown();
    std::unique_ptr<Impl> impl_;
};

// Resolve `spec` relative to `referrer` (file path) or the entry dir.
// Returns "" when unresolvable. Built-ins (pms:rhythm, pms:text,
// pms:typography/*) resolve to the assets/scripts/ path.
std::string script_resolve_spec(const std::string& spec, const std::string& referrer,
                                const std::string& entry_dir);

// Identity of the published audio analysis (changes on every
// load_audio_analysis / analyze_audio publish). The runtime memoises the
// frozen pms.audio objects against this + the mapped clip offset.
uint64_t script_audio_epoch(const AppState& state);
// Identity of the project transcript behind pms.words fallback (changes when
// the words, their timings, the lyrics edits or the audio source change).
uint64_t script_words_epoch(const AppState& state);
