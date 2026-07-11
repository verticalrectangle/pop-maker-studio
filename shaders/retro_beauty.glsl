#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_glow;    // 0..1 diffusion halo
uniform float u_fade;    // 0..1 milky lifted blacks
uniform float u_blush;   // 0..1 warm-pink era cast

// 2016 beauty-cam: the front-camera-app look of the era — heavy diffusion
// glow, milky faded blacks, and that warm-pink cast every selfie app baked
// in. Pair with Skin Smooth for the full time machine.
void main() {
    vec2 px = vec2(1.0 / u_tex_w, 1.0 / u_tex_h);
    vec4 src = texture(u_tex, v_uv);
    vec3 c = src.rgb;

    // Wide soft-focus: sparse 5x5 at 4px stride, screen-blended.
    vec3 bsum = vec3(0.0);
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
            bsum += texture(u_tex, v_uv + vec2(float(dx), float(dy)) * 4.0 * px).rgb;
    bsum *= (1.0 / 25.0);
    vec3 scr = 1.0 - (1.0 - c) * (1.0 - bsum);
    c = mix(c, scr, u_glow * 0.7);

    // Milky fade: lift the floor, soften the ceiling.
    c = c * (1.0 - u_fade * 0.22) + vec3(u_fade * 0.16);
    c = mix(c, smoothstep(vec3(0.0), vec3(1.05), c), u_fade * 0.5);

    // Era cast: warm pink into mids, a whisper of magenta in shadows.
    float lum = dot(c, vec3(0.299, 0.587, 0.114));
    float mids = 4.0 * lum * (1.0 - lum);
    c.r += u_blush * 0.055 * mids;
    c.g += u_blush * 0.012 * mids;
    c.b += u_blush * 0.030 * (1.0 - lum);

    frag = vec4(clamp(c, 0.0, 1.0), src.a);
}
