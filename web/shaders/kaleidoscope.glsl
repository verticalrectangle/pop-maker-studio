#version 300 es
// Kaleidoscope / mirror — radial sector folding.
precision highp float;
const float PI = 3.14159265;
uniform sampler2D u_tex;
uniform vec2 u_res;
uniform float u_amount;   // 0..1 strength
uniform float u_p0;       // segments (default 8)
uniform float u_p1;       // rotation (default 0)
uniform float u_p2;       // zoom (default 1)
in vec2 v_uv;
out vec4 frag;
void main() {
  vec2 c = vec2(0.5);
  vec2 d = (v_uv - c) / max(u_p2, 0.01);
  float angle = atan(d.y, d.x) + u_p1;
  float radius = length(d);
  float segments = mix(2.0, u_p0, u_amount);
  float sector = PI * 2.0 / max(segments, 2.0);
  angle = mod(angle, sector);
  if (angle > sector * 0.5) angle = sector - angle;
  vec2 uv = c + vec2(cos(angle), sin(angle)) * radius;
  uv = abs(fract(uv * 0.5) * 2.0 - 1.0);
  frag = texture(u_tex, uv);
}
