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
    vec4 c = texture(u_tex, v_uv);               // sharp center pixel — the OUTPUT color
    // Compute the matte on a small box-averaged colour, not the raw pixel. Codec/JPEG
    // compression shoves scattered green texels off the key colour; on a flat green
    // that survives as a blocky, half-keyed matte — invisible over black, but ugly
    // over a lit layer below. Averaging four neighbours pulls those stragglers back
    // onto the key so they zero out. The OUTPUT stays the sharp center pixel; only the
    // matte is computed on the smoothed colour, so foreground edges stay crisp.
    vec2 tx = 1.0 / vec2(textureSize(u_tex, 0));
    vec3 kc = (c.rgb
            + texture(u_tex, v_uv + vec2( 2.0 * tx.x, 0.0)).rgb
            + texture(u_tex, v_uv + vec2(-2.0 * tx.x, 0.0)).rgb
            + texture(u_tex, v_uv + vec2(0.0,  2.0 * tx.y)).rgb
            + texture(u_tex, v_uv + vec2(0.0, -2.0 * tx.y)).rgb) * 0.2;
    float lum_k = dot(u_key_color, vec3(0.299, 0.587, 0.114));
    vec3 ck = u_key_color - lum_k;
    float lum_p = dot(kc, vec3(0.299, 0.587, 0.114));
    vec3 cp = kc - lum_p;
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
uniform float u_corrupt;       // block/pixel corruption intensity 0..1
uniform float u_corrupt_bleed; // 0 = noisy datamosh blocks, 1 = transparent holes
uniform float u_time;
uniform float u_tex_h;     // texture height in pixels (avoids textureSize driver bugs)
uniform float u_tex_w;     // texture width in pixels

float hash(float n) { return fract(sin(n) * 43758.5453); }
float hash2(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

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

    // Block corruption — chunky datamosh "pixels": the frame is diced into
    // blocks, and a fraction of them (scaled by u_corrupt) get shoved sideways
    // and recolored each tick. u_corrupt_bleed fades the corrupted blocks toward
    // transparent holes instead of noisy colour.
    if (u_corrupt > 0.01) {
        float bs = 16.0;                                  // block size in px
        vec2  px  = vec2(v_uv.x * u_tex_w, v_uv.y * u_tex_h);
        vec2  blk = floor(px / bs);
        float tq  = floor(u_time * 7.0);                  // ~7 reshuffles/sec
        float br  = hash2(blk + tq * 1.7);
        if (br < u_corrupt * 0.6) {
            float sh  = (hash2(blk.yx + tq * 3.1) - 0.5) * 0.30 * u_corrupt; // sideways shove
            vec4  src = texture(u_tex, clamp(vec2(v_uv.x + sh, v_uv.y), 0.0, 1.0));
            float n   = hash2(floor(px / 3.0) + tq);      // coarse per-cluster noise
            vec4  noisy = vec4(src.rgb * (0.35 + 1.0 * n), src.a);
            frag = mix(noisy, vec4(0.0), u_corrupt_bleed);
        }
    }
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

// Chroma melt — chroma-keyed temporal feedback smear (the "trippy melt", NOT a
// clean key). Foreground (far from the key colour) shows the current frame; keyed
// background pixels keep the previous frame's output (u_feedback = the persistent
// slot), drifting sideways so motion smears into trails. The deliberate, export-safe
// version of the old GL_BLEND ghost. The clean keyer is k_chroma_key_frag (untouched).
static const char* k_chroma_melt_frag = R"glsl(
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;       // current chain result
uniform sampler2D u_feedback;  // previous frame's melt output (persistent slot)
uniform vec3  u_key_color;
uniform float u_threshold;
uniform float u_persist;       // 0 = no trail, ~0.9 = long smear
void main() {
    vec3 cur = texture(u_tex, v_uv).rgb;
    vec2 tx  = 1.0 / vec2(textureSize(u_tex, 0));
    // Foreground matte on a box-averaged colour (same anti-block trick as the keyer).
    vec3 kc = (cur
            + texture(u_tex, v_uv + vec2( 2.0*tx.x, 0.0)).rgb
            + texture(u_tex, v_uv + vec2(-2.0*tx.x, 0.0)).rgb
            + texture(u_tex, v_uv + vec2(0.0,  2.0*tx.y)).rgb
            + texture(u_tex, v_uv + vec2(0.0, -2.0*tx.y)).rgb) * 0.2;
    float lum_k = dot(u_key_color, vec3(0.299,0.587,0.114));
    float lum_p = dot(kc,          vec3(0.299,0.587,0.114));
    float dist  = length((kc - lum_p) - (u_key_color - lum_k));
    float fg    = smoothstep(u_threshold, u_threshold + 0.12, dist);  // 1=subject, 0=key
    // Keyed background keeps the prior frame, drifting sideways → smear trails.
    vec3 prev   = texture(u_feedback, v_uv + vec2(1.5*tx.x, 0.0)).rgb;
    vec3 trail  = mix(cur, prev, u_persist);
    frag = vec4(mix(trail, cur, fg), 1.0);
}
)glsl";

// Chroma echo — chroma-keyed feedback ECHO (sibling of Chroma Melt). Same feedback
// idea, but CRISP: a HARD matte + NO sideways drift, so keyed pixels stack the
// subject's past frames as distinct fading ghosts ("cool frames over the keyed
// colour") instead of a smudge. High persist = a longer stack of frames.
static const char* k_chroma_echo_frag = R"glsl(
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;       // current chain result
uniform sampler2D u_feedback;  // previous frame's echo output (persistent slot)
uniform vec3  u_key_color;
uniform float u_threshold;
uniform float u_persist;       // echo length (0 = none, ~0.95 = long stack)
void main() {
    vec3 cur = texture(u_tex, v_uv).rgb;
    vec2 tx  = 1.0 / vec2(textureSize(u_tex, 0));
    vec3 kc = (cur
            + texture(u_tex, v_uv + vec2( 2.0*tx.x, 0.0)).rgb
            + texture(u_tex, v_uv + vec2(-2.0*tx.x, 0.0)).rgb
            + texture(u_tex, v_uv + vec2(0.0,  2.0*tx.y)).rgb
            + texture(u_tex, v_uv + vec2(0.0, -2.0*tx.y)).rgb) * 0.2;
    float lum_k = dot(u_key_color, vec3(0.299,0.587,0.114));
    float lum_p = dot(kc,          vec3(0.299,0.587,0.114));
    float dist  = length((kc - lum_p) - (u_key_color - lum_k));
    float fg    = step(u_threshold, dist);   // HARD matte → crisp ghost frames
    // Keyed pixels keep the previous output (NO drift → frames stay crisp), decaying
    // toward the key colour. The subject stamps fresh each frame, so its path stacks.
    vec3 prev   = texture(u_feedback, v_uv).rgb;
    vec3 echo   = mix(cur, prev, u_persist);
    frag = vec4(mix(echo, cur, fg), 1.0);
}
)glsl";

