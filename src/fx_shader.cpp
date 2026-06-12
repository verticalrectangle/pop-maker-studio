#include "fx_shader.h"
#include "bg_presets.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>
#include <cmath>
#include <unordered_map>

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
    float t = clamp((dist - u_threshold) / soft, 0.0, 1.0);
    float alpha = t * t * (3.0 - 2.0 * t);
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
uniform float u_chroma;    // chroma offset as fraction of width
uniform float u_jitter;    // row-jitter intensity 0..1
uniform float u_time;
uniform float u_tex_h;     // texture height in pixels (avoids textureSize driver bugs)

float hash(float n) { return fract(sin(n) * 43758.5453); }

void main() {
    float y_id = floor(v_uv.y * u_tex_h);
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

// Datamosh color bleed — uses channel dominance as a corruption matte.
// Neon/saturated pixels (JPEG decode artifacts) get R and B channels
// dragged horizontally in opposite directions, creating a chroma-split
// that makes the corruption look like a composited colour layer.
// Clean/neutral pixels have low dominance and get no bleed at all.
static const char* k_datamosh_frag = R"glsl(
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_spread;
uniform float u_tex_w;

void main() {
    vec3 col = texture(u_tex, v_uv).rgb;

    // Corruption matte: how saturated/extreme is this pixel?
    float lo = min(col.r, min(col.g, col.b));
    float hi = max(col.r, max(col.g, col.b));
    float matte = smoothstep(0.25, 0.75, hi - lo);

    // Horizontal chroma bleed — R forward, B back, G stays
    float bleed = matte * u_spread * 40.0 / u_tex_w;
    float r = texture(u_tex, v_uv + vec2( bleed,       0.0)).r;
    float b = texture(u_tex, v_uv - vec2( bleed * 0.6, 0.0)).b;

    frag = vec4(r, col.g, b, 1.0);
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

// Scene compositor — alpha-correct "over" compositing of a clip onto the scene.
// Inputs: u_scene (current accumulated scene), u_clip (new clip to layer on top).
// u_center: clip centre in canvas pixels (Y-down, origin = top-left).
// u_half:   clip half-extents in canvas pixels.
// u_cossin: (cos(rotation), sin(rotation)).
// u_alpha:  global clip alpha multiplier.
//
// GL fragment Y-up vs canvas Y-down: cy_canvas = canvas_h - gl_FragCoord.y
// Clip UV: stbi stores top-of-image at data[0] → uploaded to GL t=0. Since our
// Y-math gives clip_uv.y=0 at the top of the canvas, no extra flip is needed.
// Output alpha is straight (not premultiplied).
static const char* k_composite_frag = R"glsl(
#version 330 core
out vec4 fragColor;
uniform sampler2D u_scene;
uniform sampler2D u_clip;
uniform vec2  u_canvas;
uniform vec2  u_center;
uniform vec2  u_half;
uniform vec2  u_cossin;
uniform float u_alpha;
uniform vec2  u_uv0;   // source UV window (crop) — quad maps to [u_uv0, u_uv1]
uniform vec2  u_uv1;
void main() {
    vec2 scene_uv = gl_FragCoord.xy / u_canvas;
    vec4 scene    = texture(u_scene, scene_uv);
    float cx = gl_FragCoord.x;
    float cy = u_canvas.y - gl_FragCoord.y;
    float dx = cx - u_center.x, dy = cy - u_center.y;
    float lx =  dx * u_cossin.x + dy * u_cossin.y;
    float ly = -dx * u_cossin.y + dy * u_cossin.x;
    vec2 clip_uv = vec2(lx / (u_half.x * 2.0) + 0.5,
                        ly / (u_half.y * 2.0) + 0.5);
    vec4 clip = vec4(0.0);
    if (clip_uv.x >= 0.0 && clip_uv.x <= 1.0 &&
        clip_uv.y >= 0.0 && clip_uv.y <= 1.0)
        clip = texture(u_clip, mix(u_uv0, u_uv1, clip_uv));
    float src_a = clip.a * u_alpha;
    fragColor = vec4(clip.rgb * src_a + scene.rgb * (1.0 - src_a),
                     src_a + scene.a  * (1.0 - src_a));
}
)glsl";

// Amount blend — mixes original (u_tex, unit 0) with effect (u_effect, unit 1)
static const char* k_blend_frag = R"glsl(
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform sampler2D u_effect;
uniform float u_amount;
void main() {
    frag = mix(texture(u_tex, v_uv), texture(u_effect, v_uv), u_amount);
}
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

static GLuint link_prog2(const char* vert_src, const char* frag_src) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   vert_src);
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


