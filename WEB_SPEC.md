# WEB_SPEC — Pop Maker Studio as a Private Cloud Editor

*"The editor runs in a locked room; the AI lives in the room with it; when you
leave, the room is incinerated — and you can check the ashes."*

Status: draft v1 (2026-06-10)

---

## 1. What we are promising (and what we are not)

ProtonMail can promise end-to-end encryption because it never needs to read
your mail. A video editor backend is the opposite: the server must decode
footage to proxy it, transcribe it, and render it. **True E2EE-while-editing
does not exist for this workload** — FHE on video is fantasy, and claiming
E2EE would be an overclaim that burns us with exactly the users we want.

The honest, sellable promise is **confidential ephemeral compute**:

1. **Plaintext exists only inside an ephemeral, per-session box.**
2. **Nothing derived from user content persists after the session** — no
   media, no proxies, no transcripts, no logs containing content.
3. **The AI never leaves the box.** Model inference is self-hosted inside the
   same trust boundary; no user content ever flows to a third-party API.
4. **Destruction is structural, not policy.** The session box has no
   persistent storage by construction; teardown is destruction.
5. **Verifiable.** Confidential-VM attestation + reproducible builds let a
   user check *which binary* ran against their content (premium tier).

Marketing shorthand: "the ProtonMail of video editors." Technical truth:
zero-retention + attested ephemeral compute. Keep both sentences in the docs.

---

## 2. Why the existing codebase is unusually close to this

The C++ engine is already fully drivable headless-style over a Unix-socket
JSON-RPC IPC (`/tmp/pop-maker-studio.sock`, `src/ipc_server.cpp`). The entire
agent surface built to date — mutations with `quiet` acks, `set_clip_keyframes`,
`take_snapshot` (render/canvas/ui), `agent_status`, batches, export, pipeline
triggers — **is the session API**. The MCP server (`mcp_server/server.py`) is
already the agent adapter on top of it.

What the engine assumes today that a server must not:

| Assumption | Where | Server replacement |
|---|---|---|
| A window + swapchain | GLFW/SDL init, ImGui backend | EGL surfaceless context, `--server` flag |
| Desktop file pickers | `filepicker_open` | disabled in server mode; assets arrive via upload service |
| One global session | `g_scene`, audio `g_clips`/`g_src_bufs`, proxy slots | **process-per-session** (which the isolation model wants anyway) |
| Local media paths | `add_clip text=/path` | per-session tmpfs paths handed out by the asset service |
| UI screenshot/backbuffer reads | `source=canvas` / `source=ui` snapshots | `canvas` reads the offscreen FBO; `ui` is meaningless server-side (disabled) |

ML inference (WhisperX via faster-whisper, MDX-Net, vision describe) is already
local subprocess work — it moves to a GPU job queue without architectural
change.

The MJPEG proxy system is the preview transport almost for free: the
`.pms_proxy.mjpeg` + `.idx` seek-table format streams naturally over a
WebSocket to a `<canvas>` client.

---

## 3. Architecture

```
                 ┌────────────────────────────────────────────────┐
                 │  SESSION BOX (per user-session, ephemeral)     │
  thin web       │  Firecracker/Kata microVM · tmpfs only         │
  client  ──TLS──┤                                                │
  (browser)      │  ┌──────────┐   unix sock   ┌──────────────┐  │
   │             │  │ engine    │◄────────────►│ session agent │  │
   │ E2E-enc     │  │ (headless │               │ (MCP server + │  │
   │ upload      │  │  EGL)     │               │  agent loop)  │  │
   ▼             │  └────┬─────┘                └──────┬───────┘  │
 ┌─────────┐     │       │ proxies/transcripts/render  │ tokens   │
 │ asset    │────┤       ▼                             ▼          │
 │ gateway  │    │   tmpfs scratch (RAM, per-session key)        │
 └─────────┘     └───────────────┬───────────────┬────────────────┘
                                 │ jobs          │ inference
                          ┌──────▼─────┐   ┌─────▼──────────┐
                          │ GPU job     │   │ token pool      │
                          │ queue       │   │ (self-hosted    │
                          │ (WhisperX,  │   │  DeepSeek on    │
                          │  MDX-Net,    │   │  vLLM/SGLang)   │
                          │  vision)    │   └────────────────┘
                          └────────────┘
```

### 3.1 Session box

- One microVM (Firecracker or Kata) per active editing session. Premium tier:
  confidential VM (AMD SEV-SNP / Intel TDX; GPU TEE where render/inference
  share the box).
- Contents: one engine process (headless), one MCP/agent process, the IPC
  socket between them, RAM-backed tmpfs for *all* derived state.
