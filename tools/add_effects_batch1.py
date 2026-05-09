#!/usr/bin/env python3
"""Add batch 1: 20 effects — warp/distortion + light/optical."""
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
        "id": "twirl", "enum": "Twirl", "label": "Twirl",
        "description": "Vortex rotation spiraling from center",
        "abbrev": "TWL", "color": [180, 60, 230, 220],
        "shader": "shaders/twirl.glsl",
        "params": [
            {"name": "strength", "label": "Strength", "default": 3.5, "min": 0.0, "max": 12.0, "fmt": "%.1f"},
            {"name": "radius",   "label": "Radius",   "default": 0.55,"min": 0.1, "max": 1.0,  "fmt": "%.2f"}
        ]
    },
    {
        "id": "ripple", "enum": "Ripple", "label": "Ripple",
        "description": "Expanding ring waves from center",
        "abbrev": "RPL", "color": [40, 160, 220, 220],
        "shader": "shaders/ripple.glsl",
        "params": [
            {"name": "frequency", "label": "Frequency", "default": 18.0, "min": 2.0, "max": 40.0, "fmt": "%.0f"},
            {"name": "amplitude", "label": "Amplitude", "default": 0.035,"min": 0.0, "max": 0.1,  "fmt": "%.3f"},
            {"name": "speed",     "label": "Speed",     "default": 2.0,  "min": 0.0, "max": 6.0,  "fmt": "%.1f"}
        ]
    },
    {
        "id": "wave_warp", "enum": "WaveWarp", "label": "Wave Warp",
        "description": "Perpendicular sine-wave displacement field",
        "abbrev": "WWP", "color": [60, 200, 160, 220],
        "shader": "shaders/wave_warp.glsl",
        "params": [
            {"name": "freq_x",    "label": "Freq X",    "default": 8.0,  "min": 1.0, "max": 30.0, "fmt": "%.0f"},
            {"name": "freq_y",    "label": "Freq Y",    "default": 6.0,  "min": 1.0, "max": 30.0, "fmt": "%.0f"},
            {"name": "amplitude", "label": "Amplitude", "default": 0.04, "min": 0.0, "max": 0.12, "fmt": "%.3f"},
            {"name": "speed",     "label": "Speed",     "default": 1.5,  "min": 0.0, "max": 5.0,  "fmt": "%.1f"}
        ]
    },
    {
        "id": "kaleidoscope", "enum": "Kaleidoscope", "label": "Kaleidoscope",
        "description": "N-way radial mirror symmetry",
        "abbrev": "KAL", "color": [230, 80, 150, 220],
        "shader": "shaders/kaleidoscope.glsl",
        "params": [
            {"name": "segments", "label": "Segments", "default": 6.0,  "min": 2.0, "max": 16.0, "fmt": "%.0f"},
            {"name": "rotation", "label": "Rotation", "default": 0.0,  "min": 0.0, "max": 6.28, "fmt": "%.2f"},
            {"name": "zoom",     "label": "Zoom",     "default": 1.0,  "min": 0.5, "max": 3.0,  "fmt": "%.1f"}
        ]
    },
    {
        "id": "zoom_blur_rad", "enum": "ZoomBlurRad", "label": "Zoom Streak",
        "description": "Radial motion blur emanating from focal point",
        "abbrev": "ZST", "color": [200, 220, 40, 220],
        "shader": "shaders/zoom_blur_rad.glsl",
        "params": [
            {"name": "amount", "label": "Amount", "default": 0.12, "min": 0.0, "max": 0.35, "fmt": "%.2f"},
            {"name": "cx",     "label": "Center X","default": 0.5, "min": 0.0, "max": 1.0,  "fmt": "%.2f"},
            {"name": "cy",     "label": "Center Y","default": 0.5, "min": 0.0, "max": 1.0,  "fmt": "%.2f"}
        ]
    },
    {
        "id": "spin_blur", "enum": "SpinBlur", "label": "Spin Blur",
        "description": "Rotational motion blur around center",
        "abbrev": "SPB", "color": [160, 230, 80, 220],
        "shader": "shaders/spin_blur.glsl",
        "params": [
            {"name": "angle", "label": "Angle", "default": 0.08, "min": 0.0, "max": 0.3, "fmt": "%.3f"}
        ]
    },
    {
        "id": "heat_haze", "enum": "HeatHaze", "label": "Heat Haze",
        "description": "Shimmering atmospheric heat distortion",
        "abbrev": "HHZ", "color": [230, 160, 40, 220],
        "shader": "shaders/heat_haze.glsl",
        "params": [
            {"name": "intensity", "label": "Intensity", "default": 0.7, "min": 0.0, "max": 2.0, "fmt": "%.1f"},
            {"name": "speed",     "label": "Speed",     "default": 1.5, "min": 0.0, "max": 4.0, "fmt": "%.1f"}
        ]
    },
    {
        "id": "barrel_warp", "enum": "BarrelWarp", "label": "Barrel Warp",
        "description": "Strong barrel / pin-cushion lens distortion",
        "abbrev": "BWP", "color": [100, 200, 230, 220],
        "shader": "shaders/barrel_warp.glsl",
        "params": [
            {"name": "k1",    "label": "Barrel",  "default":  0.4, "min": -1.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "k2",    "label": "Pincush", "default":  0.1, "min": -0.5, "max": 0.5, "fmt": "%.2f"},
            {"name": "scale", "label": "Scale",   "default":  0.9, "min":  0.5, "max": 1.0, "fmt": "%.2f"}
        ]
    },
    {
        "id": "anamorphic_streak", "enum": "AnamorphicStreak", "label": "Anamorphic Flare",
        "description": "Horizontal cyan lens streak flares on bright highlights",
        "abbrev": "ANF", "color": [80, 200, 255, 220],
        "shader": "shaders/anamorphic_streak.glsl",
        "params": [
            {"name": "threshold", "label": "Threshold", "default": 0.7,  "min": 0.3, "max": 1.0, "fmt": "%.2f"},
            {"name": "length",    "label": "Length",    "default": 0.45, "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "intensity", "label": "Intensity", "default": 1.2,  "min": 0.0, "max": 3.0, "fmt": "%.1f"}
        ]
    },
    {
        "id": "starburst_spike", "enum": "StarburstSpike", "label": "Starburst",
        "description": "Star diffraction spikes on bright light sources",
        "abbrev": "STB", "color": [255, 230, 80, 220],
        "shader": "shaders/starburst_spike.glsl",
        "params": [
            {"name": "threshold", "label": "Threshold", "default": 0.65, "min": 0.3, "max": 1.0, "fmt": "%.2f"},
            {"name": "length",    "label": "Length",    "default": 0.25, "min": 0.0, "max": 0.6, "fmt": "%.2f"},
            {"name": "rays",      "label": "Rays",      "default": 6.0,  "min": 4.0, "max": 12.0,"fmt": "%.0f"}
        ]
    },
    {
        "id": "god_rays", "enum": "GodRays", "label": "God Rays",
        "description": "Volumetric crepuscular light shafts from highlights",
        "abbrev": "GDR", "color": [255, 200, 80, 220],
        "shader": "shaders/god_rays.glsl",
        "params": [
            {"name": "intensity", "label": "Intensity", "default": 0.9, "min": 0.0, "max": 2.0, "fmt": "%.1f"},
            {"name": "decay",     "label": "Decay",     "default": 0.94,"min": 0.7, "max": 1.0, "fmt": "%.2f"},
            {"name": "cx",        "label": "Source X",  "default": 0.5, "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "cy",        "label": "Source Y",  "default": 0.15,"min": 0.0, "max": 1.0, "fmt": "%.2f"}
        ]
    },
    {
        "id": "aurora_borealis", "enum": "AuroraBorealis", "label": "Aurora",
        "description": "Animated aurora borealis color curtains",
        "abbrev": "AUR", "color": [40, 230, 180, 220],
        "shader": "shaders/aurora_borealis.glsl",
        "params": [
            {"name": "intensity",    "label": "Intensity",   "default": 0.7, "min": 0.0, "max": 2.0, "fmt": "%.1f"},
            {"name": "speed",        "label": "Speed",       "default": 0.4, "min": 0.0, "max": 2.0, "fmt": "%.1f"},
            {"name": "color_shift",  "label": "Color Shift", "default": 0.3, "min": 0.0, "max": 1.0, "fmt": "%.2f"}
        ]
    },
    {
        "id": "bokeh_dream", "enum": "BokehDream", "label": "Bokeh Dream",
        "description": "Out-of-focus dreamy bokeh highlight circles",
        "abbrev": "BKH", "color": [200, 120, 230, 220],
        "shader": "shaders/bokeh_dream.glsl",
        "params": [
            {"name": "radius",    "label": "Radius",    "default": 0.025, "min": 0.0, "max": 0.08, "fmt": "%.3f"},
            {"name": "threshold", "label": "Threshold", "default": 0.55,  "min": 0.0, "max": 1.0,  "fmt": "%.2f"},
            {"name": "intensity", "label": "Intensity", "default": 1.5,   "min": 0.0, "max": 4.0,  "fmt": "%.1f"}
        ]
    },
    {
        "id": "prism_disperse", "enum": "PrismDisperse", "label": "Prism",
        "description": "Prismatic rainbow dispersion at edges",
        "abbrev": "PRM", "color": [255, 100, 200, 220],
        "shader": "shaders/prism_disperse.glsl",
        "params": [
            {"name": "spread",    "label": "Spread",    "default": 0.06, "min": 0.0, "max": 0.2, "fmt": "%.2f"},
            {"name": "intensity", "label": "Intensity", "default": 1.0,  "min": 0.0, "max": 2.0, "fmt": "%.1f"}
        ]
    },
    {
        "id": "film_burn", "enum": "FilmBurn", "label": "Film Burn",
        "description": "Organic burning film edge degradation",
        "abbrev": "FBN", "color": [230, 100, 30, 220],
        "shader": "shaders/film_burn.glsl",
        "params": [
            {"name": "intensity", "label": "Intensity", "default": 0.8, "min": 0.0, "max": 2.0, "fmt": "%.1f"},
            {"name": "speed",     "label": "Speed",     "default": 1.2, "min": 0.0, "max": 3.0, "fmt": "%.1f"},
            {"name": "edge",      "label": "Edge",      "default": 0.25,"min": 0.0, "max": 0.6, "fmt": "%.2f"}
        ]
    },
    {
        "id": "oil_paint", "enum": "OilPaint", "label": "Oil Paint",
        "description": "Kuwahara painterly filter — sharp but smooth like oil",
        "abbrev": "OIL", "color": [180, 120, 60, 220],
        "shader": "shaders/oil_paint.glsl",
        "params": [
            {"name": "radius",    "label": "Brush",    "default": 4.0, "min": 1.0, "max": 8.0, "fmt": "%.0f"},
            {"name": "sharpness", "label": "Sharpness","default": 8.0, "min": 1.0, "max": 20.0,"fmt": "%.0f"}
        ]
    },
    {
        "id": "stained_glass", "enum": "StainedGlass", "label": "Stained Glass",
        "description": "Voronoi-cell stained glass with dark lead borders",
        "abbrev": "SGS", "color": [80, 180, 230, 220],
        "shader": "shaders/stained_glass.glsl",
        "params": [
            {"name": "cell_size",   "label": "Cell Size",  "default": 18.0, "min": 4.0, "max": 48.0, "fmt": "%.0f"},
            {"name": "border",      "label": "Border",     "default": 0.08, "min": 0.0, "max": 0.3,  "fmt": "%.2f"},
            {"name": "saturation",  "label": "Saturation", "default": 1.6,  "min": 0.5, "max": 3.0,  "fmt": "%.1f"}
        ]
    },
    {
        "id": "neon_edge_glow", "enum": "NeonEdgeGlow", "label": "Neon Edges",
        "description": "Sobel edge detection with colored neon glow",
        "abbrev": "NEG", "color": [255, 50, 200, 220],
        "shader": "shaders/neon_edge_glow.glsl",
        "params": [
            {"name": "threshold", "label": "Threshold", "default": 0.15, "min": 0.0, "max": 0.5, "fmt": "%.2f"},
            {"name": "glow",      "label": "Glow",      "default": 0.8,  "min": 0.0, "max": 2.0, "fmt": "%.1f"},
            {"name": "hue",       "label": "Hue",       "default": 0.55, "min": 0.0, "max": 1.0, "fmt": "%.2f"}
        ]
    },
    {
        "id": "cyberpunk_grade", "enum": "CyberpunkGrade", "label": "Cyberpunk",
        "description": "Deep blue-teal shadows with cyan/orange highlights",
        "abbrev": "CPK", "color": [40, 200, 230, 220],
        "shader": "shaders/cyberpunk_grade.glsl",
        "params": [
            {"name": "shadow_teal", "label": "Shadow Teal", "default": 0.6, "min": 0.0, "max": 1.0, "fmt": "%.1f"},
            {"name": "hi_orange",   "label": "Hi Orange",   "default": 0.5, "min": 0.0, "max": 1.0, "fmt": "%.1f"},
            {"name": "contrast",    "label": "Contrast",    "default": 1.3, "min": 1.0, "max": 2.5, "fmt": "%.1f"}
        ]
    },
    {
        "id": "matrix_rain", "enum": "MatrixRain", "label": "Matrix",
        "description": "Digital matrix green rain overlay",
        "abbrev": "MTX", "color": [30, 200, 60, 220],
        "shader": "shaders/matrix_rain.glsl",
        "params": [
            {"name": "density",    "label": "Density",    "default": 0.45, "min": 0.0, "max": 1.0, "fmt": "%.2f"},
            {"name": "speed",      "label": "Speed",      "default": 2.0,  "min": 0.5, "max": 6.0, "fmt": "%.1f"},
            {"name": "green_mix",  "label": "Mix",        "default": 0.55, "min": 0.0, "max": 1.0, "fmt": "%.2f"}
        ]
    },
]

# ── Shader sources ────────────────────────────────────────────────────────────

SHADERS = {

"twirl": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_strength;
uniform float u_radius;
void main() {
    vec2 c = vec2(0.5, 0.5);
    vec2 d = v_uv - c;
    float dist = length(d);
    float angle = u_strength * smoothstep(u_radius, 0.0, dist);
    float cs = cos(angle), sn = sin(angle);
    vec2 rot = vec2(cs*d.x - sn*d.y, sn*d.x + cs*d.y);
    frag = texture(u_tex, clamp(rot + c, 0.0, 1.0));
}
""",

"ripple": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_frequency;
uniform float u_amplitude;
uniform float u_speed;
uniform float u_time;
void main() {
    vec2 c = vec2(0.5, 0.5);
    vec2 d = v_uv - c;
    float dist = length(d) + 0.001;
    float wave = sin(dist * u_frequency - u_time * u_speed) * u_amplitude;
    vec2 uv = clamp(v_uv + normalize(d) * wave, 0.0, 1.0);
    frag = texture(u_tex, uv);
}
""",

"wave_warp": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_freq_x;
uniform float u_freq_y;
uniform float u_amplitude;
uniform float u_speed;
uniform float u_time;
void main() {
    vec2 uv = v_uv;
    uv.x += sin(v_uv.y * u_freq_x + u_time * u_speed * 1.1) * u_amplitude;
    uv.y += sin(v_uv.x * u_freq_y + u_time * u_speed * 0.9) * u_amplitude;
    frag = texture(u_tex, clamp(uv, 0.0, 1.0));
}
""",

"kaleidoscope": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_segments;
uniform float u_rotation;
uniform float u_zoom;
void main() {
    const float PI = 3.14159265;
    vec2 c = vec2(0.5, 0.5);
    vec2 d = (v_uv - c) / u_zoom;
    float angle = atan(d.y, d.x) + u_rotation;
    float radius = length(d);
    float sector = PI * 2.0 / max(u_segments, 2.0);
    angle = mod(angle, sector);
    if (angle > sector * 0.5) angle = sector - angle;
    vec2 uv = c + vec2(cos(angle), sin(angle)) * radius;
    frag = texture(u_tex, clamp(uv, 0.0, 1.0));
}
""",

"zoom_blur_rad": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_amount;
uniform float u_cx;
uniform float u_cy;
void main() {
    vec2 focus = vec2(u_cx, u_cy);
    vec4 acc = vec4(0.0);
    const int S = 14;
    for (int i = 0; i < S; i++) {
        float t = float(i) / float(S - 1);
        float scale = 1.0 - u_amount * t;
        vec2 uv = focus + (v_uv - focus) * scale;
        float w = 1.0 - t * 0.5;
        acc += texture(u_tex, clamp(uv, 0.0, 1.0)) * w;
    }
    frag = acc / acc.a;
}
""",

"spin_blur": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_angle;
void main() {
    vec2 c = vec2(0.5, 0.5);
    vec4 acc = vec4(0.0);
    const int S = 14;
    for (int i = 0; i < S; i++) {
        float a = u_angle * (float(i) / float(S-1) - 0.5);
        float cs = cos(a), sn = sin(a);
        vec2 d = v_uv - c;
        vec2 rot = vec2(cs*d.x - sn*d.y, sn*d.x + cs*d.y) + c;
        acc += texture(u_tex, clamp(rot, 0.0, 1.0));
    }
    frag = acc / float(S);
}
""",

"heat_haze": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_intensity;
uniform float u_speed;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
float noise2(vec2 p) {
    vec2 i = floor(p); vec2 f = fract(p); f = f*f*(3.0-2.0*f);
    return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);
}
void main() {
    float t = u_time * u_speed;
    float nx = noise2(vec2(v_uv.x*5.0, v_uv.y*10.0 + t));
    float ny = noise2(vec2(v_uv.x*5.0 + 100.0, v_uv.y*10.0 + t*1.3));
    float rise = smoothstep(0.0, 0.7, 1.0 - v_uv.y);
    vec2 warp = vec2(nx-0.5, ny-0.5) * u_intensity * 0.05 * rise;
    frag = texture(u_tex, clamp(v_uv + warp, 0.0, 1.0));
}
""",

"barrel_warp": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_k1;
uniform float u_k2;
uniform float u_scale;
void main() {
    vec2 d = (v_uv - 0.5) / u_scale;
    float r2 = dot(d, d);
    float distort = 1.0 + u_k1 * r2 + u_k2 * r2 * r2;
    vec2 uv = d * distort + 0.5;
    frag = texture(u_tex, clamp(uv, 0.0, 1.0));
}
""",

"anamorphic_streak": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_threshold;
uniform float u_length;
uniform float u_intensity;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float px = 1.0 / u_tex_w;
    vec3 streak = vec3(0.0);
    float total = 0.001;
    // Gather horizontal streak from bright neighbours
    for (int i = 1; i <= 80; i++) {
        float off = float(i) * px;
        if (off > u_length) break;
        float wt = exp(-off / u_length * 5.0);
        vec3 r = texture(u_tex, clamp(vec2(v_uv.x + off, v_uv.y), 0.0, 1.0)).rgb;
        vec3 l = texture(u_tex, clamp(vec2(v_uv.x - off, v_uv.y), 0.0, 1.0)).rgb;
        float br = max(r.r,max(r.g,r.b));
        float bl = max(l.r,max(l.g,l.b));
        float mr = step(u_threshold, br);
        float ml = step(u_threshold, bl);
        streak += (r*mr + l*ml) * wt;
        total  += (mr + ml) * wt;
    }
    streak /= total;
    // Cyan-tinted horizontal anamorphic streak
    vec3 anam = streak * vec3(0.3, 0.7, 1.5) * u_intensity;
    frag = vec4(clamp(col.rgb + anam, 0.0, 1.5), col.a);
}
""",

"starburst_spike": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_threshold;
uniform float u_length;
uniform float u_rays;
void main() {
    const float PI = 3.14159265;
    vec4 col = texture(u_tex, v_uv);
    vec2 ipx = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    int N = int(u_rays);
    vec3 burst = vec3(0.0);
    for (int r = 0; r < N; r++) {
        float ang = PI * float(r) / float(N);
        vec2 dir = vec2(cos(ang)*ipx.x, sin(ang)*ipx.y);
        for (int s = 1; s <= 40; s++) {
            float t = float(s) / 40.0;
            if (t > u_length * 4.0) break;
            float wt = (1.0 - t) * exp(-t * 3.0);
            vec2 uv_a = v_uv + dir * float(s) * 28.0 * u_length;
            vec2 uv_b = v_uv - dir * float(s) * 28.0 * u_length;
            vec3 ca = texture(u_tex, clamp(uv_a, 0.0, 1.0)).rgb;
            vec3 cb = texture(u_tex, clamp(uv_b, 0.0, 1.0)).rgb;
            float ba = dot(ca, vec3(0.2126,0.7152,0.0722));
            float bb = dot(cb, vec3(0.2126,0.7152,0.0722));
            burst += (ca * step(u_threshold, ba) + cb * step(u_threshold, bb)) * wt;
        }
    }
    burst /= float(N) * 2.5;
    frag = vec4(clamp(col.rgb + burst, 0.0, 1.0), col.a);
}
""",

