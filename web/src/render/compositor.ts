// WebGL2 compositor — composites the project frame at time t.
//
// Pipeline per renderAt(t):
//   1. Iterate tracks bottom-up (highest index = background first).
//   2. For each content clip active at t, draw its layer (video/image/camera
//      via FrameProvider, text/lyric via text.ts, shape via shapes.ts) with
//      transform (pos/scale/rotation/opacity/crop) sampled via propAt at
//      clip-local time.
//   3. fx/bodyfx bricks above a layer are collected and applied as
//      post-process passes (ping-pong FBOs) over everything composited below.
//   4. Final composite blitted to the visible canvas (devicePixelRatio-aware).
//
// renderAt never throws on an empty project — it renders a black frame.
import type { App } from "../core/app";
import {
  type Clip, type ClipType, type FxSubEffect, type Project, type Track,
  clipDuration, propAt,
} from "../core/project";
import { buildFxFrag, getFx, uniformValue, type FxDef } from "./fx";
import { renderTextLayer } from "./text";
import { renderShapeLayer } from "./shapes";
import { buildBodyFxProgram, type BodyFxProgram } from "./bodyfx";

/** Source frame provided by the media decoder (wired by main/decoder slice). */
export interface FrameProvider {
  getFrame(clip: Clip, sourceTime: number): VideoFrame | ImageBitmap | HTMLCanvasElement | null;
}

/** Body segmentation mask provider (wired by the ML inference slice). */
export interface MaskProvider {
  getMask(clip: Clip, sourceTime: number): ImageBitmap | HTMLCanvasElement | null;
}

const CONTENT_TYPES: ReadonlySet<ClipType> = new Set(["video", "image", "camera", "text", "lyric", "shape"]);
const FX_TYPES: ReadonlySet<ClipType> = new Set(["fx", "bodyfx"]);

// Fullscreen triangle vertex shader (shared by every pass).
const VS_SRC = `#version 300 es
in vec2 a_pos;
out vec2 v_uv;
void main() {
  v_uv = a_pos * 0.5 + 0.5;
  gl_Position = vec4(a_pos, 0.0, 1.0);
}`;

// Layer shader: unlike VS_SRC (which derives v_uv from position for fullscreen
// passes), content layers carry an explicit uv attribute so transformed quads
// sample the texture correctly under move/scale/rotate/crop.
const LAYER_VS_SRC = `#version 300 es
in vec2 a_pos;
in vec2 a_uv;
out vec2 v_uv;
void main() {
  v_uv = a_uv;
  gl_Position = vec4(a_pos, 0.0, 1.0);
}`;

// Blit/copy shader (no FX, just draw a texture to the current target).
const BLIT_FRAG = `#version 300 es
precision highp float;
uniform sampler2D u_tex;
in vec2 v_uv;
out vec4 frag;
void main() { frag = texture(u_tex, v_uv); }`;

// Placeholder shader — dark fill with clip name label drawn as a flat color.
const PLACEHOLDER_FRAG = `#version 300 es
precision highp float;
uniform vec2 u_res;
uniform vec4 u_color;
in vec2 v_uv;
out vec4 frag;
void main() { frag = u_color; }`;

interface Fbo {
  fbo: WebGLFramebuffer;
  tex: WebGLTexture;
  width: number;
  height: number;
}

interface FxProgram {
  prog: WebGLProgram;
  def: FxDef;
}

interface CachedTexture {
  tex: WebGLTexture;
  /** Clip id this texture belongs to. */
  clipId: number;
  /** Source frame identity (object reference) to detect staleness. */
  source: unknown;
  width: number;
  height: number;
}

interface CanvasLayerCache {
  clipId: number;
  /** Signature string; if it changes, re-render the canvas. */
  sig: string;
  tex: WebGLTexture;
  width: number;
  height: number;
}

export class Compositor {
  private readonly canvas: HTMLCanvasElement;
  private readonly app: App;
  private readonly gl: WebGL2RenderingContext;

  private vs: WebGLShader;
  private blitProg: WebGLProgram;
  private placeholderProg: WebGLProgram;
  private quadVao: WebGLVertexArrayObject;

  private frameProvider: FrameProvider | null = null;
  private maskProvider: MaskProvider | null = null;

