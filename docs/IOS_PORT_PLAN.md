# iOS Port Gameplan — engine + levers, Swift shell

*2026-07-03. Status: plan only, no code. Companion doc for plugging the engine
into the Claude design workflow that will build the iOS UI.*

## The verdict first: keep C++ — but only the engine

Keep the engine in C++. Do **not** rewrite it in Swift, and do **not** try to
ship the desktop UI. The split:

- **C++ engine (ports as-is, ~34k lines of `src/*.cpp`)**: timeline model
  (AppState/tracks/clips), .pms serializer (v64 — projects roundtrip
  desktop↔iOS for free), the 109-effect generated FX registry, face
  filters/beauty/makeup stack, audio engine, loudness, keyframes, groups,
  transcription/alignment pipelines. This is the valuable, tested,
  agent-driven core. A Swift rewrite of this is a year of re-introducing bugs
  we already fixed (the changelog of the last week alone is the argument).
- **Swift/SwiftUI shell (new, written by the Claude design workflow)**:
  screens, gestures, camera, file pickers, share sheets, App Store plumbing.
  ImGui is technically runnable on iOS but it is a desktop idiom — a touch
  timeline needs to be designed for touch, and that's exactly what the design
  workflow is for.

Why not all-Swift: the FX system is *generated C++/GLSL* from
`effects/registry.json`; the serializer, the parity doctrine (one chokepoint
shared by UI and agents), and the in-process ML pipelines are all C++
investments that port cleanly. Why not all-C++ (ImGui UI on iOS): rejected —
you'd ship a desktop app in a phone costume and fight every platform review
guideline and gesture convention.

**The boundary is the levers API you already have.** The IPC/agent command
surface (`ipc_server.cpp` handlers → 83 agent tools, generated manifest) is
the engine's public API. On iOS it stops being a socket and becomes a direct
in-process call — same JSON in/out, same handlers, zero new API design. The
SwiftUI app, the desktop UI, and Claude agents all pull the same levers.
That's also what makes the design workflow plug in: the manifest
(`effects/mcp_manifest.json` + `src/generated/agent_tools.h`) is a complete,
machine-readable spec of every lever, auto-regenerated.

---

## What breaks on iOS (the honest list)

Audit of the current tree (2026-07-03):

1. **Child processes — the #1 architectural conflict.** iOS forbids
   fork/exec. Today *all* media I/O is ffmpeg/ffprobe children: decode
   (MJPEG proxies), probe, export encode, take recording mux, vocal-stem
   prep, beat-detect decode, camera capture. ~10 files spawn processes.
   Everything must become in-process library calls.
2. **OpenGL.** 393 GL calls in `fx_shader.cpp` plus generated shader passes.
   iOS GLES is deprecated (still ships, but no future); Metal is the target.
3. **Camera.** V4L2-via-ffmpeg → AVFoundation (`AVCaptureSession`).
4. **Windowing/UI.** GLFW + ImGui → SwiftUI + a render surface.
5. **Model weight.** `build/models` is ~3 GB (Moondream 1.05 GB, whisper
   large 548 MB, RVC stack ~700 MB…). An App Store bundle cannot carry this.
6. **Licensing.**
   - **RVM (background removal) is GPL-3.0 — incompatible with App Store
     distribution.** Must be replaced on iOS (see Phase 5).
   - FFmpeg libs are LGPL: static linking on iOS needs the object-file
     relink provision, or avoid libav entirely by using AVFoundation/
     VideoToolbox (recommended — also hardware-accelerated and battery-sane).
7. **Unix socket IPC / python MCP server.** Fine in the simulator, wrong
   shape on device. The in-process command dispatcher replaces it; the
   existing C++ `agent_harness` (not python) is the on-device agent path.

What ports with near-zero friction: miniaudio (CoreAudio backend), ONNX
Runtime (official iOS builds + CoreML EP), whisper.cpp (Metal), the
serializer, the timeline/undo/keyframe/group logic, std::thread worker
architecture, the face tracker (pure ORT + math).

---

## Phase 0 — Engine extraction (desktop-only work, no Apple hardware needed)

> **STATUS 2026-07-03: `pms-engine` LINKS STANDALONE.** Whole-archive link
> test resolves with ZERO app symbols. Hoisted in the burn-down: the ML/
> import/subtitle pipeline (ui/pipeline.cpp → pipeline_core.cpp, whole),
> app.cpp's core state machinery (PropTrack/eval_prop/kf table/FX coupling/
> collectors → app_state.cpp), timeline+slot-open+audio-FX-collection
> helpers (→ timeline_core.cpp), media bin (→ media_bin.cpp), recents/
> recovery/open-project (→ project_meta.cpp), typography generation
> (→ typography_core.cpp), the font registry + g_font_black (→
> text_renderer.cpp, app registers faces), UI geometry stores (→
> ui_geom.cpp, app writes engine-owned globals), g_managed_dir (→
> paths.cpp). The one UI side effect found inside engine logic (panel flip
> after generate_typography) became a registered hook, no-op headless.
> engine_seams.h now declares only app-provided EXTRAS (hooks + geometry
> writers), not link dependencies. **PHASE 0 EXIT MET**: src/pms_engine.h
> C ABI (create/tick/command/events, matching the pms-ios contract) over
> the socket-free engine_command chokepoint, proven by the `engine-smoke`
> target — a binary linking ONLY pms-engine that drives levers headless
> (add_track/add_clip/get_all_clips/save_project) and passes. CI-able.
> Niceties DONE: engine_runtime.{h,cpp} — engine_tick is the single
> heartbeat (fx coupling, conform, beat polls, recorders, playhead clamp,
> export/render/slot-opens when gl_ready) called by BOTH the desktop frame
> and pms_tick; the event feed diffs state into typed events (playhead,
> pipeline, loudness @7Hz, face_track, takes) matching the pms-ios Swift
> bridge, drained by pms_poll_events. Phase 0 fully closed.

