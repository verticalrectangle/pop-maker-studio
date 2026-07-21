import { Emitter } from "./events";
import { History } from "./history";
import {
  deserializeProject, emptyProject, recomputeDuration, serializeProject,
  type Project,
} from "./project";
import type { MediaStore, MediaEntry } from "../media/store";

export interface AppEvents extends Record<string, unknown> {
  "project:changed": { structural: boolean };
  "playhead": number;
  "playing": boolean;
  "selection": number[]; // clip ids
  "media:added": MediaEntry;
  "media:analyzed": MediaEntry; // transcript/beats/etc updated
  "track:changed": number; // track id
  "toast": string;
}

/** Central application context — the single seam all subsystems share. */
export class App {
  project: Project = emptyProject();
  readonly history = new History(serializeProject);
  readonly events = new Emitter<AppEvents>();
  readonly media: MediaStore;

  playhead = 0;
  playing = false;
  selection: number[] = [];
  /** Loop brace; null = disarmed. */
  loop: { start: number; end: number } | null = null;

  constructor(media: MediaStore) {
    this.media = media;
  }

  /** Run a mutation as one undo step and notify listeners. */
  mutate<T>(label: string, fn: () => T, structural = true): T {
    const result = this.history.mutate(label, this.project, fn);
    recomputeDuration(this.project);
    this.events.emit("project:changed", { structural });
    return result;
  }

  undo(): void {
    const snapshot = this.history.undo(this.project);
    if (snapshot) this.restore(snapshot, "Undo");
  }

  redo(): void {
    const snapshot = this.history.redo(this.project);
    if (snapshot) this.restore(snapshot, "Redo");
  }

  private restore(snapshot: string, label: string): void {
    this.project = deserializeProject(snapshot);
    this.events.emit("project:changed", { structural: true });
    this.events.emit("toast", label);
  }

  newProject(): void {
    this.mutate("New project", () => {
      this.project = emptyProject();
      this.playhead = 0;
      this.selection = [];
      this.loop = null;
    });
  }

  setPlayhead(t: number): void {
    this.playhead = Math.max(0, Math.min(t, Math.max(this.project.duration, 0)));
    this.events.emit("playhead", this.playhead);
  }

  setPlaying(playing: boolean): void {
    if (this.playing === playing) return;
    this.playing = playing;
    this.events.emit("playing", playing);
  }

  setSelection(ids: number[]): void {
    this.selection = ids;
    this.events.emit("selection", ids);
  }

  saveToFile(): void {
    const blob = new Blob([serializeProject(this.project)], { type: "application/json" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = "project.pms";
    a.click();
    URL.revokeObjectURL(a.href);
  }

  async loadFromFile(file: File): Promise<void> {
    const text = await file.text();
    this.mutate("Load project", () => {
      this.project = deserializeProject(text);
      this.playhead = 0;
      this.selection = [];
    });
  }
}
