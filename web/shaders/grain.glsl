#version 300 es
// Film grain / noise — luma-weighted animated grain.
precision highp float;
uniform sampler2D u_tex;
uniform vec2 u_res;
uniform float u_time;
uniform float u_amount;   // 0..1 intensity
uniform float u_p0;       // grain size (default 1.5)
in vec2 v_uv;
out vec4 frag;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
void main() {
  vec4 col = texture(u_tex, v_uv);
  vec2 gp = floor(v_uv * u_res / max(u_p0, 0.1));
  float g = hash(gp + vec2(u_time * 7.3, u_time * 3.1)) * 2.0 - 1.0;
  float luma = dot(col.rgb, vec3(0.299, 0.587, 0.114));
  float w = 1.0 - abs(luma * 2.0 - 1.0);
  frag = vec4(col.rgb + g * u_amount * w * 0.35, col.a);
}
