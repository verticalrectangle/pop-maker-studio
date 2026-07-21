import { App } from "../core/app";
import type { MediaEntry } from "../media/store";
import {
  type Clip, type ClipType, type Track,
  addTrack, clipDuration, findClip, freshId, insertClip, makeClip, splitClip,
} from "../core/project";

// --- Layout constants -------------------------------------------------------
const RULER_H = 24;
const GUTTER_W = 140;
const TRACK_H = 56;
const CLIP_PAD = 4;
const EDGE_HANDLE = 6; // px grab zone for trim handles
const SNAP_PX = 6; // px snap threshold
const MIN_PXPS = 5;
const MAX_PXPS = 400;
const MIN_CLIP_DUR = 0.05;

const TYPE_FILL: Record<ClipType, string> = {
  video: "#6d5fd0",
  audio: "#2f9e6e",
  image: "#6d5fd0",
  text: "#d99a2b",
  shape: "#9b5fd0",
  lyric: "#d99a2b",
  fx: "#e05a9b",
  bodyfx: "#e05a9b",
  camera: "#d94f3d",
};

function isFx(t: ClipType): boolean {
  return t === "fx" || t === "bodyfx";
}

function fmtTime(s: number, decimals = 0): string {
  const m = Math.floor(s / 60);
  const sec = s - m * 60;
  const str = sec.toFixed(decimals);
  return `${m}:${str.padStart(decimals ? 3 + decimals : 2, "0")}`;
}

function clipLabel(clip: Clip): string {
  if (clip.text && clip.text.trim()) return clip.text;
  return clip.type;
}

function roundRect(
  ctx: CanvasRenderingContext2D,
  x: number, y: number, w: number, h: number, r: number,
): void {
  const rr = Math.min(r, w / 2, h / 2);
  ctx.beginPath();
  ctx.moveTo(x + rr, y);
  ctx.arcTo(x + w, y, x + w, y + h, rr);
  ctx.arcTo(x + w, y + h, x, y + h, rr);
  ctx.arcTo(x, y + h, x, y, rr);
  ctx.arcTo(x, y, x + w, y, rr);
  ctx.closePath();
}

// --- Interaction state ------------------------------------------------------
type Mode =
  | { kind: "idle" }
  | { kind: "marquee"; startX: number; startY: number; curX: number; curY: number; additive: boolean; baseIds: number[] }
  | { kind: "move"; primaryId: number; origins: Map<number, { start: number; end: number; trackIdx: number }>; lastDt: number; startT: number }
  | { kind: "trim"; clipId: number; edge: "left" | "right"; origStart: number; origEnd: number; origInPoint: number; trackIdx: number }
  | { kind: "pan"; startScrollT: number; startScrollY: number; startX: number; startY: number }
  | { kind: "scrub" }
  | { kind: "loop-body"; offset: number }
  | { kind: "loop-edge"; edge: "left" | "right" };

interface HitClip {
  trackIdx: number;
  clip: Clip;
  edge: "left" | "right" | "body";
}

