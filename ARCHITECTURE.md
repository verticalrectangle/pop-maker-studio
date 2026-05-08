# Pop Maker Studio — Architecture

A developer reference covering the major structural decisions, data flow, and non-obvious invariants in the codebase. Read this before touching anything in `src/`.

---

## 1. The big picture

PMS is a native C++ desktop application with no framework dependencies. The UI is built with [Dear ImGui](https://github.com/ocornut/imgui) in immediate mode. All application state lives in one struct. There is no event system, no observer pattern, no reactive bindings — every frame the entire UI is re-derived from `AppState` and drawn from scratch.

```
main loop
  └── app_frame(state)
        ├── update playhead (wall clock or audio position)
        ├── render_tick_gl(state)      ← advance export by one frame if running
        ├── ui_studio(state)           ← timeline, preview, panels
        │     ├── draw_timeline()
        │     ├── draw_preview()       ← proxy decode → fx_apply → draw
        │     └── draw_panel_*()
        └── ImGui::Render()
```

---

## 2. AppState — the single source of truth

**`src/app.h`**

Everything lives in `AppState`. There is no hidden state in the UI layer beyond a handful of `static` locals inside `draw_timeline` and `draw_preview` that represent transient interaction state (drag offsets, popup positions, glass drag handles). If it needs to survive a frame, it either lives in `AppState` or in a file-scoped `static`.

This is intentional. ImGui's immediate-mode model makes retained widget state painful. Keeping everything in one flat struct means:
- Undo/redo is trivial — snapshot and restore `AppState`
- Serialization covers everything — `project_save` walks the struct
- Debugging is easy — print the struct, you know everything

`AppState` is large (~100 fields) but deliberately flat. Resist the urge to nest it into sub-objects; that creates partial-snapshot problems for history.

---

## 3. Clip data model

**`src/app.h` — `Clip`, `Track`, `ClipType`**

Every piece of content on the timeline is a `Clip`. Clips are typed (`ClipType::Video`, `Audio`, `Text`, `Lyrics`, `Subtitle`, `Effect`) but live in the same struct. Tracks are just named containers for clips.

### Key fields

| Field | Meaning |
|---|---|
| `start`, `end` | Position on the timeline (seconds) |
| `in_point` | Where in the source file playback begins |
| `out_point` | Where in the source file playback ends (-1 = source end) |
| `speed` | Playback rate multiplier |
| `text` | Source file path (for Video/Audio) or display text |
| `source_id` | Groups clips that came from the same source drop |
| `ktracks` | Per-property keyframe tracks (map of name → PropTrack) |

### Source time vs timeline time

This distinction is critical everywhere:

```
source_time = clip.in_point + (timeline_time - clip.start) * clip.speed
```

Any code that reads a video frame or audio sample for a clip must use `source_time`, not `timeline_time`. Confusing the two was the root cause of the split-clip black-frame and wrong-audio bugs.

### Keyframes

`PropTrack` holds a sorted list of `Keyframe` structs. `Keyframe.time` is **relative to `clip.start`**, not absolute timeline time. `Clip::eval_prop(name, playhead)` handles the offset internally — callers pass absolute playhead time.

---

## 4. Project serialization

**`src/project.cpp`**

Binary format with a magic number and version field at the top:

```
[MAGIC: u32] [VERSION: u32] [fields...]
```

`MAGIC = 0x534D5001` ("PMS\x01"). Current `VERSION = 7`.

### Versioning rule

When adding a new field:
1. Bump `VERSION`
2. Write the field unconditionally in `write_clip` / `project_save`
3. Wrap the read in `if (version >= N)` in `read_clip` / `project_load`
4. Set a sensible default in the else branch

Never reorder existing fields. Never remove version guards. Older files must always load cleanly.

---

## 5. Video preview — the proxy system

**`src/video.h`, `src/proxy.h`**

Decoding H.264/HEVC in real time on the main thread is too slow for interactive scrubbing. PMS generates a proxy: a folder of MJPEG frames at a lower resolution. Scrubbing seeks to the right frame in the proxy via a pre-built seek table (fseek + stb_image), which is nearly instant.

### Slot system

Each video clip gets its own decoder slot (0–7, `MAX_VIDEO_TRACKS = 8`). Slots are keyed by a **composite string**:

```
source_path + '\x01' + clip_start_time
```

Not just the source path. Two clips from the same file (e.g. after a split) get independent slots so their decoders don't fight over seek position.

The slot table is `AppState::proxy_paths[MAX_VIDEO_TRACKS]`. It is **not serialized** — it is rebuilt at runtime.

### Slot lifecycle

```
slot_for_video(key, src)   ← assigns slot number, registers key
video_open_still(slot)     ← shows a JPEG thumbnail while proxy generates
video_open_proxy(slot, pi) ← opens MJPEG + seek table for scrubbing
video_close(slot)          ← frees decoder
```

The proxy poll loop (in `ui_studio`) runs every frame and handles the state machine:
- Slot assigned, proxy ready → `video_open_proxy` directly
- Slot assigned, no proxy yet → `video_open_still` as placeholder
- Already a full proxy (`fps > 0`) → skip

`gc_video_slots` runs every frame to free slots whose clips have been deleted or moved.

### Frame request

```cpp
float src_t = clip.in_point + (playhead - clip.start) * clip.speed;
video_get_texture(slot, src_t + lookahead);
```

`lookahead` is one frame duration, pre-fetching the next frame to hide decode latency.

---

## 6. GPU FX pipeline

**`src/fx_shader.h`, `src/fx_shader.cpp`**

All visual effects (grade, blur, vignette, chroma-key, glitch, VHS, light-leak, datamosh) are implemented as GLSL fragment shaders running on the GPU. This replaces a 600-line CPU pixel loop that blocked the main thread during scrubbing.

### Effect accumulation

Two structs accumulate effect clip data above the current video track:

```cpp
EffectAccum     ea  = collect_effects    (state, t, track_idx);
CreativeFXAccum cfx = collect_creative_fx(state, t, track_idx);
```

`collect_effects` handles `FXType::Adjustment` clips (grade, blur, vignette, text-scale, text-opacity). `collect_creative_fx` handles everything else (glitch, datamosh, VHS, ZoomPunch, LightLeak, ChromaKey).

### fx_apply

```cpp
uintptr_t fx_apply(uintptr_t src_tex, int slot, int w, int h,
                   const EffectAccum& ea, const CreativeFXAccum& cfx, float t);
```

Runs the active passes in order: **grade+vignette → blur (2-pass) → chroma-key → glitch → VHS → light-leak → datamosh**. Returns `src_tex` unchanged if no FX is active.

The `slot` parameter (0–15) identifies which stable per-slot output texture to write to. Because ImDrawList defers rendering until the end of the frame, each slot must have its own stable output texture — shared ping-pong buffers would be overwritten before the draw commands execute.

### Pass infrastructure

| Buffers | Purpose |
|---|---|
| `g_pp[2]` | Scratch ping-pong textures + FBOs for intermediate passes within one `fx_apply` call |
| `g_out[16]` | Stable per-slot output textures; returned to callers |
| `g_ghost` | Persistent datamosh ghost buffer, keyed by `clip_start` |

Each pass uses a fullscreen triangle (`gl_VertexID` trick, no VBO) and a dedicated GLSL program.

### Datamosh ghost

The datamosh effect needs per-frame persistent state. `g_ghost` holds a single RGBA texture sized to the video frame. Each frame:

1. **Output pass**: `blend(current_frame, ghost) → g_pp[A]`
2. **Update pass**: `mix(ghost, current_frame, decay) → g_pp[B]` (reads old ghost — still safe)
3. **Commit**: blit `g_pp[B]` → `g_ghost.fbo` (overwrites `g_ghost.tex`)
4. Output tex is `g_pp[A]`, then copied to `g_out[slot]`

The ghost resets (re-seeds from the current frame) when `cfx.datamosh_clip_start` changes.

### Background removal

The `bg_remove` effect stays CPU-side because it reads pre-rendered PNG alpha masks from disk (produced by a background rembg process). `video_set_pixel_fx` is called with only the bg_remove fields populated; all other FX are handled by `fx_apply`.

### Call sites

**Preview (`screen_studio.cpp`, `draw_vid_clip` lambda)**:
```
video_set_pixel_fx(slot, pfx_bg_only)  ← CPU: applies bg_remove mask
tex = video_get_texture(slot, src_t)   ← uploads proxy MJPEG to GL
tex = fx_apply(tex, slot, w, h, ea, cfx, t)  ← GPU: all other FX
dl->AddImageQuad(tex, ...)             ← draw
```

**Export (`render.cpp`, `gl_render_vid_clip`)**:
```
vf = video_decode_frame_at(src_t)      ← libavcodec, exact frame
glTexImage2D(tex_id, ..., vf->data)    ← upload RGBA to GL
tex = fx_apply(tex_id, slot, w, h, ea, cfx, t)  ← GPU FX
dl.AddImageQuad(tex, ...)              ← draw into export FBO
```

---

## 7. Audio pipeline

**`src/audio.h`, `src/audio.cpp`**

### Master clock

`g_read_pos` is a 64-bit sample counter that advances by `frameCount * 2` every audio callback invocation, unconditionally. It is the **timeline clock**, not a source read cursor. `audio_position()` converts it to seconds for playhead sync.

`audio_seek(t)` sets `g_read_pos = t * 44100 * 2`, repositioning the clock.

### A/V sync

The playhead is driven by the audio callback position, not a wall clock:

```cpp
pos = audio_position() - audio_latency();
```

Wall clock is the fallback when no audio is loaded or audio is still loading. This gives lip-sync quality synchronization at any scrub position. The latency correction (`audio_latency()`) accounts for the audio output buffer depth so the visual frame matches the audible audio rather than the buffered-ahead sample.

### Three audio sources

The callback mixes three sources every invocation:

**1. Video-embedded audio (`g_samples`)**
Decoded from the video source file by a background ffmpeg subprocess into a flat `float[]` PCM buffer at 44100 Hz stereo. The callback reads from this buffer at a position determined by the active Video clip's `in_point` — NOT linearly from `g_read_pos`. Gaps between video clips produce silence.

**2. Timeline Audio clips (`g_src_bufs`)**
Each unique source file used by an `Audio`-type clip gets its own PCM buffer, decoded on demand by `audio_source_ensure()`. The callback reads each active Audio clip from its buffer at the correct `in_point` offset.

**3. Silence**
Anything not covered by a clip. The callback initializes the output buffer to zero; only active clips add to it.

### Clip snapshots

The audio callback runs on a separate thread. It cannot safely read `AppState`. Each frame, the main thread pushes two snapshots under a mutex:

```cpp
video_audio_clips_update(video_descs);  // Video clips with embedded audio
audio_clips_update(audio_descs);        // Audio-track clips
```

The callback uses `std::try_to_lock` — if the mutex is held by the main thread, it skips mixing for that callback invocation (one ~10ms dropout is inaudible). It never blocks.

---

## 8. GL export pipeline

**`src/render.cpp` — `render_start_gl`, `render_tick_gl`**

The GL export pipeline renders directly with OpenGL — the same path as the live preview — into an offscreen FBO, then pipes raw RGBA pixels to ffmpeg via stdin. This guarantees pixel-exact correspondence between preview and export.

### Render FBO

Created once per export in `render_start_gl`:

```
glGenFramebuffers(1, &fbo);
glFramebufferTexture2D(..., color_tex, ...)   ← RGBA, out_w × out_h
```

Resolution: 1080×1920 (Vertical), 1920×1080 (Horizontal), 1080×1080 (Square).

### Per-frame render (render_tick_gl)

Called from `app_frame` every app frame. Advances one export frame per app tick.

```
for each track (high index first — lowest layer drawn first):
  gl_render_vid_clip(dl, clip, t, alpha, tex_id, fx_slot, W, H, state, ti)
draw_text_overlays(&dl, state, t, {0,0}, W, H)
ImGui_ImplOpenGL3_RenderDrawData(&draw_data) → export FBO
glReadPixels → flip rows → write to ffmpeg pipe stdin
```

### gl_render_vid_clip

For each video clip:
1. `video_decode_frame_at(src_t)` — libavcodec frame-accurate seek + decode → RGBA
2. `glTexImage2D(tex_id, ..., vf->data)` — upload to one of 16 pre-allocated video textures
3. `fx_apply(tex_id, fx_slot, w, h, ea, cfx, t)` — GPU FX (§6 above)
4. ZoomPunch transform applied to quad geometry
5. `dl.AddImageQuad(fx_tex, ...)` — draw to the offscreen FBO

The `fx_slot` equals `ti % MAX_VIDEO_TRACKS` for the primary clip and `MAX_VIDEO_TRACKS + slot_pri` for the secondary (transition) clip, mirroring the preview slot scheme.

### Video texture pool

`g_gl_ex.vid_tex[MAX_VIDEO_TRACKS * 2]` — 16 pre-allocated GL textures. Each track uses two slots (primary + secondary for transitions). `glTexImage2D` is called every frame, which reallocates on GPU if dimensions change; on most hardware this is zero-copy if size is constant.

### ffmpeg pipe

`render_start_gl` forks ffmpeg with:
```
ffmpeg -f rawvideo -pix_fmt rgba -s WxH -r fps -i pipe:0
       [audio inputs...]
       -c:v libx264 -crf N -preset P out.mp4
```

Raw RGBA frames are written one-by-one to `pipe_write` (ffmpeg's stdin). The export thread writes; the GL render runs on the main thread. Progress is `current_frame / total_frames`.

### Snapshot (render_snapshot_gl)

`render_snapshot_gl` creates a temporary FBO at export resolution, renders one frame using the same pipeline as `render_tick_gl`, reads the pixels, flips them (GL origin is bottom-left), and saves as PNG via stb_image_write. The snapshot path re-uses `gl_render_vid_clip` so snapshots are always pixel-identical to export frames.

---

## 9. Text rendering

**`src/overlay_renderer.h`, `src/overlay_renderer.cpp`**

`draw_text_overlays(dl, state, t, p, w, h)` renders all active Text/Lyrics/Subtitle clips onto an `ImDrawList` using the embedded Inter Black font.

### Font size

Font size is stored as a **fraction of canvas height** (`clip.font_size`), not pixels. Default is `h * 0.055f`. This ensures that the text appears at the same visual proportion whether rendered onto a 500 px preview canvas or a 1920 px export FBO:

```cpp
float fsz = active->font_size > 0.f ? active->font_size * h : h * 0.055f;
```

### Word wrap

Exact word wrapping uses `ImFont::CalcTextSizeA` with the export font size, ensuring the wrap in the preview matches the export exactly.

### Animation styles

| Style | Effect |
|---|---|
| Fade | Alpha in/out ramp |
| Glitch | Sinusoidal horizontal offset with decay |
| Typewriter | Alpha + upward slide |
| Bounce | Damped sine vertical bounce |
| Slide | Horizontal slide in/out |
| Stack | Vertical drop-in |
| Block | Solid background rect, inverted text color |

`eff_style` is the per-clip override if set, otherwise the project-level `state.style`.

---

## 10. Timeline UI

**`src/ui/screen_studio.cpp` — `draw_timeline()`**

### Rendering order

For each track, the timeline runs two separate loops:

1. **Clip loop** — draws clip bodies, handles click/drag to select and move clips
2. **Glass loop** — draws transition overlays at every adjacent Video clip cut point, handles glass drag

This ordering matters: the clip loop fires first, so `s_trans_hit_this_frame` (set by the glass loop) cannot suppress clip clicks for the current frame. The fix is that when a glass click is detected, `drag_track = -1` is set immediately to cancel any stray clip drag the clip loop already registered.

### Coordinate system

```
screen_x = origin.x + TL_LABEL_W + clip.start * zoom - tl_scroll
clip_time = (screen_x - origin.x - TL_LABEL_W + tl_scroll) / zoom
```

`TL_LABEL_W` is the track label column width. All hit testing uses `vis_x0 / vis_x1` (clamped to the visible area) for clip body, and `cx0 / cx1` (unclamped) for drag math.

---

## 11. Transition system

**`src/app.h` — `TransitionType`, `transition_pre`, `transition_post`**

Transitions are stored on the **outgoing** clip (clip A). The glass overlay spans `[clip_A.end - pre, clip_A.end + post]` straddling the cut.

```
clip A: ──────────[████████████]
clip B:                    [████████████]──────────
                  |←  pre →|←  post  →|
                       cut point
```

- `transition_pre` — how far the transition reaches back into clip A
- `transition_post` — how far it reaches forward into clip B

During preview blending, normalized time `t_norm` runs 0→1 over the full zone. For Dissolve: `alpha_A = 1 - t_norm`, `alpha_B = t_norm`. Their sum is always 1 — no black frames.

The `in_trans_out` branch fires when the active clip is A (playhead in `[cut-pre, cut)`). The `in_trans_in` branch fires when the active clip is B (playhead in `[cut, cut+post)`). In `in_trans_in`, clip A's last frame is drawn at `1-t` alongside clip B at `t` for Dissolve. FadeBlack goes black at the cut intentionally.

---

## 12. ML pipeline

**`src/transcribe.*`, Python subprocess**

Transcription runs as an external Python process using WhisperX (faster-whisper backend) and Demucs for stem separation. PMS communicates via:
- Stdin/stdout for progress updates (JSON lines)
- Output JSON files: `*_words.json`, `*_segments.json`

The pipeline stages are: `Extract → Transcribe → Align → Done`. Progress is polled each frame via `PipelineStatus`. The Python binary path is user-configurable (`state.python_path`).

**Model locations:**
- WhisperX / faster-whisper: HuggingFace hub cache (`~/.cache/huggingface/hub/`)
- Demucs weights: torch hub checkpoints (`~/.cache/torch/hub/checkpoints/`)

---

## 13. Undo / history

History is a stack of `AppState` snapshots. `history_push(state, label)` copies the entire struct. Undo pops and restores. This is simple and correct but memory-heavy for large projects. The clip word lists (`std::vector<WordEntry>`) and keyframe data are the main contributors to snapshot size.

The history stack is cleared on project load, new project, and after the ML pipeline completes (since those operations are not incrementally undoable).

---

## File map

```
src/
  main.cpp              Entry point, GL/ImGui init, main loop
  app.h                 AppState, Clip, Track, all data model types
  app.cpp               app_init, app_frame, app_shutdown; collect_effects/creative_fx
  project.cpp           Binary save/load (versioned)
  audio.h / audio.cpp   miniaudio device, clip-based PCM mixing
  video.h / video.cpp   MJPEG proxy preview, GL texture upload, CPU bg_remove
  proxy.h / proxy.cpp   Proxy generation (ffmpeg subprocess)
  fx_shader.h/.cpp      GPU FX pipeline: GLSL shaders + fx_apply()
  overlay_renderer.h/.cpp  ImDrawList text/subtitle rendering (preview + export)
  render.h / render.cpp GL export pipeline + ffmpeg pipe; snapshot
  transcribe.h/.cpp     ML pipeline subprocess management
  presets.h/.cpp        User effect presets (JSON)
  ui/
    screen_studio.cpp   Main editor UI (timeline, preview, panels) — largest file
    screen_splash.cpp   Splash screen
    screen_setup.cpp    First-run setup
```

---

## Things that will surprise you

- **`g_read_pos` is a clock, not a cursor.** It advances unconditionally. Source position is always computed from clip data.
- **Proxy slots are per clip instance, not per source file.** Two clips from the same video get two slots.
- **The glass loop runs after the clip loop.** `s_trans_hit_this_frame` cannot suppress the clip loop for the same frame — the cancel is done by resetting `drag_track`.
- **`Keyframe.time` is relative to `clip.start`.** `eval_prop` takes absolute playhead time and handles the offset.
- **`in_point` must be advanced on split.** `right.in_point += (cut - left.start) * left.speed`. All four split sites do this.
- **The proxy poll loop opens slots it didn't open before.** Any slot that gets assigned (by `slot_for_video`) will be opened by the poll loop within one frame.
- **Font size is a fraction of canvas height, never fixed pixels.** `h * 0.055f` default. This is why preview and 4K export look proportionally identical.
- **fx_apply uses per-slot stable output textures.** Shared ping-pong buffers would be stale by the time the deferred ImDrawList flushes. Each slot (0–15) owns its own output texture.
- **bg_remove stays CPU-side.** It reads pre-rendered PNG alpha masks from disk; everything else runs through GLSL shaders via fx_apply.
- **Datamosh ghost resets on clip_start change.** Moving or replacing a datamosh clip restarts the ghost accumulation from the first visible frame.
- **The audio master clock drives the video.** `playhead = audio_position() - audio_latency()`. Wall clock is only the fallback for no-audio projects.
