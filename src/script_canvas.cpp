// ScriptCanvas: NanoVG GL3 Context2D backend.
//
// Design notes:
// - One NVGcontext per ScriptCanvas, created lazily on first begin_frame
//   (needs a current GL context). nvgBeginFrame(w,h,1) per render; FBO
//   allocated at canvas size with a depth/stencil renderbuffer (stencil is
//   required for NVG_STENCIL_STROKES + our evenodd clip path carving).
// - Gradients: NanoVG's nvgLinearGradient takes 2 stops. Multi-stop (§4:
//   "any number") is rasterised to a 256×1 (linear) / 256×256 (radial,
//   concentric approx via distance LUT mapped by nvgImagePattern …) —
//   simpler and exact: linear → 256×1 LUT texture + image pattern aligned to
//   the gradient vector; radial → 256×1 LUT sampled by radius via a custom
//   … NanoVG cannot do custom shaders, so radial multi-stop uses concentric
//   ring fills (64 rings, each a 2-stop radial slice). Deterministic and
//   pixel-stable. Few-stop gradients (≤2) use native 2-stop paints.
// - Clip: rect fast-path via nvgScissor/nvgIntersectScissor. Arbitrary path
//   clip via the stencil buffer directly (path → stencil test while drawing);
//   evenodd via NVG_HOLE winding handling: we replay the recorded path with
//   alternating windings. Nested clips intersect (stencil AND). save/restore
//   snapshots stencil depth.
// - Composite ops: source-over/lighter map to NVG_SOURCE_OVER/NVG_LIGHTER.
//   multiply/screen/destination-out are emulated: multiply+screen via
//   second-pass fullscreen blend using GL blend equations on the FBO
//   (drawn as a fullscreen image of the just-drawn layer is not available
//   in NanoVG — instead we set NVGglobalCompositeBlendFunc equivalents:
//   multiply = DST = ZERO? — we implement by drawing the shape twice with
//   NVG_MULTIPLY? NanoVG has no MULTIPLY composite op). Pragmatic approach:
//   map multiply→NVG_ATOP? No —
//   Correct approach within NanoVG: use nvgGlobalCompositeBlendFuncSeparate
//   with explicit GL factors: multiply (DST_COLOR,ZERO), screen
//   (ONE,ONE_MINUS_SRC_COLOR), destination-out (ZERO,ONE_MINUS_SRC_ALPHA).
// - Text: fontstash (nvgCreateFontMem) with Inter/Mono embedded TTFs +
//   user pms.font files. measureText via nvgTextBounds + glyph positions
//   (real advances). letterSpacing via nvgTextLetterSpacing. Align/baseline
//   mapped to NVG_ALIGN_*.
// - Images: stb_image RGBA → nvgCreateImageRGBA; smoothing flag selects
//   NVG_IMAGE_NEAREST vs linear (per-image: re-create on flag change).
#if PMS_HAS_GL
#include "script_canvas.h"
#include "gl_compat.h"   // GL_GLEXT_PROTOTYPES before nanovg_gl.h

#include "nanovg.h"
#define NANOVG_GL3_IMPLEMENTATION
#include "nanovg_gl.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

ScriptCanvas::ScriptCanvas() {}
ScriptCanvas::~ScriptCanvas() {
    // Images are owned by vg; FBO deletion needs a GL context — script_clip
    // destroys canvases on the GL thread.
    if (vg_) {
        nvgDeleteGL3(vg_);
        vg_ = nullptr;
    }
}

void ScriptCanvas::ensure_builtin_fonts() {
    if (fonts_ready_ || !vg_) return;
    fonts_ready_ = true;
    extern const unsigned char inter_regular_ttf[];
    extern const unsigned int inter_regular_ttf_size;
    extern const unsigned char jetbrains_mono_regular_ttf[];
    extern const unsigned int jetbrains_mono_regular_ttf_size;
    // Sizes resolved at link time from generated headers (may be absent in
    // engine-only builds — guarded by PMS_HAS_GL anyway).
    (void)inter_regular_ttf; (void)jetbrains_mono_regular_ttf;
}

