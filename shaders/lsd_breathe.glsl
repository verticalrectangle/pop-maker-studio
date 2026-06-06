#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_time;
uniform float u_strength;
uniform float u_breathe_rate;
uniform float u_warp_strength;
uniform float u_color_speed;
uniform float u_chroma_split;
uniform float u_complexity;

// Convert RGB <-> HSV for hue cycling
vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main() {
    float t = u_time * u_breathe_rate;

    // ── Breathing warp ────────────────────────────────────────────────────
    // Layer 1: large slow waves — the walls expand and contract
    float wave1x = sin(v_uv.y * 3.1 + t * 1.0) * cos(v_uv.x * 2.3 + t * 0.7);
    float wave1y = cos(v_uv.x * 2.8 + t * 0.9) * sin(v_uv.y * 2.1 + t * 1.2);

    // Layer 2: finer ripples for surface texture breathing (blended by complexity)
    float wave2x = sin(v_uv.y * 9.7 + t * 2.3 + v_uv.x * 5.1) * 0.4;
    float wave2y = cos(v_uv.x * 8.3 + t * 1.9 + v_uv.y * 6.4) * 0.4;

    vec2 warp = vec2(
        mix(wave1x, wave1x + wave2x, u_complexity),
        mix(wave1y, wave1y + wave2y, u_complexity)
    ) * u_warp_strength;

    // Global breathe pulse — amplitude swells and contracts
    float pulse = 0.7 + 0.3 * sin(t * 1.57);
    warp *= pulse;

    // ── Chromatic split ───────────────────────────────────────────────────
    // Sample R, G, B at slightly offset warp positions for color fringing
    float split = u_chroma_split * u_warp_strength * 0.5;
    vec2 uv_r = clamp(v_uv + warp + vec2( split,  split * 0.5), 0.0, 1.0);
    vec2 uv_g = clamp(v_uv + warp,                               0.0, 1.0);
    vec2 uv_b = clamp(v_uv + warp + vec2(-split, -split * 0.5), 0.0, 1.0);

    float r = texture(u_tex, uv_r).r;
    float g = texture(u_tex, uv_g).g;
    float b = texture(u_tex, uv_b).b;
    float a = texture(u_tex, uv_g).a;

    vec3 col = vec3(r, g, b);

    // ── Hue cycling ───────────────────────────────────────────────────────
    vec3 hsv = rgb2hsv(col);
    // Shift hue over time; shadows (low value) shift less so blacks stay black
    float hue_delta = u_time * u_color_speed * 0.1;
    hsv.x = fract(hsv.x + hue_delta * hsv.z);
    // Boost saturation slightly so the cycling is vivid
    hsv.y = clamp(hsv.y * (1.0 + 0.4 * u_color_speed), 0.0, 1.0);
    col = hsv2rgb(hsv);

    // ── Blend with original by u_strength ─────────────────────────────────
    vec4 orig = texture(u_tex, v_uv);
    frag = vec4(mix(orig.rgb, col, u_strength), a);
}
