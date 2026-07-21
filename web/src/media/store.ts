/** Word-level transcript entry; timestamps are source-file seconds. */
export interface Word {
  word: string;
  start: number;
  end: number;
}

export type MediaKind = "video" | "audio" | "image";

export interface MediaEntry {
  id: string;
  file: File;
  kind: MediaKind;
  duration: number;
  width: number;
  height: number;
  fps: number;
  hasAudio: boolean;
  hasVideo: boolean;
  thumbnail?: ImageBitmap;
  /** Normalized waveform peaks (~1024 buckets, 0..1). */
  peaks?: Float32Array;
  transcript?: Word[];
  bpm?: number | null;
  beats?: number[];
  /** Per-second RMS for silence detection / audio cues. */
  rms?: Float32Array;
}

let counter = 0;

/** Registry of imported media files, keyed by id referenced from Clip.source. */
export class MediaStore {
  private entries = new Map<string, MediaEntry>();

  add(partial: Omit<MediaEntry, "id">): MediaEntry {
    const entry: MediaEntry = { ...partial, id: `m${++counter}` };
    this.entries.set(entry.id, entry);
    return entry;
  }

  get(id: string): MediaEntry | undefined {
    return this.entries.get(id);
  }

  list(): MediaEntry[] {
    return [...this.entries.values()];
  }

  remove(id: string): void {
    this.entries.delete(id);
  }
}
