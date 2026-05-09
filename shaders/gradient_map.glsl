#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_hue1;
uniform float u_hue2;
uniform float u_mix_orig;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Map luminance to 2-color gradient
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p1 = abs(fract(u_hue1 + K.xyz) * 6.0 - K.www);
    vec3 p2 = abs(fract(u_hue2 + K.xyz) * 6.0 - K.www);
    vec3 c1 = clamp(p1 - K.xxx, 0.0, 1.0);
    vec3 c2 = clamp(p2 - K.xxx, 0.0, 1.0);
    vec3 mapped = mix(c1, c2, lum);
    vec3 result = mix(mapped, col.rgb, u_mix_orig);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
