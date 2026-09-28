#pragma once
// Script clip runtime (SCRIPT_API.md): QuickJS-ng host per Script clip.
// One ScriptRuntime per clip: isolated JSRuntime+JSContext, ES module loader
// with file watching, the `pms` global bindings (§3), purity guards, and the
// per-frame render entry used by script_clip.cpp.
//
// Threading: all entry points run on the main/GL thread (preview, export
// tick, snapshot). Hot-reload polling (script_runtime_poll) is cheap
// (stat mtimes) and also runs there. No worker threads.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct AppState;
struct Clip;

struct ScriptError {
    std::string message;   // exception text / compile error
    std::string file;      // module path ("" = runtime-level)
    int line = 0;
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

// Render callback: the host canvas. Set before render() via set_draw_hooks;
// invoked by the Context2D binding (src/script_canvas.*).
struct ScriptDrawHooks {
    std::function<void(const std::string& file)> ensure_font_loaded;
};

class ScriptRuntime {
public:
    ScriptRuntime();
    ~ScriptRuntime();

    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    // (Re)build from the clip's entry module. Destroys any previous runtime.
    // Returns false + errors when the entry file is missing or module
    // evaluation fails. `clip_key` identifies the clip for the script log.
    bool build(AppState& state, const Clip& clip, const std::string& clip_key,
               std::vector<ScriptError>& errors);

    // Render one frame. Returns false + errors on exception; on success the
    // Context2D calls issued by render(f) are in the canvas (script_canvas).
    // `audio_epoch` = generation counter of AppState::audio_analysis; the
    // frozen pms.audio/words/lines objects are rebuilt only when it changes.
    bool render(AppState& state, const Clip& clip, const ScriptFrame& f,
                uint64_t audio_epoch, bool exporting,
                std::vector<ScriptError>& errors);

    // True when any watched file (module, pms.json dep) changed on disk.
    bool poll_dirty();
    // Last render's log tail (pms.log lines, capped).
    const std::vector<std::string>& log_tail() const { return log_tail_; }
    void clear_log() { log_tail_.clear(); }
    // Script-requested post shader for the last rendered frame.
    bool has_post() const { return has_post_; }
    void set_canvas(class ScriptCanvas* c);
    const std::string& post_frag() const { return post_frag_; }
    const std::vector<std::pair<std::string, std::vector<float>>>& post_uniforms() const {
        return post_uniforms_;
    }
    // Face tracks requested via pms.face(path) during the last render.
    const std::vector<std::string>& face_requests() const { return face_requests_; }
    // JSON files loaded via pms.json (for file watching).
    const std::vector<std::string>& json_deps() const { return json_deps_; }

public:
    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
    std::vector<std::string> log_tail_;
    bool has_post_ = false;
    std::string post_frag_;
    std::vector<std::pair<std::string, std::vector<float>>> post_uniforms_;
    std::vector<std::string> face_requests_;
    std::vector<std::string> json_deps_;
};

// Resolve `spec` relative to `referrer` (file path) or the entry dir.
// Returns "" when unresolvable. Built-ins (pms:rhythm, pms:text,
// pms:typography/*) resolve to the assets/scripts/ path.
std::string script_resolve_spec(const std::string& spec, const std::string& referrer,
                                const std::string& entry_dir);

// Current audio epoch: increments every time AppState::audio_analysis is
// published (load_audio_analysis). The runtime memoises the frozen objects
// against this + the mapped clip offset.
uint64_t script_audio_epoch(const AppState& state);
