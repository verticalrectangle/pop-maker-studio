#!/usr/bin/env python3
"""Add batch 3: 20 effects — color grading, stylized, experimental."""
import json, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REG = os.path.join(ROOT, "effects", "registry.json")
SHADER_DIR = os.path.join(ROOT, "shaders")

def ws(name, src):
    with open(os.path.join(SHADER_DIR, f"{name}.glsl"), "w") as f:
        f.write(src)
    print(f"  wrote shaders/{name}.glsl")

with open(REG) as f:
    reg = json.load(f)

BATCH = [
    {
        "id": "frosted_glass", "enum": "FrostedGlass", "label": "Frosted Glass",
        "description": "Frosted glass — directional blur with edge-dependent scatter",
        "abbrev": "FRG", "color": [200, 230, 255, 220],
        "shader": "shaders/frosted_glass.glsl",
        "params": [
            {"name": "blur",     "label": "Blur",     "default": 0.018, "min": 0.0, "max": 0.06, "fmt": "%.3f"},
            {"name": "noise",    "label": "Noise",    "default": 0.5,   "min": 0.0, "max": 1.0,  "fmt": "%.2f"},
            {"name": "tint",     "label": "Tint",     "default": 0.3,   "min": 0.0, "max": 1.0,  "fmt": "%.2f"}
        ]
    },
    {
        "id": "mirror_fold", "enum": "MirrorFold", "label": "Mirror Fold",
        "description": "Horizontal mirror fold — left half reflects to right",
        "abbrev": "MRF", "color": [140, 200, 255, 220],
        "shader": "shaders/mirror_fold.glsl",
        "params": [
            {"name": "axis",     "label": "Axis",     "default": 0.5,  "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "vertical", "label": "Vertical", "default": 0.0,  "min": 0.0, "max": 1.0, "fmt": "%.0f"},
            {"name": "blend",    "label": "Blend",    "default": 0.0,  "min": 0.0, "max": 1.0, "fmt": "%.2f"}
        ]
    },
    {
        "id": "echo_trails", "enum": "EchoTrails", "label": "Echo Trails",
        "description": "Fading ghost trail echo copies shifted in a direction",
        "abbrev": "ECT", "color": [180, 100, 255, 220],
        "shader": "shaders/echo_trails.glsl",
        "params": [
            {"name": "offset",  "label": "Offset",    "default": 0.025, "min": 0.0, "max": 0.1,  "fmt": "%.3f"},
            {"name": "fade",    "label": "Fade",      "default": 0.55,  "min": 0.1, "max": 0.9,  "fmt": "%.2f"},
            {"name": "angle",   "label": "Angle",     "default": 45.0,  "min": 0.0, "max": 360.0,"fmt": "%.0f"}
        ]
    },
    {
        "id": "gradient_map", "enum": "GradientMap", "label": "Gradient Map",
        "description": "Maps luminance to a two-color gradient ramp",
        "abbrev": "GRM", "color": [100, 180, 255, 220],
        "shader": "shaders/gradient_map.glsl",
        "params": [
            {"name": "hue1",     "label": "Shadow Hue",   "default": 0.65, "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "hue2",     "label": "Highlight Hue","default": 0.12, "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "mix_orig", "label": "Mix Original", "default": 0.25, "min": 0.0, "max": 1.0, "fmt": "%.2f"}
        ]
    },
    {
        "id": "cross_process", "enum": "CrossProcess", "label": "Cross Process",
        "description": "E6 slide film cross-processed in C41: wild hue shifts",
        "abbrev": "CXP", "color": [255, 120, 60, 220],
        "shader": "shaders/cross_process.glsl",
        "params": [
            {"name": "strength", "label": "Strength", "default": 0.85, "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "contrast", "label": "Contrast", "default": 1.4,  "min": 1.0, "max": 2.5, "fmt": "%.1f"}
        ]
    },
    {
        "id": "plasma_field", "enum": "PlasmaField", "label": "Plasma",
        "description": "Psychedelic animated plasma color field overlay",
        "abbrev": "PLS", "color": [255, 60, 200, 220],
        "shader": "shaders/plasma_field.glsl",
        "params": [
            {"name": "scale",     "label": "Scale",     "default": 4.0,  "min": 1.0, "max": 12.0, "fmt": "%.0f"},
            {"name": "speed",     "label": "Speed",     "default": 1.5,  "min": 0.0, "max": 5.0,  "fmt": "%.1f"},
            {"name": "intensity", "label": "Mix",       "default": 0.55, "min": 0.0, "max": 1.0,  "fmt": "%.2f"}
        ]
    },
    {
        "id": "fire_edge", "enum": "FireEdge", "label": "Fire Edge",
        "description": "Procedural fire rises from the bottom edges",
        "abbrev": "FRE", "color": [255, 80, 0, 220],
        "shader": "shaders/fire_edge.glsl",
        "params": [
            {"name": "intensity", "label": "Intensity", "default": 1.0,  "min": 0.0, "max": 2.5, "fmt": "%.1f"},
            {"name": "speed",     "label": "Speed",     "default": 2.0,  "min": 0.0, "max": 5.0, "fmt": "%.1f"},
            {"name": "height",    "label": "Height",    "default": 0.4,  "min": 0.0, "max": 1.0, "fmt": "%.2f"}
        ]
    },
    {
        "id": "laser_grid", "enum": "LaserGrid", "label": "Laser Grid",
        "description": "Sci-fi laser wireframe grid overlay",
        "abbrev": "LSG", "color": [0, 230, 180, 220],
        "shader": "shaders/laser_grid.glsl",
        "params": [
            {"name": "grid_size", "label": "Grid Size", "default": 14.0, "min": 4.0, "max": 40.0, "fmt": "%.0f"},
            {"name": "hue",       "label": "Hue",       "default": 0.5,  "min": 0.0, "max": 1.0,  "fmt": "%.2f"},
            {"name": "intensity", "label": "Intensity", "default": 0.8,  "min": 0.0, "max": 2.0,  "fmt": "%.1f"}
        ]
    },
    {
        "id": "technicolor", "enum": "Technicolor", "label": "Technicolor",
        "description": "3-strip Technicolor: vivid saturated film-era color",
        "abbrev": "TCH", "color": [230, 60, 80, 220],
        "shader": "shaders/technicolor.glsl",
        "params": [
            {"name": "saturation", "label": "Saturation", "default": 2.0,  "min": 1.0, "max": 4.0, "fmt": "%.1f"},
            {"name": "contrast",   "label": "Contrast",   "default": 1.3,  "min": 1.0, "max": 2.5, "fmt": "%.1f"},
            {"name": "warmth",     "label": "Warmth",     "default": 0.35, "min": 0.0, "max": 1.0, "fmt": "%.2f"}
        ]
    },
    {
        "id": "ice_crystal", "enum": "IceCrystal", "label": "Ice Crystal",
        "description": "Crystalline ice-fracture distortion pattern",
        "abbrev": "ICE", "color": [160, 220, 255, 220],
        "shader": "shaders/ice_crystal.glsl",
        "params": [
            {"name": "scale",     "label": "Scale",     "default": 8.0,  "min": 2.0, "max": 20.0, "fmt": "%.0f"},
            {"name": "refract",   "label": "Refract",   "default": 0.04, "min": 0.0, "max": 0.15, "fmt": "%.3f"},
            {"name": "tint",      "label": "Blue Tint", "default": 0.35, "min": 0.0, "max": 1.0,  "fmt": "%.2f"}
        ]
    },
    {
        "id": "kodachrome", "enum": "Kodachrome", "label": "Kodachrome",
        "description": "Kodachrome 64 film — deep reds, rich saturation, golden shadows",
        "abbrev": "KDC", "color": [220, 80, 60, 220],
        "shader": "shaders/kodachrome.glsl",
        "params": [
            {"name": "saturation", "label": "Saturation", "default": 1.5,  "min": 0.5, "max": 3.0, "fmt": "%.1f"},
            {"name": "reds",       "label": "Red Boost",  "default": 0.5,  "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "shadows",    "label": "Gold Shadow","default": 0.4,  "min": 0.0, "max": 1.0, "fmt": "%.2f"}
        ]
    },
    {
        "id": "vortex_distort", "enum": "VortexDistort", "label": "Vortex",
        "description": "Multi-vortex turbulent distortion field",
        "abbrev": "VTX", "color": [200, 80, 255, 220],
        "shader": "shaders/vortex_distort.glsl",
        "params": [
            {"name": "strength", "label": "Strength", "default": 0.06, "min": 0.0, "max": 0.2,  "fmt": "%.3f"},
            {"name": "scale",    "label": "Scale",    "default": 3.0,  "min": 1.0, "max": 8.0,  "fmt": "%.0f"},
            {"name": "speed",    "label": "Speed",    "default": 0.8,  "min": 0.0, "max": 3.0,  "fmt": "%.1f"}
        ]
    },
    {
        "id": "sepia_rich", "enum": "SepiaRich", "label": "Sepia",
        "description": "Rich warm sepia with crushed blacks and vignette",
        "abbrev": "SEP", "color": [180, 140, 60, 220],
        "shader": "shaders/sepia_rich.glsl",
        "params": [
            {"name": "strength",  "label": "Strength",  "default": 0.9,  "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "vignette",  "label": "Vignette",  "default": 0.7,  "min": 0.0, "max": 2.0, "fmt": "%.1f"},
            {"name": "contrast",  "label": "Contrast",  "default": 1.2,  "min": 0.5, "max": 2.0, "fmt": "%.1f"}
        ]
    },
    {
        "id": "digital_noise", "enum": "DigitalNoise", "label": "Digital Noise",
        "description": "Colored RGB pixel noise — digital sensor ISO noise",
        "abbrev": "DGN", "color": [120, 120, 200, 220],
        "shader": "shaders/digital_noise.glsl",
        "params": [
            {"name": "amount",     "label": "Amount",     "default": 0.25, "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "color_sep",  "label": "Color Sep",  "default": 0.7,  "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "luma_bias",  "label": "Luma Bias",  "default": 0.5,  "min": 0.0, "max": 1.0, "fmt": "%.2f"}
        ]
    },
    {
        "id": "color_dodge", "enum": "ColorDodge", "label": "Color Dodge",
        "description": "Photographic color dodge — luminous blown-out look",
        "abbrev": "CDG", "color": [255, 230, 120, 220],
        "shader": "shaders/color_dodge.glsl",
        "params": [
            {"name": "amount",    "label": "Amount",    "default": 0.5,  "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "hue",       "label": "Dodge Hue", "default": 0.08, "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "glow",      "label": "Glow",      "default": 0.6,  "min": 0.0, "max": 1.0, "fmt": "%.2f"}
        ]
    },
    {
        "id": "warhol_pop", "enum": "WarholPop", "label": "Pop Art",
        "description": "Andy Warhol pop art — bold flat posterized colors",
        "abbrev": "POP", "color": [255, 80, 255, 220],
        "shader": "shaders/warhol_pop.glsl",
        "params": [
            {"name": "levels",     "label": "Levels",     "default": 4.0,  "min": 2.0, "max": 8.0, "fmt": "%.0f"},
            {"name": "hue_shift",  "label": "Hue Shift",  "default": 0.25, "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "saturation", "label": "Saturation", "default": 2.5,  "min": 1.0, "max": 4.0, "fmt": "%.1f"}
        ]
    },
    {
        "id": "infrared_film", "enum": "InfraredFilm", "label": "Infrared",
        "description": "Infrared film: glowing white foliage, dark skies",
        "abbrev": "IRF", "color": [255, 180, 180, 220],
        "shader": "shaders/infrared_film.glsl",
        "params": [
            {"name": "channel_mix", "label": "IR Mix",    "default": 0.8,  "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "glow",        "label": "Glow",      "default": 0.5,  "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "contrast",    "label": "Contrast",  "default": 1.4,  "min": 0.5, "max": 3.0, "fmt": "%.1f"}
        ]
    },
    {
        "id": "scanline_color", "enum": "ScanlineColor", "label": "RGB Scanlines",
        "description": "RGB phosphor triad scanlines — arcade monitor look",
        "abbrev": "SLC", "color": [60, 200, 80, 220],
        "shader": "shaders/scanline_color.glsl",
        "params": [
            {"name": "line_width", "label": "Line Width", "default": 3.0,  "min": 1.0, "max": 6.0, "fmt": "%.0f"},
            {"name": "intensity",  "label": "Intensity",  "default": 0.7,  "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "rgb_sep",    "label": "RGB Sep",    "default": 0.5,  "min": 0.0, "max": 1.0, "fmt": "%.2f"}
        ]
    },
    {
        "id": "data_corrupt", "enum": "DataCorrupt", "label": "Data Corrupt",
        "description": "Data corruption: random color block replacements",
        "abbrev": "DTC", "color": [255, 50, 50, 220],
        "shader": "shaders/data_corrupt.glsl",
        "params": [
            {"name": "density",   "label": "Density",   "default": 0.18, "min": 0.0, "max": 0.5, "fmt": "%.2f"},
            {"name": "block_size","label": "Block Size", "default": 8.0,  "min": 2.0, "max": 24.0,"fmt": "%.0f"},
            {"name": "intensity", "label": "Intensity", "default": 0.9,  "min": 0.0, "max": 1.0, "fmt": "%.2f"}
        ]
    },
    {
        "id": "long_exposure", "enum": "LongExposure", "label": "Long Exposure",
        "description": "Simulated long exposure — trails of light brighten",
        "abbrev": "LXP", "color": [255, 255, 160, 220],
        "shader": "shaders/long_exposure.glsl",
        "params": [
            {"name": "threshold",  "label": "Threshold",  "default": 0.5,  "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "trail",      "label": "Trail",      "default": 0.06, "min": 0.0, "max": 0.2, "fmt": "%.2f"},
            {"name": "glow",       "label": "Glow",       "default": 0.8,  "min": 0.0, "max": 2.0, "fmt": "%.1f"}
        ]
    },
]

SHADERS = {

"frosted_glass": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_blur;
uniform float u_noise;
uniform float u_tint;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    // Noise-perturbed scatter blur
    float n1 = hash(v_uv * 300.0) - 0.5;
    float n2 = hash(v_uv * 300.0 + vec2(71.3, 37.1)) - 0.5;
    vec2 scatter = vec2(n1, n2) * u_noise * 0.5;
    vec4 acc = vec4(0.0);
    float wt = 0.0;
    float radius = u_blur / px.x;
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -4; dx <= 4; dx++) {
            float d = length(vec2(float(dx), float(dy)));
            float w = exp(-d * d * 0.12);
            vec2 uv = v_uv + (vec2(float(dx),float(dy)) + scatter * d) * px * radius * 0.25;
            acc += texture(u_tex, clamp(uv, 0.0, 1.0)) * w;
            wt += w;
        }
    }
    vec3 blur = acc.rgb / wt;
    // Frosted glass tint (slightly blue-white)
    vec3 frost = mix(blur, vec3(0.85, 0.90, 1.0), u_tint * 0.25);
    // Add subtle refraction lines
    float lines = sin(v_uv.x * u_tex_w * 0.5 + n1 * 20.0) * 0.01 * u_noise;
    frost += lines;
    frag = vec4(clamp(frost, 0.0, 1.0), acc.a / wt);
}
""",

"mirror_fold": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_axis;
uniform float u_vertical;
uniform float u_blend;
void main() {
    vec2 uv = v_uv;
    vec2 mirrored = uv;
    if (u_vertical < 0.5) {
        // Horizontal mirror (left/right)
        if (uv.x > u_axis) mirrored.x = u_axis - (uv.x - u_axis);
    } else {
        // Vertical mirror (top/bottom)
        if (uv.y > u_axis) mirrored.y = u_axis - (uv.y - u_axis);
    }
    vec4 orig = texture(u_tex, clamp(uv, 0.0, 1.0));
    vec4 fold = texture(u_tex, clamp(mirrored, 0.0, 1.0));
    frag = mix(fold, orig, u_blend);
}
""",

"echo_trails": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_offset;
uniform float u_fade;
uniform float u_angle;
void main() {
    const float DEG2RAD = 0.017453293;
    float a = u_angle * DEG2RAD;
    vec2 dir = vec2(cos(a), -sin(a)) * u_offset;
    vec4 col = texture(u_tex, v_uv);
    vec4 result = col;
    float wt = 1.0;
    float w = u_fade;
    for (int i = 1; i <= 5; i++) {
        vec2 uv = clamp(v_uv + dir * float(i), 0.0, 1.0);
        vec4 echo = texture(u_tex, uv);
        // Screen blend each echo
        result.rgb = 1.0 - (1.0 - result.rgb) * (1.0 - echo.rgb * w);
        w *= u_fade;
    }
    frag = vec4(clamp(result.rgb, 0.0, 1.0), col.a);
}
""",

"gradient_map": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_hue1;
uniform float u_hue2;
uniform float u_mix_orig;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Map luminance to 2-color gradient
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p1 = abs(fract(u_hue1 + K.xyz) * 6.0 - K.www);
    vec3 p2 = abs(fract(u_hue2 + K.xyz) * 6.0 - K.www);
    vec3 c1 = clamp(p1 - K.xxx, 0.0, 1.0);
    vec3 c2 = clamp(p2 - K.xxx, 0.0, 1.0);
    vec3 mapped = mix(c1, c2, lum);
    // Preserve some luminance in the mapped color
    mapped *= (lum * 0.7 + 0.3);
    vec3 result = mix(mapped, col.rgb, u_mix_orig);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
""",

"cross_process": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_strength;
uniform float u_contrast;
void main() {
    vec4 col = texture(u_tex, v_uv);
    vec3 c = col.rgb;
    // E6 in C41 cross process simulation:
    // R curve: boost highlights, crush shadows
    float r = clamp(pow(c.r * 1.2, 0.7) * 1.1, 0.0, 1.0);
    // G curve: slight S-curve
    float g = clamp((c.g - 0.5) * 1.0 * u_contrast + 0.5, 0.0, 1.0);
    // B curve: boost shadows, crush highlights (inverted feel)
    float b = clamp(1.0 - pow(1.0 - c.b, 0.6) * 0.9, 0.0, 1.0);
    // Push saturation massively
    vec3 xp = vec3(r, g, b);
    float lum = dot(xp, vec3(0.299, 0.587, 0.114));
    xp = mix(vec3(lum), xp, 2.2);
    // Contrast pump
    xp = clamp((xp - 0.5) * u_contrast + 0.5, 0.0, 1.0);
    frag = vec4(mix(col.rgb, xp, u_strength), col.a);
}
""",

"plasma_field": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_scale;
uniform float u_speed;
uniform float u_intensity;
uniform float u_time;
vec3 hue2rgb(float h) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(h + K.xyz) * 6.0 - K.www);
    return clamp(p - K.xxx, 0.0, 1.0);
}
void main() {
    vec4 col = texture(u_tex, v_uv);
    float t = u_time * u_speed;
    // Plasma = sum of sine waves in 2D
    float v1 = sin(v_uv.x * u_scale + t);
    float v2 = sin(v_uv.y * u_scale * 0.9 + t * 1.1);
    float v3 = sin((v_uv.x + v_uv.y) * u_scale * 0.7 + t * 0.8);
    float v4 = sin(sqrt((v_uv.x-0.5)*(v_uv.x-0.5)*u_scale*u_scale
                       +(v_uv.y-0.5)*(v_uv.y-0.5)*u_scale*u_scale) + t);
    float plasma = (v1 + v2 + v3 + v4) * 0.25;
    float hue = plasma * 0.5 + 0.5;
    vec3 plasma_col = hue2rgb(hue);
    // Screen blend plasma over image
    vec3 screen = 1.0 - (1.0 - col.rgb) * (1.0 - plasma_col * u_intensity * 0.7);
    frag = vec4(clamp(screen, 0.0, 1.0), col.a);
}
""",

"fire_edge": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_intensity;
uniform float u_speed;
uniform float u_height;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
float noise2(vec2 p) {
    vec2 i = floor(p); vec2 f = fract(p); f = f*f*(3.0-2.0*f);
    return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);
}
float fbm3(vec2 p) {
    return noise2(p)*0.5 + noise2(p*2.1)*0.25 + noise2(p*4.3)*0.125;
}
void main() {
    vec4 col = texture(u_tex, v_uv);
    float t = u_time * u_speed;
    // Fire at bottom of image (y=1 in flipped coords = bottom of screen)
    float fire_y = 1.0 - v_uv.y;  // 0 at top, 1 at bottom
    float n = fbm3(vec2(v_uv.x * 4.0 + t * 0.2, fire_y * 3.0 - t * 0.8));
    float flame = smoothstep(u_height + n * 0.4, 0.0, fire_y) * (n * 0.7 + 0.3);
    // Fire palette: black → red → orange → yellow → white
    vec3 fire_col;
    float f = flame * u_intensity;
    if (f < 0.25)      fire_col = mix(vec3(0.0), vec3(0.8, 0.1, 0.0), f/0.25);
    else if (f < 0.5)  fire_col = mix(vec3(0.8,0.1,0.0), vec3(1.0,0.5,0.05), (f-0.25)/0.25);
    else if (f < 0.75) fire_col = mix(vec3(1.0,0.5,0.05), vec3(1.0,0.9,0.3), (f-0.5)/0.25);
    else               fire_col = mix(vec3(1.0,0.9,0.3), vec3(1.0,1.0,0.9), (f-0.75)/0.25);
    float alpha = clamp(flame * u_intensity * 1.5, 0.0, 1.0);
    vec3 result = mix(col.rgb, max(col.rgb, fire_col), alpha);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
""",

"laser_grid": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_grid_size;
uniform float u_hue;
uniform float u_intensity;
void main() {
    vec4 col = texture(u_tex, v_uv);
    vec2 uv_px = v_uv * vec2(u_tex_w, u_tex_h);
    // Grid lines
    vec2 grid = abs(fract(uv_px / u_grid_size) - 0.5) * 2.0;
    float line = 1.0 - min(grid.x, grid.y);
    float laser = smoothstep(0.85, 1.0, line);
    // Glow falloff
    float glow = smoothstep(0.6, 0.85, line) * 0.3;
    // Hue to RGB
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(u_hue + K.xyz) * 6.0 - K.www);
    vec3 lcolor = clamp(p - K.xxx, 0.0, 1.0);
    // Slight perspective: fade toward center
    float depth = 1.0 - length(v_uv - 0.5) * 0.8;
    vec3 result = col.rgb * (1.0 - laser * 0.7)
                + lcolor * (laser + glow) * u_intensity * depth;
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
""",

"technicolor": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_saturation;
uniform float u_contrast;
uniform float u_warmth;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Aggressive saturation push
    vec3 sat = mix(vec3(lum), col.rgb, u_saturation);
    // Contrast S-curve
    sat = clamp((sat - 0.5) * u_contrast + 0.5, 0.0, 1.0);
    // Warm shift (Technicolor skewed warm)
    sat.r = min(sat.r * (1.0 + u_warmth * 0.25), 1.0);
    sat.b = sat.b * (1.0 - u_warmth * 0.15);
    // Slight red/cyan split to simulate 3-strip registration
    float r = texture(u_tex, clamp(v_uv + vec2(0.002, 0.0), 0.0, 1.0)).r;
    float lum2 = dot(sat, vec3(0.299, 0.587, 0.114));
    sat.r = mix(sat.r, pow(r * (1.0 + u_warmth*0.3), 0.9), 0.3);
    frag = vec4(clamp(sat, 0.0, 1.0), col.a);
}
""",

"ice_crystal": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_scale;
uniform float u_refract;
uniform float u_tint;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec2 uv_sc = v_uv * vec2(u_tex_w, u_tex_h) / u_scale;
    // Voronoi for crystal cell structure
    vec2 cell = floor(uv_sc);
    vec2 local = fract(uv_sc);
    float min_d1 = 1e9, min_d2 = 1e9;
    vec2 nearest = vec2(0.0);
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            vec2 nb = cell + vec2(float(dx), float(dy));
            vec2 pt = vec2(hash(nb), hash(nb + vec2(17.3, 43.7)));
            float d = length(local - nb + cell - nb + pt);
            d = length(fract(uv_sc) - pt - vec2(float(dx), float(dy)));
            if (d < min_d1) { min_d2 = min_d1; min_d1 = d; nearest = pt; }
            else if (d < min_d2) { min_d2 = d; }
        }
    }
    // Cell border = refraction interface
    float border_dist = min_d2 - min_d1;
    float border = smoothstep(0.05, 0.0, border_dist);
    // Refract at cell borders
    vec2 refract_dir = normalize(v_uv - (cell + nearest) * u_scale / vec2(u_tex_w, u_tex_h));
    vec2 refract_uv = clamp(v_uv + refract_dir * border * u_refract, 0.0, 1.0);
    vec3 sample_col = texture(u_tex, refract_uv).rgb;
    // Blue-white ice tint
    vec3 ice_tint = mix(sample_col, sample_col * vec3(0.7, 0.85, 1.2), u_tint);
    // Bright borders
    ice_tint += border * 0.5 * vec3(0.8, 0.9, 1.0);
    frag = vec4(clamp(ice_tint, 0.0, 1.0), 1.0);
}
""",

"kodachrome": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_saturation;
uniform float u_reds;
uniform float u_shadows;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Saturation boost
    vec3 sat = mix(vec3(lum), col.rgb, u_saturation);
    // Boost reds/magentas (Kodachrome characteristic)
    float red_dominant = max(sat.r - sat.g, max(sat.r - sat.b, 0.0));
    sat.r = min(sat.r + red_dominant * u_reds * 0.4, 1.0);
    sat.g = max(sat.g - red_dominant * u_reds * 0.1, 0.0);
    // Golden shadow lift (warm shadow color)
    float shadow_mask = smoothstep(0.35, 0.0, lum);
    vec3 gold = vec3(0.12, 0.08, 0.0);
    sat = sat + gold * shadow_mask * u_shadows;
    // Slight blue desaturation (Kodachrome tends toward warm)
    sat.b = mix(sat.b, sat.b * 0.85, u_reds * 0.3);
    frag = vec4(clamp(sat, 0.0, 1.0), col.a);
}
""",

"vortex_distort": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_strength;
uniform float u_scale;
uniform float u_speed;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
float noise2(vec2 p) {
    vec2 i = floor(p); vec2 f = fract(p); f = f*f*(3.0-2.0*f);
    return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);
}
void main() {
    float t = u_time * u_speed;
    vec2 sc = v_uv * u_scale;
    // 2 octaves of curl noise for vortex field
    float nx = noise2(sc + vec2(t * 0.3, 0.0))
             + noise2(sc * 2.0 + vec2(0.0, t * 0.4)) * 0.5;
    float ny = noise2(sc + vec2(100.0, t * 0.25))
             + noise2(sc * 2.0 + vec2(100.0, t * 0.35)) * 0.5;
    // Curl field: rotate the gradient 90 degrees
    vec2 curl = vec2(ny - 0.5, -(nx - 0.5)) * 2.0;
    vec2 uv = clamp(v_uv + curl * u_strength, 0.0, 1.0);
    frag = texture(u_tex, uv);
}
""",

"sepia_rich": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_strength;
uniform float u_vignette;
uniform float u_contrast;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    lum = clamp((lum - 0.5) * u_contrast + 0.5, 0.0, 1.0);
    // Rich warm sepia tones
    vec3 sepia = vec3(
        lum * 1.08 + 0.05,
        lum * 0.88 + 0.02,
        lum * 0.62
    );
    vec3 result = mix(col.rgb, sepia, u_strength);
    // Vignette
    vec2 d = (v_uv - 0.5) * vec2(1.0, 1.3);
    float vig = 1.0 - smoothstep(0.2, 0.7, length(d)) * u_vignette;
    result *= vig;
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
""",

"digital_noise": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_amount;
uniform float u_color_sep;
uniform float u_luma_bias;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec4 col = texture(u_tex, v_uv);
    vec2 npx = floor(v_uv * vec2(u_tex_w, u_tex_h));
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Noise stronger in darks (shadow noise)
    float noise_scale = mix(1.0, 1.0 - lum, u_luma_bias) * u_amount;
    float nr = (hash(npx + vec2(u_time * 0.3, 0.0)) - 0.5) * noise_scale * 0.35;
    float ng = (hash(npx + vec2(0.0, u_time * 0.4) + vec2(31.7, 71.3)) - 0.5) * noise_scale * 0.35;
    float nb_n = (hash(npx + vec2(u_time * 0.5) + vec2(97.1, 13.7)) - 0.5) * noise_scale * 0.35;
    // Mix RGB and luminance noise
    float luma_noise = (hash(npx + vec2(u_time * 0.35, u_time * 0.25)) - 0.5) * noise_scale * 0.35;
    vec3 noise = mix(vec3(luma_noise), vec3(nr, ng, nb_n), u_color_sep);
    frag = vec4(clamp(col.rgb + noise, 0.0, 1.0), col.a);
}
""",

"color_dodge": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_amount;
uniform float u_hue;
uniform float u_glow;
void main() {
    vec4 col = texture(u_tex, v_uv);
    // Convert hue to dodge color
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 kp = abs(fract(u_hue + K.xyz) * 6.0 - K.www);
    vec3 dodge_col = clamp(kp - K.xxx, 0.0, 1.0);
    // Color dodge blend mode
    vec3 dodged = col.rgb / max(1.0 - dodge_col * u_amount, vec3(0.001));
    dodged = clamp(dodged, 0.0, 1.0);
    // Glow halo
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    vec3 bloom = vec3(0.0);
    for (int i = 1; i <= 4; i++) {
        float r = float(i) * 3.0;
        bloom += texture(u_tex, clamp(v_uv + vec2(r, 0)*px, 0.0, 1.0)).rgb;
        bloom += texture(u_tex, clamp(v_uv - vec2(r, 0)*px, 0.0, 1.0)).rgb;
        bloom += texture(u_tex, clamp(v_uv + vec2(0, r)*px, 0.0, 1.0)).rgb;
        bloom += texture(u_tex, clamp(v_uv - vec2(0, r)*px, 0.0, 1.0)).rgb;
    }
    bloom /= 16.0;
    vec3 result = mix(dodged, 1.0 - (1.0 - dodged) * (1.0 - bloom * dodge_col * u_glow), 0.5);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
""",

"warhol_pop": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_levels;
uniform float u_hue_shift;
uniform float u_saturation;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Posterize
    float quant_lum = floor(lum * u_levels + 0.5) / u_levels;
    // Remap lum to a hue
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    float hue = fract(quant_lum * 0.7 + u_hue_shift);
    vec3 p = abs(fract(hue + K.xyz) * 6.0 - K.www);
    vec3 hue_col = clamp(p - K.xxx, 0.0, 1.0);
    // Saturate the original, then apply pop color
    vec3 sat_orig = mix(vec3(lum), col.rgb, u_saturation);
    // Mix: posterized hue with saturated original
    vec3 pop = hue_col * quant_lum;
    vec3 result = mix(sat_orig, pop, 0.65);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
""",

"infrared_film": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_channel_mix;
uniform float u_glow;
uniform float u_contrast;
void main() {
    vec4 col = texture(u_tex, v_uv);
    // IR: green channel reads as infrared (foliage glows white)
    // Simulate by: high green = white, high blue = dark
    float ir = col.r * 0.2 + col.g * 0.7 + col.b * 0.1;  // IR channel
    float vis = dot(col.rgb, vec3(0.299, 0.587, 0.114));    // Visible
    float ir_val = mix(vis, ir, u_channel_mix);
    // Contrast pump for IR look
    ir_val = clamp((ir_val - 0.5) * u_contrast + 0.5, 0.0, 1.0);
    // Wood effect: green channel gets mapped to very bright
    float wood = smoothstep(0.4, 0.8, col.g) * (1.0 - col.r * 0.5);
    ir_val = mix(ir_val, 1.0, wood * u_channel_mix * 0.5);
    // Glow on bright areas
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    float glow_acc = 0.0;
    for (int i = 1; i <= 4; i++) {
        float r = float(i) * 2.5;
        glow_acc += texture(u_tex, clamp(v_uv+vec2(r,0)*px,0.,1.)).g;
        glow_acc += texture(u_tex, clamp(v_uv-vec2(r,0)*px,0.,1.)).g;
        glow_acc += texture(u_tex, clamp(v_uv+vec2(0,r)*px,0.,1.)).g;
        glow_acc += texture(u_tex, clamp(v_uv-vec2(0,r)*px,0.,1.)).g;
    }
    glow_acc /= 16.0;
    ir_val = min(ir_val + glow_acc * u_glow * 0.3, 1.0);
    // Slight warm tone
    vec3 result = vec3(ir_val * 1.02, ir_val * 0.99, ir_val * 0.92);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
""",

"scanline_color": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_h;
uniform float u_line_width;
uniform float u_intensity;
uniform float u_rgb_sep;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float line_pos = mod(v_uv.y * u_tex_h, u_line_width * 3.0);
    // 3 sub-pixels: R G B
    float sub_r = step(line_pos, u_line_width);
    float sub_g = step(u_line_width, line_pos) * step(line_pos, u_line_width * 2.0);
    float sub_b = step(u_line_width * 2.0, line_pos);
    // RGB triad mask
    vec3 mask = mix(vec3(1.0), vec3(sub_r, sub_g, sub_b) * 1.5, u_rgb_sep);
    // Dark gap between triads
    float gap = step(u_line_width * 2.9, line_pos);
    mask *= (1.0 - gap * 0.8);
    vec3 result = col.rgb * mix(vec3(1.0), mask, u_intensity);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
""",

"data_corrupt": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_density;
uniform float u_block_size;
uniform float u_intensity;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec4 col = texture(u_tex, v_uv);
    // Block-based corruption
    vec2 block_id = floor(v_uv * vec2(u_tex_w, u_tex_h) / u_block_size);
    float frame = floor(u_time * 12.0);
    float rnd = hash(block_id + vec2(frame * 7.3, frame * 13.1));
    float rnd2 = hash(block_id + vec2(frame * 3.7, frame * 17.9));
    float rnd3 = hash(block_id + vec2(frame * 11.1, frame * 5.3));
    if (rnd < u_density) {
        // This block is corrupted: sample from a random other block
        vec2 corrupt_block = block_id + vec2((rnd2 - 0.5) * 20.0, 0.0);
        vec2 corrupt_uv = clamp((corrupt_block * u_block_size + fract(v_uv * vec2(u_tex_w,u_tex_h)/u_block_size) * u_block_size) / vec2(u_tex_w, u_tex_h), 0.0, 1.0);
        vec3 corrupt_col = texture(u_tex, corrupt_uv).rgb;
        // Add color glitch
        corrupt_col = vec3(corrupt_col.b, corrupt_col.r, corrupt_col.g) * (0.8 + rnd3 * 0.4);
        frag = vec4(mix(col.rgb, corrupt_col, u_intensity), col.a);
    } else {
        frag = col;
    }
}
""",

"long_exposure": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_threshold;
uniform float u_trail;
uniform float u_glow;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    // Bright pixels trail outward (simulates long-exposure streak)
    vec3 trails = col.rgb;
    if (lum > u_threshold) {
        // Sample along 4 directions to create light streaks
        vec3 streak = vec3(0.0);
        for (int i = 1; i <= 12; i++) {
            float t = float(i);
            float w = exp(-t * u_trail * 0.5);
            streak += texture(u_tex, clamp(v_uv + vec2(t, 0)*px*3.0, 0.0, 1.0)).rgb * w;
            streak += texture(u_tex, clamp(v_uv - vec2(t, 0)*px*3.0, 0.0, 1.0)).rgb * w;
            streak += texture(u_tex, clamp(v_uv + vec2(0, t)*px*3.0, 0.0, 1.0)).rgb * w;
            streak += texture(u_tex, clamp(v_uv - vec2(0, t)*px*3.0, 0.0, 1.0)).rgb * w;
        }
        streak /= 48.0;
        float bright_mask = smoothstep(u_threshold, u_threshold + 0.2, lum);
        trails = 1.0 - (1.0 - col.rgb) * (1.0 - streak * bright_mask * u_glow);
    }
    frag = vec4(clamp(trails, 0.0, 1.0), col.a);
}
""",

}  # end SHADERS

reg["effects"].extend(BATCH)
reg["project_version"] += 1

with open(REG, "w") as f:
    json.dump(reg, f, indent=2)
print(f"Updated registry: {len(BATCH)} effects added, version={reg['project_version']}")

os.makedirs(SHADER_DIR, exist_ok=True)
for name, src in SHADERS.items():
    ws(name, src)
print(f"Wrote {len(SHADERS)} shaders.")
