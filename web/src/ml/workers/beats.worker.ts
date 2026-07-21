/// <reference lib="webworker" />
/**
 * Beat/tempo analysis worker.
 *
 * Primary path: essentia.js (RhythmExtractor2013 + per-second RMS) running in
 * WASM. essentia.js ships a CJS module + a side .wasm whose path vite can't
 * always resolve inside a worker bundle, so the dynamic import is wrapped in a
 * try/catch.
 *
 * Fallback path (used when essentia.js fails to load): a self-contained
 * autocorrelation tempo estimator + onset-flux beat picker + per-second RMS,
 * implemented directly on the PCM. The fallback is deliberately conservative
 * (60–180 BPM window, energy-envelope onset strength) and documented here so
 * the behaviour is auditable without essentia.
 */
import { serve, type ProgressCb } from '../worker';

interface RunArgs {
  pcm: Float32Array;
  sampleRate: number;
}

interface BeatResult {
  bpm: number;
  beats: number[];
  rms: Float32Array;
}

async function run(args: unknown, onProgress: ProgressCb): Promise<unknown> {
  // Internal RPC: the main-thread caller always sends RunArgs.
  const a = args as RunArgs;
  onProgress(0, 'analyzing beats');
  let result: BeatResult;
  try {
    result = await withEssentia(a.pcm, a.sampleRate);
  } catch (err) {
    onProgress(
      0.1,
      `essentia unavailable (${err instanceof Error ? err.message : 'load error'}); using fallback`,
    );
    result = analyzeBeatsFallback(a.pcm, a.sampleRate);
  }
  onProgress(1, 'beats ready');
  return result;
}

// --- essentia.js path -------------------------------------------------------

interface EssentiaWASMModule {
  EssentiaWASM: unknown;
}
interface EssentiaCore {
  default: new (wasm: unknown) => EssentiaInstance;
}
interface EssentiaInstance {
  RhythmExtractor2013(
    signal: Float32Array,
    maxTempo?: number,
    method?: string,
    minTempo?: number,
  ): { bpm: number; beats_position: Float32Array };
  RMS(array: Float32Array): { rms: number };
  FrameGenerator(audio: Float32Array, frameSize?: number, hopSize?: number): Iterable<Float32Array>;
  arrayToVector(arr: Float32Array): unknown;
}

async function withEssentia(pcm: Float32Array, sampleRate: number): Promise<BeatResult> {
  // Static import cannot work here: essentia.js's side .wasm may fail to
  // resolve under vite's worker bundler, and a static import would fail the
  // whole worker at load time. Dynamic import + try/catch lets the fallback
  // path take over gracefully.
  const wasmMod = (await import('essentia.js/dist/essentia-wasm.es.js')) as EssentiaWASMModule;
  const coreMod = (await import('essentia.js/dist/essentia.js-core.es.js')) as EssentiaCore;
  const essentia = new coreMod.default(wasmMod.EssentiaWASM);

  const r = essentia.RhythmExtractor2013(pcm, 180, 'degara', 60);
  const beats = Array.from(r.beats_position ?? []);
  const rms = perSecondRms(pcm, sampleRate);
  return { bpm: r.bpm, beats, rms };
}

// --- fallback path ----------------------------------------------------------

/** Frame the signal into overlapping windows and return per-frame RMS energy. */
function frameEnergy(pcm: Float32Array, frameSize: number, hop: number): Float32Array {
  const frames = Math.max(1, Math.floor((pcm.length - frameSize) / hop) + 1);
  const env = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    let sum = 0;
    const off = i * hop;
    for (let j = 0; j < frameSize; j++) {
      const s = pcm[off + j] ?? 0;
      sum += s * s;
    }
    env[i] = Math.sqrt(sum / frameSize);
  }
  return env;
}

function perSecondRms(pcm: Float32Array, sampleRate: number): Float32Array {
  const durSec = pcm.length / sampleRate;
  const buckets = Math.max(1, Math.ceil(durSec));
  const energy = new Float32Array(buckets);
  const count = new Float32Array(buckets);
  const frameSize = 1024;
  const hop = 512;
  const frames = Math.max(1, Math.floor((pcm.length - frameSize) / hop) + 1);
  for (let i = 0; i < frames; i++) {
    const t = (i * hop) / sampleRate;
    const b = Math.min(buckets - 1, Math.floor(t));
    let sum = 0;
    const off = i * hop;
    for (let j = 0; j < frameSize; j++) {
      const s = pcm[off + j] ?? 0;
      sum += s * s;
    }
    energy[b] = (energy[b] ?? 0) + sum / frameSize;
    count[b] = (count[b] ?? 0) + 1;
  }
  const rms = new Float32Array(buckets);
  for (let b = 0; b < buckets; b++) {
    const c = count[b] ?? 0;
    rms[b] = c > 0 ? Math.sqrt((energy[b] ?? 0) / c) : 0;
  }
  return rms;
}

/**
 * Autocorrelation tempo: correlate the energy envelope against itself across
 * lags corresponding to 60–180 BPM and keep the strongest. Beats are placed on
 * the tempo grid starting from the strongest onset, snapped to local onset
 * peaks within ±20% of the period.
 */
function analyzeBeatsFallback(pcm: Float32Array, sampleRate: number): BeatResult {
  const hop = 512;
  const frameSize = 1024;
  const fps = sampleRate / hop;
  const env = frameEnergy(pcm, frameSize, hop);
  const frames = env.length;

  // Onset strength = half-wave-rectified envelope difference.
  const flux = new Float32Array(frames);
  for (let i = 1; i < frames; i++) {
    const d = (env[i] ?? 0) - (env[i - 1] ?? 0);
    flux[i] = d > 0 ? d : 0;
  }

  const minBpm = 60;
  const maxBpm = 180;
  const minLag = Math.max(2, Math.floor((fps * 60) / maxBpm));
  const maxLag = Math.min(frames - 1, Math.ceil((fps * 60) / minBpm));
  let bestLag = minLag;
  let bestVal = -Infinity;
  for (let lag = minLag; lag <= maxLag; lag++) {
    let acc = 0;
    for (let i = lag; i < frames; i++) acc += (env[i] ?? 0) * (env[i - lag] ?? 0);
    if (acc > bestVal) {
      bestVal = acc;
      bestLag = lag;
    }
  }
  const bpm = (60 * fps) / bestLag;

  // Anchor = strongest onset frame.
  let anchor = 0;
  let anchorVal = -Infinity;
  for (let i = 0; i < frames; i++) {
    const f = flux[i] ?? 0;
    if (f > anchorVal) {
      anchorVal = f;
      anchor = i;
    }
  }

  const win = Math.max(1, Math.floor(bestLag * 0.2));
  const beatsSet = new Set<number>();
  for (let t = anchor; t < frames; t += bestLag) {
    beatsSet.add((snapToPeak(flux, t, win) * hop) / sampleRate);
  }
  for (let t = anchor - bestLag; t >= 0; t -= bestLag) {
    beatsSet.add((snapToPeak(flux, t, win) * hop) / sampleRate);
  }
  const beats = [...beatsSet].sort((a, b) => a - b);
  const rms = perSecondRms(pcm, sampleRate);
  return { bpm, beats, rms };
}

function snapToPeak(flux: Float32Array, center: number, win: number): number {
  const lo = Math.max(0, center - win);
  const hi = Math.min(flux.length - 1, center + win);
  let best = center;
  let bestVal = -Infinity;
  for (let i = lo; i <= hi; i++) {
    const f = flux[i] ?? 0;
    if (f > bestVal) {
      bestVal = f;
      best = i;
    }
  }
  return best;
}

serve({ run });
