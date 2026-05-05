#include "fx_shader.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <cstdio>
#include <cmath>

// ── Shader sources ────────────────────────────────────────────────────────────

static const char* kVS = R"glsl(
#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }
)glsl";

// Color grade: brightness (additive), contrast (around 0.5 grey), saturation, hue rotation
static const char* kFS_GRADE = R"glsl(
#version 330 core
uniform sampler2D uTex;
uniform float uBrightness;
uniform float uContrast;
uniform float uSaturation;
uniform float uHue;   // degrees
in vec2 vUV;
out vec4 FragColor;

vec3 hue_rotate(vec3 c, float deg) {
    float rad = deg * 3.14159265 / 180.0;
    float cosA = cos(rad), sinA = sin(rad);
    // Rodrigues rotation around (1,1,1)/sqrt(3)
    float sq3 = 0.57735026919;
    mat3 R = mat3(
        cosA+(1.0-cosA)/3.0,          (1.0-cosA)/3.0+sq3*sinA,   (1.0-cosA)/3.0-sq3*sinA,
        (1.0-cosA)/3.0-sq3*sinA,      cosA+(1.0-cosA)/3.0,        (1.0-cosA)/3.0+sq3*sinA,
        (1.0-cosA)/3.0+sq3*sinA,      (1.0-cosA)/3.0-sq3*sinA,   cosA+(1.0-cosA)/3.0
    );
    return clamp(R * c, 0.0, 1.0);
}

void main() {
    vec3 c = texture(uTex, vUV).rgb;
    c = clamp(c + uBrightness, 0.0, 1.0);
    c = clamp((c - 0.5) * uContrast + 0.5, 0.0, 1.0);
    float lum = dot(c, vec3(0.299, 0.587, 0.114));
    c = clamp(mix(vec3(lum), c, uSaturation), 0.0, 1.0);
    if (abs(uHue) > 0.1) c = hue_rotate(c, uHue);
    FragColor = vec4(c, 1.0);
}
)glsl";

// Separable Gaussian blur — horizontal pass
static const char* kFS_BLUR_H = R"glsl(
#version 330 core
uniform sampler2D uTex;
uniform float uSigma;
uniform float uTexW;
in vec2 vUV;
out vec4 FragColor;
void main() {
    if (uSigma < 0.1) { FragColor = texture(uTex, vUV); return; }
    float dx = 1.0 / uTexW;
    float sig2 = uSigma * uSigma;
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    int R = int(ceil(uSigma * 2.5));
    for (int i = -R; i <= R; ++i) {
        float w = exp(-float(i*i) / (2.0*sig2));
        sum  += texture(uTex, vUV + vec2(float(i)*dx, 0.0)).rgb * w;
        wsum += w;
    }
    FragColor = vec4(sum / wsum, 1.0);
}
)glsl";

// Separable Gaussian blur — vertical pass
static const char* kFS_BLUR_V = R"glsl(
#version 330 core
uniform sampler2D uTex;
uniform float uSigma;
uniform float uTexH;
in vec2 vUV;
out vec4 FragColor;
void main() {
    if (uSigma < 0.1) { FragColor = texture(uTex, vUV); return; }
    float dy = 1.0 / uTexH;
    float sig2 = uSigma * uSigma;
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    int R = int(ceil(uSigma * 2.5));
    for (int i = -R; i <= R; ++i) {
        float w = exp(-float(i*i) / (2.0*sig2));
        sum  += texture(uTex, vUV + vec2(0.0, float(i)*dy)).rgb * w;
        wsum += w;
    }
    FragColor = vec4(sum / wsum, 1.0);
}
)glsl";

// ── GL helpers ────────────────────────────────────────────────────────────────

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512]; glGetShaderInfoLog(s, sizeof(buf), nullptr, buf);
        fprintf(stderr, "Shader compile error: %s\n", buf);
        glDeleteShader(s); return 0;
    }
    return s;
}

static GLuint link_program(const char* vs_src, const char* fs_src) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) { glDeleteShader(vs); glDeleteShader(fs); return 0; }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[512]; glGetProgramInfoLog(p, sizeof(buf), nullptr, buf);
        fprintf(stderr, "Shader link error: %s\n", buf);
        glDeleteProgram(p); return 0;
    }
    return p;
}

// ── Glitch shader ─────────────────────────────────────────────────────────────

static const char* kFS_GLITCH = R"glsl(
#version 330 core
uniform sampler2D uTex;
uniform float uChroma;
uniform float uJitter;
uniform float uTime;
uniform float uTexW;
uniform float uTexH;
in vec2 vUV;
out vec4 FragColor;

float hash1(float n) { return fract(sin(n) * 43758.5453123); }

