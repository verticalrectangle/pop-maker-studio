#include "fx_shader.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#include <cstdio>
#include <cmath>

// ── GLSL shader sources ───────────────────────────────────────────────────────

static const char* k_vert = R"glsl(
#version 330 core
out vec2 v_uv;
void main() {
    // Fullscreen triangle — no VBO needed, just gl_VertexID
    vec2 pos[3] = vec2[](vec2(-1.0,-1.0), vec2(3.0,-1.0), vec2(-1.0,3.0));
    gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);
    v_uv = pos[gl_VertexID] * 0.5 + 0.5;
}
)glsl";

// Grade + vignette ─────────────────────────────────────────────────────────
static const char* k_grade_frag = R"glsl(
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_brightness;
uniform float u_contrast;
uniform float u_saturation;
uniform float u_hue;
uniform float u_vignette;

vec3 hue_rotate(vec3 c, float deg) {
    float rad = deg * 0.017453293;
    float ch = cos(rad), sh = sin(rad);
    // Rodrigues rotation matrix for hue (same coefficients as the CPU path)
    mat3 m = mat3(
        0.299+0.701*ch+0.168*sh, 0.299-0.299*ch-0.328*sh, 0.299-0.299*ch+1.250*sh,
        0.587-0.587*ch+0.330*sh, 0.587+0.413*ch+0.035*sh, 0.587-0.587*ch-1.050*sh,
        0.114-0.114*ch-0.497*sh, 0.114-0.114*ch+0.292*sh, 0.114+0.886*ch-0.203*sh
    );
    return clamp(m * c, 0.0, 1.0);
}

void main() {
    vec4 c = texture(u_tex, v_uv);
    vec3 rgb = c.rgb + u_brightness;
    rgb = (rgb - 0.5) * u_contrast + 0.5;
    float lum = dot(rgb, vec3(0.299, 0.587, 0.114));
    rgb = mix(vec3(lum), rgb, u_saturation);
    if (abs(u_hue) > 0.1) rgb = hue_rotate(rgb, u_hue);
    if (u_vignette > 0.001) {
        vec2 d = v_uv * 2.0 - 1.0;
        float vig = 1.0 - smoothstep(0.5, 1.5, length(d) * u_vignette * 1.5);
        rgb *= vig;
    }
    frag = vec4(clamp(rgb, 0.0, 1.0), c.a);
}
)glsl";

// 1-D box blur (H or V pass) ───────────────────────────────────────────────
static const char* k_blur_frag = R"glsl(
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform vec2 u_dir;     // (1/w, 0) or (0, 1/h)
uniform float u_sigma;  // approx pixel radius

void main() {
    int r = clamp(int(u_sigma * 1.5 + 0.5), 1, 14);
    vec4 sum = vec4(0.0);
    for (int i = -14; i <= 14; i++) {
        if (i < -r || i > r) continue;
        sum += texture(u_tex, v_uv + float(i) * u_dir);
    }
    frag = sum / float(2*r+1);
}
)glsl";

// Chroma-key (YCbCr distance + spill suppression) ─────────────────────────
static const char* k_chroma_key_frag = R"glsl(
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform vec3  u_key_color;
uniform float u_threshold;
uniform float u_softness;

void main() {
    vec4 c = texture(u_tex, v_uv);
    float lum_k = dot(u_key_color, vec3(0.299, 0.587, 0.114));
    vec3 ck = u_key_color - lum_k;
    float lum_p = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    vec3 cp = c.rgb - lum_p;
    float dist = length(cp - ck);
    float soft = max(u_softness, 0.001);
    float t = (dist - u_threshold) / soft;
    float alpha = clamp(t * t * (3.0 - 2.0 * t), 0.0, 1.0);
    vec3 rgb = c.rgb;
    if (alpha < 1.0) {
        float spill = 1.0 - alpha;
        if (u_key_color.g > u_key_color.r && u_key_color.g > u_key_color.b)
            rgb.g = mix(rgb.g, (rgb.r + rgb.b) * 0.5, spill);
        else if (u_key_color.b > u_key_color.r && u_key_color.b > u_key_color.g)
            rgb.b = mix(rgb.b, (rgb.r + rgb.g) * 0.5, spill);
    }
    frag = vec4(rgb, alpha * c.a);
}
)glsl";