Goal: a `pms-engine` static-lib CMake target that compiles with **no** GLFW,
ImGui, X11, or process-spawn code, consumed by the existing desktop app as
proof. This is the largest de-risking step and it improves the desktop
codebase regardless of iOS.

- Split `src/` into `engine/` and `app/` (or CMake source-lists first,
  physical moves later). `src/ui/*` (24k lines) stays app-side. `main.cpp`,
  `filepicker.cpp`, GLFW init stay app-side.
- Define three seams as pure-virtual backends (single-header interfaces):
  - **MediaBackend** — decode(file, t) → frames, probe(file) → metadata,
    encode(session) ← frames/samples, remux/take-slicing. Desktop impl wraps
    today's ffmpeg children *unchanged*; iOS impl comes in Phase 3.
  - **CaptureBackend** — camera enumerate/start/stop → JPEG-or-RGB frames +
    mic. Desktop impl = current V4L2/ffmpeg path (+`PMS_FAKE_CAM`); iOS impl
    Phase 4. The fake-cam test rig becomes a first-class backend — that keeps
    the whole agent-driven verification loop working against the engine.
  - **RenderSurface** — "give me a GL/Metal context and a target texture."
    The scene compositor and fx passes already render to FBOs, not the
    backbuffer; only the final blit differs per platform.
- Elevate the IPC dispatcher: `engine_command(json) → json` as a plain
  function, socket server becomes a thin desktop-only wrapper around it.
  Success test: the headless rig drives the engine lib through
  `engine_command` with no socket.
- Events out: progress, meters, face-track status, take completion — one
  polled event queue (the UI tick already polls everything; formalize it).

Exit criteria: desktop app runs on the split build; headless rig passes; a
`pms_engine.h` C ABI exists (init / tick / command / events / render-into-
texture) — this header is what Swift will see.

## Phase 1 — Levers doc for the design workflow (parallel with Phase 0)

The design workflow needs a contract, not code:

- Generate `docs/LEVERS.md` from the same codegen that builds the manifest:
  every command, params, ranges, defaults (the FX registry already carries
  min/max/default/labels — this is nearly free).
- Add the ~10 engine lifecycle calls that aren't agent tools (init, tick,
  render, event poll, asset/model paths).
- Mark each lever with its iOS phase availability (e.g. `trigger_export` →
  Phase 3; `vrecord_start` → Phase 4) so screens can be designed now and
  wired progressively.

## Phase 2 — Renderer strategy

Recommendation: **ANGLE first, Metal-native selectively later.**

- **Phase 2a — ANGLE (GL-on-Metal).** The 393-call GL renderer and all
  generated GLSL run unmodified. ANGLE is production-proven on iOS. This
  gets pixel parity with desktop in days, which matters enormously because
  the whole preview==export doctrine is verified against this renderer.
- **Phase 2b — native Metal, driven by codegen.** The generated effects are
  the easy 90%: extend `codegen_effects.py` to emit MSL via
  glslang→SPIRV-Cross at build time (shaders stay authored in GLSL in the
  registry — one source of truth). Hand-coded passes (scene compositor,
  beauty/makeup/warp, chroma family) port by hand last, behind the
  RenderSurface seam, only if ANGLE overhead actually shows up in profiling.
- Decision gate: profile ANGLE on an A15-class device with a 1080p two-track
  project + beauty filter. If it holds 30 fps preview, 2b is deferred
  indefinitely.

## Phase 3 — MediaBackend: AVFoundation/VideoToolbox

Replace the ffmpeg-child architecture on iOS (and dodge the LGPL question
entirely):

- Decode: `AVAssetReader` + VideoToolbox (hardware). iPhone hardware decode
  is fast enough that **the MJPEG proxy system likely isn't needed on iOS**
  — plan to bypass it, keep the option open behind MediaBackend.
- Probe: `AVAsset` metadata (fps must come from the asset — same lesson as
  the proxy-fps pitfall on desktop).
- Encode/export: `AVAssetWriter` (H.264/HEVC hardware). The A/V-interleave
  and loudness-report logic stays engine-side; only the muxer swaps.
