#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_dot_size;
uniform float u_scatter;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    float sz = u_dot_size;
    // Find the cell for this pixel
    vec2 cell_uv = v_uv / (px * sz);
    vec2 cell_id = floor(cell_uv);
    vec3 result = vec3(0.95); // paper white
    float min_dist = 1e9;
    vec3 nearest_col = vec3(0.5);
    // Check 9 neighboring cells
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            vec2 nb = cell_id + vec2(float(dx), float(dy));
            // Jitter the dot center within the cell
            vec2 jitter = vec2(hash(nb), hash(nb + vec2(31.7, 71.3)));
            vec2 dot_ctr = (nb + 0.5 + (jitter - 0.5) * u_scatter) * px * sz;
            float d = length(v_uv - dot_ctr);
            if (d < min_dist) {
                min_dist = d;
                // Sample image at the dot center
                vec3 col = texture(u_tex, clamp(dot_ctr, 0.0, 1.0)).rgb;
                float lum = dot(col, vec3(0.299, 0.587, 0.114));
                // Dot radius proportional to luminance (dark = big dot)
                float r = (1.0 - lum * 0.7) * px.x * sz * 0.55;
                nearest_col = (d < r) ? col : vec3(0.95);
            }
        }
    }
    // Re-check with actual dot radius
    float lum_c = dot(nearest_col, vec3(0.299, 0.587, 0.114));
    vec2 best_center = (floor(v_uv / (px * sz)) + 0.5) * px * sz;
    vec3 cell_color = texture(u_tex, clamp(best_center, 0.0, 1.0)).rgb;
    float cell_lum = dot(cell_color, vec3(0.299, 0.587, 0.114));
    float r = (1.0 - cell_lum * 0.7) * px.x * sz * 0.55;
    vec2 local = v_uv - best_center;
    result = (length(local) < r) ? cell_color : vec3(0.95);
    frag = vec4(clamp(result, 0.0, 1.0), 1.0);
}
