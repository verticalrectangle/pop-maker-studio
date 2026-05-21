#!/usr/bin/env python3
"""
Pop Maker Studio — MCP server
Bridges Claude to the running app via the Unix socket IPC layer.

Usage:
  pip install -r requirements.txt
  python3 mcp_server/server.py

Claude Desktop config (claude_desktop_config.json):
  {
    "mcpServers": {
      "pop-maker-studio": {
        "command": "python3",
        "args": ["/path/to/pop-maker-studio/mcp_server/server.py"]
      }
    }
  }
"""

import json
import os
import socket
import threading
import time
import uuid
from pathlib import Path
from typing import Any

from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp import types
from mcp.types import Tool, TextContent

# ── IPC connection ─────────────────────────────────────────────────────────────

LOCK_FILE = "/tmp/pop-maker-studio.lock"

_sock: socket.socket | None = None
_sock_lock = threading.Lock()
_pending: dict[str, Any] = {}


def _connect() -> socket.socket:
    if not os.path.exists(LOCK_FILE):
        raise RuntimeError(
            "Pop Maker Studio is not running (lock file not found: " + LOCK_FILE + ")"
        )
    with open(LOCK_FILE) as f:
        parts = f.read().strip().split()
    if len(parts) < 2:
        raise RuntimeError("Malformed lock file")
    sock_path = parts[1]

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    s.setblocking(True)
    return s


def _get_sock() -> socket.socket:
    global _sock
    with _sock_lock:
        if _sock is None:
            _sock = _connect()
        return _sock


def _call(method: str, params: dict | None = None) -> Any:
    """Send one IPC request and return the result, or raise on error."""
    req_id = str(uuid.uuid4())
    payload = json.dumps({"id": req_id, "method": method, "params": params or {}}) + "\n"

    with _sock_lock:
        global _sock
        for attempt in range(2):
            try:
                if _sock is None:
                    _sock = _connect()
                _sock.sendall(payload.encode())
                buf = b""
                while b"\n" not in buf:
                    chunk = _sock.recv(65536)
                    if not chunk:
                        raise ConnectionError("IPC socket closed")
                    buf += chunk
                line = buf.split(b"\n", 1)[0]
                resp = json.loads(line)
                if "error" in resp:
                    raise ValueError(resp["error"])
                return resp.get("result", {})
            except (ConnectionError, OSError, BrokenPipeError):
                _sock = None
                if attempt == 1:
                    raise


# ── Effect manifest (codegen'd) ────────────────────────────────────────────────

_MANIFEST_PATH = Path(__file__).parent.parent / "effects" / "mcp_manifest.json"

def _load_manifest() -> list[dict]:
    if _MANIFEST_PATH.exists():
        with open(_MANIFEST_PATH) as f:
            return json.load(f)
    return []

_EFFECTS = _load_manifest()

def _build_effect_catalog() -> str:
    if not _EFFECTS:
        return "(effect manifest not found — run tools/codegen_effects.py)"
    lines = []
    for e in _EFFECTS:
        param_strs = ", ".join(
            f'{p["name"]} ({p["min"]}–{p["max"]}, default {p["default"]})'
            for p in e["params"]
        )
        lines.append(f'  {e["id"]}: {e["label"]} — {e["description"]}')
        if param_strs:
            lines.append(f'    params: {param_strs}')
    return "\n".join(lines)

_EFFECT_CATALOG = _build_effect_catalog()


# ── MCP server ─────────────────────────────────────────────────────────────────

server = Server("pop-maker-studio")


