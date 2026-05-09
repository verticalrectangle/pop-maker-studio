#!/usr/bin/env python3
"""Batch 4: 20 more unique effects (effects 81-100)."""
import json, os, textwrap

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REG  = os.path.join(ROOT, "effects", "registry.json")
SHADER_DIR = os.path.join(ROOT, "shaders")

with open(REG) as f:
    reg = json.load(f)

existing_ids = {e["id"] for e in reg["effects"]}

NEW_EFFECTS = [

# ── 1. pixel_mosaic ──────────────────────────────────────────────────────────
{
"id": "pixel_mosaic",
"label": "Pixel Mosaic",
"params": [
  {"name":"block_size","label":"Block Size","type":"float","min":2,"max":64,"default":16,"step":1},
  {"name":"color_steps","label":"Color Steps","type":"float","min":2,"max":16,"default":6,"step":1},
  {"name":"mix_orig","label":"Mix Original","type":"float","min":0,"max":1,"default":0.0,"step":0.01},
],
"shader": "pixel_mosaic.glsl",
"glsl": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_block_size;
uniform float u_color_steps;
uniform float u_mix_orig;
void main() {
    vec2 px = vec2(u_tex_w, u_tex_h);
    vec2 block = floor(v_uv * px / u_block_size) * u_block_size / px;
    vec2 center = block + (u_block_size * 0.5) / px;
    vec4 orig = texture(u_tex, v_uv);
    vec4 c = texture(u_tex, clamp(center, 0.0, 1.0));
    // Quantize to color_steps levels
    c.rgb = floor(c.rgb * u_color_steps + 0.5) / u_color_steps;
    frag = vec4(mix(c.rgb, orig.rgb, u_mix_orig), orig.a);
}
"""
},

# ── 2. thermal_map ───────────────────────────────────────────────────────────
{
"id": "thermal_map",
"label": "Thermal Camera",
"params": [
  {"name":"cold_hue","label":"Cold Hue","type":"float","min":0,"max":1,"default":0.65,"step":0.01},
  {"name":"hot_hue","label":"Hot Hue","type":"float","min":0,"max":1,"default":0.08,"step":0.01},
  {"name":"contrast","label":"Contrast","type":"float","min":0.5,"max":3.0,"default":1.6,"step":0.05},
  {"name":"scanlines","label":"Scanlines","type":"float","min":0,"max":1,"default":0.15,"step":0.01},
],
"shader": "thermal_map.glsl",
"glsl": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_h;
uniform float u_cold_hue;
uniform float u_hot_hue;
uniform float u_contrast;
uniform float u_scanlines;
vec3 hue2rgb(float h) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    return clamp(abs(fract(h + K.xyz)*6.0 - K.www) - K.xxx, 0.0, 1.0);
}
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Remap with contrast
    float heat = clamp((lum - 0.5) * u_contrast + 0.5, 0.0, 1.0);
    // Map heat level to 5-stop thermal palette
    vec3 thermal;
    if (heat < 0.25)
        thermal = mix(hue2rgb(u_cold_hue) * 0.3, hue2rgb(u_cold_hue), heat*4.0);
    else if (heat < 0.5)
        thermal = mix(hue2rgb(u_cold_hue), hue2rgb(mix(u_cold_hue, 0.33, 1.0)), (heat-0.25)*4.0);
    else if (heat < 0.75)
        thermal = mix(hue2rgb(0.33), hue2rgb(u_hot_hue + 0.05), (heat-0.5)*4.0);
    else
        thermal = mix(hue2rgb(u_hot_hue), vec3(1.0, 1.0, 0.9), (heat-0.75)*4.0);
    // Faint scan lines
    float scan = 1.0 - u_scanlines * 0.5 * (0.5 + 0.5*sin(v_uv.y * u_tex_h * 3.14159));
    frag = vec4(clamp(thermal * scan, 0.0, 1.0), col.a);
}
"""
},

# ── 3. tilt_shift ────────────────────────────────────────────────────────────
{
"id": "tilt_shift",
"label": "Tilt-Shift Miniature",
"params": [
  {"name":"focus_y","label":"Focus Center","type":"float","min":0,"max":1,"default":0.5,"step":0.01},
  {"name":"focus_band","label":"Focus Band","type":"float","min":0.02,"max":0.5,"default":0.12,"step":0.005},
  {"name":"blur_radius","label":"Blur Radius","type":"float","min":1,"max":12,"default":6,"step":0.5},
  {"name":"saturation","label":"Saturation","type":"float","min":1.0,"max":3.0,"default":1.8,"step":0.05},
],
"shader": "tilt_shift.glsl",
"glsl": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_focus_y;
uniform float u_focus_band;
uniform float u_blur_radius;
uniform float u_saturation;
void main() {
    float dist = abs(v_uv.y - u_focus_y);
    float blur_t = smoothstep(u_focus_band, u_focus_band * 3.5, dist);
    float r = u_blur_radius * blur_t;
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    vec4 sum = vec4(0.0);
    float w = 0.0;
    int R = int(r);
    for (int dy = -R; dy <= R; dy++) {
        for (int dx = -R; dx <= R; dx++) {
            float wt = exp(-float(dx*dx+dy*dy)/(r*r+0.001)*0.5);
            sum += texture(u_tex, clamp(v_uv + vec2(dx,dy)*px, 0.0, 1.0)) * wt;
            w += wt;
        }
    }
    vec4 blurred = sum / w;
    vec4 orig = texture(u_tex, v_uv);
    vec4 col = mix(orig, blurred, blur_t);
    // Saturation boost (miniature look)
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    col.rgb = mix(vec3(lum), col.rgb, u_saturation);
    frag = vec4(clamp(col.rgb, 0.0, 1.0), col.a);
}
"""
},

# ── 4. night_vision ──────────────────────────────────────────────────────────
{
"id": "night_vision",
"label": "Night Vision",
"params": [
  {"name":"amplify","label":"Amplify","type":"float","min":1,"max":6,"default":3.5,"step":0.1},
  {"name":"noise_amt","label":"Noise","type":"float","min":0,"max":1,"default":0.25,"step":0.01},
  {"name":"scanline_str","label":"Scanlines","type":"float","min":0,"max":1,"default":0.3,"step":0.01},
  {"name":"vignette","label":"Vignette","type":"float","min":0,"max":2,"default":1.2,"step":0.05},
  {"name":"u_time","label":"","type":"float","min":0,"max":1000,"default":0.0,"step":0.016,"hidden":True},
],
"shader": "night_vision.glsl",
"glsl": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_h;
uniform float u_amplify;
uniform float u_noise_amt;
uniform float u_scanline_str;
uniform float u_vignette;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Amplify and green-channel map
    float amp = clamp(lum * u_amplify, 0.0, 1.0);
    // Phosphor grain
    float grain = (hash(floor(v_uv * vec2(320, 240)) + vec2(u_time * 17.3, u_time * 31.1)) - 0.5) * u_noise_amt;
    // Horizontal scan lines
    float scan = 1.0 - u_scanline_str * 0.5 * (0.5 + 0.5 * sin(v_uv.y * u_tex_h * 3.14159));
    // Vignette
    vec2 uvc = v_uv - 0.5;
    float vig = 1.0 - dot(uvc, uvc) * u_vignette * 2.0;
    float val = clamp((amp + grain) * scan * vig, 0.0, 1.0);
    frag = vec4(val * 0.15, val, val * 0.08, col.a);
}
"""
},

# ── 5. raindrop_refract ───────────────────────────────────────────────────────
{
"id": "raindrop_refract",
"label": "Raindrops",
"params": [
  {"name":"density","label":"Density","type":"float","min":1,"max":20,"default":8,"step":0.5},
  {"name":"size","label":"Size","type":"float","min":0.1,"max":1.0,"default":0.45,"step":0.01},
  {"name":"refract_str","label":"Refraction","type":"float","min":0,"max":2,"default":1.0,"step":0.05},
  {"name":"u_time","label":"","type":"float","min":0,"max":1000,"default":0.0,"step":0.016,"hidden":True},
],
"shader": "raindrop_refract.glsl",
"glsl": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_density;
uniform float u_size;
uniform float u_refract_str;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec2 uv = v_uv;
    vec2 grid = floor(uv * u_density);
    vec2 cell = fract(uv * u_density) - 0.5;
    // Each cell gets a random drop center and phase offset
    vec2 drop_center = vec2(hash(grid), hash(grid + vec2(7.3,3.1))) - 0.5;
    float drop_phase = hash(grid + vec2(13.7, 5.9));
    float anim = fract(u_time * 0.4 + drop_phase);
    float radius = u_size * 0.5 * smoothstep(0.0, 0.2, anim) * smoothstep(1.0, 0.7, anim);
    float dist = length(cell - drop_center * 0.3);
    if (dist < radius) {
        // Sphere lens refraction
        vec2 norm = (cell - drop_center * 0.3) / (radius + 0.001);
        float z = sqrt(max(0.0, 1.0 - dot(norm, norm)));
        vec2 refr = norm * (1.0 - z) * u_refract_str * 0.08;
        uv = v_uv - refr / u_density;
    }
    frag = texture(u_tex, clamp(uv, 0.0, 1.0));
}
"""
},

# ── 6. risograph ─────────────────────────────────────────────────────────────
{
"id": "risograph",
"label": "Risograph Print",
"params": [
  {"name":"hue1","label":"Ink 1 Hue","type":"float","min":0,"max":1,"default":0.58,"step":0.01},
  {"name":"hue2","label":"Ink 2 Hue","type":"float","min":0,"max":1,"default":0.02,"step":0.01},
  {"name":"dot_size","label":"Dot Size","type":"float","min":1,"max":8,"default":3,"step":0.5},
  {"name":"misreg","label":"Mis-register","type":"float","min":0,"max":0.02,"default":0.004,"step":0.001},
  {"name":"paper","label":"Paper Tone","type":"float","min":0,"max":1,"default":0.9,"step":0.01},
],
"shader": "risograph.glsl",
"glsl": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_hue1;
uniform float u_hue2;
uniform float u_dot_size;
uniform float u_misreg;
uniform float u_paper;
vec3 hue2rgb(float h) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    return clamp(abs(fract(h + K.xyz)*6.0 - K.www) - K.xxx, 0.0, 1.0);
}
float halftone(vec2 uv, float scale, float angle, float density) {
    float s = sin(angle), c_a = cos(angle);
    vec2 rot = vec2(uv.x*c_a - uv.y*s, uv.x*s + uv.y*c_a);
    vec2 cell = fract(rot * scale) - 0.5;
    return smoothstep(density * 0.5 + 0.05, density * 0.5 - 0.05, length(cell));
}
void main() {
    vec4 col1 = texture(u_tex, clamp(v_uv + vec2(u_misreg, u_misreg*0.5), 0.0, 1.0));
    vec4 col2 = texture(u_tex, clamp(v_uv - vec2(u_misreg*0.7, u_misreg), 0.0, 1.0));
    float lum1 = dot(col1.rgb, vec3(0.299, 0.587, 0.114));
    float lum2 = dot(col2.rgb, vec3(0.299, 0.587, 0.114));
    float scale = min(u_tex_w, u_tex_h) / u_dot_size * 0.015;
    float dot1 = halftone(v_uv, scale, 0.785, 1.0 - lum1);
    float dot2 = halftone(v_uv, scale, 0.35, 1.0 - lum2);
    vec3 paper_col = vec3(u_paper, u_paper * 0.96, u_paper * 0.88);
    vec3 ink1 = hue2rgb(u_hue1);
    vec3 ink2 = hue2rgb(u_hue2);
    vec3 result = paper_col;
    result = mix(result, ink1 * 0.85, dot1 * 0.8);
    result = mix(result, ink2 * 0.8, dot2 * 0.7);
    // Multiply where both inks overlap
    float overlap = dot1 * dot2;
    result = mix(result, ink1 * ink2, overlap * 0.6);
    frag = vec4(clamp(result, 0.0, 1.0), col1.a);
}
"""
},

