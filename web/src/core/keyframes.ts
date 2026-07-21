/** Keyframe animation: scalar tracks with easing, matching the desktop engine. */
export type Interp = "linear" | "ease_in" | "ease_out" | "ease_both" | "hold";

export interface Keyframe {
  /** Seconds relative to clip start. */
  t: number;
  v: number;
}

export interface KeyTrack {
  keys: Keyframe[]; // kept sorted by t
  interp: Interp;
}

function ease(u: number, interp: Interp): number {
  switch (interp) {
    case "hold": return 0;
    case "ease_in": return u * u;
    case "ease_out": return 1 - (1 - u) * (1 - u);
    case "ease_both": return u * u * (3 - 2 * u);
    default: return u;
  }
}

export function sampleKeyTrack(track: KeyTrack, t: number, fallback: number): number {
  const keys = track.keys;
  if (keys.length === 0) return fallback;
  const first = keys[0]!;
  if (t <= first.t) return first.v;
  const last = keys[keys.length - 1]!;
  if (t >= last.t) return last.v;
  for (let i = 0; i < keys.length - 1; i++) {
    const a = keys[i]!;
    const b = keys[i + 1]!;
    if (t >= a.t && t <= b.t) {
      const span = b.t - a.t;
      const u = span > 0 ? (t - a.t) / span : 0;
      return a.v + (b.v - a.v) * ease(u, track.interp);
    }
  }
  return last.v;
}

/** Animatable scalar properties. */
export const ANIMATABLE = [
  "pos_x", "pos_y", "scale_x", "scale_y", "rotation", "opacity",
  "volume", "pan", "amount",
] as const;
export type AnimatableProp = (typeof ANIMATABLE)[number];
