#version 300 es
// Scanlines / retro CRT — dark horizontal bands.
precision highp float;
uniform sampler2D u_tex;
uniform vec2 u_res;
uniform float u_amount;   // 0..1 strength
uniform float u_p0;       // density (default 2.0)
in vec2 v_uv;
out vec4 frag;
void main() {
  vec4 col = texture(u_tex, v_uv);
  float line = mod(v_uv.y * u_res.y / u_p0, 2.0);
  float mask = (line < 1.0) ? 1.0 : (1.0 - u_amount * 0.7);
  // slight green tint for retro feel
  vec3 tinted = col.rgb * vec3(0.95, 1.03, 0.92);
  frag = vec4(mix(col.rgb, tinted * mask, u_amount), col.a);
}
