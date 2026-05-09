#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_strength;
uniform float u_saturation;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Boost saturation aggressively
    vec3 sat = mix(vec3(lum), col.rgb, u_saturation);
    // Remap hues toward pink/magenta and teal
    // Hot pink shadows, teal highlights
    float shadow = smoothstep(0.5, 0.0, lum);
    float hi = smoothstep(0.5, 1.0, lum);
    vec3 pink_tint = vec3(1.0, 0.2, 0.6);
    vec3 teal_tint = vec3(0.1, 0.9, 0.8);
    vec3 result = sat
                + pink_tint * shadow * u_strength * 0.4
                + teal_tint * hi * u_strength * 0.3;
    // Slight contrast pump
    result = clamp((result - 0.5) * 1.15 + 0.5, 0.0, 1.0);
    frag = vec4(result, col.a);
}
