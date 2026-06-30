#include "body_fx.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

// stb_image implementation is already compiled in video.cpp
// Just include the header without the implementation define
#include "stb_image.h"

#include "bg_remove.h"
#include "generated/body_fx_preview.h"   // embedded card-preview portrait + body mask
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <unordered_map>
#include <list>
#include <vector>

// ── GLSL vertex shader (fullscreen triangle, no VBO) ─────────────────────────

static const char* k_body_vert = R"glsl(
#version 330 core
out vec2 v_uv;
void main() {
    vec2 pos[3] = vec2[](vec2(-1.0,-1.0), vec2(3.0,-1.0), vec2(-1.0,3.0));
    gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);
    v_uv = pos[gl_VertexID] * 0.5 + 0.5;
}
)glsl";

// ── GLSL fragment shader preamble ─────────────────────────────────────────────

static const char* k_body_preamble = R"glsl(
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_src;
uniform sampler2D u_mask;
uniform float t;
uniform float u_amount;
uniform float p0, p1, p2, p3;
uniform vec4  u_bg_box;       // RemoveBackground: keep-region [x0,x1,y0,y1] in v_uv (0,1,0,1 = full)
uniform float u_bg_softness;  // RemoveBackground: matte / edge gamma, -1..1

float hash(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}
float noise(vec2 p){
    vec2 i=floor(p),f=fract(p);f=f*f*(3.0-2.0*f);
    return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),
               mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);
}
vec3 hue2rgb(float h){h=fract(h);
    return clamp(abs(fract(h+vec3(0,2.0/3.0,1.0/3.0))*6.0-3.0)-1.0,0.0,1.0);}
float edge_mask(float w){
    vec2 px=1.0/vec2(textureSize(u_src,0));
    float c=texture(u_mask,v_uv).r,e=0.0;
    for(float dx=-w;dx<=w;dx+=1.0)
      for(float dy=-w;dy<=w;dy+=1.0)
        e=max(e,abs(c-texture(u_mask,v_uv+vec2(dx,dy)*px).r));
    return smoothstep(0.05,0.5,e);
}
)glsl";

// ── 41 effect frag bodies ────────────────────────────────────────────────────