// RGB channel split + row jitter ──────────────────────────────────────────
static const char* k_glitch_frag = R"glsl(
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_chroma;   // chroma offset as fraction of width
uniform float u_jitter;   // row-jitter intensity 0..1
uniform float u_time;

float hash(float n) { return fract(sin(n) * 43758.5453); }

void main() {
    ivec2 sz = textureSize(u_tex, 0);
    float y_id = floor(v_uv.y * float(sz.y));
    float rnd = hash(y_id + floor(u_time * 12.0) * 31.7);
    float jshift = 0.0;
    if (u_jitter > 0.01 && rnd > 1.0 - u_jitter * 0.4) {
        float rnd2 = hash(y_id + floor(u_time * 8.0) * 57.3);
        jshift = (rnd2 - 0.5) * u_jitter * 0.12;
    }
    float r = texture(u_tex, clamp(vec2(v_uv.x + jshift + u_chroma, v_uv.y), 0.0, 1.0)).r;
    float g = texture(u_tex, clamp(vec2(v_uv.x + jshift,            v_uv.y), 0.0, 1.0)).g;
    float b = texture(u_tex, clamp(vec2(v_uv.x + jshift - u_chroma, v_uv.y), 0.0, 1.0)).b;
    float a = texture(u_tex, clamp(vec2(v_uv.x + jshift,            v_uv.y), 0.0, 1.0)).a;
    frag = vec4(r, g, b, a);
}
)glsl";

// VHS: chroma bleed + grain + tracking glitch ─────────────────────────────
static const char* k_vhs_frag = R"glsl(
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_noise;
uniform float u_bleed;    // chroma bleed as fraction of width
uniform float u_tracking;
uniform float u_time;

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

void main() {
    vec2 uv = v_uv;
    if (u_tracking > 0.01) {
        float sl  = floor(uv.y * 240.0);
        float rnd = hash(vec2(sl, floor(u_time * 3.0)));
        if (rnd > 1.0 - u_tracking * 0.15)
            uv.x += (hash(vec2(sl, u_time * 7.0)) - 0.5) * u_tracking * 0.05;
        uv.x = clamp(uv.x, 0.0, 1.0);
    }
    float r = texture(u_tex, uv).r;
    float g = texture(u_tex, clamp(uv + vec2(u_bleed * 0.5, 0.0), 0.0, 1.0)).g;
    float b = texture(u_tex, clamp(uv + vec2(u_bleed,       0.0), 0.0, 1.0)).b;
    float a = texture(u_tex, uv).a;
    float grain = hash(uv + fract(vec2(u_time * 0.01))) * u_noise * 0.35;
    frag = vec4(clamp(vec3(r, g, b) + grain, 0.0, 1.0), a);
}
)glsl";

// Procedural light-leak overlay ────────────────────────────────────────────
static const char* k_leak_frag = R"glsl(
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_intensity;
uniform float u_speed;
uniform float u_time;

void main() {
    vec4 src = texture(u_tex, v_uv);
    float t = u_time * u_speed;
    vec2 c1 = vec2(0.5 + 0.3*sin(t*0.7), 0.3 + 0.2*cos(t*0.5));
    vec2 c2 = vec2(0.2 + 0.4*cos(t*0.4), 0.7 + 0.3*sin(t*0.6));
    float d1 = 1.0 - clamp(length(v_uv - c1) * 2.5, 0.0, 1.0);
    float d2 = 1.0 - clamp(length(v_uv - c2) * 2.0, 0.0, 1.0);
    vec3 leak = vec3(1.0, 0.6, 0.2) * pow(d1, 3.0) + vec3(0.8, 0.2, 0.6) * pow(d2, 3.0);
    frag = vec4(clamp(src.rgb + leak * u_intensity, 0.0, 1.0), src.a);
}
)glsl";

