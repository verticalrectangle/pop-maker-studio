#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_strength;
uniform float u_hue;
void main() {
    vec4 col = texture(u_tex, v_uv);
    // Hue-tinted burn overlay
    float h = u_hue / 360.0;
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(h + K.xyz) * 6.0 - K.www);
    vec3 tint = clamp(p - K.xxx, 0.0, 1.0);
    // Burn blend: darken based on inverse
    vec3 burned = 1.0 - (1.0 - col.rgb) / max(tint, 0.001);
    burned = clamp(burned, 0.0, 1.0);
    frag = vec4(mix(col.rgb, burned, u_strength), col.a);
}
