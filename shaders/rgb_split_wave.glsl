#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_amplitude;
uniform float u_frequency;
uniform float u_speed;
uniform float u_time;
void main() {
    float t = u_time * u_speed;
    float pi2 = 6.28318;
    // Each channel offset by a wave with different phase
    vec2 offsetR = vec2(sin(v_uv.y * u_frequency * pi2 + t)       * u_amplitude, 0.0);
    vec2 offsetG = vec2(sin(v_uv.y * u_frequency * pi2 + t + 2.09) * u_amplitude * 0.6, 0.0);
    vec2 offsetB = vec2(sin(v_uv.y * u_frequency * pi2 + t + 4.19) * u_amplitude * 1.3, 0.0);
    float r = texture(u_tex, clamp(v_uv + offsetR, 0.0, 1.0)).r;
    float g = texture(u_tex, clamp(v_uv + offsetG, 0.0, 1.0)).g;
    float b = texture(u_tex, clamp(v_uv + offsetB, 0.0, 1.0)).b;
    vec4 orig = texture(u_tex, v_uv);
    frag = vec4(r, g, b, orig.a);
}
