#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_radius;   // smoothing radius in px
uniform float u_tone;     // skin-mask permissiveness 0..1

// Beauty smoothing: edge-preserving blur applied only where the pixel reads
// as skin in YCbCr (classic Cb/Cr window), so eyes, lips, hair and the
// background keep their detail. Luma edges are preserved bilaterally —
// pores soften, jawlines don't.

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

float skin_mask(vec3 c) {
    float cb = 0.5 - 0.168736 * c.r - 0.331264 * c.g + 0.5 * c.b;
    float cr = 0.5 + 0.5 * c.r - 0.418688 * c.g - 0.081312 * c.b;
    // Classic skin window (Cb 77–127, Cr 133–173 in 8-bit), widened by tone.
    float w  = 0.02 + 0.06 * u_tone;
    float mb = smoothstep(0.302 - w, 0.302 + w, cb) *
               (1.0 - smoothstep(0.498 - w, 0.498 + w, cb));
    float mr = smoothstep(0.522 - w, 0.522 + w, cr) *
               (1.0 - smoothstep(0.678 - w, 0.678 + w, cr));
    return mb * mr;
}

void main() {
    vec2 px = vec2(1.0 / u_tex_w, 1.0 / u_tex_h);
    vec4 center = texture(u_tex, v_uv);
    float mask = skin_mask(center.rgb);
    if (mask < 0.01) { frag = center; return; }

    float lc = luma(center.rgb);
    vec3  acc = center.rgb;
    float wsum = 1.0;
    float r = max(1.0, u_radius);
    // Sparse 5x5 ring at stride r/2 — bilateral: spatial × luma-similarity.
    for (float dy = -2.0; dy <= 2.0; dy += 1.0) {
        for (float dx = -2.0; dx <= 2.0; dx += 1.0) {
            if (dx == 0.0 && dy == 0.0) continue;
            vec2 off = vec2(dx, dy) * (r * 0.5) * px;
            vec3 s = texture(u_tex, v_uv + off).rgb;
            float dl = abs(luma(s) - lc);
            float wr = exp(-dl * dl * 60.0);          // range: keep edges
            float ws = exp(-(dx*dx + dy*dy) * 0.12);  // spatial falloff
            float w2 = wr * ws;
            acc  += s * w2;
            wsum += w2;
        }
    }
    vec3 smoothed = acc / wsum;
    frag = vec4(mix(center.rgb, smoothed, mask), center.a);
}
