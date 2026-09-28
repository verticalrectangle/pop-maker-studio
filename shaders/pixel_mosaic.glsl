#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_block_size;
uniform float u_color_steps;
uniform float u_strength;
uniform float u_sampling;        // 0=center 1=mean 2=dark_bias
uniform float u_palette_levels;  // 0=off, else extra hard-quantise to N levels
void main() {
    vec2 px = vec2(u_tex_w, u_tex_h);
    vec2 block = floor(v_uv * px / u_block_size) * u_block_size / px;
    vec4 orig = texture(u_tex, v_uv);
    vec3 c = texture(u_tex, clamp(block + (u_block_size * 0.5) / px, 0.0, 1.0)).rgb;
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
                vec2 uv = block + (vec2(float(i), float(j)) + 0.5) * (u_block_size * 0.25) / px;
                vec3 t = texture(u_tex, clamp(uv, 0.0, 1.0)).rgb;
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
    // Quantize to color_steps levels
    c.rgb = floor(c.rgb * u_color_steps + 0.5) / u_color_steps;
    // Optional extra hard quantisation to N levels (0 = off). No blending.
    if (u_palette_levels > 1.5) {
        float n = floor(u_palette_levels + 0.5);
        c.rgb = floor(c.rgb * n + 0.5) / n;
    }
    frag = vec4(mix(orig.rgb, c.rgb, u_strength), orig.a);
}
