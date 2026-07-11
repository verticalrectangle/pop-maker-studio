#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_fade;     // 0..1 lifted-blacks film fade
uniform float u_pop;      // 0..1 saturation punch
uniform float u_warmth;   // -1..1 cool<->warm split

// The 2016 feed grade: faded blacks, punchy saturation, warm mids over
// slightly teal shadows, and a soft corner vignette — the year every photo
// looked like this.
void main() {
    vec4 src = texture(u_tex, v_uv);
    vec3 c = src.rgb;
    float lum = dot(c, vec3(0.299, 0.587, 0.114));

    // Fade: lift blacks toward warm gray, roll highlights off slightly.
    c = c * (1.0 - u_fade * 0.18) + vec3(0.10, 0.095, 0.09) * u_fade;

    // Warm/teal split: warmth into mids+highs, teal into shadows.
    float hi = smoothstep(0.35, 0.9, lum);
    float lo = 1.0 - smoothstep(0.1, 0.5, lum);
    c.r += 0.070 * u_warmth * hi;
    c.g += 0.020 * u_warmth * hi;
    c.b -= 0.045 * u_warmth * hi;
    c.b += 0.040 * abs(u_warmth) * lo;
    c.g += 0.015 * abs(u_warmth) * lo;

    // Saturation pop with skin protection (don't nuke faces orange).
    float sat = 1.0 + u_pop * 0.55;
    vec3 gray = vec3(dot(c, vec3(0.299, 0.587, 0.114)));
    float skin = smoothstep(0.12, 0.0, abs(c.r - c.g - 0.10)) * step(c.b, c.g);
    c = mix(gray, c, mix(sat, 1.0 + u_pop * 0.2, skin));

    // Soft corner vignette rides the pop.
    vec2 d = v_uv - 0.5;
    c *= 1.0 - u_pop * 0.22 * smoothstep(0.35, 0.75, dot(d, d) * 2.0);

    frag = vec4(clamp(c, 0.0, 1.0), src.a);
}
