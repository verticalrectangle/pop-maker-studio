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

import asyncio
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
            name="add_track",
            description=(
                "Add a new track to the project at a given position (default 0 = top). "
                "Returns the track index. Requires batch.\n\n"
                "LAYERING: tracks are stacked like Photoshop layers — track 0 is the TOP "
                "(foreground) and the highest-index track is the BOTTOM (background). "
                "Always put video/background clips on higher-index tracks and text/FX/overlays "
                "on lower-index tracks so they render on top."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "name": {"type": "string", "description": "Track name"},
                    "position": {"type": "integer", "description": "Insert position (0 = top)", "default": 0},
                },
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
                "Set a scalar property on a clip. Requires batch.\n\n"
                "General props:\n"
                "  volume (0–2), speed (0.25–4), opacity (0–1), muted (bool),\n"
                "  in_point (seconds into source — equivalent to trimming the brick's left edge),\n"
                "  fade_in, fade_out, pos_x, pos_y, scale_x, scale_y, rotation, text\n\n"
                "Subtitle / text layout:\n"
                "  sub_pos (0=bottom 1=center 2=top 3=custom), sub_pos_x, sub_pos_y (0–1 normalised),\n"
                "  sub_anchor_h (0=left 1=center 2=right), sub_wrap_w (0–1, 0=auto),\n"
                "  font_size (pixels, 0=auto)\n\n"
                "Color arrays are [r, g, b, a] with values 0–1:\n"
                "  sub_color (text fill, also sets sub_color_override=true),\n"
                "  karaoke_highlight_color\n\n"
                "Animation / behaviour:\n"
                "  clip_style (string: none|pop|fade|slide_up|slide_down|typewriter|wave|bounce|\n"
                "              zoom_in|zoom_out|spin|flip|glitch|blur_in|split|drop|rise),\n"
                "  blend_mode (string: normal|add|multiply|screen|overlay),\n"
                "  karaoke (bool)"
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
            name="add_clip_sequence",
            description=(
                "Add multiple clips to a single track in one round-trip. Equivalent to calling "
                "add_clip N times but dramatically faster for beat-sync edits or any bulk layout. "
                "Returns an array of assigned clip IDs in order. Requires batch.\n\n"
                "Each entry in 'clips': {type, start, end, text (file path for video/audio)}"
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track": {"type": "integer"},
                    "clips": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": {
                                "type": {"type": "string"},
                                "start": {"type": "number"},
                                "end": {"type": "number"},
                                "text": {"type": "string", "default": ""},
                            },
                            "required": ["type", "start", "end"],
                        },
                    },
                },
                "required": ["track", "clips"],
            },
        ),
        Tool(
            name="set_clip_props",
            description=(
                "Set properties on multiple clips in one round-trip. Equivalent to calling "
                "set_clip_prop N times. Use for bulk muting, opacity changes, fade setup, etc. "
                "Requires batch.\n\n"
                "Each entry in 'ops': {track, clip, prop, value} — same props as set_clip_prop."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "ops": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": {
                                "track": {"type": "integer"},
                                "clip": {"type": "integer"},
                                "prop": {"type": "string"},
                                "value": {},
                            },
                            "required": ["track", "clip", "prop", "value"],
                        },
                    },
                },
                "required": ["ops"],
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
            name="add_multifx_brick",
            description=(
                "Add a Multi-FX brick — a single timeline brick containing an ordered chain of "
                "sub-effects, each with its own timing window inside the brick's span. "
                "Use this instead of multiple overlapping add_effect_brick calls when effects "
                "share the same time range or partially overlap.\n\n"
                "GLASS behaviour: if placed on the same track as a video/audio clip it overlaps, "
                "it becomes a 'glass' FX and applies only to that specific clip before compositing. "
                "Place it on a separate FX track for global (all-layers) effect.\n\n"
                "effects array: each entry is an object with:\n"
                "  fx_type (required) — same options as add_effect_brick\n"
                "  rel_start (default 0) — seconds from brick start\n"
                "  rel_end   (default 0 = until brick end) — seconds from brick start\n"
                "  params — same effect-specific param dict as add_effect_brick\n\n"
                "Requires batch."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track": {"type": "integer"},
                    "start": {"type": "number"},
                    "end": {"type": "number"},
                    "effects": {
                        "type": "array",
                        "description": "Ordered list of sub-effects",
                        "items": {
                            "type": "object",
                            "properties": {
                                "fx_type": {
                                    "type": "string",
                                    "enum": ["grade", "blur", "vignette", "glitch", "zoom_punch",
                                             "lut", "light_leak", "vhs", "datamosh", "chroma_key"],
                                },
                                "rel_start": {"type": "number", "default": 0},
                                "rel_end": {"type": "number", "default": 0, "description": "0 = until brick end"},
                                "params": {"type": "object"},
                            },
                            "required": ["fx_type"],
                        },
                    },
                },
                "required": ["track", "start", "end"],
            },
        ),
        Tool(
            name="new_project",
            description=(
                "Reset the project to a blank state (clears all tracks, clips, audio, beats). "
                "Call this before building a project from scratch. Requires batch."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="rename_track",
            description="Rename a track. Requires batch.",
            inputSchema={
                "type": "object",
                "properties": {
                    "track": {"type": "integer"},
                    "name": {"type": "string"},
                },
                "required": ["track", "name"],
            },
        ),
        Tool(
            name="add_effect_brick",
            description=(
                "Add a standalone FX brick (ClipType::Effect) to a track. "
                "FX bricks overlay the video render — they don't appear in the clip list "
                "the same way text/video do, but they affect everything below them on the timeline.\n\n"
                "fx_type options: grade | blur | vignette | glitch | zoom_punch | lut | "
                "light_leak | vhs | datamosh | chroma_key\n\n"
                "params for each type (use these exact key names):\n"
                "  grade: brightness (-1–1), contrast (0–2), saturation (0–2), hue (0–1)\n"
                "  blur: blur (0–1)\n"
                "  vignette: vignette (0–1)\n"
                "  glitch: glitch_chroma (0–20), glitch_jitter (0–1), glitch_corruption (0–1), glitch_corruption_bleed (0–1)\n"
                "  zoom_punch: zoom_strength (0–0.5), zoom_decay (0.01–1), zoom_shake (0–1)\n"
                "  light_leak: leak_intensity (0–1), leak_speed (0–3)\n"
                "  vhs: vhs_noise (0–1), vhs_bleed (0–20), vhs_tracking (0–1)\n"
                "  datamosh: datamosh_intensity (0–1), datamosh_spread (0–1)\n"
                "  chroma_key: chroma_key_r, chroma_key_g, chroma_key_b (0–1 each), chroma_key_threshold (0–1), chroma_key_softness (0–1)\n"
                "  lut: (no numeric params — set lut_path via set_clip_prop instead)\n\n"
                "Requires batch."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track": {"type": "integer"},
                    "fx_type": {
                        "type": "string",
                        "enum": ["grade", "blur", "vignette", "glitch", "zoom_punch",
                                 "lut", "light_leak", "vhs", "datamosh", "chroma_key"],
                    },
                    "start": {"type": "number"},
                    "end": {"type": "number"},
                    "params": {
                        "type": "object",
                        "description": "Effect-specific parameter values by name (see description)",
                    },
                },
                "required": ["track", "fx_type", "start", "end"],
            },
        ),
        Tool(
            name="find_audio_cue",
            description=(
                "Analyse an audio file using the app's beat detector and find a good "
                "beat-aligned source timestamp matching a natural-language description "
                "of what you're looking for. The app must be running.\n\n"
                "description examples: 'after the intro', 'first big drop', 'energetic buildup', "
                "'quiet bridge', 'chorus', 'before the outro', 'most energetic part'\n\n"
                "Returns: source_timestamp (seconds, beat-aligned) — use as a negative clip "
                "start to position the audio brick: clip start = -source_timestamp, "
                "clip end = video_duration - source_timestamp. Also returns bpm, duration, "
                "reasoning, and 2 alternative timestamps."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "path": {"type": "string", "description": "Absolute path to the audio file"},
                    "description": {"type": "string", "description": "What you're looking for"},
                    "clip_duration": {
                        "type": "number",
                        "description": "How long the clip will be in seconds (optional — used to verify the window fits)",
                    },
                },
                "required": ["path", "description"],
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


# ── find_audio_cue ────────────────────────────────────────────────────────────

def _snap_to_beat(t: float, beats: list[float]) -> float:
    if not beats:
        return t
    return min(beats, key=lambda b: abs(b - t))


def _match_cue(description: str, rms: list[float], beats: list[float],
               duration: float, clip_duration: float) -> tuple[float, str, list[dict]]:
    desc = description.lower()
    n = len(rms)

    def rms_at(t: float) -> float:
        idx = int(t)
        return rms[idx] if 0 <= idx < n else 0.0

    def rolling_max(start_s: int, end_s: int, window: int = 3) -> tuple[int, float]:
        best_s, best_v = start_s, 0.0
        for s in range(start_s, min(end_s - window + 1, n)):
            v = sum(rms[s:s + window]) / window
            if v > best_v:
                best_v = v
                best_s = s
        return best_s, best_v

    def find_drop(skip_intro_frac: float = 0.15) -> tuple[int, str]:
        skip = int(n * skip_intro_frac)
        best_s, best_v = skip, 0.0
        for s in range(skip + 1, n - 1):
            if rms[s] > best_v and rms[s] > rms[s - 1] and rms[s] >= rms[s + 1]:
                best_v = rms[s]
                best_s = s
        return best_s, f"Highest onset peak at {best_s}s (rms={best_v:.2f})"

    # Keyword → candidate second
    if any(k in desc for k in ("intro", "beginning", "start")) and "after" not in desc:
        candidate = int(n * 0.05)
        reasoning = f"Start of track (~{candidate}s)"

    elif any(k in desc for k in ("after intro", "verse", "first verse", "first section")):
        # Rising rms in 15–30% zone
        start_s = int(n * 0.15)
        end_s   = int(n * 0.35)
        candidate = start_s
        best_rise = 0.0
        for s in range(start_s, end_s - 1):
            rise = rms[s + 1] - rms[s]
            if rise > best_rise:
                best_rise = rise
                candidate = s
        reasoning = f"Rising energy at {candidate}s in first-verse zone"

    elif any(k in desc for k in ("drop", "big drop", "chorus", "hook", "peak")):
        candidate, reasoning = find_drop(0.12)
        reasoning = f"Biggest onset peak outside intro at {candidate}s"

    elif any(k in desc for k in ("build", "buildup", "ramp", "rise")):
        # Longest monotonic rise of ≥3s
        best_start, best_len = 0, 0
        s = 0
        while s < n - 1:
            if rms[s + 1] >= rms[s]:
                run_start = s
                while s < n - 1 and rms[s + 1] >= rms[s]:
                    s += 1
                run_len = s - run_start
                if run_len > best_len:
                    best_len = run_len
                    best_start = run_start
            else:
                s += 1
        candidate = best_start
        reasoning = f"Longest rising rms run ({best_len}s) starts at {candidate}s"

    elif any(k in desc for k in ("breakdown", "bridge", "quiet", "soft", "low")):
        # Lowest 3s window between two high-rms sections
        mid_start = int(n * 0.2)
        mid_end   = int(n * 0.8)
        best_s, best_v = mid_start, float("inf")
        for s in range(mid_start, mid_end - 2):
            v = sum(rms[s:s + 3]) / 3
            if v < best_v:
                best_v = v
                best_s = s
        candidate = best_s
        reasoning = f"Quietest 3s window at {candidate}s (rms={best_v:.2f})"

    elif any(k in desc for k in ("outro", "end", "fade", "closing")):
        candidate = int(n * 0.80)
        reasoning = f"Outro zone at {candidate}s (~80% through track)"

    else:
        # Generic: highest sustained 3s window outside first 15%
        skip = int(n * 0.15)
        best_s, best_v = rolling_max(skip, n)
        candidate = best_s
        reasoning = f"Highest sustained energy at {candidate}s (rms={best_v:.2f})"

    # Ensure clip_duration fits
    if clip_duration and candidate + clip_duration > duration:
        candidate = max(0, int(duration - clip_duration - 2))
        reasoning += f" (adjusted to fit {clip_duration}s clip)"

    primary = _snap_to_beat(float(candidate), beats)

    # Two alternatives: next-best drop and a different region
    alt_drop_s, _ = find_drop(0.35)
    alt1 = _snap_to_beat(float(alt_drop_s), beats)

    quiet_zone = int(n * 0.55)
    alt2 = _snap_to_beat(float(quiet_zone), beats)

    alternatives = [
        {"source_timestamp": alt1, "reasoning": f"Second energy peak at ~{alt_drop_s}s"},
        {"source_timestamp": alt2, "reasoning": f"Mid-track entry at ~{quiet_zone}s"},
    ]

    return primary, reasoning, alternatives


async def _find_audio_cue(arguments: dict) -> dict:
    path = arguments.get("path", "")
    description = arguments.get("description", "")
    clip_duration = float(arguments.get("clip_duration", 0) or 0)

    if not path:
        raise ValueError("path is required")

    # Start analysis
    _call("analyze_audio", {"path": path})

    # Poll until done (up to 120s)
    for _ in range(240):
        await asyncio.sleep(0.5)
        res = _call("get_audio_analysis", {})
        if res.get("status") == "done":
            break
        if res.get("status") == "error":
            raise ValueError("beat detection failed for: " + path)
    else:
        raise TimeoutError("audio analysis timed out")

    bpm      = res["bpm"]
    duration = res["duration"]
    beats    = res["beats"]
    rms      = res["rms"]

    primary, reasoning, alternatives = _match_cue(
        description, rms, beats, duration, clip_duration
    )

    return {
        "source_timestamp": primary,
        "bpm": bpm,
        "duration": duration,
        "reasoning": reasoning,
        "alternatives": alternatives,
    }


@server.call_tool()
async def call_tool(name: str, arguments: dict) -> list[TextContent]:
    if name == "find_audio_cue":
        result = await _find_audio_cue(arguments)
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
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
