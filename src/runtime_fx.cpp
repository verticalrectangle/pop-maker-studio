#include "platform.h"
#include "runtime_fx.h"
#include "json.hpp"

#if PMS_HAS_GL
#include "gl_compat.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;
using json   = nlohmann::json;

// ── Static state ──────────────────────────────────────────────────────────────

static std::vector<RuntimeFXDef> g_defs;

// Single output texture+FBO for applying runtime effects (resized lazily).
static GLuint g_out_fbo = 0, g_out_tex = 0;
static int    g_out_w   = 0, g_out_h   = 0;

// Shared vertex shader (fullscreen triangle, same as fx_shader.cpp).
static GLuint g_vert = 0;

// Last observed directory mtime.
static fs::file_time_type g_last_mtime{};

// Blend program: blends original and effect by amount.
static GLuint g_blend_prog = 0;

// ── GLSL helpers ──────────────────────────────────────────────────────────────

static const char* k_vert_src = R"glsl(
#version 330 core
out vec2 v_uv;
void main() {
    vec2 pos[3] = vec2[](vec2(-1.0,-1.0), vec2(3.0,-1.0), vec2(-1.0,3.0));
    gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);
    v_uv = pos[gl_VertexID] * 0.5 + 0.5;
}
)glsl";

static const char* k_blend_frag = R"glsl(
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_orig;
uniform sampler2D u_fx;
uniform float u_amount;
void main() {
    vec4 orig = texture(u_orig, v_uv);
    vec4 fx   = texture(u_fx,   v_uv);
    frag = mix(orig, fx, u_amount);
}
)glsl";

static std::string build_frag_src(const RuntimeFXDef& def) {
    std::ostringstream os;
    os << "#version 330 core\n";
    os << "in vec2 v_uv;\n";
    os << "out vec4 frag;\n";
    os << "uniform sampler2D tex;\n";
    os << "uniform vec2 res;\n";
    os << "uniform float t;\n";
    for (auto& p : def.params)
        os << "uniform float " << p.name << ";\n";
    os << "vec4 _fx_body() {\n";
    os << "    vec2 uv = gl_FragCoord.xy / res;\n";
    os << "    (void)uv;\n";
    os << def.glsl_body << "\n";
    os << "}\n";
    os << "void main() { frag = _fx_body(); }\n";
    return os.str();
}

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

static std::string shader_info_log(GLuint obj, bool is_prog) {
    GLint len = 0;
    if (is_prog) glGetProgramiv(obj, GL_INFO_LOG_LENGTH, &len);
    else         glGetShaderiv (obj, GL_INFO_LOG_LENGTH, &len);
    if (len <= 1) return "";
    std::string s(len - 1, '\0');
    if (is_prog) glGetProgramInfoLog(obj, len, nullptr, s.data());
    else         glGetShaderInfoLog (obj, len, nullptr, s.data());
    return s;
}

// Compile and link a program from vertex + fragment source.
// Returns program id (>0) on success, 0 on failure.
// On failure, writes error to out_err.
static GLuint make_program(const char* vert_src, const std::string& frag_src_str,
                           std::string& out_err) {
    const char* frag_src = frag_src_str.c_str();

    GLuint vs = compile_shader(GL_VERTEX_SHADER,   vert_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);

    GLint vs_ok = 0, fs_ok = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &vs_ok);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &fs_ok);
    if (!vs_ok) { out_err = "vert: " + shader_info_log(vs, false); glDeleteShader(vs); glDeleteShader(fs); return 0; }
    if (!fs_ok) { out_err = shader_info_log(fs, false);             glDeleteShader(vs); glDeleteShader(fs); return 0; }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs); glDeleteShader(fs);

    GLint link_ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &link_ok);
    if (!link_ok) { out_err = shader_info_log(prog, true); glDeleteProgram(prog); return 0; }

    out_err.clear();
    return prog;
}

static void compile_def(RuntimeFXDef& def) {
    if (def.program) { glDeleteProgram(def.program); def.program = 0; }
    std::string frag = build_frag_src(def);
    def.program = make_program(k_vert_src, frag, def.compile_error);
}

// ── Ensure vert shader and blend program exist ────────────────────────────────