# ── 7. vintage_negative ───────────────────────────────────────────────────────
{
"id": "vintage_negative",
"label": "Vintage Negative",
"params": [
  {"name":"orange_mask","label":"Orange Mask","type":"float","min":0,"max":1,"default":0.55,"step":0.01},
  {"name":"contrast","label":"Contrast","type":"float","min":0.5,"max":2.5,"default":1.3,"step":0.05},
  {"name":"grain","label":"Grain","type":"float","min":0,"max":1,"default":0.12,"step":0.01},
  {"name":"u_time","label":"","type":"float","min":0,"max":1000,"default":0.0,"step":0.016,"hidden":True},
],
"shader": "vintage_negative.glsl",
"glsl": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_orange_mask;
uniform float u_contrast;
uniform float u_grain;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec4 col = texture(u_tex, v_uv);
    // Invert
    vec3 neg = 1.0 - col.rgb;
    // Apply orange mask (simulates orange film base)
    neg.r = mix(neg.r, neg.r * 0.85 + 0.15, u_orange_mask);
    neg.g = mix(neg.g, neg.g * 0.7 + 0.08, u_orange_mask);
    neg.b = mix(neg.b, neg.b * 0.3 + 0.02, u_orange_mask * 0.8);
    // Contrast
    neg = clamp((neg - 0.5) * u_contrast + 0.5, 0.0, 1.0);
    // Film grain
    vec2 npx = floor(v_uv * vec2(u_tex_w, u_tex_h));
    float g = (hash(npx + vec2(u_time * 23.1, u_time * 17.7)) - 0.5) * u_grain * 0.3;
    frag = vec4(clamp(neg + g, 0.0, 1.0), col.a);
}
"""
},

# ── 8. pencil_sketch ─────────────────────────────────────────────────────────
{
"id": "pencil_sketch",
"label": "Pencil Sketch",
"params": [
  {"name":"line_str","label":"Line Strength","type":"float","min":0.5,"max":5.0,"default":2.5,"step":0.1},
  {"name":"paper_tone","label":"Paper Tone","type":"float","min":0.7,"max":1.0,"default":0.95,"step":0.005},
  {"name":"hatching","label":"Hatching","type":"float","min":0,"max":1,"default":0.4,"step":0.01},
  {"name":"mix_orig","label":"Mix Color","type":"float","min":0,"max":1,"default":0.0,"step":0.01},
],
"shader": "pencil_sketch.glsl",
"glsl": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_line_str;
uniform float u_paper_tone;
uniform float u_hatching;
uniform float u_mix_orig;
void main() {
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    // Sobel edge detection
    float k[9];
    int idx = 0;
    for (int dy=-1;dy<=1;dy++) for (int dx=-1;dx<=1;dx++) {
        vec3 c = texture(u_tex, v_uv + vec2(dx,dy)*px).rgb;
        k[idx++] = dot(c, vec3(0.299,0.587,0.114));
    }
    float gx = -k[0]+k[2]-2.0*k[3]+2.0*k[5]-k[6]+k[8];
    float gy = -k[0]-2.0*k[1]-k[2]+k[6]+2.0*k[7]+k[8];
    float edge = clamp(sqrt(gx*gx+gy*gy) * u_line_str, 0.0, 1.0);
    // Cross-hatching pattern on dark areas
    float lum0 = k[4];
    float hatch = step(0.5 - lum0 * 0.5, fract((v_uv.x + v_uv.y) * u_tex_w * 0.04));
    hatch *= step(0.5 - lum0 * 0.5, fract((v_uv.x - v_uv.y) * u_tex_w * 0.04));
    float sketch = max(edge, (1.0 - hatch) * (1.0 - lum0) * u_hatching * 0.8);
    vec3 paper = vec3(u_paper_tone, u_paper_tone * 0.97, u_paper_tone * 0.92);
    vec3 result = mix(paper, vec3(0.1, 0.08, 0.05), sketch);
    vec4 orig = texture(u_tex, v_uv);
    frag = vec4(clamp(mix(result, orig.rgb, u_mix_orig), 0.0, 1.0), orig.a);
}
"""
},

