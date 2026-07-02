#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_radius;   // smoothing radius px
uniform float u_gloss;    // 0..1 wet-highlight sheen

// Glass skin (the modern K-beauty look): chroma-gated bilateral smoothing
// like Skin Smooth, plus a luminous "wet" sheen — skin speculars get
// expanded so the surface reads dewy instead of matte.
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float skin_mask(vec3 c) {
    float cb = 0.5 - 0.168736 * c.r - 0.331264 * c.g + 0.5 * c.b;
    float cr = 0.5 + 0.5 * c.r - 0.418688 * c.g - 0.081312 * c.b;
    float mb = smoothstep(0.262, 0.342, cb) * (1.0 - smoothstep(0.458, 0.538, cb));
    float mr = smoothstep(0.482, 0.562, cr) * (1.0 - smoothstep(0.638, 0.718, cr));
    return mb * mr;
}
void main() {
    vec2 px = vec2(1.0 / u_tex_w, 1.0 / u_tex_h);
    vec4 src = texture(u_tex, v_uv);
    float mask = skin_mask(src.rgb);
    vec3 c = src.rgb;
    if (mask > 0.01) {
        float lc = luma(c);
        vec3 acc = c; float wsum = 1.0;
        float r = max(1.0, u_radius);
        for (float dy = -2.0; dy <= 2.0; dy += 1.0)
            for (float dx = -2.0; dx <= 2.0; dx += 1.0) {
                if (dx == 0.0 && dy == 0.0) continue;
                vec3 s = texture(u_tex, v_uv + vec2(dx, dy) * (r * 0.5) * px).rgb;
                float dl = abs(luma(s) - lc);
                float w2 = exp(-dl * dl * 60.0) * exp(-(dx*dx + dy*dy) * 0.12);
                acc += s * w2; wsum += w2;
            }
        c = mix(c, acc / wsum, mask);
        // Wet sheen: expand skin speculars + gentle luminous lift.
        float lum2 = luma(c);
        float spec = smoothstep(0.62, 0.95, lum2);
        c += u_gloss * mask * (spec * 0.22 + 0.035);
    }
    frag = vec4(clamp(c, 0.0, 1.0), src.a);
}
