#version 300 es
// Chromatic aberration — radial RGB split toward the edges.
precision highp float;
uniform sampler2D u_tex;
uniform vec2 u_res;
uniform float u_amount;   // 0..1 strength
uniform float u_p0;       // scale (default 1.0)
in vec2 v_uv;
out vec4 frag;
void main() {
  vec2 center = v_uv - 0.5;
  float dist = length(center);
  vec2 offset = center * dist * u_amount * 0.06 * u_p0;
  float r = texture(u_tex, v_uv + offset).r;
  float g = texture(u_tex, v_uv).g;
  float b = texture(u_tex, v_uv - offset).b;
  frag = vec4(r, g, b, texture(u_tex, v_uv).a);
}
