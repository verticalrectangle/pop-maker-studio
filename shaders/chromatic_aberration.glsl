#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_strength;
void main() {
    vec2 center = v_uv - 0.5;
    float dist = length(center);
    vec2 offset = center * dist * u_strength * 0.06;
    float r = texture(u_tex, v_uv + offset).r;
    float g = texture(u_tex, v_uv         ).g;
    float b = texture(u_tex, v_uv - offset).b;
    frag = vec4(r, g, b, texture(u_tex, v_uv).a);
}
