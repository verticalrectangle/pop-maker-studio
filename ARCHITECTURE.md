# Pop Maker Studio — Architecture

A developer reference covering the major structural decisions, data flow, and non-obvious invariants in the codebase. Read this before touching anything in `src/`.

---

## 1. The big picture

PMS is a native C++ desktop application with no framework dependencies. The UI is built with [Dear ImGui](https://github.com/ocornut/imgui) in immediate mode. All application state lives in one struct. There is no event system, no observer pattern, no reactive bindings — every frame the entire UI is re-derived from `AppState` and drawn from scratch.

```
main loop
  └── app_frame(state)
        ├── update playhead (wall clock or audio position)
        ├── ui_studio(state)           ← timeline, preview, panels
        │     ├── draw_timeline()
        │     ├── draw_preview()
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

## 6. Audio pipeline

**`src/audio.h`, `src/audio.cpp`**

### Master clock

`g_read_pos` is a 64-bit sample counter that advances by `frameCount * 2` every audio callback invocation, unconditionally. It is the **timeline clock**, not a source read cursor. `audio_position()` converts it to seconds for playhead sync.

`audio_seek(t)` sets `g_read_pos = t * 44100 * 2`, repositioning the clock.

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

### Background decode

Audio decode (both `audio_load` and `audio_source_ensure`) forks ffmpeg as a subprocess rather than using libavcodec on the background thread. This avoids concurrent libavformat usage with the video subsystem, which is not thread-safe.

```
fork() → execvp("ffmpeg", [..., "-f", "f32le", TMP]) → waitpid() → read TMP into buffer
```

Per-clip temp files are named by `std::hash` of the source path to avoid conflicts between concurrent loads.

---

## 7. Timeline UI

**`src/ui/screen_studio.cpp` — `draw_timeline()`**

### Rendering order

For each track, the timeline runs two separate loops:

1. **Clip loop** — draws clip bodies, handles click/drag to select and move clips, sets `drag_track`/`drag_clip`
2. **Glass loop** — draws transition overlays at every adjacent Video clip cut point, handles glass drag

This ordering matters: the clip loop fires first, so `s_trans_hit_this_frame` (set by the glass loop) cannot suppress clip clicks for the current frame. The fix is that when a glass click is detected, `drag_track = -1` is set immediately to cancel any stray clip drag the clip loop already registered. The clip drag update block is also gated on `s_glass_drag == 0`.

### Scroll/zoom separation

The timeline child window uses `NoScrollWithMouse | NoScrollbar`. All scroll and zoom are handled manually:
- **Ctrl + scroll** → zoom (`tl_zoom`)
- **Scroll in ruler** → horizontal pan (`tl_scroll`)
- **Scroll in track body** → vertical track scroll (`tl_v_scroll`)

This prevents the parent window from consuming scroll events.

### Coordinate system

```
screen_x = origin.x + TL_LABEL_W + clip.start * zoom - tl_scroll
clip_time = (screen_x - origin.x - TL_LABEL_W + tl_scroll) / zoom
```

`TL_LABEL_W` is the track label column width. All hit testing uses `vis_x0 / vis_x1` (clamped to the visible area) for clip body, and `cx0 / cx1` (unclamped) for drag math.

---

## 8. Transition system

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

For render (ffmpeg filtergraph), see `src/render.cpp` — `TransInfo` structs are built per-clip-pair and fed into `colorchannelmixer=aa=` expressions.

---

## 9. ML pipeline

**`src/transcribe.*`, Python subprocess**

Transcription runs as an external Python process using WhisperX (faster-whisper backend) and Demucs for stem separation. PMS communicates via:
- Stdin/stdout for progress updates (JSON lines)
- Output JSON files: `*_words.json`, `*_segments.json`

The pipeline stages are: `Extract → Transcribe → Align → Done`. Progress is polled each frame via `PipelineStatus`. The Python binary path is user-configurable (`state.python_path`).

**Model locations:**
- WhisperX / faster-whisper: HuggingFace hub cache (`~/.cache/huggingface/hub/`)
- Demucs weights: torch hub checkpoints (`~/.cache/torch/hub/checkpoints/`)

---

## 10. Undo / history

History is a stack of `AppState` snapshots. `history_push(state, label)` copies the entire struct. Undo pops and restores. This is simple and correct but memory-heavy for large projects. The clip word lists (`std::vector<WordEntry>`) and keyframe data are the main contributors to snapshot size.

The history stack is cleared on project load, new project, and after the ML pipeline completes (since those operations are not incrementally undoable).

---

## File map

```
src/
  main.cpp              Entry point, GL/ImGui init, main loop
  app.h                 AppState, Clip, Track, all data model types
  app.cpp               app_init, app_frame, app_shutdown
  project.cpp           Binary save/load (versioned)
  audio.h / audio.cpp   miniaudio device, clip-based PCM mixing
  video.h / video.cpp   MJPEG proxy preview, GL texture upload
  proxy.h / proxy.cpp   Proxy generation (ffmpeg subprocess)
  render.cpp            FFmpeg filtergraph export
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
- **The proxy poll loop opens slots it didn't open before.** Any slot that gets assigned (by `slot_for_video`) will be opened by the poll loop within one frame, even if `add_clip_to_track` was never called (e.g. after a split or project load).
