#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_blur;
uniform float u_noise;
uniform float u_tint;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    // Noise-perturbed scatter blur
    float n1 = hash(v_uv * 300.0) - 0.5;
    float n2 = hash(v_uv * 300.0 + vec2(71.3, 37.1)) - 0.5;
    vec2 scatter = vec2(n1, n2) * u_noise * 0.5;
    vec4 acc = vec4(0.0);
    float wt = 0.0;
    float radius = u_blur / px.x;
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -4; dx <= 4; dx++) {
            float d = length(vec2(float(dx), float(dy)));
            float w = exp(-d * d * 0.12);
            vec2 uv = v_uv + (vec2(float(dx),float(dy)) + scatter * d) * px * radius * 0.25;
            acc += texture(u_tex, clamp(uv, 0.0, 1.0)) * w;
            wt += w;
        }
    }
    vec3 blur = acc.rgb / wt;
    // Frosted glass tint (slightly blue-white)
    vec3 frost = mix(blur, vec3(0.85, 0.90, 1.0), u_tint * 0.25);
    // Add subtle refraction lines
    float lines = sin(v_uv.x * u_tex_w * 0.5 + n1 * 20.0) * 0.01 * u_noise;
    frost += lines;
    frag = vec4(clamp(frost, 0.0, 1.0), acc.a / wt);
}