// 0: RemoveBackground
static const char* k_frag_RemoveBackground = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    // Bounding box: zero the mask outside the keep-region (u_bg_box = 0,1,0,1 = off).
    if (v_uv.x < u_bg_box.x || v_uv.x > u_bg_box.y ||
        v_uv.y < u_bg_box.z || v_uv.y > u_bg_box.w) m = 0.0;
    // Matte / edge-softness gamma (-1..1), mirroring the old CPU path.
    if (u_bg_softness > 0.001)       m = pow(m, 1.0 + u_bg_softness * 3.0);
    else if (u_bg_softness < -0.001) m = pow(m, 1.0 / (1.0 - u_bg_softness * 3.0));
    vec4 result = vec4(orig.rgb, orig.a * m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 1: NeonOutline
static const char* k_frag_NeonOutline = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float edge = edge_mask(p0 + 1.0);
    vec3 glow_col = hue2rgb(fract(p1 + t * 0.2));
    vec4 body_col = vec4(orig.rgb + glow_col * edge * 2.0, orig.a);
    vec4 bg_col   = orig;  // body-only: leave the background untouched
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 1: VHSBody
static const char* k_frag_VHSBody = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float jitter = sin(v_uv.y * 200.0 + t * 5.0) * p1 * 0.01;
    float bleed = p0 / float(textureSize(u_src, 0).x);
    float r = texture(u_src, clamp(vec2(v_uv.x + jitter + bleed, v_uv.y), 0.0, 1.0)).r;
    float g = texture(u_src, clamp(vec2(v_uv.x + jitter, v_uv.y), 0.0, 1.0)).g;
    float b = texture(u_src, clamp(vec2(v_uv.x + jitter - bleed, v_uv.y), 0.0, 1.0)).b;
    vec4 vhs_col = vec4(r, g, b, orig.a);
    vec4 body_col = mix(orig, vhs_col, m);
    vec4 bg_col   = orig;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 2: Scanlines
static const char* k_frag_Scanlines = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float scan = sin(v_uv.y * p0 * 3.14159) * 0.5 + 0.5;
    vec4 body_col = vec4(orig.rgb * (1.0 - scan * p1), orig.a);
    vec4 bg_col   = orig;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 3: ChromaticBody
static const char* k_frag_ChromaticBody = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float r = texture(u_src, clamp(v_uv + vec2(p0, 0.0), 0.0, 1.0)).r;
    float g = orig.g;
    float b = texture(u_src, clamp(v_uv - vec2(p0, 0.0), 0.0, 1.0)).b;
    vec4 body_col = vec4(r, g, b, orig.a);
    vec4 bg_col   = orig;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 4: Retrowave
static const char* k_frag_Retrowave = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float luma = dot(orig.rgb, vec3(0.299, 0.587, 0.114));
    vec3 retro_body = mix(vec3(1.0, 0.1, 0.8), vec3(0.8, 0.1, 1.0), luma);
    vec4 body_col = vec4(mix(orig.rgb, retro_body, p0), orig.a);
    vec4 bg_col   = orig;  // body-only: leave the background untouched
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 5: TVStaticBg
static const char* k_frag_TVStaticBg = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float n   = noise(v_uv * 500.0 + t * 100.0);
    vec4 bg_col   = mix(orig, vec4(vec3(n), 1.0), p0);
    vec4 body_col = orig;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 6: Arcade
static const char* k_frag_Arcade = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float sz  = max(2.0, p0);
    vec2 pxsz = 1.0 / vec2(textureSize(u_src, 0));
    vec2 block_uv = floor(v_uv / (pxsz * sz)) * (pxsz * sz) + pxsz * sz * 0.5;
    vec4 body_col = texture(u_src, clamp(block_uv, 0.0, 1.0));
    // Simple 3-tap blur for bg
    vec4 bg_col = (texture(u_src, clamp(v_uv + vec2(pxsz.x * 3.0, 0.0), 0.0, 1.0)) +
                   texture(u_src, v_uv) +
                   texture(u_src, clamp(v_uv - vec2(pxsz.x * 3.0, 0.0), 0.0, 1.0))) / 3.0;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 7: FilmGrainBody
static const char* k_frag_FilmGrainBody = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float grain = (hash(v_uv + fract(vec2(t * 0.01))) - 0.5) * p0 * 2.0;
    float luma  = dot(orig.rgb, vec3(0.299, 0.587, 0.114));
    vec3  contr  = mix(vec3(luma), orig.rgb, 1.0 + p1);
    vec4 body_col = vec4(clamp(contr + grain, 0.0, 1.0), orig.a);
    vec4 bg_col   = orig;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 8: DepthBlur
static const char* k_frag_DepthBlur = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    vec2 px   = 1.0 / vec2(textureSize(u_src, 0));
    float r   = p0 * px.x;
    vec4 blurred = vec4(0.0);
    int taps = 9;
    for (int i = -4; i <= 4; i++)
        for (int j = -4; j <= 4; j++)
            blurred += texture(u_src, clamp(v_uv + vec2(float(i) * r, float(j) * r * px.y/px.x), 0.0, 1.0));
    blurred /= 81.0;
    vec4 body_col = orig;
    vec4 bg_col   = blurred;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 9: TiltShiftBg
static const char* k_frag_TiltShiftBg = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    vec2 px   = 1.0 / vec2(textureSize(u_src, 0));
    float blur_strength = smoothstep(0.0, 0.3, abs(v_uv.y - p1));
    float r = p0 * px.x * blur_strength;
    vec4 blurred = vec4(0.0);
    for (int i = -4; i <= 4; i++)
        for (int j = -4; j <= 4; j++)
            blurred += texture(u_src, clamp(v_uv + vec2(float(i) * r, float(j) * r * px.y/px.x), 0.0, 1.0));
    blurred /= 81.0;
    vec4 bg_col   = blurred;
    vec4 body_col = orig;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 10: Spotlight
static const char* k_frag_Spotlight = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    vec4 bg_col   = vec4(orig.rgb * (1.0 - p0), orig.a);
    vec4 body_col = vec4(clamp(orig.rgb * 1.15, 0.0, 1.0), orig.a);
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 11: FogBg
static const char* k_frag_FogBg = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float fog_n = noise(v_uv * 3.0 + t * 0.1);
    float fog   = p0 * (0.5 + 0.5 * fog_n);
    vec4 bg_col   = mix(orig, vec4(0.9, 0.9, 0.95, 1.0), fog);
    vec4 body_col = orig;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 12: BokehBg
static const char* k_frag_BokehBg = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    vec2 px   = 1.0 / vec2(textureSize(u_src, 0));
    float r   = p0 * px.x;
    // 13-tap circular kernel
    vec4 blurred = vec4(0.0);
    float count  = 0.0;
    for (float a = 0.0; a < 6.2832; a += 0.4836) {
        float dx = cos(a) * r, dy = sin(a) * r * px.y/px.x;
        blurred += texture(u_src, clamp(v_uv + vec2(dx, dy), 0.0, 1.0));
        count += 1.0;
    }
    blurred += texture(u_src, v_uv); count += 1.0;
    blurred /= count;
    vec4 bg_col   = blurred;
    vec4 body_col = orig;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 13: GlitchBody
static const char* k_frag_GlitchBody = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    vec2 px   = 1.0 / vec2(textureSize(u_src, 0));
    float chroma = p0 * px.x;
    float y_id  = floor(v_uv.y * float(textureSize(u_src, 0).y));
    float rnd   = hash(vec2(y_id, floor(t * 12.0)));
    float jshift = (rnd > 1.0 - p1 * 0.4) ? (hash(vec2(y_id, t * 8.0)) - 0.5) * p1 * 0.12 : 0.0;
    float r = texture(u_src, clamp(vec2(v_uv.x + jshift + chroma, v_uv.y), 0.0, 1.0)).r;
    float g = texture(u_src, clamp(vec2(v_uv.x + jshift, v_uv.y), 0.0, 1.0)).g;
    float b = texture(u_src, clamp(vec2(v_uv.x + jshift - chroma, v_uv.y), 0.0, 1.0)).b;
    vec4 body_col = vec4(r, g, b, orig.a);
    vec4 bg_col   = orig;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 14: GlitchBg
static const char* k_frag_GlitchBg = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    vec2 px   = 1.0 / vec2(textureSize(u_src, 0));
    float chroma = p0 * px.x;
    float y_id  = floor(v_uv.y * float(textureSize(u_src, 0).y));
    float rnd   = hash(vec2(y_id, floor(t * 12.0)));
    float jshift = (rnd > 1.0 - p1 * 0.4) ? (hash(vec2(y_id, t * 8.0)) - 0.5) * p1 * 0.12 : 0.0;
    float r = texture(u_src, clamp(vec2(v_uv.x + jshift + chroma, v_uv.y), 0.0, 1.0)).r;
    float g = texture(u_src, clamp(vec2(v_uv.x + jshift, v_uv.y), 0.0, 1.0)).g;
    float b = texture(u_src, clamp(vec2(v_uv.x + jshift - chroma, v_uv.y), 0.0, 1.0)).b;
    vec4 bg_col   = vec4(r, g, b, orig.a);
    vec4 body_col = orig;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 15: SplitReality
static const char* k_frag_SplitReality = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    vec2 px   = 1.0 / vec2(textureSize(u_src, 0));
    float body_shift = sin(t * 3.0) * p0 * 0.02;
    float bg_shift   = -sin(t * 3.0 + 1.57) * p0 * 0.015;
    vec4 body_col = texture(u_src, clamp(v_uv + vec2(body_shift, 0.0), 0.0, 1.0));
    vec4 bg_col   = texture(u_src, clamp(v_uv + vec2(bg_shift, 0.0), 0.0, 1.0));
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 16: DataCorruption
static const char* k_frag_DataCorruption = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float steps = 4.0 + p0 * 28.0;
    // Block quantize with grid jitter
    vec2 block = vec2(16.0, 16.0);
    vec2 block_uv = floor(v_uv * block) / block;
    float jitter_n = hash(block_uv + floor(t * 3.0) * 0.1);
    vec2 sample_uv = v_uv;
    if (jitter_n > 0.8) sample_uv = clamp(block_uv + 0.5/block, 0.0, 1.0);
    vec4 src = texture(u_src, sample_uv);
    vec4 quantized = floor(src * steps) / steps;
    vec4 body_col = quantized;
    vec4 bg_col   = orig;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 17: SignalLossBg
static const char* k_frag_SignalLossBg = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float row = floor(v_uv.y * 50.0);
    float shift_x = noise(vec2(row, t)) * p0 * 0.1 - p0 * 0.05;
    vec4 bg_col   = texture(u_src, clamp(v_uv + vec2(shift_x, 0.0), 0.0, 1.0));
    vec4 body_col = orig;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 18: Chromafall
static const char* k_frag_Chromafall = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float fall_r = sin(t * p0) * p1;
    float fall_b = cos(t * p0) * p1;
    float r = texture(u_src, clamp(v_uv + vec2(0.0, fall_r), 0.0, 1.0)).r;
    float g = orig.g;
    float b = texture(u_src, clamp(v_uv - vec2(0.0, fall_b), 0.0, 1.0)).b;
    vec4 body_col = vec4(r, g, b, orig.a);
    vec4 bg_col   = orig;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 19: ColorIsolation
static const char* k_frag_ColorIsolation = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float gray = dot(orig.rgb, vec3(0.299, 0.587, 0.114));
    vec4 bg_col   = vec4(vec3(gray), orig.a);
    vec4 body_col = orig;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 20: InvertBg
static const char* k_frag_InvertBg = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    vec4 bg_col   = vec4(1.0 - orig.rgb, orig.a);
    vec4 body_col = orig;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 21: DuotoneBody
static const char* k_frag_DuotoneBody = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float luma = dot(orig.rgb, vec3(0.299, 0.587, 0.114));
    vec3 duo = mix(hue2rgb(p0), hue2rgb(p1), luma);
    vec4 body_col = vec4(duo, orig.a);
    vec4 bg_col   = orig;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 22: DuotoneBg
static const char* k_frag_DuotoneBg = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float luma = dot(orig.rgb, vec3(0.299, 0.587, 0.114));
    vec3 duo = mix(hue2rgb(p0), hue2rgb(p1), luma);
    vec4 bg_col   = vec4(duo, orig.a);
    vec4 body_col = orig;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 23: ThermalBody
static const char* k_frag_ThermalBody = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float luma = dot(orig.rgb, vec3(0.299, 0.587, 0.114));
    // Blue(0) -> Cyan -> Green -> Yellow -> Red(1)
    vec3 thermal;
    if (luma < 0.25)      thermal = mix(vec3(0,0,1),   vec3(0,1,1),   luma * 4.0);
    else if (luma < 0.5)  thermal = mix(vec3(0,1,1),   vec3(0,1,0),   (luma-0.25)*4.0);
    else if (luma < 0.75) thermal = mix(vec3(0,1,0),   vec3(1,1,0),   (luma-0.5)*4.0);
    else                  thermal = mix(vec3(1,1,0),   vec3(1,0,0),   (luma-0.75)*4.0);
    float gray = luma * 0.3;
    vec4 bg_col   = vec4(vec3(gray), orig.a);
    vec4 body_col = vec4(thermal, orig.a);
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 24: XRay
static const char* k_frag_XRay = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float edge = edge_mask(1.0);
    float luma = dot(orig.rgb, vec3(0.299, 0.587, 0.114));
    float inside_glow = m * luma * 0.4;
    vec3 xray = vec3(0.6, 0.8, 1.0) * (edge * 2.0 + inside_glow);
    vec4 body_col = vec4(clamp(xray, 0.0, 1.0), orig.a);
    vec4 bg_col   = orig;  // body-only: leave the background untouched
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 25: ElectricAura
static const char* k_frag_ElectricAura = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float edge = edge_mask(p0);
    float pulse = 0.5 + 0.5 * sin(t * p1 * 6.28318);
    float hue_v = fract(t * 0.15 + v_uv.y * 0.1);
    vec3 glow = hue2rgb(hue_v) * edge * pulse * 2.5;
    vec4 body_col = vec4(clamp(orig.rgb + glow, 0.0, 1.0), orig.a);
    vec4 bg_col   = orig;  // body-only: leave the background untouched
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 26: RimLight
static const char* k_frag_RimLight = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float edge = edge_mask(p1);
    vec3 rim = hue2rgb(p0) * edge * 2.0;
    vec4 body_col = vec4(clamp(orig.rgb + rim, 0.0, 1.0), orig.a);
    vec4 bg_col   = orig;  // body-only: leave the background untouched
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 27: GodRays
static const char* k_frag_GodRays = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    // Approximate center of body as 0.5, 0.5 (simple approach)
    vec2 center = vec2(0.5, 0.4);
    vec2 dir = normalize(center - v_uv);
    float steps = 16.0;
    float decay = 0.9;
    float acc = 0.0;
    float w_acc = 1.0;
    float w_sum = 0.0;
    for (float i = 0.0; i < steps; i++) {
        vec2 uv = clamp(v_uv + dir * (i / steps) * p1, 0.0, 1.0);
        acc += texture(u_mask, uv).r * w_acc;
        w_sum += w_acc;
        w_acc *= decay;
    }
    float ray = acc / w_sum * p0;
    vec3 ray_col = vec3(1.0, 0.9, 0.7) * ray;
    vec4 body_col = vec4(clamp(orig.rgb + ray_col, 0.0, 1.0), orig.a);
    vec4 bg_col   = vec4(clamp(orig.rgb + ray_col * 0.5, 0.0, 1.0), orig.a);
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 28: Halo
static const char* k_frag_Halo = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    // Sample mask at slightly larger radius to get body boundary
    float halo = 0.0;
    float ring_r = p0;
    vec2 px = 1.0 / vec2(textureSize(u_src, 0));
    for (float a = 0.0; a < 6.2832; a += 0.3927) {
        vec2 off = vec2(cos(a), sin(a)) * ring_r;
        float s = texture(u_mask, clamp(v_uv + off * px * 40.0, 0.0, 1.0)).r;
        halo = max(halo, s * (1.0 - m));
    }
    float soft = exp(-halo * (1.0 - p1) * 5.0);
    halo *= soft;
    vec3 halo_col = vec3(1.0, 0.95, 0.8) * halo * 2.0;
    vec4 body_col = orig;
    vec4 bg_col   = vec4(clamp(orig.rgb + halo_col, 0.0, 1.0), orig.a);
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 29: BloomBody
static const char* k_frag_BloomBody = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    vec2 px = 1.0 / vec2(textureSize(u_src, 0));
    // Extract bright areas of body and blur them
    vec4 bloom = vec4(0.0);
    float sum = 0.0;
    for (int i = -5; i <= 5; i++) {
        for (int j = -5; j <= 5; j++) {
            vec2 off = vec2(float(i), float(j)) * px * 3.0;
            vec4 s = texture(u_src, clamp(v_uv + off, 0.0, 1.0));
            float mk = texture(u_mask, clamp(v_uv + off, 0.0, 1.0)).r;
            float luma = dot(s.rgb, vec3(0.299, 0.587, 0.114));
            float bright = max(0.0, luma - p0) * mk;
            bloom += s * bright;
            sum += bright + 0.001;
        }
    }
    bloom /= sum;
    vec4 body_col = vec4(clamp(orig.rgb + bloom.rgb * p1, 0.0, 1.0), orig.a);
    vec4 bg_col   = orig;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 30: Bioluminescent
static const char* k_frag_Bioluminescent = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float edge = edge_mask(2.0);
    float pulse = 0.5 + 0.5 * sin(t * p1 * 3.14159);
    vec3 bio_col = mix(vec3(0.0, 1.0, 0.6), vec3(0.0, 0.8, 1.0), pulse);
    vec3 glow = bio_col * edge * p0 * 2.0;
    vec4 body_col = vec4(clamp(orig.rgb + glow, 0.0, 1.0), orig.a);
    vec4 bg_col   = orig;  // body-only: leave the background untouched
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 31: Hologram
static const char* k_frag_Hologram = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float scan    = sin(v_uv.y * p1 * 3.14159) * 0.5 + 0.5;
    float flicker = 0.5 + 0.5 * sin(t * p0 * 17.3);
    vec3 holo_col = orig.rgb * scan * flicker;
    holo_col *= vec3(0.3, 0.8, 1.0);
    vec4 body_col = vec4(holo_col, orig.a);
    vec4 bg_col   = orig;  // body-only: leave the background untouched
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 32: MatrixRainBg
static const char* k_frag_MatrixRainBg = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float col_x = floor(v_uv.x * p1);
    float drop_y = fract(v_uv.y + hash(vec2(col_x, 0.0)) * 0.3 + t * p0 * 0.2);
    float char_bright = hash(vec2(col_x, floor(drop_y * 20.0)));
    float is_char = step(0.5, char_bright);
    float brightness = is_char > 0.5 ? 0.8 + char_bright * 0.2 : 0.1;
    float fade_trail = 1.0 - drop_y;
    vec3 matrix_col = vec3(0.0, brightness * fade_trail, 0.0);
    vec4 bg_col   = mix(orig * 0.1, vec4(matrix_col, 1.0), 0.8);
    vec4 body_col = orig;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 33: BodyMosaic
static const char* k_frag_BodyMosaic = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    vec2 px   = 1.0 / vec2(textureSize(u_src, 0));
    float sz  = max(2.0, p0);
    vec2 block_uv = floor(v_uv / (px * sz)) * (px * sz) + px * sz * 0.5;
    vec4 body_col = texture(u_src, clamp(block_uv, 0.0, 1.0));
    // 5-tap box blur for bg
    vec4 bg_col = (texture(u_src, clamp(v_uv + vec2(px.x*4.0, 0.0), 0.0, 1.0)) +
                   texture(u_src, clamp(v_uv + vec2(0.0, px.y*4.0), 0.0, 1.0)) +
                   texture(u_src, v_uv) +
                   texture(u_src, clamp(v_uv - vec2(px.x*4.0, 0.0), 0.0, 1.0)) +
                   texture(u_src, clamp(v_uv - vec2(0.0, px.y*4.0), 0.0, 1.0))) / 5.0;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 34: WireframeBg
static const char* k_frag_WireframeBg = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    vec2 px   = 1.0 / vec2(textureSize(u_src, 0));
    // Sobel edge detection on bg
    vec3 tl = texture(u_src, clamp(v_uv + vec2(-px.x, -px.y), 0.0, 1.0)).rgb;
    vec3 tc = texture(u_src, clamp(v_uv + vec2(  0.0, -px.y), 0.0, 1.0)).rgb;
    vec3 tr = texture(u_src, clamp(v_uv + vec2( px.x, -px.y), 0.0, 1.0)).rgb;
    vec3 ml = texture(u_src, clamp(v_uv + vec2(-px.x,   0.0), 0.0, 1.0)).rgb;
    vec3 mr = texture(u_src, clamp(v_uv + vec2( px.x,   0.0), 0.0, 1.0)).rgb;
    vec3 bl = texture(u_src, clamp(v_uv + vec2(-px.x,  px.y), 0.0, 1.0)).rgb;
    vec3 bc = texture(u_src, clamp(v_uv + vec2(  0.0,  px.y), 0.0, 1.0)).rgb;
    vec3 br = texture(u_src, clamp(v_uv + vec2( px.x,  px.y), 0.0, 1.0)).rgb;
    vec3 gx = -tl - 2.0*ml - bl + tr + 2.0*mr + br;
    vec3 gy = -tl - 2.0*tc - tr + bl + 2.0*bc + br;
    float edge = length(gx) + length(gy);
    edge = smoothstep(p1, p1 + 0.3, edge);
    vec3 wire_col = hue2rgb(p0) * edge;
    vec4 bg_col   = vec4(mix(orig.rgb * 0.05, wire_col, edge), orig.a);
    vec4 body_col = orig;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 35: Vaporwave
static const char* k_frag_Vaporwave = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    // bg: pink-purple gradient
    vec3 bg_top = vec3(0.5, 0.1, 0.8);
    vec3 bg_bot = vec3(1.0, 0.2, 0.6);
    vec3 bg_grad = mix(bg_top, bg_bot, v_uv.y);
    vec4 bg_col = mix(vec4(orig.rgb * 0.3, orig.a), vec4(bg_grad, 1.0), p0);
    // body: boost saturation + tint pink
    float luma = dot(orig.rgb, vec3(0.299, 0.587, 0.114));
    vec3 body_rgb = mix(vec3(luma), orig.rgb, 1.4);
    body_rgb = clamp(body_rgb + vec3(0.05, 0.0, 0.05) * p0, 0.0, 1.0);
    vec4 body_col = vec4(body_rgb, orig.a);
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 36: StrobeBg
static const char* k_frag_StrobeBg = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float flash_h = fract(floor(t * p0) * 0.31 + p1);
    vec3 strobe = hue2rgb(flash_h);
    vec4 bg_col   = vec4(strobe, 1.0);
    vec4 body_col = orig;
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 37: DiscoBody
static const char* k_frag_DiscoBody = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float hue_v = fract(v_uv.y + t * p0 + v_uv.x * 0.3);
    vec3 rainbow = hue2rgb(hue_v);
    vec4 body_col = vec4(orig.rgb * rainbow, orig.a);
    vec4 bg_col   = orig;  // body-only: leave the background untouched
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 38: UVPaint
static const char* k_frag_UVPaint = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float luma = dot(orig.rgb, vec3(0.299, 0.587, 0.114));
    float sat  = length(orig.rgb - vec3(luma));
    vec3 uv_body = clamp(orig.rgb * (1.0 + p0 * sat * 4.0), 0.0, 1.0);
    vec4 body_col = vec4(uv_body, orig.a);
    vec4 bg_col   = orig;  // body-only: leave the background untouched
    vec4 result = mix(bg_col, body_col, m);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 39: FireAura
static const char* k_frag_FireAura = R"glsl(
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float edge = edge_mask(3.0);
    // Heat distortion
    vec2 px = 1.0 / vec2(textureSize(u_src, 0));
    float distort = noise(v_uv * 12.0 + vec2(0.0, t * p1 * 2.0));
    vec2 dist_uv = v_uv + vec2(distort - 0.5, distort - 0.5) * px * 8.0 * edge;
    vec4 body_src = texture(u_src, clamp(dist_uv, 0.0, 1.0));
    float fire_n = noise(v_uv * 8.0 + vec2(0.0, t * p1));
    vec3 fire_col = mix(vec3(1.0, 0.3, 0.0), vec3(1.0, 0.8, 0.0), fire_n);
    float glow = edge * p0 * (1.0 - m);  // glow on bg side of edge
    vec4 body_col = body_src;
    vec4 bg_col   = vec4(orig.rgb * 0.7 + fire_col * glow, orig.a);
    vec4 result = mix(bg_col, body_col, m);
    // Add fire aura on top of both
    result.rgb = clamp(result.rgb + fire_col * edge * p0 * 0.8, 0.0, 1.0);
    frag = mix(orig, result, u_amount);
}
)glsl";

