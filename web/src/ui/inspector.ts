import { App } from "../core/app";
import { findClip, type Clip, type TextStyle } from "../core/project";
import { removeBackground } from "../ml/matting";
import { cutFillerWords, removeSilence } from "../features/edits";

const SHAPE_PRESETS = ["circle", "square", "triangle", "star", "heart", "diamond", "hexagon"];

function row(label: string, control: HTMLElement): HTMLDivElement {
  const r = document.createElement("div");
  r.className = "row";
  const l = document.createElement("label");
  l.textContent = label;
  r.append(l, control);
  return r;
}

function numInput(value: number, onChange: (v: number) => void): HTMLInputElement {
  const input = document.createElement("input");
  input.type = "number";
  input.step = "0.1";
  input.value = String(Number.isFinite(value) ? value : 0);
  input.addEventListener("change", () => {
    const v = parseFloat(input.value);
    if (Number.isFinite(v)) onChange(v);
  });
  return input;
}

function textInput(value: string, onChange: (v: string) => void): HTMLInputElement {
  const input = document.createElement("input");
  input.type = "text";
  input.value = value;
  input.addEventListener("change", () => onChange(input.value));
  return input;
}

function rangeInput(
  value: number, min: number, max: number, step: number,
  onChange: (v: number) => void,
): HTMLInputElement {
  const input = document.createElement("input");
  input.type = "range";
  input.min = String(min);
  input.max = String(max);
  input.step = String(step);
  input.value = String(value);
  input.addEventListener("input", () => onChange(parseFloat(input.value)));
  return input;
}

function selectInput(options: string[], value: string, onChange: (v: string) => void): HTMLSelectElement {
  const sel = document.createElement("select");
  for (const o of options) {
    const opt = document.createElement("option");
    opt.value = o;
    opt.textContent = o;
    sel.add(opt);
  }
  sel.value = value;
  sel.addEventListener("change", () => onChange(sel.value));
  return sel;
}

function rgbaToHex(c: [number, number, number, number]): string {
  const to = (v: number) => Math.round(Math.max(0, Math.min(1, v)) * 255).toString(16).padStart(2, "0");
  return `#${to(c[0])}${to(c[1])}${to(c[2])}`;
}

function hexToRgb(hex: string): [number, number, number] {
  const m = /^#?([0-9a-f]{6})$/i.exec(hex.trim());
  if (!m) return [1, 1, 1];
  const n = parseInt(m[1]!, 16);
  return [((n >> 16) & 255) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255];
}

function defaultTextStyle(): TextStyle {
  return {
    fontFamily: "Inter, sans-serif",
    fontSize: 0.08,
    color: [1, 1, 1, 1],
    bold: false,
    italic: false,
    align: "center",
    strokeWidth: 0,
    letterSpacing: 0,
    uppercase: false,
  };
}

interface Field {
  el: HTMLInputElement | HTMLSelectElement;
  get: (clip: Clip) => string | number;
}

