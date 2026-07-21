import { App } from "../core/app";

function fmtTime(s: number): string {
  if (!isFinite(s) || s < 0) s = 0;
  const m = Math.floor(s / 60);
  const sec = s - m * 60;
  return `${m}:${sec.toFixed(1).padStart(4, "0")}`;
}

function btn(label: string, onClick: () => void): HTMLButtonElement {
  const b = document.createElement("button");
  b.textContent = label;
  b.addEventListener("click", onClick);
  return b;
}

export function initTransport(app: App, el: HTMLElement): void {
  el.innerHTML = "";

  const skipStart = btn("⏮", () => app.setPlayhead(0));
  const playBtn = btn("▶", () => app.setPlaying(!app.playing));
  const skipEnd = btn("⏭", () => app.setPlayhead(app.project.duration));
  const loopBtn = btn("🔁", () => {
    if (app.loop) {
      app.loop = null;
    } else {
      const end = app.project.duration > 0 ? app.project.duration : 1;
      app.loop = { start: 0, end };
    }
    update();
  });
  const time = document.createElement("span");
  time.className = "time";

  el.append(skipStart, playBtn, skipEnd, loopBtn, time);

  function update(): void {
    playBtn.textContent = app.playing ? "⏸" : "▶";
    playBtn.classList.toggle("active", app.playing);
    loopBtn.classList.toggle("active", !!app.loop);
    time.textContent = `${fmtTime(app.playhead)} / ${fmtTime(app.project.duration)}`;
  }

  app.events.on("playing", update);
  app.events.on("playhead", update);
  app.events.on("project:changed", update);
  update();
}
