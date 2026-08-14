# Pop Maker Studio — MCP & Runtime FX Spec

Reference for the wire protocol, runtime FX registry, and IPC architecture that link Claude to the running app. **Per-tool reference lives in `mcp_server/server.py`** — every tool's description, parameter schema, and behaviour notes are there. This doc covers the protocol, the serialisation contract, and the moving pieces around the tool layer; do not duplicate per-tool documentation here.

1. **Runtime FX registry** — hot-reload custom effects from disk, no rebuild
2. **IPC protocol** — how the MCP server talks to the running app
3. **MCP tool surface** — categories, conventions, polling pattern
4. **AppState serialization** — what `get_project` returns
5. **MCP server implementation** — Python bridge, lock-file discovery
6. **App-side IPC implementation** — main-thread dispatch, auto-batching

---

## 1. Runtime FX Registry

### Effect file format

Each runtime effect lives as a single JSON file:

```
~/.local/share/pop-maker-studio/effects/{id}.json
```

Schema:
```json
{
  "id": "pixelate_pulse",
  "name": "Pixelate Pulse",
  "category": "distort",
  "description": "Pixel grid that grows on beat",
  "params": [
    { "name": "size",    "label": "Block Size", "min": 1.0,  "max": 64.0, "default": 8.0,  "curve": 0.5 },
    { "name": "softness","label": "Softness",   "min": 0.0,  "max": 1.0,  "default": 0.0 }
  ],
  "glsl": "vec2 grid = floor(uv * res / size) * size / res;\nreturn texture(tex, grid);"
}
```

**GLSL body contract:**
- Receives uniforms: `sampler2D tex`, `vec2 res` (pixel resolution), `float t` (time in seconds), one `float` uniform per param named exactly as `param.name`
- Must return `vec4` (final color)
- No `main()` — the app wraps it in a full fragment shader
- `uv` is available as `vec2 uv = gl_FragCoord.xy / res;`

**Param curve:** optional `0.5` = sqrt mapping (same as codegen effects). Applied at accumulation time.

**Amount:** every runtime effect automatically gets a system-level `amount` (0–1) that blends source and processed output, identical to generated effects. Not a user param.

### App-side registry (`src/runtime_fx.h / .cpp`)

```cpp
struct RuntimeFXParam {
    std::string name;
    std::string label;
    float min_val, max_val, default_val;
    float curve = 1.f;
};

struct RuntimeFXDef {
    std::string              id;
    std::string              name;
    std::string              category;
    std::string              description;
    std::vector<RuntimeFXParam> params;
    std::string              glsl_body;
    GLuint                   program = 0;    // 0 = compile failed
    std::string              compile_error;
    bool                     dirty = false;  // needs recompile
};

// Load / reload all effects from the watch directory.
// Called once at init and whenever the watcher detects a change.
void runtime_fx_reload(const std::string& dir);

// Returns all currently loaded (including failed) definitions.
const std::vector<RuntimeFXDef>& runtime_fx_list();

// Returns nullptr if id not found.
const RuntimeFXDef* runtime_fx_find(const std::string& id);

// Apply a runtime effect to src_tex → returns output GL texture.
// slot is the same per-slot stable texture index used by fx_apply (use slot 14 and 15 for runtime).
uintptr_t runtime_fx_apply(const std::string& id,
                             uintptr_t src_tex, int slot,
                             int w, int h,
                             const std::vector<float>& param_values,
                             float amount, float t);
```

### Clip-side storage

`Clip` gets two new fields:
```cpp
std::string              runtime_fx_id;              // id of active runtime effect ("" = none)
std::vector<float>       runtime_fx_params;           // one float per param, in registry order
float                    runtime_fx_amount = 1.f;
```

These are serialized in `project.cpp` under VERSION bump. Default: `runtime_fx_id` empty = no runtime FX active.

### Watch loop

`runtime_fx_reload` is called:
- At app init
- Every 500 ms from `app_frame` if the effects directory mtime has changed

On reload: for each `.json` in the directory, parse it, compile the GLSL program if the source has changed, update the registry. Effects that fail to compile stay in the registry with `compile_error` set — they appear in the FX picker grayed out with the error shown on hover.

### FX picker integration

Runtime effects appear in the FX picker under a **"Custom"** section at the bottom, after all generated effects. They show name + category. Failed effects are grayed. Applied exactly like generated effects — placing one on a clip sets `clip.runtime_fx_id` and initializes `clip.runtime_fx_params` to defaults.

### Render path integration