# ── 9. crt_barrel ────────────────────────────────────────────────────────────
{
"id": "crt_barrel",
"label": "CRT Barrel",
"params": [
  {"name":"distort","label":"Distortion","type":"float","min":0,"max":1,"default":0.35,"step":0.01},
  {"name":"corner_dark","label":"Corner Dark","type":"float","min":0,"max":3,"default":1.8,"step":0.05},
  {"name":"rgb_shift","label":"RGB Shift","type":"float","min":0,"max":0.02,"default":0.004,"step":0.0005},
  {"name":"scanline","label":"Scanlines","type":"float","min":0,"max":1,"default":0.35,"step":0.01},
],
"shader": "crt_barrel.glsl",
"glsl": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_h;
uniform float u_distort;
uniform float u_corner_dark;
uniform float u_rgb_shift;
uniform float u_scanline;
vec2 barrel(vec2 uv, float k) {
    vec2 cc = uv - 0.5;
    float r2 = dot(cc, cc);
    return uv + cc * (r2 * k);
}
void main() {
    vec2 uv = barrel(v_uv, u_distort * 0.6);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        frag = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec2 uvR = barrel(v_uv + vec2( u_rgb_shift, 0), u_distort*0.6);
    vec2 uvB = barrel(v_uv + vec2(-u_rgb_shift, 0), u_distort*0.6);
    float r = texture(u_tex, clamp(uvR,0.0,1.0)).r;
    float g = texture(u_tex, clamp(uv, 0.0,1.0)).g;
    float b = texture(u_tex, clamp(uvB,0.0,1.0)).b;
    vec3 col = vec3(r, g, b);
    // Scanlines
    float scan = 1.0 - u_scanline * 0.5 * (0.5 + 0.5*sin(uv.y * u_tex_h * 3.14159));
    col *= scan;
    // Vignette / corner darkening
    vec2 cc = uv - 0.5;
    float vig = 1.0 - dot(cc*1.6, cc*1.6) * u_corner_dark;
    frag = vec4(clamp(col * vig, 0.0, 1.0), 1.0);
}
"""
},

# ── 10. rgb_split_wave ────────────────────────────────────────────────────────
{
"id": "rgb_split_wave",
"label": "RGB Wave Split",
"params": [
  {"name":"amplitude","label":"Amplitude","type":"float","min":0,"max":0.05,"default":0.018,"step":0.001},
  {"name":"frequency","label":"Frequency","type":"float","min":1,"max":20,"default":6.0,"step":0.5},
  {"name":"speed","label":"Speed","type":"float","min":0,"max":10,"default":2.5,"step":0.1},
  {"name":"u_time","label":"","type":"float","min":0,"max":1000,"default":0.0,"step":0.016,"hidden":True},
],
"shader": "rgb_split_wave.glsl",
"glsl": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_amplitude;
uniform float u_frequency;
uniform float u_speed;
uniform float u_time;
void main() {
    float t = u_time * u_speed;
    float pi2 = 6.28318;
    // Each channel offset by a wave with different phase
    vec2 offsetR = vec2(sin(v_uv.y * u_frequency * pi2 + t)       * u_amplitude, 0.0);
    vec2 offsetG = vec2(sin(v_uv.y * u_frequency * pi2 + t + 2.09) * u_amplitude * 0.6, 0.0);
    vec2 offsetB = vec2(sin(v_uv.y * u_frequency * pi2 + t + 4.19) * u_amplitude * 1.3, 0.0);
    float r = texture(u_tex, clamp(v_uv + offsetR, 0.0, 1.0)).r;
    float g = texture(u_tex, clamp(v_uv + offsetG, 0.0, 1.0)).g;
    float b = texture(u_tex, clamp(v_uv + offsetB, 0.0, 1.0)).b;
    vec4 orig = texture(u_tex, v_uv);
    frag = vec4(r, g, b, orig.a);
}
"""
},

