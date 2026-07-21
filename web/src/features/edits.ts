import type { App } from "../core/app";
import { findClip, splitClip } from "../core/project";

export interface CameraCut {
  /** Timeline time where this camera takes over. */
  time: number;
  trackId: number;
}

/**
 * Multicam: in each time window, keep only the selected camera track's clips.
 * All other camera-track clips overlapping the window are split at the window
 * bounds and their in-window segments deleted. Non-camera tracks untouched.
 */
export function applyMulticamCuts(app: App, cuts: CameraCut[], cameraTrackIds: number[]): void {
  const sorted = [...cuts].sort((a, b) => a.time - b.time);
  if (sorted.length === 0) return;
  app.mutate("Multicam cuts", () => {
    for (let i = 0; i < sorted.length; i++) {
      const winStart = sorted[i]!.time;
      const winEnd = i + 1 < sorted.length ? sorted[i + 1]!.time : Infinity;
      const keep = sorted[i]!.trackId;
      for (const trackId of cameraTrackIds) {
        if (trackId === keep) continue;
        const track = app.project.tracks.find((t) => t.id === trackId);
        if (!track) continue;
        // Split straddlers at window bounds, then delete in-window segments.
        for (const clip of [...track.clips]) {
          if (clip.end <= winStart || clip.start >= winEnd) continue;
          const cuts: number[] = [];
          if (clip.start < winStart) cuts.push(winStart);
          if (Number.isFinite(winEnd) && clip.end > winEnd) cuts.push(winEnd);
          const parts = cuts.length > 0 ? splitClip(app.project, clip.id, cuts) : [clip];
          for (const part of parts) {
            if (part.start >= winStart && part.start < winEnd) {
              track.clips = track.clips.filter((c) => c.id !== part.id);
            }
          }
        }
      }
    }
  });
}

/**
 * Remove silent segments of a clip using per-second RMS (from media analysis).
 * Splits the clip at each silence boundary and deletes the silent parts.
 * Returns the number of removed segments.
 */
export function removeSilence(
  app: App,
  clipId: number,
  threshold = 0.02,
  minDuration = 0.5,
): number {
  const found = findClip(app.project, clipId);
  if (!found?.clip.source) return 0;
  const entry = app.media.get(found.clip.source);
  if (!entry?.rms) return 0;
  const { clip } = found;
  const rms = entry.rms;

  const silent = (sourceTime: number): boolean => {
    const bucket = Math.floor(sourceTime);
    return bucket >= 0 && bucket < rms.length && rms[bucket]! < threshold;
  };

  // Collect silent [start,end) ranges in timeline time.
  const ranges: { start: number; end: number }[] = [];
  let runStart: number | null = null;
  for (let t = clip.start; t < clip.end; t += 0.25) {
    const sourceTime = clip.inPoint + (t - clip.start) * clip.speed;
    if (silent(sourceTime)) {
      runStart ??= t;
    } else if (runStart !== null) {
      if (t - runStart >= minDuration) ranges.push({ start: runStart, end: t });
      runStart = null;
    }
  }
  if (runStart !== null && clip.end - runStart >= minDuration) {
    ranges.push({ start: runStart, end: clip.end });
  }
  if (ranges.length === 0) return 0;

  app.mutate("Remove silence", () => {
    const boundaries = ranges.flatMap((r) => [r.start, r.end]);
    const parts = splitClip(app.project, clipId, boundaries);
    const track = found.track;
    for (const part of parts) {
      if (ranges.some((r) => part.start >= r.start - 1e-6 && part.end <= r.end + 1e-6)) {
        track.clips = track.clips.filter((c) => c.id !== part.id);
      }
    }
  });
  return ranges.length;
}

const DEFAULT_FILLERS = [
  "um", "uh", "uh-huh", "like", "you know", "kind of", "sort of",
  "basically", "literally", "actually",
];

/**
 * Cut filler words from a clip using its source transcript.
 * Words are source-file seconds; converted via inPoint/speed.
 */
export function cutFillerWords(app: App, clipId: number, words = DEFAULT_FILLERS): number {
  const found = findClip(app.project, clipId);
  if (!found?.clip.source) return 0;
  const entry = app.media.get(found.clip.source);
  if (!entry?.transcript) return 0;
  const { clip } = found;
  const lowered = words.map((w) => w.toLowerCase());

  const hits = entry.transcript.filter((w) =>
    lowered.includes(w.word.toLowerCase().replace(/[.,!?]/g, "")),
  ).map((w) => ({
    start: clip.start + (w.start - clip.inPoint) / clip.speed,
    end: clip.start + (w.end - clip.inPoint) / clip.speed,
  })).filter((r) => r.start >= clip.start && r.end <= clip.end && r.end > r.start);

  if (hits.length === 0) return 0;
  app.mutate("Cut filler words", () => {
    const boundaries = hits.flatMap((r) => [r.start, r.end]);
    const parts = splitClip(app.project, clipId, boundaries);
    for (const part of parts) {
      if (hits.some((r) => part.start >= r.start - 1e-6 && part.end <= r.end + 1e-6)) {
        found.track.clips = found.track.clips.filter((c) => c.id !== part.id);
      }
    }
  });
  return hits.length;
}

/**
 * Close the gaps left by deletions on one track: shift every clip starting at
 * or after `from` left by `bySeconds` (ripple).
 */
export function rippleLeft(app: App, trackId: number, from: number, bySeconds: number): void {
  app.mutate("Ripple", () => {
    const track = app.project.tracks.find((t) => t.id === trackId);
    if (!track) return;
    for (const clip of track.clips) {
      if (clip.start >= from - 1e-9) {
        clip.start -= bySeconds;
        clip.end -= bySeconds;
      }
    }
    track.clips.sort((a, b) => a.start - b.start);
  });
}