// Datamosh: blend current frame with ghost ────────────────────────────────
static const char* k_datamosh_frag = R"glsl(
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_src;
uniform sampler2D u_ghost;
uniform float u_intensity;
uniform float u_bleedback;
uniform float u_t_in_clip;
uniform float u_clip_duration;

void main() {
    vec4 src   = texture(u_src,   v_uv);
    vec4 ghost = texture(u_ghost, v_uv);
    float bleed_ramp = (u_bleedback > 0.001 && u_clip_duration > 0.001)
        ? u_bleedback * clamp(u_t_in_clip / u_clip_duration, 0.0, 1.0) : 0.0;
    float eff_intensity = u_intensity * (1.0 - bleed_ramp);
    frag = mix(src, ghost, eff_intensity);
}
)glsl";

// Datamosh ghost update: mix ghost toward current frame ───────────────────
static const char* k_datamosh_update_frag = R"glsl(
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_src;
uniform sampler2D u_ghost;
uniform float u_decay;

void main() {
    vec4 src   = texture(u_src,   v_uv);
    vec4 ghost = texture(u_ghost, v_uv);
    frag = mix(ghost, src, u_decay);
}
)glsl";

// Simple blit (passthrough) ────────────────────────────────────────────────
static const char* k_blit_frag = R"glsl(
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
void main() { frag = texture(u_tex, v_uv); }
)glsl";


// ── Compile / link helpers ────────────────────────────────────────────────────

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512]; glGetShaderInfoLog(sh, sizeof(buf), nullptr, buf);
        fprintf(stderr, "[fx_shader] compile error: %s\n", buf);
        glDeleteShader(sh); return 0;
    }
    return sh;
}

static GLuint link_prog(const char* frag_src) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   k_vert);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    if (!vs || !fs) { glDeleteShader(vs); glDeleteShader(fs); return 0; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[512]; glGetProgramInfoLog(prog, sizeof(buf), nullptr, buf);
        fprintf(stderr, "[fx_shader] link error: %s\n", buf);
        glDeleteProgram(prog); return 0;
    }
    return prog;
}


// ── GPU state ─────────────────────────────────────────────────────────────────

static struct {
    GLuint grade = 0, blur = 0, chroma_key = 0, glitch = 0;
    GLuint vhs = 0, leak = 0, datamosh = 0, datamosh_update = 0, blit = 0;
} g_prog;

static GLuint g_vao = 0;  // empty VAO required by GL core profile

// Scratch ping-pong (temporary, used within a single fx_apply call)
static struct {
    GLuint fbo[2] = {}, tex[2] = {};
    int w = 0, h = 0;
} g_pp;

// Per-slot stable output textures — indexed by fx_apply's 'slot' argument.
// These persist between pass chains so deferred ImDrawList commands are safe.
static const int kMaxSlots = MAX_VIDEO_TRACKS * 2;
static struct {
    GLuint fbo = 0, tex = 0;
    int w = 0, h = 0;
} g_out[kMaxSlots];

// Datamosh ghost — one ghost buffer per clip instance, keyed by clip_start.
// Only one datamosh clip is active at a time in typical usage.
static struct {
    GLuint fbo = 0, tex = 0;
    int w = 0, h = 0;
    float clip_start = -9999.f;
} g_ghost;


// ── Internal helpers ──────────────────────────────────────────────────────────

static void make_tex_fbo(GLuint& tex, GLuint& fbo, int w, int h) {
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
}

static void pp_ensure(int w, int h) {
    if (g_pp.w == w && g_pp.h == h) return;
    if (g_pp.fbo[0]) { glDeleteFramebuffers(2, g_pp.fbo); glDeleteTextures(2, g_pp.tex); }
    make_tex_fbo(g_pp.tex[0], g_pp.fbo[0], w, h);
    make_tex_fbo(g_pp.tex[1], g_pp.fbo[1], w, h);
    g_pp.w = w; g_pp.h = h;
}

