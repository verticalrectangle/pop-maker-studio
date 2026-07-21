#version 300 es
// BodyFX mask composite — applies an effect only inside the body mask.
// u_mask is a single-channel coverage texture (1 = body, 0 = background).
// The effect fragment is blended: result = mix(original, effect, mask * amount).
precision highp float;
uniform sampler2D u_tex;     // original composited frame
uniform sampler2D u_mask;    // body coverage mask
uniform sampler2D u_effect;  // pre-rendered effect result
uniform vec2 u_res;
uniform float u_amount;      // 0..1 blend strength
in vec2 v_uv;
out vec4 frag;
void main() {
  vec4 orig = texture(u_tex, v_uv);
  vec4 fx = texture(u_effect, v_uv);
  float m = texture(u_mask, v_uv).r;
  frag = vec4(mix(orig.rgb, fx.rgb, m * u_amount), orig.a);
}
