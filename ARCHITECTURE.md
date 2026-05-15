# Pop Maker Studio — Architecture

Developer reference for the major structural decisions, data flow, and non-obvious invariants. Read this before touching anything in `src/`.

---

## The single-struct model

Everything lives in `AppState` (`src/app.h`). There is no event system, no observer pattern, no reactive bindings. Every frame the entire UI is re-derived from `AppState` and drawn from scratch.

This is Dear ImGui's immediate-mode model taken seriously. The consequences are all good: undo/redo is a `memcpy`-equivalent snapshot-and-restore, serialization covers the entire application state by construction, and debugging means printing the struct. `AppState` is large (~100 fields) but deliberately flat. Nesting it into sub-objects creates partial-snapshot problems for history — resist the urge.

The main loop:

```
app_frame(state)
  ├── update playhead (audio position or wall clock fallback)
  ├── render_tick_gl(state)         — advance export by one frame if running
  └── ui_studio(state)
        ├── draw_timeline()
        ├── draw_preview()          — proxy decode → fx_apply → scene composite
        └── draw_panel_*()
```

---

## Clip data model

Every piece of content on the timeline is a `Clip`. Clips are typed (`ClipType::Video`, `Audio`, `Text`, `Lyrics`, `Subtitle`, `Effect`) but live in the same struct. Tracks are named containers.

**Source time vs timeline time** is the single most important distinction in the codebase:

```
source_time = clip.in_point + (timeline_time - clip.start) * clip.speed
```

Any code that reads a video frame or audio sample must use `source_time`. Confusing the two caused the split-clip black-frame and wrong-audio bugs. Every decoder call site has been audited.

Keyframe times (`Keyframe.time`) are relative to `clip.start`, not absolute timeline time. `Clip::eval_prop(name, playhead)` handles the offset — callers pass absolute playhead time and don't need to think about it.

---

## Audio pipeline and A/V sync

`g_read_pos` is a 64-bit sample counter that advances by `frameCount * 2` every audio callback invocation, unconditionally. It is the **timeline clock**, not a source read cursor. This distinction matters.

The playhead is:
```
pos = audio_position() - audio_latency()
```

Wall clock is the fallback only when no audio is loaded. The latency correction accounts for the output buffer depth so the displayed frame matches the audible audio rather than the samples already buffered ahead.

The callback mixes three sources: video-embedded audio decoded to a flat PCM buffer, timeline Audio clips each with their own PCM buffer, and silence. It runs on a separate thread and uses snapshots pushed by the main thread each frame (`video_audio_clips_update`, `audio_clips_update`). The callback uses `std::try_to_lock` — one ~10ms dropout on lock contention is inaudible; deadlock would not be.

---

## Video proxy system

Decoding H.264/HEVC in real time on the main thread is too slow for interactive scrubbing. Each video clip gets a proxy: a folder of MJPEG frames at a lower resolution with a prebuilt seek table (fseek + stb_image). Scrubbing is essentially a direct fseek to a JPEG.

Each clip gets its own decoder slot (0–7), keyed by:
```
source_path + '\x01' + clip_start_time
```

Not just the source path. Two clips from the same file get independent slots so their decoders don't fight over seek position. This is why `proxy_paths[MAX_VIDEO_TRACKS]` is not serialized — it is rebuilt at runtime from the live clip list.

The proxy poll loop runs every frame and manages the slot state machine: slot assigned and proxy ready → open proxy; slot assigned and proxy still generating → open a JPEG still as placeholder. `gc_video_slots` frees slots whose clips were deleted.

**One invariant**: `fps` in the `ProxyInfo` struct must be probed from the original source file, not the proxy. The proxy is always 12 fps; the source FPS determines the frame count table.

---

## GPU FX pipeline

All visual effects are GLSL fragment shaders. The pipeline distinguishes two application contexts:

**Glass pass (pre-composite)**: an FX brick is glass when the track directly below it has an active Video clip at that time. Glass FX apply to the single clip's decoded texture before it's composited with the rest of the scene. The check is purely positional — no mode flag, no configuration.

**Global pass (post-composite)**: an FX brick on a track with no video below it applies to the full composited 9:16 frame after all clips have been merged.

`collect_effects` and `collect_creative_fx` skip glass tracks. `collect_glass_effects` and `collect_glass_fx` read only the one track directly above the target video clip. A given FX brick is collected by exactly one path — never both.

The four accumulator functions:
- `collect_effects(state, t, below_track_idx)` — global adjustments (grade, blur, vignette)
- `collect_creative_fx(state, t, below_track_idx)` — global creative FX
- `collect_glass_effects(state, t, video_track_idx)` — glass adjustments
- `collect_glass_fx(state, t, video_track_idx)` — glass creative FX

**fx_apply** runs active passes in order: grade+vignette → blur (2-pass Gaussian) → chroma-key → glitch → VHS → light-leak → datamosh → generated effects. The `slot` parameter (0–15) identifies which stable per-slot output texture to write to. Because ImDrawList defers rendering until end-of-frame, each slot must have its own stable output texture — a shared ping-pong buffer would be overwritten before the draw commands execute.

