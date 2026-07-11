// Melt Drip — the frame sags downward in noise-chosen columns with heat-haze
// shimmer, like paint or wax melting off the screen. Stateless (no feedback):
// the melt amount is a function of time + column noise.
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_time;
uniform float u_melt;   // 0..1 sag depth
uniform float u_drip;   // 0..1 column raggedness
uniform float u_haze;   // 0..1 heat shimmer

float hash(float n) { return fract(sin(n) * 43758.5453); }
float noise1(float x) {
    float i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(hash(i), hash(i + 1.0), f);
}

void main() {
    // Column-based sag: each x gets a melt offset from smooth 1-D noise, and
    // pixels lower in the frame sag more (drip fronts).
    float col = noise1(v_uv.x * (8.0 + u_drip * 30.0) + u_time * 0.3);
    float front = pow(v_uv.y, 1.5);
    float sag = u_melt * 0.25 * col * front;
    vec2 uv = v_uv;
    uv.y = clamp(uv.y - sag, 0.0, 1.0);
    // Heat haze: fine horizontal shimmer scaled by haze.
    uv.x += sin(uv.y * 60.0 + u_time * 5.0) * 0.004 * u_haze;
    vec4 c = texture(u_tex, clamp(uv, 0.0, 1.0));
    // Slight warm push in strongly melted zones (molten glow).
    c.rgb += vec3(0.10, 0.04, 0.0) * (sag / max(0.25 * u_melt, 1e-4)) * u_melt;
    frag = clamp(c, 0.0, 1.0);
}
