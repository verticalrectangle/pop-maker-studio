// Liquid Marble — flowing domain-warped refraction. The image is pulled
// through layered sine/noise flow fields like wet marbled ink.
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_time;
uniform float u_flow;    // 0..1 warp depth
uniform float u_scale;   // 1..10 swirl frequency
uniform float u_speed;   // 0..4 flow speed

float n2(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(n2(i), n2(i + vec2(1, 0)), f.x),
               mix(n2(i + vec2(0, 1)), n2(i + vec2(1, 1)), f.x), f.y);
}

void main() {
    float t = u_time * u_speed * 0.4;
    vec2 p = v_uv * u_scale;
    // Two octaves of domain warp.
    vec2 q = vec2(noise(p + vec2(0.0, t)), noise(p + vec2(5.2, t * 1.3)));
    vec2 r = vec2(noise(p + 4.0 * q + vec2(1.7, 9.2 - t)),
                  noise(p + 4.0 * q + vec2(8.3, 2.8 + t)));
    vec2 warp = (q - 0.5 + (r - 0.5) * 0.7) * 0.12 * u_flow;
    vec4 c = texture(u_tex, clamp(v_uv + warp, 0.0, 1.0));
    // Ink-vein shading: darken along steep flow gradients.
    float vein = smoothstep(0.35, 0.0, abs(r.x - r.y));
    c.rgb *= 1.0 - vein * 0.25 * u_flow;
    frag = c;
}
