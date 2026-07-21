// On-canvas WYSIWYG: selection, move/scale/rotate handles drawn over the
// preview, mirroring the desktop editor's canvas (src/ui/canvas.cpp).
// Bbox math mirrors the compositor's drawLayerWithTransform so the handles
// frame exactly what the renderer draws.
import type { App } from "../core/app";
import { propAt, type Clip } from "../core/project";
import { resolveShapePath } from "../render/shapes";

type HandleKind =
  | "none" | "body"
  | "tl" | "tr" | "bl" | "br"
  | "l" | "r" | "t" | "b"
  | "rotate";

/** Everything needed to draw + hit-test one clip's transform box. */
interface Box {
  clip: Clip;
  /** Center in project pixels. */
  cx: number;
  cy: number;
  /** Local half extents in project pixels (un-rotated). */
  hw: number;
  hh: number;
  rotDeg: number;
}

interface DragState {
  handle: HandleKind;
  clip: Clip;
  startMouseX: number;
  startMouseY: number;
  startLocalX: number;
  startLocalY: number;
  startPosX: number;
  startPosY: number;
  startScaleX: number;
  startScaleY: number;
  startRot: number;
  startAngle: number;
  cx: number;
  cy: number;
  hw: number;
  hh: number;
  moved: boolean;
  snappedX: boolean;
  snappedY: boolean;
  rotSnapped: boolean;
}

const HANDLE_R = 5;
const HIT_R = 9;
const ROT_DIST = 26;
const VISUAL = new Set(["video", "image", "camera", "text", "lyric", "shape"]);

