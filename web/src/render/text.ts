// Text/lyric layer rendering — Canvas2D → texture source for the compositor.
import type { Clip, TextStyle } from "../core/project";

function rgba(c: [number, number, number, number]): string {
  const [r, g, b, a] = c;
  return `rgba(${Math.round(r * 255)},${Math.round(g * 255)},${Math.round(b * 255)},${a})`;
}

function buildFont(style: TextStyle, px: number): string {
  const weight = style.bold ? "700" : "400";
  const italic = style.italic ? "italic " : "";
  return `${italic}${weight} ${px}px ${style.fontFamily}`;
}

/** Apply letterSpacing by manually spacing glyphs (Canvas2D has no native tracking). */
function drawWithTracking(
  ctx: CanvasRenderingContext2D,
  text: string,
  x: number,
  y: number,
  trackingPx: number,
  align: "left" | "center" | "right",
): void {
  if (trackingPx === 0) {
    ctx.textAlign = align;
    ctx.textBaseline = "middle";
    ctx.fillText(text, x, y);
    return;
  }
  // Measure total tracked width for alignment.
  let total = 0;
  for (const ch of text) total += ctx.measureText(ch).width + trackingPx;
  total -= trackingPx;
  let startX = x;
  if (align === "center") startX = x - total / 2;
  else if (align === "right") startX = x - total;
  ctx.textAlign = "left";
  ctx.textBaseline = "middle";
  let cursor = startX;
  for (const ch of text) {
    ctx.fillText(ch, cursor, y);
    cursor += ctx.measureText(ch).width + trackingPx;
  }
}

/**
 * Render a text/lyric clip to an offscreen canvas honoring TextStyle.
 * `width`/`height` are the project backing-store dimensions; the canvas is
 * sized to the text bounding box (capped to the frame) so the compositor can
 * place it via transform. Returns a canvas ready to upload to a texture.
 */
export function renderTextLayer(clip: Clip, width: number, height: number): HTMLCanvasElement {
  const style = clip.textStyle;
  const canvas = document.createElement("canvas");
  canvas.width = width;
  canvas.height = height;
  const ctx = canvas.getContext("2d");
  if (!ctx || !style) return canvas;

  const text = style.uppercase ? (clip.text ?? "").toUpperCase() : (clip.text ?? "");
  const fontSizePx = Math.max(1, style.fontSize * height);
  const trackingPx = style.letterSpacing * fontSizePx;
  const strokePx = Math.max(0, style.strokeWidth * height);

  ctx.font = buildFont(style, fontSizePx);

  // Background box (optional) — full-frame band behind the text.
  if (style.bgColor) {
    ctx.fillStyle = rgba(style.bgColor);
    ctx.fillRect(0, 0, width, height);
  }

  const cx = width / 2;
  const cy = height / 2;

  // Shadow first (drawn under stroke + fill).
  if (style.shadow) {
    const sh = style.shadow;
    ctx.save();
    ctx.shadowOffsetX = sh.dx * fontSizePx;
    ctx.shadowOffsetY = sh.dy * fontSizePx;
    ctx.shadowBlur = sh.blur * fontSizePx;
    ctx.shadowColor = rgba(sh.color);
    ctx.fillStyle = rgba(style.color);
    drawWithTracking(ctx, text, cx, cy, trackingPx, style.align);
    ctx.restore();
  }

  // Stroke then fill.
  if (style.strokeColor && strokePx > 0) {
    ctx.strokeStyle = rgba(style.strokeColor);
    ctx.lineWidth = strokePx;
    ctx.lineJoin = "round";
    ctx.miterLimit = 2;
    drawTrackedStroke(ctx, text, cx, cy, trackingPx, style.align);
  }

  ctx.fillStyle = rgba(style.color);
  drawWithTracking(ctx, text, cx, cy, trackingPx, style.align);

  return canvas;
}

/** Stroke each glyph with tracking (separate pass so fill sits on top). */
function drawTrackedStroke(
  ctx: CanvasRenderingContext2D,
  text: string,
  x: number,
  y: number,
  trackingPx: number,
  align: "left" | "center" | "right",
): void {
  let total = 0;
  for (const ch of text) total += ctx.measureText(ch).width + trackingPx;
  total -= trackingPx;
  let startX = x;
  if (align === "center") startX = x - total / 2;
  else if (align === "right") startX = x - total;
  ctx.textAlign = "left";
  ctx.textBaseline = "middle";
  let cursor = startX;
  for (const ch of text) {
    ctx.strokeText(ch, cursor, y);
    cursor += ctx.measureText(ch).width + trackingPx;
  }
}
