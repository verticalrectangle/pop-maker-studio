// Shape layer rendering — Canvas2D → texture source for the compositor.
// Presets: circle, rect, star, heart, arrow. Custom path with morph keys.
import type { Clip, ShapeData, ShapePoint } from "../core/project";
import { type Interp, sampleKeyTrack } from "../core/keyframes";

function rgba(c: [number, number, number, number] | undefined): string | undefined {
  if (!c) return undefined;
  const [r, g, b, a] = c;
  return `rgba(${Math.round(r * 255)},${Math.round(g * 255)},${Math.round(b * 255)},${a})`;
}

function ease(u: number, interp: Interp): number {
  switch (interp) {
    case "hold": return 0;
    case "ease_in": return u * u;
    case "ease_out": return 1 - (1 - u) * (1 - u);
    case "ease_both": return u * u * (3 - 2 * u);
    default: return u;
  }
}

// ---------------------------------------------------------------------------
// Preset path generators — return points in local [0,1]² space.
// ---------------------------------------------------------------------------

function circlePoints(segments: number): ShapePoint[] {
  const pts: ShapePoint[] = [];
  for (let i = 0; i < segments; i++) {
    const a = (i / segments) * Math.PI * 2;
    pts.push({ x: 0.5 + Math.cos(a) * 0.4, y: 0.5 + Math.sin(a) * 0.4 });
  }
  return pts;
}

function rectPoints(rx: number, ry: number): ShapePoint[] {
  const x0 = 0.5 - rx, x1 = 0.5 + rx, y0 = 0.5 - ry, y1 = 0.5 + ry;
  return [
    { x: x0, y: y0 }, { x: x1, y: y0 }, { x: x1, y: y1 }, { x: x0, y: y1 },
  ];
}

function starPoints(points: number, inner: number, outer: number): ShapePoint[] {
  const pts: ShapePoint[] = [];
  for (let i = 0; i < points * 2; i++) {
    const a = (i / (points * 2)) * Math.PI * 2 - Math.PI / 2;
    const r = i % 2 === 0 ? outer : inner;
    pts.push({ x: 0.5 + Math.cos(a) * r, y: 0.5 + Math.sin(a) * r });
  }
  return pts;
}

function heartPoints(): ShapePoint[] {
  const pts: ShapePoint[] = [];
  const n = 48;
  for (let i = 0; i < n; i++) {
    const tt = (i / n) * Math.PI * 2;
    const x = 16 * Math.pow(Math.sin(tt), 3);
    const y = 13 * Math.cos(tt) - 5 * Math.cos(2 * tt) - 2 * Math.cos(3 * tt) - Math.cos(4 * tt);
    pts.push({ x: 0.5 + x / 40, y: 0.5 - y / 40 });
  }
  return pts;
}

function arrowPoints(): ShapePoint[] {
  return [
    { x: 0.1, y: 0.4 }, { x: 0.6, y: 0.4 }, { x: 0.6, y: 0.25 },
    { x: 0.9, y: 0.5 }, { x: 0.6, y: 0.75 }, { x: 0.6, y: 0.6 },
    { x: 0.1, y: 0.6 },
  ];
}

function presetPath(preset: string, params: number[]): ShapePoint[] {
  switch (preset) {
    case "circle": return circlePoints(Math.max(8, Math.round(params[0] ?? 48)));
    case "rect": {
      const rx = params[0] ?? 0.4;
      const ry = params[1] ?? rx;
      return rectPoints(rx, ry);
    }
    case "star": {
      const points = Math.max(3, Math.round(params[0] ?? 5));
      const inner = params[1] ?? 0.2;
      const outer = params[2] ?? 0.4;
      return starPoints(points, inner, outer);
    }
    case "heart": return heartPoints();
    case "arrow": return arrowPoints();
    default: return circlePoints(48);
  }
}

// ---------------------------------------------------------------------------
// Path-morph interpolation — resample two paths to a common point count and
// lerp x/y/w with the keyframe easing.
// ---------------------------------------------------------------------------