// 40: BeautySmooth — person-masked beauty smoothing. Bilateral blur runs
// only inside the body mask AND a skin-tone gate, so a skin-toned wall
// behind you stays sharp and so do eyes/hair inside the silhouette.
static const char* k_frag_BeautySmooth = R"glsl(
float luma_bs(vec3 c){ return dot(c, vec3(0.299, 0.587, 0.114)); }
float skin_bs(vec3 c){
    float cb = 0.5 - 0.168736*c.r - 0.331264*c.g + 0.5*c.b;
    float cr = 0.5 + 0.5*c.r - 0.418688*c.g - 0.081312*c.b;
    float w  = 0.02 + 0.06*p1;
    float mb = smoothstep(0.302-w,0.302+w,cb)*(1.0-smoothstep(0.498-w,0.498+w,cb));
    float mr = smoothstep(0.522-w,0.522+w,cr)*(1.0-smoothstep(0.678-w,0.678+w,cr));
    return mb*mr;
}
void main() {
    vec4 orig = texture(u_src, v_uv);
    float m   = texture(u_mask, v_uv).r;
    float gate = m * skin_bs(orig.rgb);
    if (gate < 0.01) { frag = orig; return; }
    vec2 px = 1.0 / vec2(textureSize(u_src, 0));
    float lc = luma_bs(orig.rgb);
    vec3 acc = orig.rgb;
    float wsum = 1.0;
    float r = max(1.0, p0);
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            if (dx == 0 && dy == 0) continue;
            vec3 s = texture(u_src, v_uv + vec2(float(dx), float(dy)) * (r*0.5) * px).rgb;
            float dl = abs(luma_bs(s) - lc);
            float wr = exp(-dl*dl*60.0);
            float ws = exp(-float(dx*dx + dy*dy)*0.12);
            acc  += s * wr * ws;
            wsum += wr * ws;
        }
    }
    vec4 result = vec4(mix(orig.rgb, acc / wsum, gate), orig.a);
    frag = mix(orig, result, u_amount);
}
)glsl";

