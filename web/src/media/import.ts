// Media import: probe metadata for dropped files and register them in the
// project's MediaStore. Three probing paths:
//   image  -> createImageBitmap (dims + the bitmap doubles as thumbnail)
//   audio  -> AudioContext.decodeAudioData (duration + 1024-bucket peaks +
//             per-second RMS)
//   video  -> mp4/mov: mp4box demux + WebCodecs VideoDecoder thumbnail
//             webm/other: hidden <video> element metadata + seek+drawImage
// For every file with an audio track we also try decodeAudioData to populate
// peaks/rms (cheap for typical clip sizes).
import { demuxMp4, mp4Thumbnail } from "./decoder";
import type { App } from "../core/app";
import type { MediaEntry, MediaKind } from "./store";

const PEAK_BUCKETS = 1024;

function detectKind(file: File): MediaKind | null {
  const mime = file.type.toLowerCase();
  if (mime.startsWith("image/")) return "image";
  if (mime.startsWith("audio/")) return "audio";
  if (mime.startsWith("video/")) return "video";
  const n = file.name.toLowerCase();
  if (/\.(png|jpe?g|gif|webp|bmp|avif|svg)$/.test(n)) return "image";
  if (/\.(mp3|wav|ogg|flac|m4a|aac|opus|weba)$/.test(n)) return "audio";
  if (/\.(mp4|mov|m4v|webm|mkv|avi|ogv)$/.test(n)) return "video";
  return null;
}

function isMp4Name(name: string): boolean {
  const n = name.toLowerCase();
  return n.endsWith(".mp4") || n.endsWith(".mov") || n.endsWith(".m4v");
}

/** Mix all channels to mono. */
function toMono(buffer: AudioBuffer): Float32Array {
  const channels = buffer.numberOfChannels;
  const len = buffer.length;
  if (channels === 1) return buffer.getChannelData(0).slice();
  const mono = new Float32Array(len);
  for (let c = 0; c < channels; c++) {
    const data = buffer.getChannelData(c);
    for (let i = 0; i < len; i++) mono[i] = (mono[i] ?? 0) + data[i]! / channels;
  }
  return mono;
}

/** ~1024 normalized (0..1) peak buckets. */
function computePeaks(buffer: AudioBuffer): Float32Array {
  const mono = toMono(buffer);
  let globalMax = 0;
  for (let i = 0; i < mono.length; i++) {
    const a = Math.abs(mono[i]!);
    if (a > globalMax) globalMax = a;
  }
  const peaks = new Float32Array(PEAK_BUCKETS);
  const per = mono.length / PEAK_BUCKETS;
  for (let b = 0; b < PEAK_BUCKETS; b++) {
    const start = Math.floor(b * per);
    const end = Math.floor((b + 1) * per);
    let max = 0;
    for (let i = start; i < end; i++) {
      const a = Math.abs(mono[i]!);
      if (a > max) max = a;
    }
    peaks[b] = globalMax > 0 ? max / globalMax : 0;
  }
  return peaks;
}

/** Per-second RMS amplitude (0..1, raw — not normalized). */
function computeRms(buffer: AudioBuffer): Float32Array {
  const rate = buffer.sampleRate;
  const channels = buffer.numberOfChannels;
  const len = buffer.length;
  const seconds = Math.max(1, Math.ceil(buffer.duration));
  const rms = new Float32Array(seconds);
  const channelData: Float32Array[] = [];
  for (let c = 0; c < channels; c++) channelData.push(buffer.getChannelData(c));
  for (let s = 0; s < seconds; s++) {
    const start = Math.floor(s * rate);
    const end = Math.min(len, Math.floor((s + 1) * rate));
    let sum = 0;
    let n = 0;
    for (let i = start; i < end; i++) {
      let v = 0;
      for (let c = 0; c < channels; c++) v += channelData[c]![i] ?? 0;
      v /= channels;
      sum += v * v;
      n++;
    }
    rms[s] = n > 0 ? Math.sqrt(sum / n) : 0;
  }
  return rms;
}

/** Decode a file's audio track via decodeAudioData; returns duration/peaks/rms
 * or null when the file has no decodable audio. */
async function probeAudioData(
  file: File,
): Promise<{ duration: number; peaks: Float32Array; rms: Float32Array } | null> {
  try {
    const buf = await file.arrayBuffer();
    const ctx = new AudioContext();
    try {
      const decoded = await ctx.decodeAudioData(buf.slice(0));
      return {
        duration: decoded.duration,
        peaks: computePeaks(decoded),
        rms: computeRms(decoded),
      };
    } finally {
      void ctx.close();
    }
  } catch {
    return null;
  }
}