static void ensure_shared_progs() {
    if (g_vert) return;

    // Vertex shader shared across all runtime effects
    g_vert = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(g_vert, 1, &k_vert_src, nullptr);
    glCompileShader(g_vert);

    // Blend program
    std::string err;
    g_blend_prog = make_program(k_vert_src, k_blend_frag, err);
    if (!g_blend_prog)
        fprintf(stderr, "[runtime_fx] blend shader error: %s\n", err.c_str());
}

// ── Output texture/FBO ────────────────────────────────────────────────────────

static void ensure_out(int w, int h) {
    if (g_out_w == w && g_out_h == h) return;
    if (g_out_fbo) { glDeleteFramebuffers(1, &g_out_fbo); glDeleteTextures(1, &g_out_tex); }

    glGenTextures(1, &g_out_tex);
    glBindTexture(GL_TEXTURE_2D, g_out_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &g_out_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_out_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_out_tex, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    g_out_w = w; g_out_h = h;
}

// ── JSON parsing ──────────────────────────────────────────────────────────────

static bool parse_def(const fs::path& path, RuntimeFXDef& out) {
    std::ifstream f(path);
    if (!f) return false;
    try {
        json j = json::parse(f);
        out.id          = j.value("id",          "");
        out.name        = j.value("name",        "");
        out.category    = j.value("category",    "");
        out.description = j.value("description", "");
        out.glsl_body   = j.value("glsl",        "");
        out.params.clear();
        if (j.contains("params") && j["params"].is_array()) {
            for (auto& p : j["params"]) {
                RuntimeFXParam fp;
                fp.name        = p.value("name",    "param");
                fp.label       = p.value("label",   fp.name);
                fp.min_val     = p.value("min",     0.f);
                fp.max_val     = p.value("max",     1.f);
                fp.default_val = p.value("default", 0.f);
                fp.curve       = p.value("curve",   1.f);
                out.params.push_back(fp);
            }
        }
        if (out.id.empty() || out.glsl_body.empty()) return false;
    } catch (...) { return false; }
    return true;
}

// ── Reload logic ──────────────────────────────────────────────────────────────

static void reload_from_dir(const std::string& dir_str) {
    fs::path dir(dir_str);
    if (!fs::is_directory(dir)) { g_defs.clear(); return; }

    // Build new list
    std::vector<RuntimeFXDef> new_defs;
    for (auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".json") continue;
        RuntimeFXDef def;
        if (!parse_def(entry.path(), def)) continue;

        // Preserve existing compiled program if source hasn't changed
        const RuntimeFXDef* existing = runtime_fx_find(def.id);
        if (existing && existing->glsl_body == def.glsl_body) {
            def.program       = existing->program;
            def.compile_error = existing->compile_error;
        } else {
            ensure_shared_progs();
            compile_def(def);
        }
        new_defs.push_back(std::move(def));
    }

    // Delete GL programs no longer in registry
    for (auto& old : g_defs) {
        bool kept = false;
        for (auto& nd : new_defs) { if (nd.id == old.id && nd.program == old.program) { kept = true; break; } }
        if (!kept && old.program) glDeleteProgram(old.program);
    }

    g_defs = std::move(new_defs);
}

// ── Public API ────────────────────────────────────────────────────────────────

void runtime_fx_init(const std::string& dir) {
    fs::create_directories(dir);
    reload_from_dir(dir);
    try { g_last_mtime = fs::last_write_time(dir); } catch (...) {}
}

void runtime_fx_poll(const std::string& dir) {
    static auto last_check = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_check).count() < 500) return;
    last_check = now;

    try {
        auto mtime = fs::last_write_time(dir);
        if (mtime == g_last_mtime) return;
        g_last_mtime = mtime;
        reload_from_dir(dir);
    } catch (...) {}
}

void runtime_fx_shutdown() {
    for (auto& d : g_defs) if (d.program) glDeleteProgram(d.program);
    g_defs.clear();
    if (g_out_fbo) { glDeleteFramebuffers(1, &g_out_fbo); g_out_fbo = 0; }
    if (g_out_tex) { glDeleteTextures(1, &g_out_tex);     g_out_tex = 0; }
    if (g_blend_prog) { glDeleteProgram(g_blend_prog); g_blend_prog = 0; }
    if (g_vert) { glDeleteShader(g_vert); g_vert = 0; }
    g_out_w = g_out_h = 0;
}

