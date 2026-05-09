#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_time;
uniform float u_sepia;
uniform float u_scratch;
uniform float u_flicker;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
void main() {
    vec4 col = texture(u_tex, v_uv);
    // Flicker
    float flick = 1.0 - u_flicker * 0.15 * hash(vec2(u_time * 3.1, 0.5));
    // Sepia
    float luma = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    vec3 sep = vec3(luma * 1.07, luma * 0.74, luma * 0.43);
    vec3 c = mix(col.rgb, sep, u_sepia) * flick;
    // Vertical scratch lines
    float sx = floor(v_uv.x * 320.0 + u_time * 0.5 * 60.0);
    float sc = hash(vec2(sx, floor(u_time * 24.0)));
    float scratch_vis = step(1.0 - u_scratch * 0.02, sc);
    float scratch_x = hash(vec2(sx * 1.3, 0.0));
    float sdist = abs(v_uv.x - scratch_x / 320.0 * 320.0 / 320.0);
    c += scratch_vis * smoothstep(0.004, 0.0, sdist) * 0.8;
    // Vignette
    vec2 d = v_uv - 0.5;
    float vig = 1.0 - smoothstep(0.25, 0.75, length(d) * 1.4);
    frag = vec4(c * vig, col.a);
}
