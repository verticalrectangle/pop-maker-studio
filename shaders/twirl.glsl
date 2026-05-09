#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_strength;
uniform float u_radius;
void main() {
    vec2 c = vec2(0.5, 0.5);
    vec2 d = v_uv - c;
    float dist = length(d);
    float angle = u_strength * smoothstep(u_radius, 0.0, dist);
    float cs = cos(angle), sn = sin(angle);
    vec2 rot = vec2(cs*d.x - sn*d.y, sn*d.x + cs*d.y);
    frag = texture(u_tex, clamp(rot + c, 0.0, 1.0));
}
