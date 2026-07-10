// Night Drive — blue-hour city grade: crushed cool shadows, sodium-orange
// highlight split, and anamorphic horizontal flares off the brightest lights.
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_night;   // 0..1 grade strength
uniform float u_sodium;  // 0..1 orange highlight push
uniform float u_flare;   // 0..1 anamorphic flare strength

void main() {
    vec2 px = vec2(1.0 / u_tex_w, 1.0 / u_tex_h);
    vec4 c = texture(u_tex, v_uv);
    // Wide anamorphic flare: long horizontal reach, hard threshold.
    vec3 flare = vec3(0.0);
    for (int i = -8; i <= 8; i++) {
        vec3 s = texture(u_tex, clamp(v_uv + vec2(float(i) * 7.0 * px.x, 0.0), 0.0, 1.0)).rgb;
        float b = max(max(s.r, s.g), s.b);
        flare += s * smoothstep(0.75, 0.98, b) * (1.0 - abs(float(i)) / 9.0);
    }
    flare /= 8.0;
    float lum = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    // Cool the shadows/mids, crush blacks slightly.
    vec3 cool = c.rgb * vec3(0.75, 0.85, 1.2);
    cool = max(cool - 0.03, 0.0) * 1.03;
    // Sodium-vapor highlights.
    vec3 sodium = vec3(1.0, 0.62, 0.25);
    vec3 rgb = mix(c.rgb, cool, u_night * (1.0 - smoothstep(0.5, 0.9, lum)));
    rgb = mix(rgb, rgb * sodium * 1.25, u_sodium * smoothstep(0.55, 0.9, lum));
    rgb += flare * vec3(0.5, 0.7, 1.2) * u_flare;
    frag = vec4(clamp(rgb, 0.0, 1.0), c.a);
}
