#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_warmth;     // -1..1 cool↔warm white balance
uniform float u_brighten;   // 0..0.5 lift
uniform float u_glow;       // 0..1 soft-focus halo

// Beauty finishing: soft-focus glow (blurred copy screened over the image —
// the classic diffusion-filter portrait look), gentle warmth, lifted curve.
void main() {
    vec2 px = vec2(1.0 / u_tex_w, 1.0 / u_tex_h);
    vec4 src = texture(u_tex, v_uv);
    vec3 c = src.rgb;

    // Soft-focus: sparse 5x5 blur at 3px stride, screen-blended
    vec3 bsum = vec3(0.0);
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            bsum += texture(u_tex, v_uv + vec2(float(dx), float(dy)) * 3.0 * px).rgb;
        }
    }
    bsum *= (1.0 / 25.0);
    vec3 scr = 1.0 - (1.0 - c) * (1.0 - bsum);
    c = mix(c, scr, u_glow * 0.6);

    // Warmth: opposing red/blue gain
    c.r *= 1.0 + 0.10 * u_warmth;
    c.b *= 1.0 - 0.10 * u_warmth;

    // Brighten: lift that protects highlights
    c = c + u_brighten * (1.0 - c) * c * 2.0;

    frag = vec4(clamp(c, 0.0, 1.0), src.a);
}