**Scene compositor**: `scene_begin` clears an offscreen FBO; `scene_add_layer` alpha-composites each clip using a custom GLSL shader that maps canvas-space fragment positions to clip-local UV coordinates handling arbitrary rotation. `scene_apply_fx` runs global FX on the composited result once. The FBO uses straight alpha with bottom-left origin; ImGui is top-left. The blit quad uses Y-flipped UVs `{0,1},{1,1},{1,0},{0,0}` to compensate.

**Datamosh ghost**: persistent between frames, keyed by `clip_start`. The ghost resets when the clip changes. Two-pass: output `blend(current, ghost)`, then update `mix(ghost, current, decay)` — reads old ghost before overwriting.

---

## Codegen effects

Effects beyond the hand-wired core are defined in `effects/registry.json` and generated at build time by `tools/codegen_effects.py`. The script outputs ~15 headers into `src/generated/`:

`fx_accum_fields.h`, `fx_clip_fields.h`, `fx_collect_cases.h`, `fx_enum_entries.h`, `fx_shader_strings.h`, `fx_shader_init.h`, `fx_shader_apply.h`, `fx_project_read.h`, `fx_project_write.h`, `fx_ui_inspector.h`, `fx_ui_picker.h`, `fx_preview_defaults.h`, `fx_type_list.h`, `fx_ui_color.h`, `fx_ui_label.h`

Every entry in `registry.json` specifies: `id`, `name`, `category`, `params[]` (with name, min, max, default, curve), and a GLSL shader body. Adding a new effect means writing a shader body and a JSON entry — no manual edits to C++.

Power curves (`"curve": 0.5`) map slider travel to perceptual values at accumulation time. Every generated effect has a system-level `amount` (0–1) that blends source and effect output — this is not a param, it is the top-level intensity control.

Do not edit files in `src/generated/`. Re-run `tools/codegen_effects.py` after modifying `effects/registry.json`.

---

## ML pipeline

### MDX-Net vocal separation

Model: Kim_Vocal_2.onnx (~64 MB, UVR5 community). Auto-downloaded to `~/.cache/pop-maker-studio/mdx/` on first use.

Pipeline: ffmpeg decode → STFT (FFTW3, n_fft=6144, hop=1024) → chunked ONNX inference (256-frame chunks, 64-frame overlap) → iSTFT → instrumental as `original − vocals`. All C++, ONNX Runtime CPU. No GPU required.

If the model hasn't been downloaded, separation fails with a clear error pointing to the Setup screen. No fallback.

### Whisper transcription

whisper.cpp with `ggml-large-v3-turbo-q5_0`. DTW token timestamps (`cparams.dtw_token_timestamps = true` + `WHISPER_AHEADS_LARGE_V3_TURBO`) align each BPE token to the audio using attention heads. Token → word grouping: tokens that start with a space mark word boundaries. No external forced aligner.

### Voice conversion — zero Python

The voice conversion pipeline reads PyTorch `.pth` checkpoints directly: parse the zip archive, execute the pickle protocol-2 VM, reconstruct tensor metadata and model configuration. No libtorch. No Python.

From metadata alone, `pth_to_onnx()` builds a complete ONNX graph using a hand-rolled protobuf serializer. The VITS architecture is reconstructed in C++:

- `enc_p` (6-layer TextEncoder with relative multi-head attention and correct `_rel_to_abs` — dynamic T, 5-step implementation)
- `flow_reverse` (4 ResidualCouplingBlock layers in reverse order with channel flip between each; layer count and WN `n_layers` derived from weight shapes, not hardcoded)
- `dec` (NSF-HiFiGAN decoder with SineGen as a deterministic CumSum+Mod+Sin graph, f0 repeat_interleave via Expand+Reshape, ConvTranspose upsample stages with per-stage resblocks and noise convolutions)

Weight normalization is resolved at export time: `W_eff[i] = weight_g[i] / ||weight_v[i]||₂ × weight_v[i]`. All sequence-length dimensions are dynamic throughout the graph via Shape/Gather/Concat/Reshape/Slice nodes.

HuBERT feature extraction uses a shared `hubert.onnx` model (one-time export, shared across all voice models). Inference is ONNX Runtime.

---

## GL export pipeline

The export renderer is the preview renderer pointed at a framebuffer. Same code path, same shaders, same compositor — pixel-exact correspondence between what the preview showed and what renders to disk.

`render_tick_gl` runs once per app frame during an active export. For each frame: decode with libavcodec, upload to GPU, apply glass FX, composite, apply global FX, `glReadPixels`, flip rows (GL origin is bottom-left), write to ffmpeg stdin. ffmpeg encodes H.264/AAC.

Resolution: 1080×1920 (vertical), 1920×1080 (horizontal), 1080×1080 (square).

