#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_h;
uniform float u_density;
uniform float u_strength;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float line = mod(v_uv.y * u_tex_h / u_density, 2.0);
    float mask = (line < 1.0) ? 1.0 : (1.0 - u_strength);
    frag = vec4(col.rgb * mask, col.a);
}
