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

vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + 1e-10)), d / (q.x + 1e-10), q.x);
}
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main() {
    vec4 src = texture(u_tex, v_uv);
    float lum = dot(src.rgb, vec3(0.299, 0.587, 0.114));

    // ── Edge detection via screen-space derivatives ────────────────────────
    // dFdx/dFdy are GPU-native: computed from adjacent fragments in the 2×2
    // rasterization quad — zero extra texture samples.
    vec2 grad = vec2(dFdx(lum), dFdy(lum));

    // complexity blends in a wider-kernel estimate by sampling one step out,
    // catching broader contours the derivative alone misses
    if (u_complexity > 0.01) {
        vec2 px = u_complexity * 0.004 * vec2(1.0);
        float lum2 = dot(texture(u_tex, v_uv + px).rgb, vec3(0.299, 0.587, 0.114));
        float lum3 = dot(texture(u_tex, v_uv - px).rgb, vec3(0.299, 0.587, 0.114));
        vec2 wide_grad = vec2(dFdx(lum2 - lum3), dFdy(lum2 - lum3));
        grad = mix(grad, wide_grad, u_complexity);
    }

    float edge_mag  = clamp(length(grad) * 4.0, 0.0, 1.0);
    vec2  edge_norm = normalize(grad + 1e-6);

    // ── Breathing pulse ───────────────────────────────────────────────────
    float t       = u_time * u_breathe_rate;
    float breath  = sin(t * 6.2832) * u_warp_strength;
    // Second harmonic — slightly irrational ratio keeps it from feeling mechanical
    float breath2 = sin(t * 10.1664 + 0.9) * u_warp_strength * 0.25;
    float pulse   = breath + breath2;

    // Edges breathe along their normals; flat areas get a tiny residual drift
    vec2 edge_warp = edge_norm * edge_mag * pulse;
    vec2 base_warp = vec2(sin(v_uv.y * 5.0 + t * 1.3),
                          cos(v_uv.x * 4.2 + t * 1.1))
                     * u_warp_strength * 0.08 * (1.0 - edge_mag);
    vec2 warp = edge_warp + base_warp;

    // ── Chromatic split along edge normal ─────────────────────────────────
    float split = u_chroma_split * u_warp_strength * 0.6;
    vec2 uv_r = clamp(v_uv + warp + edge_norm *  split, 0.0, 1.0);
    vec2 uv_g = clamp(v_uv + warp,                      0.0, 1.0);
    vec2 uv_b = clamp(v_uv + warp - edge_norm *  split, 0.0, 1.0);

    vec3 col = vec3(
        texture(u_tex, uv_r).r,
        texture(u_tex, uv_g).g,
        texture(u_tex, uv_b).b
    );
    float a = texture(u_tex, uv_g).a;

    // ── Hue cycling — edges drift faster, giving a glowing outline feel ───
    vec3 hsv = rgb2hsv(col);
    float hue_shift = u_time * u_color_speed * 0.08
                    + edge_mag * u_color_speed * 0.04;
    hsv.x = fract(hsv.x + hue_shift * hsv.z);
    hsv.y = clamp(hsv.y * (1.0 + 0.35 * u_color_speed), 0.0, 1.0);
    col = hsv2rgb(hsv);

    frag = vec4(mix(src.rgb, col, u_strength), a);
}