export function initCanvasWysiwyg(app: App, preview: HTMLCanvasElement): void {
  const parent = preview.parentElement;
  if (!parent) return;
  const pane: HTMLElement = parent;
  const overlay = document.createElement("canvas");
  overlay.id = "canvas-handles";
  pane.appendChild(overlay);

  const measure = document.createElement("canvas").getContext("2d");
  let drag: DragState | null = null;
  let batchOpen = false;
  let cssW = 0;
  let cssH = 0;

  // -- layout: track the preview canvas rect inside the pane -----------------
  function layout(): void {
    const paneRect = pane.getBoundingClientRect();
    const r = preview.getBoundingClientRect();
    cssW = r.width;
    cssH = r.height;
    const dpr = window.devicePixelRatio || 1;
    overlay.style.left = `${r.left - paneRect.left}px`;
    overlay.style.top = `${r.top - paneRect.top}px`;
    overlay.style.width = `${cssW}px`;
    overlay.style.height = `${cssH}px`;
    const bw = Math.max(1, Math.round(cssW * dpr));
    const bh = Math.max(1, Math.round(cssH * dpr));
    if (overlay.width !== bw || overlay.height !== bh) {
      overlay.width = bw;
      overlay.height = bh;
    }
  }
  new ResizeObserver(() => { layout(); draw(); }).observe(pane);
  new ResizeObserver(() => { layout(); draw(); }).observe(preview);

  const px = (): number => cssW / app.project.width;   // project px → css px
  const py = (): number => cssH / app.project.height;

  // -- bbox computation (mirrors compositor) ---------------------------------
  function uvBoxFor(clip: Clip, localT: number): { u0: number; v0: number; u1: number; v1: number } | null {
    const W = app.project.width;
    const H = app.project.height;
    if (clip.type === "video" || clip.type === "image" || clip.type === "camera") {
      const entry = clip.source ? app.media.get(clip.source) : undefined;
      const sw = entry?.width ?? 0;
      const sh = entry?.height ?? 0;
      if (sw <= 0 || sh <= 0) return { u0: 0, v0: 0, u1: 1, v1: 1 };
      const srcAspect = sw / sh;
      const projAspect = W / H;
      // Compositor contain-fit in fraction space → uv box centered at 0.5.
      let bw: number, bh: number;
      if (srcAspect > projAspect) { bw = 1; bh = projAspect / srcAspect; }
      else { bh = 1; bw = srcAspect / projAspect; }
      return { u0: 0.5 - bw / 2, v0: 0.5 - bh / 2, u1: 0.5 + bw / 2, v1: 0.5 + bh / 2 };
    }
    if (clip.type === "text" || clip.type === "lyric") {
      const style = clip.textStyle;
      if (!style || !clip.text) return { u0: 0.45, v0: 0.45, u1: 0.55, v1: 0.55 };
      const fontPx = Math.max(1, style.fontSize * H);
      const tracking = style.letterSpacing * fontPx;
      const text = style.uppercase ? clip.text.toUpperCase() : clip.text;
      let tw = 0;
      if (measure) {
        measure.font = `${style.italic ? "italic " : ""}${style.bold ? "700" : "400"} ${fontPx}px ${style.fontFamily}`;
        for (const ch of text) tw += measure.measureText(ch).width + tracking;
        tw -= tracking;
      }
      const th = fontPx * 1.25 + Math.max(0, style.strokeWidth * H);
      let u0 = 0.5 - tw / 2 / W;
      let u1 = 0.5 + tw / 2 / W;
      if (style.align === "left") { u0 = 0.5; u1 = 0.5 + tw / W; }
      else if (style.align === "right") { u0 = 0.5 - tw / W; u1 = 0.5; }
      return { u0, v0: 0.5 - th / 2 / H, u1, v1: 0.5 + th / 2 / H };
    }
    if (clip.type === "shape" && clip.shape) {
      const path = resolveShapePath(clip.shape, localT);
      if (path.length === 0) return null;
      let u0 = 1, v0 = 1, u1 = 0, v1 = 0;
      for (const p of path) {
        u0 = Math.min(u0, p.x); u1 = Math.max(u1, p.x);
        v0 = Math.min(v0, p.y); v1 = Math.max(v1, p.y);
      }
      return { u0, v0, u1, v1 };
    }
    return null;
  }

  function boxFor(clip: Clip, t: number): Box | null {
    if (!VISUAL.has(clip.type)) return null;
    if (t < clip.start || t > clip.end) return null;
    const localT = (t - clip.start) * (clip.speed || 1);
    const uv = uvBoxFor(clip, localT);
    if (!uv) return null;
    const W = app.project.width;
    const H = app.project.height;
    const sx = propAt(clip, "scale_x", localT);
    const sy = propAt(clip, "scale_y", localT);
    const rot = propAt(clip, "rotation", localT);
    const rad = (rot * Math.PI) / 180;
    const c = Math.cos(rad);
    const s = Math.sin(rad);
    // uv-box center offset from the quad center (0.5,0.5), scaled, then rotated.
    const ddx = ((uv.u0 + uv.u1) / 2 - 0.5) * sx * W;
    const ddy = ((uv.v0 + uv.v1) / 2 - 0.5) * sy * H;
    return {
      clip,
      cx: propAt(clip, "pos_x", localT) * W + ddx * c - ddy * s,
      cy: propAt(clip, "pos_y", localT) * H + ddx * s + ddy * c,
      hw: ((uv.u1 - uv.u0) / 2) * sx * W,
      hh: ((uv.v1 - uv.v0) / 2) * sy * H,
      rotDeg: rot,
    };
  }

  function targetBox(): Box | null {
    const id = app.selection[0];
    if (id === undefined) return null;
    for (const track of app.project.tracks) {
      for (const clip of track.clips) {
        if (clip.id === id) return boxFor(clip, app.playhead);
      }
    }
    return null;
  }

  // -- geometry helpers ------------------------------------------------------
  function axes(box: Box): { ax: [number, number]; ay: [number, number] } {
    const rad = (box.rotDeg * Math.PI) / 180;
    return { ax: [Math.cos(rad), Math.sin(rad)], ay: [-Math.sin(rad), Math.cos(rad)] };
  }

  function corner(box: Box, sx: number, sy: number): [number, number] {
    const { ax, ay } = axes(box);
    return [
      box.cx + ax[0] * sx * box.hw + ay[0] * sy * box.hh,
      box.cy + ax[1] * sx * box.hw + ay[1] * sy * box.hh,
    ];
  }

  function toLocal(box: Box, x: number, y: number): [number, number] {
    const { ax, ay } = axes(box);
    const dx = x - box.cx;
    const dy = y - box.cy;
    return [dx * ax[0] + dy * ax[1], dx * ay[0] + dy * ay[1]];
  }

  function rotateKnob(box: Box): [number, number] {
    const [tx, ty] = corner(box, 0, -1);
    const { ay } = axes(box);
    const dist = ROT_DIST / px();
    const kx = tx + ay[0] * dist;
    const ky = ty + ay[1] * dist;
    // Clip top edge: keep the knob inside the overlay so it stays grabbable.
    if (ky * py() < 10) return [tx - ay[0] * dist, ty - ay[1] * dist];
    return [kx, ky];
  }

  function handleAt(box: Box, mx: number, my: number): HandleKind {
    const kx = rotateKnob(box);
    const kr = (HIT_R + 3) / px();
    if ((mx - kx[0]) ** 2 + (my - kx[1]) ** 2 <= kr * kr) return "rotate";
    const [lx, ly] = toLocal(box, mx, my);
    const tol = HIT_R / px();
    const hx = box.hw;
    const hy = box.hh;
    const nearX = (v: number, t: number): boolean => Math.abs(v - t) <= tol;
    const onL = nearX(lx, -hx);
    const onR = nearX(lx, hx);
    const onT = nearX(ly, -hy);
    const onB = nearX(ly, hy);
    if (onT && onL) return "tl";
    if (onT && onR) return "tr";
    if (onB && onL) return "bl";
    if (onB && onR) return "br";
    if (onL && Math.abs(ly) <= hy) return "l";
    if (onR && Math.abs(ly) <= hy) return "r";
    if (onT && Math.abs(lx) <= hx) return "t";
    if (onB && Math.abs(lx) <= hx) return "b";
    if (Math.abs(lx) <= hx && Math.abs(ly) <= hy) return "body";
    return "none";
  }

  /** Topmost visual clip whose bbox contains the point (project px). */
  function pickAt(mx: number, my: number): Clip | null {
    let best: Clip | null = null;
    let bestArea = Infinity;
    for (const track of app.project.tracks) {
      if (track.muted) continue;
      for (const clip of track.clips) {
        const box = boxFor(clip, app.playhead);
        if (!box) continue; // not active at the playhead
        const [lx, ly] = toLocal(box, mx, my);
        if (Math.abs(lx) <= box.hw && Math.abs(ly) <= box.hh) {
          const area = box.hw * box.hh;
          if (!best || area < bestArea) { best = clip; bestArea = area; }
        }
      }
    }
    return best;
  }

  // -- drawing ---------------------------------------------------------------
  function draw(): void {
    layout();
    const ctx = overlay.getContext("2d");
    if (!ctx) return;
    const dpr = window.devicePixelRatio || 1;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, cssW, cssH);

    const box = drag ? boxFromDrag() : targetBox();
    if (!box) return;
    const sx = px();
    const sy = py();

    // Snap guides while dragging.
    if (drag && (drag.snappedX || drag.snappedY)) {
      ctx.strokeStyle = "rgba(167,139,250,0.85)";
      ctx.lineWidth = 1;
      ctx.setLineDash([4, 4]);
      if (drag.snappedX) {
        ctx.beginPath();
        ctx.moveTo(cssW / 2, 0);
        ctx.lineTo(cssW / 2, cssH);
        ctx.stroke();
      }
      if (drag.snappedY) {
        ctx.beginPath();
        ctx.moveTo(0, cssH / 2);
        ctx.lineTo(cssW, cssH / 2);
        ctx.stroke();
      }
      ctx.setLineDash([]);
    }

    // Rotated outline.
    const corners = [corner(box, -1, -1), corner(box, 1, -1), corner(box, 1, 1), corner(box, -1, 1)];
    ctx.strokeStyle = "rgba(167,139,250,0.95)";
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    corners.forEach(([x, y], i) => { if (i === 0) ctx.moveTo(x * sx, y * sy); else ctx.lineTo(x * sx, y * sy); });
    ctx.closePath();
    ctx.stroke();

    // Rotate link + knob.
    const [tx, ty] = corner(box, 0, -1);
    const [kx, ky] = rotateKnob(box);
    ctx.strokeStyle = "rgba(255,255,255,0.4)";
    ctx.beginPath();
    ctx.moveTo(tx * sx, ty * sy);
    ctx.lineTo(kx * sx, ky * sy);
    ctx.stroke();
    ctx.beginPath();
    ctx.arc(kx * sx, ky * sy, HANDLE_R + 1.5, 0, Math.PI * 2);
    ctx.fillStyle = drag?.rotSnapped ? "rgba(167,139,250,1)" : "#14151a";
    ctx.fill();
    ctx.strokeStyle = "rgba(167,139,250,0.95)";
    ctx.stroke();

    // Corner + edge handles.
    const spots: [number, number][] = [
      ...corners,
      corner(box, 0, -1), corner(box, 0, 1), corner(box, -1, 0), corner(box, 1, 0),
    ];
    for (const [hx2, hy2] of spots) {
      ctx.beginPath();
      ctx.roundRect(hx2 * sx - HANDLE_R, hy2 * sy - HANDLE_R, HANDLE_R * 2, HANDLE_R * 2, 2);
      ctx.fillStyle = "#ffffff";
      ctx.fill();
      ctx.strokeStyle = "rgba(20,21,26,0.8)";
      ctx.lineWidth = 1;
      ctx.stroke();
    }
  }

  function boxFromDrag(): Box | null {
    if (!drag) return null;
    return boxFor(drag.clip, app.playhead);
  }

  // -- interaction -----------------------------------------------------------
  function toProject(e: PointerEvent): [number, number] {
    const r = overlay.getBoundingClientRect();
    return [
      ((e.clientX - r.left) / Math.max(r.width, 1)) * app.project.width,
      ((e.clientY - r.top) / Math.max(r.height, 1)) * app.project.height,
    ];
  }

  overlay.addEventListener("pointerdown", (e) => {
    if (e.button !== 0) return;
    const [mx, my] = toProject(e);
    const box = targetBox();
    const handle = box ? handleAt(box, mx, my) : "none";

    let clip: Clip | null = null;
    let handleKind: HandleKind = handle;
    if (box && handle !== "none") {
      clip = box.clip;
    } else {
      clip = pickAt(mx, my);
      if (!clip) {
        app.setSelection([]);
        draw();
        return;
      }
      app.setSelection([clip.id]);
      handleKind = "body";
    }
    if (!clip) return;

    const localT = (app.playhead - clip.start) * (clip.speed || 1);
    const fresh = boxFor(clip, app.playhead);
    if (!fresh) return;
    const [lx, ly] = toLocal(fresh, mx, my);
    drag = {
      handle: handleKind,
      clip,
      startMouseX: mx,
      startMouseY: my,
      startLocalX: lx,
      startLocalY: ly,
      startPosX: propAt(clip, "pos_x", localT),
      startPosY: propAt(clip, "pos_y", localT),
      startScaleX: propAt(clip, "scale_x", localT),
      startScaleY: propAt(clip, "scale_y", localT),
      startRot: propAt(clip, "rotation", localT),
      startAngle: Math.atan2(my - fresh.cy, mx - fresh.cx),
      cx: fresh.cx,
      cy: fresh.cy,
      hw: fresh.hw,
      hh: fresh.hh,
      moved: false,
      snappedX: false,
      snappedY: false,
      rotSnapped: false,
    };
    overlay.setPointerCapture(e.pointerId);
    e.preventDefault();
  });

  overlay.addEventListener("pointermove", (e) => {
    const [mx, my] = toProject(e);
    if (!drag) {
      const box = targetBox();
      overlay.style.cursor = box ? cursorFor(handleAt(box, mx, my), box.rotDeg) : "default";
      return;
    }
    const W = app.project.width;
    const H = app.project.height;
    const clip = drag.clip;
    const d = drag;

    if (d.handle === "body") {
      let nx = d.startPosX + (mx - d.startMouseX) / W;
      let ny = d.startPosY + (my - d.startMouseY) / H;
      // Center snap (8 css px), both axes.
      const tolX = 8 / Math.max(cssW, 1);
      const tolY = 8 / Math.max(cssH, 1);
      d.snappedX = Math.abs(nx - 0.5) < tolX;
      d.snappedY = Math.abs(ny - 0.5) < tolY;
      if (d.snappedX) nx = 0.5;
      if (d.snappedY) ny = 0.5;
      clip.props["pos_x"] = nx;
      clip.props["pos_y"] = ny;
    } else if (d.handle === "rotate") {
      const ang = Math.atan2(my - d.cy, mx - d.cx);
      let rot = d.startRot + ((ang - d.startAngle) * 180) / Math.PI;
      const snapped = Math.round(rot / 45) * 45;
      d.rotSnapped = !e.shiftKey && Math.abs(rot - snapped) < 6;
      if (d.rotSnapped) rot = snapped;
      clip.props["rotation"] = ((rot % 360) + 360) % 360;
    } else {
      // Scale in the clip's local frame.
      const box: Box = { clip, cx: d.cx, cy: d.cy, hw: d.hw, hh: d.hh, rotDeg: d.startRot };
      const [lx, ly] = toLocal(box, mx, my);
      const l0x = d.startLocalX;
      const l0y = d.startLocalY;
      const MIN = 0.05;
      if (d.handle === "l" || d.handle === "r") {
        if (Math.abs(l0x) > 1e-4) clip.props["scale_x"] = Math.max(MIN, d.startScaleX * (lx / l0x));
      } else if (d.handle === "t" || d.handle === "b") {
        if (Math.abs(l0y) > 1e-4) clip.props["scale_y"] = Math.max(MIN, d.startScaleY * (ly / l0y));
      } else {
        const denom = l0x * l0x + l0y * l0y;
        if (denom > 1e-6) {
          const f = Math.max(MIN, (lx * l0x + ly * l0y) / denom);
          clip.props["scale_x"] = Math.max(MIN, d.startScaleX * f);
          clip.props["scale_y"] = Math.max(MIN, d.startScaleY * f);
        }
      }
    }
    d.moved = true;
    if (!batchOpen) {
      app.history.beginBatch("Transform clip", app.project);
      batchOpen = true;
    }
    app.events.emit("project:changed", { structural: false });
    draw();
  });

  overlay.addEventListener("pointerup", (e) => {
    if (!drag) return;
    if (batchOpen) {
      app.history.endBatch();
      batchOpen = false;
    }
    drag = null;
    overlay.releasePointerCapture(e.pointerId);
    app.events.emit("project:changed", { structural: true });
    draw();
  });

  overlay.addEventListener("pointercancel", () => {
    if (batchOpen) {
      app.history.endBatch();
      batchOpen = false;
    }
    drag = null;
    draw();
  });

  function cursorFor(h: HandleKind, rotDeg: number): string {
    if (h === "body") return "move";
    if (h === "rotate") return "grab";
    if (h === "none") return "default";
    // Rotate the resize cursors with the clip (nearest 45°).
    const base: Record<string, number> = {
      tl: 315, tr: 45, br: 135, bl: 225, t: 0, r: 90, b: 180, l: 270,
    };
    const names = ["ns", "nesw", "ew", "nwse"] as const;
    const angle = (((base[h] ?? 0) + rotDeg) % 180 + 180) % 180;
    const idx = Math.round(angle / 45) % 4;
    return `${names[idx]}-resize`;
  }

  app.events.on("selection", draw);
  app.events.on("project:changed", () => { if (!drag) draw(); });
  app.events.on("playhead", draw);
  layout();
  draw();
}