void main() {
    vec2 uv = vUV;

    if (uJitter > 0.001) {
        float row = floor(uv.y * uTexH);
        float rnd  = hash1(row * 0.1 + floor(uTime * 12.0) * 7.3);
        float rnd2 = hash1(row * 0.7 + floor(uTime *  8.0) * 3.1);
        if (rnd > 1.0 - uJitter * 0.4)
            uv.x += (rnd2 - 0.5) * uJitter * 0.12;
    }

    float dx = uChroma / uTexW;
    float r = texture(uTex, clamp(vec2(uv.x + dx, uv.y), 0.0, 1.0)).r;
    float g = texture(uTex, uv).g;
    float b = texture(uTex, clamp(vec2(uv.x - dx, uv.y), 0.0, 1.0)).b;
    FragColor = vec4(r, g, b, 1.0);
}
)glsl";

// ── VHS shader ────────────────────────────────────────────────────────────────

static const char* kFS_VHS = R"glsl(
#version 330 core
uniform sampler2D uTex;
uniform float uNoise;
uniform float uBleed;
uniform float uTracking;
uniform float uTime;
uniform float uTexW;
uniform float uTexH;
in vec2 vUV;
out vec4 FragColor;

float hash2(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5); }
float hash1(float n) { return fract(sin(n) * 43758.5453123); }

void main() {
    vec2 uv = vUV;

    if (uTracking > 0.001) {
        float track_pos = fract(uTime * 0.17 + 0.3);
        float dist = abs(uv.y - track_pos);
        float band = smoothstep(0.04, 0.0, dist);
        uv.x = fract(uv.x + band * uTracking * 0.06 * (hash1(floor(uTime * 7.0)) - 0.5));
    }

    float dx = uBleed / uTexW;
    float r = texture(uTex, clamp(vec2(uv.x + dx,        uv.y), 0.0, 1.0)).r;
    float g = texture(uTex, uv).g;
    float b = texture(uTex, clamp(vec2(uv.x - dx * 0.4,  uv.y), 0.0, 1.0)).b;
    vec3 c = vec3(r, g, b);

    if (uNoise > 0.001) {
        float n = hash2(uv + vec2(fract(uTime * 37.3), fract(uTime * 19.7)));
        c += (n - 0.5) * uNoise * 0.25;
    }

    c *= 1.0 - 0.06 * abs(sin(uv.y * uTexH * 3.14159));
    FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
)glsl";

// ── State ─────────────────────────────────────────────────────────────────────

static GLuint g_prog_grade  = 0;
static GLuint g_prog_blur_h = 0;
static GLuint g_prog_blur_v = 0;
static GLuint g_prog_glitch = 0;
static GLuint g_prog_vhs    = 0;
static GLuint g_vao = 0, g_vbo = 0;

// Ping-pong FBO pair (slots 0,1) + dedicated creative FX FBOs (slots 2,3)
static GLuint g_fbo[4]     = {};
static GLuint g_fbo_tex[4] = {};
static int    g_fbo_w      = 0;
static int    g_fbo_h      = 0;

// Fullscreen quad: pos(xy) + uv
static const float kQuad[] = {
    -1.f,-1.f, 0.f,1.f,
     1.f,-1.f, 1.f,1.f,
     1.f, 1.f, 1.f,0.f,
    -1.f,-1.f, 0.f,1.f,
     1.f, 1.f, 1.f,0.f,
    -1.f, 1.f, 0.f,0.f,
};

void fx_shader_init() {
    g_prog_grade  = link_program(kVS, kFS_GRADE);
    g_prog_blur_h = link_program(kVS, kFS_BLUR_H);
    g_prog_blur_v = link_program(kVS, kFS_BLUR_V);
    g_prog_glitch = link_program(kVS, kFS_GLITCH);
    g_prog_vhs    = link_program(kVS, kFS_VHS);

    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    glGenFramebuffers(4, g_fbo);
    glGenTextures(4, g_fbo_tex);
}

void fx_shader_shutdown() {
    glDeleteProgram(g_prog_grade);
    glDeleteProgram(g_prog_blur_h);
    glDeleteProgram(g_prog_blur_v);
    glDeleteProgram(g_prog_glitch);
    glDeleteProgram(g_prog_vhs);
    glDeleteVertexArrays(1, &g_vao);
    glDeleteBuffers(1, &g_vbo);
    glDeleteFramebuffers(4, g_fbo);
    glDeleteTextures(4, g_fbo_tex);
    g_prog_grade = g_prog_blur_h = g_prog_blur_v = g_prog_glitch = g_prog_vhs = 0;
    g_vao = g_vbo = 0;
    for (int i=0;i<4;++i) { g_fbo[i]=0; g_fbo_tex[i]=0; }
    g_fbo_w = g_fbo_h = 0;
}

