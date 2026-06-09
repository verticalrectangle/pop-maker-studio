# Pop Maker Studio — Architecture

Developer reference for the major structural decisions, data flow, and non-obvious invariants. Read this before touching anything in `src/`.

---

## The single-struct model

Everything lives in `AppState` (`src/app.h`). There is no event system, no observer pattern, no reactive bindings. Every frame the entire UI is re-derived from `AppState` and drawn from scratch.

This is Dear ImGui's immediate-mode model taken seriously. The consequences are all good: undo/redo is a `memcpy`-equivalent snapshot-and-restore, serialization covers the entire application state by construction, and debugging means printing the struct. `AppState` is large (~100 fields) but deliberately flat. Nesting it into sub-objects creates partial-snapshot problems for history — resist the urge.

The main loop:

```
main loop
  ├── glfwPollEvents()                — OS drops land in g_dropped_queue / g_bin_pending
  ├── drain_dropped_queue()           — single-file drops: one path per frame → g_dropped_file
  ├── drain_bin_pending(state)        — multi-file drops: all paths → state.bin
  └── app_frame(state)
        ├── update playhead (audio position or wall clock fallback)
        ├── ipc_server_poll(state)    — process MCP/IPC commands from socket
        ├── render_tick_gl(state)     — advance export by one frame if running
        └── ui_studio(state)
              ├── draw_timeline()
              ├── draw_preview()      — three-tier decode → fx_apply → scene composite
              ├── panel_bin / panel_media / panel_clip / panel_fx / panel_terminal …
              └── transport overlay (hover-fade) + agent activity card (activity-fade)
```

---

## Clip data model

Every piece of content on the timeline is a `Clip`. Clips are typed (`ClipType::Video`, `Audio`, `Text`, `Lyrics`, `Subtitle`, `Effect`, `Background`, `BodyFX`) but live in the same struct. Tracks are named containers.

**Source time vs timeline time** is the single most important distinction in the codebase:

```
source_time = clip.in_point + (timeline_time - clip.start) * clip.speed
```

Any code that reads a video frame or audio sample must use `source_time`. Confusing the two caused the split-clip black-frame and wrong-audio bugs. Every decoder call site has been audited.

Keyframe times (`Keyframe.time`) are relative to `clip.start`, not absolute timeline time. `Clip::eval_prop(name, playhead)` handles the offset — callers pass absolute playhead time and don't need to think about it.

**`TextStyle`** (`src/app.h`) is a sub-struct on every `Clip` carrying per-clip text rendering configuration: shadow (offset x/y, RGBA), stroke (width, RGBA), glow (radius, RGBA), and background box (RGBA, padding x/y, corner radius). Defaults match the old hardcoded behavior (shadow on at 2px, everything else off) so old projects are visually unchanged on load.

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

## Video preview pipeline

Three tiers, picked per-slot at open time, transparently upgraded as media becomes available. The slot table is `state.proxy_paths[MAX_VIDEO_TRACKS]` (32 slots) keyed by source file path, NOT by track index — two clips on different tracks sharing the same source file share one slot.

**Tier 1 — Native libav decode** (`PreviewSource::Native`). When a clip is added, `video_open_native` opens the source via `avformat` + `avcodec` and the first frame shows immediately. `try_attach_hw` walks `AV_HWDEVICE_TYPE_VAAPI → CUDA → VDPAU → VIDEOTOOLBOX` and attaches the first usable HW context; silent SW fallback runs at `thread_count = 2`. `prepare_native_frame_cpu` skips `av_seek_frame + avcodec_flush_buffers` when the next requested frame is within ~8 frames forward of `last_decoded_pts` — sequential play / forward scrub then decodes one frame instead of the whole GOP-since-keyframe.

