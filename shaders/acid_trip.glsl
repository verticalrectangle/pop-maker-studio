// Acid Trip — time-cycling hue rotation banded by luminance + sine UV wobble.
// Different brightness bands rotate at different speeds so the image
// iridesces instead of just color-cycling.
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_time;
uniform float u_trip;    // 0..1 hue-cycle depth
uniform float u_speed;   // 0..4 cycle speed
uniform float u_wobble;  // 0..1 UV wobble

vec3 hue_rotate(vec3 c, float rad) {
    float ch = cos(rad), sh = sin(rad);
    mat3 m = mat3(
        0.299+0.701*ch+0.168*sh, 0.299-0.299*ch-0.328*sh, 0.299-0.299*ch+1.250*sh,
        0.587-0.587*ch+0.330*sh, 0.587+0.413*ch+0.035*sh, 0.587-0.587*ch-1.050*sh,
        0.114-0.114*ch-0.497*sh, 0.114-0.114*ch+0.292*sh, 0.114+0.886*ch-0.203*sh
    );
    return clamp(m * c, 0.0, 1.0);
}

void main() {
    float t = u_time * u_speed;
    vec2 uv = v_uv;
    uv.x += sin(uv.y * 9.0 + t * 1.3) * 0.02 * u_wobble;
    uv.y += cos(uv.x * 7.0 + t * 1.7) * 0.02 * u_wobble;
    vec4 c = texture(u_tex, clamp(uv, 0.0, 1.0));
    float lum = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    // Band the rotation by luminance so shadows and highlights drift apart.
    float band = floor(lum * 4.0) / 4.0;
    float rot = (t * 0.8 + band * 2.5 + lum * 1.5) * u_trip;
    vec3 rgb = hue_rotate(c.rgb, rot);
    // Saturation push proportional to trip depth.
    float l2 = dot(rgb, vec3(0.299, 0.587, 0.114));
    rgb = mix(vec3(l2), rgb, 1.0 + u_trip * 0.6);
    frag = vec4(clamp(rgb, 0.0, 1.0), c.a);
}