In `fx_apply` (and export path), after all static passes: if `clip.runtime_fx_id` is non-empty and `runtime_fx_find(id)` returns a valid compiled program, call `runtime_fx_apply`. Uses FX slots 14–15 (currently unused).

---

## 2. IPC Protocol

The app opens a **Unix domain socket** at `/tmp/pop-maker-studio-{pid}.sock` on startup. It writes its PID and socket path to `/tmp/pop-maker-studio.lock` so the MCP server can find it.

Communication: newline-delimited JSON. Each message is one JSON object on one line.

**Request** (MCP server → app):
```json
{"id": "req-1", "method": "get_timeline", "params": {}}
```

**Response** (app → MCP server):
```json
{"id": "req-1", "result": {...}}
```

or on error:
```json
{"id": "req-1", "error": "clip index out of range"}
```

The app processes IPC messages on the **main thread** between frames (polled from `app_frame`), so all edits are applied atomically with respect to rendering. No locking needed beyond the socket read.

**Batch semantics:** `begin_batch` pushes one `history_push` entry before the first edit. All subsequent edits in the batch are applied without additional history pushes. `end_batch` closes the batch. If the connection drops mid-batch, the in-flight changes are saved as `"<label> (incomplete)"` so nothing is lost.

**Auto-batching:** mutation calls received outside an explicit batch are wrapped in an implicit one-call batch (labelled with the method name) and committed when the call returns. So `add_clip` on its own is one undo step automatically; `begin_batch` is only needed to coalesce *multiple* mutations into one undo step.

---

## 3. MCP Tool Surface

The server registers ~70 tools spanning the full editing surface. **The canonical reference is `mcp_server/server.py`** — each tool's description, parameter schema, and behaviour notes are inline there. This section covers categories and cross-cutting conventions; do not duplicate per-tool tables here, they rot the moment a tool changes.

### Categories

| Category | Examples |
|----------|----------|
| **Project state** | `get_project`, `get_clips`, `get_all_clips`, `take_snapshot` |
| **Bin** | `add_to_bin`, `remove_from_bin` (bin contents surface via `get_project.bin`) |
| **Timeline edits** | `add_clip`, `add_clip_sequence`, `delete_clip`, `move_clip`, `trim_clip`, `split_clip`, `set_clip_prop`, `set_clip_props`, `set_text_style` |
| **Tracks** | `add_track`, `rename_track`, `delete_clips_after`, `trim_all_to` |
| **Effects / bricks** | `add_effect_brick`, `add_body_fx_brick`, `add_multifx_brick`, `add_callout`, `generate_typography` |
| **Markers** | `add_chapter_marker`, `remove_chapter_marker`, `generate_chapters` |
| **ML pipeline** | `trigger_pipeline`, `get_pipeline_status`, `get_transcript`, `read_transcript_context`, `search_transcript`, `analyze_audio`, `get_audio_analysis`, `find_audio_cue` |
| **Search / discovery** | `find_and_add_clip`, `find_video_moment`, `cut_at_phrase`, `cut_filler_words`, `remove_silence` |
| **Stock media** | `pexels_search`, `pexels_add_clip` |
| **Background removal** | `remove_background`, `process_body_fx_masks`, `get_bg_remove_status` |
| **Media probing** | `get_media_info`, `get_stills`, `describe_video`, `get_video_description` |
| **Multicam** | `apply_multicam_cuts` |
| **Style recipes** | `get_song_structure`, `list_style_recipes`, `get_style_recipe`, `animate_section` |
| **Media operations** | `crop_media`, `extract_clip_segment` |
| **Vision model** | `download_vision_model`, `get_vision_model_status` |
| **Playback** | `play`, `pause`, `seek` |
| **Export / project** | `trigger_export`, `cancel_export`, `get_export_status`, `take_snapshot`, `save_project`, `load_project`, `new_project`, `set_format` |
| **Runtime FX** | `validate_glsl` (register / list / delete handled by editing the JSON files directly — see Section 1) |
| **Batching** | `begin_batch`, `end_batch` (mostly optional — see auto-batching in Section 2) |
| **Cancellation** | `cancel_search`, `cancel_export` |

### Conventions

**Async-first.** Long-running mutations return immediately with a stage hint. `trigger_pipeline`, `analyze_audio`, `remove_background`, `find_and_add_clip`, `process_body_fx_masks` all return `{stage: "running"}` (or equivalent) and the caller polls a status endpoint (`get_pipeline_status`, `get_audio_analysis`, `get_bg_remove_status`, etc.) until `stage` is `done` or `error`. This keeps the MCP socket free during ML work.

