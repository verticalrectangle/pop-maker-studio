#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_threshold;
uniform float u_radius;
uniform float u_red_shift;
uniform float u_strength;
void main() {
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    vec4 orig = texture(u_tex, v_uv);
    // Gaussian blur of bright highlight mask with red/orange tint
    vec3 halo = vec3(0.0);
    float wsum = 0.0;
    int R = int(u_radius);
    for (int dy=-R;dy<=R;dy++) {
        for (int dx=-R;dx<=R;dx++) {
            float w = exp(-float(dx*dx+dy*dy)/(u_radius*u_radius+0.001));
            vec4 s = texture(u_tex, clamp(v_uv + vec2(dx,dy)*px, 0.0, 1.0));
            float bright = smoothstep(u_threshold, u_threshold+0.2, dot(s.rgb, vec3(0.299,0.587,0.114)));
            halo += s.rgb * bright * w;
            wsum += w;
        }
    }
    halo /= (wsum + 0.001);
    // Halation is shifted toward red-orange (light scattering in film)
    halo = halo * mix(vec3(1.0), vec3(1.5, 0.5, 0.2), u_red_shift);
    // Screen blend onto original
    vec3 result = 1.0 - (1.0 - orig.rgb) * (1.0 - halo * u_strength * 0.8);
    frag = vec4(clamp(result, 0.0, 1.0), orig.a);
}