// ── Generated shader strings ───────────────────────────────────────────────────
#include "generated/fx_shader_strings.h"

// ── GPU state ─────────────────────────────────────────────────────────────────

static struct {
    GLuint grade = 0, blur = 0, chroma_key = 0, glitch = 0;
    GLuint vhs = 0, leak = 0, datamosh = 0, blit = 0, blend = 0;
    GLuint composite = 0;
} g_prog;

// Generated effects use a map keyed by (int)FXType — no struct fields needed.
static std::unordered_map<int, GLuint> g_gen_progs;

static GLuint g_vao = 0;  // empty VAO required by GL core profile

// Scratch ping-pong (temporary, used within a single fx_apply call)
static struct {
    GLuint fbo[2] = {}, tex[2] = {};
    int w = 0, h = 0;
} g_pp;

// Scene compositor ping-pong FBOs
static struct {
    GLuint fbo[2] = {}, tex[2] = {};
    int w = 0, h = 0;
    int active = 0;   // which slot holds the current accumulated scene
    bool begun = false;
} g_scene;

// 1×1 solid-colour texture for scene_add_solid
static GLuint g_solid_tex = 0;

// Per-slot stable output textures — indexed by fx_apply's 'slot' argument.
// These persist between pass chains so deferred ImDrawList commands are safe.
// Slot map: [0 .. MAX*2-1] per-clip fx (== video decode slot) + scene/mirror
// specials at the top of that range; [MAX*2] mirror face-warp; [MAX*2+1 ..
// MAX*4] face-warp/sprite outputs for clips (one per video slot — the warp
// output must outlive the clip's fx_apply output until scene composite);
// [MAX*4+1] (kMaxSlots-1) fx-picker preview thumbnails. The preview slot
// previously shared MAX*2 with the mirror face-warp — now exclusive.
static const int kMaxSlots = MAX_VIDEO_TRACKS * 4 + 2;
static const int kFaceClipSlotBase = MAX_VIDEO_TRACKS * 2 + 1;

int fx_face_clip_slot(int video_slot) {
    if (video_slot < 0) video_slot = 0;
    return kFaceClipSlotBase + (video_slot % (MAX_VIDEO_TRACKS * 2));
}

static struct {
    GLuint fbo = 0, tex = 0;
    int w = 0, h = 0;
} g_out[kMaxSlots];

// Per-slot BG output FBOs (for bg_render_to_texture)
static struct {
    GLuint fbo = 0, tex = 0;
    int w = 0, h = 0;
} g_bg_out[MAX_BG_SLOTS];

// Self-contained mini renderer for BG ImDrawList → FBO.
// Uses its own VAO/VBO/EBO so it never touches ImGui's backend state mid-frame.
static const char* k_bg_dl_vert = R"glsl(
#version 330 core
layout(location=0) in vec2 a_pos;
layout(location=1) in vec2 a_uv;
layout(location=2) in vec4 a_col;
out vec4 v_col;
uniform vec2 u_size;
void main() {
    v_col = a_col;
    gl_Position = vec4(a_pos.x/u_size.x*2.0-1.0, 1.0-a_pos.y/u_size.y*2.0, 0.0, 1.0);
}
)glsl";

static const char* k_bg_dl_frag = R"glsl(
#version 330 core
in vec4 v_col;
out vec4 frag;
void main() { frag = v_col; }
)glsl";

static GLuint g_bg_prog = 0;
static GLuint g_bg_vao  = 0, g_bg_vbo = 0, g_bg_ebo = 0;

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

// Draw a fullscreen pass from src_tex into fbo.  The caller sets program uniforms first.
static void draw_pass(GLuint fbo, GLuint src_tex, int w, int h, GLuint prog,
                      const char* tex_uniform = "u_tex", int tex_unit = 0);  // defined below

