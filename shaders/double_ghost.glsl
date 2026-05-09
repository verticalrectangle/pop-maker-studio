#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_offset;
uniform float u_opacity;
uniform float u_angle;
void main() {
    const float DEG2RAD = 0.017453293;
    float a = u_angle * DEG2RAD;
    vec2 dir = vec2(cos(a), sin(a)) * u_offset;
    vec4 col = texture(u_tex, v_uv);
    vec4 ghost = texture(u_tex, clamp(v_uv + dir, 0.0, 1.0));
    // Additive screen blend ghost
    vec3 screen = 1.0 - (1.0 - col.rgb) * (1.0 - ghost.rgb * u_opacity);
    // Slight cyan tint on the ghost
    ghost.rgb *= vec3(0.7, 0.9, 1.2);
    vec3 result = mix(screen, col.rgb * (1.0-u_opacity) + ghost.rgb * u_opacity, 0.5);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
