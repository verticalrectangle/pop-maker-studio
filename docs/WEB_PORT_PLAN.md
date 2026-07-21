# Pop Maker Studio — Web Port Plan

Goal: full-parity web version of pop-maker-studio, running entirely in the user's
browser with all ML inference on-device (WebGPU-first, WASM fallback; verified on
AMD RDNA3 via Chrome/Dawn).

## Stack

- **Vanilla TypeScript + Vite** (no framework). Timeline/UI on Canvas 2D + DOM;
  preview compositor on **WebGL2** (GLSL shaders port 1:1 from `shaders/`).
- **Inference:**
  - Whisper transcription: `@huggingface/transformers.js` (ONNX, WebGPU EP) →
    word-level timestamps (`whisper-base`/`small`, timestamp tokens).
  - Background removal: `onnxruntime-web` WebGPU running the repo's own
    `models/rvm_mobilenetv3_fp32.onnx` (RVM, recurrent matting) in a worker.
  - Face/body tracking: `@mediapipe/tasks-vision` FaceLandmarker +
    ImageSegmenter (GPU delegate → WebGL/WebGPU).
  - Beat/RMS: essentia.js WASM (`essentia-wasm.web`) — RhythmExtractor + RMS.
- **Decode:** `mp4box.js` demux → WebCodecs `VideoDecoder` frame cache (the
  "proxy" equivalent); WebM via `jswebm`-style fallback or `VideoDecoder`+demux.
- **Encode/export:** WebCodecs `VideoEncoder` (H.264) + `AudioEncoder` (AAC) →
  `mp4-muxer`. GIF export optional via gif.js.
- **Audio:** WebAudio graph per clip (gain/pan/speed via playbackRate), master
  bus for monitoring; waveform peaks precomputed into `SharedArrayBuffer`.
- **Persistence:** project JSON (`.pms`-compatible schema subset) via File
  System Access API; media originals kept as `File` handles in IndexedDB.

## Architecture

```
web/src/
  main.ts            app bootstrap, layout
  core/
    project.ts       tracks/clips/markers model, .pms JSON schema
    history.ts       undo/redo command stack (mirrors desktop batches)
    keyframes.ts     animatable props, easing (linear/ease-in/out/both/hold)
    events.ts        tiny typed emitter
  ui/
    shell.ts         layout: preview | timeline | inspector | bin
    timeline.ts      canvas timeline: tracks, clips, ruler, markers,
                     drag/trim/split/ripple, snap, marquee select
    inspector.ts     selected-clip props + keyframe editor
    bin.ts           media bin, thumbnails, drag-to-timeline
    transport.ts     play/pause/seek/loop, playhead
  render/
    compositor.ts    WebGL2 render graph: layers bottom-up, transforms
    text.ts          text/lyric layers (canvas2d→texture atlas)
    shapes.ts        shape paths, morph keyframes
    fx.ts            FX brick chain; loads GLSL from web/shaders (ported)
    bodyfx.ts        mask-composited body FX (neon outline, blur, glitch…)
  media/
    decoder.ts       WebCodecs decode pipeline + frame cache
    audio.ts         per-clip sources, master bus, peaks/waveforms
    camera.ts        getUserMedia camera bricks, take recording (MediaRecorder)
    import.ts        file import, thumbnails, stills
  ml/
    worker.ts        inference worker host (WebGPU detect, WASM fallback)
    transcribe.ts    whisper word-level transcript (transformers.js)
    beats.ts         essentia beat/RMS analysis
    matting.ts       RVM background removal (onnxruntime-web + repo ONNX)
    tracking.ts      MediaPipe face landmarker + body segmentation
  features/
    lyrics.ts        transcript → lyric bricks + typography presets
    multicam.ts      camera-cut application
    silence.ts       silence/filler cutting from transcript + RMS
  export/
    encoder.ts       render loop → VideoEncoder/AudioEncoder → mp4-muxer
```

## Feature parity checklist (from MCP_SPEC.md tool surface)

