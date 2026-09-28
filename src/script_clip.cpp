// Per-clip Script compositing: owns one ScriptRuntime + ScriptCanvas per
// timeline clip (keyed by track/clip index + path), renders frames on the
// main/GL thread, caches the last frame per SCRIPT_API §9 (skip render when
// t, canvas size, params and module/data versions are unchanged), and
// exposes the GL texture for preview / export / snapshot compositors.
//
// Post shader: compiled once per distinct frag string, cached in the GL
// helper below (program cache keyed by hash). Export blocks on face tracks
// (face_cache_ensure_sync); preview only requests (null while building).
// Errors never throw: the clip renders the red error card and records
// errors for get_script_errors.
#include "script_clip.h"
#include "script_runtime.h"
#include "script_canvas.h"
#include "script_face.h"
#include "app.h"
#include "face_cache.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#if PMS_HAS_GL
#include "gl_compat.h"
#endif

namespace {

struct Entry {
    std::unique_ptr<ScriptRuntime> rt;
    std::unique_ptr<ScriptCanvas> canvas;
    std::string script_path;
    std::string script_params;
    // §9 cache key.
    double last_t = NAN;
    int last_w = -1, last_h = -1;
    uint64_t last_audio_epoch = 0;
    std::string last_audio_key;
    uint64_t module_gen = 0;   // bumped on rebuild (watcher)
    bool built_ok = false;
    std::vector<ScriptError> errors;
    std::vector<std::string> err_card;
    std::string perf;
    bool has_post = false;
    std::string post_frag;
    std::vector<std::pair<std::string, std::vector<float>>> post_uniforms;
};

std::map<std::string, Entry> g_entries;
std::mutex g_mtx;  // guards map structure; rendering happens on GL thread

Entry& entry_for(const std::string& key) {
    auto it = g_entries.find(key);
    if (it == g_entries.end()) {
        Entry e;
        e.rt = std::make_unique<ScriptRuntime>();
        e.canvas = std::make_unique<ScriptCanvas>();
        auto r = g_entries.emplace(key, std::move(e));
        return r.first->second;
    }
    return it->second;
}

std::string audio_key_for(const AppState& state) {
    if (!state.audio_analysis) return "null";
    char b[96];
    snprintf(b, sizeof(b), "%p#%.3f", (const void*)state.audio_analysis.get(),
             state.audio_analysis->duration);
    return b;
}

}  // namespace

// ── Post-shader GL helper ────────────────────────────────────────────────
// Fullscreen pass over the clip texture: renders `tex` through `frag` into
// the clip FBO (ping-pong via a scratch texture). Programs cached per frag.

#if PMS_HAS_GL
namespace {
struct PostProg {
    size_t hash = 0;
    GLuint prog = 0;
    GLint u_tex = -1, u_res = -1;
};
std::vector<PostProg> g_post_progs;
GLuint g_post_vbo = 0;
GLuint g_post_scratch = 0;
int g_post_sw = 0, g_post_sh = 0;

size_t hash_str(const std::string& s) {
    size_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

bool compile_post(const std::string& frag, PostProg& out, std::string* err) {
    static const char* kVert =
        "#version 330 core\n"
        "layout(location=0) in vec2 a_pos;\n"
        "out vec2 v_uv;\n"
        "void main(){ v_uv = vec2(a_pos.x*0.5+0.5, 0.5-a_pos.y*0.5);\n"
        " gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    // v_uv 0..1 origin top-left (y down, matching the canvas).
    std::string fsrc =
        "#version 330 core\n"
        "uniform sampler2D u_tex;\n"
        "uniform vec2 u_res;\n"
        "in vec2 v_uv;\n"
        "out vec4 fragColor;\n" + frag + "\n";
    auto sh = [&](GLenum type, const char* src, const char* tag) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048] = {};
            glGetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
            if (err) *err = std::string(tag) + ": " + log;
            glDeleteShader(s);
            return 0;
        }
        return s;
    };
    GLuint vs = sh(GL_VERTEX_SHADER, kVert, "post vertex");
    if (!vs) return false;
    GLuint fs = sh(GL_FRAGMENT_SHADER, fsrc.c_str(), "post fragment");
    if (!fs) { glDeleteShader(vs); return false; }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetProgramInfoLog(p, sizeof(log) - 1, nullptr, log);
        if (err) *err = std::string("post link: ") + log;
        glDeleteProgram(p);
        return false;
    }
    out.hash = hash_str(frag);
    out.prog = p;
    out.u_tex = glGetUniformLocation(p, "u_tex");
    out.u_res = glGetUniformLocation(p, "u_res");
    return true;
}

