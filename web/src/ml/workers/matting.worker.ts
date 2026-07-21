/// <reference lib="webworker" />
/**
 * Robust Video Matting worker — onnxruntime-web running `rvm_mobilenetv3_fp32`.
 *
 * WebGPU execution provider is preferred (jsep wasm bundle); falls back to the
 * plain wasm bundle. The RVM recurrent states (r1i..r4i) are kept in module
 * scope so they persist across frames within one `removeBackground` session.
 *
 * Internal resolution is fixed at 512×288 (the standard RVM inference size).
 * The caller sends an ImageBitmap of the source frame at its native size; the
 * worker resizes to 512×288 for inference and returns an alpha-mask ImageBitmap
 * at the original size.
 *
 * State shapes (channels, H/8, W/8 and H/16, W/16 of the internal 288×512):
 *   r1i [1,16,36,64]  r2i [1,20,36,64]
 *   r3i [1,40,18,32]  r4i [1,64,18,32]
 * These match the official `rvm_mobilenetv3_fp32.onnx` export with
 * `downsample_ratio = 1.0` (src already at internal resolution). If a different
 * RVM export is dropped in, adjust `STATE_SHAPES`.
 */
import { serve, type ProgressCb, type TransferResult } from '../worker';

// Model + wasm URLs are passed from the main thread (which knows the correct
// base path for the deployment — GitHub Pages subpath, custom domain, etc.).
let jsepWasmUrl = '';
let plainWasmUrl = '';
let MODEL_URL = '';

const INTERNAL_W = 512;
const INTERNAL_H = 288;

const STATE_SHAPES = [
  [1, 16, INTERNAL_H >> 3, INTERNAL_W >> 3], // r1i
  [1, 20, INTERNAL_H >> 3, INTERNAL_W >> 3], // r2i
  [1, 40, INTERNAL_H >> 4, INTERNAL_W >> 4], // r3i
  [1, 64, INTERNAL_H >> 4, INTERNAL_W >> 4], // r4i
] as const;

const STATE_NAMES = ['r1i', 'r2i', 'r3i', 'r4i'] as const;
const STATE_OUT_NAMES = ['r1o', 'r2o', 'r3o', 'r4o'] as const;

// Minimal ort surface — onnxruntime-web's typed entry points re-export the same
// types, so we import dynamically by device and share this interface.
interface OrtTensor {
  data: Float32Array;
  dims: readonly number[];
  type: string;
}
interface OrtSession {
  run(feeds: Record<string, OrtTensor>): Promise<Record<string, OrtTensor>>;
  inputNames: readonly string[];
  outputNames: readonly string[];
  release(): Promise<void>;
}
interface OrtEnv {
  wasm: {
    wasm?: string;
    wasmPaths?: string;
    numThreads?: number;
    proxy?: boolean;
  };
}
interface OrtModule {
  env: OrtEnv;
  InferenceSession: {
    create(
      buffer: Uint8Array,
      options: {
        executionProviders: string[];
        graphOptimizationLevel?: string;
      },
    ): Promise<OrtSession>;
  };
  Tensor: new (type: 'float32', data: Float32Array, dims: readonly number[]) => OrtTensor;
}

let ort: OrtModule | null = null;
let session: OrtSession | null = null;
let states: Float32Array[] | null = null;

async function ensureOrt(device: 'webgpu' | 'wasm'): Promise<OrtModule> {
  if (ort) return ort;
  // Runtime-selected bundle: the webgpu EP ships only in the webgpu bundle,
  // the wasm EP in the wasm bundle. Both re-export the onnxruntime-common API.
  ort = (device === 'webgpu'
    ? await import('onnxruntime-web/webgpu')
    : await import('onnxruntime-web/wasm')) as unknown as OrtModule;
  ort.env.wasm.wasm = device === 'webgpu' ? jsepWasmUrl : plainWasmUrl;
  // Threaded WASM needs SharedArrayBuffer (COOP/COEP). On hosts that can't
  // send those headers (e.g. GitHub Pages), fall back to single-threaded.
  const isolated = (self as unknown as { crossOriginIsolated?: boolean }).crossOriginIsolated ?? false;
  ort.env.wasm.numThreads = isolated ? Math.min(4, navigator.hardwareConcurrency ?? 2) : 1;
  return ort;
}

