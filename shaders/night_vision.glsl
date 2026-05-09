#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_time;
uniform float u_noise;
uniform float u_gain;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
void main() {
    vec4 col = texture(u_tex, v_uv);
    float luma = dot(col.rgb, vec3(0.299, 0.587, 0.114)) * u_gain;
    // Phosphor noise
    float n = hash(v_uv + vec2(u_time * 13.7, u_time * 9.1)) * u_noise * 0.3;
    // Radial vignette
    vec2 d = v_uv - 0.5;
    float vig = 1.0 - smoothstep(0.3, 0.75, length(d) * 1.3);
    float g = clamp(luma + n, 0.0, 1.0) * vig;
    frag = vec4(g * 0.15, g, g * 0.08, col.a);
}