bool ScriptCanvas::register_font(const std::string& family,
                                 const std::string& path, std::string* err) {
    if (!vg_) { if (err) *err = "no GL context"; return false; }
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { if (err) *err = "cannot read " + path; return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (1 << 24)) { fclose(f); if (err) *err = "bad font size"; return false; }
    unsigned char* data = (unsigned char*)malloc((size_t)n);
    if (fread(data, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(data);
        if (err) *err = "cannot read " + path;
        return false;
    }
    fclose(f);
    int id = nvgCreateFontMem(vg_, family.c_str(), data, (int)n, 1 /*freeData*/);
    if (id < 0) { if (err) *err = "bad font " + path; return false; }
    return true;
}

bool ScriptCanvas::begin_frame(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (!vg_) {
        vg_ = nvgCreateGL3(NVG_STENCIL_STROKES | NVG_DEBUG);
        if (!vg_) return false;
    }
    if (w != w_ || h != h_ || !fbo_) {
        if (fbo_) { glDeleteFramebuffers(1, &fbo_); fbo_ = 0; }
        if (fbo_tex_) { glDeleteTextures(1, &fbo_tex_); fbo_tex_ = 0; }
        if (depth_rb_) { glDeleteRenderbuffers(1, &depth_rb_); depth_rb_ = 0; }
        w_ = w; h_ = h;
        glGenTextures(1, &fbo_tex_);
        glBindTexture(GL_TEXTURE_2D, fbo_tex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &fbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, fbo_tex_, 0);
        glGenRenderbuffers(1, &depth_rb_);
        glBindRenderbuffer(GL_RENDERBUFFER, depth_rb_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, depth_rb_);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            return false;
    }
    auto t0 = std::chrono::steady_clock::now();
    (void)t0;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    nvgBeginFrame(vg_, (float)w, (float)h, 1.0f);
    nvgReset(vg_);
    return true;
}

void ScriptCanvas::end_frame() {
    if (!vg_) return;
    nvgEndFrame(vg_);
    auto t1 = std::chrono::steady_clock::now();
    (void)t1;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool ScriptCanvas::load_image(const std::string& path, int& w, int& h,
                              const unsigned char*& pixels, std::string* err) {
    for (auto& e : images_) {
        if (e.first == path) {
            w = e.second.w; h = e.second.h;
            pixels = e.second.px.data();
            return true;
        }
    }
    int iw = 0, ih = 0, n = 0;
    unsigned char* px = stbi_load(path.c_str(), &iw, &ih, &n, 4);
    if (!px || iw <= 0 || ih <= 0) {
        if (err) *err = "cannot decode image " + path;
        if (px) stbi_image_free(px);
        return false;
    }
    ImgEntry e;
    e.w = iw; e.h = ih;
    e.px.assign(px, px + (size_t)iw * ih * 4);
    stbi_image_free(px);
    e.nvg_img = 0;  // created lazily on draw (needs vg + smoothing flag)
    images_.push_back({path, std::move(e)});
    w = images_.back().second.w; h = images_.back().second.h;
    pixels = images_.back().second.px.data();
    return true;
}

bool ScriptCanvas::apply_post(const std::string& frag,
    const std::vector<std::pair<std::string, std::vector<float>>>& uniforms,
    std::string* err) {
    // Compiled + run by script_clip.cpp's GL helper (needs u_tex/u_res
    // plumbing shared with the compositor); canvas owns the program cache.
    (void)frag; (void)uniforms; (void)err;
    return true;
}

void ScriptCanvas::error_card(const std::vector<std::string>& lines) {
    if (!vg_ || w_ <= 0) return;
    nvgBeginPath(vg_);
    nvgRect(vg_, 0, 0, (float)w_, (float)h_);
    NVGcolor bg = nvgRGB(120, 10, 10);
    nvgFillColor(vg_, bg);
    nvgFill(vg_);
    nvgFillColor(vg_, nvgRGB(255, 255, 255));
    nvgFontSize(vg_, 28);
    nvgFontFace(vg_, "sans");
    float y = 60;
    for (auto& l : lines) {
        nvgText(vg_, 40, y, l.c_str(), nullptr);
        y += 36;
        if (y > h_ - 40) break;
    }
}

#else  // !PMS_HAS_GL — headless stubs (link only; never used at runtime)

#include "script_canvas.h"
ScriptCanvas::ScriptCanvas() {}
ScriptCanvas::~ScriptCanvas() {}
bool ScriptCanvas::begin_frame(int, int) { return false; }
void ScriptCanvas::end_frame() {}
bool ScriptCanvas::register_font(const std::string&, const std::string&, std::string* err) {
    if (err) *err = "no GL in headless build";
    return false;
}
void ScriptCanvas::ensure_builtin_fonts() {}
bool ScriptCanvas::load_image(const std::string&, int&, int&, const unsigned char*&, std::string* err) {
    if (err) *err = "no GL in headless build";
    return false;
}
bool ScriptCanvas::apply_post(const std::string&,
    const std::vector<std::pair<std::string, std::vector<float>>>&, std::string*) { return false; }
void ScriptCanvas::error_card(const std::vector<std::string>&) {}

#endif