static void out_ensure(int slot, int w, int h) {
    if (slot < 0 || slot >= kMaxSlots) return;
    auto& s = g_out[slot];
    if (s.w == w && s.h == h) return;
    if (s.fbo) { glDeleteFramebuffers(1, &s.fbo); glDeleteTextures(1, &s.tex); s.fbo = s.tex = 0; }
    make_tex_fbo(s.tex, s.fbo, w, h);
    s.w = w; s.h = h;
}

static void ghost_ensure(int w, int h, float clip_start, GLuint seed_tex) {
    bool reset = (fabsf(g_ghost.clip_start - clip_start) > 0.001f ||
                  g_ghost.w != w || g_ghost.h != h);
    if (reset) {
        if (g_ghost.fbo) { glDeleteFramebuffers(1, &g_ghost.fbo); glDeleteTextures(1, &g_ghost.tex); g_ghost.fbo = g_ghost.tex = 0; }
        make_tex_fbo(g_ghost.tex, g_ghost.fbo, w, h);
        // Seed ghost with the first frame so it starts coherent
        glBindFramebuffer(GL_FRAMEBUFFER, g_ghost.fbo);
        glViewport(0, 0, w, h);
        glUseProgram(g_prog.blit);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, seed_tex);
        glUniform1i(glGetUniformLocation(g_prog.blit, "u_tex"), 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        g_ghost.clip_start = clip_start;
        g_ghost.w = w; g_ghost.h = h;
    }
}

// Draw a fullscreen pass from src_tex into fbo.  The caller sets program uniforms first.
static void draw_pass(GLuint fbo, GLuint src_tex, int w, int h, GLuint prog,
                      const char* tex_uniform = "u_tex", int tex_unit = 0)
{
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0 + tex_unit);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glUniform1i(glGetUniformLocation(prog, tex_uniform), tex_unit);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}


// ── Public API ────────────────────────────────────────────────────────────────

void fx_shader_init() {
    g_prog.grade          = link_prog(k_grade_frag);
    g_prog.blur           = link_prog(k_blur_frag);
    g_prog.chroma_key     = link_prog(k_chroma_key_frag);
    g_prog.glitch         = link_prog(k_glitch_frag);
    g_prog.vhs            = link_prog(k_vhs_frag);
    g_prog.leak           = link_prog(k_leak_frag);
    g_prog.datamosh       = link_prog(k_datamosh_frag);
    g_prog.datamosh_update= link_prog(k_datamosh_update_frag);
    g_prog.blit           = link_prog(k_blit_frag);

    glGenVertexArrays(1, &g_vao);
}

void fx_shader_shutdown() {
    for (auto p : { g_prog.grade, g_prog.blur, g_prog.chroma_key, g_prog.glitch,
                    g_prog.vhs, g_prog.leak, g_prog.datamosh,
                    g_prog.datamosh_update, g_prog.blit })
        if (p) glDeleteProgram(p);

    if (g_pp.fbo[0]) { glDeleteFramebuffers(2, g_pp.fbo); glDeleteTextures(2, g_pp.tex); }
    for (int i = 0; i < kMaxSlots; i++)
        if (g_out[i].fbo) { glDeleteFramebuffers(1, &g_out[i].fbo); glDeleteTextures(1, &g_out[i].tex); }
    if (g_ghost.fbo) { glDeleteFramebuffers(1, &g_ghost.fbo); glDeleteTextures(1, &g_ghost.tex); }
    if (g_vao) glDeleteVertexArrays(1, &g_vao);
}