function resamplePath(path: ShapePoint[], count: number): ShapePoint[] {
  if (path.length === 0) return Array.from({ length: count }, () => ({ x: 0.5, y: 0.5 }));
  if (path.length === count) return path;
  const out: ShapePoint[] = [];
  for (let i = 0; i < count; i++) {
    const idx = (i / count) * path.length;
    const lo = Math.floor(idx);
    const hi = Math.min(lo + 1, path.length - 1);
    const f = idx - lo;
    const a = path[lo]!;
    const b = path[hi]!;
    out.push({
      x: a.x + (b.x - a.x) * f,
      y: a.y + (b.y - a.y) * f,
      w: a.w !== undefined || b.w !== undefined
        ? (a.w ?? 0.008) + ((b.w ?? 0.008) - (a.w ?? 0.008)) * f
        : undefined,
    });
  }
  return out;
}

function lerpPaths(a: ShapePoint[], b: ShapePoint[], u: number): ShapePoint[] {
  const count = Math.max(a.length, b.length);
  const aa = resamplePath(a, count);
  const bb = resamplePath(b, count);
  const out: ShapePoint[] = [];
  for (let i = 0; i < count; i++) {
    const pa = aa[i]!;
    const pb = bb[i]!;
    out.push({
      x: pa.x + (pb.x - pa.x) * u,
      y: pa.y + (pb.y - pa.y) * u,
      w: pa.w !== undefined || pb.w !== undefined
        ? (pa.w ?? 0.008) + ((pb.w ?? 0.008) - (pa.w ?? 0.008)) * u
        : undefined,
    });
  }
  return out;
}

/** Resolve the effective path at clip-local time t, applying pathKeys morph. */
export function resolveShapePath(shape: ShapeData, t: number): ShapePoint[] {
  const base = shape.path ?? presetPath(shape.preset, shape.params);
  const keys = shape.pathKeys;
  if (!keys || keys.length === 0) return base;
  if (t <= keys[0]!.t) return keys[0]!.path;
  if (t >= keys[keys.length - 1]!.t) return keys[keys.length - 1]!.path;
  for (let i = 0; i < keys.length - 1; i++) {
    const a = keys[i]!;
    const b = keys[i + 1]!;
    if (t >= a.t && t <= b.t) {
      const span = b.t - a.t;
      const u = span > 0 ? (t - a.t) / span : 0;
      return lerpPaths(a.path, b.path, ease(u, b.interp));
    }
  }
  return base;
}

/**
 * Render a shape clip to an offscreen canvas. Presets and custom paths are
 * drawn with fill, stroke, and optional glow. Path-morph keyframes interpolate
 * the path at clip-local time t. Returns a canvas sized to the project frame.
 */
export function renderShapeLayer(
  clip: Clip,
  t: number,
  width: number,
  height: number,
): HTMLCanvasElement {
  const canvas = document.createElement("canvas");
  canvas.width = width;
  canvas.height = height;
  const shape = clip.shape;
  const ctx = canvas.getContext("2d");
  if (!ctx || !shape) return canvas;

  const path = resolveShapePath(shape, t);
  if (path.length === 0) return canvas;

  ctx.save();
  // Map local [0,1]² to canvas pixels.
  const strokeBase = shape.strokeWidth * height;
  const glowBase = shape.glowRadius * height;

  ctx.beginPath();
  for (let i = 0; i < path.length; i++) {
    const p = path[i]!;
    const px = p.x * width;
    const py = p.y * height;
    if (i === 0) ctx.moveTo(px, py);
    else ctx.lineTo(px, py);
  }
  if (shape.closed) ctx.closePath();

  // Glow: draw the path several times with increasing blur + low alpha.
  if (glowBase > 0) {
    const glowColor = rgba(shape.stroke ?? shape.fill);
    if (glowColor) {
      ctx.save();
      ctx.shadowColor = glowColor;
      ctx.shadowBlur = glowBase;
      ctx.lineWidth = Math.max(strokeBase, 2);
      ctx.strokeStyle = glowColor;
      ctx.globalAlpha = 0.6;
      ctx.stroke();
      ctx.shadowBlur = glowBase * 2;
      ctx.globalAlpha = 0.3;
      ctx.stroke();
      ctx.restore();
    }
  }

  const fillStyle = rgba(shape.fill);
  if (fillStyle && shape.closed) {
    ctx.fillStyle = fillStyle;
    ctx.fill();
  }

  const strokeStyle = rgba(shape.stroke);
  if (strokeStyle && strokeBase > 0) {
    ctx.strokeStyle = strokeStyle;
    ctx.lineWidth = strokeBase;
    ctx.lineJoin = "round";
    ctx.lineCap = "round";
    ctx.stroke();
  }

  ctx.restore();
  return canvas;
}

// Re-export sampleKeyTrack for callers that animate shape scalar props.
export { sampleKeyTrack };
