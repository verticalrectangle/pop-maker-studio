#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_shadow_r, u_shadow_g, u_shadow_b;
uniform float u_highlight_r, u_highlight_g, u_highlight_b;
uniform float u_strength;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float luma = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    vec3 shadow    = vec3(u_shadow_r,    u_shadow_g,    u_shadow_b);
    vec3 highlight = vec3(u_highlight_r, u_highlight_g, u_highlight_b);
    vec3 duotone_result = mix(shadow, highlight, luma);
    frag = vec4(mix(col.rgb, duotone_result, u_strength), col.a);
}
