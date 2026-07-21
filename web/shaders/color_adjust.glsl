#version 300 es
// Color adjust — brightness, contrast, saturation in one pass.
precision highp float;
uniform sampler2D u_tex;
uniform vec2 u_res;
uniform float u_amount;    // master strength 0..1
uniform float u_p0;        // brightness  -1..1 (default 0)
uniform float u_p1;        // contrast    -1..1 (default 0)
uniform float u_p2;        // saturation  -1..1 (default 0)
in vec2 v_uv;
out vec4 frag;
void main() {
  vec4 col = texture(u_tex, v_uv);
  vec3 rgb = col.rgb;
  // brightness
  rgb += u_p0 * u_amount;
  // contrast around 0.5
  rgb = (rgb - 0.5) * (1.0 + u_p1 * u_amount) + 0.5;
  // saturation
  float l = dot(rgb, vec3(0.299, 0.587, 0.114));
  rgb = mix(vec3(l), rgb, 1.0 + u_p2 * u_amount);
  frag = vec4(clamp(rgb, 0.0, 1.0), col.a);
}
