#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_freq_x;
uniform float u_freq_y;
uniform float u_amplitude;
uniform float u_speed;
uniform float u_time;
void main() {
    vec2 uv = v_uv;
    uv.x += sin(v_uv.y * u_freq_x + u_time * u_speed * 1.1) * u_amplitude;
    uv.y += sin(v_uv.x * u_freq_y + u_time * u_speed * 0.9) * u_amplitude;
    frag = texture(u_tex, clamp(uv, 0.0, 1.0));
}
