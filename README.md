# Pop Maker Studio

![BLUE-GRAY lyric on eye close-up](docs/screenshot1.png) ![Glitch effect with timeline](docs/screenshot2.png) ![FLOWED lyric over ocean with FX panel](docs/screenshot3.png)

A native C++ music video and lyric video editor built for pop artists. No subscription. No cloud. No Python runtime. Everything — transcription, vocal separation, voice conversion, background removal, GPU effects — runs entirely on your machine.

---

## What it is

Pop Maker Studio is a non-linear editor purpose-built for the music video workflow: drop your track, get word-level lyrics in seconds, apply visual effects, and render to MP4. It is not a general-purpose editor with music features bolted on. Every decision — the three-tier preview pipeline, the subtitle animation engine, the audio master clock, the glass FX system, the project bin — was made specifically for this use case.

The application is a single binary with no runtime dependencies beyond what ships with a standard Linux desktop. No Electron. No Python. No Node. No framework. Dear ImGui running on OpenGL, doing exactly what it was designed to do.

---

## ML pipeline

The ML stack runs fully locally. Nothing is uploaded.

**Vocal separation** uses Kim_Vocal_2, a battle-tested MDX-Net model from the UVR5 community (~64 MB). The STFT (FFTW3, n_fft=6144, hop=1024), chunked ONNX inference with 25% frame overlap, iSTFT, and instrumental extraction (`original − vocals`) are implemented in C++ with ONNX Runtime. No Python. No GPU required.

**Transcription** uses whisper.cpp (`large-v3-turbo-q5_0`, ~584 MB) with DTW token timestamps enabled via `WHISPER_AHEADS_LARGE_V3_TURBO`. BPE tokens are grouped into words by detecting leading spaces in the whisper token stream. A vocal-presence gate (RMS-energy stretch ≥250 ms) skips Whisper entirely on windows where MDX-Net produced a dead stem, so `[Music]` / `♪♪` tokens never pollute the cached transcript.

**CTC forced alignment** refines Whisper timestamps to frame-accurate precision. A stay/advance trellis decoder (handrolled in C++, torchaudio forced-alignment algorithm) runs wav2vec2-base-960h (Xenova ONNX quantized, ~94 MB) per Whisper segment. The (T+1)×(L+1) trellis is built in log-prob space; backtracking yields one character span per target token, which are merged into word timestamps. Word timestamps are snapped to MJPEG proxy frame boundaries so karaoke highlighting lands on exact video frames.

**Background removal** uses u2net_human_seg via ONNX. Each frame is bilinear-resized to 320×320 for inference, the output mask is bilinear-resized back to the original frame resolution, and a separable Gaussian blur (radius ~1px) smooths mask edges. Masks are streamed as grayscale MJPEG so the canvas updates in real time while the model processes.

**Voice conversion** runs entirely in C++ with zero Python involvement. The pipeline reads PyTorch `.pth` model files directly without libtorch: the zip archive is extracted with the system `unzip`, then a hand-rolled pickle VM parses `data.pkl` to extract tensor metadata and model configuration, and exports a fully functional ONNX graph using a hand-rolled protobuf serializer. The VITS architecture (TextEncoder → ResidualCouplingBlock reverse flow → NSF-HiFiGAN decoder) is reconstructed entirely in C++. HuBERT embeddings use a shared ONNX model. `.pth` in, voice-converted audio out, no Python interpreter ever started.

---

## Video preview

Three tiers, picked per-slot, transparently upgraded as media becomes available:

**Tier 1 — Native libav decode.** When a clip is added, the source file is opened directly with libav and the first frame shows immediately. HW decode is auto-attached when available (`-hwaccel auto` walks VAAPI → CUDA → VDPAU → VideoToolbox); software fallback runs at `-threads 2` per slot. Sequential decode skips `av_seek_frame + avcodec_flush_buffers` when the next requested frame is the natural forward continuation — for h264 with a 2–5 s GOP that's the difference between decoding 1 frame vs. ~150 per scrub step.

