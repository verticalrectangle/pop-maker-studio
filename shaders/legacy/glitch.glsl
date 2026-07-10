// Metal transpile source for the legacy FXType::Glitch pass. Desktop source
// of truth: k_glitch_frag in src/fx_shader.cpp — keep in sync. Uniform names
// follow the fx_chain param keys; the px→UV conversion the desktop does on
// the CPU (glitch_chroma / w) moves into the shader.
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_glitch_chroma;           // RGB channel spread in PIXELS
uniform float u_glitch_jitter;           // row-jitter intensity 0..1
uniform float u_glitch_corruption;       // block corruption intensity 0..1
uniform float u_glitch_corruption_bleed; // 0 = noisy blocks, 1 = transparent holes
uniform float u_time;
uniform float u_tex_h;
uniform float u_tex_w;

float hash(float n) { return fract(sin(n) * 43758.5453); }
float hash2(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

void main() {
    float chroma = u_glitch_chroma / u_tex_w;
    float y_id = floor(v_uv.y * u_tex_h);
    float rnd = hash(y_id + floor(u_time * 12.0) * 31.7);
    float jshift = 0.0;
    if (u_glitch_jitter > 0.01 && rnd > 1.0 - u_glitch_jitter * 0.4) {
        float rnd2 = hash(y_id + floor(u_time * 8.0) * 57.3);
        jshift = (rnd2 - 0.5) * u_glitch_jitter * 0.12;
    }
    float r = texture(u_tex, clamp(vec2(v_uv.x + jshift + chroma, v_uv.y), 0.0, 1.0)).r;
    float g = texture(u_tex, clamp(vec2(v_uv.x + jshift,          v_uv.y), 0.0, 1.0)).g;
    float b = texture(u_tex, clamp(vec2(v_uv.x + jshift - chroma, v_uv.y), 0.0, 1.0)).b;
    float a = texture(u_tex, clamp(vec2(v_uv.x + jshift,          v_uv.y), 0.0, 1.0)).a;
    frag = vec4(r, g, b, a);

    // Block corruption — chunky datamosh "pixels" (see desktop notes).
    if (u_glitch_corruption > 0.01) {
        float bs = 16.0;
        vec2  px  = vec2(v_uv.x * u_tex_w, v_uv.y * u_tex_h);
        vec2  blk = floor(px / bs);
        float tq  = floor(u_time * 7.0);
        float br  = hash2(blk + tq * 1.7);
        if (br < u_glitch_corruption * 0.6) {
            float sh  = (hash2(blk.yx + tq * 3.1) - 0.5) * 0.30 * u_glitch_corruption;
            vec4  src = texture(u_tex, clamp(vec2(v_uv.x + sh, v_uv.y), 0.0, 1.0));
            float n   = hash2(floor(px / 3.0) + tq);
            vec4  noisy = vec4(src.rgb * (0.35 + 1.0 * n), src.a);
            frag = mix(noisy, vec4(0.0), u_glitch_corruption_bleed);
        }
    }
}