**Auto-batching.** See Section 2 — single mutations are wrapped in an implicit batch labelled with the method name. `begin_batch`/`end_batch` is only needed when coalescing a sequence as one undo step.

**Stock media (Pexels).** `pexels_search` is read-only — no batch needed. It queries photos (`/v1/search`) or videos (`/v1/videos/search`), 15 per page, returning `{results: [{id, photographer, alt, duration, width, height}], page, has_more}`; the API key comes from `PEXELS_API_KEY` or the keyring (`secret-tool lookup service pexels key api`). `pexels_add_clip` resolves an id from the most recent search (search first!), downloads it to `~/.local/share/pop-maker-studio/pexels/{videos,photos}/pexels-<id>-<slug>.<ext>` — the same path the app's C++ Pexels browser uses, so files dedupe — then places the clip at the playhead on an empty/new track in one undo step.

**Bin vs timeline.** `add_to_bin` makes a media file available to the project without placing it. `add_clip` actually places. `add_clip` on a video/audio path automatically mirrors the file into the bin, so for direct placements just call `add_clip` and skip the bin step.

**Proxy-required tools.** `remove_background` and `process_body_fx_masks` fail synchronously if the source clip's MJPEG proxy isn't on disk yet (they read masks keyed by proxy frame index). Poll the source clip's `proxy_status` via `get_project` and retry when ready.

**Generic dispatch.** Tools that don't have explicit Python handlers (most of them) forward through the catch-all dispatcher at the bottom of `server.py` — the method name is sent verbatim over IPC. So the surface in `server.py` is also a near-1:1 reflection of the IPC methods in `ipc_server.cpp`.

---

## 4. AppState Serialization

`get_project` returns a **slim** view by default (track summaries only, no per-clip detail). Pass `{verbose: true}` for the full nested schema. Slim:

```json
{
  "duration": 210.5,
  "fps": 30,
  "bpm": 128.0,
  "audio_path": "/home/alexis/music/track.wav",
  "project_path": "/home/alexis/projects/song.pms",
  "transcript_ready": true,
  "playhead": 4.5,
  "tracks": [
    {"index": 0, "name": "Lyrics", "muted": false, "locked": false, "clip_count": 24}
  ],
  "markers": [
    {"index": 0, "time": 32.4, "label": "Chorus 1", "color": "#FF66CC"}
  ],
  "bin": [
    "/home/alexis/music/track.wav",
    "/home/alexis/footage/clip1.mov",
    "/home/alexis/footage/clip2.mp4"
  ]
}
```

Verbose adds `beats[]`, full nested `clips[]` per track (with `text`, `in_point`, `duration`, `volume`, `speed`, `opacity`, `muted`, transform, fade, FX, body_fx state, etc.), and the same `markers` + `bin` arrays.

Use `get_clips(track)` or `get_all_clips()` for per-clip detail without paying for the full state dump. Beat timestamps are truncated to 3 decimal places.

---

## 5. MCP Server implementation

The MCP server is a **Python** process using the official `mcp` SDK. Single-file implementation at `mcp_server/server.py`. It:

- Reads `/tmp/pop-maker-studio.lock` to discover the running app's socket path (errors gracefully if no app instance is running, and starts PMS lazily if the binary is on `PATH`).
- Registers ~70 tools at startup. A handful have explicit Python handlers (the ones doing client-side orchestration like `find_and_add_clip`'s windowed search loop, or `remove_background` which adds a body_fx brick before calling the IPC `start_bg_remove`); the rest forward through a generic dispatcher that sends the method name verbatim over the socket.
- Marshals tool results as `TextContent` (JSON `dumps`'d). MCP progress notifications are forwarded from the C++ search status via `_send_search_progress` so the client sees MDX-Net / Whisper / window progress live.

**Location:** `mcp_server/` directory at repo root.

```
mcp_server/
  server.py         — single-file server: tool registration, dispatch, helpers
  requirements.txt  — mcp>=1.0
```

**Setup snippets** for Claude Desktop and Claude Code live in the main `README.md` under "MCP server / Setup".

---

## 6. App-side IPC implementation

**`src/ipc_server.h / .cpp`**

```cpp
// Call once at app init. Opens the Unix socket and writes the lock file.
void ipc_server_start();

// Call every frame from app_frame. Reads pending messages, dispatches
// them against AppState, writes responses. Non-blocking.
void ipc_server_poll(AppState& state);

// Call at shutdown. Closes socket, removes lock file.
void ipc_server_stop();
```

All IPC handlers run on the main thread inside `ipc_server_poll`. History push for `begin_batch` happens here. No additional locking required.

---

