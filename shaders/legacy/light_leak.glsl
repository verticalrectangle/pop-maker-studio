// Metal transpile source for the legacy FXType::LightLeak pass. Desktop
// source of truth: k_leak_frag in src/fx_shader.cpp — keep in sync. Uniform
// names follow the fx_chain param keys (leak_intensity / leak_speed).
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_leak_intensity;
uniform float u_leak_speed;
uniform float u_time;

void main() {
    vec4 src = texture(u_tex, v_uv);
    float t = u_time * u_leak_speed;
    vec2 c1 = vec2(0.5 + 0.3*sin(t*0.7), 0.3 + 0.2*cos(t*0.5));
    vec2 c2 = vec2(0.2 + 0.4*cos(t*0.4), 0.7 + 0.3*sin(t*0.6));
    float d1 = 1.0 - clamp(length(v_uv - c1) * 2.5, 0.0, 1.0);
    float d2 = 1.0 - clamp(length(v_uv - c2) * 2.0, 0.0, 1.0);
    vec3 leak = vec3(1.0, 0.6, 0.2) * pow(d1, 3.0) + vec3(0.8, 0.2, 0.6) * pow(d2, 3.0);
    frag = vec4(clamp(src.rgb + leak * u_leak_intensity, 0.0, 1.0), src.a);
}
