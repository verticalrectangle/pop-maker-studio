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