"god_rays": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_intensity;
uniform float u_decay;
uniform float u_cx;
uniform float u_cy;
void main() {
    vec4 col = texture(u_tex, v_uv);
    vec2 src = vec2(u_cx, u_cy);
    vec2 delta = (v_uv - src) / 16.0;
    vec2 uv = v_uv;
    vec3 rays = vec3(0.0);
    float illum = 1.0;
    for (int i = 0; i < 16; i++) {
        uv -= delta;
        vec3 s = texture(u_tex, clamp(uv, 0.0, 1.0)).rgb;
        float bright = dot(s, vec3(0.2126, 0.7152, 0.0722));
        rays += s * max(bright - 0.3, 0.0) * illum;
        illum *= u_decay;
    }
    rays /= 16.0;
    frag = vec4(clamp(col.rgb + rays * u_intensity, 0.0, 1.0), col.a);
}
""",

"aurora_borealis": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_intensity;
uniform float u_speed;
uniform float u_color_shift;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
float noise2(vec2 p) {
    vec2 i = floor(p); vec2 f = fract(p); f = f*f*(3.0-2.0*f);
    return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);
}
vec3 hue2rgb(float h) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(h + K.xyz) * 6.0 - K.www);
    return clamp(p - K.xxx, 0.0, 1.0);
}
void main() {
    vec4 col = texture(u_tex, v_uv);
    float t = u_time * u_speed;
    // Aurora curtains: vertical bands of color
    float n1 = noise2(vec2(v_uv.x * 3.0 + t * 0.3, t * 0.1));
    float n2 = noise2(vec2(v_uv.x * 5.0 - t * 0.2, t * 0.15 + 10.0));
    // Curtain falls from top, fades at bottom
    float curtain = smoothstep(0.7, 0.1, v_uv.y) * smoothstep(0.0, 0.3, 1.0 - v_uv.y);
    float wave = sin(v_uv.x * 8.0 + t * 0.5 + n1 * 4.0) * 0.5 + 0.5;
    float aurora_mask = wave * curtain * (n1 * 0.7 + 0.3);
    float hue = fract(u_color_shift + v_uv.x * 0.4 + n2 * 0.3 + t * 0.05);
    vec3 aurora_col = hue2rgb(hue) * vec3(0.5, 1.0, 0.8); // bias toward green/teal
    // Screen blend: aurora brightens without darkening
    vec3 screen = 1.0 - (1.0 - col.rgb) * (1.0 - aurora_col * aurora_mask * u_intensity);
    frag = vec4(clamp(screen, 0.0, 1.0), col.a);
}
""",

