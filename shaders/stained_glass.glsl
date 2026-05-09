#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_cell_size;
uniform float u_border;
uniform float u_saturation;
uniform float u_strength;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec2 uv_px = v_uv * vec2(u_tex_w, u_tex_h);
    vec2 cell_uv = uv_px / u_cell_size;
    vec2 cell_id = floor(cell_uv);
    // Find nearest Voronoi center
    float min_d = 1e9;
    vec2 nearest = vec2(0.0);
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            vec2 nb = cell_id + vec2(float(dx), float(dy));
            vec2 jitter = vec2(hash(nb), hash(nb + vec2(13.7, 7.3)));
            vec2 pt = nb + 0.5 + (jitter - 0.5) * 0.7;
            float d = length(cell_uv - pt);
            if (d < min_d) { min_d = d; nearest = pt; }
        }
    }
    // Second nearest for border
    float min_d2 = 1e9;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            vec2 nb = cell_id + vec2(float(dx), float(dy));
            vec2 jitter = vec2(hash(nb), hash(nb + vec2(13.7, 7.3)));
            vec2 pt = nb + 0.5 + (jitter - 0.5) * 0.7;
            float d = length(cell_uv - pt);
            if (d > min_d + 0.001) min_d2 = min(min_d2, d);
        }
    }
    float border_mask = smoothstep(0.0, u_border, min_d2 - min_d);
    // Sample image at the Voronoi center
    vec2 sample_uv = nearest * u_cell_size / vec2(u_tex_w, u_tex_h);
    vec3 cell_col = texture(u_tex, clamp(sample_uv, 0.0, 1.0)).rgb;
    // Boost saturation of cell color
    float lum = dot(cell_col, vec3(0.299, 0.587, 0.114));
    cell_col = mix(vec3(lum), cell_col, u_saturation);
    vec3 result = cell_col * border_mask;
    vec4 orig = texture(u_tex, v_uv);
    frag = vec4(clamp(mix(orig.rgb, result, u_strength), 0.0, 1.0), orig.a);
}
