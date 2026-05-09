#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_shadow_teal;
uniform float u_hi_orange;
uniform float u_contrast;
void main() {
    vec4 col = texture(u_tex, v_uv);
    vec3 c = col.rgb;
    // Contrast crush
    c = (c - 0.5) * u_contrast + 0.5;
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    // Shadow: push toward deep blue-teal
    vec3 shadow_col = vec3(0.02, 0.06, 0.18);
    float shadow_mask = smoothstep(0.45, 0.0, lum);
    c = mix(c, shadow_col + c * 0.4, shadow_mask * u_shadow_teal);
    // Highlight: push toward warm orange
    vec3 hi_col = vec3(1.0, 0.7, 0.35);
    float hi_mask = smoothstep(0.65, 1.0, lum);
    c = mix(c, hi_col * lum, hi_mask * u_hi_orange * 0.6);
    // Subtle cyan saturation in mids
    vec3 mid_teal = vec3(0.0, 0.9, 1.0);
    float mid_mask = 1.0 - shadow_mask - hi_mask;
    float lum2 = dot(c, vec3(0.2126,0.7152,0.0722));
    c = mix(c, mix(vec3(lum2), c, 1.0) * mix(vec3(1.0), mid_teal, 0.2), mid_mask * u_shadow_teal * 0.4);
    frag = vec4(clamp(c, 0.0, 1.0), col.a);
}
