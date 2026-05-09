#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_frequency;
uniform float u_amplitude;
uniform float u_speed;
uniform float u_time;
void main() {
    vec2 c = vec2(0.5, 0.5);
    vec2 d = v_uv - c;
    float dist = length(d) + 0.001;
    float wave = sin(dist * u_frequency - u_time * u_speed) * u_amplitude;
    vec2 uv = clamp(v_uv + normalize(d) * wave, 0.0, 1.0);
    frag = texture(u_tex, uv);
}
