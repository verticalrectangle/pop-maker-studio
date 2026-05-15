# Pop Maker Studio — MCP & Runtime FX Spec

This document defines the full design for:
1. **Runtime FX registry** — hot-reload custom effects from disk, no rebuild
2. **MCP server** — Claude-exclusive tool surface for effect generation and timeline editing
3. **IPC protocol** — how the MCP server talks to the running app
4. **AppState serialization** — what Claude reads about the project

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

**Batch semantics:** `begin_batch` pushes one `history_push` entry before the first edit. All subsequent edits in the batch are applied without additional history pushes. `end_batch` closes the batch. If the connection drops mid-batch, the batch is silently closed (partial edits remain in history).

---

## 3. MCP Tool Surface

The MCP server exposes two categories of tools to Claude.

### 3a. Effect tools

#### `register_effect`
Writes (or overwrites) `{effects_dir}/{id}.json`. The app picks it up within 500 ms.

Params:
- `id` string — snake_case identifier, unique
- `name` string — display name
- `category` string — "color" | "distort" | "blur" | "composite" | "creative" | other
- `description` string
- `params` array — `[{name, label, min, max, default, curve?}]`
- `glsl` string — shader body as described above

Returns: `{ok: true}` or `{ok: false, error: "..."}` after asking the app to validate-compile via IPC `validate_glsl` before writing.

#### `list_effects`
Returns all runtime effects currently in the registry (including compile status).

#### `delete_effect`
Removes `{id}.json` from the effects directory.

#### `validate_glsl`
Sends `validate_glsl` IPC call to the app, which attempts to compile the shader and returns success or the GL compile error. Use this before `register_effect` to give Claude immediate feedback on GLSL errors.

---

### 3b. Editing tools

#### `get_project`
Returns full project state. See Section 4 for schema.

#### `get_beats`
Returns `{bpm: 128.0, beats: [0.0, 0.47, 0.94, ...]}`. Empty if no beat analysis has run.

#### `begin_batch`
Pushes one undo history entry. All edits until `end_batch` are grouped under one undo step.

Params: `{label: "Sync clips to beat"}` — label shown in undo history.

Must be called before any editing tools. The app rejects edit calls that arrive outside a batch (returns error). This prevents Claude from making untracked edits.

#### `end_batch`
Closes the current batch. No params.

#### `move_clip`
Move a clip's start position (preserves duration).

Params: `{track: int, clip: int, start: float}`

#### `trim_clip`
Set clip start and/or end. Adjusts `in_point` correctly if start moves forward.

Params: `{track: int, clip: int, start?: float, end?: float}`

#### `split_clip`
Split a clip at `time`. Left half retains original, right half gets `in_point` advanced.

Params: `{track: int, clip: int, time: float}`

Returns: `{left_clip: int, right_clip: int}`

#### `delete_clip`
Delete a clip.

Params: `{track: int, clip: int}`

#### `add_clip`
Add a new clip to a track.

Params:
```json
{
  "track": 0,
  "type": "audio" | "video" | "text" | "effect" | "lyrics",
  "start": 10.0,
  "end": 14.0,
  "text": "/path/to/file.mp4"   // file path for audio/video, display text for text/lyrics
}
```

Returns: `{clip: int}` — index of the new clip.

#### `set_clip_prop`
Set a named property on a clip. Supported props:

| prop | type | notes |
|---|---|---|
| `volume` | float 0–2 | audio gain |
| `speed` | float 0.25–4 | playback speed |
| `opacity` | float 0–1 | video opacity |
| `muted` | bool | |
| `in_point` | float | seconds into source |
| `fade_in` | float | seconds |
| `fade_out` | float | seconds |
| `pos_x` | float 0–1 | horizontal position |
| `pos_y` | float 0–1 | vertical position |
| `scale_x` | float | |
| `scale_y` | float | |
| `rotation` | float | degrees |
| `text` | string | display text for text/lyrics clips |

Params: `{track: int, clip: int, prop: string, value: any}`

#### `add_track`
Add a new empty track.

Params: `{name: string, position?: int}` — position 0 = top. Default = top.

Returns: `{track: int}`

