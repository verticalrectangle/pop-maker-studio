// pms:rhythm — audio helpers over pms.audio, with a visual lead.
//
// Semantically identical to ~/Projects/seen-and-not-seen/src/rhythm.ts:
// same LEAD = 0.012 applied the same way in lastIndex-based lookups, pulse
// summing hits with t+LEAD-hit.t over 6*decay, lastHit, eventPulse, envAt
// rounding (t+LEAD)*fps and clamping, beatInfo returning
// {beat, bar, phase, frac} exactly as there, hash bit-identical.
//
// Plus `progress` (the laser.ts idea generalised): music-driven progress —
// cumulative hit energy + trickle, normalised to reach 1 at t1 — and the
// small `clamp01` / `easeInOutCubic` helpers scenes need with it.

export const LEAD = 0.012;

/** Index of the last element <= t, or -1. */
export function lastIndex(times, t) {
  let lo = 0;
  let hi = times.length - 1;
  let ans = -1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (times[mid] <= t) {
      ans = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return ans;
}

function hitTimes(audio, kind) {
  return (audio.hits[kind] || []).map((h) => h.t);
}

/** Sum of exponentially decaying impulses from hits at or before t (strength-weighted). */
export function pulse(audio, kind, t, decay, minStrength = 0) {
  const hits = audio.hits[kind] || [];
  const ts = hitTimes(audio, kind);
  let i = lastIndex(ts, t + LEAD);
  let sum = 0;
  for (; i >= 0; i--) {
    const dt = t + LEAD - hits[i].t;
    if (dt > decay * 6) break;
    if (hits[i].s >= minStrength) sum += hits[i].s * Math.exp(-dt / decay);
  }
  return sum;
}

/** Most recent hit of a kind (at or before t), with its age in seconds. */
export function lastHit(audio, kind, t, minStrength = 0) {
  const hits = audio.hits[kind] || [];
  const ts = hitTimes(audio, kind);
  for (let i = lastIndex(ts, t + LEAD); i >= 0; i--) {
    if (hits[i].s >= minStrength) return { hit: hits[i], age: t + LEAD - hits[i].t, index: i };
  }
  return null;
}

/** Decaying impulse from a list of event times (stage changes, downbeats, words). */
export function eventPulse(times, t, decay) {
  const i = lastIndex(times, t + LEAD);
  return i < 0 ? 0 : Math.exp(-(t + LEAD - times[i]) / decay);
}

export function envAt(audio, kind, t) {
  const values = audio.env[kind] || audio.env.mix || [];
  const f = Math.min(values.length - 1, Math.max(0, Math.round((t + LEAD) * audio.fps)));
  return values[f];
}

export function beatInfo(audio, t) {
  const b = lastIndex(audio.beats, t + LEAD);
  const bar = lastIndex(audio.downbeats, t + LEAD);
  const next = audio.beats[b + 1] ?? audio.beats[b] + 60 / audio.bpm;
  const frac = b < 0 ? 0 : (t + LEAD - audio.beats[b]) / (next - audio.beats[b]);
  const barStart = bar < 0 ? -1 : lastIndex(audio.beats, audio.downbeats[bar] + 1e-3);
  return { beat: b, bar, phase: bar < 0 ? (b + 4) % 4 : (b - barStart + 4) % 4, frac };
}

/** Deterministic hash -> [0, 1). Bit-identical to SCRIPT_API.md §7. */
export function hash(a, b = 0, c = 0) {
  let h = Math.imul(a | 0, 0x27d4eb2d) ^ Math.imul(b | 0, 0x165667b1) ^ Math.imul(c | 0, 0x9e3779b1);
  h = Math.imul(h ^ (h >>> 15), 0x85ebca6b);
  h = Math.imul(h ^ (h >>> 13), 0xc2b2ae35);
  h ^= h >>> 16;
  return (h >>> 0) / 4294967296;
}

export function clamp01(x) {
  return x < 0 ? 0 : x > 1 ? 1 : x;
}

export function easeInOutCubic(x) {
  x = clamp01(x);
  return x < 0.5 ? 4 * x * x * x : 1 - Math.pow(-2 * x + 2, 3) / 2;
}

const PROGRESS_DEFAULT_WEIGHTS = [
  ['kick', 1.6],
  ['snare', 1.2],
  ['bass', 0.6],
  ['other', 0.5],
  ['hat', 0.35],
  ['vocal', 0.3],
];
const PROGRESS_SPREAD = [0.55, 0.3, 0.15];

/**
 * Music-driven progress over [t0, t1]: cumulative hit energy (per-kind
 * weights × strength, spread over neighbouring frames like the laser burn
 * schedule) plus a constant trickle, normalised so progress(t1) = 1.
 * `weights` = [[kind, w], ...]; `opts` = {trickle, fps, wordBursts} where
 * wordBursts maps lowercased words to extra energy deposited at word.t0.
 */
export function progress(audio, weights, t0, t1, opts = {}) {
  const fps = opts.fps || audio.fps || 60;
  const trickle = opts.trickle !== undefined ? opts.trickle : 0.35;
  const w = weights || PROGRESS_DEFAULT_WEIGHTS;
  const f0 = Math.round(t0 * fps);
  const f1 = Math.round(t1 * fps);
  const n = Math.max(1, f1 - f0 + 1);
  const rate = new Float64Array(n).fill(trickle);
  const deposit = (t, amount) => {
    const f = Math.round(t * fps) - f0;
    PROGRESS_SPREAD.forEach((k, j) => {
      if (f + j >= 0 && f + j < rate.length) rate[f + j] += amount * k;
    });
  };
  for (const [kind, weight] of w) {
    for (const h of audio.hits[kind] || []) {
      if (h.t >= t0 && h.t <= t1) deposit(h.t, weight * h.s);
    }
  }
  if (opts.wordBursts && audio.words) {
    for (const word of audio.words) {
      const burst = opts.wordBursts[String(word.w || '').toLowerCase()];
      if (burst && word.t0 >= t0 && word.t0 <= t1) deposit(word.t0, burst);
    }
  }
  let total = 0;
  for (let i = 0; i < n; i++) total += rate[i];
  return (t) => {
    if (t <= t0) return 0;
    if (t >= t1) return 1;
    const fi = Math.round(t * fps) - f0;
    let cum = 0;
    for (let i = 0; i <= fi && i < n; i++) cum += rate[i];
    return clamp01(cum / total);
  };
}