// Blend original_tex + effect_tex → out_fbo using g_prog.blend.
// Called after each generated effect pass when amount < 1.
static void draw_blend_pass(GLuint original_tex, GLuint effect_tex, float amount,
                            GLuint out_fbo, int w, int h)
{
    glBindFramebuffer(GL_FRAMEBUFFER, out_fbo);
    glViewport(0, 0, w, h);
    glUseProgram(g_prog.blend);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, original_tex);
    glUniform1i(glGetUniformLocation(g_prog.blend, "u_tex"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, effect_tex);
    glUniform1i(glGetUniformLocation(g_prog.blend, "u_effect"), 1);

    glUniform1f(glGetUniformLocation(g_prog.blend, "u_amount"), amount);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE0);
}

// (definition of draw_pass — forward-declared above)
static void draw_pass(GLuint fbo, GLuint src_tex, int w, int h, GLuint prog,
                      const char* tex_uniform, int tex_unit)
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

// Forward declaration — defined by the generated include below.
static void fx_generated_init();

void fx_shader_init() {
    g_prog.grade          = link_prog(k_grade_frag);
    g_prog.blur           = link_prog(k_blur_frag);
    g_prog.chroma_key     = link_prog(k_chroma_key_frag);
    g_prog.glitch         = link_prog(k_glitch_frag);
    g_prog.vhs            = link_prog(k_vhs_frag);
    g_prog.leak           = link_prog(k_leak_frag);
    g_prog.datamosh       = link_prog(k_datamosh_frag);
    g_prog.blit           = link_prog(k_blit_frag);
    g_prog.blend          = link_prog(k_blend_frag);
    g_prog.composite      = link_prog(k_composite_frag);

    glGenVertexArrays(1, &g_vao);

    // 1×1 white texture used by scene_add_solid
    glGenTextures(1, &g_solid_tex);
    glBindTexture(GL_TEXTURE_2D, g_solid_tex);
    uint8_t white[4] = {255, 255, 255, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    fx_generated_init();

    // BG mini renderer
    g_bg_prog = link_prog2(k_bg_dl_vert, k_bg_dl_frag);
    glGenVertexArrays(1, &g_bg_vao);
    glGenBuffers(1, &g_bg_vbo);
    glGenBuffers(1, &g_bg_ebo);
    glBindVertexArray(g_bg_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_bg_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_bg_ebo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT,         GL_FALSE,      sizeof(ImDrawVert), (void*)offsetof(ImDrawVert, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT,         GL_FALSE,      sizeof(ImDrawVert), (void*)offsetof(ImDrawVert, uv));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE,       sizeof(ImDrawVert), (void*)offsetof(ImDrawVert, col));
    glBindVertexArray(0);
}

#include "generated/fx_shader_init.h"

void fx_shader_shutdown() {
    for (auto p : { g_prog.grade, g_prog.blur, g_prog.chroma_key, g_prog.glitch,
                    g_prog.vhs, g_prog.leak, g_prog.datamosh, g_prog.blit, g_prog.blend,
                    g_prog.composite })
        if (p) glDeleteProgram(p);
    for (auto& [k, v] : g_gen_progs) if (v) glDeleteProgram(v);
    g_gen_progs.clear();

    if (g_pp.fbo[0]) { glDeleteFramebuffers(2, g_pp.fbo); glDeleteTextures(2, g_pp.tex); }
    for (int i = 0; i < kMaxSlots; i++)
        if (g_out[i].fbo) { glDeleteFramebuffers(1, &g_out[i].fbo); glDeleteTextures(1, &g_out[i].tex); }
    for (auto& b : g_bg_out)
        if (b.fbo) { glDeleteFramebuffers(1, &b.fbo); glDeleteTextures(1, &b.tex); b = {}; }
    if (g_scene.fbo[0]) { glDeleteFramebuffers(2, g_scene.fbo); glDeleteTextures(2, g_scene.tex); }
    if (g_solid_tex) glDeleteTextures(1, &g_solid_tex);
    if (g_vao) glDeleteVertexArrays(1, &g_vao);
    if (g_bg_prog) glDeleteProgram(g_bg_prog);
    if (g_bg_vao)  glDeleteVertexArrays(1, &g_bg_vao);
    if (g_bg_vbo)  glDeleteBuffers(1, &g_bg_vbo);
    if (g_bg_ebo)  glDeleteBuffers(1, &g_bg_ebo);
}

// ── Face warp (filters) ───────────────────────────────────────────────────────
// One pass, up to 12 local "bumps": radial scale (enlarge/shrink) + content
// shift with gaussian falloff. Landmark logic stays on the CPU; this shader
// is dumb on purpose.
static const char* k_face_warp_fs = R"(#version 330 core
in vec2 v_uv; out vec4 frag;
uniform sampler2D u_tex;
uniform int  u_n;
uniform vec4 u_ba[12];   // cx, cy, radius, scale
uniform vec4 u_bb[12];   // dx, dy, aspect, _
void main() {
    vec2 uv = v_uv;
    for (int i = 0; i < u_n; ++i) {
        vec2 c = u_ba[i].xy;
        float r = max(u_ba[i].z, 1e-4);
        vec2 d = v_uv - c;
        d.x *= u_bb[i].z;            // aspect-correct the falloff
        float g = exp(-dot(d, d) / (r * r * 0.45));
        uv -= (u_bb[i].xy + (v_uv - c) * u_ba[i].w) * g;
    }
    frag = texture(u_tex, clamp(uv, vec2(0.001), vec2(0.999)));
}
)";
static GLuint g_face_warp_prog = 0;

