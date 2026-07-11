// Breathe Warp — slow radial breathing zoom with chroma separation that grows
// toward the edges. Subtle at low amounts (dreamy), full melt at high.
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_time;
uniform float u_breathe;  // 0..1 zoom depth
uniform float u_rate;     // 0..4 breaths per ~6s
uniform float u_chroma;   // 0..1 edge chroma split

void main() {
    vec2 d = v_uv - 0.5;
    float r = length(d);
    float phase = sin(u_time * u_rate) * 0.5 + 0.5;
    // Radial zoom that eases harder near the edges (non-linear breathing).
    float amt = u_breathe * 0.12 * phase * (0.4 + r * 1.6);
    vec2 uv = 0.5 + d * (1.0 - amt);
    float ca = u_chroma * 0.012 * (r * 2.0) * (0.5 + phase);
    vec2 dir = r > 0.0001 ? d / r : vec2(0.0);
    float cr = texture(u_tex, clamp(uv + dir * ca, 0.0, 1.0)).r;
    float cg = texture(u_tex, clamp(uv,            0.0, 1.0)).g;
    float cb = texture(u_tex, clamp(uv - dir * ca, 0.0, 1.0)).b;
    float a  = texture(u_tex, clamp(uv,            0.0, 1.0)).a;
    frag = vec4(cr, cg, cb, a);
}
