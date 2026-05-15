# Pop Maker Studio

A native C++ music video and lyric video editor built for pop artists. No subscription. No cloud. No Python runtime. Everything — transcription, vocal separation, voice conversion, background removal, GPU effects — runs entirely on your machine.

---

## What it is

Pop Maker Studio is a non-linear editor purpose-built for the music video workflow: drop your track, get word-level lyrics in seconds, apply visual effects, and render to MP4. It is not a general-purpose editor with music features bolted on. Every decision — the proxy system, the subtitle animation engine, the audio master clock, the glass FX system — was made specifically for this use case.

The application is a single binary with no runtime dependencies beyond what ships with a standard Linux desktop. No Electron. No Python. No Node. No framework. Dear ImGui running on OpenGL, doing exactly what it was designed to do.

---

## ML pipeline

The ML stack runs fully locally. Nothing is uploaded.

**Vocal separation** uses HTDemucs v4 via ONNX Runtime. The audio processing pipeline — STFT, iSTFT, overlap-add segmentation with 1-second linear cross-fades — is implemented from scratch in C++, including a hand-rolled Cooley-Tukey radix-2 DIT FFT. No external DSP library. GPU acceleration via the CUDA execution provider when available; silent CPU fallback otherwise.

**Transcription** uses whisper.cpp (`large-v3-turbo-q5_0`, ~584 MB) with DTW token timestamps for word-level alignment. No external forced aligner. The C++ layer handles everything: subprocess management, JSON output, timeline placement.

**Background removal** uses u2net_human_seg via rembg with 2× supersampling, Lanczos downsampling, and σ=0.7 Gaussian smoothing for edge quality. Masks are streamed as grayscale MJPEG so the canvas updates in real time while the model processes.

**Voice conversion** runs entirely in C++ with zero Python involvement. The pipeline reads PyTorch `.pth` model files directly — parsing the zip+pickle format without libtorch — extracts tensor metadata and model configuration, and exports a fully functional ONNX graph using a hand-rolled protobuf serializer. The VITS architecture (TextEncoder → ResidualCouplingBlock reverse flow → NSF-HiFiGAN decoder) is reconstructed entirely in C++: relative multi-head attention with correct `_rel_to_abs` semantics, weight norm resolution at export time, dynamic sequence length throughout, SineGen as a deterministic closed-form CumSum+Mod+Sin graph. HuBERT embeddings use a shared ONNX model; inference runs via ONNX Runtime. The result: `.pth` in, voice-converted audio out, no Python interpreter ever started.

---

## Effects engine

The GPU effects pipeline runs GLSL fragment shaders on every frame, compositing clip layers into a 9:16 offscreen FBO before piping pixels to ffmpeg.

The **glass FX system** lets effect bricks apply pre-composite to a single clip — before it's blended with the rest of the scene — or post-composite to the entire composited frame, depending purely on track position. No mode switch. No configuration. Drag a Glitch brick above a video clip and it applies to that clip only. Drag it to a separate track and it hits the full frame. The same brick, different behavior, zero friction.

Effects beyond the hand-wired core (Glitch, VHS, Datamosh, ZoomPunch, LightLeak, ChromaKey) are defined in a JSON registry and generated at build time: GLSL shader strings, accumulation structs, inspector UI, serialization, project versioning — all emitted by a single codegen script. Adding a new effect is writing a shader body and a JSON entry.

---

## A/V sync

The playhead is driven by the audio callback position, corrected for output buffer latency:

```
playhead = audio_position() - audio_latency()
```

The audio clock advances unconditionally. The video follows it. This gives lip-sync quality synchronization at any scrub position without polling a wall clock. It is the difference between a video editor and a slideshow with sound.

---

## Video preview

High-bitrate source footage is transcoded to a per-clip MJPEG proxy at a lower resolution. Scrubbing seeks to the correct proxy frame via a prebuilt seek table — essentially a direct fseek to the right JPEG — which makes real-time preview instant regardless of source codec. The proxy system assigns each clip its own decoder slot keyed by source path and clip start time, so splitting a clip gives each half independent decoders with no seek contention.

---

## Subtitle system

Eight animation styles (Fade, Glitch, Typewriter, Bounce, Scale, Slide, Stack, Block) with five grouping modes (Word, Phrase, Line, Segment, Custom N). Font size is stored as a fraction of canvas height, not pixels, so the preview and a 1920×1080 export are geometrically identical — no "it looked different on render" surprises. Word wrapping uses the same font metric as the export path.

---

## Export

Export uses the same OpenGL pipeline as the preview. Every frame rendered to the offscreen FBO is pixel-identical to what the preview showed. Raw RGBA frames are piped to ffmpeg for H.264/AAC encoding. The export path is not a separate renderer — it is the live renderer, pointed at a framebuffer instead of the screen.

---

## Platform

Primary development target is Linux. macOS and Windows builds are configured in GitHub Actions (macos-14 and windows-2022 with vcpkg).

---

## Building

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Dependencies: OpenGL, GLFW, FFmpeg (avcodec/avformat/avutil/swresample/swscale), FreeType, aubio, ONNX Runtime, whisper.cpp, fftw3f.