# ── 11. golden_hour ───────────────────────────────────────────────────────────
{
"id": "golden_hour",
"label": "Golden Hour",
"params": [
  {"name":"warmth","label":"Warmth","type":"float","min":0,"max":1,"default":0.7,"step":0.01},
  {"name":"glow_str","label":"Glow","type":"float","min":0,"max":1,"default":0.55,"step":0.01},
  {"name":"shadow_lift","label":"Shadow Lift","type":"float","min":0,"max":0.3,"default":0.08,"step":0.005},
  {"name":"vignette","label":"Vignette","type":"float","min":0,"max":2,"default":0.6,"step":0.05},
],
"shader": "golden_hour.glsl",
"glsl": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_warmth;
uniform float u_glow_str;
uniform float u_shadow_lift;
uniform float u_vignette;
void main() {
    vec4 col = texture(u_tex, v_uv);
    // Warm highlight push (orange-gold)
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    float hi = smoothstep(0.45, 0.9, lum);
    col.rgb += hi * u_warmth * vec3(0.25, 0.12, -0.08);
    // Cool shadow lift (slight purple-blue in shadows)
    float sha = smoothstep(0.4, 0.0, lum);
    col.rgb += sha * u_shadow_lift * vec3(0.12, 0.1, 0.2);
    // Diffuse glow from bright areas (screen blend)
    float bloom = clamp((lum - 0.55) * 2.5, 0.0, 1.0);
    vec3 glow = col.rgb * bloom * u_glow_str * vec3(1.0, 0.85, 0.5);
    col.rgb = 1.0 - (1.0 - col.rgb) * (1.0 - glow);
    // Vignette
    vec2 uvc = v_uv - 0.5;
    float vig = 1.0 - dot(uvc, uvc) * u_vignette * 2.5;
    frag = vec4(clamp(col.rgb * vig, 0.0, 1.0), col.a);
}
"""
},

# ── 12. neon_sign ─────────────────────────────────────────────────────────────
{
"id": "neon_sign",
"label": "Neon Sign",
"params": [
  {"name":"edge_str","label":"Edge Strength","type":"float","min":0.5,"max":5.0,"default":2.5,"step":0.1},
  {"name":"glow_radius","label":"Glow Radius","type":"float","min":1,"max":16,"default":8,"step":0.5},
  {"name":"hue_shift","label":"Hue Shift","type":"float","min":0,"max":1,"default":0.0,"step":0.01},
  {"name":"bg_darken","label":"BG Darken","type":"float","min":0,"max":1,"default":0.75,"step":0.01},
],
"shader": "neon_sign.glsl",
"glsl": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_edge_str;
uniform float u_glow_radius;
uniform float u_hue_shift;
uniform float u_bg_darken;
void main() {
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    // Sobel
    float k[9]; int idx = 0;
    for (int dy=-1;dy<=1;dy++) for (int dx=-1;dx<=1;dx++)
        k[idx++] = dot(texture(u_tex, v_uv + vec2(dx,dy)*px).rgb, vec3(0.299,0.587,0.114));
    float gx = -k[0]+k[2]-2.0*k[3]+2.0*k[5]-k[6]+k[8];
    float gy = -k[0]-2.0*k[1]-k[2]+k[6]+2.0*k[7]+k[8];
    float edge = clamp(sqrt(gx*gx+gy*gy)*u_edge_str, 0.0, 1.0);
    // Glow by gaussian blur of edge map
    vec3 glow = vec3(0.0);
    float wsum = 0.0;
    int R = int(u_glow_radius);
    for (int dy=-R;dy<=R;dy++) {
        for (int dx=-R;dx<=R;dx++) {
            float w = exp(-float(dx*dx+dy*dy)/(u_glow_radius*u_glow_radius+0.001));
            vec2 uv2 = clamp(v_uv + vec2(dx,dy)*px, 0.0, 1.0);
            float k2[9]; int i2=0;
            for (int dy2=-1;dy2<=1;dy2++) for (int dx2=-1;dx2<=1;dx2++)
                k2[i2++] = dot(texture(u_tex, uv2+vec2(dx2,dy2)*px).rgb, vec3(0.299,0.587,0.114));
            float gx2=-k2[0]+k2[2]-2.0*k2[3]+2.0*k2[5]-k2[6]+k2[8];
            float gy2=-k2[0]-2.0*k2[1]-k2[2]+k2[6]+2.0*k2[7]+k2[8];
            float e2 = clamp(sqrt(gx2*gx2+gy2*gy2)*u_edge_str*0.5, 0.0, 1.0);
            // Hue cycling based on angle
            float angle = atan(gy2, gx2) / 6.28318 + 0.5 + u_hue_shift;
            vec4 K = vec4(1.0,2.0/3.0,1.0/3.0,3.0);
            vec3 neon = clamp(abs(fract(angle+K.xyz)*6.0-K.www)-K.xxx, 0.0, 1.0);
            glow += neon * e2 * w;
            wsum += w;
        }
    }
    glow /= (wsum + 0.001);
    vec4 orig = texture(u_tex, v_uv);
    float lum = dot(orig.rgb, vec3(0.299,0.587,0.114));
    vec3 bg = orig.rgb * (1.0 - u_bg_darken * 0.9);
    vec3 result = 1.0 - (1.0-bg)*(1.0-glow*2.5);
    result = mix(result, vec3(1.0), edge * 0.7);
    frag = vec4(clamp(result, 0.0, 1.0), orig.a);
}
"""
},

