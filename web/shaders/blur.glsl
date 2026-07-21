#version 300 es
// Gaussian blur — 9-tap separable-style single-pass approximation.
precision highp float;
uniform sampler2D u_tex;
uniform vec2 u_res;
uniform float u_amount;   // 0..1 strength
uniform float u_p0;       // radius in px (default 6)
in vec2 v_uv;
out vec4 frag;
void main() {
  vec2 px = 1.0 / u_res;
  float r = max(1.0, u_p0) * (0.3 + u_amount);
  float o1 = r * 0.25;
  float o2 = r * 0.5;
  float o3 = r * 0.75;
  float o4 = r;
  vec4 c  = texture(u_tex, v_uv);
  vec4 s1 = texture(u_tex, v_uv + vec2(o1, 0.0) * px);
  vec4 s2 = texture(u_tex, v_uv - vec2(o1, 0.0) * px);
  vec4 s3 = texture(u_tex, v_uv + vec2(o2, 0.0) * px);
  vec4 s4 = texture(u_tex, v_uv - vec2(o2, 0.0) * px);
  vec4 s5 = texture(u_tex, v_uv + vec2(o3, 0.0) * px);
  vec4 s6 = texture(u_tex, v_uv - vec2(o3, 0.0) * px);
  vec4 s7 = texture(u_tex, v_uv + vec2(o4, 0.0) * px);
  vec4 s8 = texture(u_tex, v_uv - vec2(o4, 0.0) * px);
  vec4 s9 = texture(u_tex, v_uv + vec2(0.0, o1) * px);
  vec4 sA = texture(u_tex, v_uv - vec2(0.0, o1) * px);
  vec4 sB = texture(u_tex, v_uv + vec2(0.0, o2) * px);
  vec4 sC = texture(u_tex, v_uv - vec2(0.0, o2) * px);
  vec4 sD = texture(u_tex, v_uv + vec2(0.0, o3) * px);
  vec4 sE = texture(u_tex, v_uv - vec2(0.0, o3) * px);
  vec4 sF = texture(u_tex, v_uv + vec2(0.0, o4) * px);
  vec4 sG = texture(u_tex, v_uv - vec2(0.0, o4) * px);
  vec4 blurred = c * 0.22
    + (s1+s2+s9+sA) * 0.17
    + (s3+s4+sB+sC) * 0.12
    + (s5+s6+sD+sE) * 0.07
    + (s7+s8+sF+sG) * 0.04;
  frag = vec4(clamp(blurred.rgb, 0.0, 1.0), c.a);
}
