#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_size;
void main() {
    float px = max(1.0, u_size);
    vec2 cell = vec2(px / u_tex_w, px / u_tex_h);
    vec2 snapped = floor(v_uv / cell) * cell + cell * 0.5;
    frag = texture(u_tex, snapped);
}
