// Metal transpile source for the legacy FXType::ChromaKey pass (the clean
// keyer). Desktop source of truth: k_chroma_key_frag in src/fx_shader.cpp —
// keep in sync. Differences from the desktop text: the key colour arrives as
// three scalar uniforms named after the fx_chain param keys
// (chroma_key_r/g/b), and textureSize() is replaced by u_tex_w/u_tex_h.
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_chroma_key_r;
uniform float u_chroma_key_g;
uniform float u_chroma_key_b;
uniform float u_chroma_key_threshold;
uniform float u_chroma_key_softness;
uniform float u_tex_w;
uniform float u_tex_h;

void main() {
    vec3 key = vec3(u_chroma_key_r, u_chroma_key_g, u_chroma_key_b);
    vec4 c = texture(u_tex, v_uv);               // sharp center pixel — the OUTPUT color
    // Matte on a box-averaged colour (codec-noise tolerant); output stays the
    // sharp center pixel so foreground edges stay crisp.
    vec2 tx = 1.0 / vec2(u_tex_w, u_tex_h);
    vec3 kc = (c.rgb
            + texture(u_tex, v_uv + vec2( 2.0 * tx.x, 0.0)).rgb
            + texture(u_tex, v_uv + vec2(-2.0 * tx.x, 0.0)).rgb
            + texture(u_tex, v_uv + vec2(0.0,  2.0 * tx.y)).rgb
            + texture(u_tex, v_uv + vec2(0.0, -2.0 * tx.y)).rgb) * 0.2;
    float lum_k = dot(key, vec3(0.299, 0.587, 0.114));
    vec3 ck = key - lum_k;
    float lum_p = dot(kc, vec3(0.299, 0.587, 0.114));
    vec3 cp = kc - lum_p;
    float dist = length(cp - ck);
    float soft = max(u_chroma_key_softness, 0.001);
    float t = clamp((dist - u_chroma_key_threshold) / soft, 0.0, 1.0);
    float alpha = t * t * (3.0 - 2.0 * t);
    vec3 rgb = c.rgb;
    if (alpha < 1.0) {
        float spill = 1.0 - alpha;
        if (key.g > key.r && key.g > key.b)
            rgb.g = mix(rgb.g, (rgb.r + rgb.b) * 0.5, spill);
        else if (key.b > key.r && key.b > key.g)
            rgb.b = mix(rgb.b, (rgb.r + rgb.g) * 0.5, spill);
    }
    frag = vec4(rgb, alpha * c.a);
}