uintptr_t fx_apply(uintptr_t src_tex_in, int slot, int w, int h,
                   const EffectAccum& ea, const CreativeFXAccum& cfx, float t)
{
    if (!g_prog.grade || slot < 0 || slot >= kMaxSlots) return src_tex_in;

    bool need_grade    = ea.any_color;
    bool need_vig      = ea.any_vignette && ea.vignette > 0.001f;
    bool need_blur     = ea.any_blur     && ea.blur > 0.1f;
    bool need_chroma   = cfx.chroma_key_on;
    bool need_glitch   = cfx.glitch_on   && (cfx.glitch_chroma >= 0.1f || cfx.glitch_jitter >= 0.01f);
    bool need_vhs      = cfx.vhs_on      && (cfx.vhs_noise >= 0.01f || cfx.vhs_bleed >= 0.1f || cfx.vhs_tracking >= 0.01f);
    bool need_leak     = cfx.leak_on     && cfx.leak_intensity > 0.01f;
    bool need_datamosh = cfx.datamosh_on;

    if (!need_grade && !need_vig && !need_blur && !need_chroma &&
        !need_glitch && !need_vhs && !need_leak && !need_datamosh)
        return src_tex_in;

    pp_ensure(w, h);
    out_ensure(slot, w, h);

    // Save GL state
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    GLint prev_vp[4];
    glGetIntegerv(GL_VIEWPORT, prev_vp);

    glBindVertexArray(g_vao);

    GLuint cur = (GLuint)src_tex_in;
    int pslot = 0;  // which ping-pong slot to write to next

    // Helper: run a single-texture pass, advance cur and pslot
    auto run1 = [&](GLuint prog) {
        draw_pass(g_pp.fbo[pslot], cur, w, h, prog);
        cur = g_pp.tex[pslot];
        pslot ^= 1;
    };

    // ── Grade + vignette ─────────────────────────────────────────────────────
    if (need_grade || need_vig) {
        GLuint p = g_prog.grade;
        glUseProgram(p);
        glUniform1f(glGetUniformLocation(p, "u_brightness"), need_grade ? ea.brightness : 0.f);
        glUniform1f(glGetUniformLocation(p, "u_contrast"),   need_grade ? ea.contrast   : 1.f);
        glUniform1f(glGetUniformLocation(p, "u_saturation"), need_grade ? ea.saturation : 1.f);
        glUniform1f(glGetUniformLocation(p, "u_hue"),        need_grade ? ea.hue        : 0.f);
        glUniform1f(glGetUniformLocation(p, "u_vignette"),   need_vig   ? ea.vignette   : 0.f);
        run1(p);
    }

    // ── Blur (2-pass separable) ───────────────────────────────────────────────
    if (need_blur) {
        GLuint p = g_prog.blur;
        // Horizontal
        glUseProgram(p);
        glUniform2f(glGetUniformLocation(p, "u_dir"), 1.f / (float)w, 0.f);
        glUniform1f(glGetUniformLocation(p, "u_sigma"), ea.blur);
        run1(p);
        // Vertical
        glUniform2f(glGetUniformLocation(p, "u_dir"), 0.f, 1.f / (float)h);
        glUniform1f(glGetUniformLocation(p, "u_sigma"), ea.blur);
        run1(p);
    }

    // ── Chroma key ───────────────────────────────────────────────────────────
    if (need_chroma) {
        GLuint p = g_prog.chroma_key;
        glUseProgram(p);
        glUniform3f(glGetUniformLocation(p, "u_key_color"),
                    cfx.chroma_key_r, cfx.chroma_key_g, cfx.chroma_key_b);
        glUniform1f(glGetUniformLocation(p, "u_threshold"), cfx.chroma_key_threshold);
        glUniform1f(glGetUniformLocation(p, "u_softness"),  cfx.chroma_key_softness);
        run1(p);
    }

    // ── Glitch ───────────────────────────────────────────────────────────────
    if (need_glitch) {
        GLuint p = g_prog.glitch;
        glUseProgram(p);
        glUniform1f(glGetUniformLocation(p, "u_chroma"), cfx.glitch_chroma / (float)w);
        glUniform1f(glGetUniformLocation(p, "u_jitter"), cfx.glitch_jitter);
        glUniform1f(glGetUniformLocation(p, "u_time"),   t);
        run1(p);
    }

    // ── VHS ──────────────────────────────────────────────────────────────────
    if (need_vhs) {
        GLuint p = g_prog.vhs;
        glUseProgram(p);
        glUniform1f(glGetUniformLocation(p, "u_noise"),    cfx.vhs_noise);
        glUniform1f(glGetUniformLocation(p, "u_bleed"),    cfx.vhs_bleed / (float)w);
        glUniform1f(glGetUniformLocation(p, "u_tracking"), cfx.vhs_tracking);
        glUniform1f(glGetUniformLocation(p, "u_time"),     t);
        run1(p);
    }

    // ── Light leak ───────────────────────────────────────────────────────────
    if (need_leak) {
        GLuint p = g_prog.leak;
        glUseProgram(p);
        glUniform1f(glGetUniformLocation(p, "u_intensity"), cfx.leak_intensity);
        glUniform1f(glGetUniformLocation(p, "u_speed"),     cfx.leak_speed);
        glUniform1f(glGetUniformLocation(p, "u_time"),      t);
        run1(p);
    }

    // ── Datamosh ─────────────────────────────────────────────────────────────
    if (need_datamosh) {
        GLuint pre_mosh = cur;  // video frame before ghost blend — needed for ghost update

        ghost_ensure(w, h, cfx.datamosh_clip_start, cur);

        // Step 1: blend(src, ghost) → pp[pslot]  (output result)
        {
            int out_pp = pslot;
            glBindFramebuffer(GL_FRAMEBUFFER, g_pp.fbo[out_pp]);
            glViewport(0, 0, w, h);
            GLuint p = g_prog.datamosh;
            glUseProgram(p);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, cur);
            glUniform1i(glGetUniformLocation(p, "u_src"), 0);
            glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, g_ghost.tex);
            glUniform1i(glGetUniformLocation(p, "u_ghost"), 1);
            glUniform1f(glGetUniformLocation(p, "u_intensity"),     cfx.datamosh_intensity);
            glUniform1f(glGetUniformLocation(p, "u_bleedback"),     cfx.datamosh_bleedback);
            float t_in = cfx.datamosh_clip_start >= 0.f ? t - cfx.datamosh_clip_start : 0.f;
            glUniform1f(glGetUniformLocation(p, "u_t_in_clip"),     t_in);
            glUniform1f(glGetUniformLocation(p, "u_clip_duration"), cfx.datamosh_clip_duration);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            cur = g_pp.tex[out_pp];
            pslot ^= 1;
        }

        // Step 2: new_ghost = mix(ghost, src, decay) → pp[pslot]  (ghost is still readable)
        {
            int tmp_pp = pslot;
            glBindFramebuffer(GL_FRAMEBUFFER, g_pp.fbo[tmp_pp]);
            glViewport(0, 0, w, h);
            GLuint p = g_prog.datamosh_update;
            glUseProgram(p);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, pre_mosh);
            glUniform1i(glGetUniformLocation(p, "u_src"), 0);
            glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, g_ghost.tex);
            glUniform1i(glGetUniformLocation(p, "u_ghost"), 1);
            glUniform1f(glGetUniformLocation(p, "u_decay"), cfx.datamosh_decay);
            glDrawArrays(GL_TRIANGLES, 0, 3);

            // Step 3: blit new ghost to ghost FBO (overwrite ghost.tex with new value)
            draw_pass(g_ghost.fbo, g_pp.tex[tmp_pp], w, h, g_prog.blit);

            pslot ^= 1;
        }
    }

    // ── Write final result to stable per-slot output ──────────────────────────
    draw_pass(g_out[slot].fbo, cur, w, h, g_prog.blit);

    // Restore GL state
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    glBindVertexArray(0);

    return (uintptr_t)g_out[slot].tex;
}
