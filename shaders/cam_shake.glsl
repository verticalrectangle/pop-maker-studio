#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_intensity;   // overall shake strength (0..1)
uniform float u_speed;       // how fast the camera jitters
uniform float u_time;

// 1D value noise in [-1,1] — cheap, smooth, no texture lookups.
float h1(float x) { return fract(sin(x * 12.9898) * 43758.5453); }
float vnoise(float x) {
    float i = floor(x), f = fract(x);
    float u = f * f * (3.0 - 2.0 * f);
    return mix(h1(i), h1(i + 1.0), u) * 2.0 - 1.0;
}

void main() {
    // Continuous handheld jitter (NOT beat-synced): layer two frequencies per
    // axis so the motion feels organic rather than a clean sine.
    float t  = u_time * (1.5 + u_speed * 4.0);
    float dx = vnoise(t)             * 0.6 + vnoise(t * 2.3 + 11.0) * 0.4;
    float dy = vnoise(t * 1.1 + 5.0) * 0.6 + vnoise(t * 2.7 + 23.0) * 0.4;
    float dr = vnoise(t * 0.7 + 31.0);

    float amp  = u_intensity * 0.05;          // translation, fraction of frame
    float zoom = 1.0 + u_intensity * 0.045;   // micro-punch so shaken edges never reveal the border
    float ang  = dr * u_intensity * 0.05;     // small roll (radians)

    vec2 uv = v_uv - 0.5;
    float c = cos(ang), s = sin(ang);
    uv = mat2(c, -s, s, c) * uv;              // roll
    uv /= zoom;                               // zoom in
    uv += vec2(dx, dy) * amp;                 // jitter
    uv += 0.5;

    frag = texture(u_tex, clamp(uv, 0.0, 1.0));
}
