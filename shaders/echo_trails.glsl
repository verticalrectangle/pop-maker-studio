#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_offset;
uniform float u_fade;
uniform float u_angle;
uniform float u_strength;
void main() {
    const float DEG2RAD = 0.017453293;
    float a = u_angle * DEG2RAD;
    vec2 dir = vec2(cos(a), -sin(a)) * u_offset;
    vec4 col = texture(u_tex, v_uv);
    vec4 result = col;
    float wt = 1.0;
    float w = u_fade;
    for (int i = 1; i <= 5; i++) {
        vec2 uv = clamp(v_uv + dir * float(i), 0.0, 1.0);
        vec4 echo = texture(u_tex, uv);
        // Screen blend each echo
        result.rgb = 1.0 - (1.0 - result.rgb) * (1.0 - echo.rgb * w);
        w *= u_fade;
    }
    frag = vec4(clamp(mix(col.rgb, result.rgb, u_strength), 0.0, 1.0), col.a);
}
