#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_orange_mask;
uniform float u_contrast;
uniform float u_grain;
uniform float u_time;
uniform float u_strength;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec4 col = texture(u_tex, v_uv);
    // Invert
    vec3 neg = 1.0 - col.rgb;
    // Apply orange mask (simulates orange film base)
    neg.r = mix(neg.r, neg.r * 0.85 + 0.15, u_orange_mask);
    neg.g = mix(neg.g, neg.g * 0.7 + 0.08, u_orange_mask);
    neg.b = mix(neg.b, neg.b * 0.3 + 0.02, u_orange_mask * 0.8);
    // Contrast
    neg = clamp((neg - 0.5) * u_contrast + 0.5, 0.0, 1.0);
    // Film grain
    vec2 npx = floor(v_uv * vec2(u_tex_w, u_tex_h));
    float g = (hash(npx + vec2(u_time * 23.1, u_time * 17.7)) - 0.5) * u_grain * 0.3;
    frag = vec4(clamp(mix(col.rgb, neg + g, u_strength), 0.0, 1.0), col.a);
}
