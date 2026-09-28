// pms:rhythm — audio helpers over pms.audio (timeline seconds), with a visual
// lead: every lookup samples at t + LEAD so motion lands on the transient the
// viewer hears, not a frame after it (docs/SCRIPT_API.md §6).
//
// `progress` is music-driven progress: cumulative hit energy + a constant
// trickle, normalised to reach 1 at t1.

export const LEAD = 0.012;

function audio() {
  const a = pms.audio;
  if (!a) throw new Error('pms:rhythm needs an audio analysis (analyze_audio or load_audio_analysis)');
  return a;
}

/** Index of the last element <= t in ascending `times`, or -1. */
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

// Hit-time arrays per analysis object (pms.audio keeps its identity until the
// analysis or the audio clip offset changes).
const timesByAudio = new WeakMap();

function hitsOf(a, kind) {
  const hits = a.hits[kind];
  if (!hits) throw new Error(`unknown hit kind '${kind}' (have: ${Object.keys(a.hits).join(', ')})`);
  return hits;
}

function hitTimes(a, kind) {
  let byKind = timesByAudio.get(a);
  if (!byKind) {
    byKind = {};
    timesByAudio.set(a, byKind);
  }
  return (byKind[kind] ??= hitsOf(a, kind).map((h) => h.t));
}

/** Sum of exponentially decaying impulses (strength-weighted) from hits at or before t. */
export function pulse(kind, t, decay, minStrength = 0) {
  const a = audio();
  const hits = hitsOf(a, kind);
  let sum = 0;
  for (let i = lastIndex(hitTimes(a, kind), t + LEAD); i >= 0; i--) {
    const dt = t + LEAD - hits[i].t;
    if (dt > decay * 6) break;
    if (hits[i].s >= minStrength) sum += hits[i].s * Math.exp(-dt / decay);
  }
  return sum;
}

/** Most recent hit of a kind at or before t: {hit, age, index}, or null. */
export function lastHit(kind, t, minStrength = 0) {
  const a = audio();
  const hits = hitsOf(a, kind);
  for (let i = lastIndex(hitTimes(a, kind), t + LEAD); i >= 0; i--) {
    if (hits[i].s >= minStrength) return {hit: hits[i], age: t + LEAD - hits[i].t, index: i};
  }
  return null;
}

/** Decaying impulse from a list of event times (stage changes, downbeats, words). */
export function eventPulse(times, t, decay) {
  const i = lastIndex(times, t + LEAD);
  return i < 0 ? 0 : Math.exp(-(t + LEAD - times[i]) / decay);
}

/** Sample a per-frame envelope (pms.audio.env.<kind>, index 0 = timeline 0). */
export function envAt(values, t) {
  if (!values.length) return 0;
  const f = Math.min(values.length - 1, Math.max(0, Math.round((t + LEAD) * audio().fps)));
  return values[f];
}

/** {beat, bar, phase (0..3 within the bar), frac (0..1 within the beat)} at t. */
export function beatInfo(t) {
  const a = audio();
  const b = lastIndex(a.beats, t + LEAD);
  const bar = lastIndex(a.downbeats, t + LEAD);
  const next = a.beats[b + 1] ?? a.beats[b] + 60 / a.bpm;
  const frac = b < 0 ? 0 : (t + LEAD - a.beats[b]) / (next - a.beats[b]);
  const barStart = bar < 0 ? -1 : lastIndex(a.beats, a.downbeats[bar] + 1e-3);
  return {beat: b, bar, phase: bar < 0 ? (b + 4) % 4 : (b - barStart + 4) % 4, frac};
}

/** Deterministic hash -> [0, 1). Bit-identical to SCRIPT_API.md §7. */
export function hash(a, b = 0, c = 0) {
  let h = Math.imul(a | 0, 0x27d4eb2d) ^ Math.imul(b | 0, 0x165667b1) ^ Math.imul(c | 0, 0x9e3779b1);
  h = Math.imul(h ^ (h >>> 15), 0x85ebca6b);
  h = Math.imul(h ^ (h >>> 13), 0xc2b2ae35);
  h ^= h >>> 16;
  return (h >>> 0) / 4294967296;
}

export const clamp01 = (x) => (x < 0 ? 0 : x > 1 ? 1 : x);

export const easeInOutCubic = (x) => (x < 0.5 ? 4 * x * x * x : 1 - (-2 * x + 2) ** 3 / 2);

const PROGRESS_WEIGHTS = [
  ['kick', 1.6],
  ['snare', 1.2],
  ['bass', 0.6],
  ['other', 0.5],
  ['hat', 0.35],
  ['vocal', 0.3],
];
const PROGRESS_SPREAD = [0.55, 0.3, 0.15];

/**
 * Music-driven progress over [t0, t1] → (t) => 0..1. Energy per frame = trickle
 * + Σ weight × hit strength (spread over 3 frames); progress is the cumulative
 * energy normalised to reach 1 at t1. `weights` = [[kind, w], …] (null = the
 * defaults above); `opts` = {trickle = 0.35, wordBursts: {word: energy}} where
 * word bursts deposit at the word's t0. Build once (setup / memo), call per frame.
 */
export function progress(weights, t0, t1, opts = {}) {
  const a = audio();
  const fps = a.fps;
  const trickle = opts.trickle ?? 0.35;
  const f0 = Math.round(t0 * fps);
  const n = Math.max(1, Math.round(t1 * fps) - f0 + 1);
  const rate = new Float64Array(n).fill(trickle);
  const deposit = (t, amount) => {
    const f = Math.round(t * fps) - f0;
    PROGRESS_SPREAD.forEach((k, j) => {
      if (f + j >= 0 && f + j < n) rate[f + j] += amount * k;
    });
  };
  for (const [kind, w] of weights ?? PROGRESS_WEIGHTS) {
    for (const h of hitsOf(a, kind)) if (h.t >= t0 && h.t <= t1) deposit(h.t, w * h.s);
  }
  if (opts.wordBursts) {
    for (const word of a.words) {
      const burst = opts.wordBursts[word.w.toLowerCase()];
      if (burst && word.t0 >= t0 && word.t0 <= t1) deposit(word.t0, burst);
    }
  }
  const cum = new Float64Array(n);
  let total = 0;
  for (let i = 0; i < n; i++) cum[i] = total += rate[i];
  return (t) => {
    if (t <= t0) return 0;
    if (t >= t1) return 1;
    const fi = Math.min(n - 1, Math.round(t * fps) - f0);
    return fi < 0 ? 0 : clamp01(cum[fi] / total);
  };
}