// --- Module -----------------------------------------------------------------
export function initTimeline(app: App, canvas: HTMLCanvasElement): void {
  const parent = canvas.parentElement;
  if (!parent) return;

  let pxPerSec = 40;
  let scrollT = 0;
  let scrollY = 0;
  let cssW = 0;
  let cssH = 0;
  let dpr = 1;
  let mode: Mode = { kind: "idle" };

  canvas.tabIndex = 0;
  canvas.style.outline = "none";

  // -- coordinate transforms --
  function timeToX(t: number): number {
    return GUTTER_W + (t - scrollT) * pxPerSec;
  }
  function xToTime(x: number): number {
    return scrollT + (x - GUTTER_W) / pxPerSec;
  }
  function trackY(i: number): number {
    return RULER_H + i * TRACK_H - scrollY;
  }
  function trackAtY(y: number): number {
    return Math.floor((y - RULER_H + scrollY) / TRACK_H);
  }
  function clampScroll(): void {
    const visible = (cssW - GUTTER_W) / pxPerSec;
    scrollT = Math.max(0, Math.min(scrollT, Math.max(0, app.project.duration + 1 - visible)));
    const maxSy = Math.max(0, app.project.tracks.length * TRACK_H - (cssH - RULER_H - 32));
    scrollY = Math.max(0, Math.min(scrollY, maxSy));
  }

  // -- resize --
  function resize(): void {
    const p = canvas.parentElement;
    if (!p) return;
    dpr = window.devicePixelRatio || 1;
    cssW = p.clientWidth;
    cssH = p.clientHeight;
    canvas.width = Math.max(1, Math.floor(cssW * dpr));
    canvas.height = Math.max(1, Math.floor(cssH * dpr));
    canvas.style.width = `${cssW}px`;
    canvas.style.height = `${cssH}px`;
    clampScroll();
  }
  const ro = new ResizeObserver(() => {
    resize();
    draw();
  });
  ro.observe(parent);

  // -- drawing --
  function draw(): void {
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    ctx.save();
    ctx.scale(dpr, dpr);
    ctx.clearRect(0, 0, cssW, cssH);

    drawBackground(ctx);
    drawGutter(ctx);
    drawRuler(ctx);
    drawTracks(ctx);
    drawOverlay(ctx);
    ctx.restore();
  }

  function drawBackground(ctx: CanvasRenderingContext2D): void {
    // gutter
    ctx.fillStyle = "#101016";
    ctx.fillRect(0, 0, GUTTER_W, cssH);
    // tracks area
    ctx.fillStyle = "#15151c";
    ctx.fillRect(GUTTER_W, 0, cssW - GUTTER_W, cssH);
    // track row stripes + horizontal grid
    ctx.strokeStyle = "#1e1e28";
    ctx.lineWidth = 1;
    for (let i = 0; i <= app.project.tracks.length; i++) {
      const y = trackY(i);
      ctx.beginPath();
      ctx.moveTo(0, y + 0.5);
      ctx.lineTo(cssW, y + 0.5);
      ctx.stroke();
    }
  }

  function drawGutter(ctx: CanvasRenderingContext2D): void {
    // header divider
    ctx.fillStyle = "#0c0c11";
    ctx.fillRect(0, 0, GUTTER_W, RULER_H);
    ctx.strokeStyle = "#2a2a36";
    ctx.beginPath();
    ctx.moveTo(GUTTER_W + 0.5, 0);
    ctx.lineTo(GUTTER_W + 0.5, cssH);
    ctx.stroke();

    for (let i = 0; i < app.project.tracks.length; i++) {
      const track = app.project.tracks[i]!;
      const y = trackY(i);
      if (y + TRACK_H < RULER_H || y > cssH) continue;
      // name
      ctx.fillStyle = track.locked ? "#8f93a3" : "#f2f3f7";
      ctx.font = "12px Inter, system-ui, sans-serif";
      ctx.textBaseline = "middle";
      ctx.textAlign = "left";
      ctx.save();
      ctx.beginPath();
      ctx.rect(8, y + 8, GUTTER_W - 70, TRACK_H - 16);
      ctx.clip();
      ctx.fillText(track.name, 10, y + TRACK_H / 2);
      ctx.restore();
      // mute / lock buttons
      drawIconBtn(ctx, GUTTER_W - 58, y + 18, 20, 20, "M", track.muted, track.muted ? "#f06a6a" : "#2a2a36");
      drawIconBtn(ctx, GUTTER_W - 34, y + 18, 20, 20, "L", track.locked, track.locked ? "#f0a05c" : "#2a2a36");
    }
    // add-track button
    const addY = trackY(app.project.tracks.length) + 6;
    if (addY < cssH) {
      ctx.fillStyle = "#1d1d26";
      roundRect(ctx, 8, addY, GUTTER_W - 16, 22, 4);
      ctx.fill();
      ctx.fillStyle = "#8f93a3";
      ctx.font = "12px Inter, system-ui, sans-serif";
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      ctx.fillText("+ Add track", GUTTER_W / 2, addY + 11);
    }
  }

  function drawIconBtn(
    ctx: CanvasRenderingContext2D,
    x: number, y: number, w: number, h: number,
    label: string, active: boolean, activeColor: string,
  ): void {
    ctx.fillStyle = active ? activeColor : "#1d1d26";
    roundRect(ctx, x, y, w, h, 3);
    ctx.fill();
    ctx.fillStyle = active ? "#fff" : "#8f93a3";
    ctx.font = "bold 11px Inter, system-ui, sans-serif";
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText(label, x + w / 2, y + h / 2 + 0.5);
  }

  function pickStep(): number {
    const NICE = [0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800];
    for (const s of NICE) if (s * pxPerSec >= 60) return s;
    return NICE[NICE.length - 1]!;
  }
  function stepDecimals(step: number): number {
    if (step >= 1) return 0;
    if (step >= 0.5) return 1;
    if (step >= 0.25) return 2;
    return 1;
  }

  function drawRuler(ctx: CanvasRenderingContext2D): void {
    ctx.fillStyle = "#0c0c11";
    ctx.fillRect(GUTTER_W, 0, cssW - GUTTER_W, RULER_H);
    ctx.strokeStyle = "#2a2a36";
    ctx.beginPath();
    ctx.moveTo(0, RULER_H + 0.5);
    ctx.lineTo(cssW, RULER_H + 0.5);
    ctx.stroke();

    const step = pickStep();
    const dec = stepDecimals(step);
    const t0 = Math.floor(scrollT / step) * step;
    const tMax = scrollT + (cssW - GUTTER_W) / pxPerSec;
    ctx.fillStyle = "#8f93a3";
    ctx.font = "10px Inter, system-ui, sans-serif";
    ctx.textBaseline = "middle";
    ctx.textAlign = "left";
    for (let t = t0; t <= tMax + step; t += step) {
      if (t < 0) continue;
      const x = timeToX(t);
      if (x < GUTTER_W - 1 || x > cssW) continue;
      ctx.strokeStyle = "#2a2a36";
      ctx.beginPath();
      ctx.moveTo(x + 0.5, RULER_H - 6);
      ctx.lineTo(x + 0.5, RULER_H);
      ctx.stroke();
      ctx.fillText(fmtTime(t, dec), x + 3, RULER_H / 2);
    }

    // beats (faint)
    if (app.project.beats.length) {
      ctx.strokeStyle = "rgba(167,139,250,0.18)";
      ctx.lineWidth = 1;
      for (const b of app.project.beats) {
        if (b < scrollT || b > tMax) continue;
        const x = timeToX(b);
        ctx.beginPath();
        ctx.moveTo(x, RULER_H);
        ctx.lineTo(x, cssH);
        ctx.stroke();
      }
    }

    // chapter markers
    for (const m of app.project.markers) {
      const x = timeToX(m.time);
      if (x < GUTTER_W || x > cssW) continue;
      ctx.strokeStyle = m.color || "#a78bfa";
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      ctx.moveTo(x + 0.5, 0);
      ctx.lineTo(x + 0.5, cssH);
      ctx.stroke();
      ctx.fillStyle = m.color || "#a78bfa";
      ctx.font = "10px Inter, system-ui, sans-serif";
      ctx.textAlign = "left";
      ctx.fillText(m.label, x + 3, 8);
    }

    // loop brace
    if (app.loop) {
      const x0 = timeToX(app.loop.start);
      const x1 = timeToX(app.loop.end);
      ctx.fillStyle = "rgba(167,139,250,0.18)";
      ctx.fillRect(Math.max(x0, GUTTER_W), RULER_H - 8, Math.min(x1, cssW) - Math.max(x0, GUTTER_W), 8);
      ctx.strokeStyle = "#a78bfa";
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      ctx.moveTo(x0, RULER_H - 8);
      ctx.lineTo(x0, RULER_H);
      ctx.moveTo(x1, RULER_H - 8);
      ctx.lineTo(x1, RULER_H);
      ctx.stroke();
    }

    // playhead
    const px = timeToX(app.playhead);
    if (px >= GUTTER_W && px <= cssW) {
      ctx.strokeStyle = "#f0a05c";
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      ctx.moveTo(px + 0.5, 0);
      ctx.lineTo(px + 0.5, cssH);
      ctx.stroke();
      // handle
      ctx.fillStyle = "#f0a05c";
      ctx.beginPath();
      ctx.moveTo(px, 0);
      ctx.lineTo(px + 6, 0);
      ctx.lineTo(px, 10);
      ctx.lineTo(px - 6, 0);
      ctx.closePath();
      ctx.fill();
    }
  }

  function drawTracks(ctx: CanvasRenderingContext2D): void {
    for (let i = 0; i < app.project.tracks.length; i++) {
      const track = app.project.tracks[i]!;
      const y = trackY(i);
      if (y + TRACK_H < RULER_H || y > cssH) continue;
      if (track.muted) {
        ctx.fillStyle = "rgba(0,0,0,0.25)";
        ctx.fillRect(GUTTER_W, y, cssW - GUTTER_W, TRACK_H);
      }
      for (const clip of track.clips) {
        drawClip(ctx, clip, i, app.selection.includes(clip.id));
      }
    }
  }

  function drawClip(ctx: CanvasRenderingContext2D, clip: Clip, trackIdx: number, selected: boolean): void {
    const x = timeToX(clip.start);
    const w = (clip.end - clip.start) * pxPerSec;
    const y = trackY(trackIdx) + CLIP_PAD;
    const h = TRACK_H - CLIP_PAD * 2;
    if (x + w < GUTTER_W || x > cssW) return;
    const fx = isFx(clip.type);
    const drawW = Math.max(w, 2);

    ctx.save();
    if (fx) {
      ctx.fillStyle = "rgba(214,69,127,0.14)";
      roundRect(ctx, x, y, drawW, h, 4);
      ctx.fill();
      ctx.strokeStyle = TYPE_FILL[clip.type];
      ctx.lineWidth = 1.5;
      ctx.setLineDash(clip.type === "bodyfx" ? [4, 3] : []);
      ctx.stroke();
      ctx.setLineDash([]);
    } else {
      ctx.fillStyle = TYPE_FILL[clip.type];
      roundRect(ctx, x, y, drawW, h, 4);
      ctx.fill();
      // subtle top highlight
      ctx.fillStyle = "rgba(255,255,255,0.08)";
      roundRect(ctx, x, y, drawW, h / 2, 4);
      ctx.fill();
    }

    // content
    ctx.save();
    roundRect(ctx, x, y, drawW, h, 4);
    ctx.clip();
    ctx.fillStyle = "#fff";
    ctx.font = "11px Inter, system-ui, sans-serif";
    ctx.textBaseline = "top";
    ctx.textAlign = "left";
    ctx.fillText(clipLabel(clip), x + 6, y + 3);
    if (clip.type === "audio" && clip.source) {
      const entry = app.media.get(clip.source);
      if (entry?.peaks) drawWaveform(ctx, x, y, drawW, h, entry.peaks);
    }
    ctx.restore();

    if (selected) {
      ctx.strokeStyle = "#a78bfa";
      ctx.lineWidth = 2;
      roundRect(ctx, x - 1, y - 1, drawW + 2, h + 2, 5);
      ctx.stroke();
    }
    ctx.restore();
  }

  function drawWaveform(
    ctx: CanvasRenderingContext2D,
    x: number, y: number, w: number, h: number, peaks: Float32Array,
  ): void {
    const mid = y + h / 2 + 4;
    const amp = h / 2 - 8;
    ctx.strokeStyle = "rgba(255,255,255,0.55)";
    ctx.lineWidth = 1;
    ctx.beginPath();
    const n = peaks.length;
    const step = Math.max(1, Math.floor(n / Math.max(w, 1)));
    for (let i = 0; i < n; i += step) {
      const px = x + (i / n) * w;
      const p = peaks[i] ?? 0;
      ctx.moveTo(px, mid - p * amp);
      ctx.lineTo(px, mid + p * amp);
    }
    ctx.stroke();
  }

  function drawOverlay(ctx: CanvasRenderingContext2D): void {
    if (mode.kind === "marquee") {
      const x = Math.min(mode.startX, mode.curX);
      const y = Math.min(mode.startY, mode.curY);
      const w = Math.abs(mode.curX - mode.startX);
      const h = Math.abs(mode.curY - mode.startY);
      ctx.fillStyle = "rgba(167,139,250,0.15)";
      ctx.strokeStyle = "#a78bfa";
      ctx.lineWidth = 1;
      ctx.fillRect(x, y, w, h);
      ctx.strokeRect(x + 0.5, y + 0.5, w, h);
    }
  }

  // -- hit testing --
  function hitClip(x: number, y: number): HitClip | null {
    if (x < GUTTER_W || y < RULER_H) return null;
    const ti = trackAtY(y);
    if (ti < 0 || ti >= app.project.tracks.length) return null;
    const track = app.project.tracks[ti]!;
    const cy = trackY(ti) + CLIP_PAD;
    const ch = TRACK_H - CLIP_PAD * 2;
    if (y < cy || y > cy + ch) return null;
    for (const clip of track.clips) {
      const cx = timeToX(clip.start);
      const cw = (clip.end - clip.start) * pxPerSec;
      if (cw >= EDGE_HANDLE * 2) {
        if (x >= cx - EDGE_HANDLE && x < cx) return { trackIdx: ti, clip, edge: "left" };
        if (x > cx + cw && x <= cx + cw + EDGE_HANDLE) return { trackIdx: ti, clip, edge: "right" };
      }
      if (x >= cx && x <= cx + cw) {
        if (cw >= EDGE_HANDLE * 2 && x <= cx + EDGE_HANDLE) return { trackIdx: ti, clip, edge: "left" };
        if (cw >= EDGE_HANDLE * 2 && x >= cx + cw - EDGE_HANDLE) return { trackIdx: ti, clip, edge: "right" };
        return { trackIdx: ti, clip, edge: "body" };
      }
    }
    return null;
  }

  function hitGutterButton(x: number, y: number): { kind: "mute" | "lock"; trackIdx: number } | { kind: "add" } | null {
    if (x > GUTTER_W) return null;
    for (let i = 0; i < app.project.tracks.length; i++) {
      const yTop = trackY(i) + 18;
      if (y >= yTop && y <= yTop + 20) {
        if (x >= GUTTER_W - 58 && x <= GUTTER_W - 38) return { kind: "mute", trackIdx: i };
        if (x >= GUTTER_W - 34 && x <= GUTTER_W - 14) return { kind: "lock", trackIdx: i };
      }
    }
    const addY = trackY(app.project.tracks.length) + 6;
    if (y >= addY && y <= addY + 22 && x >= 8 && x <= GUTTER_W - 8) return { kind: "add" };
    return null;
  }

  function hitLoopBrace(x: number, y: number): "body" | "left" | "right" | null {
    if (!app.loop || y > RULER_H || y < RULER_H - 8) return null;
    const x0 = timeToX(app.loop.start);
    const x1 = timeToX(app.loop.end);
    if (x < x0 - 4 || x > x1 + 4) return null;
    if (Math.abs(x - x0) <= 5) return "left";
    if (Math.abs(x - x1) <= 5) return "right";
    return "body";
  }

  // -- snapping --
  function snapPoints(exclude: Set<number>): number[] {
    const pts: number[] = [app.playhead];
    for (const m of app.project.markers) pts.push(m.time);
    for (const b of app.project.beats) pts.push(b);
    for (const t of app.project.tracks) {
      for (const c of t.clips) {
        if (exclude.has(c.id)) continue;
        pts.push(c.start, c.end);
      }
    }
    return pts;
  }

  function nearestSnap(t: number, pts: number[], threshold: number): number | null {
    let best: number | null = null;
    let bestD = threshold;
    for (const p of pts) {
      const d = Math.abs(p - t);
      if (d < bestD) {
        bestD = d;
        best = p;
      }
    }
    return best;
  }

  function fxOverlaps(track: Track, clip: Clip, start: number, end: number, exclude: Set<number>): boolean {
    for (const c of track.clips) {
      if (c.id === clip.id || exclude.has(c.id)) continue;
      if (isFx(c.type) && start < c.end && end > c.start) return true;
    }
    return false;
  }


  // -- pointer handling --
  function getPos(e: MouseEvent): { x: number; y: number } {
    const rect = canvas.getBoundingClientRect();
    return { x: e.clientX - rect.left, y: e.clientY - rect.top };
  }

  canvas.addEventListener("mousedown", (e) => {
    canvas.focus();
    const { x, y } = getPos(e);
    // middle-drag pan
    if (e.button === 1) {
      e.preventDefault();
      mode = { kind: "pan", startScrollT: scrollT, startScrollY: scrollY, startX: x, startY: y };
      return;
    }
    if (e.button !== 0) return;

    // gutter buttons
    const gb = hitGutterButton(x, y);
    if (gb) {
      if (gb.kind === "add") {
        app.mutate("Add track", () => {
          addTrack(app.project, `Track ${app.project.tracks.length + 1}`, app.project.tracks.length);
        });
      } else {
        const track = app.project.tracks[gb.trackIdx];
        if (track) {
          app.mutate(gb.kind === "mute" ? "Mute track" : "Lock track", () => {
            if (gb.kind === "mute") track.muted = !track.muted;
            else track.locked = !track.locked;
          });
        }
      }
      return;
    }

    // ruler
    if (y < RULER_H) {
      if (x < GUTTER_W) return;
      // loop brace?
      const lb = hitLoopBrace(x, y);
      if (lb) {
        if (lb === "body") {
          mode = { kind: "loop-body", offset: xToTime(x) - app.loop!.start };
        } else {
          mode = { kind: "loop-edge", edge: lb };
        }
        return;
      }
      // scrub playhead
      app.setPlayhead(xToTime(x));
      mode = { kind: "scrub" };
      return;
    }

    // clip hit
    const hit = hitClip(x, y);
    if (hit) {
      const track = app.project.tracks[hit.trackIdx]!;
      if (track.locked) return;
      if (hit.edge === "left" || hit.edge === "right") {
        if (!app.selection.includes(hit.clip.id)) app.setSelection([hit.clip.id]);
        mode = {
          kind: "trim",
          clipId: hit.clip.id,
          edge: hit.edge,
          origStart: hit.clip.start,
          origEnd: hit.clip.end,
          origInPoint: hit.clip.inPoint,
          trackIdx: hit.trackIdx,
        };
        return;
      }
      // body — select + begin move
      if (!app.selection.includes(hit.clip.id)) {
        app.setSelection(e.shiftKey ? [...app.selection, hit.clip.id] : [hit.clip.id]);
      }
      const sel = app.selection.includes(hit.clip.id) ? app.selection : [hit.clip.id];
      const origins = new Map<number, { start: number; end: number; trackIdx: number }>();
      for (const id of sel) {
        const f = findClip(app.project, id);
        if (!f) continue;
        if (f.track.locked) continue;
        const ti = app.project.tracks.indexOf(f.track);
        origins.set(id, { start: f.clip.start, end: f.clip.end, trackIdx: ti });
      }
      mode = {
        kind: "move",
        primaryId: hit.clip.id,
        origins,
        lastDt: 0,
        startT: xToTime(x),
      };
      return;
    }

    // empty — marquee (unless shift-extend with existing selection)
    mode = { kind: "marquee", startX: x, startY: y, curX: x, curY: y, additive: e.shiftKey, baseIds: e.shiftKey ? [...app.selection] : [] };
    if (!e.shiftKey) app.setSelection([]);
  });

  canvas.addEventListener("dblclick", (e) => {
    const { x, y } = getPos(e);
    // double-click empty ruler sets playhead
    if (y < RULER_H && x >= GUTTER_W) {
      app.setPlayhead(xToTime(x));
      return;
    }
    // double-click gutter track name → rename
    if (x < GUTTER_W && y >= RULER_H) {
      const ti = trackAtY(y);
      const track = app.project.tracks[ti];
      if (track) {
        const name = window.prompt("Track name", track.name);
        if (name && name.trim()) {
          app.mutate("Rename track", () => {
            track.name = name.trim();
          });
        }
      }
    }
  });

  window.addEventListener("mousemove", (e) => {
    if (mode.kind === "idle") return;
    const { x, y } = getPos(e);

    if (mode.kind === "pan") {
      scrollT = mode.startScrollT - (x - mode.startX) / pxPerSec;
      scrollY = mode.startScrollY - (y - mode.startY);
      clampScroll();
      draw();
      return;
    }
    if (mode.kind === "scrub") {
      app.setPlayhead(Math.max(0, xToTime(x)));
      return;
    }
    if (mode.kind === "marquee") {
      mode = { ...mode, curX: x, curY: y };
      draw();
      return;
    }
    if (mode.kind === "loop-body") {
      const t = Math.max(0, xToTime(x) - mode.offset);
      const len = app.loop!.end - app.loop!.start;
      app.loop = { start: t, end: t + len };
      draw();
      return;
    }
    if (mode.kind === "loop-edge") {
      const t = Math.max(0, xToTime(x));
      if (mode.edge === "left") {
        app.loop = { start: Math.min(t, app.loop!.end - 0.1), end: app.loop!.end };
      } else {
        app.loop = { start: app.loop!.start, end: Math.max(t, app.loop!.start + 0.1) };
      }
      draw();
      return;
    }
    if (mode.kind === "move") {
      const rawDt = xToTime(x) - mode.startT;
      const exclude = new Set(mode.origins.keys());
      const pts = snapPoints(exclude);
      const threshold = SNAP_PX / pxPerSec;
      // find best snap across all moved edges
      let bestDt = rawDt;
      let bestDist = threshold;
      for (const [, o] of mode.origins) {
        for (const edge of [o.start + rawDt, o.end + rawDt]) {
          for (const p of pts) {
            const d = Math.abs(p - edge);
            if (d < bestDist) {
              bestDist = d;
              bestDt = rawDt + (p - edge);
            }
          }
        }
      }
      // fx overlap check
      let ok = true;
      for (const [id, o] of mode.origins) {
        const f = findClip(app.project, id);
        if (!f) continue;
        if (isFx(f.clip.type) && fxOverlaps(f.track, f.clip, o.start + bestDt, o.end + bestDt, exclude)) {
          ok = false;
          break;
        }
      }
      if (ok) {
        // revert to origins then apply
        for (const [id, o] of mode.origins) {
          const f = findClip(app.project, id);
          if (!f) continue;
          f.clip.start = o.start + bestDt;
          f.clip.end = o.end + bestDt;
        }
        mode = { ...mode, lastDt: bestDt };
        draw();
      }
      return;
    }
    if (mode.kind === "trim") {
      const f = findClip(app.project, mode.clipId);
      if (!f) return;
      const { clip, track } = f;
      const exclude = new Set([clip.id]);
      const pts = snapPoints(exclude);
      const threshold = SNAP_PX / pxPerSec;
      const t = Math.max(0, xToTime(x));
      if (mode.edge === "left") {
        let ns = t;
        const snapped = nearestSnap(ns, pts, threshold);
        if (snapped !== null) ns = snapped;
        // inPoint >= 0 constraint
        const minStart = mode.origStart - mode.origInPoint / clip.speed;
        ns = Math.max(minStart, Math.min(ns, mode.origEnd - MIN_CLIP_DUR));
        clip.start = ns;
        clip.inPoint = mode.origInPoint + (ns - mode.origStart) * clip.speed;
      } else {
        let ne = t;
        const snapped = nearestSnap(ne, pts, threshold);
        if (snapped !== null) ne = snapped;
        ne = Math.max(mode.origStart + MIN_CLIP_DUR, ne);
        const srcDur = clip.source ? app.media.get(clip.source)?.duration : undefined;
        if (srcDur !== undefined) {
          const maxEnd = mode.origStart + (srcDur - mode.origInPoint) * clip.speed;
          ne = Math.min(ne, maxEnd);
        }
      }
      draw();
      return;
    }
  });

  window.addEventListener("mouseup", () => {
    if (mode.kind === "idle") return;

    if (mode.kind === "marquee") {
      const x0 = Math.min(mode.startX, mode.curX);
      const x1 = Math.max(mode.startX, mode.curX);
      const y0 = Math.min(mode.startY, mode.curY);
      const y1 = Math.max(mode.startY, mode.curY);
      const ids: number[] = [];
      if (x1 - x0 > 3 && y1 - y0 > 3) {
        for (let i = 0; i < app.project.tracks.length; i++) {
          const track = app.project.tracks[i]!;
          if (track.locked) continue;
          const ty = trackY(i) + CLIP_PAD;
          const th = TRACK_H - CLIP_PAD * 2;
          if (ty + th < y0 || ty > y1) continue;
          for (const clip of track.clips) {
            const cx = timeToX(clip.start);
            const cw = (clip.end - clip.start) * pxPerSec;
            if (cx + cw < x0 || cx > x1) continue;
            ids.push(clip.id);
          }
        }
      }
      app.setSelection(mode.additive ? [...new Set([...mode.baseIds, ...ids])] : ids);
      mode = { kind: "idle" };
      return;
    }

    if (mode.kind === "move") {
      const origins = mode.origins;
      const lastDt = mode.lastDt;
      if (Math.abs(lastDt) > 1e-6) {
        // revert then commit one undo step
        for (const [id, o] of origins) {
          const f = findClip(app.project, id);
          if (f) {
            f.clip.start = o.start;
            f.clip.end = o.end;
          }
        }
        app.mutate("Move clips", () => {
          for (const [id, o] of origins) {
            const f = findClip(app.project, id);
            if (!f) continue;
            f.clip.start = o.start + lastDt;
            f.clip.end = o.end + lastDt;
          }
        });
      }
      mode = { kind: "idle" };
      return;
    }

    if (mode.kind === "trim") {
      const f = findClip(app.project, mode.clipId);
      if (f) {
        const finalStart = f.clip.start;
        const finalEnd = f.clip.end;
        const finalIn = f.clip.inPoint;
        // revert then commit
        f.clip.start = mode.origStart;
        f.clip.end = mode.origEnd;
        f.clip.inPoint = mode.origInPoint;
        const changed =
          finalStart !== mode.origStart || finalEnd !== mode.origEnd || finalIn !== mode.origInPoint;
        if (changed) {
          app.mutate("Trim clip", () => {
            f.clip.start = finalStart;
            f.clip.end = finalEnd;
            f.clip.inPoint = finalIn;
          });
        }
      }
      mode = { kind: "idle" };
      return;
    }

    mode = { kind: "idle" };
  });

  // -- wheel: zoom / scroll --
  canvas.addEventListener("wheel", (e) => {
    e.preventDefault();
    const { x } = getPos(e);
    if (e.altKey) {
      // zoom anchored at cursor
      const cursorT = xToTime(x);
      const factor = Math.exp(-e.deltaY * 0.0015);
      pxPerSec = Math.max(MIN_PXPS, Math.min(MAX_PXPS, pxPerSec * factor));
      scrollT = cursorT - (x - GUTTER_W) / pxPerSec;
      clampScroll();
    } else if (e.shiftKey || Math.abs(e.deltaX) > Math.abs(e.deltaY)) {
      scrollT += (e.shiftKey ? e.deltaY : e.deltaX) / pxPerSec;
      clampScroll();
    } else {
      scrollY += e.deltaY;
      clampScroll();
    }
    draw();
  }, { passive: false });
  // -- drag & drop from media bin --
  canvas.addEventListener("dragover", (e) => {
    if (e.dataTransfer?.types.includes("application/pms-media")) {
      e.preventDefault();
      e.dataTransfer.dropEffect = "copy";
    }
  });

  canvas.addEventListener("drop", (e) => {
    e.preventDefault();
    const id = e.dataTransfer?.getData("application/pms-media");
    if (!id) return;
    const entry = app.media.get(id);
    if (!entry) return;
    const { x, y } = getPos(e as DragEvent);
    dropMedia(entry, x, y);
  });

  function dropMedia(entry: MediaEntry, x: number, y: number): void {
    if (y < RULER_H || x < 0 || x > cssW || y > cssH) return;
    const start = Math.max(0, x >= GUTTER_W ? xToTime(x) : app.playhead);
    const dur = entry.duration > 0 ? entry.duration : entry.kind === "image" ? 3 : 1;
    const type: ClipType = entry.kind === "audio" ? "audio" : entry.kind === "image" ? "image" : "video";

    app.mutate("Add from bin", () => {
      let track: Track | undefined;
      const ti = trackAtY(y);
      if (ti >= 0 && ti < app.project.tracks.length) track = app.project.tracks[ti];
      if (track?.locked) track = undefined;
      if (!track) {
        track = addTrack(
          app.project,
          (entry.kind[0]?.toUpperCase() ?? "") + entry.kind.slice(1) + " " + (app.project.tracks.length + 1),
          app.project.tracks.length,
        );
      }
      const clip = makeClip(type, start, start + dur);
      clip.source = entry.id;
      clip.inPoint = 0;
      clip.speed = 1;
      if ((type === "video" || type === "image") && entry.width > 0 && entry.height > 0) {
        const s = Math.min(app.project.width / entry.width, app.project.height / entry.height);
        clip.props.scale_x = s;
        clip.props.scale_y = s;
      }
      insertClip(track, clip);
      app.setSelection([clip.id]);
    });
  }

  // -- keyboard --
  canvas.addEventListener("keydown", (e) => {
    const tag = (e.target as HTMLElement | null)?.tagName;
    if (tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT") return;
    if (e.code === "Delete" || e.code === "Backspace") {
      e.preventDefault();
      deleteSelection();
    } else if ((e.code === "KeyB" || e.code === "KeyS") && !e.metaKey && !e.ctrlKey) {
      e.preventDefault();
      splitAtPlayhead();
    } else if ((e.metaKey || e.ctrlKey) && e.code === "KeyD") {
      e.preventDefault();
      duplicateSelection();
    }
  });

  function deleteSelection(): void {
    if (app.selection.length === 0) return;
    const ids = [...app.selection];
    app.mutate("Delete clips", () => {
      for (const t of app.project.tracks) {
        t.clips = t.clips.filter((c) => !ids.includes(c.id));
      }
    });
    app.setSelection([]);
  }

  function splitAtPlayhead(): void {
    const ids = [...app.selection];
    if (ids.length === 0) return;
    const ph = app.playhead;
    const result: number[] = [];
    app.mutate("Split clips", () => {
      for (const id of ids) {
        const out = splitClip(app.project, id, [ph]);
        for (const c of out) result.push(c.id);
      }
    });
    if (result.length) app.setSelection(result);
  }

  function duplicateSelection(): void {
    const ids = [...app.selection];
    if (ids.length === 0) return;
    const newIds: number[] = [];
    app.mutate("Duplicate clips", () => {
      for (const id of ids) {
        const f = findClip(app.project, id);
        if (!f) continue;
        const dur = clipDuration(f.clip);
        const copy: Clip = {
          ...f.clip,
          id: freshId(),
          start: f.clip.start + dur,
          end: f.clip.end + dur,
          props: { ...f.clip.props },
          keyframes: Object.fromEntries(
            Object.entries(f.clip.keyframes).map(([k, v]) => [
              k,
              { interp: v.interp, keys: v.keys.map((kk) => ({ ...kk })) },
            ]),
          ),
          fx: f.clip.fx ? { ...f.clip.fx, params: { ...f.clip.fx.params } } : undefined,
          fxChain: f.clip.fxChain?.map((s) => ({ ...s, params: { ...s.params } })),
        };
        insertClip(f.track, copy);
        newIds.push(copy.id);
      }
    });
    if (newIds.length) app.setSelection(newIds);
  }

  // -- event subscriptions --
  app.events.on("project:changed", draw);
  app.events.on("playhead", draw);
  app.events.on("selection", draw);
  app.events.on("media:analyzed", draw);

  resize();
  draw();
}
