/**
 * MediaPipe tracking — face landmarks and body segmentation.
 *
 * These run on the main thread (MediaPipe's GPU delegate renders into its own
 * WebGL context; `HTMLVideoElement` inputs are not usable from a worker). They
 * are per-frame on-demand queries, not bulk offline inference, so main-thread
 * execution is appropriate. Models are pinned to specific CDN URLs.
 */
import {
  FaceLandmarker,
  ImageSegmenter,
  FilesetResolver,
  type NormalizedLandmark,
} from '@mediapipe/tasks-vision';

// Pinned CDN assets — bump deliberately when revalidating.
const WASM_BASE =
  'https://cdn.jsdelivr.net/npm/@mediapipe/tasks-vision@0.10.22-rc.20250304/wasm';
const FACE_MODEL_URL =
  'https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task';
const SELFIE_MODEL_URL =
  'https://storage.googleapis.com/mediapipe-models/image_segmenter/selfie_multiclass_landscaper/float32/latest/selfie_multiclass_landscaper.tflite';

export interface FaceLandmarks {
  /** Normalized landmark points (0..1 image space). */
  points: { x: number; y: number }[];
}

export interface FaceTracker {
  /**
   * Detect face landmarks for a video frame at `timeMs` (media timestamp).
   * Returns `null` when no face is found.
   */
  detect(video: HTMLVideoElement | ImageBitmap, timeMs: number): FaceLandmarks | null;
  close(): void;
}

// MediaPipe's WasmFileset interface isn't exported, so mirror its shape.
interface VisionFileset {
  wasmLoaderPath: string;
  wasmBinaryPath: string;
  assetLoaderPath?: string;
  assetBinaryPath?: string;
}
let visionFileset: Promise<VisionFileset> | null = null;
function fileset(): Promise<VisionFileset> {
  if (!visionFileset) visionFileset = FilesetResolver.forVisionTasks(WASM_BASE);
  return visionFileset;
}
/**
 * Create a face tracker backed by MediaPipe `FaceLandmarker` (GPU delegate,
 * video running mode, single face).
 */
export async function createFaceTracker(): Promise<FaceTracker> {
  const fl = await FaceLandmarker.createFromOptions(await fileset(), {
    baseOptions: { modelAssetPath: FACE_MODEL_URL, delegate: 'GPU' },
    runningMode: 'VIDEO',
    numFaces: 1,
  });
  return {
    detect(video, timeMs) {
      const r = fl.detectForVideo(video, timeMs);
      const faces = r.faceLandmarks;
      if (!faces || faces.length === 0) return null;
      const pts = faces[0]!.map((p: NormalizedLandmark) => ({ x: p.x, y: p.y }));
      return { points: pts };
    },
    close() {
      fl.close();
    },
  };
}

export interface BodySegmenter {
  /**
   * Segment a frame into a category mask `ImageData` (one byte per pixel = the
   * selfie-multiclass category index), at the frame's native resolution.
   * Returns `null` when the segmenter produces no category mask.
   */
  segment(video: HTMLVideoElement | ImageBitmap, timeMs: number): ImageData | null;
  close(): void;
}

/**
 * Create a body segmenter backed by MediaPipe `ImageSegmenter` running the
 * selfie multiclass landscaper model (GPU delegate, video running mode).
 */
export async function createBodySegmenter(): Promise<BodySegmenter> {
  const seg = await ImageSegmenter.createFromOptions(await fileset(), {
    baseOptions: { modelAssetPath: SELFIE_MODEL_URL, delegate: 'GPU' },
    runningMode: 'VIDEO',
    outputCategoryMask: true,
    outputConfidenceMasks: false,
  });
  return {
    segment(video, timeMs) {
      const r = seg.segmentForVideo(video, timeMs);
      const mask = r.categoryMask;
      if (!mask) return null;
      const w = mask.width;
      const h = mask.height;
      const cats = mask.getAsUint8Array();
      // Copy immediately — the MPMask is owned by the task and reused next call.
      const id = new ImageData(w, h);
      const d = id.data;
      for (let i = 0; i < cats.length; i++) {
        const c = cats[i]!;
        d[i * 4] = c;
        d[i * 4 + 1] = c;
        d[i * 4 + 2] = c;
        d[i * 4 + 3] = 255;
      }
      return id;
    },
    close() {
      seg.close();
    },
  };
}
