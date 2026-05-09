#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_hue1;
uniform float u_hue2;
uniform float u_dot_size;
uniform float u_misreg;
uniform float u_paper;
uniform float u_strength;
vec3 hue2rgb(float h) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    return clamp(abs(fract(h + K.xyz)*6.0 - K.www) - K.xxx, 0.0, 1.0);
}
float halftone(vec2 uv, float scale, float angle, float density) {
    float s = sin(angle), c_a = cos(angle);
    vec2 rot = vec2(uv.x*c_a - uv.y*s, uv.x*s + uv.y*c_a);
    vec2 cell = fract(rot * scale) - 0.5;
    return smoothstep(density * 0.5 + 0.05, density * 0.5 - 0.05, length(cell));
}
void main() {
    vec4 col1 = texture(u_tex, clamp(v_uv + vec2(u_misreg, u_misreg*0.5), 0.0, 1.0));
    vec4 col2 = texture(u_tex, clamp(v_uv - vec2(u_misreg*0.7, u_misreg), 0.0, 1.0));
    float lum1 = dot(col1.rgb, vec3(0.299, 0.587, 0.114));
    float lum2 = dot(col2.rgb, vec3(0.299, 0.587, 0.114));
    float scale = min(u_tex_w, u_tex_h) / u_dot_size * 0.015;
    float dot1 = halftone(v_uv, scale, 0.785, 1.0 - lum1);
    float dot2 = halftone(v_uv, scale, 0.35, 1.0 - lum2);
    vec3 paper_col = vec3(u_paper, u_paper * 0.96, u_paper * 0.88);
    vec3 ink1 = hue2rgb(u_hue1);
    vec3 ink2 = hue2rgb(u_hue2);
    vec3 result = paper_col;
    result = mix(result, ink1 * 0.85, dot1 * 0.8);
    result = mix(result, ink2 * 0.8, dot2 * 0.7);
    // Multiply where both inks overlap
    float overlap = dot1 * dot2;
    result = mix(result, ink1 * ink2, overlap * 0.6);
    vec4 orig = texture(u_tex, v_uv);
    frag = vec4(clamp(mix(orig.rgb, result, u_strength), 0.0, 1.0), orig.a);
}
