import type { Project } from "./project";

/**
 * Snapshot-based undo. Projects are small (KBs of JSON); correctness beats
 * cleverness here. Mirrors the desktop's batch model: mutations inside
 * beginBatch/endBatch collapse into one undo step.
 */
export class History {
  private undoStack: { label: string; snapshot: string }[] = [];
  private redoStack: { label: string; snapshot: string }[] = [];
  private batchDepth = 0;
  private pending: { label: string; snapshot: string } | null = null;
  private limit = 200;

  constructor(private serialize: (p: Project) => string) {}

  beginBatch(label: string, project: Project): void {
    if (this.batchDepth === 0) this.pending = { label, snapshot: this.serialize(project) };
    this.batchDepth++;
  }

  /** Every mutating operation wraps itself: history.mutate("label", project, fn). */
  mutate<T>(label: string, project: Project, fn: () => T): T {
    this.beginBatch(label, project);
    try {
      return fn();
    } finally {
      this.endBatch();
    }
  }

  endBatch(): void {
    this.batchDepth--;
    if (this.batchDepth === 0 && this.pending) {
      this.undoStack.push(this.pending);
      if (this.undoStack.length > this.limit) this.undoStack.shift();
      this.redoStack = [];
      this.pending = null;
    }
  }

  canUndo(): boolean { return this.undoStack.length > 0; }
  canRedo(): boolean { return this.redoStack.length > 0; }
  undoLabel(): string | undefined { return this.undoStack[this.undoStack.length - 1]?.label; }

  /** Returns the snapshot to restore, pushing current state to the redo stack. */
  undo(current: Project): string | undefined {
    const entry = this.undoStack.pop();
    if (!entry) return undefined;
    this.redoStack.push({ label: entry.label, snapshot: this.serialize(current) });
    return entry.snapshot;
  }

  redo(current: Project): string | undefined {
    const entry = this.redoStack.pop();
    if (!entry) return undefined;
    this.undoStack.push({ label: entry.label, snapshot: this.serialize(current) });
    return entry.snapshot;
  }
}
