import { App } from "../core/app";
import type { MediaEntry, MediaKind } from "../media/store";

const KIND_LABEL: Record<MediaKind, string> = {
  video: "VIDEO",
  audio: "AUDIO",
  image: "IMAGE",
};

function drawPlaceholder(ctx: CanvasRenderingContext2D, kind: MediaKind): void {
  ctx.fillStyle = "#262933";
  ctx.fillRect(0, 0, 82, 46);
  ctx.fillStyle = "#9aa0b0";
  ctx.font = "bold 10px Inter, system-ui, sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillText(KIND_LABEL[kind], 41, 23);
}

export function initBin(app: App, el: HTMLElement): void {
  el.innerHTML = "";

  const title = document.createElement("div");
  title.className = "panel-title";
  title.textContent = "Media Bin";
  const grid = document.createElement("div");
  grid.className = "bin-grid";
  el.append(title, grid);

  let selectedId: string | null = null;

  function render(): void {
    grid.innerHTML = "";
    const items = app.media.list();
    if (items.length === 0) {
      const empty = document.createElement("div");
      empty.style.color = "var(--text-dim)";
      empty.style.fontSize = "12px";
      empty.style.padding = "8px 0";
      empty.textContent = "No media imported yet. Use Import or drag files in.";
      grid.append(empty);
      return;
    }
    for (const entry of items) {
      const node = makeItem(entry);
      if (entry.id === selectedId) node.style.borderColor = "var(--accent)";
      grid.append(node);
    }
  }

  function makeItem(entry: MediaEntry): HTMLElement {
    const item = document.createElement("div");
    item.className = "bin-item";
    item.style.position = "relative";
    item.draggable = true;

    const c = document.createElement("canvas");
    c.width = 82;
    c.height = 46;
    const ctx = c.getContext("2d");
    if (ctx) {
      if (entry.thumbnail) {
        ctx.drawImage(entry.thumbnail, 0, 0, 82, 46);
      } else {
        drawPlaceholder(ctx, entry.kind);
      }
    }

    const label = document.createElement("div");
    label.className = "label";
    label.textContent = entry.file.name;

    const del = document.createElement("button");
    del.textContent = "✕";
    del.style.position = "absolute";
    del.style.top = "2px";
    del.style.right = "2px";
    del.style.width = "18px";
    del.style.height = "18px";
    del.style.padding = "0";
    del.style.fontSize = "10px";
    del.style.lineHeight = "1";
    del.style.borderRadius = "4px";
    del.title = "Remove from bin";
    del.addEventListener("click", (e) => {
      e.stopPropagation();
      app.media.remove(entry.id);
      if (selectedId === entry.id) selectedId = null;
      render();
    });

    item.addEventListener("click", () => {
      selectedId = entry.id;
      render();
    });

    item.addEventListener("dragstart", (e) => {
      e.dataTransfer?.setData("application/pms-media", entry.id);
      e.dataTransfer?.setData("text/plain", entry.file.name);
      e.dataTransfer!.effectAllowed = "copy";
    });

    item.append(c, label, del);
    return item;
  }

  app.events.on("media:added", render);
  app.events.on("media:analyzed", render);
  render();
}
