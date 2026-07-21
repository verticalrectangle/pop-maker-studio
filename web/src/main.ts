import { App } from "./core/app";
import { MediaStore } from "./media/store";
import { initToolbar } from "./ui/toolbar";
import { initTimeline } from "./ui/timeline";
import { initCanvasWysiwyg } from "./ui/canvasWysiwyg";
import { initTransport } from "./ui/transport";
import { initBin } from "./ui/bin";
import { initInspector } from "./ui/inspector";
import { Compositor } from "./render/compositor";
import { AudioEngine } from "./media/audio";
import { importFiles } from "./media/import";
import { DecoderManager, createFrameProvider } from "./media/decoder";
import { createMaskProvider } from "./ml/matting";
import { addTrack, insertClip, makeClip } from "./core/project";

const app = new App(new MediaStore());

const previewCanvas = document.getElementById("preview") as HTMLCanvasElement;
const timelineCanvas = document.getElementById("timeline") as HTMLCanvasElement;
const statusbar = document.getElementById("statusbar") as HTMLElement;

const compositor = new Compositor(previewCanvas, app);
const audio = new AudioEngine(app);
const decoders = new DecoderManager();
compositor.setFrameProvider(createFrameProvider(decoders, app));
compositor.setMaskProvider(createMaskProvider(app));

initToolbar(app, document.getElementById("toolbar") as HTMLElement);
initTransport(app, document.getElementById("transport") as HTMLElement);
initBin(app, document.getElementById("bin") as HTMLElement);
initInspector(app, document.getElementById("inspector") as HTMLElement);
initTimeline(app, timelineCanvas);
initCanvasWysiwyg(app, previewCanvas);

// Drag-and-drop import anywhere on the app.
window.addEventListener("dragover", (e) => e.preventDefault());
window.addEventListener("drop", (e) => {
  e.preventDefault();
  if (e.dataTransfer?.files.length) void importFiles(app, [...e.dataTransfer.files]);
});

// Status line + toasts.
const statusProject = document.createElement("span");
const statusToast = document.createElement("span");
statusToast.className = "toast";
const statusGpu = document.createElement("span");
statusbar.append(statusProject, statusToast, statusGpu);
app.events.on("toast", (msg) => {
  statusToast.textContent = msg;
  setTimeout(() => { if (statusToast.textContent === msg) statusToast.textContent = ""; }, 3000);
});
void navigator.gpu?.requestAdapter().then((adapter) => {
  statusGpu.textContent = adapter ? "WebGPU: on" : "WebGPU: unavailable (WASM fallback)";
});

app.events.on("project:changed", () => {
  statusProject.textContent =
    `${app.project.tracks.length} tracks · ${app.project.duration.toFixed(1)}s · ` +
    `${app.project.width}×${app.project.height}@${app.project.fps}`;
});

// Main loop: advance playhead while playing, render preview, repaint timeline.
let lastTs = performance.now();
function frame(ts: number): void {
  const dt = (ts - lastTs) / 1000;
  lastTs = ts;
  if (app.playing) {
    let t = app.playhead + dt;
    if (app.loop && t >= app.loop.end) t = app.loop.start;
    if (t >= app.project.duration && app.project.duration > 0) {
      app.setPlaying(false);
      t = app.project.duration;
    }
    app.setPlayhead(t);
  }
  compositor.renderAt(app.playhead);
  audio.sync(app.playhead, app.playing);
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);

// Debug/test handle (handy in devtools; harmless in production).
(window as unknown as { __pms: unknown }).__pms = {
  app, importFiles,
  model: { addTrack, makeClip, insertClip },
};

// Keyboard shortcuts.
window.addEventListener("keydown", (e) => {
  const tag = (e.target as HTMLElement).tagName;
  if (tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT") return;
  if (e.code === "Space") { e.preventDefault(); app.setPlaying(!app.playing); }
  else if ((e.metaKey || e.ctrlKey) && e.code === "KeyZ" && !e.shiftKey) app.undo();
  else if ((e.metaKey || e.ctrlKey) && (e.code === "KeyY" || (e.code === "KeyZ" && e.shiftKey))) app.redo();
});