// ── Effect info table ─────────────────────────────────────────────────────────

// Accent colors as IM_COL32(r,g,b,a) encoded as unsigned int
#define ACOL(r,g,b) ((unsigned)(0xFF000000u | ((unsigned)(b) << 16) | ((unsigned)(g) << 8) | (unsigned)(r)))

static const BodyFXInfo g_body_fx_infos[] = {
    // 0: RemoveBackground
    { BodyFXType::RemoveBackground, "Remove Background", "AI mask removes background, keeps subject", "Mask",
      ACOL(60, 200, 255), 0,
      {{}, {}, {}, {}},
      k_frag_RemoveBackground },
    // 1: NeonOutline
    { BodyFXType::NeonOutline, "Neon Outline", "Glowing edge trace on body silhouette", "Retro / 80s",
      ACOL(255,80,200), 2,
      {{"Glow Width", 1.f, 4.f, 2.f}, {"Hue", 0.f, 1.f, 0.8f}, {}, {}},
      k_frag_NeonOutline },
    // 1: VHSBody
    { BodyFXType::VHSBody, "VHS Body", "Chroma bleed + row jitter on body, clean bg", "Retro / 80s",
      ACOL(110,195,95), 2,
      {{"Bleed (px)", 0.f, 20.f, 8.f}, {"Jitter", 0.f, 1.f, 0.3f}, {}, {}},
      k_frag_VHSBody },
    // 2: Scanlines
    { BodyFXType::Scanlines, "Scanlines", "CRT scanlines on body, clear background", "Retro / 80s",
      ACOL(80,200,180), 2,
      {{"Density", 50.f, 200.f, 120.f}, {"Darkness", 0.f, 1.f, 0.6f}, {}, {}},
      k_frag_Scanlines },
    // 3: ChromaticBody
    { BodyFXType::ChromaticBody, "Chromatic Body", "RGB split on body only", "Retro / 80s",
      ACOL(0,210,220), 1,
      {{"Spread", 0.f, 0.03f, 0.008f}, {}, {}, {}},
      k_frag_ChromaticBody },
    // 4: Retrowave
    { BodyFXType::Retrowave, "Retrowave", "Purple/pink body duotone, dark teal bg", "Retro / 80s",
      ACOL(200,80,255), 1,
      {{"Intensity", 0.f, 1.f, 0.8f}, {}, {}, {}},
      k_frag_Retrowave },
    // 5: TVStaticBg
    { BodyFXType::TVStaticBg, "TV Static Bg", "Static noise on background, clean body", "Retro / 80s",
      ACOL(180,180,180), 1,
      {{"Density", 0.f, 1.f, 0.6f}, {}, {}, {}},
      k_frag_TVStaticBg },
    // 6: Arcade
    { BodyFXType::Arcade, "Arcade", "Pixelate body into blocks, blurred bg", "Retro / 80s",
      ACOL(255,135,40), 1,
      {{"Pixel Size", 2.f, 20.f, 8.f}, {}, {}, {}},
      k_frag_Arcade },
    // 7: FilmGrainBody
    { BodyFXType::FilmGrainBody, "Film Grain Body", "Heavy grain + contrast on body", "Retro / 80s",
      ACOL(200,160,100), 2,
      {{"Grain", 0.f, 0.5f, 0.25f}, {"Contrast", 0.f, 0.5f, 0.2f}, {}, {}},
      k_frag_FilmGrainBody },
    // 8: DepthBlur
    { BodyFXType::DepthBlur, "Depth Blur", "Sharp body, Gaussian bg blur", "Depth / Focus",
      ACOL(100,160,255), 1,
      {{"Blur Radius (px)", 0.f, 20.f, 8.f}, {}, {}, {}},
      k_frag_DepthBlur },
    // 9: TiltShiftBg
    { BodyFXType::TiltShiftBg, "Tilt-Shift Bg", "Tilt-shift blur bands on background", "Depth / Focus",
      ACOL(80,200,255), 2,
      {{"Blur", 0.f, 15.f, 6.f}, {"Band Position", 0.f, 1.f, 0.5f}, {}, {}},
      k_frag_TiltShiftBg },
    // 10: Spotlight
    { BodyFXType::Spotlight, "Spotlight", "Body lit, background darkened", "Depth / Focus",
      ACOL(255,220,100), 1,
      {{"Darkness", 0.f, 1.f, 0.7f}, {}, {}, {}},
      k_frag_Spotlight },
    // 11: FogBg
    { BodyFXType::FogBg, "Fog Bg", "Exponential haze on background", "Depth / Focus",
      ACOL(200,210,220), 1,
      {{"Density", 0.f, 1.f, 0.6f}, {}, {}, {}},
      k_frag_FogBg },
    // 12: BokehBg
    { BodyFXType::BokehBg, "Bokeh Bg", "Circular bokeh blur on background", "Depth / Focus",
      ACOL(100,140,255), 1,
      {{"Radius (px)", 0.f, 15.f, 6.f}, {}, {}, {}},
      k_frag_BokehBg },
    // 13: GlitchBody
    { BodyFXType::GlitchBody, "Glitch Body", "RGB split + jitter on body, clean bg", "Glitch",
      ACOL(0,210,220), 2,
      {{"Chroma (px)", 0.f, 20.f, 8.f}, {"Jitter", 0.f, 1.f, 0.3f}, {}, {}},
      k_frag_GlitchBody },
    // 14: GlitchBg
    { BodyFXType::GlitchBg, "Glitch Bg", "RGB split + jitter on bg, clean body", "Glitch",
      ACOL(0,180,200), 2,
      {{"Chroma (px)", 0.f, 20.f, 8.f}, {"Jitter", 0.f, 1.f, 0.3f}, {}, {}},
      k_frag_GlitchBg },
    // 15: SplitReality
    { BodyFXType::SplitReality, "Split Reality", "Body and bg glitch in opposite directions", "Glitch",
      ACOL(200,60,255), 1,
      {{"Intensity", 0.f, 1.f, 0.6f}, {}, {}, {}},
      k_frag_SplitReality },
    // 16: DataCorruption
    { BodyFXType::DataCorruption, "Data Corruption", "JPEG-block artifacts on body, static bg", "Glitch",
      ACOL(255,60,100), 1,
      {{"Intensity", 0.f, 1.f, 0.5f}, {}, {}, {}},
      k_frag_DataCorruption },
    // 17: SignalLossBg
    { BodyFXType::SignalLossBg, "Signal Loss Bg", "Bg rows drop and shift, clean body", "Glitch",
      ACOL(60,200,100), 1,
      {{"Intensity", 0.f, 1.f, 0.5f}, {}, {}, {}},
      k_frag_SignalLossBg },
    // 18: Chromafall
    { BodyFXType::Chromafall, "Chromafall", "Color channels fall downward on body", "Glitch",
      ACOL(255,100,200), 2,
      {{"Speed", 0.1f, 3.f, 1.f}, {"Spread", 0.f, 0.05f, 0.02f}, {}, {}},
      k_frag_Chromafall },
    // 19: ColorIsolation
    { BodyFXType::ColorIsolation, "Color Isolation", "Body full color, background grayscale", "Color",
      ACOL(255,200,50), 0,
      {{}, {}, {}, {}},
      k_frag_ColorIsolation },
    // 20: InvertBg
    { BodyFXType::InvertBg, "Invert Bg", "Body normal, background inverted colors", "Color",
      ACOL(200,100,255), 0,
      {{}, {}, {}, {}},
      k_frag_InvertBg },
    // 21: DuotoneBody
    { BodyFXType::DuotoneBody, "Duotone Body", "Two-color gradient mapped onto body luma", "Color",
      ACOL(255,80,150), 2,
      {{"Hue A", 0.f, 1.f, 0.6f}, {"Hue B", 0.f, 1.f, 0.1f}, {}, {}},
      k_frag_DuotoneBody },
    // 22: DuotoneBg
    { BodyFXType::DuotoneBg, "Duotone Bg", "Two-color gradient mapped onto bg luma", "Color",
      ACOL(200,80,255), 2,
      {{"Hue A", 0.f, 1.f, 0.7f}, {"Hue B", 0.f, 1.f, 0.2f}, {}, {}},
      k_frag_DuotoneBg },
    // 23: ThermalBody
    { BodyFXType::ThermalBody, "Thermal Body", "Infrared heat palette on body", "Color",
      ACOL(255,120,40), 0,
      {{}, {}, {}, {}},
      k_frag_ThermalBody },
    // 24: XRay
    { BodyFXType::XRay, "X-Ray", "Body luminous white/blue silhouette on black", "Color",
      ACOL(180,220,255), 1,
      {{"Threshold", 0.f, 1.f, 0.3f}, {}, {}, {}},
      k_frag_XRay },
    // 25: ElectricAura
    { BodyFXType::ElectricAura, "Electric Aura", "Pulsing neon edge glow, dark bg", "Light / Glow",
      ACOL(80,200,255), 2,
      {{"Width", 1.f, 5.f, 2.f}, {"Speed", 0.5f, 3.f, 1.5f}, {}, {}},
      k_frag_ElectricAura },
    // 26: RimLight
    { BodyFXType::RimLight, "Rim Light", "Edge highlight on body silhouette", "Light / Glow",
      ACOL(255,220,100), 2,
      {{"Hue", 0.f, 1.f, 0.15f}, {"Width", 1.f, 4.f, 2.f}, {}, {}},
      k_frag_RimLight },
    // 27: GodRays
    { BodyFXType::GodRays, "God Rays", "Light shafts from body outward", "Light / Glow",
      ACOL(255,230,150), 2,
      {{"Intensity", 0.f, 1.f, 0.6f}, {"Spread", 0.01f, 0.1f, 0.05f}, {}, {}},
      k_frag_GodRays },
    // 28: Halo
    { BodyFXType::Halo, "Halo", "Soft radial glow around body", "Light / Glow",
      ACOL(255,240,200), 2,
      {{"Radius", 0.1f, 0.5f, 0.3f}, {"Softness", 0.f, 1.f, 0.5f}, {}, {}},
      k_frag_Halo },
    // 29: BloomBody
    { BodyFXType::BloomBody, "Bloom Body", "High-exposure bloom on bright body areas", "Light / Glow",
      ACOL(255,255,200), 2,
      {{"Threshold", 0.f, 1.f, 0.5f}, {"Intensity", 0.f, 2.f, 1.f}, {}, {}},
      k_frag_BloomBody },
    // 30: Bioluminescent
    { BodyFXType::Bioluminescent, "Bioluminescent", "Green/teal glow seeping from body edges", "Light / Glow",
      ACOL(0,220,160), 2,
      {{"Intensity", 0.f, 2.f, 1.f}, {"Speed", 0.2f, 2.f, 1.f}, {}, {}},
      k_frag_Bioluminescent },
    // 31: Hologram
    { BodyFXType::Hologram, "Hologram", "Scanlines + transparency flicker on body", "Abstract",
      ACOL(80,200,255), 2,
      {{"Flicker", 0.5f, 5.f, 2.f}, {"Lines", 50.f, 200.f, 100.f}, {}, {}},
      k_frag_Hologram },
    // 32: MatrixRainBg
    { BodyFXType::MatrixRainBg, "Matrix Rain Bg", "Falling green characters on background", "Abstract",
      ACOL(0,200,60), 2,
      {{"Speed", 0.5f, 3.f, 1.f}, {"Density", 10.f, 50.f, 25.f}, {}, {}},
      k_frag_MatrixRainBg },
    // 33: BodyMosaic
    { BodyFXType::BodyMosaic, "Body Mosaic", "Pixelate body into large tiles", "Abstract",
      ACOL(255,165,80), 1,
      {{"Pixel Size", 2.f, 32.f, 12.f}, {}, {}, {}},
      k_frag_BodyMosaic },
    // 34: WireframeBg
    { BodyFXType::WireframeBg, "Wireframe Bg", "Edge-detect background — wireframe look", "Abstract",
      ACOL(100,200,255), 2,
      {{"Hue", 0.f, 1.f, 0.5f}, {"Threshold", 0.f, 0.5f, 0.1f}, {}, {}},
      k_frag_WireframeBg },
    // 35: Vaporwave
    { BodyFXType::Vaporwave, "Vaporwave", "Pink-purple gradient bg, saturated body", "Abstract",
      ACOL(255,100,220), 1,
      {{"Intensity", 0.f, 1.f, 0.8f}, {}, {}, {}},
      k_frag_Vaporwave },
    // 36: StrobeBg
    { BodyFXType::StrobeBg, "Strobe Bg", "Background color-flashes, body normal", "Party",
      ACOL(255,255,100), 2,
      {{"Speed", 1.f, 10.f, 4.f}, {"Color Offset", 0.f, 1.f, 0.f}, {}, {}},
      k_frag_StrobeBg },
    // 37: DiscoBody
    { BodyFXType::DiscoBody, "Disco Body", "Rainbow color cycling per scanline on body", "Party",
      ACOL(255,80,200), 1,
      {{"Speed", 0.5f, 3.f, 1.f}, {}, {}, {}},
      k_frag_DiscoBody },
    // 38: UVPaint
    { BodyFXType::UVPaint, "UV Paint", "UV-lit body: saturated colors boosted", "Party",
      ACOL(160,80,255), 1,
      {{"Intensity", 0.f, 1.f, 0.7f}, {}, {}, {}},
      k_frag_UVPaint },
    // 39: FireAura
    { BodyFXType::FireAura, "Fire Aura", "Orange-red heat distortion bleeding from body", "Party",
      ACOL(255,120,20), 2,
      {{"Intensity", 0.f, 1.f, 0.7f}, {"Speed", 0.5f, 3.f, 1.f}, {}, {}},
      k_frag_FireAura },
    // 40: BeautySmooth
    { BodyFXType::BeautySmooth, "Beauty Smooth", "Skin smoothing masked to the person — background stays sharp", "Beauty",
      ACOL(255,180,160), 2,
      {{"Smoothness", 1.f, 6.f, 3.f}, {"Skin Range", 0.f, 1.f, 0.5f}, {}, {}},
      k_frag_BeautySmooth },
};

