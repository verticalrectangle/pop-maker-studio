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
    // Accumulate streaks from nearby bright pixels in 4 directions
    vec3 streak = vec3(0.0);
    float wsum = 0.0;
    for (int i = 1; i <= 16; i++) {
        float fi = float(i);
        float w = exp(-fi * u_trail);
        float step_px = fi * 5.0;
        streak += texture(u_tex, clamp(v_uv + vec2(step_px, 0)*px, 0.0, 1.0)).rgb * w;
        streak += texture(u_tex, clamp(v_uv - vec2(step_px, 0)*px, 0.0, 1.0)).rgb * w;
        streak += texture(u_tex, clamp(v_uv + vec2(0, step_px)*px, 0.0, 1.0)).rgb * w;
        streak += texture(u_tex, clamp(v_uv - vec2(0, step_px)*px, 0.0, 1.0)).rgb * w;
        wsum += 4.0 * w;
    }
    streak /= wsum;
    // Screen-blend the streak onto the original, stronger in bright areas
    float bright_mask = smoothstep(u_threshold - 0.1, u_threshold + 0.25, lum);
    vec3 result = 1.0 - (1.0 - col.rgb) * (1.0 - streak * bright_mask * u_glow * 1.5);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