"bokeh_dream": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_radius;
uniform float u_threshold;
uniform float u_intensity;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float ar = u_tex_w / u_tex_h;
    vec3 bokeh = vec3(0.0);
    float total = 0.001;
    // Circular gather — bright pixels become bokeh discs
    int steps = 12;
    for (int a = 0; a < steps; a++) {
        float ang = 6.28318 * float(a) / float(steps);
        for (int r = 1; r <= 6; r++) {
            float rad = float(r) / 6.0 * u_radius;
            vec2 uv = v_uv + vec2(cos(ang) / ar, sin(ang)) * rad;
            vec3 s = texture(u_tex, clamp(uv, 0.0, 1.0)).rgb;
            float bright = dot(s, vec3(0.2126,0.7152,0.0722));
            float wt = step(u_threshold, bright) * (1.0 - float(r)/7.0);
            bokeh += s * wt;
            total += wt;
        }
    }
    bokeh /= total;
    vec3 result = 1.0 - (1.0 - col.rgb) * (1.0 - bokeh * u_intensity * 0.5);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
""",

"prism_disperse": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_spread;
uniform float u_intensity;
void main() {
    vec4 col = texture(u_tex, v_uv);
    vec2 ipx = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    // Edge detection for prism mask
    vec3 dx = (texture(u_tex, v_uv + vec2(ipx.x,0)).rgb
             - texture(u_tex, v_uv - vec2(ipx.x,0)).rgb);
    vec3 dy = (texture(u_tex, v_uv + vec2(0,ipx.y)).rgb
             - texture(u_tex, v_uv - vec2(0,ipx.y)).rgb);
    float edge = length(dx) + length(dy);
    // Sample 7 wavelengths of the visible spectrum
    float r = texture(u_tex, clamp(v_uv + vec2( u_spread*1.0, 0), 0.0, 1.0)).r;
    float g = col.g;
    float b = texture(u_tex, clamp(v_uv - vec2( u_spread*1.0, 0), 0.0, 1.0)).b;
    // Rainbow overlay at edges
    vec3 prism = vec3(r, g, b);
    float mask = clamp(edge * 3.0, 0.0, 1.0);
    frag = vec4(mix(col.rgb, prism, mask * u_intensity), col.a);
}
""",

