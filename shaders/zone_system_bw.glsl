#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_zones;
uniform float u_contrast;
uniform float u_grain;
uniform float u_paper_white;
uniform float u_time;
uniform float u_strength;
float hash(vec2 p) { return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // High-contrast S-curve
    lum = clamp((lum - 0.5) * u_contrast + 0.5, 0.0, 1.0);
    // Quantize to N zones
    lum = floor(lum * u_zones) / (u_zones - 1.0);
    // Film grain (larger grain in shadows)
    vec2 npx = floor(v_uv * vec2(u_tex_w, u_tex_h));
    float shadow_grain = 1.5 - lum;
    float g = (hash(npx + vec2(u_time*17.3, u_time*31.1)) - 0.5) * u_grain * 0.35 * shadow_grain;
    lum = clamp(lum + g, 0.0, 1.0);
    // Paper: white point + very slight warm tint
    vec3 result = mix(vec3(0.04, 0.035, 0.03), vec3(u_paper_white, u_paper_white*0.99, u_paper_white*0.96), lum);
    frag = vec4(mix(col.rgb, result, u_strength), col.a);
}
