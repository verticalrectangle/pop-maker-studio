#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_char_size;
uniform float u_fg_r;
uniform float u_fg_g;
uniform float u_fg_b;
uniform float u_bg_dark;
uniform float u_strength;
float hash(vec2 p) { return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453); }
// Approximate ASCII char density from luminance using procedural patterns
float char_pattern(vec2 cell_uv, float density) {
    // Use density to select pattern type
    float d = density;
    // Horizontal line (low density)
    float pat1 = step(0.45, cell_uv.y) * step(cell_uv.y, 0.55);
    // Cross (+)
    float pat2 = max(step(0.45,cell_uv.y)*step(cell_uv.y,0.55), step(0.45,cell_uv.x)*step(cell_uv.x,0.55));
    // Hash (#) - grid lines
    float gx = step(0.3,cell_uv.x)*step(cell_uv.x,0.4) + step(0.6,cell_uv.x)*step(cell_uv.x,0.7);
    float gy = step(0.3,cell_uv.y)*step(cell_uv.y,0.4) + step(0.6,cell_uv.y)*step(cell_uv.y,0.7);
    float pat3 = max(gx, gy);
    // Block (full)
    float pat4 = 1.0;
    if (d < 0.25) return mix(0.0, pat1, d*4.0);
    if (d < 0.5)  return mix(pat1, pat2, (d-0.25)*4.0);
    if (d < 0.75) return mix(pat2, pat3, (d-0.5)*4.0);
    return mix(pat3, pat4, (d-0.75)*4.0);
}
void main() {
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    vec2 char_uv = floor(v_uv / (px * u_char_size)) * px * u_char_size;
    float lum = dot(texture(u_tex, char_uv + px * u_char_size * 0.5).rgb, vec3(0.299,0.587,0.114));
    vec2 cell_uv = fract(v_uv / (px * u_char_size));
    float on = char_pattern(cell_uv, lum);
    vec4 orig = texture(u_tex, v_uv);
    vec3 fg = vec3(u_fg_r, u_fg_g, u_fg_b);
    vec3 bg = orig.rgb * (1.0 - u_bg_dark);
    vec3 result = mix(bg, fg * (0.3 + lum * 0.7), on);
    frag = vec4(clamp(mix(orig.rgb, result, u_strength), 0.0, 1.0), orig.a);
}
