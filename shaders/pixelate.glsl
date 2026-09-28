#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_size;
uniform float u_sampling;        // 0=center 1=mean 2=dark_bias
uniform float u_palette_levels;  // 0=off, else hard-quantise to N levels
void main() {
    float px = max(1.0, u_size);
    vec2 cell = vec2(px / u_tex_w, px / u_tex_h);
    vec2 base = floor(v_uv / cell) * cell;
    vec4 s = texture(u_tex, base + cell * 0.5);
    vec3 c = s.rgb;
    float mode = floor(u_sampling + 0.5);
    if (mode > 0.5) {
        // 4x4 tap grid over the block: true mean, or mean biased toward
        // the block's darks (dark_bias = half mean + half the two darkest
        // taps, approx. 20th-percentile luma — glasses rims/irises survive).
        vec3 acc = vec3(0.0);
        float lmin1 = 2.0;
        float lmin2 = 2.0;
        vec3 cmin1 = vec3(0.0);
        vec3 cmin2 = vec3(0.0);
        for (int j = 0; j < 4; ++j) {
            for (int i = 0; i < 4; ++i) {
                vec2 uv = base + (vec2(float(i), float(j)) + 0.5) * (cell * 0.25);
                vec3 t = texture(u_tex, uv).rgb;
                acc += t;
                float l = dot(t, vec3(0.299, 0.587, 0.114));
                if (l < lmin1) {
                    lmin2 = lmin1; cmin2 = cmin1;
                    lmin1 = l;     cmin1 = t;
                } else if (l < lmin2) {
                    lmin2 = l; cmin2 = t;
                }
            }
        }
        vec3 mean = acc * (1.0 / 16.0);
        c = (mode > 1.5) ? (0.5 * mean + 0.25 * (cmin1 + cmin2)) : mean;
    }
    // Optional hard quantisation to N levels (0 = off). No blending.
    if (u_palette_levels > 1.5) {
        float n = floor(u_palette_levels + 0.5);
        c = floor(c * n + 0.5) / n;
    }
    frag = vec4(c, s.a);
}