uintptr_t face_warp_apply(uintptr_t src_tex, int slot, int w, int h,
                          const float* bumps, int n_bumps) {
    if (n_bumps <= 0 || slot < 0 || slot >= kMaxSlots || w <= 0 || h <= 0)
        return src_tex;
    if (!g_face_warp_prog) {
        g_face_warp_prog = link_prog(k_face_warp_fs);
        if (!g_face_warp_prog) return src_tex;
    }
    if (n_bumps > 12) n_bumps = 12;

    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    GLint prev_vp[4];
    glGetIntegerv(GL_VIEWPORT, prev_vp);

    out_ensure(slot, w, h);
    glBindVertexArray(g_vao);
    glBindFramebuffer(GL_FRAMEBUFFER, g_out[slot].fbo);
    glViewport(0, 0, w, h);
    glUseProgram(g_face_warp_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)src_tex);
    glUniform1i(glGetUniformLocation(g_face_warp_prog, "u_tex"), 0);
    glUniform1i(glGetUniformLocation(g_face_warp_prog, "u_n"), n_bumps);
    float ba[48] = {}, bb[48] = {};
    float aspect = (float)w / (float)h;
    for (int i = 0; i < n_bumps; ++i) {
        ba[i*4+0] = bumps[i*6+0];           // cx
        ba[i*4+1] = bumps[i*6+1];           // cy
        ba[i*4+2] = bumps[i*6+2];           // radius
        ba[i*4+3] = bumps[i*6+3];           // scale
        bb[i*4+0] = bumps[i*6+4];           // dx
        bb[i*4+1] = bumps[i*6+5];           // dy
        bb[i*4+2] = aspect;
    }
    glUniform4fv(glGetUniformLocation(g_face_warp_prog, "u_ba"), 12, ba);
    glUniform4fv(glGetUniformLocation(g_face_warp_prog, "u_bb"), 12, bb);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    return (uintptr_t)g_out[slot].tex;
}

// ── Face sprites (doggy ears/nose/tongue at playback/export) ─────────────────
// Attribute-less quad: corners come in as uniforms, gl_VertexID picks them.
// Position UV convention matches the fx pipeline: (0,0) = image top-left =
// texture row 0 = NDC (-1,-1).
static const char* k_sprite_vs = R"(#version 330 core
uniform vec2 u_p[4];     // tl, tr, br, bl in target UV
uniform vec2 u_uv[4];
out vec2 v_uv;
void main() {
    int idx[6] = int[6](0, 1, 2, 0, 2, 3);
    int i = idx[gl_VertexID];
    gl_Position = vec4(u_p[i] * 2.0 - 1.0, 0.0, 1.0);
    v_uv = u_uv[i];
}
)";
static const char* k_sprite_fs = R"(#version 330 core
in vec2 v_uv; out vec4 frag;
uniform sampler2D u_tex;
void main() { frag = texture(u_tex, v_uv); }
)";
static GLuint g_sprite_prog = 0;