"film_burn": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_intensity;
uniform float u_speed;
uniform float u_edge;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
float fbm(vec2 p) {
    float v = 0.0; float a = 0.5;
    for (int i = 0; i < 4; i++) { v += a * fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); p *= 2.1; a *= 0.5; }
    return v;
}
void main() {
    vec4 col = texture(u_tex, v_uv);
    float t = u_time * u_speed;
    // Distance to nearest edge
    float de = min(min(v_uv.x, 1.0-v_uv.x), min(v_uv.y, 1.0-v_uv.y));
    float in_edge = smoothstep(u_edge, 0.0, de);
    // Organic noise burn pattern
    float burn_n = fbm(v_uv * 6.0 + t * 0.3);
    float burn = in_edge * (burn_n * 1.5 + 0.3) * u_intensity;
    // Burn transitions: white hot → orange → brown → char
    vec3 hot   = vec3(1.0, 0.98, 0.8);
    vec3 flame = vec3(1.0, 0.4, 0.05);
    vec3 char_col  = vec3(0.05, 0.02, 0.0);
    vec3 fire = burn < 0.4 ? mix(col.rgb, hot, burn/0.4)
              : burn < 0.7 ? mix(hot, flame, (burn-0.4)/0.3)
              :               mix(flame, char_col, (burn-0.7)/0.3);
    float alpha = burn > 0.95 ? 0.0 : col.a;
    frag = vec4(clamp(fire, 0.0, 1.0), alpha);
}
""",

"oil_paint": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_radius;
uniform float u_sharpness;
void main() {
    // Kuwahara filter: pick quadrant with minimum variance
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    int R = int(u_radius);
    vec3 mean[4];   float var[4];
    for (int q = 0; q < 4; q++) { mean[q] = vec3(0.0); var[q] = 0.0; }
    float cnt = 0.0;
    for (int dy = -R; dy <= R; dy++) {
        for (int dx = -R; dx <= R; dx++) {
            vec2 uv = v_uv + vec2(float(dx), float(dy)) * px;
            vec3 c = texture(u_tex, clamp(uv, 0.0, 1.0)).rgb;
            if (dx <= 0 && dy <= 0) { mean[0] += c; var[0] += dot(c,c); }
            if (dx >= 0 && dy <= 0) { mean[1] += c; var[1] += dot(c,c); }
            if (dx <= 0 && dy >= 0) { mean[2] += c; var[2] += dot(c,c); }
            if (dx >= 0 && dy >= 0) { mean[3] += c; var[3] += dot(c,c); }
        }
    }
    float n = float(R+1)*float(R+1);
    float min_var = 1e9;
    vec3 result = mean[0] / n;
    for (int q = 0; q < 4; q++) {
        mean[q] /= n;
        var[q] = var[q]/n - dot(mean[q], mean[q]);
        float v = dot(var[q], vec3(1.0));
        if (v < min_var) { min_var = v; result = mean[q]; }
    }
    // Slight sharpness boost
    vec3 orig = texture(u_tex, v_uv).rgb;
    frag = vec4(clamp(result + (result - orig) * (u_sharpness * 0.05), 0.0, 1.0), 1.0);
}
""",