- No persistent volume is ever attached. No content-bearing logs leave the
  box; the only telemetry that escapes is structural (session duration,
  resource usage, error codes — no strings derived from media or transcripts).
- Idle timeout + explicit "end session" both trigger teardown: kill VM, scrub
  per-session scratch key, emit a **signed deletion receipt** (session id,
  teardown time, measurement hash) to the client.

### 3.2 Engine in server mode

New `--server` flag:

- EGL surfaceless GL context; no window, no input backend. `render_start_gl`,
  `render_snapshot_gl`, the scene compositor, and proxy generation are
  unchanged — they only need a context, not a surface.
- `take_snapshot`: `render` works as-is; `canvas` reads the offscreen
  composite FBO instead of the backbuffer; `ui` returns an error
  ("server mode has no UI").
- UI-only code (panels, timeline drawing, filepicker, transport pill) compiled
  out or short-circuited; the IPC dispatcher is the only entry point.
- Project save/load points at tmpfs; autosave loop writes
  `/scratch/session.pms` so an agent crash never loses state inside the
  session (still destroyed at teardown).

Engine state is global singletons, so multi-tenancy is process-per-session by
necessity — which is exactly what the isolation model wants. No refactor to
multi-session is planned or desired.

### 3.3 Thin web client

- Browser app: timeline UI, canvas preview, chat/agent panel.
- Speaks the existing IPC JSON-RPC over WebSocket (gateway terminates TLS,
  pipes into the session box's socket). The protocol already exists; the web
  client is a new frontend to it, not a new backend.
- Preview: MJPEG proxy frames streamed over the socket and painted to
  `<canvas>`; audio via a downmixed Opus stream rendered server-side from the
  same mixer that drives preview (volume/pan keyframes included).
- Scrub = `seek` + indexed proxy frame fetch (the `.idx` seek table makes
  random access cheap).
- Client-side encryption of uploads (per-session key, e.g. age/AES-GCM);
  the key is released only to the attested session box.

### 3.4 Asset gateway

- Accepts encrypted uploads, streams them into the session box, decrypts
  *inside* the box only.
- Exports: rendered MP4 is encrypted with the session key inside the box,
  offered to the client as a download, then wiped with everything else.
- No server-side library/galleries in v1. If users want persistent projects,
  that's a separate, explicit, encrypted-at-rest opt-in ("vault") with its own
  spec — default remains destroy-on-exit.

### 3.5 Agent + token pools (the DeepSeek part)

The privacy promise dies if the agent calls a third-party LLM API with
transcripts and frame snapshots in the prompt. Therefore:

- **Self-hosted open weights.** DeepSeek's open releases (V3-family, R1,
  distills) served on our own GPUs via vLLM or SGLang. No user content leaves
  the trust boundary for inference, ever.
- **Token pool = scheduler, not API keys.** Pooled inference capacity with:
  - per-user/per-plan token budgets drawn from the pool,
  - shared prefix cache — system prompt + tool schemas are identical across
    every session, so the expensive prefix is computed once per replica,
  - continuous batching across tenants,
  - priority lanes (interactive agent turns > background jobs like chapter
    generation).
- **Model sizing is a unit-economics decision, not a tech one.** Full V3/R1
  needs ~8×H100-class per replica. The agent's job is tool-calling against a
  well-documented MCP surface — a distill or mid-size open model may be
  entirely sufficient and changes cost by an order of magnitude. Ship with a
  configurable model id; benchmark tool-call accuracy on recorded (synthetic,
  content-free) session traces before paying for the big one.
- Prompt/response logging: **off**. Debugging uses synthetic sessions, never
  user traffic.

### 3.6 Inference placement — local opt-in

The desktop app already runs WhisperX/MDX-Net/vision locally behind the
"Set Up AI Features" download modal. Extend that pattern instead of
replacing it: **inference follows the media, and the user chooses where the
agent brain lives.**

- **Local-everything** (desktop): editing, ML pipeline, *and* the agent LLM
  on-device — user downloads quantized open weights through the existing
  model-download UX. Nothing leaves the machine, ever. This is a stronger
  privacy claim than the cloud tier and should be marketed as such; it also
  costs us zero GPU capex.
- **Hybrid** (desktop + token pool): editing and ML local, agent inference in
  our pool. The LLM is the one component that is expensive to run and needs
  only text + small snapshots — but it is also the component that carries
  transcripts, so this is exactly where the consent gate sits. The opt-in
  screen states plainly what leaves the machine (transcripts, snapshot
  thumbnails, clip metadata) and that nothing is stored.
- **Cloud** (session box): inference is server-side by definition — running
  models on the client against media that lives in the box would ship content
  the wrong way. No local-inference option in cloud sessions.