# ── 13. mirror_tunnel ────────────────────────────────────────────────────────
{
"id": "mirror_tunnel",
"label": "Mirror Tunnel",
"params": [
  {"name":"depth","label":"Depth","type":"float","min":1,"max":12,"default":5,"step":0.5},
  {"name":"rotation","label":"Rotation","type":"float","min":0,"max":1,"default":0.12,"step":0.01},
  {"name":"zoom","label":"Zoom","type":"float","min":0.3,"max":0.95,"default":0.65,"step":0.01},
  {"name":"u_time","label":"","type":"float","min":0,"max":1000,"default":0.0,"step":0.016,"hidden":True},
],
"shader": "mirror_tunnel.glsl",
"glsl": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_depth;
uniform float u_rotation;
uniform float u_zoom;
uniform float u_time;
void main() {
    vec2 uv = v_uv - 0.5;
    int maxSteps = int(u_depth);
    float angle_acc = u_time * u_rotation * 0.5;
    for (int i = 0; i < 12; i++) {
        if (i >= maxSteps) break;
        float s = sin(angle_acc), c_a = cos(angle_acc);
        uv = vec2(uv.x*c_a - uv.y*s, uv.x*s + uv.y*c_a);
        uv /= u_zoom;
        uv = abs(fract(uv * 0.5 + 0.5) * 2.0 - 1.0) - 0.5;
        angle_acc += 0.2 + u_rotation * 0.3;
    }
    uv += 0.5;
    frag = texture(u_tex, clamp(uv, 0.0, 1.0));
}
"""
},

# ── 14. liquid_chrome ────────────────────────────────────────────────────────
{
"id": "liquid_chrome",
"label": "Liquid Chrome",
"params": [
  {"name":"flow","label":"Flow","type":"float","min":0,"max":1,"default":0.5,"step":0.01},
  {"name":"metallic","label":"Metallic","type":"float","min":0,"max":1,"default":0.8,"step":0.01},
  {"name":"tint_r","label":"Tint R","type":"float","min":0,"max":1,"default":0.85,"step":0.01},
  {"name":"tint_g","label":"Tint G","type":"float","min":0,"max":1,"default":0.9,"step":0.01},
  {"name":"tint_b","label":"Tint B","type":"float","min":0,"max":1,"default":1.0,"step":0.01},
  {"name":"u_time","label":"","type":"float","min":0,"max":1000,"default":0.0,"step":0.016,"hidden":True},
],
"shader": "liquid_chrome.glsl",
"glsl": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_flow;
uniform float u_metallic;
uniform float u_tint_r;
uniform float u_tint_g;
uniform float u_tint_b;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5); }
float noise2(vec2 p) {
    vec2 i=floor(p); vec2 f=fract(p); f=f*f*(3.0-2.0*f);
    return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);
}
void main() {
    float t = u_time * 0.3;
    // Animated flow distortion
    vec2 d;
    d.x = noise2(v_uv * 3.0 + vec2(t, t * 0.7)) - 0.5;
    d.y = noise2(v_uv * 3.0 + vec2(t * 0.8 + 1.7, t * 1.1 + 3.3)) - 0.5;
    vec2 uv = v_uv + d * u_flow * 0.04;
    vec4 col = texture(u_tex, clamp(uv, 0.0, 1.0));
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Metallic specular: sharp highlight at mid-high luminance
    float spec = smoothstep(0.5, 0.85, lum) * smoothstep(1.0, 0.75, lum);
    // Reflection bands
    float band = sin(lum * 8.0 + t * 2.0) * 0.5 + 0.5;
    vec3 tint = vec3(u_tint_r, u_tint_g, u_tint_b);
    vec3 chrome = mix(vec3(lum * 0.15), vec3(lum), band) * tint;
    chrome += spec * tint * 1.2;
    vec3 result = mix(col.rgb, chrome, u_metallic);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
"""
},

