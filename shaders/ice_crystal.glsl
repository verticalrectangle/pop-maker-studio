#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_scale;
uniform float u_refract;
uniform float u_tint;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec2 uv_sc = v_uv * vec2(u_tex_w, u_tex_h) / u_scale;
    // Voronoi for crystal cell structure
    vec2 cell = floor(uv_sc);
    vec2 local = fract(uv_sc);
    float min_d1 = 1e9, min_d2 = 1e9;
    vec2 nearest = vec2(0.0);
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            vec2 nb = cell + vec2(float(dx), float(dy));
            vec2 pt = vec2(hash(nb), hash(nb + vec2(17.3, 43.7)));
            float d = length(local - nb + cell - nb + pt);
            d = length(fract(uv_sc) - pt - vec2(float(dx), float(dy)));
            if (d < min_d1) { min_d2 = min_d1; min_d1 = d; nearest = pt; }
            else if (d < min_d2) { min_d2 = d; }
        }
    }
    // Cell border = refraction interface
    float border_dist = min_d2 - min_d1;
    float border = smoothstep(0.05, 0.0, border_dist);
    // Refract at cell borders
    vec2 refract_dir = normalize(v_uv - (cell + nearest) * u_scale / vec2(u_tex_w, u_tex_h));
    vec2 refract_uv = clamp(v_uv + refract_dir * border * u_refract, 0.0, 1.0);
    vec3 sample_col = texture(u_tex, refract_uv).rgb;
    // Blue-white ice tint
    vec3 ice_tint = mix(sample_col, sample_col * vec3(0.7, 0.85, 1.2), u_tint);
    // Bright borders
    ice_tint += border * 0.5 * vec3(0.8, 0.9, 1.0);
    frag = vec4(clamp(ice_tint, 0.0, 1.0), 1.0);
}