uintptr_t face_sprites_apply(uintptr_t src_tex, int slot, int w, int h,
                             const FaceSpriteQuad* quads, int n) {
    if (n <= 0 || slot < 0 || slot >= kMaxSlots || w <= 0 || h <= 0)
        return src_tex;
    if (!g_sprite_prog) {
        g_sprite_prog = link_prog2(k_sprite_vs, k_sprite_fs);
        if (!g_sprite_prog) return src_tex;
    }
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    GLint prev_vp[4];
    glGetIntegerv(GL_VIEWPORT, prev_vp);

    out_ensure(slot, w, h);
    glBindVertexArray(g_vao);
    // face_warp_apply output already lives in this slot — draw in place.
    if ((GLuint)src_tex != g_out[slot].tex)
        draw_pass(g_out[slot].fbo, (GLuint)src_tex, w, h, g_prog.blit);

    glBindFramebuffer(GL_FRAMEBUFFER, g_out[slot].fbo);
    glViewport(0, 0, w, h);
    glUseProgram(g_sprite_prog);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(g_sprite_prog, "u_tex"), 0);
    for (int i = 0; i < n; ++i) {
        const FaceSpriteQuad& q = quads[i];
        if (!q.tex) continue;
        float uv[8] = {q.u0, 0.f, q.u1, 0.f, q.u1, 1.f, q.u0, 1.f};
        glUniform2fv(glGetUniformLocation(g_sprite_prog, "u_p"),  4, &q.p[0][0]);
        glUniform2fv(glGetUniformLocation(g_sprite_prog, "u_uv"), 4, uv);
        glBindTexture(GL_TEXTURE_2D, (GLuint)q.tex);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    return (uintptr_t)g_out[slot].tex;
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
    bool need_datamosh = cfx.datamosh_on && cfx.datamosh_spread > 0.01f;

    if (!need_grade && !need_vig && !need_blur && !need_chroma &&
        !need_glitch && !need_vhs && !need_leak && !need_datamosh && !cfx.any_gen_fx)
        return src_tex_in;

    if (w <= 0 || h <= 0) return src_tex_in;

    // Save GL state BEFORE pp_ensure/out_ensure touch FBO bindings
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    GLint prev_vp[4];
    glGetIntegerv(GL_VIEWPORT, prev_vp);

    pp_ensure(w, h);
    out_ensure(slot, w, h);

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
        glUniform1f(glGetUniformLocation(p, "u_tex_h"),  (float)h);
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

    // ── Datamosh colour bleed ─────────────────────────────────────────────────
    if (need_datamosh) {
        GLuint p = g_prog.datamosh;
        glUseProgram(p);
        glUniform1f(glGetUniformLocation(p, "u_spread"), cfx.datamosh_spread);
        glUniform1f(glGetUniformLocation(p, "u_tex_w"),  (float)w);
        run1(p);
    }

    // ── Generated effects ─────────────────────────────────────────────────────
#include "generated/fx_shader_apply.h"

    // ── Write final result to stable per-slot output ──────────────────────────
    draw_pass(g_out[slot].fbo, cur, w, h, g_prog.blit);

    // Restore GL state
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    glBindVertexArray(0);

    return (uintptr_t)g_out[slot].tex;
}

uintptr_t fx_preview_gen_effect(FXType ft, uintptr_t src_tex, int w, int h, float t) {
    // Build a CreativeFXAccum with the requested effect enabled at default params.
    CreativeFXAccum cfx;
    switch (ft) {
#include "generated/fx_preview_defaults.h"
        default: return src_tex;
    }
    cfx.any_gen_fx = true;  // bypass early-return guard in fx_apply
    EffectAccum ea;
    // Use the last slot as a dedicated preview slot — never used by normal video rendering.
    return fx_apply(src_tex, kMaxSlots - 1, w, h, ea, cfx, t);
}

// ── Scene compositor ──────────────────────────────────────────────────────────

static void scene_ensure(int w, int h) {
    if (g_scene.w == w && g_scene.h == h) return;
    if (g_scene.fbo[0]) {
        glDeleteFramebuffers(2, g_scene.fbo);
        glDeleteTextures(2, g_scene.tex);
        g_scene.fbo[0] = g_scene.fbo[1] = g_scene.tex[0] = g_scene.tex[1] = 0;
    }
    make_tex_fbo(g_scene.tex[0], g_scene.fbo[0], w, h);
    make_tex_fbo(g_scene.tex[1], g_scene.fbo[1], w, h);
    g_scene.w = w;
    g_scene.h = h;
}

