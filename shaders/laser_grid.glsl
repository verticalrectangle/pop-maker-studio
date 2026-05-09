#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_grid_size;
uniform float u_hue;
uniform float u_intensity;
void main() {
    vec4 col = texture(u_tex, v_uv);
    vec2 uv_px = v_uv * vec2(u_tex_w, u_tex_h);
    // Grid lines
    vec2 grid = abs(fract(uv_px / u_grid_size) - 0.5) * 2.0;
    float line = 1.0 - min(grid.x, grid.y);
    float laser = smoothstep(0.85, 1.0, line);
    // Glow falloff
    float glow = smoothstep(0.6, 0.85, line) * 0.3;
    // Hue to RGB
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(u_hue + K.xyz) * 6.0 - K.www);
    vec3 lcolor = clamp(p - K.xxx, 0.0, 1.0);
    // Slight perspective: fade toward center
    float depth = 1.0 - length(v_uv - 0.5) * 0.8;
    vec3 result = col.rgb * (1.0 - laser * 0.7)
                + lcolor * (laser + glow) * u_intensity * depth;
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