static const int k_n_body_fx = (int)(sizeof(g_body_fx_infos) / sizeof(g_body_fx_infos[0]));

// ── GL state ──────────────────────────────────────────────────────────────────

static GLuint g_programs[(int)BodyFXType::Count] = {};
static GLuint g_vao = 0;

// Output FBO/texture — a PING-PONG PAIR (resized lazily). Chaining body-FX feeds one
// effect's output as the next effect's source; with a single shared FBO that means
// sampling u_src while rendering into the SAME texture (read-write feedback → undefined →
// the cutout alpha randomly comes out opaque on a re-scrub). The pair lets each effect
// read one buffer and write the OTHER, so source != destination always.
static struct {
    GLuint fbo[2] = {0, 0}, tex[2] = {0, 0};
    int w = 0, h = 0;
} g_out;

// Mask texture cache: key = "mask_dir/NNNNNN.png", value = GLuint
// LRU: list of keys in use order, map for O(1) lookup
static std::list<std::string>                          g_mask_lru;
static std::unordered_map<std::string, std::pair<GLuint, std::list<std::string>::iterator>> g_mask_cache;
static const int k_mask_cache_max = 128;

// ── MJPEG seek-table cache ────────────────────────────────────────────────────
// Maps mask_dir → {offsets vector, start_frame}.
// Built lazily on first access; cleared entry when masks are updated (append/rewrite).
struct MaskIndex {
    std::vector<uint64_t> offsets;
    int start_frame = 0;
};
static std::unordered_map<std::string, MaskIndex> g_mask_index;