// Run the post pass in place: clip FBO texture `tex` (w×h) through frag.
bool run_post(GLuint clip_fbo, GLuint tex, int w, int h,
              const std::string& frag,
              const std::vector<std::pair<std::string, std::vector<float>>>& uniforms,
              std::string* err) {
    size_t hh = hash_str(frag);
    PostProg* pp = nullptr;
    for (auto& p : g_post_progs)
        if (p.hash == hh) { pp = &p; break; }
    PostProg fresh;
    if (!pp) {
        if (!compile_post(frag, fresh, err)) return false;
        g_post_progs.push_back(fresh);
        pp = &g_post_progs.back();
    }
    if (g_post_sw != w || g_post_sh != h || !g_post_scratch) {
        if (g_post_scratch) glDeleteTextures(1, &g_post_scratch);
        g_post_sw = w; g_post_sh = h;
        glGenTextures(1, &g_post_scratch);
        glBindTexture(GL_TEXTURE_2D, g_post_scratch);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (!g_post_vbo) {
        static const float kTri[6] = {-1,-1, 3,-1, -1,3};
        glGenBuffers(1, &g_post_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, g_post_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kTri), kTri, GL_STATIC_DRAW);
    }
    // Save state NanoVG / compositor may rely on.
    GLint prev_prog = 0, prev_fbo = 0, prev_tex = 0, prev_ab = 0;
    GLboolean prev_blend = glIsEnabled(GL_BLEND), prev_depth = glIsEnabled(GL_DEPTH_TEST),
              prev_stencil = glIsEnabled(GL_STENCIL_TEST), prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_ab);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST); glDisable(GL_SCISSOR_TEST);
    // Copy clip → scratch, then render scratch → clip FBO through shader.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, clip_fbo);
    glBindTexture(GL_TEXTURE_2D, g_post_scratch);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, clip_fbo);
    glViewport(0, 0, w, h);
    glUseProgram(pp->prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_post_scratch);
    if (pp->u_tex >= 0) glUniform1i(pp->u_tex, 0);
    if (pp->u_res >= 0) glUniform2f(pp->u_res, (float)w, (float)h);
    for (auto& u : uniforms) {
        GLint loc = glGetUniformLocation(pp->prog, u.first.c_str());
        if (loc < 0) continue;
        if (u.second.size() == 1) glUniform1f(loc, u.second[0]);
        else if (u.second.size() == 2) glUniform2f(loc, u.second[0], u.second[1]);
        else if (u.second.size() == 3) glUniform3f(loc, u.second[0], u.second[1], u.second[2]);
        else if (u.second.size() >= 4) glUniform4f(loc, u.second[0], u.second[1], u.second[2], u.second[3]);
    }
    glBindBuffer(GL_ARRAY_BUFFER, g_post_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableVertexAttribArray(0);
    // Restore.
    glUseProgram((GLuint)prev_prog);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_ab);
    if (prev_blend) glEnable(GL_BLEND);
    if (prev_depth) glEnable(GL_DEPTH_TEST);
    if (prev_stencil) glEnable(GL_STENCIL_TEST);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST);
    return true;
}
}  // namespace
#endif