# ── 15. zone_system_bw ───────────────────────────────────────────────────────
{
"id": "zone_system_bw",
"label": "Zone System B&W",
"params": [
  {"name":"zones","label":"Zones","type":"float","min":3,"max":10,"default":6,"step":1},
  {"name":"contrast","label":"Contrast","type":"float","min":0.5,"max":3.0,"default":1.8,"step":0.05},
  {"name":"grain","label":"Grain","type":"float","min":0,"max":1,"default":0.08,"step":0.01},
  {"name":"paper_white","label":"Paper White","type":"float","min":0.85,"max":1.0,"default":0.96,"step":0.005},
  {"name":"u_time","label":"","type":"float","min":0,"max":1000,"default":0.0,"step":0.016,"hidden":True},
],
"shader": "zone_system_bw.glsl",
"glsl": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_zones;
uniform float u_contrast;
uniform float u_grain;
uniform float u_paper_white;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // High-contrast S-curve
    lum = clamp((lum - 0.5) * u_contrast + 0.5, 0.0, 1.0);
    // Quantize to N zones
    lum = floor(lum * u_zones) / (u_zones - 1.0);
    // Film grain (larger grain in shadows)
    vec2 npx = floor(v_uv * vec2(u_tex_w, u_tex_h));
    float shadow_grain = 1.5 - lum;
    float g = (hash(npx + vec2(u_time*17.3, u_time*31.1)) - 0.5) * u_grain * 0.35 * shadow_grain;
    lum = clamp(lum + g, 0.0, 1.0);
    // Paper: white point + very slight warm tint
    vec3 result = mix(vec3(0.04, 0.035, 0.03), vec3(u_paper_white, u_paper_white*0.99, u_paper_white*0.96), lum);
    frag = vec4(result, col.a);
}
"""
},

# ── 16. glitter_dust ─────────────────────────────────────────────────────────
{
"id": "glitter_dust",
"label": "Glitter Dust",
"params": [
  {"name":"density","label":"Density","type":"float","min":100,"max":2000,"default":800,"step":50},
  {"name":"size","label":"Size","type":"float","min":0.5,"max":4.0,"default":1.5,"step":0.1},
  {"name":"sparkle","label":"Sparkle","type":"float","min":0.5,"max":3.0,"default":1.8,"step":0.05},
  {"name":"color_var","label":"Color Var","type":"float","min":0,"max":1,"default":1.0,"step":0.01},
  {"name":"u_time","label":"","type":"float","min":0,"max":1000,"default":0.0,"step":0.016,"hidden":True},
],
"shader": "glitter_dust.glsl",
"glsl": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_density;
uniform float u_size;
uniform float u_sparkle;
uniform float u_color_var;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453); }
vec3 hue2rgb(float h) {
    vec4 K = vec4(1.0,2.0/3.0,1.0/3.0,3.0);
    return clamp(abs(fract(h+K.xyz)*6.0-K.www)-K.xxx, 0.0, 1.0);
}
void main() {
    vec4 col = texture(u_tex, v_uv);
    vec3 glitter = vec3(0.0);
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h) * u_size;
    // Scatter random glitter points
    for (int i = 0; i < 4; i++) {
        float fi = float(i) * 317.3 + 71.1;
        // Jittered grid of sparkles
        vec2 grid = floor(v_uv * u_density * 0.01 + fi);
        for (int gx = -1; gx <= 1; gx++) {
            for (int gy = -1; gy <= 1; gy++) {
                vec2 g = grid + vec2(gx, gy);
                vec2 center = (g + vec2(hash(g+fi), hash(g+fi+vec2(3.1,7.7)))) / (u_density * 0.01);
                float dist = length((v_uv - center) / (px + 0.001));
                float t = u_time * 3.0 + hash(g+fi) * 6.28;
                float anim = 0.5 + 0.5 * sin(t);
                float spark = smoothstep(1.5, 0.0, dist) * anim * u_sparkle;
                float hue = hash(g + vec2(fi, fi*1.3));
                glitter += hue2rgb(mix(0.0, hue, u_color_var)) * spark;
            }
        }
    }
    vec3 result = 1.0 - (1.0 - col.rgb) * (1.0 - clamp(glitter, 0.0, 1.0));
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
"""
},

