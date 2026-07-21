#version 300 es
// Glitch — random block horizontal displacement.
precision highp float;
uniform sampler2D u_tex;
uniform vec2 u_res;
uniform float u_time;
uniform float u_amount;   // 0..1 intensity
uniform float u_p0;       // speed (default 4)
in vec2 v_uv;
out vec4 frag;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
void main() {
  float t = floor(u_time * u_p0 * 8.0);
  vec2 block = floor(v_uv * vec2(12.0, 20.0));
  float r = hash(block + vec2(t, t * 0.7));
  float blk = step(1.0 - u_amount * 0.8, r);
  float shift = (hash(block + vec2(t * 1.3, 0.0)) * 2.0 - 1.0) * blk * 0.18 * u_amount;
  frag = texture(u_tex, v_uv + vec2(shift, 0.0));
}
