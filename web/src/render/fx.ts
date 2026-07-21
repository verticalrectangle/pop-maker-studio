// FX shader registry — GLSL ES 3.00 fragment bodies for post-process passes.
// Each FxDef.frag is the body AFTER the standard uniform preamble the compositor
// injects (`#version 300 es`, precision, u_tex/u_res/u_time/u_amount, and the
// effect's own uniforms listed in `uniforms`). The body declares `in vec2 v_uv;`
// and `out vec4 frag;` and a `void main()`.

export interface FxDef {
  /** Snake_case effect name, matches desktop shader basename. */
  name: string;
  /** Named uniforms (beyond the standard u_tex/u_res/u_time/u_amount) the body needs. */
  uniforms: string[];
  /** Default values for the named uniforms (and u_p0..u_p3 fallback slots). */
  defaults: Record<string, number>;
  /** GLSL ES 3.00 fragment body. */
  frag: string;
}

const STANDARD_UNIFORMS = ["u_tex", "u_res", "u_time", "u_amount"];

function uniformDecls(uniforms: string[]): string {
  return uniforms
    .filter((u) => !STANDARD_UNIFORMS.includes(u))
    .map((u) => `uniform float ${u};`)
    .join("\n");
}

/** Build the full fragment shader source for an effect. */
export function buildFxFrag(def: FxDef): string {
  return `#version 300 es
precision highp float;
uniform sampler2D u_tex;
uniform vec2 u_res;
uniform float u_time;
uniform float u_amount;
${uniformDecls(def.uniforms)}
in vec2 v_uv;
out vec4 frag;
${def.frag}`;
}

const registry = new Map<string, FxDef>();

export function registerFx(def: FxDef): void {
  registry.set(def.name, def);
}

export function getFx(name: string): FxDef | undefined {
  return registry.get(name);
}

export function listFx(): string[] {
  return [...registry.keys()];
}

/** Resolve a uniform value: explicit param, then default, then 0. */
export function uniformValue(
  def: FxDef,
  name: string,
  params: Record<string, number> | undefined,
): number {
  if (params && name in params) return params[name]!;
  if (name in def.defaults) return def.defaults[name]!;
  return 0;
}

// ---------------------------------------------------------------------------
// Core effect set — ported from desktop shaders/*.glsl to GLSL ES 3.00.
// Each frag body below omits the preamble (added by buildFxFrag).
// ---------------------------------------------------------------------------

registerFx({
  name: "vignette",
  uniforms: ["u_p0", "u_p1"],
  defaults: { u_p0: 0.3, u_p1: 0.6 },
  frag: `void main() {
  vec4 col = texture(u_tex, v_uv);
  vec2 d = v_uv - 0.5;
  float r = length(d) * 1.4142;
  float v = smoothstep(u_p0, u_p0 + u_p1, r);
  frag = vec4(col.rgb * (1.0 - v * u_amount), col.a);
}`,
});

registerFx({
  name: "pixelate",
  uniforms: ["u_p0"],
  defaults: { u_p0: 8 },
  frag: `void main() {
  float px = max(1.0, u_p0 * (0.5 + u_amount));
  vec2 cell = px / u_res;
  vec2 snapped = floor(v_uv / cell) * cell + cell * 0.5;
  frag = texture(u_tex, snapped);
}`,
});

registerFx({
  name: "blur",
  uniforms: ["u_p0"],
  defaults: { u_p0: 6 },
  frag: `void main() {
  vec2 px = 1.0 / u_res;
  float r = max(1.0, u_p0) * (0.3 + u_amount);
  float o1 = r * 0.25, o2 = r * 0.5, o3 = r * 0.75, o4 = r;
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
  vec4 b = c * 0.22
    + (s1+s2+s9+sA) * 0.17
    + (s3+s4+sB+sC) * 0.12
    + (s5+s6+sD+sE) * 0.07
    + (s7+s8+sF+sG) * 0.04;
  frag = vec4(clamp(b.rgb, 0.0, 1.0), c.a);
}`,
});