"stained_glass": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_cell_size;
uniform float u_border;
uniform float u_saturation;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec2 uv_px = v_uv * vec2(u_tex_w, u_tex_h);
    vec2 cell_uv = uv_px / u_cell_size;
    vec2 cell_id = floor(cell_uv);
    // Find nearest Voronoi center
    float min_d = 1e9;
    vec2 nearest = vec2(0.0);
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            vec2 nb = cell_id + vec2(float(dx), float(dy));
            vec2 jitter = vec2(hash(nb), hash(nb + vec2(13.7, 7.3)));
            vec2 pt = nb + 0.5 + (jitter - 0.5) * 0.7;
            float d = length(cell_uv - pt);
            if (d < min_d) { min_d = d; nearest = pt; }
        }
    }
    // Second nearest for border
    float min_d2 = 1e9;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            vec2 nb = cell_id + vec2(float(dx), float(dy));
            vec2 jitter = vec2(hash(nb), hash(nb + vec2(13.7, 7.3)));
            vec2 pt = nb + 0.5 + (jitter - 0.5) * 0.7;
            float d = length(cell_uv - pt);
            if (d > min_d + 0.001) min_d2 = min(min_d2, d);
        }
    }
    float border_mask = smoothstep(0.0, u_border, min_d2 - min_d);
    // Sample image at the Voronoi center
    vec2 sample_uv = nearest * u_cell_size / vec2(u_tex_w, u_tex_h);
    vec3 cell_col = texture(u_tex, clamp(sample_uv, 0.0, 1.0)).rgb;
    // Boost saturation of cell color
    float lum = dot(cell_col, vec3(0.299, 0.587, 0.114));
    cell_col = mix(vec3(lum), cell_col, u_saturation);
    vec3 result = cell_col * border_mask;
    frag = vec4(clamp(result, 0.0, 1.0), 1.0);
}
""",

"neon_edge_glow": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_threshold;
uniform float u_glow;
uniform float u_hue;
void main() {
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    // Sobel edge detection
    vec3 tl = texture(u_tex, v_uv + vec2(-px.x, -px.y)).rgb;
    vec3 tc = texture(u_tex, v_uv + vec2(    0, -px.y)).rgb;
    vec3 tr = texture(u_tex, v_uv + vec2( px.x, -px.y)).rgb;
    vec3 ml = texture(u_tex, v_uv + vec2(-px.x,     0)).rgb;
    vec3 mr = texture(u_tex, v_uv + vec2( px.x,     0)).rgb;
    vec3 bl = texture(u_tex, v_uv + vec2(-px.x,  px.y)).rgb;
    vec3 bc = texture(u_tex, v_uv + vec2(    0,  px.y)).rgb;
    vec3 br = texture(u_tex, v_uv + vec2( px.x,  px.y)).rgb;
    vec3 gx = -tl - 2.0*ml - bl + tr + 2.0*mr + br;
    vec3 gy = -tl - 2.0*tc - tr + bl + 2.0*bc + br;
    float edge = length(vec2(length(gx), length(gy)));
    edge = smoothstep(u_threshold, u_threshold + 0.2, edge);
    // Hue → RGB for neon color
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(u_hue + K.xyz) * 6.0 - K.www);
    vec3 neon_col = clamp(p - K.xxx, 0.0, 1.0);
    // Dark background + glowing edges
    vec4 col = texture(u_tex, v_uv);
    float dark = 0.15;
    vec3 result = col.rgb * dark + neon_col * edge * u_glow;
    // Glow bloom — add blurred edge contribution
    vec3 bloom = vec3(0.0);
    for (int i = 1; i <= 4; i++) {
        float r = float(i) * 2.0;
        bloom += neon_col * edge / (r * r + 1.0);
    }
    result += bloom * u_glow * 0.3;
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
""",