const std::vector<RuntimeFXDef>& runtime_fx_list() { return g_defs; }

const RuntimeFXDef* runtime_fx_find(const std::string& id) {
    for (auto& d : g_defs) if (d.id == id) return &d;
    return nullptr;
}

uintptr_t runtime_fx_apply(const std::string& id,
                            uintptr_t src_tex, int w, int h,
                            const std::vector<float>& param_values,
                            float amount, float t) {
    if (w <= 0 || h <= 0 || !src_tex) return src_tex;

    const RuntimeFXDef* def = runtime_fx_find(id);
    if (!def || !def->program) return src_tex;

    ensure_out(w, h);
    if (!g_out_fbo) return src_tex;

    // Save GL state
    GLint prev_fbo = 0, prev_prog = 0, prev_vp[4] = {};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM,          &prev_prog);
    glGetIntegerv(GL_VIEWPORT,                  prev_vp);

    // Check if we need a VAO (core profile requires one for draw calls)
    static GLuint g_vao = 0;
    if (!g_vao) glGenVertexArrays(1, &g_vao);

    // Apply the runtime FX shader
    glBindFramebuffer(GL_FRAMEBUFFER, g_out_fbo);
    glViewport(0, 0, w, h);
    glUseProgram(def->program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)src_tex);
    glUniform1i(glGetUniformLocation(def->program, "tex"), 0);
    glUniform2f(glGetUniformLocation(def->program, "res"), (float)w, (float)h);
    glUniform1f(glGetUniformLocation(def->program, "t"), t);

    for (int i = 0; i < (int)def->params.size(); ++i) {
        float v = (i < (int)param_values.size()) ? param_values[i] : def->params[i].default_val;
        GLint loc = glGetUniformLocation(def->program, def->params[i].name.c_str());
        if (loc >= 0) glUniform1f(loc, v);
    }

    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    GLuint fx_tex = g_out_tex;

    // If amount < 1, blend original and FX output
    if (amount < 0.999f && g_blend_prog) {
        // We need a second output for blending
        // Use a ping-pong approach: allocate a temp tex+fbo lazily
        static GLuint g_blend_fbo = 0, g_blend_tex = 0;
        static int g_blend_w = 0, g_blend_h = 0;
        if (g_blend_w != w || g_blend_h != h) {
            if (g_blend_fbo) { glDeleteFramebuffers(1, &g_blend_fbo); glDeleteTextures(1, &g_blend_tex); }
            glGenTextures(1, &g_blend_tex);
            glBindTexture(GL_TEXTURE_2D, g_blend_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glGenFramebuffers(1, &g_blend_fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, g_blend_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_blend_tex, 0);
            g_blend_w = w; g_blend_h = h;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, g_blend_fbo);
        glViewport(0, 0, w, h);
        glUseProgram(g_blend_prog);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)src_tex);
        glUniform1i(glGetUniformLocation(g_blend_prog, "u_orig"), 0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_out_tex);
        glUniform1i(glGetUniformLocation(g_blend_prog, "u_fx"), 1);

        glUniform1f(glGetUniformLocation(g_blend_prog, "u_amount"), amount);

        if (!g_vao) glGenVertexArrays(1, &g_vao);
        glBindVertexArray(g_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);

        fx_tex = g_blend_tex;
    }

    // Restore GL state
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glUseProgram((GLuint)prev_prog);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    glActiveTexture(GL_TEXTURE0);

    return (uintptr_t)fx_tex;
}

std::string runtime_fx_validate(const std::string& glsl_body,
                                const std::vector<RuntimeFXParam>& params) {
    ensure_shared_progs();
    RuntimeFXDef tmp;
    tmp.id        = "_validate_tmp";
    tmp.glsl_body = glsl_body;
    tmp.params    = params;
    std::string frag = build_frag_src(tmp);
    std::string err;
    GLuint prog = make_program(k_vert_src, frag, err);
    if (prog) glDeleteProgram(prog);
    return err;
}

#else
uintptr_t runtime_fx_apply(const std::string&, uintptr_t s, int, int, const std::vector<float>&, float, float) { return s; }
void runtime_fx_poll(const std::string&) {}
std::string runtime_fx_validate(const std::string&, const std::vector<RuntimeFXParam>&) { return {}; }
#endif  // PMS_HAS_GL