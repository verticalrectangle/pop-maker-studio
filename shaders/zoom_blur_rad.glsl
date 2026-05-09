#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_amount;
uniform float u_cx;
uniform float u_cy;
void main() {
    vec2 focus = vec2(u_cx, u_cy);
    vec4 acc = vec4(0.0);
    const int S = 14;
    for (int i = 0; i < S; i++) {
        float t = float(i) / float(S - 1);
        float scale = 1.0 - u_amount * t;
        vec2 uv = focus + (v_uv - focus) * scale;
        float w = 1.0 - t * 0.5;
        acc += texture(u_tex, clamp(uv, 0.0, 1.0)) * w;
    }
    frag = acc / acc.a;
}