**Tier 2 — MJPEG proxy** (`PreviewSource::Proxy`). In parallel with the native open, `proxy_start` queues an ffmpeg transcode (quarter-res MJPEG + binary frame-offset table). Scrubbing on the proxy is `fseek + libjpeg-turbo decode + GPU upload` — measurably cheaper than HW-decoding the original, so `screen_studio`'s per-slot scan loop swaps `Native → Proxy` the instant transcode finishes. The proxy worker pool runs `min(4, cores/2)` workers with `-hwaccel auto`, `-threads K` (so workers × threads ≈ hardware_concurrency), and `scale=...:flags=fast_bilinear`. The proxy `-r` cap is 30 fps; `ProxyInfo::fps` is probed from the **original** source so the CTC aligner snaps word timestamps to real source frame boundaries.

**Tier 3 — Single-frame still** (`PreviewSource::Still`). Fallback when libav can't open the file. `proxy_ensure_still` runs ffmpeg once, caches the JPEG.

Every slot keeps an 8-frame `DecodedFrame ring` (RGB pixels, optional RGBA composite for chroma-key / bg_remove / glitch corruption-bleed). The canvas pre-walk (`canvas.cpp`) dispatches a parallel JPEG/decoder batch via the thread pool for the active clip per track plus a 3-frame boundary warm into neighbour clips when the playhead is within 1 s of a cut. `video_get_texture` is a ring lookup; misses fall through to sync decode on the main thread.

`gc_video_slots` frees slots whose clips were deleted. `proxy_is_ready` is cached at session scope — terminal "ready" hits never re-stat; in-progress paths are throttled to one stat per 250 ms so the timeline draw loop can't flood syscalls at 60 fps × N visible clips.

---

## GPU FX pipeline

All visual effects are GLSL fragment shaders. The pipeline distinguishes two application contexts:

**Glass pass (pre-composite)**: an FX brick is glass when the track directly below it has an active Video clip at that time. Glass FX apply to the single clip's decoded texture before it's composited with the rest of the scene. The check is purely positional — no mode flag, no configuration.

**Global pass (post-composite)**: an FX brick on a track with no video below it applies to the full composited frame after all clips have been merged. Canvas resolution depends on `state.format`: 1080×1920 vertical (9:16), 1920×1080 horizontal (16:9), or 1080×1080 square (1:1).

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

Effects beyond the hand-wired core are defined in `effects/registry.json` and generated at build time by `tools/codegen_effects.py`. The script outputs into `src/generated/`:

| File | Purpose |
|------|---------|
| `fx_enum_entries.h` | `FXType` enum cases |
| `fx_clip_fields.h` | Per-clip amount + param fields (with beat-sync variants) |
| `fx_accum_fields.h` | `CreativeFXAccum` fields |
| `fx_collect_cases.h` | Switch cases for accumulating clip FX |
| `fx_shader_strings.h` | GLSL source as `R"glsl(...)glsl"` string literals |
| `fx_shader_init.h` | `fx_generated_init()` — compile all programs at startup |
| `fx_shader_apply.h` | Per-effect `glUniform` calls and blend pass |
| `fx_project_read.h` | Versioned deserialization |
| `fx_project_write.h` | Serialization |
| `fx_ui_inspector.h` | ImGui slider + beat-sync button per param |
| `fx_ui_color.h`, `fx_ui_label.h`, `fx_ui_abbrev.h`, `fx_ui_picker.h` | Display metadata |
| `fx_preview_defaults.h` | Default param values for the effect picker preview |
| `fx_type_list.h`, `fx_gen_names.h` | Iteration helpers |
| `fx_attached_accum.h`, `fx_attached_defaults.h`, `fx_attached_ui.h` | AttachedFX system |
| `fx_clip_set_dispatch.h` | `fx_clip_set_param(Clip&, fx_id, param, value)` — used by IPC `set_clip_fx` |

The script also writes `effects/mcp_manifest.json` — a machine-readable effect catalog (id, label, description, params with ranges). The MCP server doesn't expose a generic `apply_effect` tool any more (deprecated in favour of `add_effect_brick` + `set_clip_prop`), but the manifest is still consumed by IPC handlers and tooling that needs the canonical effect list without hardcoding.

