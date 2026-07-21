#version 300 es
// Fade to black — simple multiply toward black.
precision highp float;
uniform sampler2D u_tex;
uniform vec2 u_res;
uniform float u_amount;   // 0..1 (1 = fully black)
in vec2 v_uv;
out vec4 frag;
void main() {
  vec4 col = texture(u_tex, v_uv);
  frag = vec4(col.rgb * (1.0 - u_amount), col.a);
}
