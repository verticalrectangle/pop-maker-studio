// Skia (Ganesh GL) surface + post/unpremultiply passes for Script clips —
// see script_gpu.h.
#include "script_gpu.h"

#include "gl_compat.h"
#include "gl_state_guard.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/core/SkSurfaceProps.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrContextOptions.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLAssembleInterface.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/gpu/ganesh/gl/GrGLTypes.h"

#include <dlfcn.h>

#include <cstring>
#include <optional>
#include <unordered_map>

namespace {

// GL entry points for Skia: the process already links the GL library, so
// dlsym covers core functions; extensions come from the window system's
// loader (EGL on Wayland, GLX on X11 — GLVND stubs work for either).
GrGLFuncPtr gl_proc(void*, const char name[]) {
    using EglGet = void* (*)(const char*);
    using GlxGet = void* (*)(const unsigned char*);
    static EglGet egl = (EglGet)dlsym(RTLD_DEFAULT, "eglGetProcAddress");
    static GlxGet glx = (GlxGet)dlsym(RTLD_DEFAULT, "glXGetProcAddressARB");
    if (void* p = dlsym(RTLD_DEFAULT, name)) return (GrGLFuncPtr)p;
    if (egl)
        if (void* p = egl(name)) return (GrGLFuncPtr)p;
    if (glx)
        if (void* p = glx((const unsigned char*)name)) return (GrGLFuncPtr)p;
    return nullptr;
}

// One Skia context for every Script clip. Abandoned (not destroyed through
// GL) at process exit: the GL context is gone by then.
struct Gpu {
    sk_sp<GrDirectContext> gr;
    std::string error;
    ~Gpu() {
        if (gr) gr->abandonContext();
    }
};

GrDirectContext* gr_context(std::string* err) {
    static Gpu g;
    if (g.gr) return g.gr.get();
    if (!g.error.empty()) {
        if (err) *err = g.error;
        return nullptr;
    }
    sk_sp<const GrGLInterface> iface = GrGLMakeAssembledInterface(nullptr, gl_proc);
    if (iface) {
        GrContextOptions opts;
        g.gr = GrDirectContexts::MakeGL(std::move(iface), opts);
    }
    if (!g.gr) {
        g.error = "Skia could not start on this GL context (needs OpenGL 3.3 core)";
        if (err) *err = g.error;
        return nullptr;
    }
    g.gr->setResourceCacheLimit((size_t)512 << 20);
    return g.gr.get();
}

// ── Fullscreen passes ────────────────────────────────────────────────────

// v_uv: 0..1, origin top-left; row 0 of every texture involved is the top
// row, so NDC y = -1 (framebuffer row 0) maps to v_uv.y = 0.
const char* kVert =
    "#version 330 core\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);\n"
    "  v_uv = p * 0.5 + 0.5;\n"
    "  gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

const char* kUnpremulFrag =
    "#version 330 core\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "  vec4 c = texelFetch(u_tex, ivec2(gl_FragCoord.xy), 0);\n"
    "  fragColor = c.a > 0.0 ? vec4(clamp(c.rgb / c.a, 0.0, 1.0), c.a) : vec4(0.0);\n"
    "}\n";

struct Program {
    GLuint prog = 0;
    GLint u_tex = -1, u_res = -1;
    std::unordered_map<std::string, GLint> uniforms;
    std::string error;  // non-empty = failed to build (cached, not retried)
};

GLuint compile(GLenum type, const char* src, const char* tag, std::string& err) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096] = {};
        glGetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
        err = std::string(tag) + ": " + log;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

Program build(const std::string& frag_src) {
    Program p;
    GLuint vs = compile(GL_VERTEX_SHADER, kVert, "post vertex shader", p.error);
    if (!vs) return p;
    GLuint fs = compile(GL_FRAGMENT_SHADER, frag_src.c_str(), "post fragment shader", p.error);
    if (!fs) {
        glDeleteShader(vs);
        return p;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = {};
        glGetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
        p.error = std::string("post shader link: ") + log;
        glDeleteProgram(prog);
        return p;
    }
    p.prog = prog;
    p.u_tex = glGetUniformLocation(prog, "u_tex");
    p.u_res = glGetUniformLocation(prog, "u_res");
    return p;
}

// Post programs keyed by the author's frag body (compiled once per string).
Program& post_program(const std::string& frag) {
    static std::unordered_map<std::string, Program> cache;
    auto it = cache.find(frag);
    if (it != cache.end()) return it->second;
    std::string src =
        "#version 330 core\n"
        "uniform sampler2D u_tex;\n"
        "uniform vec2 u_res;\n"
        "in vec2 v_uv;\n"
        "out vec4 fragColor;\n"
        "#line 1\n" +
        frag + "\n";
    return cache.emplace(frag, build(src)).first->second;
}

Program& unpremul_program() {
    static Program p = build(kUnpremulFrag);
    return p;
}

