#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_levels;
uniform float u_line_width;
uniform float u_line_hue;
uniform float u_fill_sat;
uniform float u_strength;
vec3 hue2rgb(float h) {
    vec4 K = vec4(1.0,2.0/3.0,1.0/3.0,3.0);
    return clamp(abs(fract(h+K.xyz)*6.0-K.www)-K.xxx, 0.0, 1.0);
}
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Quantize to contour levels
    float level = floor(lum * u_levels) / u_levels;
    float level_frac = fract(lum * u_levels);
    // Draw contour lines at zone boundaries
    float line = smoothstep(u_line_width, 0.0, min(level_frac, 1.0-level_frac));
    // Fill: luminance-to-hue map (like topographic coloring)
    float fill_hue = mix(0.67, 0.0, level);  // blue (deep) → red (high)
    vec3 fill_col = mix(vec3(level), hue2rgb(fill_hue), u_fill_sat);
    fill_col *= (0.3 + 0.7 * level);
    vec3 line_col = hue2rgb(u_line_hue) * 0.8;
    vec3 result = mix(fill_col, line_col, line);
    frag = vec4(clamp(mix(col.rgb, result, u_strength), 0.0, 1.0), col.a);
}
