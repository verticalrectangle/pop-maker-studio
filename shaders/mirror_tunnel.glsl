#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_depth;
uniform float u_rotation;
uniform float u_zoom;
uniform float u_time;
void main() {
    vec2 uv = v_uv - 0.5;
    int maxSteps = int(u_depth);
    float angle_acc = u_time * u_rotation * 0.5;
    for (int i = 0; i < 12; i++) {
        if (i >= maxSteps) break;
        float s = sin(angle_acc), c_a = cos(angle_acc);
        uv = vec2(uv.x*c_a - uv.y*s, uv.x*s + uv.y*c_a);
        uv /= u_zoom;
        uv = abs(fract(uv * 0.5 + 0.5) * 2.0 - 1.0) - 0.5;
        angle_acc += 0.2 + u_rotation * 0.3;
    }
    uv += 0.5;
    frag = texture(u_tex, clamp(uv, 0.0, 1.0));
}