# ── 17. contour_map ───────────────────────────────────────────────────────────
{
"id": "contour_map",
"label": "Contour Map",
"params": [
  {"name":"levels","label":"Levels","type":"float","min":3,"max":20,"default":8,"step":1},
  {"name":"line_width","label":"Line Width","type":"float","min":0.01,"max":0.15,"default":0.04,"step":0.005},
  {"name":"line_hue","label":"Line Hue","type":"float","min":0,"max":1,"default":0.35,"step":0.01},
  {"name":"fill_sat","label":"Fill Sat","type":"float","min":0,"max":1,"default":0.6,"step":0.01},
],
"shader": "contour_map.glsl",
"glsl": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_levels;
uniform float u_line_width;
uniform float u_line_hue;
uniform float u_fill_sat;
vec3 hue2rgb(float h) {
    vec4 K = vec4(1.0,2.0/3.0,1.0/3.0,3.0);
    return clamp(abs(fract(h+K.xyz)*6.0-K.www)-K.xxx, 0.0, 1.0);
}
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Quantize to contour levels
    float level = floor(lum * u_levels) / u_levels;
    float level_frac = fract(lum * u_levels);
    // Draw contour lines at zone boundaries
    float line = smoothstep(u_line_width, 0.0, min(level_frac, 1.0-level_frac));
    // Fill: luminance-to-hue map (like topographic coloring)
    float fill_hue = mix(0.67, 0.0, level);  // blue (deep) → red (high)
    vec3 fill_col = mix(vec3(level), hue2rgb(fill_hue), u_fill_sat);
    fill_col *= (0.3 + 0.7 * level);
    vec3 line_col = hue2rgb(u_line_hue) * 0.8;
    vec3 result = mix(fill_col, line_col, line);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
"""
},

# ── 18. film_halation ─────────────────────────────────────────────────────────
{
"id": "film_halation",
"label": "Film Halation",
"params": [
  {"name":"threshold","label":"Threshold","type":"float","min":0.3,"max":0.9,"default":0.65,"step":0.01},
  {"name":"radius","label":"Radius","type":"float","min":2,"max":20,"default":10,"step":0.5},
  {"name":"red_shift","label":"Red Shift","type":"float","min":0,"max":1,"default":0.75,"step":0.01},
  {"name":"strength","label":"Strength","type":"float","min":0,"max":2,"default":0.9,"step":0.05},
],
"shader": "film_halation.glsl",
"glsl": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_threshold;
uniform float u_radius;
uniform float u_red_shift;
uniform float u_strength;
void main() {
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    vec4 orig = texture(u_tex, v_uv);
    // Gaussian blur of bright highlight mask with red/orange tint
    vec3 halo = vec3(0.0);
    float wsum = 0.0;
    int R = int(u_radius);
    for (int dy=-R;dy<=R;dy++) {
        for (int dx=-R;dx<=R;dx++) {
            float w = exp(-float(dx*dx+dy*dy)/(u_radius*u_radius+0.001));
            vec4 s = texture(u_tex, clamp(v_uv + vec2(dx,dy)*px, 0.0, 1.0));
            float bright = smoothstep(u_threshold, u_threshold+0.2, dot(s.rgb, vec3(0.299,0.587,0.114)));
            halo += s.rgb * bright * w;
            wsum += w;
        }
    }
    halo /= (wsum + 0.001);
    // Halation is shifted toward red-orange (light scattering in film)
    halo = halo * mix(vec3(1.0), vec3(1.5, 0.5, 0.2), u_red_shift);
    // Screen blend onto original
    vec3 result = 1.0 - (1.0 - orig.rgb) * (1.0 - halo * u_strength * 0.8);
    frag = vec4(clamp(result, 0.0, 1.0), orig.a);
}
"""
},

