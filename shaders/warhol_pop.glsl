#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_levels;
uniform float u_hue_shift;
uniform float u_saturation;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Posterize
    float quant_lum = floor(lum * u_levels + 0.5) / u_levels;
    // Remap lum to a hue
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    float hue = fract(quant_lum * 0.7 + u_hue_shift);
    vec3 p = abs(fract(hue + K.xyz) * 6.0 - K.www);
    vec3 hue_col = clamp(p - K.xxx, 0.0, 1.0);
    // Saturate the original, then apply pop color
    vec3 sat_orig = mix(vec3(lum), col.rgb, u_saturation);
    // Mix: posterized hue with saturated original
    vec3 pop = hue_col * quant_lum;
    vec3 result = mix(sat_orig, pop, 0.65);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
