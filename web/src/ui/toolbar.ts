import { App } from "../core/app";
import { importFiles } from "../media/import";
import { exportProject } from "../export/encoder";
import { runPipeline } from "../features/pipeline";

interface FormatOption {
  label: string;
  width: number;
  height: number;
}

const FORMATS: FormatOption[] = [
  { label: "1920×1080", width: 1920, height: 1080 },
  { label: "1080×1920", width: 1080, height: 1920 },
  { label: "1080×1080", width: 1080, height: 1080 },
];

function btn(label: string, onClick: () => void): HTMLButtonElement {
  const b = document.createElement("button");
  b.textContent = label;
  b.addEventListener("click", onClick);
  return b;
}

export function initToolbar(app: App, el: HTMLElement): void {
  el.innerHTML = "";

  const title = document.createElement("span");
  title.className = "title";
  title.textContent = "Pop Maker Studio";

  const fileInput = document.createElement("input");
  fileInput.type = "file";
  fileInput.multiple = true;
  fileInput.hidden = true;
  fileInput.addEventListener("change", () => {
    if (fileInput.files && fileInput.files.length) {
      void importFiles(app, [...fileInput.files]);
    }
    fileInput.value = "";
  });

  const loadInput = document.createElement("input");
  loadInput.type = "file";
  loadInput.accept = ".pms,application/json";
  loadInput.hidden = true;
  loadInput.addEventListener("change", () => {
    const f = loadInput.files?.[0];
    if (f) void app.loadFromFile(f);
    loadInput.value = "";
  });

  const importBtn = btn("Import", () => fileInput.click());
  const newBtn = btn("New", () => app.newProject());
  const saveBtn = btn("Save", () => app.saveToFile());
  const loadBtn = btn("Load", () => loadInput.click());

  const spacer = document.createElement("span");
  spacer.className = "spacer";

  const fmtSelect = document.createElement("select");
  for (const f of FORMATS) {
    const opt = document.createElement("option");
    opt.value = f.label;
    opt.textContent = f.label;
    fmtSelect.add(opt);
  }
  fmtSelect.value = `${app.project.width}×${app.project.height}`;
  fmtSelect.addEventListener("change", () => {
    const f = FORMATS.find((o) => o.label === fmtSelect.value);
    if (!f) return;
    app.mutate("Set format", () => {
      app.project.width = f.width;
      app.project.height = f.height;
    });
  });

  const lyricsBtn = btn("Lyrics", () => void runLyrics());
  const exportBtn = btn("Export MP4", () => void runExport());
  exportBtn.classList.add("primary");

  el.append(title, importBtn, newBtn, saveBtn, loadBtn, lyricsBtn, spacer, fmtSelect, exportBtn, fileInput, loadInput);

  app.events.on("project:changed", () => {
    fmtSelect.value = `${app.project.width}×${app.project.height}`;
  });

  async function runLyrics(): Promise<void> {
    lyricsBtn.disabled = true;
    const originalLabel = lyricsBtn.textContent;
    try {
      await runPipeline(app, "karaoke", (stage, detail) => {
        lyricsBtn.textContent = stage === "done" ? originalLabel : detail;
      });
    } catch (err) {
      app.events.emit("toast", `Lyrics failed: ${String(err)}`);
    } finally {
      lyricsBtn.disabled = false;
      lyricsBtn.textContent = originalLabel;
    }
  }

  async function runExport(): Promise<void> {
    exportBtn.disabled = true;
    const originalLabel = exportBtn.textContent;
    try {
      await exportProject(app, (p) => {
        exportBtn.textContent = `Export ${Math.round(p * 100)}%`;
      });
      app.events.emit("toast", "Export complete");
    } catch (err) {
      app.events.emit("toast", `Export failed: ${String(err)}`);
    } finally {
      exportBtn.disabled = false;
      exportBtn.textContent = originalLabel;
    }
  }
}