registerFx({
  name: "color_adjust",
  uniforms: ["u_p0", "u_p1", "u_p2"],
  defaults: { u_p0: 0, u_p1: 0, u_p2: 0 },
  frag: `void main() {
  vec4 col = texture(u_tex, v_uv);
  vec3 rgb = col.rgb;
  rgb += u_p0 * u_amount;
  rgb = (rgb - 0.5) * (1.0 + u_p1 * u_amount) + 0.5;
  float l = dot(rgb, vec3(0.299, 0.587, 0.114));
  rgb = mix(vec3(l), rgb, 1.0 + u_p2 * u_amount);
  frag = vec4(clamp(rgb, 0.0, 1.0), col.a);
}`,
});

registerFx({
  name: "chromatic_aberration",
  uniforms: ["u_p0"],
  defaults: { u_p0: 1 },
  frag: `void main() {
  vec2 center = v_uv - 0.5;
  float dist = length(center);
  vec2 offset = center * dist * u_amount * 0.06 * u_p0;
  float r = texture(u_tex, v_uv + offset).r;
  float g = texture(u_tex, v_uv).g;
  float b = texture(u_tex, v_uv - offset).b;
  frag = vec4(r, g, b, texture(u_tex, v_uv).a);
}`,
});

registerFx({
  name: "glitch",
  uniforms: ["u_p0"],
  defaults: { u_p0: 4 },
  frag: `float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
void main() {
  float t = floor(u_time * u_p0 * 8.0);
  vec2 block = floor(v_uv * vec2(12.0, 20.0));
  float r = hash(block + vec2(t, t * 0.7));
  float blk = step(1.0 - u_amount * 0.8, r);
  float shift = (hash(block + vec2(t * 1.3, 0.0)) * 2.0 - 1.0) * blk * 0.18 * u_amount;
  frag = texture(u_tex, v_uv + vec2(shift, 0.0));
}`,
});

registerFx({
  name: "scanlines",
  uniforms: ["u_p0"],
  defaults: { u_p0: 2 },
  frag: `void main() {
  vec4 col = texture(u_tex, v_uv);
  float line = mod(v_uv.y * u_res.y / u_p0, 2.0);
  float mask = (line < 1.0) ? 1.0 : (1.0 - u_amount * 0.7);
  vec3 tinted = col.rgb * vec3(0.95, 1.03, 0.92);
  frag = vec4(mix(col.rgb, tinted * mask, u_amount), col.a);
}`,
});

registerFx({
  name: "skin_smooth",
  uniforms: ["u_p0", "u_p1"],
  defaults: { u_p0: 4, u_p1: 0.5 },
  frag: `float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float skin_mask(vec3 c, float tone) {
  float cb = 0.5 - 0.168736 * c.r - 0.331264 * c.g + 0.5 * c.b;
  float cr = 0.5 + 0.5 * c.r - 0.418688 * c.g - 0.081312 * c.b;
  float w = 0.02 + 0.06 * tone;
  float mb = smoothstep(0.302 - w, 0.302 + w, cb) * (1.0 - smoothstep(0.498 - w, 0.498 + w, cb));
  float mr = smoothstep(0.522 - w, 0.522 + w, cr) * (1.0 - smoothstep(0.678 - w, 0.678 + w, cr));
  return mb * mr;
}
void main() {
  vec2 px = 1.0 / u_res;
  vec4 center = texture(u_tex, v_uv);
  float mask = skin_mask(center.rgb, u_p1) * u_amount;
  if (mask < 0.01) { frag = center; return; }
  float lc = luma(center.rgb);
  vec3 acc = center.rgb;
  float wsum = 1.0;
  float r = max(1.0, u_p0);
  for (float dy = -2.0; dy <= 2.0; dy += 1.0) {
    for (float dx = -2.0; dx <= 2.0; dx += 1.0) {
      if (dx == 0.0 && dy == 0.0) continue;
      vec2 off = vec2(dx, dy) * (r * 0.5) * px;
      vec3 s = texture(u_tex, v_uv + off).rgb;
      float dl = abs(luma(s) - lc);
      float wr = exp(-dl * dl * 60.0);
      float ws = exp(-(dx * dx + dy * dy) * 0.12);
      float w2 = wr * ws;
      acc += s * w2;
      wsum += w2;
    }
  }
  vec3 smoothed = acc / wsum;
  frag = vec4(mix(center.rgb, smoothed, mask), center.a);
}`,
});

