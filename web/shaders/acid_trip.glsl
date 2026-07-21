#version 300 es
// Acid Trip — time-cycling hue rotation banded by luminance + sine UV wobble.
precision highp float;
uniform sampler2D u_tex;
uniform vec2 u_res;
uniform float u_time;
uniform float u_amount;   // 0..1 hue-cycle depth
uniform float u_p0;       // speed 0..4 (default 1.5)
uniform float u_p1;       // wobble 0..1 (default 0.5)
in vec2 v_uv;
out vec4 frag;
vec3 hue_rotate(vec3 c, float rad) {
  float ch = cos(rad), sh = sin(rad);
  mat3 m = mat3(
    0.299 + 0.701 * ch + 0.168 * sh, 0.299 - 0.299 * ch - 0.328 * sh, 0.299 - 0.299 * ch + 1.250 * sh,
    0.587 - 0.587 * ch + 0.330 * sh, 0.587 + 0.413 * ch + 0.035 * sh, 0.587 - 0.587 * ch - 1.050 * sh,
    0.114 - 0.114 * ch - 0.497 * sh, 0.114 - 0.114 * ch + 0.292 * sh, 0.114 + 0.886 * ch - 0.203 * sh
  );
  return clamp(m * c, 0.0, 1.0);
}
void main() {
  float t = u_time * u_p0;
  vec2 uv = v_uv;
  uv.x += sin(uv.y * 9.0 + t * 1.3) * 0.02 * u_p1;
  uv.y += cos(uv.x * 7.0 + t * 1.7) * 0.02 * u_p1;
  vec4 c = texture(u_tex, clamp(uv, 0.0, 1.0));
  float lum = dot(c.rgb, vec3(0.299, 0.587, 0.114));
  float band = floor(lum * 4.0) / 4.0;
  float rot = (t * 0.8 + band * 2.5 + lum * 1.5) * u_amount;
  vec3 rgb = hue_rotate(c.rgb, rot);
  float l2 = dot(rgb, vec3(0.299, 0.587, 0.114));
  rgb = mix(vec3(l2), rgb, 1.0 + u_amount * 0.6);
  frag = vec4(clamp(rgb, 0.0, 1.0), c.a);
}