struct PassObjects {
    GLuint vao = 0;
    GLuint sampler = 0;  // linear + clamp, overrides Skia's texture params
};

PassObjects& pass_objects() {
    static PassObjects o;
    if (!o.vao) {
        glGenVertexArrays(1, &o.vao);
        glGenSamplers(1, &o.sampler);
        glSamplerParameteri(o.sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(o.sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri(o.sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(o.sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    return o;
}

struct Target {
    GLuint tex = 0, fbo = 0;
    int w = 0, h = 0;

    void ensure(int nw, int nh) {
        if (tex && w == nw && h == nh) return;
        release();
        w = nw;
        h = nh;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    }

    void release() {
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (tex) glDeleteTextures(1, &tex);
        fbo = tex = 0;
    }
};

void run_pass(const Program& p, GLuint src, const Target& dst, float res_w, float res_h,
              const ScriptUniforms* uniforms, std::unordered_map<std::string, GLint>* locs) {
    PassObjects& o = pass_objects();
    glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
    glViewport(0, 0, dst.w, dst.h);
    glUseProgram(p.prog);
    glBindVertexArray(o.vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src);
    glBindSampler(0, o.sampler);
    if (p.u_tex >= 0) glUniform1i(p.u_tex, 0);
    if (p.u_res >= 0) glUniform2f(p.u_res, res_w, res_h);
    if (uniforms) {
        for (const auto& u : *uniforms) {
            auto it = locs->find(u.first);
            if (it == locs->end())
                it = locs->emplace(u.first, glGetUniformLocation(p.prog, u.first.c_str())).first;
            GLint loc = it->second;
            if (loc < 0) continue;
            const std::vector<float>& v = u.second;
            switch (v.size()) {
                case 1: glUniform1f(loc, v[0]); break;
                case 2: glUniform2f(loc, v[0], v[1]); break;
                case 3: glUniform3f(loc, v[0], v[1], v[2]); break;
                case 4: glUniform4f(loc, v[0], v[1], v[2], v[3]); break;
                default: break;
            }
        }
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

}  // namespace

struct ScriptSurface::Impl {
    sk_sp<SkSurface> surface;
    int w = 0, h = 0;
    Target out, scratch;
    std::optional<GLStateGuard> guard;

    ~Impl() {
        out.release();
        scratch.release();
    }
};

ScriptSurface::ScriptSurface() : impl_(new Impl) {}
ScriptSurface::~ScriptSurface() = default;

SkCanvas* ScriptSurface::begin(int w, int h, std::string* err) {
    Impl& s = *impl_;
    GrDirectContext* gr = gr_context(err);
    if (!gr) return nullptr;
    s.guard.emplace();
    gr->resetContext();
    if (!s.surface || s.w != w || s.h != h) {
        SkSurfaceProps props(0, kUnknown_SkPixelGeometry);
        SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
        s.surface = SkSurfaces::RenderTarget(gr, skgpu::Budgeted::kNo, info, 0,
                                             kTopLeft_GrSurfaceOrigin, &props, false);
        if (!s.surface) {
            s.guard.reset();
            if (err) *err = "Skia could not allocate a " + std::to_string(w) + "x" +
                            std::to_string(h) + " render target";
            return nullptr;
        }
        s.w = w;
        s.h = h;
    }
    SkCanvas* c = s.surface->getCanvas();
    c->restoreToCount(1);
    c->resetMatrix();
    c->clear(SK_ColorTRANSPARENT);
    return c;
}

bool ScriptSurface::end(const std::string& post_frag, const ScriptUniforms& uniforms, int res_w,
                        int res_h, std::string* err) {
    Impl& s = *impl_;
    if (!s.guard) return false;
    s.surface->getCanvas()->restoreToCount(1);
    GrBackendTexture bt =
        SkSurfaces::GetBackendTexture(s.surface.get(), SkSurface::BackendHandleAccess::kFlushRead);
    gr_context(nullptr)->flushAndSubmit();
    GrGLTextureInfo info{};
    GrBackendTextures::GetGLTextureInfo(bt, &info);
    GLuint src = info.fID;

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    bool ok = true;
    if (!post_frag.empty()) {
        Program& p = post_program(post_frag);
        if (p.prog) {
            s.scratch.ensure(s.w, s.h);
            run_pass(p, src, s.scratch, (float)res_w, (float)res_h, &uniforms, &p.uniforms);
            src = s.scratch.tex;
        } else {
            ok = false;
            if (err) *err = p.error;
        }
    }
    s.out.ensure(s.w, s.h);
    Program& up = unpremul_program();
    if (up.prog) run_pass(up, src, s.out, (float)s.w, (float)s.h, nullptr, nullptr);
    else if (err && ok) { *err = up.error; ok = false; }
    s.guard.reset();
    return ok;
}

unsigned ScriptSurface::texture() const { return impl_->out.tex; }
int ScriptSurface::width() const { return impl_->w; }
int ScriptSurface::height() const { return impl_->h; }