# ── 19. ascii_art ─────────────────────────────────────────────────────────────
{
"id": "ascii_art",
"label": "ASCII Art",
"params": [
  {"name":"char_size","label":"Char Size","type":"float","min":4,"max":24,"default":10,"step":1},
  {"name":"fg_r","label":"FG Red","type":"float","min":0,"max":1,"default":0.0,"step":0.01},
  {"name":"fg_g","label":"FG Green","type":"float","min":0,"max":1,"default":1.0,"step":0.01},
  {"name":"fg_b","label":"FG Blue","type":"float","min":0,"max":1,"default":0.15,"step":0.01},
  {"name":"bg_dark","label":"BG Darken","type":"float","min":0,"max":1,"default":0.85,"step":0.01},
],
"shader": "ascii_art.glsl",
"glsl": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_char_size;
uniform float u_fg_r;
uniform float u_fg_g;
uniform float u_fg_b;
uniform float u_bg_dark;
float hash(vec2 p) { return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453); }
// Approximate ASCII char density from luminance using procedural patterns
float char_pattern(vec2 cell_uv, float density) {
    // Use density to select pattern type
    float d = density;
    // Horizontal line (low density)
    float pat1 = step(0.45, cell_uv.y) * step(cell_uv.y, 0.55);
    // Cross (+)
    float pat2 = max(step(0.45,cell_uv.y)*step(cell_uv.y,0.55), step(0.45,cell_uv.x)*step(cell_uv.x,0.55));
    // Hash (#) - grid lines
    float gx = step(0.3,cell_uv.x)*step(cell_uv.x,0.4) + step(0.6,cell_uv.x)*step(cell_uv.x,0.7);
    float gy = step(0.3,cell_uv.y)*step(cell_uv.y,0.4) + step(0.6,cell_uv.y)*step(cell_uv.y,0.7);
    float pat3 = max(gx, gy);
    // Block (full)
    float pat4 = 1.0;
    if (d < 0.25) return mix(0.0, pat1, d*4.0);
    if (d < 0.5)  return mix(pat1, pat2, (d-0.25)*4.0);
    if (d < 0.75) return mix(pat2, pat3, (d-0.5)*4.0);
    return mix(pat3, pat4, (d-0.75)*4.0);
}
void main() {
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    vec2 char_uv = floor(v_uv / (px * u_char_size)) * px * u_char_size;
    float lum = dot(texture(u_tex, char_uv + px * u_char_size * 0.5).rgb, vec3(0.299,0.587,0.114));
    vec2 cell_uv = fract(v_uv / (px * u_char_size));
    float on = char_pattern(cell_uv, lum);
    vec4 orig = texture(u_tex, v_uv);
    vec3 fg = vec3(u_fg_r, u_fg_g, u_fg_b);
    vec3 bg = orig.rgb * (1.0 - u_bg_dark);
    vec3 result = mix(bg, fg * (0.3 + lum * 0.7), on);
    frag = vec4(clamp(result, 0.0, 1.0), orig.a);
}
"""
},

# ── 20. dna_helix ─────────────────────────────────────────────────────────────
{
"id": "dna_helix",
"label": "DNA Helix Grid",
"params": [
  {"name":"grid_scale","label":"Grid Scale","type":"float","min":5,"max":30,"default":12,"step":0.5},
  {"name":"wave_amp","label":"Wave Amp","type":"float","min":0.1,"max":1.0,"default":0.5,"step":0.01},
  {"name":"line_width","label":"Line Width","type":"float","min":0.01,"max":0.15,"default":0.05,"step":0.005},
  {"name":"hue","label":"Hue","type":"float","min":0,"max":1,"default":0.55,"step":0.01},
  {"name":"bg_darken","label":"BG Darken","type":"float","min":0,"max":1,"default":0.6,"step":0.01},
  {"name":"u_time","label":"","type":"float","min":0,"max":1000,"default":0.0,"step":0.016,"hidden":True},
],
"shader": "dna_helix.glsl",
"glsl": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_grid_scale;
uniform float u_wave_amp;
uniform float u_line_width;
uniform float u_hue;
uniform float u_bg_darken;
uniform float u_time;
vec3 hue2rgb(float h) {
    vec4 K = vec4(1.0,2.0/3.0,1.0/3.0,3.0);
    return clamp(abs(fract(h+K.xyz)*6.0-K.www)-K.xxx, 0.0, 1.0);
}
void main() {
    vec4 col = texture(u_tex, v_uv);
    float t = u_time * 0.8;
    vec2 uv = v_uv * u_grid_scale;
    float row = floor(uv.y);
    float cell_y = fract(uv.y);
    // Alternating sine waves for 2 strands
    float phase1 = row * 0.5 + t;
    float phase2 = row * 0.5 + t + 3.14159;
    float wave1 = sin(uv.x * 0.6 + phase1) * u_wave_amp;
    float wave2 = sin(uv.x * 0.6 + phase2) * u_wave_amp;
    // Map to cell_y [0,1]
    float cy1 = (wave1 + 1.0) * 0.5;
    float cy2 = (wave2 + 1.0) * 0.5;
    float d1 = abs(cell_y - cy1);
    float d2 = abs(cell_y - cy2);
    float strand = smoothstep(u_line_width, 0.0, min(d1, d2));
    // Connecting rungs between strands
    float rung_phase = fract(uv.x * 0.3 + t * 0.2);
    float rung = step(0.48, rung_phase) * step(rung_phase, 0.52);
    float rung_line = rung * smoothstep(u_line_width*2.0, 0.0, abs(cell_y - mix(cy1, cy2, 0.5)));
    float overlay = max(strand, rung_line * 0.6);
    // Hue varies along the helix
    float hv = fract(u_hue + uv.x * 0.03 + row * 0.07);
    vec3 line_col = hue2rgb(hv);
    vec3 bg = col.rgb * (1.0 - u_bg_darken * 0.5);
    vec3 result = mix(bg, line_col, overlay);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
"""
},

]

added = 0
for e in NEW_EFFECTS:
    if e["id"] in existing_ids:
        print(f"  skip {e['id']} (already exists)")
        continue
    shader_path = os.path.join(SHADER_DIR, e.pop("glsl_key", None) or e["shader"])
    glsl = textwrap.dedent(e.pop("glsl")).lstrip("\n")
    with open(shader_path, "w") as f:
        f.write(glsl)
    reg["effects"].append(e)
    existing_ids.add(e["id"])
    added += 1
    print(f"  wrote {e['shader']}")

reg["project_version"] = reg.get("project_version", 19) + 1
with open(REG, "w") as f:
    json.dump(reg, f, indent=2)

print(f"Updated registry: {added} effects added, version={reg['project_version']}")
print(f"Wrote {added} shaders.")