// Returns pointer to a loaded MaskIndex for mask_dir, or nullptr if unavailable.
static const MaskIndex* get_mask_index(const std::string& mask_dir) {
    auto it = g_mask_index.find(mask_dir);
    if (it != g_mask_index.end()) return &it->second;

    std::string mjpeg = mask_dir + "/bg_masks.mjpeg";
    std::string idx   = mask_dir + "/bg_masks.idx";

    // Build .idx from MJPEG if missing or stale
    {
        FILE* mf = fopen(mjpeg.c_str(), "rb");
        if (!mf) return nullptr;
        fclose(mf);
    }

    bool need_build = true;
    {
        FILE* idxf = fopen(idx.c_str(), "rb");
        if (idxf) { need_build = false; fclose(idxf); }
    }

    if (need_build) {
        FILE* mf = fopen(mjpeg.c_str(), "rb");
        if (!mf) return nullptr;
        std::vector<uint64_t> offsets;
        offsets.reserve(8192);
        uint8_t carry[2] = {0, 0};
        bool has_carry = false;
        uint8_t chunk[65536];
        int64_t abs_off = 0;
        while (true) {
            size_t nr = fread(chunk, 1, sizeof(chunk), mf);
            if (nr == 0) break;
            size_t start = 0;
            if (has_carry) {
                // Check boundary: carry[0] carry[1] chunk[0]
                if (carry[0] == 0xFF && carry[1] == 0xD8 &&
                    nr > 0 && chunk[0] == 0xFF) {
                    offsets.push_back((uint64_t)(abs_off - 2));
                }
                start = 0;
            }
            for (size_t i = start; i + 2 < nr; ++i) {
                if (chunk[i] == 0xFF && chunk[i+1] == 0xD8 && chunk[i+2] == 0xFF)
                    offsets.push_back((uint64_t)(abs_off + (int64_t)i));
            }
            if (nr >= 2) {
                carry[0] = chunk[nr-2]; carry[1] = chunk[nr-1];
            } else if (nr == 1) {
                carry[0] = 0; carry[1] = chunk[0];
            }
            has_carry = (nr > 0);
            abs_off += (int64_t)nr;
        }
        fclose(mf);

        if (!offsets.empty()) {
            FILE* idxf = fopen(idx.c_str(), "wb");
            if (idxf) {
                uint32_t cnt = (uint32_t)offsets.size();
                fwrite(&cnt, sizeof(cnt), 1, idxf);
                fwrite(offsets.data(), sizeof(uint64_t), cnt, idxf);
                fclose(idxf);
            }
        }
    }

    // Load .idx
    FILE* idxf = fopen(idx.c_str(), "rb");
    if (!idxf) return nullptr;
    uint32_t cnt = 0;
    if (fread(&cnt, sizeof(cnt), 1, idxf) != 1 || cnt == 0) { fclose(idxf); return nullptr; }
    MaskIndex mi;
    mi.offsets.resize(cnt);
    fread(mi.offsets.data(), sizeof(uint64_t), cnt, idxf);
    fclose(idxf);
    mi.start_frame = bg_remove_read_start_frame(mask_dir);

    auto& entry = g_mask_index[mask_dir];
    entry = std::move(mi);
    return &g_mask_index[mask_dir];
}