The placement question is **capability-detected, not a cold survey**: probe
VRAM/Metal/ROCm at setup, recommend local agent on capable hardware
(16 GB+ → mid-size distill), recommend hybrid below that, always overridable.
Local agent quality bar: the same tool-call accuracy benchmark as §3.5 —
if a quantized distill can't drive the MCP surface reliably, don't offer it
on that hardware; a janky local agent damages the product more than an
honest "your GPU can't run this well" message.

### 3.7 GPU job queue (ML inference)

- WhisperX, MDX-Net, vision describe run as jobs against the session's tmpfs,
  either inside the session box (GPU TEE tier) or on attested workers that
  receive the per-session scratch key and stream results back.
- Same progress plumbing as today: jobs report via `agent_status` so progress
  is visible in the client (per the "agent tools need visible UI" rule).

---

## 4. Session lifecycle

1. **Open** — client authenticates, requests session; orchestrator boots a
   session box, returns WebSocket endpoint + attestation evidence (premium).
2. **Key release** — client verifies attestation (or trusts policy on the
   standard tier), sends the per-session content key into the box.
3. **Edit** — uploads decrypt into tmpfs; engine + agent work over IPC;
   preview streams out; all derived artifacts stay in tmpfs.
4. **Export** — render inside the box, encrypt, download.
5. **Destroy** — explicit end, idle timeout, or disconnect grace expiry →
   VM killed, key scrubbed, deletion receipt signed and delivered.
   Reconnect within the grace window resumes the same box.

Crash semantics: if the box dies, the content dies with it. That is the
product behaving as advertised, not data loss — but the autosave inside
tmpfs plus the disconnect grace window keeps ordinary hiccups painless.

---

## 5. Security tiers

| Tier | Isolation | Verifiability | Pitch |
|---|---|---|---|
| 1 — Standard | microVM, tmpfs-only, no content logs | published architecture + deletion receipts | "zero retention by construction" |
| 2 — Attested | confidential VM (SEV-SNP/TDX), GPU TEE where available | remote attestation, reproducible engine builds, transparency log of measurements | "verify the binary that touched your footage" |
| 3 — E2EE | — | — | **not offered**; impossible for server-side editing, say so openly |

Explicitly out of scope / threat-model honesty in public docs:

- We can see content *during* the session on tier 1 (we promise not to and
  log nothing; tier 2 reduces even our own admins' access).
- Traffic analysis (session timing/size) is not hidden.
- A malicious client device is out of scope.

---

## 6. Roadmap

### M0 — Headless engine (useful immediately, even without the cloud)
- `--server` flag: EGL surfaceless, no window/input, IPC-only.
- `take_snapshot canvas` from offscreen FBO; `ui` errors in server mode.
- CI target: headless render-parity tests (render vs canvas snapshots diffed
  per commit — the live-debugging workflow, automated).

### M1 — Session supervisor (single host)
- One container per session: engine + MCP server + agent loop + tmpfs.
- WebSocket↔unix-socket gateway; session open/teardown/timeout; deletion
  receipt (unsigned, structural only at this stage).
- Thin client v0: chat panel + MJPEG preview + transport. No timeline editing
  in the browser yet — the agent is the editor.

### M2 — Self-hosted inference + token pools
- vLLM/SGLang serving an open model; agent loop pointed at it.
- Pool scheduler: budgets, prefix cache, batching, priority lanes.
- Tool-call accuracy benchmark on synthetic traces; pick model size.

### M3 — Real isolation + asset service
- Firecracker/Kata per session; encrypted upload/download path;
  per-session keys; content-free telemetry audit.
- Browser timeline UI (read-only first, then editing — the IPC surface
  already supports everything).

### M4 — Attested tier
- Confidential VMs, reproducible builds, attestation in the key-release
  handshake, signed deletion receipts, measurement transparency log.

---

## 7. Open questions

- **Disconnect grace window**: how long before "you left the room" triggers
  incineration? (Proposal: 15 min default, user-configurable down to 0.)
- **Vault opt-in**: persistent encrypted projects contradict the headline
  promise — separate product surface or omit entirely?
- **Audio preview transport**: server-rendered Opus vs shipping PCM segments;
  latency vs fidelity for scrubbing.
- **Multi-clip uploads of large source files**: resumable encrypted upload
  protocol; proxy-first upload (generate proxy client-side?) is tempting but
  moves compute to weak devices — probably no.
- **Billing unit**: session-minutes + token budget + render-minutes; how they
  pool across a subscription. Local-everything users consume none of these —
  what does their plan look like (one-time? free with cloud upsell?).
- **Model choice**: which open model actually clears the tool-calling bar for
  this MCP surface (benchmark before committing GPU capex). Same benchmark
  doubles as the local-agent hardware gate (§3.6).
