#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_levels;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float n = max(2.0, u_levels);
    vec3 p = floor(col.rgb * n + 0.5) / n;
    frag = vec4(p, col.a);
}