export function initInspector(app: App, el: HTMLElement): void {
  let fields: Field[] = [];
  let currentClipId: number | null = null;

  /** Track an input + its getter so sync() can update it in-place. */
  function track<T extends HTMLInputElement | HTMLSelectElement>(input: T, get: (c: Clip) => string | number): T {
    fields.push({ el: input, get });
    return input;
  }

  function build(): void {
    el.innerHTML = "";
    fields = [];
    currentClipId = null;

    const sel = app.selection;
    if (sel.length === 0) {
      const note = document.createElement("div");
      note.style.color = "var(--text-dim)";
      note.style.fontSize = "12px";
      note.style.padding = "8px 0";
      note.textContent = "Select a clip to edit its properties.";
      el.append(note);
      return;
    }
    if (sel.length > 1) {
      const note = document.createElement("div");
      note.style.color = "var(--text-dim)";
      note.style.fontSize = "12px";
      note.style.padding = "8px 0";
      note.textContent = `${sel.length} clips selected. Edit one at a time.`;
      el.append(note);
      return;
    }
    const found = findClip(app.project, sel[0]!);
    if (!found) {
      const note = document.createElement("div");
      note.style.color = "var(--text-dim)";
      note.textContent = "Clip not found.";
      el.append(note);
      return;
    }
    const { clip, track: trackName } = found;
    currentClipId = clip.id;

    const heading = document.createElement("h3");
    heading.textContent = `${clip.type.toUpperCase()} · ${trackName.name}`;
    el.append(heading);

    const mutate = (fn: (c: Clip) => void): void => {
      app.mutate("Edit clip", () => fn(clip));
    };

    // --- common rows ---
    el.append(row("Name", track(textInput(clip.text ?? clipName(clip), (v) => mutate((c) => { c.text = v; })), (c) => c.text ?? clipName(c))));
    el.append(row("Start", track(numInput(clip.start, (v) => mutate((c) => { c.start = v; })), (c) => c.start)));
    el.append(row("End", track(numInput(clip.end, (v) => mutate((c) => { c.end = v; })), (c) => c.end)));
    el.append(row("Speed", track(numInput(clip.speed, (v) => mutate((c) => { c.speed = Math.max(0.01, v); })), (c) => c.speed)));
    el.append(row("Opacity", track(rangeInput(clip.props.opacity ?? 1, 0, 1, 0.01, (v) => mutate((c) => { c.props.opacity = v; })), (c) => c.props.opacity ?? 1)));
    el.append(row("Volume", track(rangeInput(clip.props.volume ?? 1, 0, 1, 0.01, (v) => mutate((c) => { c.props.volume = v; })), (c) => c.props.volume ?? 1)));
    el.append(row("Pan", track(rangeInput(clip.props.pan ?? 0, -1, 1, 0.01, (v) => mutate((c) => { c.props.pan = v; })), (c) => c.props.pan ?? 0)));
    el.append(row("Pos X", track(rangeInput(clip.props.pos_x ?? 0.5, 0, 1, 0.001, (v) => mutate((c) => { c.props.pos_x = v; })), (c) => c.props.pos_x ?? 0.5)));
    el.append(row("Pos Y", track(rangeInput(clip.props.pos_y ?? 0.5, 0, 1, 0.001, (v) => mutate((c) => { c.props.pos_y = v; })), (c) => c.props.pos_y ?? 0.5)));
    el.append(row("Scale X", track(rangeInput(clip.props.scale_x ?? 1, 0, 4, 0.01, (v) => mutate((c) => { c.props.scale_x = v; })), (c) => c.props.scale_x ?? 1)));
    el.append(row("Scale Y", track(rangeInput(clip.props.scale_y ?? 1, 0, 4, 0.01, (v) => mutate((c) => { c.props.scale_y = v; })), (c) => c.props.scale_y ?? 1)));
    el.append(row("Rotation", track(rangeInput(clip.props.rotation ?? 0, 0, 360, 0.1, (v) => mutate((c) => { c.props.rotation = v; })), (c) => c.props.rotation ?? 0)));

    // --- type-specific ---
    if (clip.type === "text" || clip.type === "lyric") {
      const ts = clip.textStyle ?? defaultTextStyle();
      const tsHeading = document.createElement("h3");
      tsHeading.textContent = "Text";
      el.append(tsHeading);
      el.append(row("Content", track(textInput(clip.text ?? "", (v) => mutate((c) => { c.text = v; })), (c) => c.text ?? "")));
      el.append(
        row(
          "Font size",
          track(numInput(ts.fontSize, (v) =>
            mutate((c) => {
              if (!c.textStyle) c.textStyle = defaultTextStyle();
              c.textStyle.fontSize = Math.max(0.001, v);
            }),
          ), (c) => c.textStyle?.fontSize ?? 0.08),
        ),
      );
      const colorInput = track(
        (() => {
          const ci = document.createElement("input");
          ci.type = "color";
          ci.value = rgbaToHex(ts.color);
          ci.addEventListener("input", () =>
            mutate((c) => {
              if (!c.textStyle) c.textStyle = defaultTextStyle();
              const [r, g, b] = hexToRgb(ci.value);
              c.textStyle.color = [r, g, b, c.textStyle.color[3] ?? 1];
            }),
          );
          return ci;
        })(),
        (c) => rgbaToHex(c.textStyle?.color ?? defaultTextStyle().color),
      );
      el.append(row("Color", colorInput));
    }

    if (clip.type === "fx" || clip.type === "bodyfx") {
      const fxHeading = document.createElement("h3");
      fxHeading.textContent = "Effect";
      el.append(fxHeading);
      if (clip.fx) {
        el.append(row("FX type", track(textInput(clip.fx.fxType, (v) => mutate((c) => { if (c.fx) c.fx.fxType = v; })), (c) => c.fx?.fxType ?? "")));
        el.append(row("Amount", track(rangeInput(clip.fx.amount, 0, 1, 0.01, (v) => mutate((c) => { if (c.fx) c.fx.amount = v; })), (c) => c.fx?.amount ?? 1)));
      } else if (clip.fxChain && clip.fxChain.length) {
        const note = document.createElement("div");
        note.style.color = "var(--text-dim)";
        note.style.fontSize = "12px";
        note.textContent = `${clip.fxChain.length} effects in chain.`;
        el.append(note);
      } else {
        const note = document.createElement("div");
        note.style.color = "var(--text-dim)";
        note.style.fontSize = "12px";
        note.textContent = "No effect configured.";
        el.append(note);
      }
    }

    if (clip.type === "video" || clip.type === "camera") {
      const aiHeading = document.createElement("h3");
      aiHeading.textContent = "AI Tools";
      el.append(aiHeading);

      const bgBtn = document.createElement("button");
      bgBtn.textContent = "Remove background";
      bgBtn.addEventListener("click", () => {
        const entry = clip.source ? app.media.get(clip.source) : undefined;
        if (!entry) return;
        bgBtn.disabled = true;
        void removeBackground(app, entry, (p) => {
          bgBtn.textContent = `BG ${Math.round(p * 100)}%`;
        }).then(() => {
          app.events.emit("toast", "Background masks ready");
        }).catch((err: unknown) => {
          app.events.emit("toast", `BG removal failed: ${String(err)}`);
        }).finally(() => {
          bgBtn.disabled = false;
          bgBtn.textContent = "Remove background";
        });
      });

      const silenceBtn = document.createElement("button");
      silenceBtn.textContent = "Remove silence";
      silenceBtn.addEventListener("click", () => {
        const n = removeSilence(app, clip.id);
        app.events.emit("toast", n > 0 ? `Removed ${n} silent segments` : "No silence found (run beat analysis first)");
      });

      const fillerBtn = document.createElement("button");
      fillerBtn.textContent = "Cut filler words";
      fillerBtn.addEventListener("click", () => {
        const n = cutFillerWords(app, clip.id);
        app.events.emit("toast", n > 0 ? `Cut ${n} filler words` : "No fillers found (transcribe first)");
      });

      const btnRow = document.createElement("div");
      btnRow.className = "row";
      btnRow.append(bgBtn, silenceBtn, fillerBtn);
      el.append(btnRow);
    }

    if (clip.type === "shape") {
      const shHeading = document.createElement("h3");
      shHeading.textContent = "Shape";
      el.append(shHeading);
      const preset = clip.shape?.preset ?? "circle";
      el.append(
        row(
          "Preset",
          track(selectInput(SHAPE_PRESETS, preset, (v) =>
            mutate((c) => {
              if (!c.shape) c.shape = { preset: v, params: [], closed: true, strokeWidth: 0.008, glowRadius: 0 };
              else c.shape.preset = v;
            }),
          ), (c) => c.shape?.preset ?? "circle"),
        ),
      );
    }
  }

  /** Update field values in-place without rebuilding DOM. */
  function sync(): void {
    if (currentClipId === null) return;
    const found = findClip(app.project, currentClipId);
    if (!found) { build(); return; }
    const { clip } = found;
    for (const f of fields) {
      if (f.el === document.activeElement) continue;
      f.el.value = String(f.get(clip));
    }
  }

  // Avoid clobbering inputs the user is actively editing (range drag / text focus).
  function shouldRebuild(): boolean {
    const ae = document.activeElement;
    return !ae || !el.contains(ae);
  }

  app.events.on("selection", build);
  app.events.on("project:changed", (payload) => {
    if (payload.structural) { if (shouldRebuild()) build(); }
    else sync();
  });
  build();
}

function clipName(clip: Clip): string {
  if (clip.text && clip.text.trim()) return clip.text;
  return clip.type;
}