unsigned script_clip_texture(AppState& state, const Clip& clip,
                             const std::string& clip_key,
                             float t, int canvas_w, int canvas_h,
                             bool exporting, std::vector<ScriptError>& errors) {
    if (canvas_w <= 0 || canvas_h <= 0) return 0;
    Entry& e = entry_for(clip_key);
    uint64_t audio_epoch = script_audio_epoch(state);
    std::string akey = audio_key_for(state);
    bool size_ok = (e.last_w == canvas_w && e.last_h == canvas_h);
    bool same_frame = size_ok && e.built_ok && e.last_t == (double)t &&
                      e.last_audio_epoch == audio_epoch && e.last_audio_key == akey &&
                      e.script_path == clip.script_path &&
                      e.script_params == clip.script_params &&
                      !e.rt->poll_dirty();
    if (same_frame) {
        if (!e.errors.empty())
            for (auto& er : e.errors) errors.push_back(er);
        return e.canvas->texture();
    }
    // (Re)build when path/params changed or files dirty.
    bool need_build = !e.built_ok || e.script_path != clip.script_path ||
                      e.script_params != clip.script_params || e.rt->poll_dirty();
    if (need_build) {
        e.script_path = clip.script_path;
        e.script_params = clip.script_params;
        e.errors.clear();
        e.rt->set_canvas(e.canvas.get());
        std::vector<ScriptError> berr;
        e.built_ok = e.rt->build(state, clip, clip_key, berr);
        e.module_gen++;
        for (auto& er : berr) { e.errors.push_back(er); errors.push_back(er); }
        if (!e.errors.empty() && e.errors.size() > 8) e.errors.resize(8);
        if (!e.built_ok) {
            e.err_card.clear();
            for (auto& er : e.errors) e.err_card.push_back(er.message);
#if PMS_HAS_GL
            if (e.canvas->begin_frame(canvas_w, canvas_h)) {
                e.canvas->error_card(e.err_card.empty() ?
                    std::vector<std::string>{"script error"} : e.err_card);
                e.canvas->end_frame();
            }
#endif
            e.last_t = (double)t;
            e.last_w = canvas_w; e.last_h = canvas_h;
            e.last_audio_epoch = audio_epoch;
            e.last_audio_key = akey;
            return e.canvas->texture();
        }
    }
    // Export blocks on face tracks requested last frame; preview requests.
    // (First frame: request happens inside render via pms.face.)
#if PMS_HAS_GL
    if (!e.canvas->begin_frame(canvas_w, canvas_h)) return 0;
    e.canvas->ensure_builtin_fonts();
    ScriptFrame f;
    f.t = t;
    f.local = (double)t - (double)clip.start;
    f.frame = (int)std::lround((double)t * (double)state.fps);
    f.fps = state.fps;
    f.width = canvas_w; f.height = canvas_h;
    f.duration = (double)clip.end - (double)clip.start;
    f.exporting = exporting;
    std::vector<ScriptError> rerr;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = e.rt->render(state, clip, f, audio_epoch, exporting, rerr);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    char pb[64];
    snprintf(pb, sizeof(pb), "script %.2f ms", ms);
    e.perf = pb;
    e.canvas->end_frame();
    if (!ok) {
        for (auto& er : rerr) { e.errors.push_back(er); errors.push_back(er); }
        if (e.errors.size() > 8) e.errors.resize(8);
        e.err_card.clear();
        for (auto& er : e.errors) e.err_card.push_back(er.message);
        if (e.canvas->begin_frame(canvas_w, canvas_h)) {
            e.canvas->error_card(e.err_card.empty() ?
                std::vector<std::string>{"script error"} : e.err_card);
            e.canvas->end_frame();
        }
    } else {
        // Post shader pass.
        if (e.rt->has_post()) {
            // The canvas FBO was unbound by end_frame; wrap the same texture
            // in a temp FBO and run the pass through it.
            GLuint tmpfbo = 0;
            glGenFramebuffers(1, &tmpfbo);
            glBindFramebuffer(GL_FRAMEBUFFER, tmpfbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, e.canvas->texture(), 0);
            std::string perr;
            GLuint tex = e.canvas->texture();
            // run_post copies FBO→scratch then renders back into FBO.
            bool pok = run_post(tmpfbo, tex, canvas_w, canvas_h,
                                e.rt->post_frag(), e.rt->post_uniforms(), &perr);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &tmpfbo);
            if (!pok) {
                ScriptError er;
                er.message = "post shader: " + perr;
                e.errors.push_back(er);
                errors.push_back(er);
                if (e.errors.size() > 8) e.errors.resize(8);
            }
        }
        // Face prefetch for next frame (preview): request tracks seen.
        for (auto& r : e.rt->face_requests()) face_cache_request(r, 0);
    }
    // Export gate: block until every requested track is ready. (Preview
    // returns the frame with null faces instead.)
    if (exporting && ok) {
        for (auto& r : e.rt->face_requests()) {
            if (!face_cache_ensure_sync(r, 0, nullptr)) {
                ScriptError er;
                er.message = "face track failed: " + r;
                e.errors.push_back(er);
                errors.push_back(er);
            }
        }
    }
    e.last_t = (double)t;
    e.last_w = canvas_w; e.last_h = canvas_h;
    e.last_audio_epoch = audio_epoch;
    e.last_audio_key = akey;
    return e.canvas->texture();
#else
    (void)exporting;
    return 0;
#endif
}

void script_clip_gc(const std::vector<std::string>& live_keys) {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        bool live = false;
        for (auto& k : live_keys)
            if (k == it->first) { live = true; break; }
        if (!live) it = g_entries.erase(it);
        else ++it;
    }
}

void script_clip_invalidate(const std::string& clip_key) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_entries.find(clip_key);
    if (it != g_entries.end()) {
        it->second.built_ok = false;
        it->second.last_t = NAN;
    }
}

bool script_clip_poll() {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto& kv : g_entries) {
        if (kv.second.rt && kv.second.rt->poll_dirty()) return true;
    }
    return false;
}

std::string script_clip_perf(const std::string& clip_key) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_entries.find(clip_key);
    if (it == g_entries.end()) return "";
    return it->second.perf;
}

std::vector<std::pair<std::string, std::vector<ScriptError>>> script_clip_errors(
    const AppState& state) {
    std::lock_guard<std::mutex> lk(g_mtx);
    std::vector<std::pair<std::string, std::vector<ScriptError>>> out;
    // Walk the timeline for stable clip labels.
    for (size_t ti = 0; ti < state.tracks.size(); ++ti) {
        for (size_t ci = 0; ci < state.tracks[ti].clips.size(); ++ci) {
            const Clip& cl = state.tracks[ti].clips[ci];
            if (cl.clip_type != ClipType::Script) continue;
            char key[64];
            snprintf(key, sizeof(key), "%zu:%zu", ti, ci);
            auto it = g_entries.find(key);
            std::vector<ScriptError> errs;
            if (it != g_entries.end()) errs = it->second.errors;
            // Also surface the runtime log tail as info errors? No — log is
            // separate; errors only. Log tail appended by IPC layer.
            if (!errs.empty()) out.push_back({key, errs});
        }
    }
    return out;
}
