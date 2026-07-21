/**
 * Beat/tempo analysis.
 *
 * Runs in a dedicated worker (`workers/beats.worker.ts`) which tries
 * essentia.js first and falls back to a pure-JS autocorrelation + onset-flux
 * estimator. The main thread only decodes the entry's audio to mono PCM
 * (22.05 kHz — enough resolution for rhythm, half the work of 44.1 kHz) and
 * transfers it in.
 */
import type { App } from '../core/app';
import type { MediaEntry } from '../media/store';
import { WorkerRPC } from './worker';

let rpc: WorkerRPC | null = null;

function rpcClient(): WorkerRPC {
  if (!rpc) {
    const worker = new Worker(new URL('./workers/beats.worker.ts', import.meta.url), {
      type: 'module',
    });
    rpc = new WorkerRPC(worker);
  }
  return rpc;
}

async function decodePcmMono(file: File, sampleRate: number): Promise<Float32Array> {
  const buf = await file.arrayBuffer();
  const ctx = new AudioContext({ sampleRate });
  try {
    const decoded = await ctx.decodeAudioData(buf);
    const ch = decoded.numberOfChannels;
    if (ch === 1) return decoded.getChannelData(0).slice();
    const len = decoded.length;
    const out = new Float32Array(len);
    for (let c = 0; c < ch; c++) {
      const data = decoded.getChannelData(c);
      for (let i = 0; i < len; i++) out[i] = (out[i] ?? 0) + (data[i] ?? 0) / ch;
    }
    return out;
  } finally {
    void ctx.close();
  }
}

export interface BeatAnalysis {
  bpm: number;
  beats: number[];
  rms: Float32Array;
}

/**
 * Analyze `entry` for BPM, beat positions (source-file seconds), and per-second
 * RMS. Stores results on the entry and emits `media:analyzed`.
 */
export async function analyzeBeats(app: App, entry: MediaEntry): Promise<BeatAnalysis> {
  if (!entry.hasAudio && entry.kind !== 'audio') {
    throw new Error('analyzeBeats: entry has no audio');
  }
  const pcm = await decodePcmMono(entry.file, 22050);
  const result = await rpcClient().call<BeatAnalysis>('run', {
    pcm,
    sampleRate: 22050,
  });
  entry.bpm = result.bpm;
  entry.beats = result.beats;
  entry.rms = result.rms;
  app.events.emit('media:analyzed', entry);
  return result;
}