// Chroma frame — chroma-keyed DISCRETE frame echoes (multi-tap delay). Unlike Melt/Echo
// (single-buffer feedback → a continuous trail), this samples a per-slot RING of past
// snapshot frames (u_ring, a 2D array) and composites the subject from N taps spaced
// `spacing` seconds apart, each fainter — distinct stacked "frames over the keyed colour".
static const char* k_chroma_frame_frag = R"glsl(
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D      u_tex;     // live current frame
uniform sampler2DArray u_ring;    // ring of past snapshots
uniform int   u_head;             // most-recent ring layer
uniform int   u_ntaps;            // active taps
uniform vec3  u_key_color;
uniform float u_threshold;
uniform float u_falloff;          // per-tap fade (newest = 1, each older x falloff)
const int RING = 8;
float matte(vec3 c) {
    float lk = dot(u_key_color, vec3(0.299,0.587,0.114));
    float lp = dot(c,           vec3(0.299,0.587,0.114));
    return step(u_threshold, length((c - lp) - (u_key_color - lk)));
}
void main() {
    vec3 live = texture(u_tex, v_uv).rgb;
    float live_fg = matte(live);
    vec3 echo = live;   // keyed bg starts as the live key colour
    // Composite taps oldest -> newest (over): newest on top, full strength; oldest faint.
    for (int i = RING - 1; i >= 0; --i) {
        if (i >= u_ntaps) continue;
        int layer = ((u_head - i) % RING + RING) % RING;
        vec3 tcol = texture(u_ring, vec3(v_uv, float(layer))).rgb;
        float a = matte(tcol) * pow(u_falloff, float(i));
        echo = mix(echo, tcol, a);
    }
    frag = vec4(mix(echo, live, live_fg), 1.0);   // live subject on top
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
    GLuint composite = 0, chroma_melt = 0, chroma_echo = 0, chroma_frame = 0;
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
// + a dedicated bank for the face-filter picker previews (one per filter id) so
// the whole grid of warps can be shown at once without clobbering each other.
static const int kFacePreviewSlots    = 40;  // >= face_filter_count() so picker previews never share an FBO
static const int kFacePreviewSlotBase = MAX_VIDEO_TRACKS * 4 + 2;
static const int kMaxSlots = MAX_VIDEO_TRACKS * 4 + 2 + kFacePreviewSlots;
static const int kFaceClipSlotBase = MAX_VIDEO_TRACKS * 2 + 1;

int fx_face_clip_slot(int video_slot) {
    if (video_slot < 0) video_slot = 0;
    return kFaceClipSlotBase + (video_slot % (MAX_VIDEO_TRACKS * 2));
}

int fx_face_preview_slot(int filter_id) {
    if (filter_id < 0) filter_id = 0;
    return kFacePreviewSlotBase + (filter_id % kFacePreviewSlots);
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
    g_prog.chroma_melt    = link_prog(k_chroma_melt_frag);
    g_prog.chroma_echo    = link_prog(k_chroma_echo_frag);
    g_prog.chroma_frame   = link_prog(k_chroma_frame_frag);

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
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);   // before out_ensure —
    GLint prev_vp[4];                                    // it leaves its FBO bound
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

// ── Face beauty (skin smoothing / brighten / eye pop) ─────────────────────────
// The industry recipe, done procedurally from the mesh: an elliptical skin
// mask in the face basis with holes punched over eyes/brows/mouth; a 12-tap
// poisson-disk bilateral-lite blur (taps weighted by luma similarity, so
// pores melt but edges — glasses, hairline, nostrils — survive); a soft-light
// brighten + warm tint on the same mask; and a gentle brighten inside the
// eye discs. Geometry arrives in pixels — no aspect gymnastics in UV space.
static const char* k_face_beauty_fs = R"(#version 330 core
in vec2 v_uv; out vec4 frag;
uniform sampler2D u_tex;
uniform vec2 u_dim;      // texture w, h in px
uniform vec4 u_face;     // cx, cy, rx, ry (px)
uniform vec2 u_up;       // face up unit vector
uniform vec4 u_eyes;     // eyeL xy, eyeR xy (px)
uniform vec4 u_feat;     // eye_r, mouth_x, mouth_y, mouth_r
uniform vec4 u_amt;      // smooth, brighten, warmth, eye_pop
uniform vec4 u_makeup;   // blush, lip_tint, _, _
uniform vec4 u_cheeks;   // cheekL xy, cheekR xy (px)
uniform vec4 u_blushc;   // blush color rgb, _
uniform vec4 u_lipc;     // lip color rgb, _
uniform vec4 u_eyeglow;  // rgb, amount
uniform vec4 u_cyber;    // skin_tint, desat, chrome, scanlines
uniform vec4 u_tintc;    // skin tint color rgb, _
uniform vec4 u_mouthax;  // lip ellipse semi-width, semi-height (px), _, _
uniform vec2 u_lippoly[12];  // outer-lip ring in px — the mask FOLLOWS the mouth
uniform vec4 u_nose;     // nose bridge xy (px), nose blush amt, freckles amt
uniform float u_brow_r;
uniform vec4 u_lash;     // amount, wing, liner, _
uniform vec4 u_chin_px;  // chin x, y (px), crease-smooth amount, _
uniform vec2 u_blink;    // per-eye blink 0..1 — lid landmarks lag a blink,
                         // so eye makeup fades out for those frames instead
                         // of floating over the closed eye
uniform vec4 u_eyeout;   // outer eye corners L xy, R xy (px)
uniform vec2 u_lidL[7];  // upper-lid chain, outer→inner (px)
uniform vec2 u_lidR[7];
float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// Distance to a segment, for the liner wing stroke.
float seg_dist(vec2 p2, vec2 a2, vec2 b2) {
    vec2 e2 = b2 - a2;
    float t2 = clamp(dot(p2 - a2, e2) / max(dot(e2, e2), 1e-4), 0.0, 1.0);
    return length(p2 - (a2 + e2 * t2));
}
void main() {
    vec2 p   = v_uv * u_dim;
    vec3 col = texture(u_tex, v_uv).rgb;

    // Skin mask: face ellipse in the (right, up) basis…
    vec2 rightv = vec2(-u_up.y, u_up.x);
    vec2 d  = p - u_face.xy;
    float a = dot(d, rightv) / max(u_face.z, 1.0);
    float b = dot(d, u_up)   / max(u_face.w, 1.0);
    float mask = 1.0 - smoothstep(0.72, 1.05, length(vec2(a, b)));
    // Hard cut at the chin line: the ellipse has margin below the chin, so
    // brighten/smooth were painting the top of the neck and fading out in a
    // visible U across the chest. b is the up-axis coordinate; the chin sits
    // near b = -0.89. Nothing below it gets skin processing.
    mask *= 1.0 - smoothstep(0.84, 1.0, -b);
    // …minus feature discs (eyes + brow band above them + mouth).
    float er = max(u_feat.x, 1.0);
    float holeL = 1.0 - smoothstep(er * 0.7, er * 1.25, distance(p, u_eyes.xy));
    float holeR = 1.0 - smoothstep(er * 0.7, er * 1.25, distance(p, u_eyes.zw));
    vec2 browL = u_eyes.xy + u_up * er * 1.2;
    vec2 browR = u_eyes.zw + u_up * er * 1.2;
    float br = max(u_brow_r, 1.0);
    float holeBL = 1.0 - smoothstep(br * 0.6, br * 1.1, distance(p, browL));
    float holeBR = 1.0 - smoothstep(br * 0.6, br * 1.1, distance(p, browR));
    float mr = max(u_feat.w, 1.0);
    float holeM = 1.0 - smoothstep(mr * 0.75, mr * 1.3, distance(p, u_feat.yz));
    mask *= (1.0 - holeL) * (1.0 - holeR) * (1.0 - holeM)
          * (1.0 - holeBL * 0.85) * (1.0 - holeBR * 0.85);

    // Bilateral-lite smoothing: poisson disk scaled to face size.
    if (u_amt.x > 0.001 && mask > 0.003) {
        float rad = max(u_face.z, u_face.w) * 0.045;
        vec2 taps[12] = vec2[12](
            vec2(-0.326,-0.406), vec2(-0.840,-0.074), vec2(-0.696, 0.457),
            vec2(-0.203, 0.621), vec2( 0.962,-0.195), vec2( 0.473,-0.480),
            vec2( 0.519, 0.767), vec2( 0.185,-0.893), vec2( 0.507, 0.064),
            vec2( 0.896, 0.412), vec2(-0.322,-0.933), vec2(-0.792,-0.598));
        float l0 = lum(col);
        vec3 acc = col; float wsum = 1.0;
        for (int i = 0; i < 12; ++i) {
            vec3 c = texture(u_tex, v_uv + taps[i] * rad / u_dim).rgb;
            float wl = exp(-pow((lum(c) - l0) * 9.0, 2.0));
            acc  += c * wl;
            wsum += wl;
        }
        col = mix(col, acc / wsum, u_amt.x * mask);
    }
    // Soft-light brighten + warmth on skin.
    if (u_amt.y > 0.001) {
        vec3 lift = col + (vec3(1.0) - col) * col * 0.9;
        col = mix(col, lift, u_amt.y * mask);
    }
    if (u_amt.z > 0.001)
        col += vec3(0.035, 0.012, -0.02) * (u_amt.z * mask);
    // Eye pop: brighten inside the eye discs (soft).
    if (u_amt.w > 0.001) {
        float eL = 1.0 - smoothstep(er * 0.35, er * 0.95, distance(p, u_eyes.xy));
        float eR = 1.0 - smoothstep(er * 0.35, er * 0.95, distance(p, u_eyes.zw));
        col *= 1.0 + u_amt.w * 0.30 * max(eL, eR);
    }
    // Double-chin crease erase: frequency-separation retouch. The fold reads
    // as a horizontal crease shadow + bulge highlight; a wide blur in a
    // chin-anchored region removes that local contrast while the AVERAGE
    // tone stays identical — no band (nothing painted), no wobble (nothing
    // moves). Uses its own region, not the face mask (which ends at the chin).
    if (u_chin_px.z > 0.001) {
        float below = smoothstep(0.70, 0.95, -b) * (1.0 - smoothstep(1.30, 1.55, -b));
        float reg = below * (1.0 - smoothstep(er * 1.3, er * 2.3,
                                              distance(p, u_chin_px.xy)));
        if (reg > 0.003) {
            vec2 px2 = 1.0 / u_dim;
            float rad = er * 0.55;
            vec3 acc = vec3(0.0);
            for (int i = 0; i < 12; ++i) {
                float ang = float(i) * 0.5236;
                float rr2 = (0.35 + 0.65 * fract(float(i) * 0.618)) * rad;
                acc += texture(u_tex, v_uv + vec2(cos(ang), sin(ang)) * rr2 * px2).rgb;
            }
            col = mix(col, acc * (1.0 / 12.0), reg * u_chin_px.z * 0.8);
        }
    }
    // Under-jaw contour shadow: a soft darkening band just OUTSIDE the lower
    // face ellipse — the makeup-artist trick that makes a double chin recede.
    // (elen = elliptical distance in the face basis; >1 is outside the face.)
    if (u_makeup.w > 0.001) {
        float elen = length(vec2(a, b));
        float band = smoothstep(0.94, 1.03, elen) * (1.0 - smoothstep(1.03, 1.18, elen));
        float below = smoothstep(0.45, 0.85, -b);    // lower face only
        // Hug the CHIN: without this the band relative to the big face
        // ellipse sweeps a wide U across the neck and chest.
        vec2 chin_px = u_face.xy - u_up * u_face.w * 0.92;
        float near_chin = 1.0 - smoothstep(u_face.z * 0.55, u_face.z * 0.95,
                                           distance(p, chin_px));
        float sh = u_makeup.w * band * below * near_chin;
        col *= 1.0 - sh * vec3(0.20, 0.22, 0.24);
    }
    // Makeup gates: geometry says WHERE, chroma says WHAT. Without these the
    // lip disc tinted whatever sat in front of the mouth (a microphone, a
    // hand) and blush landed on headphone cups inside the face ellipse.
    float m_cb = 0.5 - 0.168736 * col.r - 0.331264 * col.g + 0.5 * col.b;
    float m_cr = 0.5 + 0.5 * col.r - 0.418688 * col.g - 0.081312 * col.b;
    float skin_chroma = smoothstep(0.27, 0.31, m_cb) * (1.0 - smoothstep(0.45, 0.50, m_cb))
                      * smoothstep(0.50, 0.54, m_cr) * (1.0 - smoothstep(0.66, 0.70, m_cr));
    // Blush: two soft rosy discs on the cheeks, skin-chroma gated.
    if (u_makeup.x > 0.001) {
        float br2 = max(u_face.z, u_face.w) * 0.30;
        float cL = 1.0 - smoothstep(br2 * 0.3, br2, distance(p, u_cheeks.xy));
        float cR = 1.0 - smoothstep(br2 * 0.3, br2, distance(p, u_cheeks.zw));
        float bm = max(cL, cR) * u_makeup.x * mask * skin_chroma;
        col = mix(col, col * (0.75 + 0.5 * u_blushc.rgb), bm * 0.55);
    }
    // E-girl layer: across-the-nose blush + faux freckles, both in the face
    // basis so they ride head rotation. Freckles are hash-jittered dots over
    // an elliptical band spanning the nose bridge and upper cheeks.
    if (u_nose.z + u_nose.w > 0.001) {
        vec2 nb = vec2(dot(p - u_nose.xy, rightv), dot(p - u_nose.xy, u_up)) / max(er, 1.0);
        float band = 1.0 - smoothstep(0.55, 1.0, length(nb * vec2(0.50, 1.55)));
        if (u_nose.z > 0.001) {
            float m_cb2 = 0.5 - 0.168736 * col.r - 0.331264 * col.g + 0.5 * col.b;
            float m_cr2 = 0.5 + 0.5 * col.r - 0.418688 * col.g - 0.081312 * col.b;
            float sk2 = smoothstep(0.27, 0.31, m_cb2) * (1.0 - smoothstep(0.45, 0.50, m_cb2))
                      * smoothstep(0.50, 0.54, m_cr2) * (1.0 - smoothstep(0.66, 0.70, m_cr2));
            col = mix(col, col * (0.75 + 0.5 * u_blushc.rgb),
                      band * u_nose.z * mask * sk2 * 0.50);
        }
        if (u_nose.w > 0.001) {
            vec2 cell = floor(nb * 6.0);
            vec2 fr2  = fract(nb * 6.0);
            float h1 = fract(sin(dot(cell, vec2(127.1, 311.7))) * 43758.5453);
            vec2 jit = fract(sin(vec2(dot(cell, vec2(269.5, 183.3)),
                                      dot(cell, vec2(419.2, 371.9)))) * 43758.5453);
            float fd = length(fr2 - 0.30 - jit * 0.40);
            // Size + opacity jitter per dot; soft edges and a warm brown so
            // they read as freckles, not specks.
            float rsz  = 0.11 + 0.10 * fract(h1 * 7.31);
            float dotm = (1.0 - smoothstep(rsz * 0.4, rsz + 0.06, fd)) * step(0.45, h1);
            float op   = 0.35 + 0.30 * fract(h1 * 13.7);
            col = mix(col, col * vec3(0.74, 0.55, 0.45),
                      dotm * band * u_nose.w * mask * op);
        }
    }
    // Lip tint: the OUTER-LIP POLYGON from the live mesh (follows a smile
    // exactly — an ellipse can't bend), feathered by distance to its edge,
    // with a bitten-lip gradient: strongest at the mouth center, fading to
    // the edge (the douyin 咬唇 look). Redness gate keeps it off teeth/mic.
    if (u_makeup.y > 0.001) {
        int crossings = 0;
        float dmin = 1e9;
        for (int i = 0; i < 12; ++i) {
            vec2 a2 = u_lippoly[i];
            vec2 b2 = u_lippoly[i == 11 ? 0 : i + 1];
            if ((a2.y > p.y) != (b2.y > p.y)) {
                float xin = a2.x + (p.y - a2.y) * (b2.x - a2.x) / (b2.y - a2.y);
                if (p.x < xin) crossings++;
            }
            vec2 e2 = b2 - a2;
            float ts = clamp(dot(p - a2, e2) / max(dot(e2, e2), 1e-4), 0.0, 1.0);
            dmin = min(dmin, length(p - (a2 + e2 * ts)));
        }
        float inside  = float((crossings & 1) == 1);
        float feather = max(u_mouthax.y * 0.30, 1.5);
        // No outside bleed: color above the vermilion border is what reads
        // as "smudged". The mask is zero outside the ring, full inside past
        // a short feather.
        float lm2 = inside * smoothstep(0.0, feather * 0.8, dmin);
        // Bitten-lip gradient via the center ellipse.
        vec2 md = p - u_feat.yz;
        float la = dot(md, rightv) / max(u_mouthax.x, 1.0);
        float lb = dot(md, u_up)   / max(u_mouthax.y, 1.0);
        float grad = 1.0 - 0.55 * u_makeup.z * smoothstep(0.25, 1.0, length(vec2(la, lb)));
        // Redness gate polices the EDGES only. Deep inside the lip polygon
        // geometry wins — a lower lip washed out by window light is still a
        // lip (the gate-everywhere version painted upper lips only).
        float deep  = inside * smoothstep(feather, feather * 2.2, dmin);
        float lippy = max(smoothstep(0.03, 0.12, col.r - col.g), deep);
        float t = u_makeup.y * lm2 * grad * lippy;
        // Colorize toward the lip color, keeping the lip's own shading — a
        // dark goth plum and a hot Barbie pink both read as lipstick.
        vec3 lip_target = u_lipc.rgb * (0.30 + 1.05 * lum(col));
        col = mix(col, clamp(lip_target, 0.0, 1.0), t * 0.85);
    }

    // ── Cyber layer (all skin-masked; runs after makeup) ──────────────────
    if (u_cyber.x + u_cyber.y + u_cyber.z + u_cyber.w > 0.001) {
        float lm3 = lum(col);
        // Desaturate → tint → chrome curve → scanlines.
        col = mix(col, vec3(lm3), u_cyber.y * mask);
        col = mix(col, u_tintc.rgb * (0.15 + 1.1 * lm3), u_cyber.x * mask);
        if (u_cyber.z > 0.001) {
            vec3 crm = clamp((col - 0.5) * 2.2 + 0.5, 0.0, 1.0);
            crm += smoothstep(0.75, 0.98, lm3) * 0.25;   // hard speculars
            col = mix(col, crm, u_cyber.z * mask);
        }
        if (u_cyber.w > 0.001) {
            float sl = 0.5 + 0.5 * sin(p.y * 1.4);
            col *= 1.0 - u_cyber.w * mask * 0.35 * smoothstep(0.55, 0.95, sl);
            col += u_tintc.rgb * u_cyber.w * mask * 0.06 * (1.0 - sl);
        }
    }
    // Lashes + eyeliner along the REAL upper-lid polyline (the parametric
    // arc floated at brow height — same lesson as the lips: the mesh knows
    // the true shape, use it). Liner is a crisp line ON the lid edge; the
    // lash band sits just above it, thicker toward the outer corner; the
    // wing extends from the outer corner.
    if (u_lash.x + u_lash.z > 0.001) {
        vec3 ink = vec3(0.04, 0.03, 0.04);
        for (int side = 0; side < 2; ++side) {
            float bfade = 1.0 - 0.9 * smoothstep(0.25, 0.55,
                                    side == 0 ? u_blink.x : u_blink.y);
            vec2 outc = side == 0 ? u_eyeout.xy : u_eyeout.zw;
            float dmin2 = 1e9; float tbest = 0.0;
            for (int i = 0; i < 6; ++i) {
                vec2 a3 = side == 0 ? u_lidL[i]     : u_lidR[i];
                vec2 b3 = side == 0 ? u_lidL[i + 1] : u_lidR[i + 1];
                vec2 e3 = b3 - a3;
                float ts = clamp(dot(p - a3, e3) / max(dot(e3, e3), 1e-4), 0.0, 1.0);
                float dd = length(p - (a3 + e3 * ts));
                if (dd < dmin2) { dmin2 = dd; tbest = (float(i) + ts) / 6.0; }
            }
            // Signed side of the chain: positive above the lid.
            float above = dot(p - outc, u_up);   // coarse, per-eye
            float taper = 1.0 - tbest * 0.6;     // thicker at the outer corner
            if (u_lash.z > 0.001) {              // liner: crisp, ON the line
                float lt = er * 0.055 * taper;
                float line = 1.0 - smoothstep(lt * 0.5, lt, dmin2);
                col = mix(col, ink, line * u_lash.z * 0.95 * bfade);
            }
            if (u_lash.x > 0.001) {              // lash band: soft, just above
                float bt = er * 0.16 * taper;
                float band = (1.0 - smoothstep(bt * 0.35, bt, dmin2))
                           * smoothstep(-er * 0.05, er * 0.10, above);
                col = mix(col, col * 0.30, band * u_lash.x * 0.75 * bfade);
            }
            if (u_lash.y > 0.001) {              // wing from the outer corner
                vec2 inc = side == 0 ? u_lidL[6] : u_lidR[6];
                vec2 outdir = normalize(outc - inc);
                vec2 w1 = outc + (outdir * 0.80 + u_up * 0.35) * er * u_lash.y;
                float wd = seg_dist(p, outc, w1);
                float along = clamp(dot(p - outc, w1 - outc) /
                                    max(dot(w1 - outc, w1 - outc), 1e-4), 0.0, 1.0);
                float wt = mix(er * 0.10, er * 0.015, along);
                float wing = 1.0 - smoothstep(wt * 0.4, wt, wd);
                col = mix(col, ink, wing * max(u_lash.x, u_lash.z) * 0.92 * bfade);
            }
        }
    }
    // Eye glow: additive colored halo, larger + softer than eye pop.
    if (u_eyeglow.a > 0.001) {
        float gL = 1.0 - smoothstep(er * 0.3, er * 1.6, distance(p, u_eyes.xy));
        float gR = 1.0 - smoothstep(er * 0.3, er * 1.6, distance(p, u_eyes.zw));
        col += u_eyeglow.rgb * (u_eyeglow.a * 0.55 * max(gL, gR));
    }
    frag = vec4(clamp(col, 0.0, 1.0), texture(u_tex, v_uv).a);
}
)";
// ── UV-mapped face makeup pass ─────────────────────────────────────────────
// Draws the tracked face mesh (MediaPipe canonical UVs, 898 tris) textured
// with an authored makeup PNG. Per-pixel lighting adaptation ties pigment to
// the skin beneath (luminance + color cast) so a texture authored under
// neutral light sits naturally in a warm/dim room.
#include "generated/face_uv_mesh.h"