Every entry specifies: `id`, `label`, `params[]` (name, min, max, default, curve, fmt), a GLSL shader path, and display metadata. Power curves (`"curve": 0.5`) map slider travel to perceptual values at accumulation time.

Do not edit files in `src/generated/`. Re-run `tools/codegen_effects.py` after modifying `effects/registry.json`.

---

## ML pipeline

### MDX-Net vocal separation (`src/separate.cpp`)

Model: Kim_Vocal_2.onnx (~64 MB, UVR5 community). Pipeline: ffmpeg decode → STFT (FFTW3, n_fft=6144, hop=1024, Hann window, center-padded matching librosa defaults) → chunked ONNX inference (256-frame chunks, 64-frame overlap, linear crossfade) → iSTFT → instrumental as `original − vocals`. All C++, ONNX Runtime CPU. CUDA attempted opportunistically.

The model expects `[1, 4, kDimF, kDimT]` float tensors (L_real, L_imag, R_real, R_imag). Output is a vocal mask in the same layout. ISTFT uses overlap-add normalized by window power squared.

### Whisper transcription (`src/transcribe.cpp`)

whisper.cpp with `ggml-large-v3-turbo-q5_0`. DTW token timestamps (`cparams.dtw_token_timestamps = true` + `WHISPER_AHEADS_LARGE_V3_TURBO`) align each BPE token to the audio using attention heads. Token → word grouping: tokens that start with a space mark word boundaries. `td.t_dtw` is preferred over `td.t0` when non-negative.

### CTC forced alignment (`src/forced_align.cpp`)

Runs after Whisper to produce frame-accurate word timestamps. Model: wav2vec2-base-960h ONNX (Xenova quantized, ~94 MB). The vocab is loaded from `wav2vec2_vocab.json` at runtime (character → token index map; `<pad>` = CTC blank; `|` = word separator).

The stay/advance trellis (torchaudio forced-alignment algorithm) uses a plain character target (no blank padding) and a (T+1)×(L+1) log-prob DP. Each state j = "tokens consumed so far"; transitions are stay-at-j (emit blank) or advance-to-j+1 (emit tokens[j]). Backtracking from the frame with the highest cell(t, L) score yields one PathPoint per frame; consecutive same-token points are merged into CharSeg spans, which are mapped to absolute word timestamps.

Processing is chunked into 30-second windows. Each chunk is mean/variance normalized before ONNX inference (wav2vec2 feature extractor convention). The model's `attention_mask` input is optional — detected via `sess.GetInputCount() >= 2`.

Final timestamps are snapped to proxy FPS frame boundaries:
```cpp
snap(t) = round(t * proxy_fps) / proxy_fps
```

Whisper timestamps serve as fallback if alignment fails for any chunk.

### Voice conversion — zero Python (`src/pth_reader.cpp`, `src/rvc_onnx.cpp`, `src/vc_onnx.cpp`)

The voice conversion pipeline reads PyTorch `.pth` checkpoints directly without libtorch or Python: the zip archive is extracted via the system `unzip` binary, then a hand-rolled pickle protocol-2 VM parses `data.pkl` to reconstruct tensor metadata and model configuration.

From metadata alone, `pth_to_onnx()` builds a complete ONNX graph using a hand-rolled protobuf serializer. The VITS architecture is reconstructed in C++:

- `enc_p` (6-layer TextEncoder with relative multi-head attention and correct `_rel_to_abs` — dynamic T, 5-step implementation)
- `flow_reverse` (4 ResidualCouplingBlock layers in reverse order with channel flip between each; layer count and WN `n_layers` derived from weight shapes, not hardcoded)
- `dec` (NSF-HiFiGAN decoder with SineGen as a deterministic CumSum+Mod+Sin graph, f0 repeat_interleave via Expand+Reshape, ConvTranspose upsample stages with per-stage resblocks and noise convolutions)

