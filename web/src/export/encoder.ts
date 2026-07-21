import { ArrayBufferTarget, Muxer } from "mp4-muxer";
import type { App } from "../core/app";
import { Compositor } from "../render/compositor";
import { DecoderManager, createFrameProvider } from "../media/decoder";
import { createMaskProvider } from "../ml/matting";

/**
 * Offline export: renders every frame through the compositor into a WebCodecs
 * VideoEncoder (H.264), mixes all audible clips in an OfflineAudioContext into
 * an AudioEncoder (AAC), muxes to MP4, and downloads the file.
 */
export async function exportProject(app: App, onProgress?: (p: number) => void): Promise<void> {
  const { width, height, fps, duration } = app.project;
  if (duration <= 0) throw new Error("Nothing to export — timeline is empty");
  const totalFrames = Math.ceil(duration * fps);

  const muxer = new Muxer({
    target: new ArrayBufferTarget(),
    video: { codec: "avc", width, height },
    audio: { codec: "aac", sampleRate: 48000, numberOfChannels: 2 },
    fastStart: "in-memory",
  });

  const videoEncoder = new VideoEncoder({
    output: (chunk, meta) => muxer.addVideoChunk(chunk, meta),
    error: (e) => { throw e; },
  });
  videoEncoder.configure({
    codec: "avc1.640028", // High 4.0
    width, height,
    bitrate: 12_000_000,
    framerate: fps,
  });

  // Render frames through a dedicated offscreen compositor.
  const canvas = document.createElement("canvas");
  canvas.width = width;
  canvas.height = height;
  const compositor = new Compositor(canvas, app);
  const decoders = new DecoderManager();
  compositor.setFrameProvider(createFrameProvider(decoders, app));
  compositor.setMaskProvider(createMaskProvider(app));
  // Export renders source-faithful frames regardless of the live playhead.
  for (let f = 0; f < totalFrames; f++) {
    const t = f / fps;
    // Block until each active visual source has produced its frame — offline
    // export prefers waiting over dropping a layer.
    for (const track of app.project.tracks) {
      if (track.muted) continue;
      for (const clip of track.clips) {
        if (clip.type !== "video" && clip.type !== "image" && clip.type !== "camera") continue;
        if (t < clip.start || t >= clip.end || !clip.source) continue;
        const entry = app.media.get(clip.source);
        if (entry) await decoders.awaitFrame(entry, clip.inPoint + (t - clip.start) * clip.speed);
      }
    }
    compositor.renderAt(t);
    const frame = new VideoFrame(canvas, { timestamp: Math.round(t * 1e6), duration: Math.round(1e6 / fps) });
    videoEncoder.encode(frame, { keyFrame: f % (fps * 2) === 0 });
    frame.close();
    if (f % 10 === 0) {
      onProgress?.((f / totalFrames) * 0.8);
      const { promise, resolve } = Promise.withResolvers<void>();
      setTimeout(resolve, 0); // keep UI responsive
      await promise;
    }
  }
  await videoEncoder.flush();

  // Audio mixdown.
  const audioBuffer = await mixAudio(app, 48000);
  if (audioBuffer) {
    const audioEncoder = new AudioEncoder({
      output: (chunk, meta) => muxer.addAudioChunk(chunk, meta),
      error: (e) => { throw e; },
    });
    audioEncoder.configure({ codec: "mp4a.40.2", sampleRate: 48000, numberOfChannels: 2, bitrate: 192_000 });
    const left = audioBuffer.getChannelData(0);
    const right = audioBuffer.getChannelData(1);
    const CHUNK = 48000; // 1s per encode call
    for (let offset = 0; offset < audioBuffer.length; offset += CHUNK) {
      const len = Math.min(CHUNK, audioBuffer.length - offset);
      const interleaved = new Float32Array(len * 2);
      for (let i = 0; i < len; i++) {
        interleaved[i * 2] = left[offset + i]!;
        interleaved[i * 2 + 1] = right[offset + i]!;
      }
      const data = new AudioData({
        format: "f32",
        sampleRate: 48000,
        numberOfFrames: len,
        numberOfChannels: 2,
        timestamp: Math.round((offset / 48000) * 1e6),
        data: interleaved,
      });
      audioEncoder.encode(data);
      data.close();
      onProgress?.(0.8 + (offset / audioBuffer.length) * 0.2);
    }
    await audioEncoder.flush();
  }

  muxer.finalize();
  onProgress?.(1);

  const blob = new Blob([muxer.target.buffer], { type: "video/mp4" });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = "pop-maker-studio-export.mp4";
  a.click();
  URL.revokeObjectURL(a.href);
  app.events.emit("toast", "Export complete");
}

/** Offline mix of every audible clip, honoring volume/pan and clip bounds. */
async function mixAudio(app: App, sampleRate: number): Promise<AudioBuffer | null> {
  const { duration } = app.project;
  const ctx = new OfflineAudioContext(2, Math.ceil(duration * sampleRate), sampleRate);
  let sources = 0;

  for (const track of app.project.tracks) {
    if (track.muted) continue;
    for (const clip of track.clips) {
      if (clip.type !== "audio" && clip.type !== "video" && clip.type !== "camera") continue;
      const entry = clip.source ? app.media.get(clip.source) : undefined;
      if (!entry?.hasAudio) continue;
      try {
        const raw = await entry.file.arrayBuffer();
        const decoded = await ctx.decodeAudioData(raw.slice(0));
        const node = ctx.createBufferSource();
        node.buffer = decoded;
        node.playbackRate.value = clip.speed;
        const gain = ctx.createGain();
        gain.gain.value = clip.props["volume"] ?? 1;
        const pan = ctx.createStereoPanner();
        pan.pan.value = clip.props["pan"] ?? 0;
        node.connect(gain).connect(pan).connect(ctx.destination);
        const sourceOffset = clip.inPoint;
        const clipDur = clip.end - clip.start;
        node.start(clip.start, sourceOffset, clipDur * clip.speed);
        sources++;
      } catch {
        // Undecodable audio in this container — skip rather than fail the export.
      }
    }
  }
  if (sources === 0) return null;
  return ctx.startRendering();
}
