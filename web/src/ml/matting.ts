/**
 * Background removal via Robust Video Matting.
 *
 * The RVM model runs in a dedicated worker (`workers/matting.worker.ts`) via
 * onnxruntime-web (WebGPU preferred, WASM fallback). The main thread decodes
 * source frames by seeking a hidden `<video>` element and shipping
 * `ImageBitmap`s to the worker; the worker returns per-frame alpha masks which
 * are cached in a module-level `MaskStore` keyed by entry id.
 */
import type { App } from '../core/app';
import type { Clip } from '../core/project';
import type { MediaEntry } from '../media/store';
import { WorkerRPC } from './worker';

let rpc: WorkerRPC | null = null;

function rpcClient(): WorkerRPC {
  if (!rpc) {
    const worker = new Worker(new URL('./workers/matting.worker.ts', import.meta.url), {
      type: 'module',
    });
    rpc = new WorkerRPC(worker);
  }
  return rpc;
}

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

// --- MaskStore --------------------------------------------------------------

interface MaskEntry {
  /** Alpha masks per decoded frame, aligned with `times`. `null` = failed. */
  masks: (ImageBitmap | null)[];
  /** Source-file seconds of each frame. */
  times: number[];
}

const maskStore = new Map<string, MaskEntry>();

/**
 * Look up the cached alpha mask for `entryId` nearest to `sourceTime`.
 * Returns `null` when no mask has been generated for that entry or the time is
 * out of range.
 */
export function getMaskAt(entryId: string, sourceTime: number): ImageBitmap | null {
  const entry = maskStore.get(entryId);
  if (!entry || entry.times.length === 0) return null;
  let lo = 0;
  let hi = entry.times.length - 1;
  if (sourceTime <= entry.times[0]!) return entry.masks[0] ?? null;
  if (sourceTime >= entry.times[hi]!) return entry.masks[hi] ?? null;
  while (lo < hi - 1) {
    const mid = (lo + hi) >> 1;
    const t = entry.times[mid]!;
    if (t < sourceTime) lo = mid;
    else hi = mid;
  }
  const dl = sourceTime - entry.times[lo]!;
  const dh = entry.times[hi]! - sourceTime;
  const idx = dl <= dh ? lo : hi;
  return entry.masks[idx] ?? null;
}

// --- MaskProvider (compositor hook) -----------------------------------------

export interface MaskProvider {
  /** Return the alpha mask for a clip at a source-relative time, or null. */
  getMask(clip: Clip, sourceTime: number): ImageBitmap | null;
}

/**
 * Build a `MaskProvider` backed by the `MaskStore`, resolving clip → media
 * entry via the app's `MediaStore`. The compositor calls this once and queries
 * it per rendered frame.
 */
export function createMaskProvider(app: App): MaskProvider {
  return {
    getMask(clip, sourceTime) {
      if (clip.source == null) return null;
      const entry = app.media.get(clip.source);
      if (!entry) return null;
      return getMaskAt(entry.id, sourceTime);
    },
  };
}

// --- frame extraction -------------------------------------------------------

function seekTo(video: HTMLVideoElement, t: number): Promise<void> {
  // Executor form: resolve/reject fire from async event listeners.
  // (Promise.withResolvers needs ES2024; the project targets ES2022.)
  return new Promise<void>((resolve, reject) => {
    const onSeeked = (): void => {
      video.removeEventListener('seeked', onSeeked);
      resolve();
    };
    video.addEventListener('seeked', onSeeked);
    video.currentTime = t;
    // Guard against seeks that never fire (some codecs at the last frame).
    setTimeout(() => {
      video.removeEventListener('seeked', onSeeked);
      reject(new Error(`seek to ${t}s timed out`));
    }, 5000);
  });
}
async function grabFrame(
  video: HTMLVideoElement,
  w: number,
  h: number,
): Promise<ImageBitmap> {
  const oc = new OffscreenCanvas(w, h);
  const ctx = oc.getContext('2d');
  if (!ctx) throw new Error('offscreen 2d context unavailable');
  ctx.drawImage(video, 0, 0, w, h);
  return oc.transferToImageBitmap();
}

// --- public API -------------------------------------------------------------

/**
 * Remove the background from `entry` (video/image) by running RVM over every
 * frame and caching the resulting alpha masks. Emits `media:analyzed` when
 * complete. `onProgress` receives a 0..1 fraction.
 */
export async function removeBackground(
  app: App,
  entry: MediaEntry,
  onProgress?: (p: number) => void,
): Promise<void> {
  if (entry.kind !== 'video' && entry.kind !== 'image') {
    throw new Error('removeBackground: entry is not video or image');
  }
  const device = (await detectGpu()) ? 'webgpu' : 'wasm';
  const rpc = rpcClient();
  const base = import.meta.env.BASE_URL;
  await rpc.call('init', {
    device,
    urls: {
      jsep: `${base}models/ort-wasm-simd-threaded.jsep.wasm`,
      plain: `${base}models/ort-wasm-simd-threaded.wasm`,
      model: `${base}models/rvm_mobilenetv3_fp32.onnx`,
    },
  });

  const url = URL.createObjectURL(entry.file);
  const video = document.createElement('video');
  video.src = url;
  video.muted = true;
  await new Promise<void>((resolve, reject) => {
    video.addEventListener('loadedmetadata', () => resolve(), { once: true });
    video.addEventListener('error', () => reject(new Error('video load error')), {
      once: true,
    });
  });

  const w = video.videoWidth || entry.width;
  const h = video.videoHeight || entry.height;
  const fps = entry.fps > 0 ? entry.fps : 30;
  const duration = entry.kind === 'image' ? 0 : video.duration || entry.duration;
  const frameCount =
    entry.kind === 'image' ? 1 : Math.max(1, Math.ceil(duration * fps));

  const masks: (ImageBitmap | null)[] = new Array(frameCount).fill(null);
  const times: number[] = new Array(frameCount);

  for (let i = 0; i < frameCount; i++) {
    const t = entry.kind === 'image' ? 0 : i / fps;
    times[i] = t;
    try {
      await seekTo(video, t);
      const bmp = await grabFrame(video, w, h);
      const mask = await rpc.call<ImageBitmap>(
        'infer',
        { bitmap: bmp, width: w, height: h },
        [bmp],
        (p) => onProgress?.((i + p) / frameCount),
      );
      masks[i] = mask;
    } catch {
      masks[i] = null;
    }
  }

  URL.revokeObjectURL(url);
  maskStore.set(entry.id, { masks, times });
  app.events.emit('media:analyzed', entry);
}
