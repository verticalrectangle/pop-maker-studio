#pragma once
// GPU surface behind a Script clip: Skia (Ganesh GL) renders into a
// canvas-sized render target on the app's GL context, then one or two
// fullscreen passes produce the compositor layer texture — the clip's post
// shader (SCRIPT_API §5, premultiplied in/out) and an unpremultiply pass
// into straight alpha, row 0 = top (the layout decoded video frames use).
//
// Skia shares the app's context: begin() snapshots the GL state
// (GLStateGuard) and resets Skia's cached state; end() flushes and restores
// the snapshot, so the app's renderers never see Skia's bindings.
// GL thread only. One ScriptSurface per Script clip.
#include <memory>
#include <string>
#include <utility>
#include <vector>

class SkCanvas;

using ScriptUniforms = std::vector<std::pair<std::string, std::vector<float>>>;

class ScriptSurface {
public:
    ScriptSurface();
    ~ScriptSurface();
    ScriptSurface(const ScriptSurface&) = delete;
    ScriptSurface& operator=(const ScriptSurface&) = delete;

    // Start a w×h frame: (re)allocate on size change, clear to transparent.
    // Returns the canvas (identity matrix) or nullptr + err when Skia cannot
    // run on the current GL context.
    SkCanvas* begin(int w, int h, std::string* err);

    // Finish the frame: flush Skia, run the post shader when `post_frag` is
    // non-empty (u_res = res_w×res_h, the script's logical canvas size), then
    // write the straight-alpha output texture. Returns false + err when the
    // post shader fails to compile/link (the output then holds the frame
    // without the post pass). Always restores GL state.
    bool end(const std::string& post_frag, const ScriptUniforms& uniforms, int res_w, int res_h,
             std::string* err);

    // Output texture (straight alpha, row 0 = top); 0 before the first end().
    unsigned texture() const;
    int width() const;
    int height() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