"cyberpunk_grade": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_shadow_teal;
uniform float u_hi_orange;
uniform float u_contrast;
void main() {
    vec4 col = texture(u_tex, v_uv);
    vec3 c = col.rgb;
    // Contrast crush
    c = (c - 0.5) * u_contrast + 0.5;
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    // Shadow: push toward deep blue-teal
    vec3 shadow_col = vec3(0.02, 0.06, 0.18);
    float shadow_mask = smoothstep(0.45, 0.0, lum);
    c = mix(c, shadow_col + c * 0.4, shadow_mask * u_shadow_teal);
    // Highlight: push toward warm orange
    vec3 hi_col = vec3(1.0, 0.7, 0.35);
    float hi_mask = smoothstep(0.65, 1.0, lum);
    c = mix(c, hi_col * lum, hi_mask * u_hi_orange * 0.6);
    // Subtle cyan saturation in mids
    vec3 mid_teal = vec3(0.0, 0.9, 1.0);
    float mid_mask = 1.0 - shadow_mask - hi_mask;
    float lum2 = dot(c, vec3(0.2126,0.7152,0.0722));
    c = mix(c, mix(vec3(lum2), c, 1.0) * mix(vec3(1.0), mid_teal, 0.2), mid_mask * u_shadow_teal * 0.4);
    frag = vec4(clamp(c, 0.0, 1.0), col.a);
}
""",

"matrix_rain": """\
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_density;
uniform float u_speed;
uniform float u_green_mix;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec4 col = texture(u_tex, v_uv);
    // Column-based rain strips
    float col_w = 12.0 / u_tex_w;
    float col_id = floor(v_uv.x / col_w);
    float col_phase = hash(vec2(col_id, 0.0));
    float drop_speed = u_speed * (0.5 + col_phase * 1.5);
    float drop_y = fract(col_phase + u_time * drop_speed * 0.15);
    // Rain streak: bright head, fading tail
    float dist_to_head = v_uv.y - drop_y;
    float rain = 0.0;
    if (dist_to_head > 0.0 && dist_to_head < 0.35) {
        float head = exp(-dist_to_head * 12.0);
        rain = head * step(col_phase, u_density);
    }
    // Bright head flash
    float head_flash = exp(-abs(v_uv.y - drop_y) * 60.0) * step(col_phase, u_density);
    // Monochrome-to-green shift
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    vec3 green_tint = vec3(lum * 0.2, lum, lum * 0.3);
    vec3 base = mix(col.rgb, green_tint, u_green_mix);
    vec3 rain_col = vec3(0.0, rain * 0.8, 0.0) + vec3(head_flash * 0.8, head_flash, head_flash * 0.8);
    frag = vec4(clamp(base + rain_col, 0.0, 1.0), col.a);
}
""",
}  # end SHADERS dict

reg["effects"].extend(BATCH)
reg["project_version"] += 1

with open(REG, "w") as f:
    json.dump(reg, f, indent=2)
print(f"Updated registry: {len(BATCH)} effects added, version={reg['project_version']}")

os.makedirs(SHADER_DIR, exist_ok=True)
for name, src in SHADERS.items():
    ws(name, src)
print(f"Wrote {len(SHADERS)} shaders.")