void scene_begin(int canvas_w, int canvas_h) {
    if (!g_prog.composite || canvas_w <= 0 || canvas_h <= 0) { g_scene.begun = false; return; }

    // Save GL state BEFORE scene_ensure, which may bind a scene FBO on first call
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    GLint prev_vp[4];
    glGetIntegerv(GL_VIEWPORT, prev_vp);

    scene_ensure(canvas_w, canvas_h);

    g_scene.active = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, g_scene.fbo[0]);
    glViewport(0, 0, canvas_w, canvas_h);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    g_scene.begun = true;
}

void scene_add_layer(uintptr_t clip_tex, float cx, float cy, float hw, float hh,
                     float cos_r, float sin_r, float alpha,
                     float u0, float v0, float u1, float v1)
{
    if (!g_scene.begun || !g_prog.composite) return;
    if (clip_tex == 0 || hw <= 0.f || hh <= 0.f) return;

    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    GLint prev_vp[4];
    glGetIntegerv(GL_VIEWPORT, prev_vp);

    // The composite shader blends src-over in-shader and relies on every
    // fragment of the fullscreen triangle OVERWRITING the destination FBO.
    // GL blending must be off: with GL_BLEND on (left enabled by
    // bg_render_to_texture or the ImGui backend), fragments outside the
    // clip quad output alpha 0 and KEEP the destination's stale pixels from
    // previous frames — moving a layer smears it across the canvas and a
    // background brick appears to cover everything forever. Scissor would
    // similarly leave stale pixels outside its rect.
    GLboolean prev_blend   = glIsEnabled(GL_BLEND);
    GLboolean prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);

    int next = g_scene.active ^ 1;
    glBindVertexArray(g_vao);
    glBindFramebuffer(GL_FRAMEBUFFER, g_scene.fbo[next]);
    glViewport(0, 0, g_scene.w, g_scene.h);
    glUseProgram(g_prog.composite);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_scene.tex[g_scene.active]);
    glUniform1i(glGetUniformLocation(g_prog.composite, "u_scene"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, (GLuint)clip_tex);
    glUniform1i(glGetUniformLocation(g_prog.composite, "u_clip"), 1);

    glUniform2f(glGetUniformLocation(g_prog.composite, "u_canvas"),
                (float)g_scene.w, (float)g_scene.h);
    glUniform2f(glGetUniformLocation(g_prog.composite, "u_center"), cx, cy);
    glUniform2f(glGetUniformLocation(g_prog.composite, "u_half"),   hw, hh);
    glUniform2f(glGetUniformLocation(g_prog.composite, "u_cossin"), cos_r, sin_r);
    glUniform1f(glGetUniformLocation(g_prog.composite, "u_alpha"),  alpha);
    glUniform2f(glGetUniformLocation(g_prog.composite, "u_uv0"),    u0, v0);
    glUniform2f(glGetUniformLocation(g_prog.composite, "u_uv1"),    u1, v1);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE0);

    g_scene.active = next;
    if (prev_blend)   glEnable(GL_BLEND);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    glBindVertexArray(0);
}

