// Fractal Mirror — recursive kaleidoscopic folding with slow zoom drift.
// Each iteration mirrors the plane about a rotating axis, so the frame turns
// into a living mandala built from itself.
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_time;
uniform float u_folds;   // 1..6 fold iterations
uniform float u_drift;   // 0..1 rotation drift speed
uniform float u_zoom;    // 0..1 breathing zoom depth

void main() {
    vec2 p = v_uv - 0.5;
    float t = u_time * u_drift;
    float z = 1.0 + sin(u_time * 0.5) * 0.25 * u_zoom;
    p *= z;
    int folds = int(clamp(u_folds, 1.0, 6.0));
    for (int i = 0; i < 6; i++) {
        if (i >= folds) break;
        float a = t * (0.3 + 0.13 * float(i)) + float(i) * 0.7853;
        vec2 ax = vec2(cos(a), sin(a));
        // Reflect across the axis if on the negative side.
        float d = dot(p, vec2(-ax.y, ax.x));
        p -= 2.0 * min(0.0, d) * vec2(-ax.y, ax.x);
        p *= 1.08;                       // slight zoom per fold
    }
    vec2 uv = fract(p + 0.5);
    // Mirror-tile so wrap seams are symmetric, not hard cuts.
    uv = abs(uv * 2.0 - 1.0);
    frag = texture(u_tex, uv);
}
