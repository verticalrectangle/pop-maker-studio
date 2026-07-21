#version 300 es
// Vignette — darkened corners, ported to GLSL ES 3.00.
precision highp float;
uniform sampler2D u_tex;
uniform vec2 u_res;
uniform float u_amount;   // 0..1 strength
uniform float u_p0;       // inner radius (default 0.3)
uniform float u_p1;       // outer softness (default 0.6)
in vec2 v_uv;
out vec4 frag;
void main() {
  vec4 col = texture(u_tex, v_uv);
  vec2 d = v_uv - 0.5;
  float r = length(d) * 1.4142;
  float inner = u_p0;
  float outer = u_p0 + u_p1;
  float v = smoothstep(inner, outer, r);
  frag = vec4(col.rgb * (1.0 - v * u_amount), col.a);
}