static const char* k_face_mk_vs = R"(#version 330 core
layout(location = 0) in vec2 a_px;   // landmark position, texture pixels
layout(location = 1) in vec2 a_uv;   // canonical makeup-texture UV
uniform vec2 u_dim;
out vec2 v_mkuv;
out vec2 v_srcuv;
void main() {
    v_mkuv  = a_uv;
    v_srcuv = a_px / u_dim;
    gl_Position = vec4(v_srcuv * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* k_face_mk_fs = R"(#version 330 core
in vec2 v_mkuv;
in vec2 v_srcuv;
out vec4 frag;
uniform sampler2D u_mk;
uniform sampler2D u_src;
uniform float u_opacity;
uniform float u_adapt;    // 0 = raw decal, 1 = full lighting adaptation
uniform vec4 u_mk_eyes;   // eye centers (px) — for blink fade
uniform vec4 u_mk_blink;  // blink L, blink R, eye radius px, _
float lum2(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
void main() {
    vec4 mk   = texture(u_mk, v_mkuv);
    vec3 base = texture(u_src, v_srcuv).rgb;
    // Blink fade: painted eye makeup would float over a closed eye (the lid
    // landmarks lag a blink) — fade it inside the eye discs instead.
    vec2 ppx = v_srcuv * vec2(textureSize(u_src, 0));
    float er2 = max(u_mk_blink.z, 1.0);
    float nearL = 1.0 - smoothstep(er2 * 0.8, er2 * 2.2, distance(ppx, u_mk_eyes.xy));
    float nearR = 1.0 - smoothstep(er2 * 0.8, er2 * 2.2, distance(ppx, u_mk_eyes.zw));
    float bfade = 1.0 - 0.9 * max(nearL * smoothstep(0.25, 0.55, u_mk_blink.x),
                                  nearR * smoothstep(0.25, 0.55, u_mk_blink.y));
    mk.a *= bfade;
    float bl  = lum2(base);
    vec3 tint = base / max(bl, 0.04);
    vec3 lit  = mk.rgb
              * mix(vec3(1.0), clamp(tint, 0.55, 1.6), u_adapt * 0.65)
              * mix(1.0, clamp(bl * 1.9, 0.20, 1.45), u_adapt * 0.85);
    frag = vec4(mix(base, clamp(lit, 0.0, 1.0), mk.a * u_opacity), 1.0);
}
)";

static GLuint g_face_mk_prog = 0;
static GLuint g_face_mk_vao = 0, g_face_mk_vbo = 0, g_face_mk_ibo = 0;
static struct { GLuint tex = 0, fbo = 0; int w = 0, h = 0; } g_makeup_out[kMaxSlots];

uintptr_t face_makeup_apply(uintptr_t src_tex, int slot, int w, int h,
                            const float (*pts)[2], unsigned makeup_tex,
                            float opacity, float adapt,
                            float eyeL_x, float eyeL_y, float eyeR_x, float eyeR_y,
                            float eye_r, float blink_l, float blink_r) {
    if (!src_tex || !makeup_tex || slot < 0 || slot >= kMaxSlots ||
        w <= 0 || h <= 0 || opacity <= 0.001f)
        return src_tex;
    if (!g_face_mk_prog) {
        g_face_mk_prog = link_prog2(k_face_mk_vs, k_face_mk_fs);
        if (!g_face_mk_prog) return src_tex;
        glGenVertexArrays(1, &g_face_mk_vao);
        glGenBuffers(1, &g_face_mk_vbo);
        glGenBuffers(1, &g_face_mk_ibo);
        glBindVertexArray(g_face_mk_vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_face_mk_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(k_face_tris),
                     k_face_tris, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, g_face_mk_vbo);
        glBufferData(GL_ARRAY_BUFFER, FACE_UV_NPTS * 4 * sizeof(float),
                     nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                              (void*)(2 * sizeof(float)));
        glBindVertexArray(0);
    }
    // Save bindings BEFORE the ensure (make_tex_fbo leaves its FBO bound).
    GLint prev_fbo = 0, prev_vp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_vp);
    auto& s = g_makeup_out[slot];
    if (s.w != w || s.h != h) {
        if (s.fbo) { glDeleteFramebuffers(1, &s.fbo); glDeleteTextures(1, &s.tex); s.fbo = s.tex = 0; }
        make_tex_fbo(s.tex, s.fbo, w, h);
        s.w = w; s.h = h;
    }
    GLboolean was_blend = glIsEnabled(GL_BLEND);
    glDisable(GL_BLEND);
    // Base copy, then the textured mesh over it (fragment blends in-shader so
    // it can light-adapt against the base).
    draw_pass(s.fbo, (GLuint)src_tex, w, h, g_prog.blit);
    float vtx[FACE_UV_NPTS * 4];
    for (int i = 0; i < FACE_UV_NPTS; ++i) {
        vtx[i * 4 + 0] = pts[i][0];
        vtx[i * 4 + 1] = pts[i][1];
        vtx[i * 4 + 2] = k_face_uv[i][0];
        vtx[i * 4 + 3] = k_face_uv[i][1];
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(0, 0, w, h);
    glUseProgram(g_face_mk_prog);
    glBindVertexArray(g_face_mk_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_face_mk_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vtx), vtx);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)makeup_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, (GLuint)src_tex);
    glUniform1i(glGetUniformLocation(g_face_mk_prog, "u_mk"), 0);
    glUniform1i(glGetUniformLocation(g_face_mk_prog, "u_src"), 1);
    glUniform2f(glGetUniformLocation(g_face_mk_prog, "u_dim"), (float)w, (float)h);
    glUniform1f(glGetUniformLocation(g_face_mk_prog, "u_opacity"), opacity);
    glUniform1f(glGetUniformLocation(g_face_mk_prog, "u_adapt"), adapt);
    glUniform4f(glGetUniformLocation(g_face_mk_prog, "u_mk_eyes"),
                eyeL_x, eyeL_y, eyeR_x, eyeR_y);
    glUniform4f(glGetUniformLocation(g_face_mk_prog, "u_mk_blink"),
                blink_l, blink_r, eye_r, 0.f);
    glDrawElements(GL_TRIANGLES, FACE_UV_NTRI * 3, GL_UNSIGNED_SHORT, 0);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    if (was_blend) glEnable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    return s.tex;
}

