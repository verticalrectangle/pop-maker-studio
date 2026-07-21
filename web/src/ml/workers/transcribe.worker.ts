/// <reference lib="webworker" />
/**
 * Transcription worker — runs @huggingface/transformers.js Whisper ASR fully
 * off the main thread. Receives 16 kHz mono PCM, returns word-level chunks with
 * source-file-relative timestamps (seconds).
 */
import { pipeline, env, type Chunk } from '@huggingface/transformers';
import { serve, type ProgressCb } from '../worker';

// Keep model artifacts in the browser cache; never look for local files.
env.allowLocalModels = false;
env.useBrowserCache = true;
// Threaded WASM needs SharedArrayBuffer (COOP/COEP). On hosts that can't
// send those headers (e.g. GitHub Pages), fall back to single-threaded.
const isolated = (self as unknown as { crossOriginIsolated?: boolean }).crossOriginIsolated ?? false;
if (!isolated && env.backends?.onnx?.wasm) {
  env.backends.onnx.wasm.numThreads = 1;
}

interface RunArgs {
  pcm: Float32Array;
  sampleRate: number;
  device: 'webgpu' | 'wasm';
}

interface ProgressInfo {
  status: string;
  file?: string;
  name?: string;
  progress?: number;
  loaded?: number;
  total?: number;
  task?: string;
  model?: string;
}

type AsrPipeline = {
  (audio: Float32Array, options: {
    return_timestamps: 'word';
    chunk_length_s?: number;
    stride_length_s?: number;
  }): Promise<{ text: string; chunks?: Chunk[] }>;
};

interface WordOut {
  word: string;
  start: number;
  end: number;
}

let asr: AsrPipeline | null = null;

function describeProgress(info: ProgressInfo): string {
  const file = info.file ?? info.name ?? info.model ?? 'model';
  switch (info.status) {
    case 'initiate': return `downloading ${file}`;
    case 'download': return `downloading ${file}`;
    case 'progress': return `downloading ${file} ${Math.round(info.progress ?? 0)}%`;
    case 'done': return `cached ${file}`;
    case 'ready': return 'ready';
    default: return info.status;
  }
}

async function run(args: unknown, onProgress: ProgressCb): Promise<unknown> {
  // Internal RPC: the main-thread caller always sends RunArgs.
  const a = args as RunArgs;
  const model =
    a.device === 'webgpu'
      ? 'onnx-community/whisper-base_timestamped'
      : 'Xenova/whisper-base';
  if (!asr) {
    onProgress(0, 'loading model');
    asr = (await pipeline('automatic-speech-recognition', model, {
      device: a.device,
      progress_callback: (info: ProgressInfo) => {
        const frac =
          info.status === 'progress' && typeof info.progress === 'number'
            ? info.progress / 100
            : info.status === 'done'
              ? 1
              : 0;
        onProgress(frac, describeProgress(info));
      },
    })) as unknown as AsrPipeline;
    onProgress(1, 'model ready');
  }

  onProgress(0, 'transcribing');
  const out = await asr(a.pcm, {
    return_timestamps: 'word',
    chunk_length_s: 30,
    stride_length_s: 5,
  });

  const words = mapChunksToWords(out.chunks ?? []);
  onProgress(1, 'transcribed');
  return words;
}

/**
 * Whisper word-timestamp chunks are normally one word each, but some models
 * group words. Split grouped text on whitespace and distribute the chunk's
 * time span linearly across the words.
 */
function mapChunksToWords(chunks: Chunk[]): WordOut[] {
  const out: WordOut[] = [];
  for (const c of chunks) {
    const start = c.timestamp[0] ?? 0;
    const end = c.timestamp[1] ?? start;
    const text = c.text.trim();
    if (!text) continue;
    const parts = text.split(/\s+/);
    if (parts.length === 1) {
      out.push({ word: parts[0]!, start, end });
      continue;
    }
    const span = (end - start) / parts.length;
    for (let i = 0; i < parts.length; i++) {
      const w = parts[i]!;
      out.push({ word: w, start: start + i * span, end: start + (i + 1) * span });
    }
  }
  return out;
}

serve({ run });
