// Metal transpile source for the legacy FXType::VHS pass. Desktop source of
// truth: k_vhs_frag in src/fx_shader.cpp — keep in sync. Uniform names follow
// the fx_chain param keys; the px→UV conversion (vhs_bleed / w) moves into
// the shader.
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_vhs_noise;
uniform float u_vhs_bleed;     // chroma bleed in PIXELS
uniform float u_vhs_tracking;
uniform float u_time;
uniform float u_tex_w;

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

void main() {
    float bleed = u_vhs_bleed / u_tex_w;
    vec2 uv = v_uv;
    if (u_vhs_tracking > 0.01) {
        float sl  = floor(uv.y * 240.0);
        float rnd = hash(vec2(sl, floor(u_time * 3.0)));
        if (rnd > 1.0 - u_vhs_tracking * 0.15)
            uv.x += (hash(vec2(sl, u_time * 7.0)) - 0.5) * u_vhs_tracking * 0.05;
        uv.x = clamp(uv.x, 0.0, 1.0);
    }
    float r = texture(u_tex, uv).r;
    float g = texture(u_tex, clamp(uv + vec2(bleed * 0.5, 0.0), 0.0, 1.0)).g;
    float b = texture(u_tex, clamp(uv + vec2(bleed,       0.0), 0.0, 1.0)).b;
    float a = texture(u_tex, uv).a;
    float grain = hash(uv + fract(vec2(u_time * 0.01))) * u_vhs_noise * 0.35;
    frag = vec4(clamp(vec3(r, g, b) + grain, 0.0, 1.0), a);
}
