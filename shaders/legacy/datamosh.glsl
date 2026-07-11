// Metal transpile source for the legacy FXType::Datamosh colour-bleed pass.
// Desktop source of truth: k_datamosh_frag in src/fx_shader.cpp — keep in
// sync. Uniform names follow the fx_chain param keys (datamosh_spread; the
// datamosh_intensity key rides the wet/dry amount, matching the desktop CPU
// path which only feeds u_spread to this program).
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_datamosh_spread;
uniform float u_tex_w;

void main() {
    vec3 col = texture(u_tex, v_uv).rgb;

    // Corruption matte: how saturated/extreme is this pixel?
    float lo = min(col.r, min(col.g, col.b));
    float hi = max(col.r, max(col.g, col.b));
    float matte = smoothstep(0.25, 0.75, hi - lo);

    // Horizontal chroma bleed — R forward, B back, G stays
    float bleed = matte * u_datamosh_spread * 40.0 / u_tex_w;
    float r = texture(u_tex, v_uv + vec2( bleed,       0.0)).r;
    float b = texture(u_tex, v_uv - vec2( bleed * 0.6, 0.0)).b;

    frag = vec4(r, col.g, b, 1.0);
}