@server.list_tools()
async def list_tools() -> list[Tool]:
    return [
        Tool(
            name="get_project",
            description=(
                "Returns the full project state: duration, fps, bpm, audio path, "
                "playhead position, beats array, and all tracks with their clips. "
                "Each clip includes text, timing, volume, animation style, TextStyle "
                "(shadow/stroke/glow/bg), and karaoke settings. Read-only — no batch needed."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="get_pipeline_status",
            description=(
                "Returns the current ML pipeline status: stage (idle/extract/transcribe/"
                "align/done/error), progress (0–1), message, and error string. "
                "Poll this after trigger_pipeline or generate_typography."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="begin_batch",
            description=(
                "Start a named edit batch. All mutation calls (add_clip, set_clip_prop, etc.) "
                "MUST be wrapped between begin_batch and end_batch. The label appears in the "
                "undo history. You cannot nest batches."
            ),
            inputSchema={
                "type": "object",
                "properties": {"label": {"type": "string", "description": "Undo history label"}},
                "required": ["label"],
            },
        ),
        Tool(
            name="end_batch",
            description="Commit the current edit batch to the undo history.",
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="add_clip",
            description=(
                "Add a new clip to a track. type: text | lyrics | subtitle | video | audio | "
                "effect | background | body_fx. For text/lyrics/subtitle, set 'text'. "
                "For video/audio, set 'text' to the file path. Requires batch."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track": {"type": "integer"},
                    "type": {"type": "string"},
                    "start": {"type": "number"},
                    "end": {"type": "number"},
                    "text": {"type": "string", "default": ""},
                },
                "required": ["track", "type", "start", "end"],
            },
        ),
        Tool(
            name="delete_clip",
            description="Delete a clip. Requires batch.",
            inputSchema={
                "type": "object",
                "properties": {
                    "track": {"type": "integer"},
                    "clip": {"type": "integer"},
                },
                "required": ["track", "clip"],
            },
        ),
        Tool(
            name="move_clip",
            description="Move a clip to a new start time (preserves duration). Requires batch.",
            inputSchema={
                "type": "object",
                "properties": {
                    "track": {"type": "integer"},
                    "clip": {"type": "integer"},
                    "start": {"type": "number"},
                },
                "required": ["track", "clip", "start"],
            },
        ),
        Tool(
            name="trim_clip",
            description="Trim a clip's start and/or end time. Requires batch.",
            inputSchema={
                "type": "object",
                "properties": {
                    "track": {"type": "integer"},
                    "clip": {"type": "integer"},
                    "start": {"type": "number"},
                    "end": {"type": "number"},
                },
                "required": ["track", "clip"],
            },
        ),
        Tool(
            name="split_clip",
            description="Split a clip at a time point. Returns left_clip and right_clip indices. Requires batch.",
            inputSchema={
                "type": "object",
                "properties": {
                    "track": {"type": "integer"},
                    "clip": {"type": "integer"},
                    "time": {"type": "number"},
                },
                "required": ["track", "clip", "time"],
            },
        ),
        Tool(
            name="set_clip_prop",
            description=(
                "Set a scalar property on a clip. Valid props: volume (0–2), speed (0.25–4), "
                "opacity (0–1), muted (bool), in_point, fade_in, fade_out, pos_x, pos_y, "
                "scale_x, scale_y, rotation, text. Requires batch."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track": {"type": "integer"},
                    "clip": {"type": "integer"},
                    "prop": {"type": "string"},
                    "value": {},
                },
                "required": ["track", "clip", "prop", "value"],
            },
        ),
        Tool(
            name="set_text_style",
            description=(
                "Set visual text styling on a clip. All fields are optional — only provided "
                "fields are updated. Color arrays are [r, g, b, a] with values 0–1. Requires batch.\n"
                "Fields: shadow_enabled (bool), shadow_ox, shadow_oy, shadow_col,\n"
                "  stroke_enabled, stroke_w, stroke_col,\n"
                "  glow_enabled, glow_r, glow_col,\n"
                "  bg_enabled, bg_col, bg_pad_x, bg_pad_y, bg_corner"
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track": {"type": "integer"},
                    "clip": {"type": "integer"},
                    "shadow_enabled": {"type": "boolean"},
                    "shadow_ox": {"type": "number"},
                    "shadow_oy": {"type": "number"},
                    "shadow_col": {"type": "array", "items": {"type": "number"}, "minItems": 4, "maxItems": 4},
                    "stroke_enabled": {"type": "boolean"},
                    "stroke_w": {"type": "number"},
                    "stroke_col": {"type": "array", "items": {"type": "number"}, "minItems": 4, "maxItems": 4},
                    "glow_enabled": {"type": "boolean"},
                    "glow_r": {"type": "number"},
                    "glow_col": {"type": "array", "items": {"type": "number"}, "minItems": 4, "maxItems": 4},
                    "bg_enabled": {"type": "boolean"},
                    "bg_col": {"type": "array", "items": {"type": "number"}, "minItems": 4, "maxItems": 4},
                    "bg_pad_x": {"type": "number"},
                    "bg_pad_y": {"type": "number"},
                    "bg_corner": {"type": "number"},
                },
                "required": ["track", "clip"],
            },
        ),
        Tool(
            name="apply_effect",
            description=(
                "Apply a shader effect to a clip. Set amount (0–1). "
                "Provide params dict with effect-specific parameter values.\n\n"
                "Available effects:\n" + _EFFECT_CATALOG + "\n\n"
                "Requires batch."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track": {"type": "integer"},
                    "clip": {"type": "integer"},
                    "fx_id": {"type": "string", "description": "Effect id from the list above"},
                    "amount": {"type": "number", "description": "0–1 blend amount"},
                    "params": {"type": "object", "description": "Effect-specific params by name"},
                },
                "required": ["track", "clip", "fx_id"],
            },
        ),
        Tool(
            name="generate_typography",
            description=(
                "Generate lyric clips from the loaded word JSON using the currently selected "
                "typography preset. Fires synchronously. After calling, you can check "
                "get_project to see the new clips. Requires batch."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="trigger_pipeline",
            description=(
                "Start the ML processing pipeline (vocal separation + Whisper transcription + "
                "CTC alignment). Returns immediately — poll get_pipeline_status until "
                "stage is 'done' or 'error'. mode: both | transcribe_only | separate_only. "
                "Requires an audio file to be loaded. Requires batch."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "mode": {
                        "type": "string",
                        "enum": ["both", "transcribe_only", "separate_only"],
                        "default": "both",
                    }
                },
            },
        ),
        Tool(
            name="seek",
            description="Move the playhead to a time (seconds). Read-only — no batch needed.",
            inputSchema={
                "type": "object",
                "properties": {"time": {"type": "number"}},
                "required": ["time"],
            },
        ),
        Tool(
            name="play",
            description="Start playback from the current playhead position.",
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="pause",
            description="Pause playback.",
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="save_project",
            description=(
                "Save the project to disk. If path is omitted, saves to the last-used path. "
                "Returns the path written. No batch needed."
            ),
            inputSchema={
                "type": "object",
                "properties": {"path": {"type": "string"}},
            },
        ),
        Tool(
            name="load_project",
            description="Load a .pms project file, replacing the current project. Requires batch.",
            inputSchema={
                "type": "object",
                "properties": {"path": {"type": "string"}},
                "required": ["path"],
            },
        ),
        Tool(
            name="validate_glsl",
            description=(
                "Test-compile a GLSL fragment shader body without registering it as a runtime effect. "
                "Returns {ok: bool, error?: string}. Useful before writing a shader to disk. "
                "No batch needed."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "glsl": {"type": "string"},
                    "params": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": {
                                "name": {"type": "string"},
                                "label": {"type": "string"},
                                "min": {"type": "number"},
                                "max": {"type": "number"},
                                "default": {"type": "number"},
                            },
                            "required": ["name"],
                        },
                    },
                },
                "required": ["glsl"],
            },
        ),
    ]


@server.call_tool()
async def call_tool(name: str, arguments: dict) -> list[TextContent]:
    try:
        result = _call(name, arguments)
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    except ValueError as e:
        # IPC returned an error string — surface it as a tool error Claude can act on
        raise ValueError(str(e)) from e
    except RuntimeError as e:
        raise RuntimeError(str(e)) from e


# ── Entry point ────────────────────────────────────────────────────────────────

async def main():
    async with stdio_server() as (read_stream, write_stream):
        await server.run(read_stream, write_stream, server.create_initialization_options())


if __name__ == "__main__":
    import asyncio
    asyncio.run(main())