#### `delete_track`
Delete an entire track and all its clips.

Params: `{track: int}`

#### `seek`
Move the playhead.

Params: `{time: float}`

#### `play` / `pause`
Control playback. No params.

#### `trigger_pipeline`
Kick the ML pipeline on the current audio file.

Params: `{mode: "both" | "transcribe_only" | "separate_only"}`

#### `validate_glsl`
Ask the app to test-compile a GLSL shader body.

Params: `{glsl: string, params: [{name, min, max, default}]}`

Returns: `{ok: true}` or `{ok: false, error: "line 3: undeclared identifier 'colour'"}`.

---

## 4. AppState Serialization

`get_project` returns:

```json
{
  "duration": 210.5,
  "fps": 30,
  "bpm": 128.0,
  "beats": [0.0, 0.47, 0.94],
  "audio_path": "/home/alexis/music/track.wav",
  "playhead": 4.5,
  "tracks": [
    {
      "index": 0,
      "name": "Lyrics",
      "muted": false,
      "locked": false,
      "clips": [
        {
          "index": 0,
          "type": "lyrics",
          "start": 0.47,
          "end": 0.94,
          "text": "Hello",
          "in_point": 0.0,
          "duration": 0.47,
          "volume": 1.0,
          "speed": 1.0,
          "opacity": 1.0,
          "muted": false
        }
      ]
    }
  ]
}
```

Only fields useful for editing decisions are included. No internal GL state, no pixel data, no render status. Beat timestamps are truncated to 3 decimal places.

---

## 5. MCP Server implementation

The MCP server is a **TypeScript/Node.js** process that:
- Implements the MCP protocol (Claude Desktop standard)
- On each tool call, either writes to the effects directory or sends an IPC message to the app and awaits the response
- Finds the running app instance via `/tmp/pop-maker-studio.lock`
- Errors gracefully if no app instance is running

**Location:** `mcp/` directory at repo root.

```
mcp/
  src/
    index.ts        — MCP server entry point, tool registration
    ipc.ts          — Unix socket client, request/response handling
    effects.ts      — Effect file writing and validation
    tools/
      effect_tools.ts
      editing_tools.ts
  package.json
  tsconfig.json
```

**Claude Desktop config** (user adds to `claude_desktop_config.json`):
```json
{
  "mcpServers": {
    "pop-maker-studio": {
      "command": "node",
      "args": ["/path/to/pop-maker-studio/mcp/dist/index.js"]
    }
  }
}
```

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

## 7. Brick system (post-MCP)

The brick composer lets users build effects without writing GLSL. It is implemented on top of the runtime FX registry — the output of a brick composition is a generated `{id}.json` file written to the effects directory. The effect then loads like any other runtime effect.

**Primitive operations available in the brick composer:**

| Brick | GLSL it generates |
|---|---|
| Hue shift | `hue_rotate(color, amount)` |
| Brightness / contrast | standard formula |
| Pixelate | grid snap |
| Blur (box) | tap sum |
| Chromatic aberration | per-channel UV offset |
| Noise overlay | procedural noise |
| Edge detect | Sobel |
| Color grade | lift/gamma/gain |
| Mix | blend two upstream outputs |
| Feedback | reads previous frame (requires ghost buffer) |

Each brick has param sliders. The composer chains bricks left-to-right; the GLSL generator concatenates their bodies with the output of each feeding into the next as `color`.

The brick graph is stored in the `{id}.json` alongside the generated GLSL, in a `"bricks"` field. Re-opening a custom effect in the composer re-hydrates the brick graph. The GLSL is regenerated on any brick change and validated before saving.

---

## 8. Implementation order

1. **Runtime FX registry** (`src/runtime_fx.h/.cpp`, watch loop, FX picker integration, clip storage, render path)
2. **App IPC server** (`src/ipc_server.h/.cpp`, socket, lock file, all editing handlers, batch/undo)
3. **MCP server** (`mcp/`, TypeScript, all tools wired to IPC)
4. **Brick composer** (UI in `src/ui/panel_fx.cpp` or new `src/ui/brick_composer.cpp`, GLSL codegen)

Items 1–3 ship together as one PR. Item 4 follows.