- Takes: camera frames → `AVAssetWriter` per take, same loop-clock slicing
  logic (recorder.cpp logic is engine-side and portable; only the sink
  changes).
- Audio decode for waveforms/beat/stems: `AVAudioFile`/`ExtAudioFile`.

## Phase 4 — CaptureBackend + the record loop

- `AVCaptureSession` → BGRA/NV12 frames → engine intake (same half-res
  submit path the tracker uses; rotation metadata maps to the existing
  rot_q plumbing — the roll ladder already tolerates arbitrary orientation).
- Mic capture via miniaudio/CoreAudio; the shared loop clock and A/V pair
  logic is engine code and ports as-is (session-stamp fix included).
- The live mirror composite (canvas.cpp) is app-side today; its logic moves
  to the engine's render path so SwiftUI just displays the engine texture.

## Phase 5 — ML on device

- ONNX Runtime iOS + CoreML execution provider. Per-model validation order:
  face trio (YuNet + landmarks + blendshapes — small, critical, likely
  fine), Kim_Vocal_2 (64 MB), wav2vec2 CTC (91 MB), whisper.cpp with Metal
  (use `large-v3-turbo` only on Pro-class devices; default to a smaller
  model per device tier).
- **Model packs download-on-demand** from the HF repo (plumbing exists:
  `hf_api.cpp`, models repo, release manifest). Bundle only the face trio
  (~5 MB) + sprites + makeup textures; everything else is a first-use
  download with the existing progress UI pattern.
- **RVM must be replaced on iOS (GPL-3.0).** Recommendation: Apple Vision
  `VNGeneratePersonSegmentationRequest` behind the existing bg-remove seam —
  native, fast, zero bundle cost, and arguably better UX. MODNet (Apache) is
  the cross-platform fallback if output parity with desktop matters.
- Defer to post-v1: Moondream (1 GB — replace with Vision framework
  describe/OCR or an API call), RVC voice convert (heavy; revisit), Piper.

## Phase 6 — The SwiftUI shell (the design-workflow deliverable)

- App skeleton: engine lifecycle owner, render view (`CAMetalLayer` fed by
  the engine texture), event pump → observable state for SwiftUI.
- Screens the design workflow owns end-to-end via levers: Home/projects,
  camera/record (filters picker = `face_filter` lever + preview textures),
  timeline (read via `get_all_clips`, edit via the same move/trim/split
  levers agents use), FX browser (driven by the generated manifest — the
  searchable catalog is data, not code), export.
- Design-workflow contract: screens call `engine_command` JSON only. If a
  screen needs a lever that doesn't exist, that's an engine PR (add the
  handler + regen manifest) — never a bypass. This is the same parity
  doctrine that keeps desktop UI and agents honest.

## Phase 7 — Test rig on Apple silicon

- The headless verification rig (fake cam, snapshots, IPC-driven renders) is
  the crown jewel of the current dev loop — port it early, not last: build
  `pms-engine` for **macOS** (same Apple GL/Metal constraints, no device
  needed), run the rig in CI. Simulator + a couple of physical devices for
  camera/perf passes.
- Golden-frame diffs against desktop renders for the FX registry (same
  registry → same pixels within tolerance) — automatable with the existing
  snapshot machinery.

---

## Sequencing & effort shape

| Phase | Depends on | Parallel with | Rough shape |
|---|---|---|---|
| 0 engine extraction | — | 1 | the big one; mostly mechanical, high line-count |
| 1 levers doc | — | 0 | small; mostly codegen extension |
| 2a ANGLE renderer | 0 | 3 | small-medium; integration work |
| 3 MediaBackend | 0 | 2a | medium; the real iOS engineering |
| 4 Capture/record | 2a, 3 | 5 | medium |
| 5 ML on device | 0 | 4 | medium; mostly validation + download UX |
| 6 SwiftUI shell | 1, 2a | continuously | owned by the design workflow |
| 2b Metal-native | 2a ships | — | only if profiling demands it |

Order of first light: 0 → 1 → (2a + 3) → first on-device preview of an
existing .pms project → 4/5 → record + filters on device → 6 fills in
continuously against the levers.

## Risks & open questions

- **GPL (RVM)** — resolved by Vision framework swap on iOS; decide whether
  desktop also migrates for parity.
- **Memory ceilings** — 1080p GL pipeline + face models fit comfortably;
  whisper-large + Moondream do not coexist with a video pipeline on 6 GB
  devices. Model tiering per device is a requirement, not a nice-to-have.
- **ANGLE + external textures** — camera frames arrive as CVPixelBuffers;
  bridging them into ANGLE GL textures without copies needs a spike early in
  Phase 2a (there are known paths; verify before committing to zero-copy).
- **.pms compatibility** — keep the serializer byte-identical across
  platforms; add a `platform` field only if a divergence ever forces it
  (none anticipated).
- **Where the Claude design workflow bites first**: it can start against
  Phase 1's levers doc immediately — screens designed against the manifest,
  mocked engine, then wired to the real one when 2a/3 land.
