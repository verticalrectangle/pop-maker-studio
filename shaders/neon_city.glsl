// Neon City — the cyberpunk hero grade: teal shadows / magenta highlights,
// scanlines, and horizontal neon bloom streaks off bright signs.
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_time;
uniform float u_neon;      // 0..1 grade strength
uniform float u_scanline;  // 0..1 scanline visibility
uniform float u_streak;    // 0..1 horizontal bloom streaks

void main() {
    vec2 px = vec2(1.0 / u_tex_w, 1.0 / u_tex_h);
    vec4 c = texture(u_tex, v_uv);
    // Horizontal streak bloom: sample a wide row of thresholded brights.
    vec3 streak = vec3(0.0);
    for (int i = -6; i <= 6; i++) {
        vec3 s = texture(u_tex, clamp(v_uv + vec2(float(i) * 4.0 * px.x, 0.0), 0.0, 1.0)).rgb;
        float b = max(max(s.r, s.g), s.b);
        streak += s * smoothstep(0.6, 0.95, b) * (1.0 - abs(float(i)) / 7.0);
    }
    streak /= 6.0;
    // Duotone grade: teal shadows, magenta highs, pivot on luma.
    float lum = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    vec3 teal    = vec3(0.05, 0.85, 0.9);
    vec3 magenta = vec3(1.0, 0.2, 0.85);
    vec3 duo = mix(teal * lum * 1.3, magenta * lum * 1.15, smoothstep(0.2, 0.8, lum));
    vec3 rgb = mix(c.rgb, duo, u_neon * 0.75);
    rgb += streak * magenta * u_streak * 0.8;
    // Scanlines with slow roll.
    float sl = sin((v_uv.y + u_time * 0.02) * u_tex_h * 1.8) * 0.5 + 0.5;
    rgb *= 1.0 - u_scanline * 0.25 * sl;
    frag = vec4(clamp(rgb, 0.0, 1.0), c.a);
}
