// Frame provider for the compositor.
//
// Three serving strategies, picked per media entry:
//   image            -> cached ImageBitmap (the entry thumbnail)
//   mp4 / mov        -> mp4box demux + WebCodecs VideoDecoder with a small
//                      ordered frame cache and keyframe-aware seeking
//   webm / other     -> a hidden HTMLVideoElement seeked on demand
//
// `getFrame` is synchronous and never throws: it returns the best frame
// already decoded at or before `sourceTime`, or `null` when nothing is ready
// yet. All decoding/seeking happens asynchronously in the background.
import { createFile, DataStream, type ISOFile, type MP4Info, type MP4Sample, type MP4Track, type MP4Buffer } from "mp4box";
import type { MediaEntry } from "./store";
import type { App } from "../core/app";
import type { Clip } from "../core/project";
import type { FrameProvider } from "../render/compositor";

const LOOKAHEAD = 30; // max frames held in the decoded cache window
const SEEK_EPSILON = 1 / 120; // ~one frame at 60fps; treat smaller deltas as "same frame"

function isMp4Name(name: string): boolean {
  const lower = name.toLowerCase();
  return lower.endsWith(".mp4") || lower.endsWith(".mov") || lower.endsWith(".m4v");
}

/** Demuxed mp4 state for one entry. */
interface MP4State {
  samples: MP4Sample[]; // composition-ordered video samples
  times: Float64Array; // sample cts in seconds, parallel to `samples`
  keyframes: number[]; // indices into `samples` that are sync samples
  codec: string;
  description: Uint8Array | undefined;
  width: number;
  height: number;
  fps: number;
  duration: number;
}

/** Per-entry runtime decode state. */
interface MP4Runtime {
  decoder: VideoDecoder | null;
  /** Sample index the decoder was (re)configured to start from. */
  baseSample: number;
  /** Next sample index to feed into the decoder. */
  feedCursor: number;
  /** Cached decoded frames: sample index -> VideoFrame. */
  cache: Map<number, VideoFrame>;
  /** Sample indices currently in the cache, in feed order. */
  cacheOrder: number[];
  /** Highest sample index whose frame has been emitted so far. */
  lastEmitted: number;
  /** True once a fatal error forced a fallback to the <video> path. */
  failed: boolean;
}

/** Full per-entry record held by DecoderManager. */
interface EntryState {
  mp4: MP4State | null;
  rt: MP4Runtime | null;
  video: HTMLVideoElement | null; // webm/unsupported + mp4 fallback
  videoReady: boolean;
  videoSeekTo: number; // last requested seek target
  videoSeeking: boolean; // true while a seek is in flight
  canvas: HTMLCanvasElement | null; // webm frame is drawn here for the compositor
  canvasCtx: CanvasRenderingContext2D | null;
  image: ImageBitmap | null; // cached image bitmap
  loadPromise: Promise<void> | null;
}

/** Build the MP4State by fully demuxing the file's video track. */
export async function demuxMp4(file: File): Promise<MP4State> {
  const buf = (await file.arrayBuffer()) as MP4Buffer;
  buf.fileStart = 0;

  const iso: ISOFile = createFile();
  let infoResolve!: (i: MP4Info) => void;
  let infoReject!: (e: unknown) => void;
  const infoReady = new Promise<MP4Info>((res, rej) => {
    infoResolve = res;
    infoReject = rej;
  });

  const samples: MP4Sample[] = [];
  let samplesResolve!: () => void;
  let samplesReject!: (e: unknown) => void;
  const samplesReady = new Promise<void>((res, rej) => {
    samplesResolve = res;
    samplesReject = rej;
  });

  let videoTrack: MP4Track | undefined;

  iso.onReady = (info) => {
    videoTrack = info.videoTracks[0];
    if (!videoTrack) {
      infoResolve(info);
      samplesResolve();
      return;
    }
    if (videoTrack.nb_samples === 0) {
      samplesResolve();
    } else {
      iso.setExtractionOptions(videoTrack.id, 0);
      iso.start();
    }
    infoResolve(info);
  };
  iso.onSamples = (_id, _user, batch) => {
    samples.push(...batch);
    if (videoTrack && samples.length >= videoTrack.nb_samples) samplesResolve();
  };
  iso.onError = (e) => {
    infoReject(new Error(e));
    samplesReject(new Error(e));
  };

  iso.start();
  iso.appendBuffer(buf);
  iso.flush();

  const info = await infoReady;
  // If there is no video track, this isn't a usable mp4 for frames.
  if (!videoTrack) {
    throw new Error("mp4 has no video track");
  }
  await samplesReady;

  // Sort by composition timestamp and build parallel time array.
  const sorted = samples.slice().sort((a, b) => a.cts - b.cts);
  const times = new Float64Array(sorted.length);
  const keyframes: number[] = [];
  for (let i = 0; i < sorted.length; i++) {
    const s = sorted[i]!;
    times[i] = s.cts / s.timescale;
    if (s.is_sync) keyframes.push(i);
  }
  if (keyframes.length === 0 && sorted.length > 0) keyframes.push(0);

  const codec = videoTrack.codec;
  const description = codecDescription(iso, videoTrack.id);
  const duration = info.duration / info.timescale || times[times.length - 1] || 0;
  const fps = sorted.length > 0 && duration > 0 ? sorted.length / duration : 0;

  return {
    samples: sorted,
    times,
    keyframes,
    codec,
    description,
    width: videoTrack.track_width,
    height: videoTrack.track_height,
    fps,
    duration,
  };
}

