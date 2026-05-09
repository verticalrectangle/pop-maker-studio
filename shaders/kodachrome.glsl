#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_saturation;
uniform float u_reds;
uniform float u_shadows;
uniform float u_strength;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Saturation boost
    vec3 sat = mix(vec3(lum), col.rgb, u_saturation);
    // Boost reds/magentas (Kodachrome characteristic)
    float red_dominant = max(sat.r - sat.g, max(sat.r - sat.b, 0.0));
    sat.r = min(sat.r + red_dominant * u_reds * 0.4, 1.0);
    sat.g = max(sat.g - red_dominant * u_reds * 0.1, 0.0);
    // Golden shadow lift (warm shadow color)
    float shadow_mask = smoothstep(0.35, 0.0, lum);
    vec3 gold = vec3(0.12, 0.08, 0.0);
    sat = sat + gold * shadow_mask * u_shadows;
    // Slight blue desaturation (Kodachrome tends toward warm)
    sat.b = mix(sat.b, sat.b * 0.85, u_reds * 0.3);
    frag = vec4(clamp(mix(col.rgb, sat, u_strength), 0.0, 1.0), col.a);
}
