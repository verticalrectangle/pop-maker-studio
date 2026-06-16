# AGENT_HARNESS — In-App Agent (DeepSeek API, for now)

Status: draft v1 (2026-06-10) · target: dev branch, personal use

An "Agent" button next to Terminal in the top bar opens a terminal-like chat
panel. The panel drives the editor through the same tool surface external
agents already use, with inference on the DeepSeek API today and any
OpenAI-compatible endpoint tomorrow (vLLM, llama.cpp, Ollama — that is the
WEB_SPEC §3.6 local tier for free).

---

## 1. Architecture at a glance

```
┌─ app process ──────────────────────────────────────────────┐
│                                                            │
│  Agent panel (ImGui, main thread)                          │
│    transcript view · input box · stop · status             │
│        ▲ snapshots of AgentChat state (mutex)              │
│        │                                                   │
│  agent worker (std::thread)                                │
│    chat loop: POST /chat/completions (stream)              │
│        │ tool_calls                                        │
│        ▼                                                   │
│  unix-socket client → /tmp/pop-maker-studio.sock           │
│        (the app talks to ITSELF as a normal IPC client)    │
│                                                            │
└────────────────────────────────────────────────────────────┘
         │ HTTPS (curl subprocess, SSE)
         ▼
   DeepSeek API  /  any OpenAI-compatible base URL
```

Key decisions:

- **The harness is an IPC client of its own app.** Tool calls connect to the
  same unix socket the MCP server uses. Zero dispatcher refactor, and full
  parity: auto-batch undo, `quiet` acks, `agent_status` pill, identical
  behavior to an external agent. Requests are served by `ipc_server_poll` on
  the main thread within a frame; the worker thread blocks on the socket
  exactly like server.py does.
- **HTTP via curl subprocess** (`curl -sN --no-buffer`), house style (ffmpeg,
  secret-tool are already subprocesses). No new link dependencies. Streaming
  is SSE: read `data: {...}` lines off the pipe, accumulate deltas.
- **One worker thread per agent turn cycle**, never touching ImGui. The panel
  reads a mutex-guarded transcript; the worker appends to it. Stop = flag
  checked between deltas + SIGTERM to the curl child.

## 2. Tool schemas — generated from server.py

server.py stays the single source of truth; its hand-tuned descriptions are
the most curated prose in the repo and live best in Python.

- `python mcp_server/server.py --dump-tools` prints
  `[{name, description, inputSchema, category, audience}]` and exits — no
  socket, no MCP loop, no side effects.
- `tools/gen_agent_tools.py` renders that into
  `src/generated/agent_tools.h`: two raw-string literals —
  `AGENT_TOOLS_JSON` (the function declarations + `ipc` flag) and
  `AGENT_TOOLS_INDEX` (a categorized name map appended to the system prompt).
  **The generated header is committed** (same policy as
  `generated/fx_enum_entries.h`) so diffs show what a description change did
  to the C++ side.
- Regeneration is `python3 tools/gen_agent_tools.py`. It no longer rots
  silently: CMake runs `--check` as a build step (`check_agent_tools`,
  no-op if the python `mcp` deps are absent) and `tools/pre-commit-hook.sh`
  runs it plus the coverage test on commit.

### Taxonomy: categories + audience

`mcp_server/server.py` carries three small tables (`_CATEGORIES`,
`OPERATOR_TOOLS`, `MCP_IPC_ALIASES`) that are the single source of truth for
how the surface is organized:

- **Category** (read / timeline / text / fx / audio / transcript / export /
  project / capture / playback / system) groups the ~90 tools into a
  navigable map. The generator emits it as `AGENT_TOOLS_INDEX`, appended to
  the harness system prompt so the model can orient by domain instead of
  scanning a flat wall of functions.
- **Audience** — `operator` tools drive the live UI / hardware
  (`ui_input`, `get_canvas_geometry`, `set_monitor`, `set_camera_monitor`,
  `vrecord_start/stop`). They stay on the **external** MCP surface (a driver
  like Claude Code uses them) but the generator **withholds** them from the
  autonomous in-app harness, which shouldn't move its own mouse. Everything
  else is `agent`.

