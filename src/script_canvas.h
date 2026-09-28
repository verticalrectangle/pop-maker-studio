#pragma once
// Context2D on NanoVG (SCRIPT_API.md §4): Canvas-2D-shaped drawing context
// implemented over a NanoVG GL3 context, rendering into the clip's own RGBA
// framebuffer (canvas size). One ScriptCanvas per Script clip, reused across
// frames (FBO reallocated on canvas-size change only).
//
// Coordinate system: canvas pixels, y down. The NanoVG framebuffer is GL
// oriented (y up); script_canvas_present() flips when compositing.
//
// Anything not in SCRIPT_API §4 throws TypeError on access (proxy guard), so
// ports fail loudly.
#include <functional>
#include <string>
#include <vector>

struct NVGcontext;

class ScriptCanvas {
public:
    ScriptCanvas();
    ~ScriptCanvas();

    ScriptCanvas(const ScriptCanvas&) = delete;
    ScriptCanvas& operator=(const ScriptCanvas&) = delete;

    // Begin a frame: ensure FBO/vg sized w×h, clear transparent, reset state.
    // Returns false (GL unavailable — headless) — caller renders error card by
    // other means. Must be called with a current GL context.
    bool begin_frame(int w, int h);
    // Finish NanoVG encoding for this frame (nvgEndFrame). Texture valid after.
    void end_frame();

    // GL texture of the framebuffer (GL RGBA, y-up). Valid after end_frame
    // until the next begin_frame.
    unsigned texture() const { return fbo_tex_; }
    int width() const { return w_; }
    int height() const { return h_; }

    // The NanoVG context for the JS bindings (valid between begin/end_frame).
    NVGcontext* vg() { return vg_; }

    // Register a TTF/OTF file under a family name (pms.font). Idempotent.
    // Returns false + err on load failure.
    bool register_font(const std::string& family, const std::string& path,
                       std::string* err);
    // Ensure the built-ins (Inter, Mono from PMS embedded fonts) exist.
    void ensure_builtin_fonts();

    // Load an image file to RGBA pixels (stb_image, cached per runtime).
    // Returns false + err on decode failure. Out: w/h + RGBA bytes.
    bool load_image(const std::string& path, int& w, int& h,
                    const unsigned char*& pixels, std::string* err);

    // Post-shader pass: compile `frag` (cached per distinct string), run over
    // the clip framebuffer into the same FBO. Uniforms: name → vec1..4.
    // Returns false + err on compile failure. GL context required.
    bool apply_post(const std::string& frag,
                    const std::vector<std::pair<std::string, std::vector<float>>>& uniforms,
                    std::string* err);

    // Render the red error card (script errors) into the framebuffer.
    void error_card(const std::vector<std::string>& lines);

    // Last-frame timing (ms) for the perf readout.
    double last_ms() const { return last_ms_; }

private:
    NVGcontext* vg_ = nullptr;
    unsigned fbo_ = 0, fbo_tex_ = 0, depth_rb_ = 0;
    int w_ = 0, h_ = 0;
    double last_ms_ = 0.0;
    // image cache: path → {w,h,pixels,nvg image}
    struct ImgEntry { int w = 0, h = 0; std::vector<unsigned char> px; int nvg_img = 0; bool smooth = true; };
    std::vector<std::pair<std::string, ImgEntry>> images_;
    // post program cache: frag hash → program
    struct PostProg { std::string frag; unsigned prog = 0; int u_tex = -1, u_res = -1; };
    std::vector<PostProg> post_cache_;
    bool fonts_ready_ = false;
};
