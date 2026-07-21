#version 300 es
// Neon edge glow — Sobel edge detection + neon-colored bloom.
precision highp float;
uniform sampler2D u_tex;
uniform vec2 u_res;
uniform float u_amount;   // 0..1 strength
uniform float u_p0;       // threshold (default 0.5)
uniform float u_p1;       // glow (default 1.2)
uniform float u_p2;       // hue 0..1 (default 0.5)
in vec2 v_uv;
out vec4 frag;
void main() {
  vec2 px = 1.0 / u_res;
  vec3 tl = texture(u_tex, v_uv + vec2(-px.x, -px.y)).rgb;
  vec3 tc = texture(u_tex, v_uv + vec2(    0.0, -px.y)).rgb;
  vec3 tr = texture(u_tex, v_uv + vec2( px.x, -px.y)).rgb;
  vec3 ml = texture(u_tex, v_uv + vec2(-px.x,     0.0)).rgb;
  vec3 mr = texture(u_tex, v_uv + vec2( px.x,     0.0)).rgb;
  vec3 bl = texture(u_tex, v_uv + vec2(-px.x,  px.y)).rgb;
  vec3 bc = texture(u_tex, v_uv + vec2(    0.0,  px.y)).rgb;
  vec3 br = texture(u_tex, v_uv + vec2( px.x,  px.y)).rgb;
  vec3 gx = -tl - 2.0 * ml - bl + tr + 2.0 * mr + br;
  vec3 gy = -tl - 2.0 * tc - tr + bl + 2.0 * bc + br;
  float edge = length(vec2(length(gx), length(gy)));
  edge = smoothstep(u_p0, u_p0 + 0.2, edge);
  vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
  vec3 p = abs(fract(vec3(u_p2) + K.xyz) * 6.0 - K.www);
  vec3 neon_col = clamp(p - K.xxx, 0.0, 1.0);
  vec4 col = texture(u_tex, v_uv);
  vec3 result = col.rgb * 0.15 + neon_col * edge * u_p1 * u_amount;
  vec3 bloom = vec3(0.0);
  for (int i = 1; i <= 4; i++) {
    float r = float(i) * 2.0;
    bloom += neon_col * edge / (r * r + 1.0);
  }
  result += bloom * u_p1 * u_amount * 0.3;
  frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
