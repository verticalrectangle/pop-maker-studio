# PHILOSOPHY

## The engine comes first

Build the engine. Expose it as MCP. Let the agent drive. Only then wrap the parts humans actually need to touch.

Never build UI → bolt on API → force agents into human paths. Invert it. Every surface — MCP, human GUI, CI harness, iOS app — speaks the same JSON-RPC protocol against the same engine. They are not three products. They are one engine with multiple faces.

## Why this works

**The engine is honest.** An agent can't fake a compositor. Either it renders or it doesn't. The MCP surface is the contract — if the agent can do it, the human can do it. Both share the same source of truth (AppState).

**You discover what humans actually need.** Watch the agent struggle. A command the agent never uses? Don't build a button for it. A multi-step sequence the agent keeps repeating? That's the next human shortcut. The agent stress-tests your API before any human touches it.

**Every client is a first-class citizen.** The agent is not an afterthought bolted onto a GUI. The GUI is not a simplification of the API. They are peers — different views into the same state, validated by the same contract.

## Consequences

- Undo/redo is a memcpy of AppState. Every client benefits.
- Serialization covers the entire application by construction. No partial-save bugs.
- The IPC boundary (`src/ipc_server.cpp`) is the only entry point. No backdoors, no side channels.
- A bug fixed in the engine is fixed for every client simultaneously.
- `begin_batch`/`end_batch` gives every client transactional multi-step edits — human, agent, or CI harness.

## What this looks like in practice

```
AppState (single source of truth)
    │
    ├── ipc_server.cpp  ──  JSON-RPC over Unix socket
    │       ├── MCP client (Claude/Codex agent)
    │       ├── WebSocket gateway (browser client)
    │       ├── CI harness (agentic build pipeline)
    │       └── iOS app (future)
    │
    └── Dear ImGui (desktop GUI)
```

The GUI is just another client. The agent is just another client. The engine doesn't care which one is talking.
