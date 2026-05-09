#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_strength;
// Iron/thermal palette: black → blue → cyan → yellow → red → white
vec3 thermal_palette(float t) {
    t = clamp(t, 0.0, 1.0);
    vec3 a = mix(vec3(0.0, 0.0, 0.0), vec3(0.0, 0.0, 1.0),   smoothstep(0.00, 0.20, t));
    vec3 b = mix(a,                    vec3(0.0, 1.0, 1.0),   smoothstep(0.20, 0.40, t));
    vec3 c = mix(b,                    vec3(1.0, 1.0, 0.0),   smoothstep(0.40, 0.65, t));
    vec3 d = mix(c,                    vec3(1.0, 0.0, 0.0),   smoothstep(0.65, 0.85, t));
    return  mix(d,                     vec3(1.0, 1.0, 1.0),   smoothstep(0.85, 1.00, t));
}
void main() {
    vec4 col = texture(u_tex, v_uv);
    float luma = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    vec3 heat = thermal_palette(luma);
    frag = vec4(mix(col.rgb, heat, u_strength), col.a);
}