**Tier 2 — MJPEG proxy.** In parallel with native decode, ffmpeg transcodes a quarter-resolution MJPEG with a binary frame-offset table. Scrubbing on the proxy is `fseek + libjpeg-turbo decode + GPU upload` — measurably cheaper than HW-decoding the original even on a fast GPU, so the per-slot scan loop swaps to proxy the instant transcode finishes. The transcoder runs `min(4, cores/2)` workers in parallel with HW-accelerated decode and a fast-bilinear scaler, so a 20-clip import drops from ~60–90 s to ~10–20 s.

**Tier 3 — Single-frame still.** Fallback when libav can't open the file. Shown by `proxy_ensure_still`, generated once and cached on disk.

Every slot keeps an 8-frame decoded ring. The canvas pre-walk dispatches parallel JPEG decodes for the active clip per track plus a 3-frame boundary warm into upcoming / previous clips when the playhead is within 1 s of a cut, so scrubbing across clip boundaries hits a populated ring instead of a sync decode on the main thread. The session-level `proxy_is_ready` cache eliminates the per-frame stat-syscall flood the timeline draw loop used to make (one call per visible video clip per frame).

---

## Effects engine

The GPU effects pipeline runs GLSL fragment shaders on every frame, compositing clip layers into an offscreen FBO before piping pixels to ffmpeg.

The **glass FX system** lets effect bricks apply pre-composite to a single clip — before it's blended with the rest of the scene — or post-composite to the entire composited frame, depending purely on track position. No mode switch. No configuration. Drag a Glitch brick above a video clip and it applies to that clip only. Drag it to a separate track and it hits the full frame.

100 effects are defined in a JSON registry and generated at build time: GLSL shader strings, accumulation structs, inspector UI, serialization, project versioning, and MCP tool descriptions — all emitted by a single codegen script (`tools/codegen_effects.py`). Adding a new effect is writing a shader body and a JSON entry.

**Runtime effects** can be dropped into the `effects/` directory as `.json` + `.glsl` pairs and hot-reloaded within one frame — no rebuild required.

---

## Text rendering

Lyric and subtitle clips support per-clip visual styling: shadow (offset, color), stroke (width, color), glow (multi-pass radial bloom, radius, color), and background box (color, padding, corner radius). All layers are rendered in order — glow → background → shadow → stroke → text — via a shared `text_renderer` module used by both the canvas preview and the export renderer, guaranteeing pixel-exact correspondence.

Typography presets wire directly into the text style system: the neon preset enables hot-pink glow; cyberpunk enables cyan stroke.

---

## Subtitle system

Eight animation styles (Fade, Glitch, Typewriter, Bounce, Scale, Slide, Stack, Block) with five grouping modes (Word, Phrase, Line, Segment, Custom N). Font size is stored as a fraction of canvas height, not pixels, so the preview and a 1920×1080 export are geometrically identical.

---

## A/V sync

The playhead is driven by the audio callback position, corrected for output buffer latency:

```
playhead = audio_position() - audio_latency()
```

The audio clock advances unconditionally. The video follows it. This gives lip-sync quality synchronization at any scrub position without polling a wall clock.

---

## Project bin

Adding a media file and placing it on the timeline are two different things. The bin is the project-scoped media library shown in the right-side panel; it holds every file you've added to the project, with thumbnail, duration, and a usage counter. Multi-file drops land in the bin without auto-placement so dropping 5 clips doesn't stack 5 overlapping tracks at the playhead. Single-file drops on a specific track still place directly (and mirror into the bin so the panel reflects everything in the project). Drag from the bin to a track when you're ready to commit a placement.

The bin is persisted in the project file. Pre-bin projects backfill from existing clip paths on load so older saves still show their media.

---

## MCP server

Pop Maker Studio exposes its full editing surface to Claude via the Model Context Protocol. The app runs a Unix socket IPC server on startup; a Python MCP bridge (`mcp_server/server.py`) reads the lock file, connects to the socket, and registers ~70 tools covering clip creation and manipulation, text style, typography generation, ML pipeline control, effect application, audio analysis and cue detection, lyric/moment search, playback, project persistence, and bin management.