Weight normalization is resolved at export time: `W_eff[i] = weight_g[i] / ||weight_v[i]||₂ × weight_v[i]`. All sequence-length dimensions are dynamic throughout the graph via Shape/Gather/Concat/Reshape/Slice nodes.

HuBERT feature extraction uses a shared `hubert.onnx` model exported once by the `export-hubert` CLI tool (same C++ pipeline: reads `hubert_base_ls960.pt`, exports ONNX). Inference is ONNX Runtime.

---

## Text rendering (`src/text_renderer.h/cpp`)

All Text, Lyrics, and Subtitle clip rendering goes through `render_text_block(TextRenderCtx, lines)`. Both the canvas preview (`src/ui/canvas.cpp`) and the export overlay renderer (`src/overlay_renderer.cpp`) call this single function, guaranteeing pixel-exact correspondence between preview and export.

Layer order per line block:
1. **Glow** — 3 passes × 8 angles, decreasing alpha and increasing radius per pass (~24 AddText calls/line)
2. **Background box** — `AddRectFilled` with `ts.bg_col` and `ts.bg_corner`; also fires when `eff_style == AnimStyle::Block`
3. **Shadow** — single AddText at `{lx + shadow_ox, ly + shadow_oy}`
4. **Stroke** — 8-directional AddText at `±stroke_w` (N/S/E/W + diagonals)
5. **Main text / Karaoke** — per-word color using `clip->karaoke_highlight_color` for active words

`TextRenderCtx` carries: draw list, font, font size, animation alpha/dx/dy, clip pointer (for TextStyle and color), effective AnimStyle, anchor mode (left/center/right), anchor X position, top-Y, line height, playhead time, and an optional pointer to the karaoke word list.

---

## Project bin

`AppState.bin` is a `std::vector<std::string>` of paths "available to the project" — distinct from `state.tracks[i].clips[j].text` paths which are "placed on the timeline". Adding a file and placing a file are two operations.

Routing rules in `main.cpp`:
- Single-file OS drop → `g_dropped_queue` → existing readers (hover-track placement preserved). `add_clip_to_track` and the screen_studio drop handler call `bin_add` so the bin reflects every placement.
- Multi-file OS drop → `g_bin_pending` → `bin_add` for each, no auto-placement. Solves the "5 files dropped = 5 stacked tracks" problem.
- Browse buttons / media library clicks → place + `bin_add` (via `add_clip_to_track`).
- IPC `add_to_bin` / `remove_from_bin` for agent-driven workflows.