### MCP↔IPC vocabulary

The agent-facing MCP names are friendly verbs; some wrap a lower-level IPC
method under a different name. The map lives in `MCP_IPC_ALIASES` so it is
documented in exactly one place and the coverage test treats the wrapped
method as covered:

| MCP tool                | IPC method              |
|-------------------------|-------------------------|
| `add_chapter_marker`    | `add_marker`            |
| `remove_chapter_marker` | `remove_marker`         |
| `get_chapter_markers`   | `get_markers`           |
| `remove_background`     | `start_bg_remove`       |
| `process_body_fx_masks` | `start_body_fx_process` |

### Coverage guard

`tools/check_tool_coverage.py` greps the `method == "..."` dispatch out of
`ipc_server.cpp` and fails if any IPC method is neither exposed as an MCP
tool, an alias target, nor on the `INTERNAL_IPC` allowlist (the agent's own
loop, debug hooks, and status pollers that blocking tools already wait on).
This is what keeps the surface *complete*: a new IPC method that nobody wired
up to an agent tool fails the check instead of silently going unreachable.

### python_only tools — the server.py bridge

Not every MCP tool is an IPC passthrough — `detect_screen_activity`,
`make_contact_sheet`, transcript searches, etc. are implemented *in*
server.py. Rather than a hand-curated set, the generator computes the truth
mechanically: it greps the actual `method == "..."` dispatch strings out of
ipc_server.cpp and tags each tool `ipc: true/false` by intersection — there
is no list that can rot.