/** Grab a thumbnail by seeking a hidden <video> and drawing the frame. */
function videoElementThumbnail(file: File, duration: number): Promise<ImageBitmap | undefined> {
  return new Promise((resolve) => {
    const video = document.createElement("video");
    video.muted = true;
    video.playsInline = true;
    video.preload = "metadata";
    video.style.display = "none";
    const url = URL.createObjectURL(file);
    let settled = false;
    const cleanup = (result: ImageBitmap | undefined) => {
      if (settled) return;
      settled = true;
      URL.revokeObjectURL(url);
      video.remove();
      video.src = "";
      resolve(result);
    };
    video.addEventListener("error", () => cleanup(undefined));
    video.addEventListener("loadeddata", () => {
      const target = Math.min(0.1, Math.max(0, (duration || video.duration) / 2));
      const onSeeked = () => {
        const canvas = document.createElement("canvas");
        canvas.width = video.videoWidth || 320;
        canvas.height = video.videoHeight || 180;
        const ctx2d = canvas.getContext("2d");
        if (!ctx2d) {
          cleanup(undefined);
          return;
        }
        try {
          ctx2d.drawImage(video, 0, 0, canvas.width, canvas.height);
          void createImageBitmap(canvas).then((bmp) => cleanup(bmp)).catch(() => cleanup(undefined));
        } catch {
          cleanup(undefined);
        }
      };
      video.addEventListener("seeked", onSeeked, { once: true });
      try {
        video.currentTime = target;
      } catch {
        cleanup(undefined);
      }
    });
    video.src = url;
    document.body.append(video);
    // Hard timeout: never block import on a flaky thumbnail.
    setTimeout(() => cleanup(undefined), 4000);
  });
}

/** Probe a video file's metadata + thumbnail via a hidden <video> element. */
async function probeVideoViaElement(file: File): Promise<Partial<MediaEntry>> {
  const meta = await new Promise<{ duration: number; width: number; height: number } | null>((resolve) => {
    const video = document.createElement("video");
    video.muted = true;
    video.preload = "metadata";
    video.style.display = "none";
    const url = URL.createObjectURL(file);
    let settled = false;
    const finish = (r: { duration: number; width: number; height: number } | null) => {
      if (settled) return;
      settled = true;
      URL.revokeObjectURL(url);
      video.remove();
      video.src = "";
      resolve(r);
    };
    video.addEventListener("loadedmetadata", () => {
      finish({
        duration: video.duration || 0,
        width: video.videoWidth || 0,
        height: video.videoHeight || 0,
      });
    });
    video.addEventListener("error", () => finish(null));
    video.src = url;
    document.body.append(video);
    setTimeout(() => finish(null), 4000);
  });
  const duration = meta?.duration ?? 0;
  const width = meta?.width ?? 0;
  const height = meta?.height ?? 0;
  const thumbnail = await videoElementThumbnail(file, duration);
  return { duration, width, height, fps: 0, thumbnail };
}

async function probeImage(file: File): Promise<Partial<MediaEntry>> {
  const bitmap = await createImageBitmap(file);
  return {
    duration: 0,
    width: bitmap.width,
    height: bitmap.height,
    fps: 0,
    hasAudio: false,
    hasVideo: true,
    thumbnail: bitmap,
  };
}

async function probeAudio(file: File): Promise<Partial<MediaEntry>> {
  const audio = await probeAudioData(file);
  return {
    duration: audio?.duration ?? 0,
    width: 0,
    height: 0,
    fps: 0,
    hasAudio: true,
    hasVideo: false,
    peaks: audio?.peaks,
    rms: audio?.rms,
  };
}

/** Probe one file and add it to the store. Skips unsupported files silently. */
async function importOne(app: App, file: File): Promise<MediaEntry | null> {
  const kind = detectKind(file);
  if (!kind) return null;

  let partial: Partial<MediaEntry>;
  try {
    if (kind === "image") {
      partial = await probeImage(file);
    } else if (kind === "audio") {
      partial = await probeAudio(file);
    } else if (isMp4Name(file.name)) {
      // mp4/mov: demux for accurate dims/fps, VideoDecoder thumbnail, with
      // element-based fallback if demuxing or decoding fails.
      try {
        const state = await demuxMp4(file);
        const thumb = (await mp4Thumbnail(state)) ?? (await videoElementThumbnail(file, state.duration));
        const audio = await probeAudioData(file);
        partial = {
          duration: state.duration,
          width: state.width,
          height: state.height,
          fps: state.fps,
          hasAudio: !!audio,
          hasVideo: true,
          thumbnail: thumb,
          peaks: audio?.peaks,
          rms: audio?.rms,
        };
      } catch {
        const fb = await probeVideoViaElement(file);
        const audio = await probeAudioData(file);
        partial = {
          ...fb,
          hasAudio: !!audio,
          hasVideo: true,
          peaks: audio?.peaks,
          rms: audio?.rms,
        };
      }
    } else {
      const fb = await probeVideoViaElement(file);
      const audio = await probeAudioData(file);
      partial = {
        ...fb,
        hasAudio: !!audio,
        hasVideo: true,
        peaks: audio?.peaks,
        rms: audio?.rms,
      };
    }
  } catch {
    return null;
  }

  const entry = app.media.add({
    file,
    kind,
    duration: partial.duration ?? 0,
    width: partial.width ?? 0,
    height: partial.height ?? 0,
    fps: partial.fps ?? 0,
    hasAudio: partial.hasAudio ?? false,
    hasVideo: partial.hasVideo ?? false,
    thumbnail: partial.thumbnail,
    peaks: partial.peaks,
    rms: partial.rms,
  });
  app.events.emit("media:added", entry);
  return entry;
}

/** Import a batch of dropped files into the project's media store. */
export async function importFiles(app: App, files: File[]): Promise<void> {
  await Promise.all(files.map((f) => importOne(app, f).catch(() => null)));
}
