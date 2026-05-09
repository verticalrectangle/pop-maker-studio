#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_intensity;
uniform float u_speed;
uniform float u_time;
float hash(float p) { return fract(sin(p * 127.1) * 43758.5453); }
void main() {
    // Animating split — direction and magnitude vary over time
    float t = u_time * u_speed;
    float ox = (hash(floor(t))       * 2.0 - 1.0) * u_intensity * 0.05;
    float oy = (hash(floor(t) + 1.0) * 2.0 - 1.0) * u_intensity * 0.02;
    float r = texture(u_tex, v_uv + vec2( ox,  oy)).r;
    float g = texture(u_tex, v_uv                ).g;
    float b = texture(u_tex, v_uv - vec2( ox,  oy)).b;
    frag = vec4(r, g, b, texture(u_tex, v_uv).a);
}