`bin_add` is a no-op for duplicates and mirrors into the cross-project `RecentMedia` list. `bin_remove` drops the bin entry only — clips already on the timeline keep working (the timeline holds its own path reference). `bin_backfill_from_timeline` runs on project load for backward compat (pre-v36 projects don't carry the bin field).

UI: `panel_bin` (single-column compact rows: thumbnail / filename / duration / `used Nx`). Hover × removes. Drag uses the existing `MEDIA_VID/IMG/AUD` payload types so timeline drop sites accept it unchanged.

---

## Embedded terminal

`src/terminal.cpp` runs a real PTY backed by `libvterm`. The terminal panel lives below the timeline in a draggable splitter (`panel_terminal`). Drops on the terminal panel inject the file path at the shell prompt instead of touching the timeline — handled by the `terminal_claims_drop` check at the top of `screen_studio`'s drop handler.

Mouse selection, right-click copy/paste, double-wide character handling, and live resize (vterm + pty track panel width on every frame) are all wired through `panel_terminal.cpp`.

---

## IPC and MCP server

`ipc_server.cpp` listens on a PID-scoped Unix domain socket (`/tmp/pop-maker-studio-{pid}.sock`). The lock file `/tmp/pop-maker-studio.lock` holds `{pid} {socket_path}` for client discovery.

Protocol: newline-delimited JSON. Request: `{"id": "...", "method": "...", "params": {...}}`. Response: `{"id": "...", "result": {...}}` or `{"id": "...", "error": "string"}`.

`ipc_server_poll(state)` is called from the main loop — same thread as the UI. All mutations land on the main thread between frames, identical to a UI interaction. No locking required.

**Batch system**: mutation commands run inside a `begin_batch`/`end_batch` pair so the entire sequence becomes one `history_push` and one undo step. Mutations called outside an explicit batch are auto-wrapped in a single-call batch labelled with the method name, so clients only need `begin_batch` when grouping multiple mutations. If the client disconnects with an open batch, the partial batch is pushed with an `(incomplete)` label so nothing is lost.

**Commands** (read-only — no batch effect): `get_project`, `get_clips`, `get_all_clips`, `get_pipeline_status`, `get_export_status`, `get_bg_remove_status`, `get_audio_analysis`, `get_transcript`, `get_media_info`, `get_stills`, `seek`, `play`, `pause`, `validate_glsl`, `save_project`, plus search/probe tools (`find_and_add_clip`, `find_audio_cue`, `search_transcript`).

**Commands** (mutation — auto-batched if standalone): `add_clip`, `add_clip_sequence`, `delete_clip`, `move_clip`, `trim_clip`, `split_clip`, `set_clip_prop`, `set_clip_props`, `set_text_style`, `add_track`, `rename_track`, `delete_clips_after`, `trim_all_to`, `add_effect_brick`, `add_body_fx_brick`, `add_multifx_brick`, `add_callout`, `add_chapter_marker`, `remove_chapter_marker`, `add_to_bin`, `remove_from_bin`, `trigger_pipeline`, `generate_typography`, `apply_multicam_cuts`, `cut_at_phrase`, `cut_filler_words`, `remove_silence`, `crop_media`, `extract_clip_segment`, `load_project`, `new_project`.

The **Python MCP server** (`mcp_server/server.py`) bridges Claude to the IPC layer using the `mcp` SDK. It reads the lock file, connects to the socket, and registers ~70 tools — the full editing surface plus search, audio/video analysis, and bin management. Tool descriptions live in `server.py` and are the canonical reference; the generic dispatcher forwards anything not explicitly named to the IPC layer using the same method name.

**Async-first.** Long-running mutations return immediately with a stage hint. `trigger_pipeline` returns `{stage: "running"}` and the caller polls `get_pipeline_status` until `stage` is `done` or `error`. Same pattern for `analyze_audio`, `remove_background`, `find_and_add_clip`. This keeps the MCP socket / chat free during ML work.

---

## GL export pipeline

The export renderer is the preview renderer pointed at a framebuffer. Same code path, same shaders, same compositor — pixel-exact correspondence between what the preview showed and what renders to disk.

`render_tick_gl` runs once per app frame during an active export. For each frame: decode with libavcodec, upload to GPU, apply glass FX, composite, apply global FX, `glReadPixels`, flip rows (GL origin is bottom-left), write to ffmpeg stdin. ffmpeg encodes H.264/AAC.

Resolution: 1080×1920 (vertical), 1920×1080 (horizontal), 1080×1080 (square).

`render_snapshot_gl` uses the same pipeline to render a single PNG frame — snapshots are pixel-identical to export frames by construction.

---

## Project serialization

Binary format:
```
[MAGIC: u32 = 0x534D5001]  [VERSION: u32 = 36]  [fields...]
```

When adding a field: bump `VERSION`, write unconditionally in `write_clip`/`project_save`, wrap the read in `if (version >= N)` in `read_clip`/`project_load` with a sensible default. Never reorder existing fields. Never remove version guards. Recent gates: v33 lyrics word edits, v36 project bin (also backfilled from existing clip paths for pre-v36 saves).

The generated effect fields are all written under a single version gate tied to `project_version` in `effects/registry.json` — when the effect set changes, that version bumps and the codegen regenerates `fx_project_read.h` with the new gate. Old projects get default-constructed values for new fields.

---

## History

A stack of `AppState` snapshots. `history_push` copies the entire struct. `AppState` is large, so the stack has a fixed depth. The main contributors to snapshot size are the word lists (`std::vector<WordEntry>`) and keyframe data. The stack is cleared on project load and after ML pipeline completion — those operations are not incrementally undoable.

---

## File map

```
src/
  main.cpp               Entry point, GL/ImGui init, main loop, drop queues
  app.h                  AppState (incl. bin), Clip, Track, TextStyle, all data model types
  app.cpp                app_frame; collect_effects/creative_fx; glass variants
  project.cpp            Binary save/load, VERSION=36
  audio.h / audio.cpp    miniaudio device, PCM mixing, master clock
  video.h / video.cpp    Three-tier preview (Native libav, MJPEG proxy, Still), GL upload
  proxy.h / proxy.cpp    Parallel MJPEG proxy generation (worker pool, ffmpeg + hwaccel)
  fx_shader.h/.cpp       GPU FX: GLSL shaders, fx_apply, scene compositor
  overlay_renderer.h/.cpp  ImDrawList text/subtitle rendering (preview + export)
  text_renderer.h/.cpp   Shared text layer renderer (glow/bg/shadow/stroke/karaoke)
  render.h / render.cpp  GL export pipeline, ffmpeg pipe, snapshot
  transcribe.h/.cpp      ML pipeline orchestration (separate → whisper → align)
  separate.h/.cpp        MDX-Net vocal separation, FFTW3 STFT/iSTFT
  forced_align.h/.cpp    CTC forced alignment, stay/advance trellis DP, wav2vec2 ONNX
  ipc_server.h/.cpp      Unix socket IPC server, JSON command dispatch, auto-batching
  pth_reader.h/.cpp      PyTorch .pth reader (system unzip + handrolled pickle VM, no libtorch)
  rvc_onnx.h/.cpp        VITS→ONNX exporter (hand-rolled protobuf)
  vc_job.h/.cpp          Voice conversion job queue
  vc_onnx.h/.cpp         HuBERT + voice ONNX inference
  bg_remove.h/.cpp       Background removal (u2net ONNX, grayscale MJPEG mask streaming)
  body_fx.h/.cpp         BodyFX brick (skeleton-tracked effects)
  noise_reduce.h/.cpp    Spectral noise reduction
  terminal.h/.cpp        Embedded PTY + libvterm parser
  runtime_fx.h/.cpp      Hot-reload custom GLSL effects from ~/.local/share/.../effects
  paths.h / paths.cpp    Binary-relative model path resolution
  history.h/.cpp         AppState snapshot stack
  presets.h/.cpp         User effect presets (JSON)
  generated/             Auto-generated from codegen_effects.py — do not edit

  ui/
    screen_studio.cpp    Main editor UI, library button row, drop handler, overlay fades
    screen_splash.cpp    Splash screen
    screen_setup.cpp     First-run setup
    canvas.cpp           Canvas preview, text layout, hit testing, prefetch pre-walk
    timeline.cpp         Timeline, clip drag/resize, track management
    pipeline.cpp         ML pipeline UI, kick_pipeline()
    panel_clip.cpp       Clip inspector (properties, text style, karaoke, bg_remove)
    panel_animation.cpp  Typography presets, generate_typography()
    panel_fx.cpp         Effect inspector
    panel_media.cpp      Media browser (Bin + Videos / Images / Audio recents)
    panel_terminal.cpp   Terminal panel chrome, drop-injects path at prompt
    export_ui.cpp        Export dialog
    studio_shared.cpp    add_clip_to_track, slot table helpers, panel-view enum dispatch

effects/
  registry.json          Effect definitions (params, shader paths, metadata)
  mcp_manifest.json      Effect catalog — generated by codegen_effects.py
  shaders/               GLSL source files for generated effects

tools/
  codegen_effects.py     registry.json → src/generated/*.h + effects/mcp_manifest.json
  export_hubert.cpp      CLI tool: hubert_base.pt → hubert.onnx

mcp_server/
  server.py              Python MCP server (~70 tools, reads lock file). The
                         canonical reference for tool descriptions — don't
                         re-document tools in side files; they rot.
  requirements.txt       mcp>=1.0
```

---

## Things that will surprise you

- **`g_read_pos` is a clock, not a cursor.** It advances unconditionally. Source position is always computed from clip data.
- **Preview slots are per source path, not per track.** `state.proxy_paths[MAX_VIDEO_TRACKS]` (32 slots) is keyed by `clip.text`. Two clips with the same source file share one slot, regardless of track.
- **Bin entries and timeline clips are independent.** Removing a file from the bin does NOT remove placed clips (they hold their own path). `bin_used_count` only counts placements for the UI badge.
- **Font size is a fraction of canvas height, never pixels.** This is why preview and 4K export look proportionally identical.
- **`Keyframe.time` is relative to `clip.start`.** `eval_prop` takes absolute playhead time and handles the offset.
- **`in_point` must be advanced on split.** `right.in_point += (cut - left.start) * left.speed`. All split sites do this.
- **Glass FX is collected by exactly one path.** `collect_effects`/`collect_creative_fx` skip glass tracks. The glass functions read only the one track directly above the target video. No double-counting.
- **Effect clips don't conflict with Video/Audio for overlap checks.** Only Effect-vs-Effect overlaps are blocked. This is what allows FX bricks on the same track as a video clip (glass system).
- **fx_apply uses per-slot stable output textures.** Shared ping-pong buffers would be overwritten before the deferred ImDrawList flushes. Each slot (0–15) owns its output texture.
- **bg_remove stays CPU-side and requires the MJPEG proxy.** It reads grayscale MJPEG masks (`bg_masks.mjpeg`) generated by a background thread, keyed by **proxy** frame index. IPC `start_bg_remove` / `process_body_fx_masks` fail-fast when the proxy isn't ready — agents need to wait for proxy generation before requesting masks.
- **Datamosh ghost resets on `clip_start` change.** Moving or replacing a datamosh clip restarts the ghost from the first visible frame.
- **Proxy is `-r 30` capped to source fps; `ProxyInfo::fps` is the SOURCE fps.** The proxy file's literal frame rate is 30 (or lower if the source is slower), but `ProxyInfo::fps` carries the original source fps because that's what CTC alignment + frame-snapping needs.
- **Native decode and proxy decode coexist per slot.** A slot opens Native first (instant) and gets upgraded to Proxy when transcode finishes. The screen_studio per-slot loop gates on `video_source(slot) == Proxy` to avoid re-upgrading.
- **WN `n_layers` is derived from weight shapes.** `cond_layer` output channels ÷ (2 × hidden_channels) — not hardcoded.
- **CTC alignment runs per Whisper segment, not over the full audio.** The (T+1)×(L+1) trellis is tiny (T = frames in one segment, L = chars in that segment's text). Whisper timestamps are the fallback if any segment fails.
- **IPC mutations land on the main thread.** `ipc_server_poll` runs in the main loop. There is no concurrency between MCP edits and UI interactions — they interleave frame by frame.
- **Single mutations auto-batch.** `begin_batch`/`end_batch` is only required when grouping multiple mutations as one undo step. Standalone `add_clip` becomes one undo step automatically (labelled with the method name).
- **`set_clip_fx` dispatches via generated code.** `fx_clip_set_dispatch.h` is a codegen output. If you add a new effect to the registry, re-run `codegen_effects.py` or `set_clip_fx` won't recognize the new effect id.