  /** Ping-pong framebuffers at project resolution. */
  private fboA: Fbo | null = null;
  private fboB: Fbo | null = null;
  /** Accumulation FBO for the current compositing pass. */
  private accum: Fbo | null = null;

  /** Per-clip source textures (video/image/camera frames). */
  private texCache = new Map<number, CachedTexture>();
  /** Per-clip canvas-rendered layers (text/shape), keyed by signature. */
  private canvasCache = new Map<number, CanvasLayerCache>();
  /** Compiled FX programs keyed by effect name. */
  private fxPrograms = new Map<string, FxProgram>();
  /** Compiled bodyfx programs keyed by `${fxType}:${hasMask}`. */
  private bodyfxPrograms = new Map<string, BodyFxProgram>();

  private projW = 1920;
  private projH = 1080;
  private lastProjectSig = "";

  constructor(canvas: HTMLCanvasElement, app: App) {
    this.canvas = canvas;
    this.app = app;
    const gl = canvas.getContext("webgl2", {
      premultipliedAlpha: false,
      alpha: false,
      antialias: false,
      preserveDrawingBuffer: false,
    });
    if (!gl) throw new Error("WebGL2 unavailable");
    this.gl = gl;

    this.vs = this.compileShader(gl.VERTEX_SHADER, VS_SRC);
    this.blitProg = this.linkProgram(this.vs, this.compileShader(gl.FRAGMENT_SHADER, BLIT_FRAG));
    this.placeholderProg = this.linkProgram(
      this.vs,
      this.compileShader(gl.FRAGMENT_SHADER, PLACEHOLDER_FRAG),
    );
    this.quadVao = this.createQuad(gl);

    // Invalidate canvas-layer caches when the project changes structurally.
    app.events.on("project:changed", () => {
      this.canvasCache.clear();
      this.lastProjectSig = "";
    });
  }

  setFrameProvider(p: FrameProvider): void {
    this.frameProvider = p;
  }

  setMaskProvider(p: MaskProvider): void {
    this.maskProvider = p;
  }

  // -------------------------------------------------------------------------
  // Public render entry.
  // -------------------------------------------------------------------------

  renderAt(t: number): void {
    const gl = this.gl;
    const proj = this.app.project;
    this.ensureSize(proj);

    // Clear visible canvas to black (handles empty project).
    // Fit the canvas into its parent pane at project aspect; never read the
    // canvas's own clientWidth (that feedback-loops the intrinsic size).
    const dpr = window.devicePixelRatio || 1;
    const pane = this.canvas.parentElement;
    const availW = (pane ? pane.clientWidth : proj.width) - 24;
    const availH = (pane ? pane.clientHeight : proj.height) - 80;
    const scale = Math.max(0.01, Math.min(availW / proj.width, availH / proj.height));
    const cssW = Math.max(1, Math.floor(proj.width * scale));
    const cssH = Math.max(1, Math.floor(proj.height * scale));
    const cssWpx = `${cssW}px`;
    const cssHpx = `${cssH}px`;
    if (this.canvas.style.width !== cssWpx) this.canvas.style.width = cssWpx;
    if (this.canvas.style.height !== cssHpx) this.canvas.style.height = cssHpx;
    const dw = Math.round(cssW * dpr);
    const dh = Math.round(cssH * dpr);
    if (this.canvas.width !== dw || this.canvas.height !== dh) {
      this.canvas.width = dw;
      this.canvas.height = dh;
    }
    gl.viewport(0, 0, dw, dh);
    gl.disable(gl.SCISSOR_TEST);
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);

    if (proj.tracks.length === 0) return;

