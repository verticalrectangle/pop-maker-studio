// Porcelain Skin — luma-guided smoothing + brighten + tone evening. A
// full-frame beauty pass (no face tracking): smoothing is edge-aware (bilateral
// approximation) so eyes/lips/hair keep detail while skin planes flatten.
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_smooth;    // 0..1 skin smoothing strength
uniform float u_brighten;  // 0..0.5 lift
uniform float u_warmth;    // -1..1 cool/warm shift

void main() {
    vec2 px = vec2(1.0 / u_tex_w, 1.0 / u_tex_h);
    vec4 c = texture(u_tex, v_uv);
    // Bilateral-ish blur: 12-tap disc, weights fall off with color distance so
    // edges survive.
    vec3 acc = c.rgb;
    float wsum = 1.0;
    for (int i = 0; i < 12; i++) {
        float a = float(i) * 0.5236 * 6.28318 / 6.28318 + float(i) * 2.39996;
        float r = 2.0 + 4.0 * fract(float(i) * 0.618034);
        vec3 s = texture(u_tex, v_uv + vec2(cos(a), sin(a)) * r * px).rgb;
        float w = exp(-dot(s - c.rgb, s - c.rgb) * 18.0);
        acc += s * w;
        wsum += w;
    }
    vec3 smoothed = acc / wsum;
    // Skin-probability mask: warm hues, mid luminance.
    float lum = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    float skin = smoothstep(0.15, 0.35, lum) * (1.0 - smoothstep(0.75, 0.95, lum))
               * smoothstep(0.0, 0.08, c.r - c.b) * step(c.b, c.r);
    vec3 rgb = mix(c.rgb, smoothed, u_smooth * clamp(skin * 1.4, 0.0, 1.0));
    // Gentle lift with highlight rolloff (porcelain brightness, no clipping).
    rgb = rgb + u_brighten * (1.0 - rgb) * (0.6 + 0.4 * skin);
    // Warmth: shift R up / B down proportionally.
    rgb.r = clamp(rgb.r + u_warmth * 0.06, 0.0, 1.0);
    rgb.b = clamp(rgb.b - u_warmth * 0.05, 0.0, 1.0);
    frag = vec4(clamp(rgb, 0.0, 1.0), c.a);
}