// ── Internal helpers ──────────────────────────────────────────────────────────

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[1024];
        glGetShaderInfoLog(sh, sizeof(buf), nullptr, buf);
        fprintf(stderr, "[body_fx] compile error: %s\n", buf);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint link_body_prog(const char* frag_body) {
    // Combine preamble + frag_body into one fragment shader source
    const char* srcs[2] = { k_body_preamble, frag_body };
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 2, srcs, nullptr);
    glCompileShader(fs);
    GLint ok = 0;
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[1024];
        glGetShaderInfoLog(fs, sizeof(buf), nullptr, buf);
        fprintf(stderr, "[body_fx] frag compile error: %s\n", buf);
        glDeleteShader(fs);
        return 0;
    }

    GLuint vs = compile_shader(GL_VERTEX_SHADER, k_body_vert);
    if (!vs) { glDeleteShader(fs); return 0; }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[1024];
        glGetProgramInfoLog(prog, sizeof(buf), nullptr, buf);
        fprintf(stderr, "[body_fx] link error: %s\n", buf);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static void ensure_out(int w, int h) {
    if (g_out.w == w && g_out.h == h) return;
    for (int i = 0; i < 2; ++i) {
        if (g_out.fbo[i]) { glDeleteFramebuffers(1, &g_out.fbo[i]); glDeleteTextures(1, &g_out.tex[i]); }
        glGenTextures(1, &g_out.tex[i]);
        glBindTexture(GL_TEXTURE_2D, g_out.tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &g_out.fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, g_out.fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_out.tex[i], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_out.w = w; g_out.h = h;
}

// ── Public API ────────────────────────────────────────────────────────────────

void body_fx_init() {
    // Create empty VAO (required by GL core profile)
    if (!g_vao) glGenVertexArrays(1, &g_vao);

    for (int i = 0; i < k_n_body_fx; ++i) {
        int idx = (int)g_body_fx_infos[i].type;
        if (idx < 0 || idx >= (int)BodyFXType::Count) continue;
        if (g_programs[idx]) continue; // already compiled
        g_programs[idx] = link_body_prog(g_body_fx_infos[i].frag_body);
        if (!g_programs[idx])
            fprintf(stderr, "[body_fx] failed to compile effect %s\n", g_body_fx_infos[i].name);
    }
}

void body_fx_shutdown() {
    for (int i = 0; i < (int)BodyFXType::Count; ++i) {
        if (g_programs[i]) { glDeleteProgram(g_programs[i]); g_programs[i] = 0; }
    }
    if (g_vao) { glDeleteVertexArrays(1, &g_vao); g_vao = 0; }
    for (int i = 0; i < 2; ++i) {
        if (g_out.fbo[i]) { glDeleteFramebuffers(1, &g_out.fbo[i]); g_out.fbo[i] = 0; }
        if (g_out.tex[i]) { glDeleteTextures(1, &g_out.tex[i]); g_out.tex[i] = 0; }
    }
    g_out.w = g_out.h = 0;

    // Free mask cache
    for (auto& [key, val] : g_mask_cache)
        glDeleteTextures(1, &val.first);
    g_mask_cache.clear();
    g_mask_lru.clear();
}

const BodyFXInfo* body_fx_info_list() { return g_body_fx_infos; }
int               body_fx_info_count() { return k_n_body_fx; }

const BodyFXInfo* body_fx_find_info(BodyFXType t) {
    for (int i = 0; i < k_n_body_fx; ++i)
        if (g_body_fx_infos[i].type == t) return &g_body_fx_infos[i];
    return nullptr;
}

// Invalidate the seek-table cache for a mask directory (call after append/rewrite).
void body_fx_invalidate_mask_index(const std::string& mask_dir) {
    g_mask_index.erase(mask_dir);
}

unsigned body_fx_mask_texture(const std::string& mask_dir, int frame_idx) {
    char buf[32];
    snprintf(buf, sizeof(buf), "m%07d", frame_idx);
    std::string key = mask_dir + "/" + buf;

    // Check texture cache
    auto it = g_mask_cache.find(key);
    if (it != g_mask_cache.end()) {
        g_mask_lru.erase(it->second.second);
        g_mask_lru.push_front(key);
        it->second.second = g_mask_lru.begin();
        return it->second.first;
    }

    // Load from bg_masks.mjpeg via seek table
    const MaskIndex* mi = get_mask_index(mask_dir);
    if (!mi) return 0;
    int local_idx = frame_idx - mi->start_frame;
    if (local_idx < 0 || local_idx >= (int)mi->offsets.size()) return 0;

    std::string mjpeg = mask_dir + "/bg_masks.mjpeg";
    FILE* f = fopen(mjpeg.c_str(), "rb");
    if (!f) return 0;

    if (fseek(f, (long)mi->offsets[local_idx], SEEK_SET) != 0) { fclose(f); return 0; }

    // Determine JPEG size: distance to next frame offset (or EOF)
    size_t jpeg_size;
    if (local_idx + 1 < (int)mi->offsets.size())
        jpeg_size = (size_t)(mi->offsets[local_idx + 1] - mi->offsets[local_idx]);
    else {
        fseek(f, 0, SEEK_END);
        jpeg_size = (size_t)(ftell(f) - (long)mi->offsets[local_idx]);
        fseek(f, (long)mi->offsets[local_idx], SEEK_SET);
    }

    std::vector<unsigned char> jpeg_buf(jpeg_size);
    if (fread(jpeg_buf.data(), 1, jpeg_size, f) != jpeg_size) { fclose(f); return 0; }
    fclose(f);

    int w, h, n;
    unsigned char* data = stbi_load_from_memory(jpeg_buf.data(), (int)jpeg_size, &w, &h, &n, 1);
    if (!data) return 0;

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(data);

    // Evict LRU if too many
    if ((int)g_mask_cache.size() >= k_mask_cache_max) {
        const std::string& evict_key = g_mask_lru.back();
        auto evict_it = g_mask_cache.find(evict_key);
        if (evict_it != g_mask_cache.end()) {
            glDeleteTextures(1, &evict_it->second.first);
            g_mask_cache.erase(evict_it);
        }
        g_mask_lru.pop_back();
    }

    g_mask_lru.push_front(key);
    g_mask_cache[key] = { tex, g_mask_lru.begin() };
    return tex;
}

uintptr_t body_fx_apply(BodyFXType type,
                         uintptr_t src_tex, unsigned mask_tex,
                         int w, int h,
                         const float params[4],
                         float amount, float t_sec,
                         const float bg_box[4], float bg_softness) {
    int idx = (int)type;
    if (idx < 0 || idx >= (int)BodyFXType::Count) return src_tex;
    GLuint prog = g_programs[idx];
    if (!prog || !src_tex || !mask_tex) {
        static int s_dbg_ap = 0;
        if (s_dbg_ap++ < 12)
            fprintf(stderr, "[body_fx] apply early-return: type=%d prog=%u src=%lu mask=%u\n",
                    idx, prog, (unsigned long)src_tex, mask_tex);
        return src_tex;
    }

    ensure_out(w, h);
    if (!g_out.fbo[0]) return src_tex;
    // Ping-pong: render into whichever buffer is NOT the source, so a chained effect
    // (src == a previous g_out result) never samples the texture it renders into.
    int oi = (src_tex == (uintptr_t)g_out.tex[0]) ? 1 : 0;

    // Save GL state
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    GLint prev_vao = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    GLint prev_prog = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    GLint prev_vp[4];
    glGetIntegerv(GL_VIEWPORT, prev_vp);
    // This pass must OVERWRITE the output FBO. The preview leaves GL_BLEND enabled
    // (ImGui backend), so without disabling it the fullscreen triangle src-overs onto the
    // buffer's stale content from previous frames — the cutout alpha then accumulates
    // toward opaque across a live scrub (the bug; export has blend off, hence stayed clean).
    // scene_add_layer disables blend for exactly this reason.
    GLboolean prev_blend = glIsEnabled(GL_BLEND);
    glDisable(GL_BLEND);

    glBindFramebuffer(GL_FRAMEBUFFER, g_out.fbo[oi]);
    glViewport(0, 0, w, h);
    glUseProgram(prog);
    glBindVertexArray(g_vao);

    // Bind source texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)src_tex);
    glUniform1i(glGetUniformLocation(prog, "u_src"), 0);

    // Bind mask texture
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mask_tex);
    glUniform1i(glGetUniformLocation(prog, "u_mask"), 1);

    // Set uniforms
    glUniform1f(glGetUniformLocation(prog, "t"), t_sec);
    glUniform1f(glGetUniformLocation(prog, "u_amount"), amount);
    glUniform1f(glGetUniformLocation(prog, "p0"), params ? params[0] : 0.f);
    glUniform1f(glGetUniformLocation(prog, "p1"), params ? params[1] : 0.f);
    glUniform1f(glGetUniformLocation(prog, "p2"), params ? params[2] : 0.f);
    glUniform1f(glGetUniformLocation(prog, "p3"), params ? params[3] : 0.f);
    static const float full_box[4] = {0.f, 1.f, 0.f, 1.f};
    const float* bx = bg_box ? bg_box : full_box;
    glUniform4f(glGetUniformLocation(prog, "u_bg_box"), bx[0], bx[1], bx[2], bx[3]);
    glUniform1f(glGetUniformLocation(prog, "u_bg_softness"), bg_softness);

    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Restore GL state
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(prev_vao);
    glUseProgram(prev_prog);
    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    if (prev_blend) glEnable(GL_BLEND);

    return (uintptr_t)g_out.tex[oi];
}

// ── Body-FX card preview thumbnails ───────────────────────────────────────────
// Render each effect onto the embedded preview portrait + its U2Net body mask so
// the picker cards show a live thumbnail (and the hovered card animates over t).
// body_fx_apply reuses one shared g_out fbo, so each effect's result is copied to
// its own cache texture — otherwise every card would show the last one rendered.
static unsigned g_bfx_prev_src = 0, g_bfx_prev_mask = 0;
static std::unordered_map<int, unsigned> g_bfx_prev_cache;

static void ensure_bfx_prev_source() {
    if (g_bfx_prev_src) return;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    auto mk = [](unsigned& t, GLenum ifmt, GLenum fmt, const void* px) {
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, ifmt, body_fx_preview_w, body_fx_preview_h, 0,
                     fmt, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    mk(g_bfx_prev_src,  GL_RGB, GL_RGB, body_fx_preview_rgb);
    mk(g_bfx_prev_mask, GL_R8,  GL_RED, body_fx_preview_mask);
    glBindTexture(GL_TEXTURE_2D, 0);
}

uintptr_t body_fx_preview_texture(BodyFXType type, float t) {
    ensure_bfx_prev_source();
    const int W = body_fx_preview_w, H = body_fx_preview_h;
    const BodyFXInfo* info = body_fx_find_info(type);
    float params[4] = {0.f, 0.f, 0.f, 0.f};
    if (info)
        for (int i = 0; i < 4; ++i)
            params[i] = i < info->n_params ? info->params[i].default_val : 0.5f;

    body_fx_apply(type, (uintptr_t)g_bfx_prev_src, g_bfx_prev_mask, W, H, params, 1.f, t);

    unsigned& cache = g_bfx_prev_cache[(int)type];
    if (!cache) {
        glGenTextures(1, &cache);
        glBindTexture(GL_TEXTURE_2D, cache);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_out.fbo[0]);  // preview is a single effect → buffer 0
    glBindTexture(GL_TEXTURE_2D, cache);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, W, H);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    return (uintptr_t)cache;
}

void body_fx_evict_mask_cache(const std::string& mask_dir) {
    std::string prefix = mask_dir + "/";
    std::vector<std::string> to_evict;
    for (auto& [key, val] : g_mask_cache) {
        if (key.substr(0, prefix.size()) == prefix)
            to_evict.push_back(key);
    }
    for (auto& key : to_evict) {
        auto it = g_mask_cache.find(key);
        if (it != g_mask_cache.end()) {
            glDeleteTextures(1, &it->second.first);
            g_mask_lru.erase(it->second.second);
            g_mask_cache.erase(it);
        }
    }
}
