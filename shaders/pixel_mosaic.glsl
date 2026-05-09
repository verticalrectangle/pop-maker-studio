#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_block_size;
uniform float u_color_steps;
uniform float u_strength;
void main() {
    vec2 px = vec2(u_tex_w, u_tex_h);
    vec2 block = floor(v_uv * px / u_block_size) * u_block_size / px;
    vec2 center = block + (u_block_size * 0.5) / px;
    vec4 orig = texture(u_tex, v_uv);
    vec4 c = texture(u_tex, clamp(center, 0.0, 1.0));
    // Quantize to color_steps levels
    c.rgb = floor(c.rgb * u_color_steps + 0.5) / u_color_steps;
    frag = vec4(mix(orig.rgb, c.rgb, u_strength), orig.a);
}