    // Composite into the accumulation FBO at project resolution.
    const accum = this.accum!;
    gl.bindFramebuffer(gl.FRAMEBUFFER, accum.fbo);
    gl.viewport(0, 0, accum.width, accum.height);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);

    this.compositeTracks(proj, t);

    // Blit accum → visible canvas.
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, dw, dh);
    this.drawTexture(accum.tex, this.blitProg, dw, dh, /*flipY*/ true);

    // Reclaim memory: delete stale source textures.
    this.evictStaleTextures(proj);
  }

  // -------------------------------------------------------------------------
  // Track/clip compositing.
  // -------------------------------------------------------------------------

  private compositeTracks(proj: Project, t: number): void {
    const gl = this.gl;
    // Bottom-up: highest index first (background), index 0 last (foreground).
    const tracks = proj.tracks;
    let pendingFx: { clip: Clip; track: Track }[] = [];

    for (let i = tracks.length - 1; i >= 0; i--) {
      const track = tracks[i]!;
      if (track.muted) continue;
      const clip = this.activeClip(track, t);
      if (!clip) continue;

      if (FX_TYPES.has(clip.type)) {
        // Defer FX bricks: apply once all content below has been composited.
        pendingFx.push({ clip, track });
        continue;
      }

      if (!CONTENT_TYPES.has(clip.type)) continue; // audio etc. — no visual

      // Flush any FX bricks that were queued above this content layer.
      // (FX bricks composite over everything below them; a content layer at a
      // lower track index is "above", so we apply pending FX before drawing
      // the next foreground layer.)
      if (pendingFx.length > 0) {
        this.applyPendingFx(pendingFx, t);
        pendingFx = [];
      }

      this.drawContentClip(clip, t);
    }

    // Apply any remaining FX bricks (e.g. bricks on the top track).
    if (pendingFx.length > 0) this.applyPendingFx(pendingFx, t);
  }

  private activeClip(track: Track, t: number): Clip | undefined {
    for (const c of track.clips) {
      if (t >= c.start && t < c.end) return c;
    }
    // Inclusive end at the very last frame.
    const last = track.clips[track.clips.length - 1];
    if (last && t === last.end) return last;
    return undefined;
  }

  private drawContentClip(clip: Clip, t: number): void {
    const gl = this.gl;
    const localT = (t - clip.start) * (clip.speed || 1);
    const opacity = propAt(clip, "opacity", localT);
    if (opacity <= 0) return;

    let tex: WebGLTexture | null = null;
    let texW = this.projW;
    let texH = this.projH;

    switch (clip.type) {
      case "video":
      case "camera": {
        const sourceTime = clip.inPoint + localT;
        const frame = this.frameProvider?.getFrame(clip, sourceTime) ?? null;
        if (frame) {
          const cached = this.uploadSourceFrame(clip.id, frame);
          tex = cached.tex;
          texW = cached.width;
          texH = cached.height;
        } else {
          this.drawPlaceholder(clip);
          return;
        }
        break;
      }
      case "image": {
        const sourceTime = clip.inPoint;
        const frame = this.frameProvider?.getFrame(clip, sourceTime) ?? null;
        if (frame) {
          const cached = this.uploadSourceFrame(clip.id, frame);
          tex = cached.tex;
          texW = cached.width;
          texH = cached.height;
        } else {
          this.drawPlaceholder(clip);
          return;
        }
        break;
      }
      case "text":
      case "lyric": {
        const layer = this.getCanvasLayer(clip, () => renderTextLayer(clip, this.projW, this.projH));
        tex = layer.tex;
        texW = layer.width;
        texH = layer.height;
        break;
      }
      case "shape": {
        const layer = this.getCanvasLayer(clip, () =>
          renderShapeLayer(clip, localT, this.projW, this.projH),
        );
        tex = layer.tex;
        texW = layer.width;
        texH = layer.height;
        break;
      }
      default:
        return;
    }

    if (!tex) return;
    this.drawLayerWithTransform(tex, texW, texH, clip, localT, opacity);
  }

  /** Draw a source texture with pos/scale/rotation/crop/opacity transform. */
  private drawLayerWithTransform(
    tex: WebGLTexture,
    texW: number,
    texH: number,
    clip: Clip,
    localT: number,
    opacity: number,
  ): void {
    const gl = this.gl;
    const posX = propAt(clip, "pos_x", localT);
    const posY = propAt(clip, "pos_y", localT);
    const scaleX = propAt(clip, "scale_x", localT);
    const scaleY = propAt(clip, "scale_y", localT);
    const rotation = propAt(clip, "rotation", localT);
    const cropL = Math.max(0, Math.min(0.5, propAt(clip, "crop_l", localT)));
    const cropR = Math.max(0, Math.min(0.5, propAt(clip, "crop_r", localT)));
    const cropT = Math.max(0, Math.min(0.5, propAt(clip, "crop_t", localT)));
    const cropB = Math.max(0, Math.min(0.5, propAt(clip, "crop_b", localT)));

    // Target rect on the project canvas, centered at (posX, posY) in [0,1]².
    // Fit the source into the project frame preserving aspect (contain), then
    // apply scale. Crop shrinks the source UV rectangle.
    const srcAspect = texW / texH || 1;
    const projAspect = this.projW / this.projH || 1;
    let baseW: number, baseH: number;
    if (srcAspect > projAspect) {
      baseW = 1;
      baseH = projAspect / srcAspect;
    } else {
      baseH = 1;
      baseW = srcAspect / projAspect;
    }
    const w = baseW * scaleX;
    const h = baseH * scaleY;

    // UV crop rectangle (in source texel space [0,1]²).
    const u0 = cropL;
    const u1 = 1 - cropR;
    const v0 = cropT;
    const v1 = 1 - cropB;
    if (u1 <= u0 || v1 <= v0) return;

    // Build a quad in clip space [-1,1]² centered at (posX*2-1, 1-posY*2).
    const cx = posX * 2 - 1;
    const cy = 1 - posY * 2;
    const hw = w;
    const hh = h;
    const rot = rotation;

    // We draw via the blit program but with a custom quad VAO carrying
    // position + uv. Simpler: use a uniform-less blit and rely on a dedicated
    // transform program. To keep one path, use drawTexturedQuad with a
    // per-call vertex buffer.
    this.drawTransformedQuad(tex, cx, cy, hw, hh, rot, u0, u1, v0, v1, opacity);
  }

  // -------------------------------------------------------------------------
  // FX post-processing.
  // -------------------------------------------------------------------------

  private applyPendingFx(pending: { clip: Clip; track: Track }[], t: number): void {
    // Apply each FX brick in order (they were collected bottom-up, so the
    // first in the list is the lowest brick — apply in that order).
    for (const { clip } of pending) {
      if (clip.type === "bodyfx") this.applyBodyFx(clip, t);
      else this.applyFxBrick(clip, t);
    }
  }

  private applyFxBrick(clip: Clip, t: number): void {
    const localT = t - clip.start;
    const amount = propAt(clip, "amount", localT);
    if (amount <= 0) return;

    // A brick has either a single fx or an ordered fxChain.
    if (clip.fxChain && clip.fxChain.length > 0) {
      for (const sub of clip.fxChain) this.applyFxSub(clip, sub, localT, amount);
    } else if (clip.fx) {
      this.runFxPass(clip.fx.fxType, clip.fx.amount, clip.fx.params, localT);
    }
  }

  private applyFxSub(clip: Clip, sub: FxSubEffect, localT: number, brickAmount: number): void {
    // relStart/relEnd window: skip if outside the sub-effect's active window.
    const relEnd = sub.relEnd > 0 ? sub.relEnd : clipDuration(clip);
    if (localT < sub.relStart || localT >= relEnd) return;
    // Window fade: ramp amount in/out over the first/last 10% of the window.
    const winLen = relEnd - sub.relStart;
    const into = (localT - sub.relStart) / Math.max(winLen, 0.001);
    const fade = Math.min(into / 0.1, (1 - into) / 0.1, 1);
    const amt = brickAmount * Math.max(0, fade);
    if (amt <= 0) return;
    this.runFxPass(sub.fxType, amt, sub.params, localT);
  }

  /** Run one FX pass: ping-pong the accum FBO through the effect shader. */
  private runFxPass(
    fxType: string,
    amount: number,
    params: Record<string, number> | undefined,
    localT: number,
  ): void {
    const gl = this.gl;
    const def = getFx(fxType);
    if (!def) return;
    const prog = this.getFxProgram(fxType, def);
    if (!prog) return;

    // Ping-pong: read from accum, write to scratch, swap.
    const src = this.accum!;
    const dst = this.fboA === src ? this.fboB! : this.fboA!;

    gl.bindFramebuffer(gl.FRAMEBUFFER, dst.fbo);
    gl.viewport(0, 0, dst.width, dst.height);
    gl.useProgram(prog);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, src.tex);
    gl.uniform1i(gl.getUniformLocation(prog, "u_tex"), 0);
    gl.uniform2f(gl.getUniformLocation(prog, "u_res"), dst.width, dst.height);
    gl.uniform1f(gl.getUniformLocation(prog, "u_time"), this.app.playhead);
    gl.uniform1f(gl.getUniformLocation(prog, "u_amount"), amount);
    // Bind named uniforms from def + params.
    for (const uname of def.uniforms) {
      if (uname === "u_tex" || uname === "u_res" || uname === "u_time" || uname === "u_amount") continue;
      const loc = gl.getUniformLocation(prog, uname);
      if (loc) gl.uniform1f(loc, uniformValue(def, uname, params));
    }
    this.bindQuad();
    gl.drawArrays(gl.TRIANGLES, 0, 3);

    // Swap accum ↔ scratch.
    this.accum = dst;
    if (this.fboA === dst) this.fboA = src;
    else this.fboB = src;
  }

  private applyBodyFx(clip: Clip, t: number): void {
    const gl = this.gl;
    const localT = t - clip.start;
    const amount = propAt(clip, "amount", localT);
    if (amount <= 0) return;

    const sourceTime = clip.inPoint + localT * (clip.speed || 1);
    const maskSrc = this.maskProvider?.getMask(clip, sourceTime) ?? null;
    const mask = maskSrc ? this.uploadMask(maskSrc) : null;
    const hasMask = mask !== null;

    // Determine the effect type: bodyfx bricks carry an fxChain or fx like fx
    // bricks, but the effect is mask-composited. Use the first sub-effect or
    // clip.fx.fxType.
    let fxType: string | null = null;
    let params: Record<string, number> | undefined;
    let subAmount = amount;
    if (clip.fxChain && clip.fxChain.length > 0) {
      // Pick the active sub-effect at localT.
      for (const sub of clip.fxChain) {
        const relEnd = sub.relEnd > 0 ? sub.relEnd : clipDuration(clip);
        if (localT >= sub.relStart && localT < relEnd) {
          fxType = sub.fxType;
          params = sub.params;
          break;
        }
      }
    } else if (clip.fx) {
      fxType = clip.fx.fxType;
      params = clip.fx.params;
      subAmount = clip.fx.amount;
    }
    if (!fxType) return;

    const key = `${fxType}:${hasMask}`;
    let bodyProg = this.bodyfxPrograms.get(key);
    if (!bodyProg) {
      let built: BodyFxProgram | null;
      try {
        built = buildBodyFxProgram(gl, this.vs, fxType, hasMask);
      } catch {
        return;
      }
      if (!built) return;
      bodyProg = built;
      this.bodyfxPrograms.set(key, built);
    }

    // 1. Render the effect into the scratch FBO (fboA), reading from accum.
    const src = this.accum!;
    const scratch = this.fboA === src ? this.fboB! : this.fboA!;
    const def = bodyProg.def;
    gl.bindFramebuffer(gl.FRAMEBUFFER, scratch.fbo);
    gl.viewport(0, 0, scratch.width, scratch.height);
    gl.useProgram(bodyProg.effect);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, src.tex);
    gl.uniform1i(gl.getUniformLocation(bodyProg.effect, "u_tex"), 0);
    gl.uniform2f(gl.getUniformLocation(bodyProg.effect, "u_res"), scratch.width, scratch.height);
    gl.uniform1f(gl.getUniformLocation(bodyProg.effect, "u_time"), this.app.playhead);
    gl.uniform1f(gl.getUniformLocation(bodyProg.effect, "u_amount"), subAmount);
    for (const uname of def.uniforms) {
      if (uname === "u_tex" || uname === "u_res" || uname === "u_time" || uname === "u_amount") continue;
      const loc = gl.getUniformLocation(bodyProg.effect, uname);
      if (loc) gl.uniform1f(loc, uniformValue(def, uname, params));
    }
    this.bindQuad();
    gl.drawArrays(gl.TRIANGLES, 0, 3);

    // 2. Composite effect over original via mask shader → accum.
    gl.bindFramebuffer(gl.FRAMEBUFFER, src.fbo);
    gl.viewport(0, 0, src.width, src.height);
    gl.useProgram(bodyProg.composite);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, src.tex);
    gl.uniform1i(gl.getUniformLocation(bodyProg.composite, "u_tex"), 0);
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, mask ?? this.whiteTex());
    gl.uniform1i(gl.getUniformLocation(bodyProg.composite, "u_mask"), 1);
    gl.activeTexture(gl.TEXTURE2);
    gl.bindTexture(gl.TEXTURE_2D, scratch.tex);
    gl.uniform1i(gl.getUniformLocation(bodyProg.composite, "u_effect"), 2);
    gl.uniform2f(gl.getUniformLocation(bodyProg.composite, "u_res"), src.width, src.height);
    gl.uniform1f(gl.getUniformLocation(bodyProg.composite, "u_amount"), amount);
    this.bindQuad();
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    // Restore active texture unit.
    gl.activeTexture(gl.TEXTURE0);
  }

  // -------------------------------------------------------------------------
  // GL helpers.
  // -------------------------------------------------------------------------

  private ensureSize(proj: Project): void {
    const w = Math.max(1, proj.width || 1920);
    const h = Math.max(1, proj.height || 1080);
    const sig = `${w}x${h}|${proj.tracks.length}|${proj.duration}`;
    if (sig === this.lastProjectSig && this.accum) {
      this.projW = w;
      this.projH = h;
      return;
    }
    this.projW = w;
    this.projH = h;
    this.lastProjectSig = sig;
    this.releaseFbo(this.fboA);
    this.releaseFbo(this.fboB);
    this.fboA = this.createFbo(w, h);
    this.fboB = this.createFbo(w, h);
    this.accum = this.fboA;
  }

  private createFbo(w: number, h: number): Fbo {
    const gl = this.gl;
    const tex = gl.createTexture()!;
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    const fbo = gl.createFramebuffer()!;
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    return { fbo, tex, width: w, height: h };
  }

  private releaseFbo(f: Fbo | null): void {
    if (!f) return;
    const gl = this.gl;
    gl.deleteTexture(f.tex);
    gl.deleteFramebuffer(f.fbo);
  }

  private createQuad(gl: WebGL2RenderingContext): WebGLVertexArrayObject {
    const vao = gl.createVertexArray()!;
    gl.bindVertexArray(vao);
    const buf = gl.createBuffer()!;
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    // Fullscreen triangle: positions for a single triangle covering [-1,1]².
    gl.bufferData(
      gl.ARRAY_BUFFER,
      new Float32Array([-1, -1, 3, -1, -1, 3]),
      gl.STATIC_DRAW,
    );
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.bindVertexArray(null);
    return vao;
  }

  private bindQuad(): void {
    this.gl.bindVertexArray(this.quadVao);
  }

  private compileShader(type: number, src: string): WebGLShader {
    const gl = this.gl;
    const sh = gl.createShader(type)!;
    gl.shaderSource(sh, src);
    gl.compileShader(sh);
    if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
      const log = gl.getShaderInfoLog(sh) ?? "compile error";
      gl.deleteShader(sh);
      throw new Error(`Shader compile failed: ${log}`);
    }
    return sh;
  }

  private linkProgram(vs: WebGLShader, frag: WebGLShader): WebGLProgram {
    const gl = this.gl;
    const prog = gl.createProgram()!;
    gl.attachShader(prog, vs);
    gl.attachShader(prog, frag);
    gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
      const log = gl.getProgramInfoLog(prog) ?? "link error";
      gl.deleteProgram(prog);
      throw new Error(`Program link failed: ${log}`);
    }
    return prog;
  }

  private getFxProgram(name: string, def: FxDef): WebGLProgram | null {
    const cached = this.fxPrograms.get(name);
    if (cached) return cached.prog;
    try {
      const frag = this.compileShader(this.gl.FRAGMENT_SHADER, buildFxFrag(def));
      const prog = this.linkProgram(this.vs, frag);
      this.gl.deleteShader(frag);
      this.fxPrograms.set(name, { prog, def });
      return prog;
    } catch {
      return null;
    }
  }

  /** Draw a texture fullscreen (blit). flipY because FBO textures are upside-down vs canvas. */
  private drawTexture(
    tex: WebGLTexture,
    prog: WebGLProgram,
    _w: number,
    _h: number,
    flipY: boolean,
  ): void {
    const gl = this.gl;
    gl.useProgram(prog);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.uniform1i(gl.getUniformLocation(prog, "u_tex"), 0);
    // FBO textures are upside-down relative to the canvas; flip Y on blit.
    if (flipY) this.drawFlippedQuad();
    else { this.bindQuad(); gl.drawArrays(gl.TRIANGLES, 0, 3); }
  }

  private flippedVao: WebGLVertexArrayObject | null = null;
  private drawFlippedQuad(): void {
    const gl = this.gl;
    if (!this.flippedVao) {
      const vao = gl.createVertexArray()!;
      gl.bindVertexArray(vao);
      const buf = gl.createBuffer()!;
      gl.bindBuffer(gl.ARRAY_BUFFER, buf);
      // Flip Y so FBO content appears upright on the canvas.
      gl.bufferData(
        gl.ARRAY_BUFFER,
        new Float32Array([-1, 1, 3, 1, -1, -3]),
        gl.STATIC_DRAW,
      );
      gl.enableVertexAttribArray(0);
      gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
      gl.bindVertexArray(null);
      this.flippedVao = vao;
    }
    gl.bindVertexArray(this.flippedVao);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
  }

  /** Draw a dark placeholder rect with the clip name area. */
  private drawPlaceholder(clip: Clip): void {
    const gl = this.gl;
    gl.useProgram(this.placeholderProg);
    gl.uniform4f(gl.getUniformLocation(this.placeholderProg, "u_color"), 0.08, 0.09, 0.12, 1);
    gl.uniform2f(gl.getUniformLocation(this.placeholderProg, "u_res"), this.projW, this.projH);
    this.bindQuad();
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    void clip; // name label omitted in GL placeholder; canvas2D fallback not wired
  }

  /** Draw a transformed textured quad (content layer) into the accum FBO. */
  private drawTransformedQuad(
    tex: WebGLTexture,
    cx: number,
    cy: number,
    hw: number,
    hh: number,
    rot: number,
    u0: number,
    u1: number,
    v0: number,
    v1: number,
    opacity: number,
  ): void {
    const gl = this.gl;
    const x0 = -hw, x1 = hw, y0 = -hh, y1 = hh;
    const cos = Math.cos(rot), sin = Math.sin(rot);
    // Build a per-call vertex buffer: 6 verts (two triangles), each with pos + uv.
    const corners: Array<[number, number, number, number]> = [
      [x0, y0, u0, v1],
      [x1, y0, u1, v1],
      [x1, y1, u1, v0],
      [x0, y1, u0, v0],
    ];
    const transformed: Array<[number, number, number, number]> = corners.map(([lx, ly, u, v]) => {
      const wx = cx + lx * cos - ly * sin;
      const wy = cy + lx * sin + ly * cos;
      return [wx, wy, u, v];
    });
    const verts = new Float32Array([
      transformed[0]![0], transformed[0]![1], transformed[0]![2], transformed[0]![3],
      transformed[1]![0], transformed[1]![1], transformed[1]![2], transformed[1]![3],
      transformed[2]![0], transformed[2]![1], transformed[2]![2], transformed[2]![3],
      transformed[0]![0], transformed[0]![1], transformed[0]![2], transformed[0]![3],
      transformed[2]![0], transformed[2]![1], transformed[2]![2], transformed[2]![3],
      transformed[3]![0], transformed[3]![1], transformed[3]![2], transformed[3]![3],
    ]);

    // Upload to a scratch buffer + VAO. createLayerVao also builds layerProg.
    if (!this.layerVao) this.layerVao = this.createLayerVao();
    const prog = this.layerProg!;
    gl.bindVertexArray(this.layerVao);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.layerBuf!);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, verts);

    gl.useProgram(prog);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.uniform1i(gl.getUniformLocation(prog, "u_tex"), 0);
    gl.uniform1f(gl.getUniformLocation(prog, "u_opacity"), opacity);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    gl.drawArrays(gl.TRIANGLES, 0, 6);
    gl.disable(gl.BLEND);
    gl.bindVertexArray(null);
  }

  private layerProg: WebGLProgram | null = null;
  private layerVao: WebGLVertexArrayObject | null = null;
  private layerBuf: WebGLBuffer | null = null;

  private createLayerVao(): WebGLVertexArrayObject {
    const gl = this.gl;
    const vao = gl.createVertexArray()!;
    gl.bindVertexArray(vao);
    const buf = gl.createBuffer()!;
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    gl.bufferData(gl.ARRAY_BUFFER, 6 * 4 * 4, gl.DYNAMIC_DRAW);
    // pos (2 floats) + uv (2 floats), stride 16.
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 16, 0);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 2, gl.FLOAT, false, 16, 8);
    gl.bindVertexArray(null);
    this.layerBuf = buf;
    if (!this.layerProg) {
      this.layerProg = this.linkProgram(
        this.compileShader(gl.VERTEX_SHADER, LAYER_VS_SRC),
        this.compileShader(
          gl.FRAGMENT_SHADER,
          `#version 300 es
precision highp float;
uniform sampler2D u_tex;
uniform float u_opacity;
in vec2 v_uv;
out vec4 frag;
void main() {
  vec4 c = texture(u_tex, v_uv);
  frag = vec4(c.rgb, c.a * u_opacity);
}`,
        ),
      );
    }
    return vao;
  }

  // -------------------------------------------------------------------------
  // Texture upload + caching.
  // -------------------------------------------------------------------------

  private uploadSourceFrame(clipId: number, frame: VideoFrame | ImageBitmap | HTMLCanvasElement): {
    tex: WebGLTexture; width: number; height: number;
  } {
    const gl = this.gl;
    const cached = this.texCache.get(clipId);
    if (cached && cached.source === frame) {
      return { tex: cached.tex, width: cached.width, height: cached.height };
    }
    const width = (frame as { width?: number }).width ?? this.projW;
    const height = (frame as { height?: number }).height ?? this.projH;

    let tex: WebGLTexture;
    if (cached) {
      tex = cached.tex;
    } else {
      tex = gl.createTexture()!;
    }
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
    // VideoFrame/ImageBitmap/HTMLCanvasElement are all validTexImageSource.
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE, frame as TexImageSource);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

    this.texCache.set(clipId, { tex, clipId, source: frame, width, height });
    return { tex, width, height };
  }

  /** Canvas-rendered layer (text/shape) cached by signature. */
  private getCanvasLayer(clip: Clip, render: () => HTMLCanvasElement): {
    tex: WebGLTexture; width: number; height: number;
  } {
    const gl = this.gl;
    const sig = this.canvasLayerSig(clip);
    const cached = this.canvasCache.get(clip.id);
    if (cached && cached.sig === sig) {
      return { tex: cached.tex, width: cached.width, height: cached.height };
    }
    const canvas = render();
    const width = canvas.width;
    const height = canvas.height;
    let tex: WebGLTexture;
    if (cached) {
      tex = cached.tex;
    } else {
      tex = gl.createTexture()!;
    }
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE, canvas);
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    this.canvasCache.set(clip.id, { clipId: clip.id, sig, tex, width, height });
    return { tex, width, height };
  }

  private canvasLayerSig(clip: Clip): string {
    if (clip.type === "shape") {
      // Shape layers depend on clip-local time for path morph; include the
      // quantized time so the cache re-renders as the morph progresses.
      const t = Math.floor(this.app.playhead * 30);
      return `shape:${clip.id}:${t}:${this.projW}x${this.projH}`;
    }
    const ts = clip.textStyle;
    const sig = `text:${clip.text ?? ""}:${ts ? JSON.stringify(ts) : ""}:${this.projW}x${this.projH}`;
    return sig;
  }

  private evictStaleTextures(proj: Project): void {
    const live = new Set<number>();
    for (const tr of proj.tracks) for (const c of tr.clips) live.add(c.id);
    for (const [id, entry] of this.texCache) {
      if (!live.has(id)) {
        this.gl.deleteTexture(entry.tex);
        this.texCache.delete(id);
      }
    }
    for (const [id, entry] of this.canvasCache) {
      if (!live.has(id)) {
        this.gl.deleteTexture(entry.tex);
        this.canvasCache.delete(id);
      }
    }
  }

  private _whiteTex: WebGLTexture | null = null;
  private whiteTex(): WebGLTexture {
    if (this._whiteTex) return this._whiteTex;
    const gl = this.gl;
    const tex = gl.createTexture()!;
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texImage2D(
      gl.TEXTURE_2D, 0, gl.RGBA8, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE,
      new Uint8Array([255, 255, 255, 255]),
    );
    this._whiteTex = tex;
    return tex;
  }

  /** Reusable mask texture; masks are per-frame so we re-upload every call. */
  private _maskTex: WebGLTexture | null = null;
  private uploadMask(src: ImageBitmap | HTMLCanvasElement): WebGLTexture {
    const gl = this.gl;
    this._maskTex ??= gl.createTexture()!;
    gl.bindTexture(gl.TEXTURE_2D, this._maskTex);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, 0);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE, src);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    return this._maskTex;
  }
}
