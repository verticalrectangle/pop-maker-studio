#version 300 es
// Pixelate — snap UVs to a coarse grid.
precision highp float;
uniform sampler2D u_tex;
uniform vec2 u_res;
uniform float u_amount;   // 0..1 strength
uniform float u_p0;       // pixel size in px (default 8)
in vec2 v_uv;
out vec4 frag;
void main() {
  float px = max(1.0, u_p0 * (0.5 + u_amount));
  vec2 cell = px / u_res;
  vec2 snapped = floor(v_uv / cell) * cell + cell * 0.5;
  frag = texture(u_tex, snapped);
}
