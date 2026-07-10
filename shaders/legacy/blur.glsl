// Metal transpile source for the legacy FXType::Blur pass. The desktop runs
// k_blur_frag (src/fx_shader.cpp) twice as a separable H+V box; the manifest
// runner is single-pass, so this is a one-pass golden-angle disc blur with the
// same "blur" param scale (approx pixel radius). Visually gaussian-ish; not
// bit-identical to the desktop two-pass box.
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_blur;      // approx pixel radius (desktop u_sigma)
uniform float u_tex_w;
uniform float u_tex_h;

void main() {
    float r = clamp(u_blur * 1.5, 0.0, 21.0);
    if (r < 0.5) { frag = texture(u_tex, v_uv); return; }
    vec2 px = 1.0 / vec2(u_tex_w, u_tex_h);
    vec4 sum = texture(u_tex, v_uv);
    float wsum = 1.0;
    // 24-tap golden-angle spiral disc; radius grows with sqrt(i) for even
    // area coverage.
    for (int i = 1; i <= 24; i++) {
        float a = float(i) * 2.39996323;             // golden angle
        float rad = r * sqrt(float(i) / 24.0);
        vec2 off = vec2(cos(a), sin(a)) * rad * px;
        sum += texture(u_tex, clamp(v_uv + off, 0.0, 1.0));
        wsum += 1.0;
    }
    frag = sum / wsum;
}