/** Extract the raw decoder-config record (avcC/hvcC body) for VideoDecoder. */
function codecDescription(iso: ISOFile, trackId: number): Uint8Array | undefined {
  const trak = iso.getTrackById(trackId);
  const entry = trak.mdia?.minf?.stbl?.stsd?.entries[0];
  const box = entry?.avcC ?? entry?.hvcC;
  if (!box) return undefined;
  // Serialize the box, then skip the 8-byte box header (size + fourcc) to get
  // the raw decoder configuration record WebCodecs expects.
  const stream = new DataStream(new ArrayBuffer(box.size + 8), 0);
  stream.endianness = DataStream.BIG_ENDIAN;
  box.write(stream);
  return new Uint8Array(stream.buffer, 8, box.size);
}

/** Largest keyframe index <= the given sample index. */
function keyframeBefore(state: MP4State, sampleIdx: number): number {
  let lo = 0;
  let hi = state.keyframes.length - 1;
  let result = state.keyframes[0] ?? 0;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    const k = state.keyframes[mid]!;
    if (k <= sampleIdx) {
      result = k;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return result;
}

/** Sample index whose frame should be shown at `sourceTime` (best <= time). */
function sampleAtTime(state: MP4State, sourceTime: number): number {
  const times = state.times;
  let lo = 0;
  let hi = times.length - 1;
  if (hi < 0) return -1;
  if (sourceTime <= times[0]!) return 0;
  let result = 0;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (times[mid]! <= sourceTime) {
      result = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return result;
}

export class DecoderManager {
  private states = new Map<string, EntryState>();
  /** Entries that must not use the WebCodecs path (decoder unsupported). */
  private forceVideo = new Set<string>();

  /** Begin loading/demuxing an entry's frames in the background. */
  preload(entry: MediaEntry): void {
    this.stateFor(entry);
  }

  /**
   * Like getFrame but waits (bounded) until a frame at/near sourceTime exists.
   * Used by offline export where dropping frames is worse than waiting.
   */
  async awaitFrame(
    entry: MediaEntry,
    sourceTime: number,
    timeoutMs = 2000,
  ): Promise<VideoFrame | ImageBitmap | HTMLCanvasElement | null> {
    const deadline = performance.now() + timeoutMs;
    let frame = this.getFrame(entry, sourceTime);
    while (!frame && performance.now() < deadline) {
      const { promise, resolve } = Promise.withResolvers<void>();
      setTimeout(resolve, 16);
      await promise;
      frame = this.getFrame(entry, sourceTime);
    }
    return frame;
  }

  /** Best available frame at or before `sourceTime`, or `null`. Sync, never throws. */
  getFrame(entry: MediaEntry, sourceTime: number): VideoFrame | ImageBitmap | HTMLCanvasElement | null {
    try {
      const st = this.stateFor(entry);
      if (entry.kind === "image") return this.getImage(st, entry);
      if (st.mp4 && !st.rt?.failed && !this.forceVideo.has(entry.id)) {
        return this.getMp4Frame(st, sourceTime);
      }
      return this.getVideoFrame(st, entry, sourceTime);
    } catch {
      return null;
    }
  }

  dispose(): void {
    for (const st of this.states.values()) {
      st.rt?.decoder?.close();
      for (const f of st.rt?.cache.values() ?? []) {
        try {
          f.close();
        } catch {
          /* already closed */
        }
      }
      st.rt = null;
      st.mp4 = null;
      if (st.video) {
        st.video.src = "";
        st.video.remove();
        st.video = null;
      }
      st.image = null;
    }
    this.states.clear();
  }

  // --- per-strategy helpers ---

  private stateFor(entry: MediaEntry): EntryState {
    let st = this.states.get(entry.id);
    if (st) return st;
    st = {
      mp4: null,
      rt: null,
      video: null,
      videoReady: false,
      videoSeekTo: -1,
      videoSeeking: false,
      canvas: null,
      canvasCtx: null,
      image: null,
      loadPromise: this.loadEntry(entry),
    };
    this.states.set(entry.id, st);
    return st;
  }

  private loadEntry(entry: MediaEntry): Promise<void> {
    if (entry.kind === "image") {
      // Images are served from the entry thumbnail; nothing to load here.
      return Promise.resolve();
    }
    if (isMp4Name(entry.file.name) && typeof VideoDecoder !== "undefined") {
      const p = demuxMp4(entry.file)
        .then((state) => {
          const st = this.states.get(entry.id);
          if (st) {
            st.mp4 = state;
            st.rt = {
              decoder: null,
              baseSample: -1,
              feedCursor: 0,
              cache: new Map(),
              cacheOrder: [],
              lastEmitted: -1,
              failed: false,
            };
          }
        })
        .catch(() => {
          // Demux failed -> fall back to the <video> element path.
          this.forceVideo.add(entry.id);
        });
      return p;
    }
    // webm / unsupported codec: set up a hidden <video> element lazily on first
    // getFrame (we need a URL; created in getVideoFrame).
    return Promise.resolve();
  }

  private getImage(st: EntryState, entry: MediaEntry): ImageBitmap | null {
    if (st.image) return st.image;
    if (entry.thumbnail) {
      st.image = entry.thumbnail;
      return st.image;
    }
    return null;
  }

  private getMp4Frame(st: EntryState, sourceTime: number): VideoFrame | null {
    const state = st.mp4;
    const rt = st.rt;
    if (!state || !rt) return null;
    if (state.samples.length === 0) return null;

    const target = sampleAtTime(state, Math.max(0, sourceTime));
    if (target < 0) return null;

    // Best cached frame at or before the target.
    const best = this.bestCached(state, rt, target);
    this.scheduleDecode(state, rt, target);
    return best;
  }

  private bestCached(state: MP4State, rt: MP4Runtime, target: number): VideoFrame | null {
    // Walk backwards from target to find the nearest cached sample.
    for (let i = target; i >= 0; i--) {
      const f = rt.cache.get(i);
      if (f) return f;
      if (i < target - LOOKAHEAD) break;
    }
    return null;
  }

  private scheduleDecode(state: MP4State, rt: MP4Runtime, target: number): void {
    const keyIdx = keyframeBefore(state, target);
    // (Re)configure the decoder if we need to start from a different keyframe
    // than the one it is currently anchored to, or if it has no decoder yet.
    if (rt.decoder === null || rt.baseSample !== keyIdx || rt.decoder.state === "closed") {
      this.resetDecoder(state, rt, keyIdx);
    }
    if (rt.decoder === null || rt.decoder.state !== "configured") return;

    // Feed forward from the feed cursor up to target + lookahead.
    const end = Math.min(state.samples.length - 1, target + LOOKAHEAD);
    for (let i = rt.feedCursor; i <= end; i++) {
      const s = state.samples[i]!;
      const chunk = new EncodedVideoChunk({
        type: s.is_sync ? "key" : "delta",
        timestamp: Math.round((s.cts / s.timescale) * 1_000_000),
        duration: Math.round((s.duration / s.timescale) * 1_000_000),
        data: s.data,
      });
      try {
        rt.decoder.decode(chunk);
      } catch {
        this.failToVideo(rt);
        return;
      }
      rt.feedCursor = i + 1;
    }
    // If the decoder queue is healthy, let it drain; otherwise back off.
    if (rt.decoder.decodeQueueSize > LOOKAHEAD * 2) return;
  }

  private resetDecoder(state: MP4State, rt: MP4Runtime, keyIdx: number): void {
    // Close any existing decoder and cached frames.
    if (rt.decoder) {
      try {
        rt.decoder.close();
      } catch {
        /* ignore */
      }
    }
    this.clearCache(rt);

    let decoder: VideoDecoder;
    try {
      decoder = new VideoDecoder({
        output: (frame) => this.onFrame(state, rt, frame),
        error: () => this.failToVideo(rt),
      });
    } catch {
      this.failToVideo(rt);
      return;
    }

    const config: VideoDecoderConfig = {
      codec: state.codec,
      codedWidth: state.width,
      codedHeight: state.height,
    };
    if (state.description) config.description = state.description;

    try {
      decoder.configure(config);
    } catch {
      try {
        decoder.close();
      } catch {
        /* ignore */
      }
      this.failToVideo(rt);
      return;
    }

    rt.decoder = decoder;
    rt.baseSample = keyIdx;
    rt.feedCursor = keyIdx;
    rt.lastEmitted = keyIdx - 1;
  }

  private onFrame(state: MP4State, rt: MP4Runtime, frame: VideoFrame): void {
    // Match the frame back to a sample by composition timestamp.
    const t = frame.timestamp / 1_000_000;
    const idx = sampleAtTime(state, t);
    if (idx < 0) {
      frame.close();
      return;
    }
    // Evict a stale frame for the same slot.
    const prev = rt.cache.get(idx);
    if (prev) {
      try {
        prev.close();
      } catch {
        /* ignore */
      }
      rt.cacheOrder = rt.cacheOrder.filter((n) => n !== idx);
    }
    rt.cache.set(idx, frame);
    rt.cacheOrder.push(idx);
    if (idx > rt.lastEmitted) rt.lastEmitted = idx;
    this.evictOld(rt, idx);
  }

  private evictOld(rt: MP4Runtime, keepIdx: number): void {
    // Keep a window of LOOKAHEAD frames around the most recent emission.
    const cutoff = keepIdx - LOOKAHEAD;
    if (cutoff <= 0) return;
    const toRemove = rt.cacheOrder.filter((n) => n < cutoff);
    for (const n of toRemove) {
      const f = rt.cache.get(n);
      if (f) {
        try {
          f.close();
        } catch {
          /* ignore */
        }
        rt.cache.delete(n);
      }
    }
    rt.cacheOrder = rt.cacheOrder.filter((n) => n >= cutoff);
  }

  private clearCache(rt: MP4Runtime): void {
    for (const f of rt.cache.values()) {
      try {
        f.close();
      } catch {
        /* ignore */
      }
    }
    rt.cache.clear();
    rt.cacheOrder = [];
  }

  private failToVideo(rt: MP4Runtime): void {
    rt.failed = true;
    if (rt.decoder) {
      try {
        rt.decoder.close();
      } catch {
        /* ignore */
      }
      rt.decoder = null;
    }
    this.clearCache(rt);
  }

  // --- HTMLVideoElement path (webm, unsupported, or mp4 fallback) ---

  private getVideoFrame(st: EntryState, entry: MediaEntry, sourceTime: number): HTMLCanvasElement | null {
    let video = st.video;
    if (!video) {
      video = document.createElement("video");
      video.src = URL.createObjectURL(entry.file);
      video.muted = true;
      video.playsInline = true;
      video.preload = "auto";
      video.style.display = "none";
      video.addEventListener("loadeddata", () => {
        st.videoReady = true;
      });
      video.addEventListener("seeked", () => {
        st.videoSeeking = false;
      });
      video.addEventListener("error", () => {
        st.videoReady = false;
      });
      document.body.append(video);
      st.video = video;
      st.videoSeekTo = -1;
      st.videoSeeking = false;
    }
    if (!st.videoReady || video.readyState < 2) return null;

    // Seek when we have drifted past the threshold. While a seek is in flight,
    // report no frame so the compositor keeps the previous texture rather than
    // showing a stale one.
    if (Math.abs(video.currentTime - sourceTime) > SEEK_EPSILON) {
      if (st.videoSeekTo !== sourceTime) {
        st.videoSeekTo = sourceTime;
        st.videoSeeking = true;
        try {
          video.currentTime = sourceTime;
        } catch {
          st.videoSeeking = false;
        }
      }
      if (st.videoSeeking) return null;
    }

    // Draw the current frame into a persistent canvas the compositor textures.
    let canvas = st.canvas;
    let ctx = st.canvasCtx;
    if (!canvas || !ctx) {
      canvas = document.createElement("canvas");
      canvas.width = video.videoWidth || entry.width || 320;
      canvas.height = video.videoHeight || entry.height || 180;
      ctx = canvas.getContext("2d");
      if (!ctx) return null;
      st.canvas = canvas;
      st.canvasCtx = ctx;
    }
    try {
      ctx.drawImage(video, 0, 0, canvas.width, canvas.height);
    } catch {
      return null;
    }
    return canvas;
  }
}

/**
 * Map a timeline clip to a media entry and serve its frame at the clip's
 * source-local time. `sourceTime` is already in source-file seconds
 * (clip.inPoint + (t - clip.start) * clip.speed), computed by the compositor.
 */
export function createFrameProvider(decoders: DecoderManager, app: App): FrameProvider {
  return {
    getFrame(clip: Clip, sourceTime: number): VideoFrame | ImageBitmap | HTMLCanvasElement | null {
      // Camera bricks play their most recently recorded take; everything else
      // references a bin entry directly via `clip.source`.
      let entry: MediaEntry | undefined;
      if (clip.type === "camera") {
        const takes = clip.takes;
        if (takes && takes.length > 0) entry = app.media.get(takes[takes.length - 1]!);
      } else if (clip.source) {
        entry = app.media.get(clip.source);
      }
      if (!entry) return null;
      if (entry.kind === "image" || clip.type === "video" || clip.type === "camera") {
        return decoders.getFrame(entry, Math.max(0, sourceTime));
      }
      return null;
    },
  };
}

/**
 * Decode the first keyframe of a demuxed mp4 into an ImageBitmap thumbnail.
 * Returns `undefined` if the decoder is unavailable or decoding fails — callers
 * should fall back to a `<video>` seek + drawImage in that case.
 */
export async function mp4Thumbnail(state: MP4State): Promise<ImageBitmap | undefined> {
  if (state.samples.length === 0 || typeof VideoDecoder === "undefined") return undefined;
  const keyIdx = state.keyframes[0] ?? 0;
  const sample = state.samples[keyIdx];
  if (!sample) return undefined;

  return new Promise<ImageBitmap | undefined>((resolve) => {
    let settled = false;
    const finish = (result: ImageBitmap | undefined) => {
      if (settled) return;
      settled = true;
      if (decoder) {
        try {
          decoder.close();
        } catch {
          /* ignore */
        }
      }
      resolve(result);
    };
    let decoder: VideoDecoder | null = null;
    try {
      decoder = new VideoDecoder({
        output: (frame) => {
          createImageBitmap(frame)
            .then((bmp) => {
              frame.close();
              finish(bmp);
            })
            .catch(() => {
              frame.close();
              finish(undefined);
            });
        },
        error: () => finish(undefined),
      });
    } catch {
      finish(undefined);
      return;
    }
    if (!decoder) {
      finish(undefined);
      return;
    }
    const config: VideoDecoderConfig = {
      codec: state.codec,
      codedWidth: state.width,
      codedHeight: state.height,
    };
    if (state.description) config.description = state.description;
    try {
      decoder.configure(config);
      decoder.decode(
        new EncodedVideoChunk({
          type: sample.is_sync ? "key" : "delta",
          timestamp: Math.round((sample.cts / sample.timescale) * 1_000_000),
          duration: Math.round((sample.duration / sample.timescale) * 1_000_000),
          data: sample.data,
        }),
      );
      void decoder.flush().catch(() => finish(undefined));
    } catch {
      finish(undefined);
    }
  });
}