`ipc: true` tools (currently 66 of 87 agent tools) dispatch straight to the
editor socket. `ipc: false` tools route through a persistent **server.py child**
spawned lazily on first use: MCP over stdio, newline-delimited JSON-RPC —
the exact transport external agents use, so server.py stays the single
source of truth and the loop closes neatly (app → server.py child → back
into the app's own IPC socket for its editor calls). The child's stderr
goes to `/tmp/pms_agent_bridge.log`; `PMS_MCP_SERVER` / `PMS_MCP_PYTHON`
override the script path and interpreter. MCP image parts (contact sheets,
stills) are attached to the wire for vision models the same way snapshots
are. If python or server.py is missing, those tools fail with a clear
error and everything IPC-served keeps working.

## 3. The chat loop

OpenAI-compatible messages array:

1. System prompt: a short static editor briefing + the generated
   `AGENT_TOOLS_INDEX` (categorized tool map). Full per-tool schemas ride the
   `tools` field, not the prompt.
2. `POST {base_url}/chat/completions` with `model`, `messages`,
   `tools` (function declarations from the generated JSON), `stream: true`.
3. Stream deltas:
   - `content` deltas → append to the visible assistant bubble live.
   - `tool_calls` deltas → accumulate per index (id/name/arguments arrive as
     fragments) until `finish_reason`.
4. `finish_reason == "tool_calls"` → for each call: dispatch over the socket
   (JSON-RPC, string id, `quiet: true` injected by default for mutation
   tools), render a tool row in the transcript
   (`▸ set_clip_prop {...} → ok`), append the `role:"tool"` result message,
   loop back to 2.
5. `finish_reason == "stop"` → turn over, input re-enabled.

Guardrails:

- Tool result truncation: results over ~8 KB are truncated with a marker
  (the model can re-query with `quiet`/slim variants).
- Max tool iterations per user turn (default 24) → loop breaks with a
  visible "tool budget exhausted" row.
- Malformed tool-call JSON → error fed back as the tool result; the model
  gets one chance to repair before the loop aborts the turn.
- Context: rolling window — when the estimated prompt size crosses the
  model's limit, oldest turns are dropped pairwise (keep system + recent).
  v1 is dumb-but-predictable; summarization is future work.

## 4. Vision — sending the agent images

DeepSeek's vision-capable models (and every OpenAI-compatible vision server)
take images as `{"type":"image_url","image_url":{"url":"data:image/png;base64,…"}}`
content parts. The harness uses this so the in-app agent can *see* what it
edits — same workflow I (Claude) use over MCP:

- **Image-bearing tools.** `take_snapshot` is the canonical one: the harness
  special-cases it (issue IPC call → poll `get_snapshot_status` → read the
  PNG from disk). Any tool result containing a readable `.png`/`.jpg` path
  field gets the same treatment via a small adapter table.
- **Delivery.** OpenAI `role:"tool"` messages are text-only in most
  implementations, so image results are delivered as: a short text tool
  result ("snapshot saved to …, image attached") followed by an injected
  `role:"user"` message whose content is `[image_url part]` labeled
  `[tool output image: take_snapshot]`. This is the standard pattern and
  works across DeepSeek/vLLM/llama.cpp.
- **Downscaling.** Images are resized before base64 (longest edge ≤ 1024 for
  canvas/render frames) — stb_image + stb_image_resize are already vendored
  for other paths; encode with stb_image_write to PNG in memory.
- **Capability flag.** `agent_vision` toggle in settings (on by default for
  deepseek vision models; off sends a text-only result with the file path so
  non-vision models degrade gracefully).

## 5. Secrets & settings

- **API key in the Secret Service** (KWallet implements it): subprocess
  `secret-tool store --label "Pop Maker Studio Agent" service pms-agent key api`
  / `secret-tool lookup service pms-agent key api`. The key never touches the
  settings file or the project file. If `secret-tool` is missing, the
  settings UI says so and the field stays disabled — no fake-encrypted
  fallback.
- **Settings modal additions** (the existing `##settings_modal`):
  - API key field (password-masked) + Save / Clear buttons + keyring status
  - Base URL (default `https://api.deepseek.com`)
  - Model id (default `deepseek-chat`)
  - Vision toggle
  Base URL/model/vision live in the normal app config (not secret).
- No privacy banner in v1 (dev-branch personal tool); the WEB_SPEC consent
  language applies when this grows into a product surface.

## 6. Panel UI

- **Top bar**: `[Agent] [Terminal] [Project] [Export]` — Agent toggles
  `state.agent_panel_open`, same accent-tint active state as Terminal.
- **Placement**: a strip in the bottom zone like the terminal. When both the
  terminal and agent are open they share the strip side-by-side (50/50);
  alone, each takes the full width.
- **Transcript**: scrollable child window;
  - user rows (`>` prefix, fg color),
  - assistant text (streamed in place),
  - tool rows: single line `▸ name {args ≤120 chars} → ok/err`, dim; click
    expands full args/result in a popup,
  - image rows: thumbnail of what was sent to the model.
- **Input**: single-line InputText (Enter sends; Shift+Enter newline is
  future work), Stop button while a turn runs, "·· thinking" shimmer while
  waiting for first token.
- **State**: `AgentChat` struct (messages, streaming buffer, run state) is a
  file-local static in panel_agent.cpp guarded by a mutex; the worker thread
  owns mutation, the UI thread snapshots per frame. Chat history is
  session-only (not saved into .pms) in v1.

## 7. Files

| File | Role |
|---|---|
| `mcp_server/server.py` | `--dump-tools` mode + `PYTHON_IMPL_TOOLS` set |
| `tools/gen_agent_tools.py` | renders dump → header |
| `src/generated/agent_tools.h` | committed generated tool JSON |
| `src/agent_harness.cpp/.h` | worker thread, chat loop, SSE parse, IPC client, vision adapter |
| `src/ui/panel_agent.cpp/.h` | the panel |
| `src/ui/screen_studio.cpp` | Agent button, strip layout, settings fields |
| `src/app.h` | `agent_panel_open`, agent config fields |

## 8. Non-goals (v1)

- No multi-turn persistence across app restarts.
- No context summarization (rolling window only).
- No parallel tool calls (serial execution in call order).
- No python_only tool emulation — they're simply absent.
- No local model management UI (point base URL at Ollama and it works;
  download/UX comes with WEB_SPEC §3.6).