static void ensure_fbo(int w, int h) {
    if (w == g_fbo_w && h == g_fbo_h) return;
    g_fbo_w = w; g_fbo_h = h;
    for (int i = 0; i < 4; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_fbo[i]);
        glBindTexture(GL_TEXTURE_2D, g_fbo_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fbo_tex[i], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void draw_quad(GLuint prog, GLuint src_tex, int w, int h) {
    glViewport(0, 0, w, h);
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

uintptr_t fx_apply(uintptr_t src_tex,
                   float brightness, float contrast,
                   float saturation, float hue_deg,
                   float blur_sigma) {
    bool need_grade = (fabsf(brightness) > 0.005f || fabsf(contrast - 1.f) > 0.005f ||
                       fabsf(saturation - 1.f) > 0.005f || fabsf(hue_deg) > 0.5f);
    bool need_blur  = (blur_sigma > 0.1f);
    if (!need_grade && !need_blur) return src_tex;
    if (!g_prog_grade || !g_vao) return src_tex;

    // Query actual texture dimensions from GL
    GLint tw = 1, th = 1;
    glBindTexture(GL_TEXTURE_2D, (GLuint)src_tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    if (tw < 1 || th < 1) return src_tex;

    ensure_fbo(tw, th);

    // Save GL state
    GLint prev_fbo; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    GLint vp[4];    glGetIntegerv(GL_VIEWPORT, vp);

    GLuint cur_src = (GLuint)src_tex;
    int    ping = 0;  // which FBO we write to next

    // Pass 1: color grade → fbo[ping]
    if (need_grade) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_fbo[ping]);
        GLuint p = g_prog_grade;
        glUseProgram(p);
        glUniform1f(glGetUniformLocation(p,"uBrightness"), brightness);
        glUniform1f(glGetUniformLocation(p,"uContrast"),   contrast);
        glUniform1f(glGetUniformLocation(p,"uSaturation"), saturation);
        glUniform1f(glGetUniformLocation(p,"uHue"),        hue_deg);
        draw_quad(p, cur_src, tw, th);
        cur_src = g_fbo_tex[ping];
        ping ^= 1;
    }

    // Pass 2: blur horizontal → fbo[ping]
    if (need_blur) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_fbo[ping]);
        GLuint p = g_prog_blur_h;
        glUseProgram(p);
        glUniform1f(glGetUniformLocation(p,"uSigma"), blur_sigma);
        glUniform1f(glGetUniformLocation(p,"uTexW"),  (float)tw);
        draw_quad(p, cur_src, tw, th);
        cur_src = g_fbo_tex[ping];
        ping ^= 1;

        // Pass 3: blur vertical → fbo[ping]
        glBindFramebuffer(GL_FRAMEBUFFER, g_fbo[ping]);
        p = g_prog_blur_v;
        glUseProgram(p);
        glUniform1f(glGetUniformLocation(p,"uSigma"), blur_sigma);
        glUniform1f(glGetUniformLocation(p,"uTexH"),  (float)th);
        draw_quad(p, cur_src, tw, th);
        cur_src = g_fbo_tex[ping];
    }

    // Restore GL state
    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glUseProgram(0);

    return (uintptr_t)cur_src;
}

template<typename F>
static uintptr_t fx_apply_creative(GLuint prog, int fbo_slot, uintptr_t src_tex, F set_uniforms) {
    GLint tw=1, th=1;
    glBindTexture(GL_TEXTURE_2D, (GLuint)src_tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    if (tw < 1 || th < 1) return src_tex;
    ensure_fbo(tw, th);

    GLint prev_fbo; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    GLint vp[4];    glGetIntegerv(GL_VIEWPORT, vp);

    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo[fbo_slot]);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
    glUniform1f(glGetUniformLocation(prog, "uTexW"), (float)tw);
    glUniform1f(glGetUniformLocation(prog, "uTexH"), (float)th);
    set_uniforms(prog);
    draw_quad(prog, (GLuint)src_tex, tw, th);

    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glUseProgram(0);

    return (uintptr_t)g_fbo_tex[fbo_slot];
}

uintptr_t fx_apply_glitch(uintptr_t src_tex, float chroma_px, float jitter, float time) {
    if (chroma_px < 0.1f && jitter < 0.01f) return src_tex;
    if (!g_prog_glitch || !g_vao) return src_tex;
    return fx_apply_creative(g_prog_glitch, 2, src_tex, [&](GLuint p) {
        glUniform1f(glGetUniformLocation(p, "uChroma"), chroma_px);
        glUniform1f(glGetUniformLocation(p, "uJitter"), jitter);
        glUniform1f(glGetUniformLocation(p, "uTime"),   time);
    });
}

uintptr_t fx_apply_vhs(uintptr_t src_tex, float noise, float bleed_px, float tracking, float time) {
    if (noise < 0.01f && bleed_px < 0.1f && tracking < 0.01f) return src_tex;
    if (!g_prog_vhs || !g_vao) return src_tex;
    return fx_apply_creative(g_prog_vhs, 3, src_tex, [&](GLuint p) {
        glUniform1f(glGetUniformLocation(p, "uNoise"),    noise);
        glUniform1f(glGetUniformLocation(p, "uBleed"),    bleed_px);
        glUniform1f(glGetUniformLocation(p, "uTracking"), tracking);
        glUniform1f(glGetUniformLocation(p, "uTime"),     time);
    });
}
