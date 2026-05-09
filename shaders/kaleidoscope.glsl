#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_segments;
uniform float u_rotation;
uniform float u_zoom;
uniform float u_strength;
void main() {
    const float PI = 3.14159265;
    vec2 c = vec2(0.5, 0.5);
    vec2 d = (v_uv - c) / u_zoom;
    float angle = atan(d.y, d.x) + u_rotation;
    float radius = length(d);
    float sector = PI * 2.0 / max(u_segments, 2.0);
    angle = mod(angle, sector);
    if (angle > sector * 0.5) angle = sector - angle;
    vec2 uv = c + vec2(cos(angle), sin(angle)) * radius;
    // Mirror-tile so out-of-bounds regions fold back rather than clamp to edges
    uv = abs(fract(uv * 0.5) * 2.0 - 1.0);
    vec4 orig = texture(u_tex, v_uv);
    vec4 effect = texture(u_tex, uv);
    frag = vec4(mix(orig.rgb, effect.rgb, u_strength), orig.a);
}
