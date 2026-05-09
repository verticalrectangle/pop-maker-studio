#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_strength;
void main() {
    vec2 p = v_uv * 2.0 - 1.0;
    float r = length(p);
    float theta = atan(p.y, p.x);
    float r2 = r * (1.0 + u_strength * r * r);
    vec2 warped = vec2(cos(theta), sin(theta)) * r2 * 0.5 + 0.5;
    if (warped.x < 0.0 || warped.x > 1.0 || warped.y < 0.0 || warped.y > 1.0)
        frag = vec4(0.0, 0.0, 0.0, 1.0);
    else
        frag = texture(u_tex, warped);
}
