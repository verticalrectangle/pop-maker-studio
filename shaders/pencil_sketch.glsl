#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_line_str;
uniform float u_paper_tone;
uniform float u_hatching;
uniform float u_strength;
void main() {
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    // Sobel edge detection
    float k[9];
    int idx = 0;
    for (int dy=-1;dy<=1;dy++) for (int dx=-1;dx<=1;dx++) {
        vec3 c = texture(u_tex, v_uv + vec2(dx,dy)*px).rgb;
        k[idx++] = dot(c, vec3(0.299,0.587,0.114));
    }
    float gx = -k[0]+k[2]-2.0*k[3]+2.0*k[5]-k[6]+k[8];
    float gy = -k[0]-2.0*k[1]-k[2]+k[6]+2.0*k[7]+k[8];
    float edge = clamp(sqrt(gx*gx+gy*gy) * u_line_str, 0.0, 1.0);
    // Cross-hatching pattern on dark areas
    float lum0 = k[4];
    float hatch = step(0.5 - lum0 * 0.5, fract((v_uv.x + v_uv.y) * u_tex_w * 0.04));
    hatch *= step(0.5 - lum0 * 0.5, fract((v_uv.x - v_uv.y) * u_tex_w * 0.04));
    float sketch = max(edge, (1.0 - hatch) * (1.0 - lum0) * u_hatching * 0.8);
    vec3 paper = vec3(u_paper_tone, u_paper_tone * 0.97, u_paper_tone * 0.92);
    vec3 result = mix(paper, vec3(0.1, 0.08, 0.05), sketch);
    vec4 orig = texture(u_tex, v_uv);
    frag = vec4(clamp(mix(orig.rgb, result, u_strength), 0.0, 1.0), orig.a);
}