static GLuint g_face_beauty_prog = 0;
// Own buffer per slot — the warp pass reads this output into g_out[slot];
// sharing g_out here would be a same-texture read/write feedback loop.
static struct { GLuint tex = 0, fbo = 0; int w = 0, h = 0; } g_beauty_out[kMaxSlots];

uintptr_t face_beauty_apply(uintptr_t src_tex, int slot, int w, int h,
                            const FaceBeautyParams& p) {
    if (slot < 0 || slot >= kMaxSlots || w <= 0 || h <= 0) return src_tex;
    if (!g_face_beauty_prog) {
        g_face_beauty_prog = link_prog(k_face_beauty_fs);
        if (!g_face_beauty_prog) return src_tex;
    }
    // Save bindings BEFORE the ensure: make_tex_fbo leaves the fresh FBO
    // bound, so reading the "previous" binding after it captures OUR buffer —
    // the restore then pins the rest of the frame into it (one black frame
    // the first time a size is seen).
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    GLint prev_vp[4];
    glGetIntegerv(GL_VIEWPORT, prev_vp);
    auto& s = g_beauty_out[slot];
    if (s.w != w || s.h != h) {
        if (s.fbo) { glDeleteFramebuffers(1, &s.fbo); glDeleteTextures(1, &s.tex); s.fbo = s.tex = 0; }
        make_tex_fbo(s.tex, s.fbo, w, h);
        s.w = w; s.h = h;
    }

    glBindVertexArray(g_vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(0, 0, w, h);
    glUseProgram(g_face_beauty_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)src_tex);
    auto u = [&](const char* nm) { return glGetUniformLocation(g_face_beauty_prog, nm); };
    glUniform1i(u("u_tex"), 0);
    glUniform2f(u("u_dim"), (float)w, (float)h);
    glUniform4f(u("u_face"), p.face_cx, p.face_cy, p.face_rx, p.face_ry);
    glUniform2f(u("u_up"), p.upx, p.upy);
    glUniform4f(u("u_eyes"), p.eyeL_x, p.eyeL_y, p.eyeR_x, p.eyeR_y);
    glUniform4f(u("u_feat"), p.eye_r, p.mouth_x, p.mouth_y, p.mouth_r);
    glUniform4f(u("u_amt"), p.smooth, p.brighten, p.warmth, p.eye_pop);
    glUniform4f(u("u_makeup"), p.blush, p.lip_tint, p.lip_grad, p.jaw_shade);
    glUniform4f(u("u_cheeks"), p.cheekL_x, p.cheekL_y, p.cheekR_x, p.cheekR_y);
    glUniform4f(u("u_blushc"), p.blush_col[0], p.blush_col[1], p.blush_col[2], 0.f);
    glUniform4f(u("u_lipc"), p.lip_col[0], p.lip_col[1], p.lip_col[2], 0.f);
    glUniform4f(u("u_eyeglow"), p.eye_glow_col[0], p.eye_glow_col[1], p.eye_glow_col[2], p.eye_glow);
    glUniform4f(u("u_cyber"), p.skin_tint, p.desat, p.chrome, p.scanlines);
    glUniform4f(u("u_tintc"), p.tint_col[0], p.tint_col[1], p.tint_col[2], 0.f);
    glUniform4f(u("u_mouthax"), p.mouth_sw, p.mouth_sh, 0.f, 0.f);
    glUniform2fv(u("u_lippoly"), 12, &p.lip_poly[0][0]);
    glUniform4f(u("u_nose"), p.nose_x, p.nose_y, p.nose_blush, p.freckles);
    glUniform4f(u("u_lash"), p.lash, p.lash_wing, p.liner, 0.f);
    glUniform2f(u("u_blink"), p.blink_l, p.blink_r);
    glUniform4f(u("u_chin_px"), p.chin_x, p.chin_y, p.chin_smooth, 0.f);
    glUniform4f(u("u_eyeout"), p.eyeoutL_x, p.eyeoutL_y, p.eyeoutR_x, p.eyeoutR_y);
    glUniform2fv(u("u_lidL"), 7, &p.lidL[0][0]);
    glUniform2fv(u("u_lidR"), 7, &p.lidR[0][0]);
    glUniform1f(u("u_brow_r"), p.brow_r);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    return (uintptr_t)s.tex;
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

// ── Chroma Frame ring: per-slot history of snapshot frames for discrete echoes ──
// Melt/Echo reuse the single g_out[slot] feedback texture; Frame needs DISTINCT past
// frames, so each Frame-carrying slot keeps a small 2D-array ring, advanced every
// `spacing` seconds of clip time. Lazily allocated; only slots with a Frame brick pay.
static const int kFrameRingLayers = 8;
struct FrameRing {
    GLuint arr = 0;         // GL_TEXTURE_2D_ARRAY, kFrameRingLayers layers
    GLuint fbo = 0;         // scratch FBO for per-layer writes
    int    w = 0, h = 0;
    int    head = -1;       // most-recent filled layer
    int    count = 0;       // layers filled so far
    float  last_t = -1e9f;  // clip-time of the last tap advance
};
static std::unordered_map<int, FrameRing> g_frame_rings;

static FrameRing& frame_ring_ensure(int slot, int w, int h) {
    FrameRing& r = g_frame_rings[slot];
    if (r.arr && (r.w != w || r.h != h)) {
        glDeleteTextures(1, &r.arr); r.arr = 0;
        r.head = -1; r.count = 0; r.last_t = -1e9f;
    }
    if (!r.arr) {
        glGenTextures(1, &r.arr);
        glBindTexture(GL_TEXTURE_2D_ARRAY, r.arr);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGB8, w, h, kFrameRingLayers,
                     0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        if (!r.fbo) glGenFramebuffers(1, &r.fbo);
        r.w = w; r.h = h;
    }
    return r;
}

uintptr_t fx_apply(uintptr_t src_tex_in, int slot, int w, int h,
                   const EffectAccum& ea, const CreativeFXAccum& cfx, float t)
{
    if (!g_prog.grade || slot < 0 || slot >= kMaxSlots) return src_tex_in;

    bool need_grade    = ea.any_color;
    bool need_vig      = ea.any_vignette && ea.vignette > 0.001f;
    bool need_blur     = ea.any_blur     && ea.blur > 0.1f;
    bool need_chroma   = cfx.chroma_key_on;
    bool need_glitch   = cfx.glitch_on   && (cfx.glitch_chroma >= 0.1f || cfx.glitch_jitter >= 0.01f
                                             || cfx.glitch_corruption >= 0.01f);
    bool need_vhs      = cfx.vhs_on      && (cfx.vhs_noise >= 0.01f || cfx.vhs_bleed >= 0.1f || cfx.vhs_tracking >= 0.01f);
    bool need_leak     = cfx.leak_on     && cfx.leak_intensity > 0.01f;
    bool need_datamosh = cfx.datamosh_on && cfx.datamosh_spread > 0.01f;
    bool need_melt     = cfx.chroma_melt_on;
    bool need_echo     = cfx.chroma_echo_on;
    bool need_frame    = cfx.chroma_frame_on;

    if (!need_grade && !need_vig && !need_blur && !need_chroma &&
        !need_glitch && !need_vhs && !need_leak && !need_datamosh &&
        !need_melt && !need_echo && !need_frame && !cfx.any_gen_fx)
        return src_tex_in;

    if (w <= 0 || h <= 0) return src_tex_in;

    // Save GL state BEFORE pp_ensure/out_ensure touch FBO bindings
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    GLint prev_vp[4];
    glGetIntegerv(GL_VIEWPORT, prev_vp);

    pp_ensure(w, h);
    out_ensure(slot, w, h);

    // Every FX pass (and the final blit into the persistent slot below) OVERWRITES
    // its target — the shaders composite internally — so GL blending must be off.
    // The live preview leaves GL_BLEND enabled (ImGui backend), which made the slot
    // blit BLEND: a chroma-keyed clip's alpha-0 pixels never cleared, so the slot
    // accumulated last frame's content into a sideways ghost. Export ran blend-off,
    // hence clean. Force it off for the chain, restore the caller's state after.
    GLboolean prev_blend = glIsEnabled(GL_BLEND);
    glDisable(GL_BLEND);

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
        glUniform1f(glGetUniformLocation(p, "u_corrupt"),       cfx.glitch_corruption);
        glUniform1f(glGetUniformLocation(p, "u_corrupt_bleed"), cfx.glitch_corruption_bleed);
        glUniform1f(glGetUniformLocation(p, "u_time"),   t);
        glUniform1f(glGetUniformLocation(p, "u_tex_h"),  (float)h);
        glUniform1f(glGetUniformLocation(p, "u_tex_w"),  (float)w);
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

    // ── Chroma melt: feed the keyed frame back into the persistent slot for a
    //    deliberate temporal smear — the old GL_BLEND "ghost", now on purpose and
    //    export-safe (g_out[slot] still holds last frame's output = the feedback).
    if (need_melt) {
        GLuint p = g_prog.chroma_melt;
        glBindFramebuffer(GL_FRAMEBUFFER, g_pp.fbo[pslot]);
        glViewport(0, 0, w, h);
        glUseProgram(p);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, cur);
        glUniform1i(glGetUniformLocation(p, "u_tex"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_out[slot].tex);   // previous frame = feedback
        glUniform1i(glGetUniformLocation(p, "u_feedback"), 1);
        glUniform3f(glGetUniformLocation(p, "u_key_color"),
                    cfx.chroma_melt_r, cfx.chroma_melt_g, cfx.chroma_melt_b);
        glUniform1f(glGetUniformLocation(p, "u_threshold"), cfx.chroma_melt_threshold);
        glUniform1f(glGetUniformLocation(p, "u_persist"),   cfx.chroma_melt_persist);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        cur = g_pp.tex[pslot];
        pslot ^= 1;
    }

    // ── Chroma echo: crisp sibling of melt — keyed pixels stack the subject's past
    //    frames as fading ghosts (no drift). Also reads g_out[slot] (last frame).
    if (need_echo) {
        GLuint p = g_prog.chroma_echo;
        glBindFramebuffer(GL_FRAMEBUFFER, g_pp.fbo[pslot]);
        glViewport(0, 0, w, h);
        glUseProgram(p);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, cur);
        glUniform1i(glGetUniformLocation(p, "u_tex"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_out[slot].tex);   // previous frame = feedback
        glUniform1i(glGetUniformLocation(p, "u_feedback"), 1);
        glUniform3f(glGetUniformLocation(p, "u_key_color"),
                    cfx.chroma_echo_r, cfx.chroma_echo_g, cfx.chroma_echo_b);
        glUniform1f(glGetUniformLocation(p, "u_threshold"), cfx.chroma_echo_threshold);
        glUniform1f(glGetUniformLocation(p, "u_persist"),   cfx.chroma_echo_persist);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        cur = g_pp.tex[pslot];
        pslot ^= 1;
    }

    // ── Chroma frame: discrete multi-tap echoes from a per-slot frame ring ─────
    if (need_frame) {
        FrameRing& ring = frame_ring_ensure(slot, w, h);
        float spacing = fmaxf(0.01f, cfx.chroma_frame_spacing);
        // Reset on a backward / large time jump (scrub) so echoes don't smear a seek.
        if (t < ring.last_t - 0.001f || t - ring.last_t > spacing * 8.f) {
            ring.head = -1; ring.count = 0; ring.last_t = -1e9f;
        }
        // Advance one tap every `spacing` seconds (or on first frame): snapshot the
        // current chain result into the ring head via a layer-targeted blit.
        if (ring.head < 0 || t - ring.last_t >= spacing) {
            ring.head = (ring.head + 1) % kFrameRingLayers;
            if (ring.count < kFrameRingLayers) ring.count++;
            ring.last_t = t;
            glBindFramebuffer(GL_FRAMEBUFFER, ring.fbo);
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ring.arr, 0, ring.head);
            glViewport(0, 0, w, h);
            glUseProgram(g_prog.blit);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, cur);
            glUniform1i(glGetUniformLocation(g_prog.blit, "u_tex"), 0);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        // Composite the live frame + the ring taps into the ping-pong.
        int ntaps = (int)(cfx.chroma_frame_taps + 0.5f);
        ntaps = ntaps < 1 ? 1 : (ntaps > kFrameRingLayers ? kFrameRingLayers : ntaps);
        if (ntaps > ring.count) ntaps = ring.count;
        GLuint p = g_prog.chroma_frame;
        glBindFramebuffer(GL_FRAMEBUFFER, g_pp.fbo[pslot]);
        glViewport(0, 0, w, h);
        glUseProgram(p);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, cur);
        glUniform1i(glGetUniformLocation(p, "u_tex"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, ring.arr);
        glUniform1i(glGetUniformLocation(p, "u_ring"), 1);
        glUniform1i(glGetUniformLocation(p, "u_head"), ring.head);
        glUniform1i(glGetUniformLocation(p, "u_ntaps"), ntaps);
        glUniform3f(glGetUniformLocation(p, "u_key_color"),
                    cfx.chroma_frame_r, cfx.chroma_frame_g, cfx.chroma_frame_b);
        glUniform1f(glGetUniformLocation(p, "u_threshold"), cfx.chroma_frame_threshold);
        glUniform1f(glGetUniformLocation(p, "u_falloff"),   cfx.chroma_frame_falloff);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        glActiveTexture(GL_TEXTURE0);
        cur = g_pp.tex[pslot];
        pslot ^= 1;
    }

    // ── Write final result to stable per-slot output ──────────────────────────
    draw_pass(g_out[slot].fbo, cur, w, h, g_prog.blit);

    // Restore GL state
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    glBindVertexArray(0);
    if (prev_blend) glEnable(GL_BLEND);

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