**Async-first.** Long-running tools return immediately and surface state via polling. `trigger_pipeline` returns `stage="running"` and is followed by `get_pipeline_status` until `stage="done"`. `analyze_audio`, `remove_background`, `find_and_add_clip` follow the same pattern. The MCP server doesn't tie up the chat waiting on ML work.

**Auto-batching.** Single mutations are wrapped in an implicit one-call batch labelled with the method name, so a one-off `set_clip_prop` is one undo step automatically. `begin_batch`/`end_batch` is only needed when you want a sequence of mutations to undo as a single step (e.g. `add_track` + `add_clip` + `set_clip_prop` setting up a clip from scratch).

**Lyrics search without full transcription.** `find_and_add_clip(path, query)` runs windowed MDX-Net + Whisper, stops at the first hit, and auto-extracts a segment around the match — orders of magnitude faster than transcribing a whole song just to locate a phrase.

### Setup

```bash
pip install -r mcp_server/requirements.txt
```

Start Pop Maker Studio first (the MCP server reads `/tmp/pop-maker-studio.lock` to find the app's socket), then either run the bridge standalone:

```bash
python3 mcp_server/server.py
```

or wire it into Claude. **Claude Desktop** (`~/.config/claude/claude_desktop_config.json` on Linux, `~/Library/Application Support/Claude/claude_desktop_config.json` on macOS):

```json
{
  "mcpServers": {
    "pop-maker-studio": {
      "command": "python3",
      "args": ["/absolute/path/to/pop-maker-studio/mcp_server/server.py"]
    }
  }
}
```

**Claude Code** (`.mcp.json` in the repo root):

```json
{
  "mcpServers": {
    "pop-maker-studio": {
      "command": "python3",
      "args": ["mcp_server/server.py"]
    }
  }
}
```

The wire protocol and per-tool semantics are in `MCP_SPEC.md`. The tool descriptions themselves live in `mcp_server/server.py` and are the canonical reference — anything written separately rots the moment a tool changes.

---

## Embedded terminal

A real PTY-backed terminal lives in a draggable strip below the timeline. It runs your login shell (vterm parses escape codes; mouse selection, right-click copy/paste, double-wide character handling, and live resize all work). Drops onto the terminal panel inject the file path at the prompt instead of touching the timeline — useful for running an ad-hoc ffmpeg or ffprobe on a file without leaving the app.

---

## Export

Export uses the same OpenGL pipeline as the preview. Every frame rendered to the offscreen FBO is pixel-identical to what the preview showed. Raw RGBA frames are piped to ffmpeg for H.264/AAC encoding. The export path is not a separate renderer — it is the live renderer, pointed at a framebuffer instead of the screen.

Three output formats: vertical (9:16, 1080×1920 — TikTok / Reels / Shorts), horizontal (16:9, 1920×1080 — YouTube), square (1:1, 1080×1080 — Instagram).

**Examples** (9:16 TikTok vertical, rendered in Pop Maker Studio):

<div align="center"><table><tr>
<td valign="top"><img src="docs/export_example1.gif" width="284"><br><img src="docs/export_example2.gif" width="284"></td>
<td valign="middle"><img src="docs/export_example.gif" width="270"></td>
</tr></table></div>

---

## Platform

Primary development target is Linux. The GitHub Actions release workflow builds on Ubuntu 22.04, downloads all models (Whisper, Kim_Vocal_2, u2net, HuBERT, wav2vec2, Piper voices), and packages a self-contained tarball.

---

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Dependencies: OpenGL, GLFW, FFmpeg (avcodec/avformat/avutil/swresample/swscale), libjpeg-turbo, FreeType, aubio, vterm, ONNX Runtime, whisper.cpp, fftw3f.

After modifying `effects/registry.json`, regenerate the codegen headers:

```bash
python3 tools/codegen_effects.py
```