void scene_add_solid(float r, float g, float b, float a) {
    if (!g_scene.begun || !g_solid_tex) return;
    // Update solid texture colour
    uint8_t px[4] = {(uint8_t)(r * 255.f), (uint8_t)(g * 255.f),
                     (uint8_t)(b * 255.f), 255};
    glBindTexture(GL_TEXTURE_2D, g_solid_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    // Composite as a full-canvas layer using the clip geometry
    float cw = (float)g_scene.w, ch = (float)g_scene.h;
    scene_add_layer((uintptr_t)g_solid_tex, cw * 0.5f, ch * 0.5f,
                    cw * 0.5f, ch * 0.5f, 1.f, 0.f, a);
}

void scene_apply_fx(int canvas_w, int canvas_h,
                    const EffectAccum& ea, const CreativeFXAccum& cfx, float t)
{
    if (!g_scene.begun) return;
    uintptr_t src = (uintptr_t)g_scene.tex[g_scene.active];
    uintptr_t out = fx_apply(src, kSceneFxSlot, canvas_w, canvas_h, ea, cfx, t);
    if (out == src) return;  // no FX active — nothing to blit back
    fx_blit(out, g_scene.fbo[g_scene.active], canvas_w, canvas_h);
}

uintptr_t scene_result() {
    return g_scene.begun ? (uintptr_t)g_scene.tex[g_scene.active] : 0;
}

uintptr_t bg_render_to_texture(const char* preset_id, int slot,
                                int canvas_w, int canvas_h,
                                float t, float speed, float intensity,
                                const float c1[4], const float c2[4], const float c3[4])
{
    if (slot < 0 || slot >= MAX_BG_SLOTS) return 0;
    if (canvas_w <= 0 || canvas_h <= 0) return 0;
    auto& buf = g_bg_out[slot];

    // Save GL state BEFORE any FBO creation (creation binds buf.fbo, which
    // would otherwise be saved as prev_fbo and never actually restored).
    GLint prev_fbo = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    GLint prev_vp[4]; glGetIntegerv(GL_VIEWPORT, prev_vp);

    // (Re)create FBO if size changed
    if (buf.fbo == 0 || buf.w != canvas_w || buf.h != canvas_h) {
        if (buf.fbo) { glDeleteFramebuffers(1, &buf.fbo); glDeleteTextures(1, &buf.tex); buf = {}; }
        glGenTextures(1, &buf.tex);
        glBindTexture(GL_TEXTURE_2D, buf.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, canvas_w, canvas_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &buf.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, buf.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, buf.tex, 0);
        buf.w = canvas_w; buf.h = canvas_h;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, buf.fbo);
    glViewport(0, 0, canvas_w, canvas_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    // Collect BG draw commands into a temporary draw list.
    ImDrawList tmp_dl(ImGui::GetDrawListSharedData());
    tmp_dl.AddDrawCmd();
    tmp_dl.PushClipRectFullScreen();
    draw_bg_preset(preset_id, &tmp_dl, {0.f, 0.f}, (float)canvas_w, (float)canvas_h,
                   t, speed, intensity, c1, c2, c3);
    tmp_dl.PopClipRect();

    // Render with our own VAO/VBO/program — never touches ImGui's backend state.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(g_bg_prog);
    glUniform2f(glGetUniformLocation(g_bg_prog, "u_size"), (float)canvas_w, (float)canvas_h);
    glBindVertexArray(g_bg_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_bg_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_bg_ebo);

    for (int n = 0; n < tmp_dl.CmdBuffer.Size; ++n) {
        const ImDrawCmd& cmd = tmp_dl.CmdBuffer[n];
        if (cmd.UserCallback) {
            cmd.UserCallback(&tmp_dl, &cmd);
            continue;
        }
        const ImDrawVert* vtx = tmp_dl.VtxBuffer.Data + cmd.VtxOffset;
        const ImDrawIdx*  idx = tmp_dl.IdxBuffer.Data + cmd.IdxOffset;
        glBufferData(GL_ARRAY_BUFFER,         tmp_dl.VtxBuffer.Size * sizeof(ImDrawVert), tmp_dl.VtxBuffer.Data, GL_STREAM_DRAW);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, tmp_dl.IdxBuffer.Size * sizeof(ImDrawIdx),  tmp_dl.IdxBuffer.Data, GL_STREAM_DRAW);
        (void)vtx; (void)idx;
        glDrawElementsBaseVertex(GL_TRIANGLES, (GLsizei)cmd.ElemCount, sizeof(ImDrawIdx)==2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                                 (void*)(intptr_t)(cmd.IdxOffset * sizeof(ImDrawIdx)), (GLint)cmd.VtxOffset);
    }
    glBindVertexArray(0);

    // Restore GL state
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);

    return (uintptr_t)buf.tex;
}

void fx_blit(uintptr_t src_tex, unsigned dst_fbo, int w, int h) {
    if (!g_prog.blit || !src_tex) return;

    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    GLint prev_vp[4];
    glGetIntegerv(GL_VIEWPORT, prev_vp);

    glBindVertexArray(g_vao);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dst_fbo);
    glViewport(0, 0, w, h);
    glUseProgram(g_prog.blit);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)src_tex);
    glUniform1i(glGetUniformLocation(g_prog.blit, "u_tex"), 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    glBindVertexArray(0);
}