- [x] Multi-track timeline (foreground→background ordering), tracks add/delete/rename/mute/lock
- [x] Clips: add (video/audio/image/text/shape), move, trim, split (multi-point), delete-after, speed retime
- [x] Keyframes: pos/scale/rotation/opacity/volume/pan + FX amounts; shape path morph
- [x] FX bricks: standalone + coupled multi-FX chains; GLSL ported from `shaders/*.glsl`
- [x] Body FX bricks with mask processing (MediaPipe segmentation)
- [x] Chapter markers, loop region, transport
- [x] Bin, thumbnails, contact sheet equivalent (import preview grid)
- [x] ML pipeline: transcribe → word transcript → lyric bricks + presets; re-lay without re-transcribing
- [x] Beat analysis + beat-sync helpers (find_audio_cue equivalent via RMS profile)
- [x] Background removal (RVM), silence removal, filler-word cutting, cut-at-phrase
- [x] Camera record bricks + takes; audio monitor optional (getUserMedia loopback)
- [x] Multicam cuts
- [x] Export MP4 (WebCodecs + mp4-muxer)
- [ ] Virtual mic / system audio routing — **not possible on web** (no PipeWire); document as N/A
- [ ] Blender/iOS-specific exporters — N/A

## Inference performance notes (AMD RX 7700 XT)

- Chrome on Linux: WebGPU ships enabled by default (Dawn→Vulkan/RADV). All
  heavy models run in a dedicated Worker with `powerPreference: high-performance`.
- transformers.js `device: 'webgpu'` + fp16 where supported; fallback chain
  webgpu → wasm (SIMD+threads, COOP/COEP headers required → Vite dev server and
  any static host MUST send `Cross-Origin-Opener-Policy: same-origin` and
  `Cross-Origin-Embedder-Policy: require-corp` for SharedArrayBuffer).
- RVM fp32 ONNX runs ~realtime at 512×288 internal res on RDNA3 WebGPU.

## Deployment

The web app builds to a static `web/dist/` and runs on any static host.
`base: "./"` in `vite.config.ts` emits relative asset paths, so it works
at any subpath (GitHub Pages `/<repo>/web/`, custom domain root, etc.).

### GitHub Pages (default CI target)

`.github/workflows/pages.yml` builds the web app and deploys it alongside
the landing page. GitHub Pages **cannot send custom HTTP headers**, so
`SharedArrayBuffer` is unavailable. The app auto-detects this at runtime
via `self.crossOriginIsolated` and falls back:

- ML inference (transcription, background removal) runs single-threaded
  (WASM `numThreads = 1`). On WebGPU browsers the GPU execution provider
  still runs — only the WASM orchestrator is single-threaded.
- Beat analysis uses its pure-JS fallback path (no essentia.js threads).
- The core editor (timeline, canvas, FX, export) is unaffected.

### Self-hosted / Netlify / Cloudflare Pages / Vercel

Any provider that lets you set response headers can enable
`SharedArrayBuffer` for threaded WASM. Set on the HTML and asset responses:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

The Vite dev server already sends these (see `crossOriginIsolation` plugin
in `vite.config.ts`). The app auto-detects `crossOriginIsolated` and
enables `numThreads` up to 4 — no code changes or rebuild needed.

**COEP caveat:** `require-corp` enforces CORS on every subresource.
MediaPipe wasm/models (`cdn.jsdelivr.net`, `storage.googleapis.com` in
`ml/tracking.ts`) send `Access-Control-Allow-Origin: *` and work fine.
If you add a cross-origin asset that doesn't send CORS headers, it will
be blocked — switch COEP to `credentialless` or proxy the asset.

## Milestones

1. **M1 Core:** scaffold, project model, undo, timeline editing, WebGL compositor
   with video/audio/text, playback. Export MP4 of a simple cut.
2. **M2 FX:** FX brick system + ported GLSL, keyframes, shapes, markers.
3. **M3 ML:** transcription + lyrics, beats, RVM removal, face/body tracking FX.
4. **M4 Features:** camera bricks, multicam, silence/filler tools, polish.
