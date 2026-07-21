// BodyFX — mask-composited post-process. An effect (any registered FxDef) is
// rendered to a scratch buffer, then blended back into the main composited
// frame only where the body mask (from MaskProvider) is non-zero.
//
// The mask composite itself is a GLSL pass (bodyfx_mask.glsl) that mixes
// original → effect by mask * amount. When no mask is available (null), the
// brick falls back to a full-frame effect blend (mask treated as all-ones).
import { buildFxFrag, getFx, uniformValue, type FxDef } from "./fx";

/** Mask composite fragment — blends u_effect over u_tex by u_mask.r * u_amount. */
const MASK_FRAG = `#version 300 es
precision highp float;
uniform sampler2D u_tex;
uniform sampler2D u_mask;
uniform sampler2D u_effect;
uniform vec2 u_res;
uniform float u_amount;
in vec2 v_uv;
out vec4 frag;
void main() {
  vec4 orig = texture(u_tex, v_uv);
  vec4 fx = texture(u_effect, v_uv);
  float m = texture(u_mask, v_uv).r;
  frag = vec4(mix(orig.rgb, fx.rgb, m * u_amount), orig.a);
}`;

/** Full-frame fallback when no mask: blend effect over original by amount. */
const NOMASK_FRAG = `#version 300 es
precision highp float;
uniform sampler2D u_tex;
uniform sampler2D u_mask;
uniform sampler2D u_effect;
uniform vec2 u_res;
uniform float u_amount;
in vec2 v_uv;
out vec4 frag;
void main() {
  vec4 orig = texture(u_tex, v_uv);
  vec4 fx = texture(u_effect, v_uv);
  frag = vec4(mix(orig.rgb, fx.rgb, u_amount), orig.a);
}`;

export interface BodyFxProgram {
  /** Effect fragment program (the per-brick chosen FX). */
  effect: WebGLProgram;
  /** Mask composite program. */
  composite: WebGLProgram;
  /** The FxDef the effect program was built from. */
  def: FxDef;
}

/** Compile a bodyfx brick's programs. Returns null if the effect is unknown. */
export function buildBodyFxProgram(
  gl: WebGL2RenderingContext,
  vs: WebGLShader,
  fxType: string,
  hasMask: boolean,
): BodyFxProgram | null {
  const def = getFx(fxType);
  if (!def) return null;
  const effectFrag = compile(gl, gl.FRAGMENT_SHADER, buildFxFrag(def));
  const compositeFrag = compile(gl, gl.FRAGMENT_SHADER, hasMask ? MASK_FRAG : NOMASK_FRAG);
  const effect = link(gl, vs, effectFrag);
  const composite = link(gl, vs, compositeFrag);
  gl.deleteShader(effectFrag);
  gl.deleteShader(compositeFrag);
  return { effect, composite, def };
}

function compile(gl: WebGL2RenderingContext, type: number, src: string): WebGLShader {
  const sh = gl.createShader(type)!;
  gl.shaderSource(sh, src);
  gl.compileShader(sh);
  if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
    const log = gl.getShaderInfoLog(sh) ?? "compile error";
    gl.deleteShader(sh);
    throw new Error(`BodyFX shader compile failed: ${log}`);
  }
  return sh;
}

function link(gl: WebGL2RenderingContext, vs: WebGLShader, frag: WebGLShader): WebGLProgram {
  const prog = gl.createProgram()!;
  gl.attachShader(prog, vs);
  gl.attachShader(prog, frag);
  gl.linkProgram(prog);
  if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
    const log = gl.getProgramInfoLog(prog) ?? "link error";
    gl.deleteProgram(prog);
    throw new Error(`BodyFX program link failed: ${log}`);
  }
  return prog;
}

export { uniformValue };