`render_snapshot_gl` uses the same pipeline to render a single PNG frame — snapshots are pixel-identical to export frames by construction.

---

## Text rendering

`draw_text_overlays` renders all active Text/Lyrics/Subtitle clips onto an `ImDrawList` using the embedded Inter Black font.

Font size is stored as a **fraction of canvas height** (`clip.font_size`), not pixels. Default is `h * 0.055f`. The preview canvas might be 500 px; the export FBO 1920 px — the text appears at the same visual proportion in both because the math is the same.

Word wrapping uses `ImFont::CalcTextSizeA` with the export font size. The wrap in preview matches the wrap in export exactly.

Eight animation styles: Fade, Glitch, Typewriter, Bounce, Scale, Slide, Stack, Block. `eff_style` is the per-clip override if set, otherwise the project-level `state.style`.

---

## Project serialization

Binary format:
```
[MAGIC: u32 = 0x534D5001]  [VERSION: u32 = 16]  [fields...]
```

When adding a field: bump `VERSION`, write unconditionally in `write_clip`/`project_save`, wrap the read in `if (version >= N)` in `read_clip`/`project_load` with a sensible default in the else branch. Never reorder existing fields. Never remove version guards.

---

## History

A stack of `AppState` snapshots. `history_push` copies the entire struct. `AppState` is large, so the stack has a fixed depth. The main contributors to snapshot size are the word lists (`std::vector<WordEntry>`) and keyframe data. The stack is cleared on project load and after ML pipeline completion — those operations are not incrementally undoable.

---

## File map

```
src/
  main.cpp              Entry point, GL/ImGui init, main loop
  app.h                 AppState, Clip, Track, all data model types
  app.cpp               app_frame; collect_effects/creative_fx; glass variants
  project.cpp           Binary save/load, VERSION=16
  audio.h / audio.cpp   miniaudio device, PCM mixing, master clock
  video.h / video.cpp   MJPEG proxy preview, GL texture upload, CPU bg_remove
  proxy.h / proxy.cpp   Proxy generation (ffmpeg subprocess)
  fx_shader.h/.cpp      GPU FX: GLSL shaders, fx_apply, scene compositor
  overlay_renderer.h/.cpp  ImDrawList text/subtitle rendering (preview + export)
  render.h / render.cpp GL export pipeline, ffmpeg pipe, snapshot
  transcribe.h/.cpp     ML pipeline subprocess management
  pth_reader.h/.cpp     PyTorch .pth reader (zip+pickle, no libtorch)
  rvc_onnx.h/.cpp       VITS→ONNX exporter (hand-rolled protobuf)
  vc_job.h/.cpp         Voice conversion job queue
  vc_onnx.h/.cpp        HuBERT + voice ONNX inference
  demucs.h/.cpp         HTDemucs ONNX inference, hand-rolled STFT
  presets.h/.cpp        User effect presets (JSON)
  generated/            Auto-generated from codegen_effects.py — do not edit
  ui/
    screen_studio.cpp   Main editor UI — largest file
    screen_splash.cpp   Splash screen
    screen_setup.cpp    First-run setup

effects/
  registry.json         Effect definitions (params, shader body, metadata)
  shaders/              GLSL source files for generated effects

tools/
  codegen_effects.py    registry.json → src/generated/*.h
  test_pth_reader.cpp   Standalone .pth parser test
```

---

## Things that will surprise you

- **`g_read_pos` is a clock, not a cursor.** It advances unconditionally. Source position is always computed from clip data.
- **Proxy slots are per clip instance, not per source file.** Two clips from the same video get two independent slots.
- **Font size is a fraction of canvas height, never pixels.** This is why preview and 4K export look proportionally identical.
- **`Keyframe.time` is relative to `clip.start`.** `eval_prop` takes absolute playhead time and handles the offset.
- **`in_point` must be advanced on split.** `right.in_point += (cut - left.start) * left.speed`. All split sites do this.
- **Glass FX is collected by exactly one path.** `collect_effects`/`collect_creative_fx` skip glass tracks. The glass functions read only the one track directly above the target video. No double-counting.
- **Effect clips don't conflict with Video/Audio for overlap checks.** Only Effect-vs-Effect overlaps are blocked. This is what allows FX bricks on the same track as a video clip (glass system).
- **fx_apply uses per-slot stable output textures.** Shared ping-pong buffers would be overwritten before the deferred ImDrawList flushes. Each slot (0–15) owns its output texture.
- **bg_remove stays CPU-side.** It reads PNG alpha masks generated by a background process; all other FX run through GLSL via fx_apply.
- **Datamosh ghost resets on `clip_start` change.** Moving or replacing a datamosh clip restarts the ghost from the first visible frame.
- **Proxy FPS must be probed from the original file, not the proxy.** The proxy is always 12 fps.
- **WN `n_layers` is derived from weight shapes.** `cond_layer` output channels ÷ (2 × hidden_channels) — not hardcoded.
