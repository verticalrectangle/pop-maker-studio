#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_angle;
void main() {
    vec2 c = vec2(0.5, 0.5);
    vec4 acc = vec4(0.0);
    const int S = 14;
    for (int i = 0; i < S; i++) {
        float a = u_angle * (float(i) / float(S-1) - 0.5);
        float cs = cos(a), sn = sin(a);
        vec2 d = v_uv - c;
        vec2 rot = vec2(cs*d.x - sn*d.y, sn*d.x + cs*d.y) + c;
        acc += texture(u_tex, clamp(rot, 0.0, 1.0));
    }
    frag = acc / float(S);
}
