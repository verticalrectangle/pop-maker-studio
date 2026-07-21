#version 300 es
// RGB shift — animating channel split with time-varying direction.
precision highp float;
uniform sampler2D u_tex;
uniform vec2 u_res;
uniform float u_time;
uniform float u_amount;   // 0..1 intensity
uniform float u_p0;       // speed (default 2)
in vec2 v_uv;
out vec4 frag;
float hash(float p) { return fract(sin(p * 127.1) * 43758.5453); }
void main() {
  float t = u_time * u_p0;
  float ox = (hash(floor(t)) * 2.0 - 1.0) * u_amount * 0.05;
  float oy = (hash(floor(t) + 1.0) * 2.0 - 1.0) * u_amount * 0.02;
  float r = texture(u_tex, v_uv + vec2(ox, oy)).r;
  float g = texture(u_tex, v_uv).g;
  float b = texture(u_tex, v_uv - vec2(ox, oy)).b;
  frag = vec4(r, g, b, texture(u_tex, v_uv).a);
}
