#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_k1;
uniform float u_k2;
uniform float u_scale;
void main() {
    vec2 d = (v_uv - 0.5) / u_scale;
    float r2 = dot(d, d);
    float distort = 1.0 + u_k1 * r2 + u_k2 * r2 * r2;
    vec2 uv = d * distort + 0.5;
    frag = texture(u_tex, clamp(uv, 0.0, 1.0));
}
