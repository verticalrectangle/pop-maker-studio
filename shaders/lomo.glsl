#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_vignette;
uniform float u_saturation;
uniform float u_fade;
uniform float u_strength;
void main() {
    vec4 col = texture(u_tex, v_uv);
    // Saturation boost
    float luma = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    vec3 sat = mix(vec3(luma), col.rgb, u_saturation);
    // Warm fade (lift shadows toward warm tone)
    sat = mix(sat, sat + vec3(u_fade * 0.15, u_fade * 0.05, -u_fade * 0.05), 1.0);
    // Vignette
    vec2 d = (v_uv - 0.5) * vec2(1.0, 1.4);
    float vig = 1.0 - smoothstep(0.3, 0.75, length(d)) * u_vignette;
    vec3 lomo_result = sat * vig;
    frag = vec4(mix(col.rgb, lomo_result, u_strength), col.a);
}
