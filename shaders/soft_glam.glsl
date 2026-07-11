// Soft Glam — evening-makeup look: edge-aware smoothing, teal-shadow /
// champagne-highlight split tone, and animated micro-sparkle on the brightest
// speculars (glitter without particles).
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_time;
uniform float u_glam;     // 0..1 smoothing strength
uniform float u_split;    // 0..1 split-tone strength
uniform float u_sparkle;  // 0..1 glitter on speculars

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

void main() {
    vec2 px = vec2(1.0 / u_tex_w, 1.0 / u_tex_h);
    vec4 c = texture(u_tex, v_uv);
    vec3 acc = c.rgb;
    float wsum = 1.0;
    for (int i = 0; i < 10; i++) {
        float a = float(i) * 2.39996;
        float r = 2.0 + 3.5 * fract(float(i) * 0.618034);
        vec3 s = texture(u_tex, v_uv + vec2(cos(a), sin(a)) * r * px).rgb;
        float w = exp(-dot(s - c.rgb, s - c.rgb) * 14.0);
        acc += s * w; wsum += w;
    }
    vec3 rgb = mix(c.rgb, acc / wsum, u_glam * 0.8);
    // Split tone: shadows toward teal, highlights toward champagne.
    float lum = dot(rgb, vec3(0.299, 0.587, 0.114));
    vec3 shadow_tone = vec3(0.85, 1.0, 1.05);
    vec3 high_tone   = vec3(1.08, 1.0, 0.9);
    rgb *= mix(shadow_tone, high_tone, smoothstep(0.25, 0.75, lum)) * u_split
         + vec3(1.0) * (1.0 - u_split);
    // Micro sparkle: hashed glints that flicker on strong speculars.
    float spec = smoothstep(0.78, 0.95, lum);
    vec2 cell = floor(v_uv * vec2(u_tex_w, u_tex_h) / 3.0);
    float g = hash(cell + floor(u_time * 8.0));
    rgb += vec3(1.0) * step(0.985, g) * spec * u_sparkle * 0.8;
    frag = vec4(clamp(rgb, 0.0, 1.0), c.a);
}