async function init(args: unknown, onProgress: ProgressCb): Promise<unknown> {
  // Internal RPC: the main-thread caller sends { device, urls }.
  const a = args as { device: 'webgpu' | 'wasm'; urls: { jsep: string; plain: string; model: string } };
  jsepWasmUrl = a.urls.jsep;
  plainWasmUrl = a.urls.plain;
  MODEL_URL = a.urls.model;
  const useGpu = a.device === 'webgpu';
  onProgress(0, 'loading matting model');
  const m = await ensureOrt(useGpu ? 'webgpu' : 'wasm');
  const res = await fetch(MODEL_URL);
  if (!res.ok) throw new Error(`failed to fetch RVM model (${res.status})`);
  const bytes = new Uint8Array(await res.arrayBuffer());
  session = await m.InferenceSession.create(bytes, {
    executionProviders: [useGpu ? 'webgpu' : 'wasm'],
    graphOptimizationLevel: 'all',
  });
  // Validate the model exposes the expected RVM inputs.
  const expected = ['src', ...STATE_NAMES, 'downsample_ratio'];
  for (const name of expected) {
    if (!session.inputNames.includes(name)) {
      throw new Error(
        `RVM model missing input '${name}'; has [${session.inputNames.join(', ')}]`,
      );
    }
  }
  states = STATE_SHAPES.map((dims) => {
    const n = dims.reduce((acc, b) => acc * b, 1);
    return new Float32Array(n);
  });
  onProgress(1, 'matting model ready');
  return null;
}

interface InferArgs {
  bitmap: ImageBitmap;
  width: number;
  height: number;
}

async function infer(args: unknown, onProgress: ProgressCb): Promise<unknown> {
  if (!session || !ort || !states) throw new Error('matting worker not initialized');
  // Internal RPC: the main-thread caller always sends InferArgs.
  const a = args as InferArgs;
  const m = ort;

  // Resize the source frame to the internal inference resolution.
  const srcCanvas = new OffscreenCanvas(INTERNAL_W, INTERNAL_H);
  const sctx = srcCanvas.getContext('2d');
  if (!sctx) throw new Error('offscreen 2d context unavailable');
  sctx.drawImage(a.bitmap, 0, 0, INTERNAL_W, INTERNAL_H);
  const img = sctx.getImageData(0, 0, INTERNAL_W, INTERNAL_H);
  a.bitmap.close();

  // NCHW float32 RGB normalized to [0,1].
  const srcData = new Float32Array(3 * INTERNAL_H * INTERNAL_W);
  const px = img.data;
  const plane = INTERNAL_H * INTERNAL_W;
  for (let i = 0; i < plane; i++) {
    srcData[i] = (px[i * 4] ?? 0) / 255;
    srcData[i + plane] = (px[i * 4 + 1] ?? 0) / 255;
    srcData[i + plane * 2] = (px[i * 4 + 2] ?? 0) / 255;
  }

  const feeds: Record<string, OrtTensor> = {
    src: new m.Tensor('float32', srcData, [1, 3, INTERNAL_H, INTERNAL_W]),
    downsample_ratio: new m.Tensor('float32', new Float32Array([1.0]), [1]),
  };
  for (let i = 0; i < STATE_NAMES.length; i++) {
    const name = STATE_NAMES[i]!;
    const dims = STATE_SHAPES[i]!;
    const st = states[i]!;
    feeds[name] = new m.Tensor('float32', st, dims);
  }

  const out = await session.run(feeds);

  // Roll recurrent states forward.
  for (let i = 0; i < STATE_OUT_NAMES.length; i++) {
    const o = out[STATE_OUT_NAMES[i]!];
    if (o && states[i]) states[i] = o.data;
  }

  const pha = out.pha;
  if (!pha) throw new Error('RVM model did not output `pha`');
  // pha is [1,1,INTERNAL_H,INTERNAL_W]; build an alpha mask at the original size.
  const maskCanvas = new OffscreenCanvas(a.width, a.height);
  const mctx = maskCanvas.getContext('2d');
  if (!mctx) throw new Error('offscreen 2d context unavailable');
  // Upscale the internal alpha to the original frame size via a temp canvas.
  const alphaCanvas = new OffscreenCanvas(INTERNAL_W, INTERNAL_H);
  const actx = alphaCanvas.getContext('2d');
  if (!actx) throw new Error('offscreen 2d context unavailable');
  const alphaImg = actx.createImageData(INTERNAL_W, INTERNAL_H);
  const ad = alphaImg.data;
  for (let i = 0; i < plane; i++) {
    const alphaVal = Math.max(0, Math.min(255, Math.round((pha.data[i] ?? 0) * 255)));
    ad[i * 4] = 255;
    ad[i * 4 + 1] = 255;
    ad[i * 4 + 2] = 255;
    ad[i * 4 + 3] = alphaVal;
  }
  actx.putImageData(alphaImg, 0, 0);
  mctx.drawImage(alphaCanvas, 0, 0, a.width, a.height);
  const mask = maskCanvas.transferToImageBitmap();
  onProgress(1, 'frame matted');
  const result: TransferResult = { result: mask, transfer: [mask] };
  return result;
}

serve({ init, infer });
