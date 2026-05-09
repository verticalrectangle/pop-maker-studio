#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_bleeding;
uniform float u_paper;
uniform float u_saturation;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    // Soft blur (watercolor wash base)
    vec3 acc = vec3(0.0);
    float wt = 0.0;
    for (int dy = -3; dy <= 3; dy++) {
        for (int dx = -3; dx <= 3; dx++) {
            float d = length(vec2(float(dx), float(dy)));
            float w = exp(-d * 0.6);
            vec2 uv = v_uv + vec2(float(dx),float(dy)) * px * (1.0 + u_bleeding * 30.0);
            acc += texture(u_tex, clamp(uv, 0.0, 1.0)).rgb * w;
            wt += w;
        }
    }
    vec3 wash = acc / wt;
    // Paper texture from noise
    float paper_n = hash(v_uv * vec2(u_tex_w, u_tex_h) * 0.05);
    float paper_tex = mix(1.0, paper_n * 0.3 + 0.85, u_paper);
    // Saturation boost (pigment richness)
    float lum = dot(wash, vec3(0.299, 0.587, 0.114));
    wash = mix(vec3(lum), wash, u_saturation) * paper_tex;
    // Slight edge darkening (wet paper bloom)
    vec4 orig = texture(u_tex, v_uv);
    frag = vec4(clamp(wash, 0.0, 1.0), orig.a);
}