registerFx({
  name: "acid_trip",
  uniforms: ["u_p0", "u_p1"],
  defaults: { u_p0: 1.5, u_p1: 0.5 },
  frag: `vec3 hue_rotate(vec3 c, float rad) {
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
}`,
});

registerFx({
  name: "neon_edge",
  uniforms: ["u_p0", "u_p1", "u_p2"],
  defaults: { u_p0: 0.5, u_p1: 1.2, u_p2: 0.5 },
  frag: `void main() {
  vec2 px = 1.0 / u_res;
  vec3 tl = texture(u_tex, v_uv + vec2(-px.x, -px.y)).rgb;
  vec3 tc = texture(u_tex, v_uv + vec2(0.0, -px.y)).rgb;
  vec3 tr = texture(u_tex, v_uv + vec2(px.x, -px.y)).rgb;
  vec3 ml = texture(u_tex, v_uv + vec2(-px.x, 0.0)).rgb;
  vec3 mr = texture(u_tex, v_uv + vec2(px.x, 0.0)).rgb;
  vec3 bl = texture(u_tex, v_uv + vec2(-px.x, px.y)).rgb;
  vec3 bc = texture(u_tex, v_uv + vec2(0.0, px.y)).rgb;
  vec3 br = texture(u_tex, v_uv + vec2(px.x, px.y)).rgb;
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
}`,
});

registerFx({
  name: "kaleidoscope",
  uniforms: ["u_p0", "u_p1", "u_p2"],
  defaults: { u_p0: 8, u_p1: 0, u_p2: 1 },
  frag: `const float PI = 3.14159265;
void main() {
  vec2 c = vec2(0.5);
  vec2 d = (v_uv - c) / max(u_p2, 0.01);
  float angle = atan(d.y, d.x) + u_p1;
  float radius = length(d);
  float segments = mix(2.0, u_p0, u_amount);
  float sector = PI * 2.0 / max(segments, 2.0);
  angle = mod(angle, sector);
  if (angle > sector * 0.5) angle = sector - angle;
  vec2 uv = c + vec2(cos(angle), sin(angle)) * radius;
  uv = abs(fract(uv * 0.5) * 2.0 - 1.0);
  frag = texture(u_tex, uv);
}`,
});

registerFx({
  name: "rgb_shift",
  uniforms: ["u_p0"],
  defaults: { u_p0: 2 },
  frag: `float hash(float p) { return fract(sin(p * 127.1) * 43758.5453); }
void main() {
  float t = u_time * u_p0;
  float ox = (hash(floor(t)) * 2.0 - 1.0) * u_amount * 0.05;
  float oy = (hash(floor(t) + 1.0) * 2.0 - 1.0) * u_amount * 0.02;
  float r = texture(u_tex, v_uv + vec2(ox, oy)).r;
  float g = texture(u_tex, v_uv).g;
  float b = texture(u_tex, v_uv - vec2(ox, oy)).b;
  frag = vec4(r, g, b, texture(u_tex, v_uv).a);
}`,
});

registerFx({
  name: "grain",
  uniforms: ["u_p0"],
  defaults: { u_p0: 1.5 },
  frag: `float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
void main() {
  vec4 col = texture(u_tex, v_uv);
  vec2 gp = floor(v_uv * u_res / max(u_p0, 0.1));
  float g = hash(gp + vec2(u_time * 7.3, u_time * 3.1)) * 2.0 - 1.0;
  float luma = dot(col.rgb, vec3(0.299, 0.587, 0.114));
  float w = 1.0 - abs(luma * 2.0 - 1.0);
  frag = vec4(col.rgb + g * u_amount * w * 0.35, col.a);
}`,
});

registerFx({
  name: "fade_to_black",
  uniforms: [],
  defaults: {},
  frag: `void main() {
  vec4 col = texture(u_tex, v_uv);
  frag = vec4(col.rgb * (1.0 - u_amount), col.a);
}`,
});
