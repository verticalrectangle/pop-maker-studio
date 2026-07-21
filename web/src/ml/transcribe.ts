/**
 * Speech-to-text transcription.
 *
 * Heavy work (model load + inference) runs in a dedicated worker
 * (`workers/transcribe.worker.ts`) via vite's `new Worker(new URL(...))`.
 * The main thread only decodes the entry's audio to 16 kHz mono PCM and
 * transfers it into the worker.
 */
import type { App } from '../core/app';
import type { MediaEntry, Word } from '../media/store';
import { WorkerRPC } from './worker';

let rpc: WorkerRPC | null = null;

function rpcClient(): WorkerRPC {
  if (!rpc) {
    const worker = new Worker(new URL('./workers/transcribe.worker.ts', import.meta.url), {
      type: 'module',
    });
    rpc = new WorkerRPC(worker);
  }
  return rpc;
}

/** True when a WebGPU adapter is available (cached after first probe). */
let gpuProbed = false;
let hasGpu = false;
async function detectGpu(): Promise<boolean> {
  if (gpuProbed) return hasGpu;
  gpuProbed = true;
  const nav = navigator as Navigator & {
    gpu?: { requestAdapter(): Promise<unknown> };
  };
  try {
    hasGpu = !!(await nav.gpu?.requestAdapter());
  } catch {
    hasGpu = false;
  }
  return hasGpu;
}

/**
 * Decode an entry's audio file to 16 kHz mono Float32Array PCM.
 * `AudioContext({ sampleRate: 16000 })` makes `decodeAudioData` resample to
 * 16 kHz; channels are averaged to mono.
 */
async function decodePcm16kMono(file: File): Promise<Float32Array> {
  const buf = await file.arrayBuffer();
  const ctx = new AudioContext({ sampleRate: 16000 });
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

/**
 * Transcribe `entry` into word-level `Word[]` (source-file seconds), store the
 * result on the entry, and emit `media:analyzed`. `onProgress` streams model
 * download + transcription progress as `(message, fraction 0..1)`.
 */
export async function transcribe(
  app: App,
  entry: MediaEntry,
  onProgress?: (msg: string, p: number) => void,
): Promise<Word[]> {
  if (!entry.hasAudio && entry.kind !== 'audio') {
    throw new Error('transcribe: entry has no audio');
  }
  const device = (await detectGpu()) ? 'webgpu' : 'wasm';
  onProgress?.('decoding audio', 0);
  const pcm = await decodePcm16kMono(entry.file);
  onProgress?.('audio decoded', 1);

  const words = await rpcClient().call<Word[]>(
    'run',
    { pcm, sampleRate: 16000, device },
    [pcm.buffer],
    (p, msg) => onProgress?.(msg ?? 'transcribing', p),
  );

  entry.transcript = words;
  app.events.emit('media:analyzed', entry);
  return words;
}
