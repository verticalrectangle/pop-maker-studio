// Blush Doll — soft-skin base + rosy blush wash on skin midtones + brightened
// eyes/teeth zone (bright neutrals). Full-frame doll-makeup look, no landmarks:
// the blush rides a skin-tone mask weighted toward the frame's center band
// where a face usually sits in selfie framing.
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_blush;   // 0..1 blush strength
uniform float u_smooth;  // 0..1 skin smoothing
uniform float u_tint;    // 0 = peach, 1 = pink

void main() {
    vec2 px = vec2(1.0 / u_tex_w, 1.0 / u_tex_h);
    vec4 c = texture(u_tex, v_uv);
    // Cheap 8-tap soft base.
    vec3 blur = c.rgb;
    for (int i = 0; i < 8; i++) {
        float a = float(i) * 0.785398;
        blur += texture(u_tex, v_uv + vec2(cos(a), sin(a)) * 3.0 * px).rgb;
    }
    blur /= 9.0;
    float lum = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    float skin = smoothstep(0.18, 0.4, lum) * (1.0 - smoothstep(0.8, 0.95, lum))
               * smoothstep(0.0, 0.08, c.r - c.b);
    vec3 rgb = mix(c.rgb, blur, u_smooth * skin);
    // Center-band weight: strongest in the middle vertical third.
    float band = 1.0 - smoothstep(0.18, 0.5, abs(v_uv.y - 0.45));
    vec3 blush_col = mix(vec3(1.0, 0.55, 0.42), vec3(1.0, 0.45, 0.62), u_tint);
    float bw = u_blush * skin * band * 0.45;
    rgb = mix(rgb, blush_col * (0.4 + 0.6 * lum + 0.3), bw);
    // Doll pop: brighten near-white neutrals (eyes/teeth) slightly.
    float neutral = 1.0 - smoothstep(0.05, 0.15, abs(c.r - c.g) + abs(c.g - c.b));
    float bright = smoothstep(0.55, 0.8, lum);
    rgb += neutral * bright * 0.12 * u_blush;
    frag = vec4(clamp(rgb, 0.0, 1.0), c.a);
}
