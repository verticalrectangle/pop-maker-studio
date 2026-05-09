#!/usr/bin/env python3
"""Add batch 2: 20 effects — artistic, retro, glitch, color."""
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
        "id": "pixel_sort", "enum": "PixelSort", "label": "Pixel Sort",
        "description": "Glitch art: sorts pixels by brightness along rows",
        "abbrev": "PST", "color": [255, 80, 120, 220],
        "shader": "shaders/pixel_sort.glsl",
        "params": [
            {"name": "threshold", "label": "Threshold", "default": 0.35, "min": 0.0, "max": 0.9, "fmt": "%.2f"},
            {"name": "amount",    "label": "Amount",    "default": 0.7,  "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "direction", "label": "Direction", "default": 0.0,  "min": 0.0, "max": 1.0, "fmt": "%.0f"}
        ]
    },
    {
        "id": "solarize", "enum": "Solarize", "label": "Solarize",
        "description": "Sabattier partial-negative solarization",
        "abbrev": "SLZ", "color": [230, 80, 200, 220],
        "shader": "shaders/solarize.glsl",
        "params": [
            {"name": "threshold", "label": "Threshold", "default": 0.5, "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "strength",  "label": "Strength",  "default": 0.9, "min": 0.0, "max": 1.0, "fmt": "%.2f"}
        ]
    },
    {
        "id": "double_ghost", "enum": "DoubleGhost", "label": "Double Ghost",
        "description": "Double-exposure ghost offset — drunk / split double-vision",
        "abbrev": "DGH", "color": [180, 180, 255, 220],
        "shader": "shaders/double_ghost.glsl",
        "params": [
            {"name": "offset",  "label": "Offset",  "default": 0.035, "min": 0.0, "max": 0.15, "fmt": "%.3f"},
            {"name": "opacity", "label": "Opacity", "default": 0.55,  "min": 0.0, "max": 1.0,  "fmt": "%.2f"},
            {"name": "angle",   "label": "Angle",   "default": 10.0,  "min": 0.0, "max": 90.0, "fmt": "%.0f"}
        ]
    },
    {
        "id": "watercolor", "enum": "Watercolor", "label": "Watercolor",
        "description": "Watercolor wash — bleeding edges and soft paper texture",
        "abbrev": "WCL", "color": [100, 180, 230, 220],
        "shader": "shaders/watercolor.glsl",
        "params": [
            {"name": "bleeding",   "label": "Bleeding",   "default": 0.018, "min": 0.0, "max": 0.06, "fmt": "%.3f"},
            {"name": "paper",      "label": "Paper",      "default": 0.5,   "min": 0.0, "max": 1.0,  "fmt": "%.2f"},
            {"name": "saturation", "label": "Saturation", "default": 1.4,   "min": 0.5, "max": 2.5,  "fmt": "%.1f"}
        ]
    },
    {
        "id": "comic_dots", "enum": "ComicDots", "label": "Comic Book",
        "description": "Ben-Day halftone dots + ink outline like pop art comics",
        "abbrev": "CMC", "color": [255, 220, 40, 220],
        "shader": "shaders/comic_dots.glsl",
        "params": [
            {"name": "dot_size",      "label": "Dot Size",  "default": 4.0,  "min": 1.0, "max": 10.0, "fmt": "%.0f"},
            {"name": "ink_threshold", "label": "Ink",       "default": 0.12, "min": 0.0, "max": 0.4,  "fmt": "%.2f"},
            {"name": "color_levels",  "label": "Colors",    "default": 4.0,  "min": 2.0, "max": 8.0,  "fmt": "%.0f"}
        ]
    },
    {
        "id": "crosshatch", "enum": "Crosshatch", "label": "Crosshatch",
        "description": "Pen crosshatch shading on light sketch paper",
        "abbrev": "XHT", "color": [140, 120, 80, 220],
        "shader": "shaders/crosshatch.glsl",
        "params": [
            {"name": "density",   "label": "Density",   "default": 8.0,  "min": 2.0, "max": 20.0, "fmt": "%.0f"},
            {"name": "thickness", "label": "Thickness", "default": 0.35, "min": 0.1, "max": 0.8,  "fmt": "%.2f"},
            {"name": "angle",     "label": "Angle",     "default": 0.0,  "min": 0.0, "max": 90.0, "fmt": "%.0f"}
        ]
    },
    {
        "id": "daguerreotype", "enum": "Daguerreotype", "label": "Daguerreotype",
        "description": "19th-century silver-plate sepia photograph",
        "abbrev": "DAG", "color": [180, 150, 80, 220],
        "shader": "shaders/daguerreotype.glsl",
        "params": [
            {"name": "tone",    "label": "Tone",    "default": 0.5, "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "vignette","label": "Vignette","default": 0.9, "min": 0.0, "max": 2.0, "fmt": "%.1f"},
            {"name": "scratch", "label": "Scratch", "default": 0.4, "min": 0.0, "max": 1.0, "fmt": "%.2f"}
        ]
    },
    {
        "id": "super8_film", "enum": "Super8Film", "label": "Super 8",
        "description": "Super 8mm film with gate weave, grain and warm fade",
        "abbrev": "S8F", "color": [220, 160, 60, 220],
        "shader": "shaders/super8_film.glsl",
        "params": [
            {"name": "grain",    "label": "Grain",    "default": 0.6, "min": 0.0, "max": 1.5, "fmt": "%.1f"},
            {"name": "gate",     "label": "Gate Weave","default": 0.4, "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "fade",     "label": "Fade",     "default": 0.5, "min": 0.0, "max": 1.0, "fmt": "%.2f"}
        ]
    },
    {
        "id": "vhs_dropout", "enum": "VHSDrop", "label": "VHS Dropout",
        "description": "VHS tape dropout: horizontal signal-loss white bars",
        "abbrev": "VHD", "color": [200, 200, 200, 220],
        "shader": "shaders/vhs_dropout.glsl",
        "params": [
            {"name": "density",  "label": "Density",  "default": 0.12, "min": 0.0, "max": 0.4, "fmt": "%.2f"},
            {"name": "strength", "label": "Strength", "default": 0.85, "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "speed",    "label": "Speed",    "default": 3.0,  "min": 0.0, "max": 8.0, "fmt": "%.1f"}
        ]
    },
    {
        "id": "x_ray", "enum": "XRay", "label": "X-Ray",
        "description": "X-ray: inverted high-contrast with blue-white tone",
        "abbrev": "XRY", "color": [180, 220, 255, 220],
        "shader": "shaders/x_ray.glsl",
        "params": [
            {"name": "contrast",  "label": "Contrast",  "default": 1.8, "min": 0.5, "max": 4.0, "fmt": "%.1f"},
            {"name": "blue_tint", "label": "Blue Tint", "default": 0.6, "min": 0.0, "max": 1.0, "fmt": "%.2f"}
        ]
    },
    {
        "id": "bit_crush", "enum": "BitCrush", "label": "Bit Crush",
        "description": "Bit-depth reduction — chunky posterized pixels with dither",
        "abbrev": "BCR", "color": [80, 255, 80, 220],
        "shader": "shaders/bit_crush.glsl",
        "params": [
            {"name": "levels",  "label": "Levels",  "default": 6.0,  "min": 2.0, "max": 32.0, "fmt": "%.0f"},
            {"name": "dither",  "label": "Dither",  "default": 0.4,  "min": 0.0, "max": 1.0,  "fmt": "%.2f"}
        ]
    },
    {
        "id": "tv_static", "enum": "TVStatic", "label": "TV Static",
        "description": "Analog TV static noise — channel between stations",
        "abbrev": "TVS", "color": [200, 200, 200, 220],
        "shader": "shaders/tv_static.glsl",
        "params": [
            {"name": "amount",    "label": "Amount",    "default": 0.55, "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "color_mix", "label": "Color Mix", "default": 0.25, "min": 0.0, "max": 1.0, "fmt": "%.2f"}
        ]
    },
    {
        "id": "dither_bayer", "enum": "DitherBayer", "label": "Dither",
        "description": "Ordered Bayer dithering — retro 1-bit style",
        "abbrev": "DTH", "color": [40, 40, 40, 220],
        "shader": "shaders/dither_bayer.glsl",
        "params": [
            {"name": "levels",    "label": "Levels",    "default": 3.0,  "min": 1.0, "max": 8.0, "fmt": "%.0f"},
            {"name": "scale",     "label": "Scale",     "default": 2.0,  "min": 1.0, "max": 6.0, "fmt": "%.0f"},
            {"name": "color",     "label": "Color",     "default": 0.4,  "min": 0.0, "max": 1.0, "fmt": "%.2f"}
        ]
    },
    {
        "id": "miami_vice", "enum": "MiamiVice", "label": "Miami Vice",
        "description": "80s hot pink and teal pastel neon palette",
        "abbrev": "MVC", "color": [255, 80, 160, 220],
        "shader": "shaders/miami_vice.glsl",
        "params": [
            {"name": "strength",  "label": "Strength",  "default": 0.8, "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "saturation","label": "Saturation","default": 1.8, "min": 0.5, "max": 3.0, "fmt": "%.1f"}
        ]
    },
    {
        "id": "horror_grade", "enum": "HorrorGrade", "label": "Horror",
        "description": "Desaturated red-channel horror film grade",
        "abbrev": "HRR", "color": [200, 20, 20, 220],
        "shader": "shaders/horror_grade.glsl",
        "params": [
            {"name": "desat",   "label": "Desaturate","default": 0.8, "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "red",     "label": "Red Boost", "default": 0.45,"min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "crush",   "label": "Crush",     "default": 0.15,"min": 0.0, "max": 0.4, "fmt": "%.2f"}
        ]
    },
    {
        "id": "split_toning", "enum": "SplitToning", "label": "Split Tone",
        "description": "Shadow/highlight split toning with independent hue control",
        "abbrev": "SPT", "color": [120, 80, 200, 220],
        "shader": "shaders/split_toning.glsl",
        "params": [
            {"name": "shadow_hue", "label": "Shadow Hue",    "default": 0.6,  "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "hi_hue",     "label": "Highlight Hue", "default": 0.1,  "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "strength",   "label": "Strength",      "default": 0.55, "min": 0.0, "max": 1.0, "fmt": "%.2f"}
        ]
    },
    {
        "id": "desert_gold", "enum": "DesertGold", "label": "Desert Gold",
        "description": "Sun-bleached golden desert haze with lifted blacks",
        "abbrev": "DSG", "color": [230, 190, 80, 220],
        "shader": "shaders/desert_gold.glsl",
        "params": [
            {"name": "warmth",  "label": "Warmth",  "default": 0.6, "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "fade",    "label": "Fade",    "default": 0.4, "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "haze",    "label": "Haze",    "default": 0.35,"min": 0.0, "max": 1.0, "fmt": "%.2f"}
        ]
    },
    {
        "id": "emboss_relief", "enum": "EmbossRelief", "label": "Emboss",
        "description": "3D emboss relief sculpture with directional lighting",
        "abbrev": "EMB", "color": [160, 160, 160, 220],
        "shader": "shaders/emboss_relief.glsl",
        "params": [
            {"name": "strength",  "label": "Strength",  "default": 3.0,  "min": 0.5, "max": 8.0,  "fmt": "%.1f"},
            {"name": "angle",     "label": "Light Angle","default": 135.0,"min": 0.0, "max": 360.0,"fmt": "%.0f"},
            {"name": "colorize",  "label": "Colorize",  "default": 0.0,  "min": 0.0, "max": 1.0,  "fmt": "%.2f"}
        ]
    },
    {
        "id": "pointillist", "enum": "Pointillist", "label": "Pointillism",
        "description": "Seurat-style pointillist dots — image from colored circles",
        "abbrev": "PTL", "color": [200, 230, 80, 220],
        "shader": "shaders/pointillist.glsl",
        "params": [
            {"name": "dot_size", "label": "Dot Size", "default": 5.0, "min": 2.0, "max": 14.0, "fmt": "%.0f"},
            {"name": "scatter",  "label": "Scatter",  "default": 0.4, "min": 0.0, "max": 1.0,  "fmt": "%.2f"}
        ]
    },
    {
        "id": "interlace_glitch", "enum": "InterlaceGlitch", "label": "Interlace",
        "description": "CRT interlace field offset glitch — alternating row shift",
        "abbrev": "ILG", "color": [200, 80, 80, 220],
        "shader": "shaders/interlace_glitch.glsl",
        "params": [
            {"name": "strength",  "label": "Strength",  "default": 0.012, "min": 0.0, "max": 0.05, "fmt": "%.3f"},
            {"name": "intensity", "label": "Intensity", "default": 0.6,   "min": 0.0, "max": 1.0,  "fmt": "%.2f"},
            {"name": "speed",     "label": "Speed",     "default": 2.0,   "min": 0.0, "max": 6.0,  "fmt": "%.1f"}
        ]
    },
]

SHADERS = {

"pixel_sort": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_threshold;
uniform float u_amount;
uniform float u_direction;
void main() {
    // Approximate pixel sort: sample along the sort axis,
    // displace toward brighter pixels above threshold.
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    vec2 ipx = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    vec2 axis = (u_direction < 0.5) ? vec2(ipx.x, 0.0) : vec2(0.0, ipx.y);
    // Find how far we're in a "sort run" — scan toward origin for contiguous above-threshold
    float run = 0.0;
    float max_run = 80.0 * u_amount;
    for (float i = 1.0; i <= max_run; i += 1.0) {
        vec3 s = texture(u_tex, v_uv - axis * i).rgb;
        float sl = dot(s, vec3(0.299, 0.587, 0.114));
        if (sl < u_threshold) break;
        run = i;
    }
    // If we're in a sort run, sample from a displaced position
    if (lum >= u_threshold && run > 0.0) {
        // Sorted output: sample ahead in the run to simulate sort
        float disp = run * u_amount;
        vec2 sort_uv = v_uv + axis * disp;
        frag = texture(u_tex, clamp(sort_uv, 0.0, 1.0));
    } else {
        frag = col;
    }
}
""",

"solarize": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_threshold;
uniform float u_strength;
void main() {
    vec4 col = texture(u_tex, v_uv);
    // Sabattier: invert channels that exceed threshold
    vec3 inv = 1.0 - col.rgb;
    // Smooth inversion using a tent function around threshold
    vec3 solar = mix(col.rgb,
                     mix(col.rgb, inv, step(u_threshold, col.rgb)),
                     u_strength);
    // Enhance the characteristic solarization colors
    float lum = dot(solar, vec3(0.299, 0.587, 0.114));
    solar = mix(vec3(lum), solar, 1.6); // boost saturation
    frag = vec4(clamp(solar, 0.0, 1.0), col.a);
}
""",

"double_ghost": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_offset;
uniform float u_opacity;
uniform float u_angle;
void main() {
    const float DEG2RAD = 0.017453293;
    float a = u_angle * DEG2RAD;
    vec2 dir = vec2(cos(a), sin(a)) * u_offset;
    vec4 col = texture(u_tex, v_uv);
    vec4 ghost = texture(u_tex, clamp(v_uv + dir, 0.0, 1.0));
    // Additive screen blend ghost
    vec3 screen = 1.0 - (1.0 - col.rgb) * (1.0 - ghost.rgb * u_opacity);
    // Slight cyan tint on the ghost
    ghost.rgb *= vec3(0.7, 0.9, 1.2);
    vec3 result = mix(screen, col.rgb * (1.0-u_opacity) + ghost.rgb * u_opacity, 0.5);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
""",

"watercolor": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_bleeding;
uniform float u_paper;
uniform float u_saturation;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    // Soft blur (watercolor wash base)
    vec3 acc = vec3(0.0);
    float wt = 0.0;
    for (int dy = -3; dy <= 3; dy++) {
        for (int dx = -3; dx <= 3; dx++) {
            float d = length(vec2(float(dx), float(dy)));
            float w = exp(-d * 0.6);
            vec2 uv = v_uv + vec2(float(dx),float(dy)) * px * (1.0 + u_bleeding * 30.0);
            acc += texture(u_tex, clamp(uv, 0.0, 1.0)).rgb * w;
            wt += w;
        }
    }
    vec3 wash = acc / wt;
    // Paper texture from noise
    float paper_n = hash(v_uv * vec2(u_tex_w, u_tex_h) * 0.05);
    float paper_tex = mix(1.0, paper_n * 0.3 + 0.85, u_paper);
    // Saturation boost (pigment richness)
    float lum = dot(wash, vec3(0.299, 0.587, 0.114));
    wash = mix(vec3(lum), wash, u_saturation) * paper_tex;
    // Slight edge darkening (wet paper bloom)
    vec4 orig = texture(u_tex, v_uv);
    frag = vec4(clamp(wash, 0.0, 1.0), orig.a);
}
""",

"comic_dots": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_dot_size;
uniform float u_ink_threshold;
uniform float u_color_levels;
void main() {
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    // Snap to dot grid
    vec2 cell = floor(v_uv / (px * u_dot_size)) * px * u_dot_size;
    vec2 cell_center = cell + px * u_dot_size * 0.5;
    vec3 cell_col = texture(u_tex, clamp(cell_center, 0.0, 1.0)).rgb;
    float lum = dot(cell_col, vec3(0.299, 0.587, 0.114));
    // Dot radius scales with brightness
    float dot_r = lum * 0.55;
    vec2 local = (v_uv - cell_center) / (px * u_dot_size);
    float in_dot = step(length(local), dot_r);
    // Posterize cell color
    cell_col = floor(cell_col * u_color_levels + 0.5) / u_color_levels;
    // Ink outline from Sobel
    vec3 gx = texture(u_tex, v_uv + vec2( px.x, 0)).rgb
             -texture(u_tex, v_uv - vec2( px.x, 0)).rgb;
    vec3 gy = texture(u_tex, v_uv + vec2(0,  px.y)).rgb
             -texture(u_tex, v_uv - vec2(0,  px.y)).rgb;
    float edge = clamp((length(gx)+length(gy) - u_ink_threshold) * 8.0, 0.0, 1.0);
    vec3 result = mix(vec3(1.0), cell_col, in_dot) * (1.0 - edge);
    frag = vec4(clamp(result, 0.0, 1.0), 1.0);
}
""",

"crosshatch": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_density;
uniform float u_thickness;
uniform float u_angle;
void main() {
    const float DEG2RAD = 0.017453293;
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    vec2 uv_px = v_uv * vec2(u_tex_w, u_tex_h);
    float paper = 0.95;
    float ink = 0.0;
    // 4 hatch layers at increasing darkness
    float angles[4];
    angles[0] = u_angle * DEG2RAD;
    angles[1] = angles[0] + 0.785;
    angles[2] = angles[0] + 0.393;
    angles[3] = angles[0] - 0.393;
    for (int i = 0; i < 4; i++) {
        float thresh = float(i+1) * 0.22;
        if (lum < thresh) {
            float cs = cos(angles[i]), sn = sin(angles[i]);
            float proj = cs * uv_px.x + sn * uv_px.y;
            float line = abs(fract(proj / u_density) - 0.5) * 2.0;
            float hatch = 1.0 - smoothstep(1.0 - u_thickness, 1.0, line);
            ink = max(ink, hatch);
        }
    }
    vec3 result = vec3(paper) * (1.0 - ink * 0.9);
    // Faint original color show-through
    result = mix(result, result * (col.rgb * 0.4 + 0.7), 0.25);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
""",

"daguerreotype": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tone;
uniform float u_vignette;
uniform float u_scratch;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // High contrast
    lum = clamp((lum - 0.5) * 1.4 + 0.5, 0.0, 1.0);
    // Sepia / silver-brown tone
    vec3 warm = vec3(0.85, 0.75, 0.55);
    vec3 cool = vec3(0.7, 0.75, 0.85);
    vec3 toned = mix(cool * lum, warm * lum, u_tone);
    // Heavy vignette (oval plate edges)
    vec2 d = (v_uv - 0.5) * vec2(1.0, 1.3);
    float vig = 1.0 - smoothstep(0.25, 0.75, length(d)) * u_vignette;
    toned *= vig;
    // Random scratches
    float scratch_n = hash(vec2(floor(v_uv.x * 400.0) / 400.0, u_time * 0.1));
    float scratch = step(1.0 - u_scratch * 0.04, scratch_n)
                  * smoothstep(0.4, 0.6, v_uv.y);
    toned += scratch * 0.4;
    // Silver plate texture noise
    float plate = hash(v_uv * 500.0) * 0.04 - 0.02;
    frag = vec4(clamp(toned + plate, 0.0, 1.0), col.a);
}
""",

"super8_film": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_grain;
uniform float u_gate;
uniform float u_fade;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    // Gate weave: horizontal shift per frame
    float weave = sin(u_time * 12.0) * 0.008 * u_gate;
    float weave_y = cos(u_time * 7.3) * 0.005 * u_gate;
    vec2 uv = v_uv + vec2(weave, weave_y);
    vec4 col = texture(u_tex, clamp(uv, 0.0, 1.0));
    // Warm color fade (kodachrome-ish)
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    vec3 warm = col.rgb * vec3(1.1, 0.98, 0.8) + vec3(u_fade*0.12, u_fade*0.05, 0.0);
    // Lift shadows (fade blacks)
    warm = mix(warm, warm + vec3(0.08, 0.06, 0.04) * u_fade, smoothstep(0.3, 0.0, lum));
    // Grain
    float grain = (hash(uv * 800.0 + fract(u_time * 24.0)) - 0.5) * u_grain * 0.12;
    warm += grain;
    // Sprocket hole vignette (narrow frame)
    float frame_v = smoothstep(0.0, 0.04, v_uv.y) * smoothstep(1.0, 0.96, v_uv.y);
    float frame_h = smoothstep(0.0, 0.03, v_uv.x) * smoothstep(1.0, 0.97, v_uv.x);
    warm *= frame_v * frame_h;
    frag = vec4(clamp(warm, 0.0, 1.0), col.a);
}
""",

"vhs_dropout": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_h;
uniform float u_density;
uniform float u_strength;
uniform float u_speed;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec4 col = texture(u_tex, v_uv);
    // Scanline-based dropout
    float scan = floor(v_uv.y * u_tex_h);
    float frame = floor(u_time * u_speed * 15.0);
    float drop_r = hash(vec2(scan, frame));
    float drop_r2 = hash(vec2(scan * 0.5, frame + 1.0));
    // Dropout bands (white signal loss)
    float dropout = step(1.0 - u_density, drop_r) * u_strength;
    vec3 result = mix(col.rgb, vec3(1.0), dropout);
    // Chroma shift on dropout lines
    if (dropout > 0.0) {
        float shift = (drop_r2 - 0.5) * 0.04;
        result.r = texture(u_tex, clamp(v_uv + vec2(shift, 0.0), 0.0, 1.0)).r;
        result.b = texture(u_tex, clamp(v_uv - vec2(shift, 0.0), 0.0, 1.0)).b;
    }
    frag = vec4(result, col.a);
}
""",

"x_ray": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_contrast;
uniform float u_blue_tint;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Invert
    float inv = 1.0 - lum;
    // Contrast push
    inv = clamp((inv - 0.5) * u_contrast + 0.5, 0.0, 1.0);
    // Blue-white x-ray tinting: shadows blue-grey, highlights white
    vec3 xray = mix(
        vec3(0.55, 0.65, 0.85),   // shadow blue
        vec3(0.95, 0.97, 1.0),    // highlight white
        inv
    );
    xray = mix(vec3(inv), xray, u_blue_tint);
    frag = vec4(clamp(xray, 0.0, 1.0), col.a);
}
""",

"bit_crush": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_levels;
uniform float u_dither;
void main() {
    vec4 col = texture(u_tex, v_uv);
    // 4x4 Bayer matrix for dithering
    float bayer[16];
    bayer[0]= 0.0/16.0; bayer[1]= 8.0/16.0; bayer[2]= 2.0/16.0; bayer[3]=10.0/16.0;
    bayer[4]=12.0/16.0; bayer[5]= 4.0/16.0; bayer[6]=14.0/16.0; bayer[7]= 6.0/16.0;
    bayer[8]= 3.0/16.0; bayer[9]=11.0/16.0;bayer[10]= 1.0/16.0;bayer[11]= 9.0/16.0;
    bayer[12]=15.0/16.0;bayer[13]= 7.0/16.0;bayer[14]=13.0/16.0;bayer[15]= 5.0/16.0;
    int bx = int(mod(v_uv.x * u_tex_w, 4.0));
    int by = int(mod(v_uv.y * u_tex_h, 4.0));
    float threshold = bayer[by * 4 + bx] - 0.5;
    float step_size = 1.0 / max(u_levels - 1.0, 1.0);
    vec3 dithered = col.rgb + threshold * step_size * u_dither;
    vec3 crushed = floor(dithered / step_size + 0.5) * step_size;
    frag = vec4(clamp(crushed, 0.0, 1.0), col.a);
}
""",

"tv_static": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_amount;
uniform float u_color_mix;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec4 col = texture(u_tex, v_uv);
    // Animated static noise
    vec2 npx = floor(v_uv * vec2(u_tex_w, u_tex_h) / 2.0) * 2.0;
    float n = hash(npx + fract(vec2(u_time * 37.4, u_time * 23.1)));
    float n2 = hash(npx * 0.5 + fract(vec2(u_time * 11.7, u_time * 41.3)));
    // Mix grey and colored static
    vec3 grey_static = vec3(n);
    vec3 color_static = vec3(n, n2, hash(npx + 50.0 + fract(u_time * 19.3)));
    vec3 static_col = mix(grey_static, color_static, u_color_mix);
    // Blend static over image
    vec3 result = mix(col.rgb, static_col, u_amount);
    // Add horizontal roll bar occasionally
    float roll = fract(u_time * 0.08);
    float bar = smoothstep(0.02, 0.0, abs(v_uv.y - roll)) * 0.3;
    result = mix(result, vec3(1.0), bar);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
""",

"dither_bayer": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_levels;
uniform float u_scale;
uniform float u_color;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // 8x8 Bayer threshold matrix (normalized)
    float bayer8[64];
    bayer8[ 0]= 0.0; bayer8[ 1]=32.0; bayer8[ 2]= 8.0; bayer8[ 3]=40.0;
    bayer8[ 4]= 2.0; bayer8[ 5]=34.0; bayer8[ 6]=10.0; bayer8[ 7]=42.0;
    bayer8[ 8]=48.0; bayer8[ 9]=16.0; bayer8[10]=56.0; bayer8[11]=24.0;
    bayer8[12]=50.0; bayer8[13]=18.0; bayer8[14]=58.0; bayer8[15]=26.0;
    bayer8[16]=12.0; bayer8[17]=44.0; bayer8[18]= 4.0; bayer8[19]=36.0;
    bayer8[20]=14.0; bayer8[21]=46.0; bayer8[22]= 6.0; bayer8[23]=38.0;
    bayer8[24]=60.0; bayer8[25]=28.0; bayer8[26]=52.0; bayer8[27]=20.0;
    bayer8[28]=62.0; bayer8[29]=30.0; bayer8[30]=54.0; bayer8[31]=22.0;
    bayer8[32]= 3.0; bayer8[33]=35.0; bayer8[34]=11.0; bayer8[35]=43.0;
    bayer8[36]= 1.0; bayer8[37]=33.0; bayer8[38]= 9.0; bayer8[39]=41.0;
    bayer8[40]=51.0; bayer8[41]=19.0; bayer8[42]=59.0; bayer8[43]=27.0;
    bayer8[44]=49.0; bayer8[45]=17.0; bayer8[46]=57.0; bayer8[47]=25.0;
    bayer8[48]=15.0; bayer8[49]=47.0; bayer8[50]= 7.0; bayer8[51]=39.0;
    bayer8[52]=13.0; bayer8[53]=45.0; bayer8[54]= 5.0; bayer8[55]=37.0;
    bayer8[56]=63.0; bayer8[57]=31.0; bayer8[58]=55.0; bayer8[59]=23.0;
    bayer8[60]=61.0; bayer8[61]=29.0; bayer8[62]=53.0; bayer8[63]=21.0;
    int px = int(mod(v_uv.x * u_tex_w / u_scale, 8.0));
    int py = int(mod(v_uv.y * u_tex_h / u_scale, 8.0));
    float threshold = bayer8[py * 8 + px] / 64.0 - 0.5;
    float step_sz = 1.0 / max(u_levels - 1.0, 1.0);
    float dith_lum = lum + threshold * step_sz;
    float quant = floor(dith_lum / step_sz + 0.5) * step_sz;
    // Color dither or monochrome
    vec3 result = mix(vec3(quant), col.rgb * quant / max(lum, 0.001), u_color);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
""",

"miami_vice": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_strength;
uniform float u_saturation;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Boost saturation aggressively
    vec3 sat = mix(vec3(lum), col.rgb, u_saturation);
    // Remap hues toward pink/magenta and teal
    // Hot pink shadows, teal highlights
    float shadow = smoothstep(0.5, 0.0, lum);
    float hi = smoothstep(0.5, 1.0, lum);
    vec3 pink_tint = vec3(1.0, 0.2, 0.6);
    vec3 teal_tint = vec3(0.1, 0.9, 0.8);
    vec3 result = sat
                + pink_tint * shadow * u_strength * 0.4
                + teal_tint * hi * u_strength * 0.3;
    // Slight contrast pump
    result = clamp((result - 0.5) * 1.15 + 0.5, 0.0, 1.0);
    frag = vec4(result, col.a);
}
""",

"horror_grade": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_desat;
uniform float u_red;
uniform float u_crush;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Desaturate
    vec3 grey = mix(col.rgb, vec3(lum), u_desat);
    // Red channel boost (blood, danger)
    grey.r = clamp(grey.r + u_red * 0.35, 0.0, 1.0);
    grey.g = clamp(grey.g - u_red * 0.1, 0.0, 1.0);
    grey.b = clamp(grey.b - u_red * 0.15, 0.0, 1.0);
    // Crush shadows to pure black
    grey = max(grey - vec3(u_crush), vec3(0.0)) / (1.0 - u_crush);
    // Slight green-shift in midtones (sickly)
    float mid = smoothstep(0.2, 0.7, lum) * (1.0 - smoothstep(0.7, 1.0, lum));
    grey.g += mid * 0.04;
    frag = vec4(clamp(grey, 0.0, 1.0), col.a);
}
""",

"split_toning": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_shadow_hue;
uniform float u_hi_hue;
uniform float u_strength;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Convert hue to color
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 ps = abs(fract(u_shadow_hue + K.xyz) * 6.0 - K.www);
    vec3 ph = abs(fract(u_hi_hue     + K.xyz) * 6.0 - K.www);
    vec3 shadow_col = clamp(ps - K.xxx, 0.0, 1.0);
    vec3 hi_col     = clamp(ph - K.xxx, 0.0, 1.0);
    // Blend by luminance zone
    float shadow_wt = smoothstep(0.5, 0.0, lum);
    float hi_wt     = smoothstep(0.5, 1.0, lum);
    vec3 tone = col.rgb
              + shadow_col * shadow_wt * u_strength * 0.4
              - (1.0 - shadow_col) * shadow_wt * u_strength * 0.1
              + hi_col * hi_wt * u_strength * 0.35;
    frag = vec4(clamp(tone, 0.0, 1.0), col.a);
}
""",

"desert_gold": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_warmth;
uniform float u_fade;
uniform float u_haze;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Warm golden push
    vec3 warm = col.rgb * vec3(1.0 + u_warmth*0.3, 1.0 + u_warmth*0.1, 1.0 - u_warmth*0.3);
    // Lift shadows (sun-bleached fade)
    float lift = u_fade * 0.18;
    warm = warm * (1.0 - lift) + lift;
    // Haze: push toward warm white (atmospheric scattering)
    vec3 haze_col = vec3(1.0, 0.92, 0.75);
    warm = mix(warm, haze_col, u_haze * smoothstep(0.4, 1.0, lum) * 0.5);
    // Slight orange cast in highlights
    warm.r = min(warm.r + u_warmth * 0.08, 1.0);
    frag = vec4(clamp(warm, 0.0, 1.0), col.a);
}
""",

"emboss_relief": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_strength;
uniform float u_angle;
uniform float u_colorize;
void main() {
    const float DEG2RAD = 0.017453293;
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    float a = u_angle * DEG2RAD;
    vec2 light = vec2(cos(a), sin(a));
    // Sample in light direction and opposite
    vec3 fwd = texture(u_tex, clamp(v_uv + light * px * u_strength, 0.0, 1.0)).rgb;
    vec3 bwd = texture(u_tex, clamp(v_uv - light * px * u_strength, 0.0, 1.0)).rgb;
    vec3 orig = texture(u_tex, v_uv).rgb;
    float lum_fwd = dot(fwd, vec3(0.299, 0.587, 0.114));
    float lum_bwd = dot(bwd, vec3(0.299, 0.587, 0.114));
    // Emboss = difference gives raised surface effect
    float bump = (lum_fwd - lum_bwd) * 0.5 + 0.5;
    vec3 relief = vec3(bump);
    // Optional color preserve
    float lum_orig = dot(orig, vec3(0.299, 0.587, 0.114));
    vec3 colored = mix(vec3(bump), orig * (bump / max(lum_orig, 0.001)), u_colorize);
    frag = vec4(clamp(colored, 0.0, 1.0), 1.0);
}
""",

"pointillist": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_dot_size;
uniform float u_scatter;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    float sz = u_dot_size;
    // Find the cell for this pixel
    vec2 cell_uv = v_uv / (px * sz);
    vec2 cell_id = floor(cell_uv);
    vec3 result = vec3(0.95); // paper white
    float min_dist = 1e9;
    vec3 nearest_col = vec3(0.5);
    // Check 9 neighboring cells
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            vec2 nb = cell_id + vec2(float(dx), float(dy));
            // Jitter the dot center within the cell
            vec2 jitter = vec2(hash(nb), hash(nb + vec2(31.7, 71.3)));
            vec2 dot_ctr = (nb + 0.5 + (jitter - 0.5) * u_scatter) * px * sz;
            float d = length(v_uv - dot_ctr);
            if (d < min_dist) {
                min_dist = d;
                // Sample image at the dot center
                vec3 col = texture(u_tex, clamp(dot_ctr, 0.0, 1.0)).rgb;
                float lum = dot(col, vec3(0.299, 0.587, 0.114));
                // Dot radius proportional to luminance (dark = big dot)
                float r = (1.0 - lum * 0.7) * px.x * sz * 0.55;
                nearest_col = (d < r) ? col : vec3(0.95);
            }
        }
    }
    // Re-check with actual dot radius
    float lum_c = dot(nearest_col, vec3(0.299, 0.587, 0.114));
    vec2 best_center = (floor(v_uv / (px * sz)) + 0.5) * px * sz;
    vec3 cell_color = texture(u_tex, clamp(best_center, 0.0, 1.0)).rgb;
    float cell_lum = dot(cell_color, vec3(0.299, 0.587, 0.114));
    float r = (1.0 - cell_lum * 0.7) * px.x * sz * 0.55;
    vec2 local = v_uv - best_center;
    result = (length(local) < r) ? cell_color : vec3(0.95);
    frag = vec4(clamp(result, 0.0, 1.0), 1.0);
}
""",

"interlace_glitch": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_h;
uniform float u_strength;
uniform float u_intensity;
uniform float u_speed;
uniform float u_time;
float hash(float n) { return fract(sin(n) * 43758.5453); }
void main() {
    float line = floor(v_uv.y * u_tex_h);
    float field = mod(line, 2.0);
    // Animate which lines glitch
    float t = floor(u_time * u_speed * 8.0);
    float glitch = hash(line * 0.1 + t);
    float shift = (field * 2.0 - 1.0) * u_strength * step(1.0 - u_intensity * 0.3, glitch);
    vec2 uv = clamp(v_uv + vec2(shift, 0.0), 0.0, 1.0);
    vec4 col = texture(u_tex, uv);
    // Alternate field brightness difference
    float bright = 1.0 + (field - 0.5) * 0.06 * u_intensity;
    frag = vec4(clamp(col.rgb * bright, 0.0, 1.0), col.a);
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
