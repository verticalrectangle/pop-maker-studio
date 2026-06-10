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
import base64
import io
import json
import math
import os
import subprocess
import re
import socket
import sys
import threading
import time
import uuid
from contextlib import contextmanager
from pathlib import Path
from typing import Any

from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp import types
from mcp.types import Tool, TextContent, ImageContent

# ── IPC connection ─────────────────────────────────────────────────────────────

SOCK_PATH = "/tmp/pop-maker-studio.sock"

_sock: socket.socket | None = None
_sock_lock = threading.Lock()
_pending: dict[str, Any] = {}
_last_project_path: str = ""  # auto-reload on reconnect


def _sock_connectable() -> bool:
    """Return True only if the socket file exists AND accepts connections."""
    if not os.path.exists(SOCK_PATH):
        return False
    try:
        probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        probe.connect(SOCK_PATH)
        probe.close()
        return True
    except OSError:
        return False


def _autostart_pms() -> None:
    """Launch the PMS binary if it's not already running."""
    binary = Path(__file__).parent.parent / "build" / "pop-maker-studio"
    if not binary.exists():
        raise RuntimeError(f"PMS binary not found at {binary}. Build the project first.")
    subprocess.Popen(
        [str(binary)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    for _ in range(30):
        if _sock_connectable():
            return
        time.sleep(0.2)
    raise RuntimeError("PMS launched but socket never appeared — check build/pop-maker-studio")


def _connect() -> socket.socket:
    if not _sock_connectable():
        _autostart_pms()
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK_PATH)
    s.setblocking(True)
    return s


def _get_sock() -> socket.socket:
    global _sock
    with _sock_lock:
        if _sock is None:
            _sock = _connect()
        return _sock


def _resolve_path(path: str) -> str:
    """Return the exact filesystem path for a given path string.

    Handles cases where the passed-in path uses Unicode look-alikes (e.g.
    straight apostrophe U+0027 vs curly quote U+2019) that differ from the
    actual on-disk filename.  Lists the parent directory and returns the first
    entry whose name matches when both sides are NFC-normalised and common
    typographic substitutions are collapsed.
    """
    import os, unicodedata
    if os.path.exists(path):
        return path
    parent = os.path.dirname(path)
    name   = os.path.basename(path)
    if not os.path.isdir(parent):
        return path

    _APOSTROPHE_VARIANTS = str.maketrans({
        "‘": "'", "’": "'", "ʼ": "'",  # curly/modifier → straight
        "＇": "'",                                   # fullwidth apostrophe
        "“": '"', "”": '"',                   # curly double quotes
        "？": "?", "！": "!",                   # fullwidth punctuation
        "–": "-", "—": "-",                   # dashes
        "：": ":", "；": ";",
    })

    def _fold(s: str) -> str:
        return unicodedata.normalize("NFC", s).translate(_APOSTROPHE_VARIANTS).casefold()

    name_folded = _fold(name)
    for entry in os.listdir(parent):
        if _fold(entry) == name_folded:
            return os.path.join(parent, entry)
    return path


def _resolve_paths(params: dict) -> dict:
    """Walk params dict and resolve any 'path', 'src', 'dst' values."""
    out = {}
    for k, v in params.items():
        if isinstance(v, str) and k in ("path", "src", "dst"):
            out[k] = _resolve_path(v)
        else:
            out[k] = v
    return out


def _call(method: str, params: dict | None = None, *, _pms_retries: int = 5) -> Any:
    """Send one IPC request and return the result, or raise on error.

    Retries up to _pms_retries times (3s apart) if PMS is not yet running,
    so callers work correctly right after PMS restarts.
    """
    for pms_attempt in range(_pms_retries):
        resolved = _resolve_paths(params or {})
        payload = json.dumps(
            {"id": str(uuid.uuid4()), "method": method, "params": resolved},
            ensure_ascii=False,
        ) + "\n"

        try:
            with _sock_lock:
                global _sock, _last_project_path
                for attempt in range(2):
                    try:
                        was_none = _sock is None
                        if _sock is None:
                            _sock = _connect()
                        if was_none and _last_project_path and method != "load_project":
                            try:
                                _sock.sendall((json.dumps({"id": "reload", "method": "load_project",
                                    "params": {"path": _last_project_path}}, ensure_ascii=False) + "\n").encode())
                                rbuf = b""
                                while b"\n" not in rbuf:
                                    rbuf += _sock.recv(65536)
                                print(f"[pms] auto-reloaded {_last_project_path}", flush=True)
                            except Exception:
                                pass
                        _sock.sendall(payload.encode())
                        buf = b""
                        while True:
                            while b"\n" not in buf:
                                chunk = _sock.recv(65536)
                                if not chunk:
                                    raise ConnectionError("IPC socket closed")
                                buf += chunk
                            nl = buf.index(b"\n")
                            line, buf = buf[:nl], buf[nl + 1:]
                            resp = json.loads(line)
                            if resp.get("type") == "progress":
                                continue  # intermediate progress line — keep reading
                            if "error" in resp:
                                raise ValueError(resp["error"])
                            result = resp.get("result", {})
                            if isinstance(result, dict) and result.get("project_path"):
                                _last_project_path = result["project_path"]
                            return result
                    except (ConnectionError, OSError, BrokenPipeError):
                        _sock = None
                        if attempt == 1:
                            raise
        except (RuntimeError, ConnectionError, OSError, BrokenPipeError) as e:
            if pms_attempt < _pms_retries - 1:
                print(f"[pms] not ready ({e}), retrying in 3s…", flush=True)
                time.sleep(3)
                continue
            raise


@contextmanager
def _batch(label: str):
    _call("begin_batch", {"label": label})
    result = {}
    try:
        yield result
    finally:
        result.update(_call("end_batch", {}))


async def _notify_progress(progress: float, total: float = 1.0) -> None:
    """Send an MCP progress notification if the client provided a progress token."""
    try:
        ctx = server.request_context
        if not ctx.meta:
            return
        token = getattr(ctx.meta, "progressToken", None)
        if token is None:
            return
        await ctx.session.send_progress_notification(
            progress_token=token,
            progress=progress,
            total=total,
        )
    except Exception:
        pass


# ── MCP server ─────────────────────────────────────────────────────────────────

server = Server("pop-maker-studio")


async def _send_search_progress(status: dict, last_msg: str) -> str:
    """Forward C++ search status to the MCP client as a progress notification.

    Returns the new last_msg (unchanged if no notification was sent). Safe to
    call from any tool handler — silently no-ops when the client did not
    include a progressToken with the request, or when no request context
    exists (e.g. unit tests).
    """
    msg = status.get("message", "")
    if not msg or msg == last_msg:
        return last_msg
    try:
        ctx = server.request_context
        token = ctx.meta.progressToken if ctx.meta else None
        if token is not None:
            progress = float(status.get("progress", 0.0))
            await ctx.session.send_progress_notification(token, progress, 1.0, msg)
    except LookupError:
        pass
    except Exception as e:
        print(f"[search progress] notification failed: {e}", flush=True)
    return msg


@server.list_tools()
async def list_tools() -> list[Tool]:
    return [
        Tool(
            name="get_project",
            description=(
                "Returns project state. Slim by default: {duration, fps, bpm, audio_path, "
                "project_path, transcript_ready, playhead, tracks: [{index, name, muted, locked, clip_count}], markers}. "
                "Use verbose=true for full clip details (all styling, FX props). "
                "Prefer get_clips(track_name=...) when you only need clip positions on one track. "
                "Read-only — no batch needed.\n\n"
                "PROBE BEFORE ASKING: When the user provides media files, use tools to gather facts "
                "before asking them anything. Call get_media_info on each file to learn resolution, "
                "duration, and codec. To understand video content, capture a still: "
                "ffmpeg -y -ss 0.5 -i <path> -vframes 1 -vf scale=480:-1 /tmp/still_<name>.jpg -loglevel quiet "
                "then Read the jpg — you can see and describe it yourself. "
                "For lyrics/transcript, run trigger_pipeline on the audio first. "
                "Only ask the user about subjective choices (style, pacing, colors) that tools cannot determine."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "verbose": {"type": "boolean", "description": "Return full clip details (default false)"},
                },
            },
        ),
        Tool(
            name="get_clips",
            description=(
                "Return the clips on one track: [{index, type, start, end, duration, in_point, source, text}]. "
                "Use this instead of get_project when you only need clip positions on a specific track — "
                "much smaller response. Accepts track index or track_name. Read-only — no batch needed."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track":      {"type": "integer"},
                    "track_name": {"type": "string", "description": "Track name (alternative to track index)"},
                },
            },
        ),
        Tool(
            name="get_all_clips",
            description=(
                "Return clips across ALL tracks: [{index, name, clips: [{index, type, start, end, "
                "duration, in_point, source, text}]}]. Use for full-timeline orientation without "
                "get_project(verbose=true). Read-only — no batch needed."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="trigger_export",
            description=(
                "Render and export the project to MP4 (or GIF). Blocks until complete — "
                "returns {done, success, output, stage} when finished. No batch needed.\n\n"
                "output_path: override default path (defaults to {project_dir}/{name}.mp4).\n"
                "crf: quality 0–51, lower = better (default 23).\n"
                "preset: ultrafast|fast|medium|slow (default medium).\n"
                "gif: true to export animated GIF instead of MP4."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "output_path": {"type": "string", "description": "Override output file path"},
                    "crf":    {"type": "integer", "description": "Quality 0–51 (default 23)"},
                    "preset": {"type": "string", "enum": ["ultrafast", "fast", "medium", "slow"]},
                    "gif":    {"type": "boolean", "description": "Export GIF instead of MP4"},
                },
            },
        ),
        Tool(
            name="get_export_status",
            description=(
                "Poll export progress. Returns {running, progress (0–1), frame, total_frames, "
                "eta_secs, stage, output}. Read-only — no batch needed."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="cancel_export",
            description="Cancel a running export. No batch needed.",
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="cancel_search",
            description="Cancel a running find_and_add_clip / search_transcript operation. Safe to call even if no search is running. No batch needed.",
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="get_stills",
            description=(
                "Generate and return thumbnail images for a list of media files so you can "
                "visually identify their content before deciding which to add to the timeline.\n\n"
                "For each path: if a still already exists on disk it is returned instantly; "
                "otherwise a quarter-resolution JPEG is extracted via ffmpeg (< 1 s per file). "
                "The full proxy transcode is NOT started — this is purely for identification.\n\n"
                "Returns one inline image per file. Files that fail to produce a still are "
                "included in the result with ok=false and no image.\n\n"
                "Read-only — no batch needed."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "paths": {
                        "type": "array",
                        "items": {"type": "string"},
                        "description": "Absolute paths to media files (video, image, or audio with cover art)",
                    },
                },
                "required": ["paths"],
            },
        ),
        Tool(
            name="get_media_info",
            description=(
                "Probe a media file and return its codec/format metadata: duration (seconds), "
                "width, height, fps, has_video, has_audio, video_codec, audio_codec, "
                "sample_rate, channels. Use this to diagnose audio/video stream issues "
                "before adding a clip. Read-only — no batch needed."
            ),
            inputSchema={
                "type": "object",
                "properties": {"path": {"type": "string", "description": "Absolute path to the media file"}},
                "required": ["path"],
            },
        ),
        Tool(
            name="crop_media",
            description=(
                "Crop a video or image file to a target aspect ratio and save the result. "
                "Call this before add_clip whenever the user asks to crop source files, or "
                "when the source aspect ratio differs from the canvas.\n\n"
                "aspect: 'square' (1:1), 'vertical' (9:16), 'horizontal' (16:9), or 'W:H'.\n"
                "face_detect: when true (default), automatically detects the face bounding box "
                "and centers the crop on it with padding — no need to guess x_pct/y_pct.\n"
                "x_pct / y_pct: manual fallback when face_detect=false or no face is found. "
                "Default 0.5/0.5 = dead center.\n"
                "pad_top / pad_bottom: extra padding above/below the detected face as a fraction "
                "of face height (default 0.4 / 0.3).\n\n"
                "Returns {path, width, height, crop, face_detected} plus an inline thumbnail "
                "so you can verify framing immediately. "
                "Supports HEIC, JPG, PNG (images) and MOV, MP4, etc. (video). Read-only — no batch needed."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "source_path":  {"type": "string", "description": "Absolute path to source file"},
                    "aspect":       {"type": "string", "default": "square",
                                     "description": "square | vertical | horizontal | W:H"},
                    "face_detect":  {"type": "boolean", "default": True,
                                     "description": "Auto-detect face and center crop on it"},
                    "pad_top":      {"type": "number", "default": 0.4,
                                     "description": "Padding above face as fraction of face height"},
                    "pad_bottom":   {"type": "number", "default": 0.3,
                                     "description": "Padding below face as fraction of face height"},
                    "x_pct":        {"type": "number", "default": 0.5,
                                     "description": "Horizontal center of crop (0=left, 1=right) — used when face_detect=false"},
                    "y_pct":        {"type": "number", "default": 0.5,
                                     "description": "Vertical center of crop (0=top, 1=bottom) — used when face_detect=false"},
                },
                "required": ["source_path"],
            },
        ),
        Tool(
            name="get_search_status",
            description=(
                "Poll a running transcript search started by find_and_add_clip. "
                "Returns: running (bool), progress (0–1), message, "
                "found (bool), start (seconds), end (seconds), excerpt, error.\n\n"
                "Call every 3s until running=false. Print message each poll so the user sees progress. "
                "When done: if found=true, call extract_clip_segment then add_clip."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="extract_clip_segment",
            description=(
                "Stream-copy a time range from a source media file to a new file (no re-encode). "
                "Near-instant for any codec. Use after find_and_add_clip reports a match to "
                "extract just the relevant segment before adding it to the timeline.\n\n"
                "NEVER use ffmpeg or shell commands to cut audio/video segments — this tool is "
                "instant, codec-agnostic, and handles FLAC/MP3/WAV/MP4/MOV/WebM correctly.\n\n"
                "Returns: {dst, duration}. dst is the path to add as a video clip."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "src":   {"type": "string", "description": "Source media file path"},
                    "dst":   {"type": "string", "description": "Output file path. Use .webm for video sources. For audio-only sources (FLAC/MP3/WAV), match the source extension (e.g. .flac) — the container is chosen from the source format, not the dst extension."},
                    "start": {"type": "number", "description": "Start time in seconds"},
                    "end":   {"type": "number", "description": "End time in seconds"},
                },
                "required": ["src", "dst", "start", "end"],
            },
        ),
        Tool(
            name="get_pipeline_status",
            description=(
                "Returns the current ML pipeline status: stage (idle/extract/transcribe/"
                "align/done/error), progress (0–1), message, and error string. "
                "trigger_pipeline returns immediately with stage='running' — poll this every 2–3s "
                "until stage='done' (or 'error'). No batch needed."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="begin_batch",
            description=(
                "Group multiple mutations into one undo step.\n\n"
                "YOU PROBABLY DON'T NEED THIS for a single mutation. The server auto-batches every "
                "individual mutation call (add_clip, set_clip_prop, add_track, delete_clip, "
                "add_effect_brick, etc.) using the method name as the undo label, so a one-off "
                "set_clip_prop on its own becomes one undo step automatically.\n\n"
                "USE begin_batch when you want a SEQUENCE of mutations to undo as a single step — "
                "e.g. add_track + add_clip + set_clip_prop to set up a clip in one motion, or a "
                "loop of set_clip_prop calls applying a style to many clips. Pass a descriptive "
                "label that reads well in the undo menu (\"Set up intro lyrics\", \"Apply karaoke "
                "preset\").\n\n"
                "Batches can't be nested. Always pair with end_batch — if the connection drops "
                "mid-batch the in-flight changes are saved as \"<label> (incomplete)\" so nothing "
                "is lost."
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
            name="add_to_bin",
            description=(
                "Add a media file to the project Bin. The Bin is the project-scoped media library "
                "shown in the right-side panel — files here are 'available to use' but are not on the "
                "timeline yet. The user (or you, via add_clip) drags from the Bin onto a track to "
                "actually place them.\n\n"
                "When to call this:\n"
                "  - You discovered several candidate files and want to surface them to the user "
                "without committing placements yet.\n"
                "  - The user said something like 'have a look at these clips' or 'find me good "
                "options' — load them into the Bin, then ask which to place.\n"
                "  - You're processing a batch import (extracted segments, cropped variants) where "
                "not every file will end up on the timeline.\n\n"
                "When NOT to call this: don't call it before every add_clip. add_clip on a "
                "video/audio path automatically mirrors the file into the Bin, so for direct "
                "placements just call add_clip and skip this. No batch needed."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "path": {"type": "string", "description": "Absolute path to a media file"},
                },
                "required": ["path"],
            },
        ),
        Tool(
            name="remove_from_bin",
            description=(
                "Remove a media file from the project Bin. Does NOT remove clips already placed "
                "on the timeline that reference the file — those keep working (the timeline holds "
                "its own path reference). Use delete_clip first if you also want to remove its "
                "placements. No batch needed."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "path": {"type": "string", "description": "Absolute path of the bin entry to remove"},
                },
                "required": ["path"],
            },
        ),
        Tool(
            name="add_clip",
            description=(
                "Place a clip on a track.\n\n"
                "BIN vs TIMELINE: add_to_bin makes a media file 'available to the project' (shows in "
                "the Bin panel). add_clip actually places it on the timeline. Use add_to_bin when you've "
                "discovered files the user might want but you're not sure yet; use add_clip when you're "
                "committing to a placement. add_clip on a video/audio path automatically also adds the "
                "file to the bin, so you don't need both calls for files you're placing right away.\n\n"
                "type: video | audio | text | lyrics | subtitle | effect | background | body_fx\n"
                "For video files (.mp4 .mov .webm etc): type='video', text=absolute path.\n"
                "For audio-only files (.flac .mp3 .wav .ogg etc): type='audio', text=absolute path. "
                "NEVER use type='video' for audio-only files — it will fail with 'cannot write output header'.\n"
                "Images (PNG/JPG/HEIC) must be converted to video first with crop_media — "
                "use type='video' with the resulting .mp4 path, not the raw image.\n\n"
                "TEXT CLIPS — read this before adding any:\n"
                "  For LYRICS / CAPTIONS / KARAOKE: do NOT hand-build clips here. Call trigger_pipeline "
                "(it transcribes and lays styled lyric bricks for you), then optionally generate_typography "
                "(preset=...) to swap the visual style. type='lyrics' clips placed here will render with "
                "default positioning that may be off-canvas and won't follow the global typography preset.\n"
                "  For a SINGLE LABELED CALLOUT (label an object, person, action on screen): use add_callout "
                "instead — it sets correct positioning and gives you an arrow option.\n"
                "  type='text' is only for genuine one-off text not covered by the above. If you do use it, "
                "you MUST set sub_pos_x (0.5 = center) and font_size via set_clip_prop or the text will "
                "render off-canvas.\n\n"
                "FRAME SNAPPING — start/end always snap to the project's frame grid (1/fps seconds). The "
                "engine rounds start to the nearest frame and rounds end UP. If you place clips back-to-back "
                "by passing next.start = prev.end (your intended values), the snapped previous-end may push "
                "one frame past your intended next.start, causing an overlap error. RECOVERY: read the error "
                "message — it includes the colliding clip's snapped end (e.g. \"[90.100s – 90.767s]\"). Use "
                "that snapped end as the next clip's start. Better: call get_clips after each add and use the "
                "returned 'end' field as the next clip's start.\n\n"
                "FILE PATH CONVENTIONS:\n"
                "  Cropped media:       {parent}/{stem}_crop.mp4  (video)  or  {parent}/{stem}_crop.png  (image)\n"
                "  Extracted segments:  {parent}/{stem}/{stem}_{start_int}_{end_int}.webm\n"
                "  Transcripts:         {parent}/{stem}/{stem}_words.json\n\n"
                "Always call crop_media before add_clip when the source aspect ratio differs from the canvas."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track":      {"type": "integer"},
                    "track_name": {"type": "string", "description": "Track name (alternative to track index)"},
                    "type": {"type": "string"},
                    "start": {"type": "number"},
                    "end": {"type": "number"},
                    "text": {"type": "string", "default": ""},
                },
                "required": ["type", "start", "end"],
            },
        ),
        Tool(
            name="add_track",
            description=(
                "Add a track at position (0=top/foreground, higher=background). Returns track index.\n\n"
                "LAYERING RULE: track 0 = top (foreground). Highest index = bottom (background).\n"
                "  Text, FX, overlays → low-index tracks (0, 1, 2…)\n"
                "  Video, background  → high-index tracks"
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
            description="Delete a clip.",
            inputSchema={
                "type": "object",
                "properties": {
                    "track":      {"type": "integer"},
                    "track_name": {"type": "string", "description": "Track name (alternative to track index)"},
                    "clip": {"type": "integer"},
                },
                "required": ["clip"],
            },
        ),
        Tool(
            name="move_clip",
            description="Move a clip to a new start time (preserves duration).",
            inputSchema={
                "type": "object",
                "properties": {
                    "track":      {"type": "integer"},
                    "track_name": {"type": "string", "description": "Track name (alternative to track index)"},
                    "clip": {"type": "integer"},
                    "start": {"type": "number"},
                },
                "required": ["clip", "start"],
            },
        ),
        Tool(
            name="trim_clip",
            description="Trim a clip's start and/or end time.",
            inputSchema={
                "type": "object",
                "properties": {
                    "track":      {"type": "integer"},
                    "track_name": {"type": "string", "description": "Track name (alternative to track index)"},
                    "clip": {"type": "integer"},
                    "start": {"type": "number"},
                    "end": {"type": "number"},
                },
                "required": ["clip"],
            },
        ),
        Tool(
            name="split_clip",
            description="Split a clip at a time point. Returns left_clip and right_clip indices.",
            inputSchema={
                "type": "object",
                "properties": {
                    "track":      {"type": "integer"},
                    "track_name": {"type": "string", "description": "Track name (alternative to track index)"},
                    "clip": {"type": "integer"},
                    "time": {"type": "number"},
                },
                "required": ["clip", "time"],
            },
        ),
        Tool(
            name="set_clip_prop",
            description=(
                "Set one property on a clip.\n\n"
                "LAYOUT:    pos_x, pos_y (0–1 canvas fraction), scale_x, scale_y, rotation\n"
                "CROP:      crop_l, crop_t, crop_r, crop_b (0–0.95 fraction trimmed per side,\n"
                "           display orientation). Non-destructive render-time UV window —\n"
                "           prefer this over crop_media when the media is already on the\n"
                "           timeline; the fit box follows the cropped aspect.\n"
                "PLAYBACK:  volume (0–2), speed (0.25–4), opacity (0–1), muted (bool),\n"
                "           fade_in, fade_out, in_point (source offset seconds)\n"
                "TEXT:      text, font_size (0=auto), sub_pos (0=bottom 1=center 2=top 3=custom),\n"
                "           sub_pos_x/y (0–1), sub_anchor_h (0=left 1=center 2=right),\n"
                "           sub_wrap_w (0–1), sub_color ([r,g,b,a] 0–1)\n"
                "ANIMATION: clip_style (none|fade|glitch|typewriter|bounce|scale|slide|stack|block),\n"
                "           blend_mode (normal|add|multiply|screen|overlay)\n"
                "COLOR GRADE (video clips only):\n"
                "           grade_brightness (-1–1), grade_contrast (0–3),\n"
                "           grade_saturation (0–3), grade_hue (-180–180)\n"
                "AUDIO SYNC: To sync audio to a video at a specific source moment, use a negative\n"
                "           clip start: start = -source_timestamp, end = video_duration - source_timestamp"
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track":      {"type": "integer"},
                    "track_name": {"type": "string", "description": "Track name (alternative to track index)"},
                    "clip": {"type": "integer"},
                    "prop": {"type": "string"},
                    "value": {},
                },
                "required": ["clip", "prop", "value"],
            },
        ),
        Tool(
            name="add_clip_sequence",
            description=(
                "Add multiple clips to a single track in one round-trip. Equivalent to calling "
                "add_clip N times but dramatically faster for beat-sync edits or any bulk layout. "
                "Returns an array of assigned clip IDs in order.\n\n"
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
            description="Set properties on multiple clips in one call. ops: [{track, clip, prop, value}].",
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
                "fields are updated. Color arrays are [r, g, b, a] with values 0–1.\n"
                "Fields: shadow_enabled (bool), shadow_ox, shadow_oy, shadow_col,\n"
                "  stroke_enabled, stroke_w, stroke_col,\n"
                "  glow_enabled, glow_r, glow_col,\n"
                "  bg_enabled, bg_col, bg_pad_x, bg_pad_y, bg_corner\n\n"
                "WARNING — do NOT call this on lyrics/subtitle clips generated by trigger_pipeline. "
                "Those clips inherit style from the global preset; overriding a single clip detaches it "
                "from the global system and produces inconsistent results. "
                "If the user wants a different text style, call generate_typography with the preset "
                "that best matches their request — the preset IS the style. "
                "Only use set_text_style on individual text clips (type='text') when the user explicitly "
                "asks for a per-clip override."
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
            name="generate_typography",
            description=(
                "Lay lyric/subtitle bricks on the timeline using a typography preset. Call this AFTER "
                "trigger_pipeline finishes (stage='done' via get_pipeline_status) — that's how lyrics "
                "get onto the timeline. Also call again to swap to a different visual style: idempotent, "
                "clears the existing lyric clips for the current audio source and re-lays them with the "
                "new preset.\n\n"
                "PRESET SYSTEM: each preset bundles grouping, position, animation, color, and optional FX clips. "
                "The preset IS the style — do NOT call set_text_style on the generated clips afterward to tweak "
                "them; that breaks the global styling. Match the user's style request to the closest preset here "
                "and use that preset. Karaoke is a preset — use preset='karaoke', do NOT set karaoke=true manually.\n\n"
                "INTERPRET, DON'T HAND-ROLL: if the user describes a style (e.g. 'bold white caps with drop shadow', "
                "'tiktok one-word', 'spotify bottom bar'), map it to the closest preset below and call this tool. "
                "Do NOT build text clips manually via add_clip(type='text') just because the user's wording doesn't "
                "match a preset name verbatim — the presets are deliberately broad and the closest fit is correct.\n\n"
                "PRESET GUIDE — pick based on user's style request:\n"
                "  flash      — ONE WORD · white · all-caps · hard cuts · no effects  ← TikTok lyric video default\n"
                "  strobe     — same as flash but inverts color every word · ultra aggressive\n"
                "  rave       — one word · neon pink · random positions · beat-reactive · chromatic aberration FX\n"
                "  cyberpunk  — 3 words · cyan · monospace · bottom-left · glitch FX\n"
                "  drill      — 3 words · yellow · top third · aggressive cuts\n"
                "  tumblr     — phrases · white · lowercase · centered · fade\n"
                "  indie2012  — lines · muted off-white · small · left offset · fade\n"
                "  sadgirl    — one word · pastel pink · large · centered · slow fade\n"
                "  cottagecore— lines · warm cream · centered · gentle fade · film grain FX\n"
                "  film       — lines · white · bottom subtitle style · classic fade\n"
                "  headline   — ONE WORD · white · all-caps · massive · scale animation\n"
                "  manifesto  — lines · white · all-caps · left flush · stacked · block anim\n"
                "  zine       — phrases · white · mixed case · glitch animation\n"
                "  newspaper  — segments · white · all-caps · tight centered block\n"
                "  minimal    — phrases · light grey · small · centered · fade · breathing room\n"
                "  spotify    — phrases · white · bottom center · clean · fade\n"
                "  apple      — ONE WORD · white · large · centered · smooth fade\n"
                "  kinetic    — phrases · white · centered · slides in from left\n"
                "  karaoke    — lines · grey → highlighted word · bottom · classic karaoke\n"
                "  vhs        — lines · white · bottom · VHS grain FX\n"
                "  neon       — one word · hot pink · glow · beat-reactive · centered\n"
                "  lofi       — phrases · warm · small · film grain FX · chill\n\n"
                "MATCHING EXAMPLES:\n"
                "  'one word at a time, bold white, no effects' → flash\n"
                "  'one word, white, big, clean fade' → apple\n"
                "  'one word, white, really massive fills frame' → headline\n"
                "  'tiktok style word by word' → flash\n"
                "  'spotify style' → spotify\n"
                "  'karaoke / word highlight' → karaoke\n"
                "  'aesthetic, pastel' → sadgirl\n"
                "  'retro, vhs' → vhs"
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "preset": {
                        "type": "string",
                        "description": (
                            "Typography preset id. One of: flash, strobe, rave, cyberpunk, drill, "
                            "tumblr, indie2012, sadgirl, cottagecore, film, headline, manifesto, "
                            "zine, newspaper, minimal, spotify, apple, kinetic, karaoke, vhs, neon, lofi. "
                            "Defaults to the app's currently selected preset (spotify) if omitted."
                        ),
                    }
                },
            },
        ),
        Tool(
            name="trigger_pipeline",
            description=(
                "Kick off the ML pipeline (Demucs + transcription) on the project audio. NON-BLOCKING: "
                "returns immediately with stage='running'. Poll get_pipeline_status every 2–3s until "
                "stage='done', then call generate_typography(preset='...') to lay the lyric/subtitle "
                "bricks on the timeline. No batch needed.\n\n"
                "FULL WORKFLOW:\n"
                "  1. add_track('Audio') + add_clip(type='audio', text=<path>) — audio MUST be on the\n"
                "     timeline before this tool will accept the call.\n"
                "  2. trigger_pipeline(mode='both')  → returns stage='running'\n"
                "  3. loop: get_pipeline_status() every 2–3s until stage='done' (or 'error').\n"
                "  4. generate_typography(preset='flash')  ← lays the lyric bricks in the chosen style.\n"
                "  5. (optional) generate_typography(preset='...') again to swap the visual style.\n\n"
                "PROBE FIRST: call get_transcript() — if status='ready' the transcript is already cached on disk. "
                "If lyric bricks are already on the timeline you can skip this entirely. "
                "Only call trigger_pipeline when no transcript exists for this audio.\n\n"
                "PIPELINE MODES:\n"
                "  both            — (default) Demucs stem separation → vocals.wav → transcription. "
                "Transcribes the isolated vocal stem, not the raw mix. Best for music.\n"
                "  transcribe_only — Skip separation, transcribe source audio directly. Faster; use for speech/podcasts.\n"
                "  separate_only   — Run Demucs only, no transcription. No bricks to lay afterward.\n\n"
                "PRESET QUICK-PICKS for step 4 (full list in generate_typography):\n"
                "  flash    — one word, white, all caps, hard cuts (TikTok default)\n"
                "  apple    — one word, white, large, smooth fade\n"
                "  spotify  — phrases, white, bottom center, clean fade\n"
                "  karaoke  — line with current word highlighted, bottom\n"
                "  headline — one word, massive, scale animation\n\n"
                "DO NOT use this to search for a specific moment — use find_and_add_clip instead "
                "(windowed search, much faster).\n\n"
                "NEVER run this on a full-length song/track when the user only wants a section. "
                "Instead: (1) find_and_add_clip(path=full_file, query='end phrase') — returns exact end "
                "timestamp AND auto-extracts the segment to result.dst; "
                "(2) extract_clip_segment(src=full_file, start=0, end=result.end) → short clip; "
                "(3) add_track + add_clip(audio, short clip); "
                "(4) trigger_pipeline on the short clip only. "
                "NEVER add padding beyond result.end — find_and_add_clip gives exact word-boundary timestamps."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "mode": {
                        "type": "string",
                        "enum": ["both", "transcribe_only", "separate_only"],
                        "default": "both",
                    },
                    "path": {
                        "type": "string",
                        "description": "Absolute path to media file. Transcribes this file without adding it to the timeline.",
                    },
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
                "Save the project to disk as a .pms file. Always use the .pms extension — the UI "
                "file picker filters by *.pms, so any other extension (.pmsproj, .json, etc.) "
                "produces a file the user can't reopen from the app. "
                "If path is omitted, saves to the last-used path. Returns the path written. No batch needed."
            ),
            inputSchema={
                "type": "object",
                "properties": {"path": {"type": "string", "description": "Absolute path ending in .pms"}},
            },
        ),
        Tool(
            name="load_project",
            description="Load a .pms project file, replacing the current project.",
            inputSchema={
                "type": "object",
                "properties": {"path": {"type": "string", "description": "Absolute path to a .pms file"}},
                "required": ["path"],
            },
        ),
        Tool(
            name="add_multifx_brick",
            description=(
                "Add an ordered chain of FX sub-effects as one brick. Prefer over multiple overlapping add_effect_brick calls.\n\n"
                "Same-track as a video clip = glass mode (affects that clip only). "
                "Separate FX track = global (affects all layers below).\n\n"
                "effects[]: {fx_type, rel_start (0), rel_end (0=brick end), params, body_fx_type (if fx_type='body_fx')}\n"
                "body_fx constraint: must be glass mode (same track as video clip)."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track":      {"type": "integer"},
                    "track_name": {"type": "string", "description": "Track name (alternative to track index)"},
                    "start": {"type": "number"},
                    "end": {"type": "number"},
                    "effects": {
                        "type": "array",
                        "description": "Ordered list of sub-effects",
                        "items": {
                            "type": "object",
                            "properties": {
                                "fx_type": {"type": "string", "description": "Any fx_type from add_effect_brick, or 'body_fx'"},
                                "body_fx_type": {"type": "string", "description": "Required when fx_type is 'body_fx'"},
                                "rel_start": {"type": "number", "default": 0},
                                "rel_end": {"type": "number", "default": 0, "description": "0 = until brick end"},
                                "params": {"type": "object"},
                            },
                            "required": ["fx_type"],
                        },
                    },
                },
                "required": ["start", "end"],
            },
        ),
        Tool(
            name="new_project",
            description=(
                "Reset the project to a blank state (clears all tracks, clips, audio, beats). "
                "Call get_project first — if tracks=[], audio_path='', and duration=0 the project is "
                "already blank; skip this call and go straight to set_format. Only call new_project "
                "when the project has existing content you need to discard.\n\n"
                "PROBE BEFORE ASKING: When the user provides media files, use tools to answer factual "
                "questions yourself before asking the user. See get_project description for the full rule."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="rename_track",
            description="Rename a track.",
            inputSchema={
                "type": "object",
                "properties": {
                    "track":      {"type": "integer"},
                    "track_name": {"type": "string", "description": "Track name to look up (alternative to track index)"},
                    "name": {"type": "string", "description": "New name to assign"},
                },
                "required": ["name"],
            },
        ),
        Tool(
            name="add_effect_brick",
            description=(
                "Add a standalone FX brick to a track. Affects everything below on the timeline "
                "(or only the sibling video clip if on the same track — glass mode).\n\n"
                "fx_type — use the exact snake_case name. All params are floats. "
                "'body_fx' is NOT valid here — use add_body_fx_brick instead.\n\n"
                "BASIC: grade (brightness,contrast,saturation,hue) | blur (blur) | vignette (vignette) | "
                "glitch (glitch_chroma 0-20,glitch_jitter,glitch_corruption,glitch_corruption_bleed) | "
                "zoom_punch (zoom_strength,zoom_decay,zoom_shake) | light_leak (leak_intensity,leak_speed) | "
                "vhs (vhs_noise,vhs_bleed,vhs_tracking) | datamosh (datamosh_intensity,datamosh_spread) | "
                "chroma_key (chroma_key_r/g/b,chroma_key_threshold,chroma_key_softness) | "
                "lut (no params—set lut_path via set_clip_prop) | "
                "ken_burns (start_scale,end_scale,start_x,start_y,end_x,end_y)\n\n"
                "GLITCH: pixelate (amount,size 1-64) | glitch_block (amount,intensity,speed) | "
                "interlace_glitch (amount,intensity,speed) | data_corrupt (amount,density,block_size,intensity) | "
                "double_ghost (amount,offset,opacity,angle) | rgb_split_wave (amount,amplitude,frequency,speed) | "
                "bit_crush (amount,levels 2-32,dither) | tv_static (amount,intensity,color_mix) | "
                "dither_bayer (amount,levels,scale,color) | vhs_dropout (amount,density,speed)\n\n"
                "FILM: film_grain (amount,intensity,size) | old_film (amount,sepia,scratch,flicker) | "
                "lomo (amount,vignette,saturation,fade) | super8_film (amount,grain,gate,fade) | "
                "daguerreotype (amount,tone,vignette,scratch) | bleach_bypass (amount) | "
                "film_halation (amount,threshold,radius,red_shift) | film_burn (amount,intensity,speed,edge)\n\n"
                "COLOR: chromatic_aberration (amount) | duotone (amount,shadow_r/g/b,highlight_r/g/b) | "
                "gradient_map (amount,hue1,hue2) | cross_process (amount,contrast) | "
                "technicolor (amount,saturation,contrast,warmth) | kodachrome (amount,saturation,reds,shadows) | "
                "miami_vice (amount,saturation) | golden_hour (amount,warmth,glow_str,shadow_lift,vignette) | "
                "split_toning (amount,shadow_hue,hi_hue) | solarize (amount,threshold) | "
                "warhol_pop (amount,levels,hue_shift,saturation) | cyberpunk_grade (amount,shadow_teal,hi_orange,contrast) | "
                "sepia_rich (amount,vignette,contrast) | color_burn (amount,hue) | "
                "horror_grade (amount,desat,red,crush) | desert_gold (amount,warmth,fade,haze) | "
                "infrared_film (amount,channel_mix,glow,contrast) | x_ray (amount,contrast,blue_tint) | "
                "vintage_negative (amount,orange_mask,contrast,grain) | "
                "zone_system_bw (amount,zones,contrast,grain,paper_white) | "
                "thermal (amount) | night_vision (amount,noise,gain) | holographic (amount,speed) | "
                "rgb_split (amount,intensity,speed) | color_dodge (amount,intensity,hue,glow) | "
                "sketch (amount,invert) | emboss_relief (amount,angle,colorize)\n\n"
                "LIGHT: neon_glow (amount,width) | god_rays (amount,intensity,decay,cx,cy) | "
                "aurora_borealis (amount,intensity,speed,color_shift) | "
                "starburst_spike (amount,threshold,length,rays) | bokeh_dream (amount,radius,threshold,intensity) | "
                "neon_edge_glow (amount,threshold,glow,hue) | neon_sign (amount,edge_str,glow_radius,hue_shift,bg_darken) | "
                "plasma_field (amount,scale,speed,intensity) | fire_edge (amount,intensity,speed,height) | "
                "laser_grid (amount,grid_size,hue,intensity) | anamorphic_streak (amount,threshold,length,intensity) | "
                "prism_disperse (amount,spread,intensity) | glitter_dust (amount,density,size,sparkle,color_var) | "
                "dna_helix (amount,grid_scale,wave_amp,line_width,hue,bg_darken)\n\n"
                "WARP: fisheye (amount) | twirl (amount,radius) | ripple (amount,frequency,amplitude,speed) | "
                "wave_warp (amount,freq_x,freq_y,amplitude,speed) | kaleidoscope (amount,segments,rotation,zoom) | "
                "mirror_fold (amount,axis,vertical) | vortex_distort (amount,scale,speed) | "
                "barrel_warp (amount,k1,k2,scale) | tilt_shift (amount,focus_y,focus_band,blur_radius,saturation) | "
                "mirror_tunnel (amount,depth,rotation,zoom) | liquid_chrome (amount,flow,metallic,tint_r,tint_g,tint_b) | "
                "zoom_blur_rad (amount,intensity,cx,cy) | spin_blur (amount,angle) | "
                "heat_haze (amount,intensity,speed) | frosted_glass (amount,blur,noise,tint) | "
                "echo_trails (amount,offset,fade,angle) | "
                "double_exposure (amount,offset_x,offset_y,scale2,desaturate2,opacity) | "
                "ice_crystal (amount,scale,refract,tint) | raindrop_refract (amount,density,size,refract_str) | "
                "oil_paint (amount,radius,sharpness) | watercolor (amount,bleeding,paper,saturation)\n\n"
                "PATTERN: scanlines (amount,density) | halftone (amount,size) | posterize (amount,levels) | "
                "crt (amount,curvature,glow) | crt_barrel (amount,distort,corner_dark,rgb_shift,scanline) | "
                "pixel_mosaic (amount,block_size,color_steps) | ascii_art (amount,char_size,fg_r,fg_g,fg_b,bg_dark) | "
                "comic_dots (amount,dot_size,ink_threshold,color_levels) | crosshatch (amount,density,thickness,angle) | "
                "stained_glass (amount,cell_size,border,saturation) | matrix_rain (amount,density,speed,green_mix) | "
                "pixel_sort (amount,threshold,intensity,direction) | pointillist (amount,dot_size,scatter) | "
                "scanline_color (amount,line_width,intensity,rgb_sep) | "
                "contour_map (amount,levels,line_width,line_hue,fill_sat) | "
                "risograph (amount,hue1,hue2,dot_size,misreg,paper) | "
                "pencil_sketch (amount,line_str,paper_tone,hatching) | "
                "long_exposure (amount,threshold,trail,glow) | "
                "thermal_map (amount,cold_hue,hot_hue,contrast,scanlines) | "
                "digital_noise (amount,intensity,color_sep,luma_bias)"
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track":      {"type": "integer"},
                    "track_name": {"type": "string", "description": "Track name (alternative to track index)"},
                    "fx_type": {"type": "string", "description": "Snake_case FX type name (see description)"},
                    "start": {"type": "number"},
                    "end": {"type": "number"},
                    "params": {
                        "type": "object",
                        "description": "Effect-specific parameter values by name (see description)",
                    },
                },
                "required": ["fx_type", "start", "end"],
            },
        ),
        Tool(
            name="analyze_audio",
            description=(
                "Run beat/RMS analysis on an audio file. Blocks until complete — "
                "returns {status: 'done', bpm, duration, beats, rms} when finished. "
                "No polling needed."
            ),
            inputSchema={
                "type": "object",
                "properties": {"path": {"type": "string", "description": "Absolute path to audio file"}},
                "required": ["path"],
            },
        ),
        Tool(
            name="get_audio_analysis",
            description=(
                "Poll beat/RMS analysis started by analyze_audio. "
                "Returns {status: 'idle'|'running'|'done'|'error', bpm?, duration?, beats?, rms?}. "
                "Poll every 2s until status='done'."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="find_audio_cue",
            description=(
                "Find a beat-aligned timestamp matching a description ('first drop', 'chorus', 'outro', etc.).\n"
                "Requires: get_audio_analysis status='done' (call analyze_audio first, then poll).\n"
                "Returns: source_timestamp, bpm, duration, reasoning, alternatives. "
                "Audio positioning: clip start = -source_timestamp, end = video_duration - source_timestamp."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "path":          {"type": "string", "description": "Absolute path to the audio file"},
                    "description":   {"type": "string", "description": "What you're looking for"},
                    "clip_duration": {"type": "number", "description": "Clip length in seconds (optional, for fit check)"},
                },
                "required": ["path", "description"],
            },
        ),
        Tool(
            name="get_vision_model_status",
            description=(
                "Check whether the local Moondream2 vision model is installed. "
                "Returns {status: 'ready'|'downloading'|'idle'|'error', progress?, message?}. "
                "Poll every 3s after download_vision_model until status='ready'."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="download_vision_model",
            description="Start Moondream2 download (~1.1 GB, one-time). Returns immediately. Poll get_vision_model_status until status='ready'.",
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="describe_video",
            description=(
                "DO NOT CALL THIS TOOL. The built-in vision model is broken and returns empty descriptions.\n\n"
                "Instead, use YOUR OWN vision capability:\n"
                "  1. Run: ffmpeg -y -ss 0.5 -i <path> -vframes 1 -vf scale=480:-1 /tmp/still_<name>.jpg -loglevel quiet\n"
                "  2. Read the JPEG with your Read tool — you will see the image.\n"
                "  3. Describe what you see and use that for mood/content matching.\n\n"
                "ONE STILL IS NOT ENOUGH IF YOU DON'T SEE WHAT YOU'RE LOOKING FOR. The first 500 ms "
                "frame may be a black intro, a logo, a wide establishing shot, or a transition with "
                "none of the actual subject. When the still doesn't show the thing the user asked "
                "about (person, action, object, setting), DO NOT conclude the video lacks it — sample "
                "more frames before deciding:\n"
                "  a. Get duration via get_media_info(path) (or ffprobe).\n"
                "  b. Sample ~5 stills spread across the timeline — e.g. at 10%, 30%, 50%, 70%, 90% "
                "of duration. Generate them in parallel: one bash command with multiple ffmpeg "
                "invocations joined by '&' and a final 'wait', or 5 Bash tool calls in a single message.\n"
                "  c. Read all of them and synthesise — describe the video as a whole, not as one frame.\n"
                "  d. If you're scanning for a specific moment, narrow down: once you find the rough "
                "neighbourhood (between still N and N+1), sample 3–5 more stills inside that window "
                "to pin the timestamp more precisely. Binary-search style.\n\n"
                "Repeat the full process for each video. Faster and more accurate than any local model."
            ),
            inputSchema={
                "type": "object",
                "properties": {"path": {"type": "string", "description": "Absolute path to the video file"}},
                "required": ["path"],
            },
        ),
        Tool(
            name="get_video_description",
            description=(
                "DO NOT USE — the built-in vision model is broken. "
                "Use ffmpeg + your own Read tool to view frames instead (see describe_video for instructions)."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="find_video_moment",
            description=(
                "DO NOT USE — depends on the broken built-in vision model (see describe_video).\n\n"
                "To find a moment in a video by visual content: capture a still per timestamp with\n"
                "  ffmpeg -y -ss <t> -i <path> -vframes 1 -vf scale=480:-1 /tmp/still_<t>.jpg -loglevel quiet\n"
                "then Read each JPEG and pick the one matching the query yourself."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "path":  {"type": "string", "description": "Absolute path to the video file"},
                    "query": {"type": "string", "description": "Natural-language description of the moment"},
                },
                "required": ["path", "query"],
            },
        ),
        Tool(
            name="find_and_add_clip",
            description=(
                "Find a specific spoken moment in a media file and add it to the timeline. "
                "Does windowed speech-to-text search — chunk size is calculated from the file length "
                "(clamp(duration/4, 90s, 300s)) with 15% overlap — stops as soon as the match is found. "
                "Much faster than trigger_pipeline on long files.\n\n"
                "WORKS ON AUDIO-ONLY FILES TOO (FLAC, MP3, WAV, etc.) — not just video. "
                "This means you can locate an exact timestamp in a song or podcast without running "
                "the full pipeline. Useful for finding where a lyric or phrase occurs so you can "
                "trim the project to that point before generating typography.\n\n"
                "FOR LYRIC VIDEOS / SCOPED SECTIONS — use this BEFORE trigger_pipeline when the user wants "
                "only a section of a song. The canonical flow is:\n"
                "  1. find_and_add_clip(path=full_file, query='start phrase')  → start timestamp\n"
                "  2. find_and_add_clip(path=full_file, query='end phrase')    → end timestamp\n"
                "  3. extract_clip_segment(src=full_file, dst='/tmp/scoped.flac', start=startA, end=endB + tail_sec)\n"
                "  4. add_track + add_clip(audio, /tmp/scoped.flac) + trigger_pipeline\n"
                "  5. poll get_pipeline_status until stage='done', then generate_typography(preset='flash')\n"
                "DO NOT skip step (2): the auto-extracted dst only covers a small window around the matched "
                "phrase, not from start to end of section. You need both timestamps to extract the full span.\n\n"
                "CHUNK-STRADDLING PHRASES are handled internally: when a partial match lands at the trailing "
                "edge of a search chunk, the scanner extends into the next chunk before reporting found, so "
                "result.end reflects the real phrase end (not the chunk boundary). Same goes for queries whose "
                "match lives past an existing cached transcript — the cache is extended automatically.\n\n"
                "Blocks until the match is found — no polling needed. Returns status=found.\n\n"
                "Always returns extracted=true with a ready-to-use dst file. NEVER call extract_clip_segment "
                "manually after this — the tool handles extraction internally.\n\n"
                "result includes in_point = offset into dst where your content starts.\n\n"
                "MATCH QUALITY: result includes score (0-1, fraction of query words found) and "
                "partial_match (true when score<1.0 OR the located span is wider than ~1.5s/word). "
                "When partial_match=true, treat start/end as APPROXIMATE — the located span may include "
                "leading/trailing context rather than just the query phrase. Do not derive exact word "
                "timestamps from a partial match; re-run with a corrected query or use trigger_pipeline + "
                "get_transcript on the matched segment for word-level alignment.\n\n"
                "add_clip: track=<target>, type=video, text=result.dst, start=0, end=result.clip_duration, in_point=result.in_point"
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "path":    {"type": "string",  "description": "Absolute path to the media file (video or audio)"},
                    "query":   {"type": "string",  "description": "What to search for in the transcript"},
                    "track":   {"type": "integer", "description": "Track to add the clip to (default 0)"},
                    "padding": {"type": "number",  "description": "Seconds of context before/after match (default 5.0)"},
                },
                "required": ["path", "query"],
            },
        ),
        Tool(
            name="get_transcript",
            description=(
                "Return the word-level transcript produced by the transcription pipeline. "
                "Returns {status: 'idle'|'ready'|'error', words?: [{word, start, end}], path?}. "
                "Word timestamps are source-file-relative seconds.\n\n"
                "By default reads the project's current transcript. Pass `path` to inspect "
                "the cached transcript of any previously-transcribed file (e.g. the full song "
                "before you sectioned it) without changing project state.\n\n"
                "STATUS IDLE — what to do next depends on your task:\n"
                "  Finding a specific moment/phrase → use find_and_add_clip (windowed, stops early, MUCH faster).\n"
                "    DO NOT run trigger_pipeline just to search.\n"
                "  Generating full subtitles/karaoke for a clip already on the timeline → use trigger_pipeline.\n"
                "  Re-using a previously-transcribed file's words for a new section audio → use shift_transcript."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "path": {"type": "string", "description": "Optional: read the cached transcript for this file path instead of the project's current one. Read-only — does not change project state."},
                },
            },
        ),
        Tool(
            name="set_transcript",
            description=(
                "Install a custom word-level transcript for the project's current audio. "
                "Writes the canonical cache JSON next to the audio file and reloads the "
                "in-memory word cache so generate_typography uses your words immediately.\n\n"
                "USE THIS WHEN:\n"
                "  - WhisperX on the current audio produced wrong / sparse word timings and "
                "you want to override with hand-edited words.\n"
                "  - You computed words from another source (different transcriber, manual "
                "alignment) and want the managed Lyrics path to use them.\n\n"
                "DON'T USE THIS FOR: shifting/clipping a transcript across audio files — call "
                "shift_transcript, which handles the offset and source-file selection for you.\n\n"
                "After this call, generate_typography(preset='flash'|...) will lay lyric clips at "
                "the timings you provided. words: [{word: str, start: float, end: float}, ...]"
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "words": {
                        "type": "array",
                        "description": "Word list to install as the project transcript",
                        "items": {
                            "type": "object",
                            "properties": {
                                "word":  {"type": "string"},
                                "start": {"type": "number", "description": "Seconds, audio-relative"},
                                "end":   {"type": "number", "description": "Seconds, audio-relative"},
                            },
                            "required": ["word", "start", "end"],
                        },
                    },
                },
                "required": ["words"],
            },
        ),
        Tool(
            name="shift_transcript",
            description=(
                "Repurpose another file's cached transcript as the project's transcript, "
                "with a time shift and optional clipping. Closes the gap when a long source "
                "(e.g. full song) has a clean WhisperX transcript but you're working on an "
                "extracted section where re-transcribing produces worse results.\n\n"
                "FLOW: extract section /tmp/section.flac from full.flac starting at 88.71s → "
                "add the section as the project audio → "
                "shift_transcript(source_path='/path/to/full.flac', offset=-88.71, start=88.71, end=127) → "
                "generate_typography(preset='flash'). Done.\n\n"
                "source_path: optional — the file whose cached transcript to read. Defaults to "
                "the project's current transcript (useful for shifting in place).\n"
                "start, end: optional — keep only words overlapping this range in SOURCE-file "
                "time (pre-shift). Use to clip to the section you extracted.\n"
                "offset: required — seconds added to every word's start/end. Pass the negative "
                "of the section's start timestamp to convert source-time to section-time."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "offset":      {"type": "number", "description": "Seconds added to every word timestamp (pass -section_start to convert source time → section time)"},
                    "start":       {"type": "number", "description": "Lower bound in source-file seconds (pre-shift) for clipping"},
                    "end":         {"type": "number", "description": "Upper bound in source-file seconds (pre-shift) for clipping"},
                    "source_path": {"type": "string", "description": "Optional source file whose transcript to read. Defaults to project's current transcript."},
                },
                "required": ["offset"],
            },
        ),
        Tool(
            name="read_transcript_context",
            description=(
                "Read words from the saved transcript for a source video file around a specific timestamp. "
                "Use after find_and_add_clip to find exact speaker boundaries, sentence starts, etc.\n\n"
                "Returns {utterances, words}.\n"
                "utterances: words grouped by pauses — each is {start, end, gap_before, text}. "
                "gap_before > 0 marks a likely speaker change (default threshold 0.8s). "
                "Use utterances to read the transcript; use words only when you need per-word timestamps.\n"
                "words: raw [{word, start, end}] for the window [time-before, time+after]."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "path":          {"type": "string", "description": "Absolute path to the source video file"},
                    "time":          {"type": "number", "description": "Center timestamp in source-file seconds"},
                    "before":        {"type": "number", "description": "Seconds before 'time' to include (default 30)"},
                    "after":         {"type": "number", "description": "Seconds after 'time' to include (default 60)"},
                    "gap_threshold": {"type": "number", "description": "Pause length in seconds that splits utterances (default 0.8)"},
                },
                "required": ["path", "time"],
            },
        ),
        Tool(
            name="search_transcript",
            description=(
                "Search the transcript for ALL occurrences of a phrase. Returns every match with "
                "source-file timestamps — use this before any phrase-based cut to see if the phrase "
                "appears more than once. Timestamps are source-file seconds (subtract clip in_point "
                "to get timeline position). Requires trigger_pipeline or find_and_add_clip to have "
                "run first."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "phrase": {"type": "string", "description": "The phrase to search for"},
                    "path":   {"type": "string", "description": "Source video path (defaults to project audio_path)"},
                },
                "required": ["phrase"],
            },
        ),
        Tool(
            name="cut_at_phrase",
            description=(
                "Trim a clip to end (or start) right at a phrase. Collapses transcript lookup + "
                "trim into one call.\n\n"
                "side='after' (default): trims clip end to just after the last word of the phrase.\n"
                "side='before': trims clip start to just before the first word.\n\n"
                "If the phrase appears more than once, returns all matches and does NOT trim — "
                "re-call with occurrence=N (0-indexed) to select which one.\n\n"
                "Requires a ready transcript. To get one without running the full pipeline:\n"
                "  use find_and_add_clip first (windowed search, much faster than trigger_pipeline).\n"
                "Only use trigger_pipeline if you need a full transcript for subtitles/karaoke."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track":      {"type": "integer"},
                    "clip":       {"type": "integer"},
                    "phrase":     {"type": "string"},
                    "side":       {"type": "string", "enum": ["after", "before"], "default": "after"},
                    "occurrence": {"type": "integer", "description": "0-indexed match to use when multiple exist"},
                },
                "required": ["track", "clip", "phrase"],
            },
        ),
        Tool(
            name="remove_silence",
            description=(
                "Automatically detect and remove silent segments from a video or audio clip. "
                "Uses per-second RMS energy (1-second resolution — pauses shorter than ~0.5s "
                "may not be detected). Works back-to-front so clip indices stay valid. "
                "Returns {removed: [{start, end, duration}, ...], count}.\n\n"
                "Typical threshold: 0.04–0.08 for speech; min_duration: 0.5–1.0s; "
                "padding: 0.1–0.2s to avoid clipping words."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track":        {"type": "integer", "description": "Track index"},
                    "clip":         {"type": "integer", "description": "Clip index within the track"},
                    "threshold":    {"type": "number",  "description": "RMS below this = silence (0–1). Default 0.05"},
                    "min_duration": {"type": "number",  "description": "Minimum silence length in seconds to remove. Default 0.5"},
                    "padding":      {"type": "number",  "description": "Seconds of audio to preserve on each side of a cut. Default 0.15"},
                },
                "required": ["track", "clip"],
            },
        ),
        Tool(
            name="cut_filler_words",
            description=(
                "Remove filler words ('um', 'uh', 'like', etc.) from a clip using the "
                "word-level transcript. Run trigger_pipeline and wait for it to finish first. "
                "Use dry_run=true to preview matches before committing. "
                "Returns {removed: [{word, start, end}, ...], count} or "
                "{matches: [...]} in dry-run mode."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track":    {"type": "integer", "description": "Track index"},
                    "clip":     {"type": "integer", "description": "Clip index within the track"},
                    "words":    {
                        "type": "array",
                        "items": {"type": "string"},
                        "description": (
                            "Filler words/phrases to remove. "
                            "Default: [\"um\",\"uh\",\"uh-huh\",\"like\",\"you know\","
                            "\"kind of\",\"sort of\",\"basically\",\"literally\","
                            "\"actually\",\"right\",\"okay\"]"
                        ),
                    },
                    "padding":  {"type": "number",  "description": "Seconds to trim from each side to avoid clipping. Default 0.04"},
                    "dry_run":  {"type": "boolean", "description": "If true, return matches without making cuts. Default false"},
                },
                "required": ["track", "clip"],
            },
        ),
        Tool(
            name="apply_multicam_cuts",
            description=(
                "Given multiple camera-angle clips on parallel tracks, apply a cut list that "
                "keeps only the selected camera in each time window. All other camera-track "
                "clips in that window are deleted. Non-camera tracks (captions, FX, etc.) are "
                "left untouched.\n\n"
                "Precondition: each camera track has a clip covering the full range. "
                "Returns {windows: [{start, end, track}], deleted_count}.\n\n"
                "Example: cuts=[{time:0,track:2},{time:8.3,track:3}], camera_tracks=[2,3]"
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "cuts": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": {
                                "time":  {"type": "number",  "description": "Timeline time (seconds) where this camera takes over"},
                                "track": {"type": "integer", "description": "Track index to use from this time onward"},
                            },
                            "required": ["time", "track"],
                        },
                        "description": "Ordered list of camera switches. Must start at or before the earliest clip start.",
                    },
                    "camera_tracks": {
                        "type": "array",
                        "items": {"type": "integer"},
                        "description": "Which track indices are camera tracks. Only these will have segments deleted.",
                    },
                },
                "required": ["cuts", "camera_tracks"],
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
        Tool(
            name="add_chapter_marker",
            description=(
                "Add a chapter/section marker at a specific timeline position. "
                "Markers appear as colored vertical lines with labels in the timeline ruler. "
                "They are sorted by time automatically. "
                "Returns {index} of the inserted marker."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "time":  {"type": "number",  "description": "Timeline position in seconds"},
                    "label": {"type": "string",  "description": "Chapter label (short text displayed in ruler)"},
                    "color": {"type": "string",  "description": "Hex color e.g. '#4A90E2' (optional, defaults to cornflower blue)"},
                },
                "required": ["time", "label"],
            },
        ),
        Tool(
            name="remove_chapter_marker",
            description="Remove a chapter marker by its index (from get_project markers array or add_chapter_marker response).",
            inputSchema={
                "type": "object",
                "properties": {
                    "index": {"type": "integer", "description": "Marker index to remove"},
                },
                "required": ["index"],
            },
        ),
        Tool(
            name="generate_chapters",
            description=(
                "Auto-generate chapter markers from the transcript by finding natural pause points. "
                "Call get_transcript first — this tool requires a ready transcript.\n\n"
                "Two modes (mutually exclusive):\n"
                "  num_chapters: place exactly N chapters by splitting at the N-1 longest pauses\n"
                "  min_pause_seconds: place a chapter wherever a pause exceeds this threshold (default 3.0s)\n\n"
                "Always adds a first chapter at t=0 labeled from the first few words. "
                "Returns {chapters: [{time, label}]}."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "num_chapters":       {"type": "integer", "description": "Exact number of chapters to generate (overrides min_pause_seconds)"},
                    "min_pause_seconds":  {"type": "number",  "description": "Minimum gap in seconds to trigger a chapter boundary (default 3.0)"},
                },
            },
        ),
        Tool(
            name="add_callout",
            description=(
                "Add a callout text overlay — a positioned text box with optional arrow pointing at a canvas location. "
                "Good for labelling objects, people, or actions on screen.\n\n"
                "Position (pos_x, pos_y) and arrow target (arrow_x, arrow_y) are canvas fractions: "
                "0,0 = top-left, 1,1 = bottom-right, 0.5,0.5 = center.\n\n"
                "callout_style: 0=plain box, 1=box (default), 2=pill (high corner radius), 3=speech bubble\n\n"
                "Returns {track, clip} indices of the created text clip."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track":    {"type": "integer", "description": "Track index to place the callout on"},
                    "start":    {"type": "number",  "description": "Start time in seconds"},
                    "end":      {"type": "number",  "description": "End time in seconds"},
                    "text":     {"type": "string",  "description": "Callout label text"},
                    "pos_x":    {"type": "number",  "description": "Canvas X position of the text box (0–1)"},
                    "pos_y":    {"type": "number",  "description": "Canvas Y position of the text box (0–1)"},
                    "arrow_x":  {"type": "number",  "description": "Canvas X of the arrow tip target (0–1). Enables arrow if provided."},
                    "arrow_y":  {"type": "number",  "description": "Canvas Y of the arrow tip target (0–1). Enables arrow if provided."},
                    "callout_style": {"type": "integer", "description": "Box style: 0=plain, 1=box, 2=pill, 3=speech bubble (default 1)"},
                    "font_size":     {"type": "number",  "description": "Font size as fraction of canvas height (default 0.04)"},
                    "bg_color":      {"type": "array",   "description": "[r,g,b,a] background color 0–1 (default semi-transparent dark)"},
                },
                "required": ["track", "start", "end", "text", "pos_x", "pos_y"],
            },
        ),
        Tool(
            name="add_body_fx_brick",
            description=(
                "Add a Body FX solid brick to a track. The brick applies a body/silhouette-based "
                "visual effect (e.g. neon outline, depth blur, glitch, retro TV) to the composited "
                "frame below it — it is NOT a glass effect and does NOT attach to a specific video clip.\n\n"
                "After adding the brick, call process_body_fx_masks to compute the body masks from "
                "the video clip(s) on tracks below. Processing is async; poll get_project or "
                "get_pipeline_status to monitor progress.\n\n"
                "fx_type options — pass the exact display name (space-separated Title Case, as listed):\n"
                "  Mask:         Remove Background\n"
                "  Retro / 80s:  Neon Outline | VHS Body | Scanlines | Chromatic Body | Retrowave | "
                "TV Static Bg | Arcade | Film Grain Body\n"
                "  Depth/Focus:  Depth Blur | Tilt-Shift Bg | Spotlight | Fog Bg | Bokeh Bg\n"
                "  Glitch:       Glitch Body | Glitch Bg | Split Reality | Data Corruption | "
                "Signal Loss Bg | Chromafall\n"
                "  Color:        Color Isolation | Invert Bg | Duotone Body | Duotone Bg | Thermal Body | X-Ray\n"
                "  Light/Glow:   Electric Aura | Rim Light | God Rays | Halo | Bloom Body | Bioluminescent\n"
                "  Abstract:     Hologram | Matrix Rain Bg | Body Mosaic | Wireframe Bg | Vaporwave\n"
                "  Party:        Strobe Bg | Disco Body | UV Paint | Fire Aura\n"
                "Names are case-sensitive AND space-separated — these are different effects from the "
                "snake_case shader names used by add_effect_brick (e.g. BodyFX 'Scanlines' uses body masks; "
                "add_effect_brick fx_type='scanlines' is a fullscreen shader). If unsure, use 'Neon Outline' "
                "or 'Depth Blur'.\n\n"
                "Returns {track, clip} of the created brick."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track":    {"type": "integer", "description": "Track index for the brick (should be above video tracks)"},
                    "start":    {"type": "number",  "description": "Start time in seconds"},
                    "end":      {"type": "number",  "description": "End time in seconds"},
                    "fx_type":  {"type": "string",  "description": "BodyFX effect name (see list above)"},
                    "amount":   {"type": "number",  "description": "Blend strength 0–1 (default 0.8)"},
                    "param_0":  {"type": "number",  "description": "Effect-specific param 0 (optional)"},
                    "param_1":  {"type": "number",  "description": "Effect-specific param 1 (optional)"},
                    "param_2":  {"type": "number",  "description": "Effect-specific param 2 (optional)"},
                    "param_3":  {"type": "number",  "description": "Effect-specific param 3 (optional)"},
                },
                "required": ["track", "start", "end"],
            },
        ),
        Tool(
            name="process_body_fx_masks",
            description=(
                "Start body mask processing for a Body FX brick. This analyses the video clip(s) "
                "on tracks below the brick and writes body-segmentation masks for the brick's time "
                "range. Async — returns immediately; poll get_project for body_fx_mask_status "
                "('Processing' → 'Ready'). Only needed once per brick; re-run if you extend the "
                "brick's right edge past the already-processed range.\n\n"
                "REQUIRES PROXY: the video clip's MJPEG proxy must be on disk before calling this. "
                "Newly-imported clips show instantly via direct decode but proxy generation runs "
                "in the background — this call fails with 'video proxy not ready yet' until the "
                "proxy finishes. Poll get_project for proxy_status on the source video clip and "
                "retry when it reports 'ready'."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track": {"type": "integer", "description": "Track index of the BodyFX brick"},
                    "clip":  {"type": "integer", "description": "Clip index of the BodyFX brick"},
                },
                "required": ["track", "clip"],
            },
        ),
        Tool(
            name="remove_background",
            description=(
                "Remove the background from a video clip using U2Net body segmentation. "
                "Adds a 'Remove Background' body_fx brick on the same track as the video clip "
                "(mask processing starts automatically). Blocks until masks are ready. No batch needed.\n\n"
                "REQUIRES PROXY: fails with 'video proxy not ready yet' when the clip's MJPEG "
                "proxy hasn't finished transcoding. Newly-imported clips preview instantly via "
                "direct libav decode but bg_remove specifically reads from the proxy. Poll "
                "get_project for the source clip's proxy_status and retry when it reports 'ready'."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track": {"type": "integer", "description": "Track index of the video clip"},
                    "clip":  {"type": "integer", "description": "Clip index of the video clip"},
                },
                "required": ["track", "clip"],
            },
        ),
        Tool(
            name="set_format",
            description=(
                "Set the project canvas format / aspect ratio. Three presets:\n"
                "  square     — 1:1   1080×1080  (Instagram square)\n"
                "  vertical   — 9:16  1080×1920  (TikTok / Reels / Shorts)\n"
                "  horizontal — 16:9  1920×1080  (YouTube / widescreen)\n"
                "No batch needed."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "format": {
                        "type": "string",
                        "enum": ["square", "vertical", "horizontal"],
                        "description": "square=1:1, vertical=9:16, horizontal=16:9",
                    },
                },
                "required": ["format"],
            },
        ),
        Tool(
            name="take_snapshot",
            description=(
                "Renders the canvas frame to PNG and returns the image inline so you can see it. "
                "Pass an optional time (seconds) to snap at that timestamp without seeking first. "
                "source='render' (default) re-renders the frame through the export pipeline — what an "
                "exported video will contain. source='canvas' captures the live preview rect from the "
                "app's framebuffer — the exact pixels the user is looking at (interactive scene "
                "compositor + text overlays). If the two disagree, the preview compositor and export "
                "renderer have diverged — capture both to diagnose which side is wrong. "
                "Read-only — no batch needed."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "time": {"type": "number", "description": "Timeline position to snap (default: current playhead)"},
                    "source": {
                        "type": "string",
                        "enum": ["render", "canvas"],
                        "description": "render = export pipeline (default); canvas = live preview ground truth",
                    },
                },
            },
        ),
        Tool(
            name="verify_clips",
            description=(
                "Seek to each time and snapshot to visually confirm clip content. "
                "Use after placing video clips from transcript timestamps — pass the midpoint of each clip. "
                "Returns [{time, path}] in order. Read-only — no batch needed."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "times": {
                        "type": "array",
                        "items": {"type": "number"},
                        "description": "Timeline positions (seconds) to snapshot, one per clip to verify",
                    },
                },
                "required": ["times"],
            },
        ),
        Tool(
            name="undo",
            description=(
                "Undo the last edit. Returns updated project state so you can verify the revert. "
                "Read-only — no batch needed."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="redo",
            description=(
                "Redo the last undone edit. Returns updated project state. "
                "Read-only — no batch needed."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="trim_all_to",
            description=(
                "Trim every clip on every track so nothing extends past 'time'. "
                "Clips starting at or after 'time' are deleted; clips straddling 'time' are clamped. "
                "One undo step. No batch needed."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "time": {"type": "number", "description": "Maximum end time in seconds"},
                },
                "required": ["time"],
            },
        ),
        Tool(
            name="delete_clips_after",
            description=(
                "Delete all clips on one track whose start >= time. "
                "One undo step. No batch needed."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track":      {"type": "integer"},
                    "track_name": {"type": "string", "description": "Track name (alternative to track index)"},
                    "time":       {"type": "number", "description": "Delete clips starting at or after this time (seconds)"},
                },
                "required": ["time"],
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
    """
    Requires beat analysis to already be done (call analyze_audio then poll
    get_audio_analysis until done, then call this). Does the cue matching instantly.
    """
    path          = arguments.get("path", "")
    description   = arguments.get("description", "")
    clip_duration = float(arguments.get("clip_duration", 0) or 0)

    if not path:
        raise ValueError("path is required")

    res = _call("get_audio_analysis", {})
    if res.get("status") != "done":
        raise ValueError(
            "Audio analysis not ready. Call analyze_audio(path) first, "
            "then poll get_audio_analysis every 2s until status='done', then call find_audio_cue."
        )

    bpm      = res["bpm"]
    duration = res["duration"]
    beats    = res["beats"]
    rms      = res["rms"]

    primary, reasoning, alternatives = _match_cue(
        description, rms, beats, duration, clip_duration
    )

    return {
        "source_timestamp": primary,
        "bpm":              bpm,
        "duration":         duration,
        "reasoning":        reasoning,
        "alternatives":     alternatives,
    }


# ── vision model download ─────────────────────────────────────────────────────

async def _download_vision_model() -> dict:
    """Start the download and return immediately. Claude polls get_vision_model_status."""
    st = _call("get_vision_model_status", {})
    if st.get("status") == "ready":
        return {"status": "ready", "message": "Vision model already installed."}
    _call("download_vision_model", {})
    return {"status": "started", "message": "Download started. Poll get_vision_model_status every 3s until status='ready'."}


# ── find_video_moment ─────────────────────────────────────────────────────────

def _tfidf_score(query: str, text: str) -> float:
    """Simple word-overlap score between query and text (0–1)."""
    q_words = set(query.lower().split())
    t_words = set(text.lower().split())
    if not q_words:
        return 0.0
    return len(q_words & t_words) / len(q_words)


async def _find_video_moment(arguments: dict) -> list[dict]:
    """
    Requires scene analysis to already be done (call describe_video then poll
    get_video_description until done, then call this). Scores frames instantly.
    """
    path  = arguments.get("path", "")
    query = arguments.get("query", "")
    if not path:
        raise ValueError("path is required")
    if not query:
        raise ValueError("query is required")

    st = _call("get_vision_model_status", {})
    if st.get("status") != "ready":
        raise ValueError(
            "Vision model not installed. Call download_vision_model, "
            "then poll get_vision_model_status every 3s until status='ready'."
        )

    res = _call("get_video_description", {})
    if res.get("status") != "done":
        raise ValueError(
            "Scene analysis not ready. Call describe_video(path) first, "
            "then poll get_video_description every 3s until status='done', then call find_video_moment."
        )

    frames = res.get("frames", [])
    if not frames:
        return [{"timestamp": 0.0, "description": "(no frames)", "confidence": 0.0}]

    scored = []
    for f in frames:
        desc  = f.get("description", "")
        score = _tfidf_score(query, desc)
        scored.append({"timestamp": f["timestamp"], "description": desc, "confidence": round(score, 3)})

    # Sort descending by confidence, return top 3
    scored.sort(key=lambda x: x["confidence"], reverse=True)
    return scored[:3]


# ── remove_silence ────────────────────────────────────────────────────────────

async def _remove_silence(arguments: dict) -> dict:
    track        = int(arguments["track"])
    clip_idx     = int(arguments["clip"])
    threshold    = float(arguments.get("threshold",    0.05))
    min_duration = float(arguments.get("min_duration", 0.5))
    padding      = float(arguments.get("padding",      0.15))

    clips = _call("get_clips", {"track": track})
    if clip_idx >= len(clips):
        raise ValueError(f"clip {clip_idx} does not exist on track {track}")

    c = clips[clip_idx]
    source   = c.get("source", "")
    tl_start = float(c["start"])
    in_point = float(c["in_point"])
    duration = float(c["duration"])

    if not source:
        raise ValueError("clip has no source file (is it a video/audio clip?)")

    # Analyze audio — blocks until done (streaming IPC)
    res = _call("analyze_audio", {"path": source})
    if res.get("status") != "done":
        raise ValueError("audio analysis failed for: " + source)

    rms = res["rms"]  # per-second list, 0-indexed by second

    # Find silence runs within the clip's source range
    src_start_s = int(in_point)
    src_end_s   = int(in_point + duration)

    silence_spans: list[tuple[int, int]] = []  # (src_start_sec, src_end_sec) exclusive
    run_start = None
    for s in range(src_start_s, min(src_end_s + 1, len(rms))):
        val = rms[s] if s < len(rms) else 0.0
        if val < threshold:
            if run_start is None:
                run_start = s
        else:
            if run_start is not None:
                if (s - run_start) >= min_duration:
                    silence_spans.append((run_start, s))
                run_start = None
    if run_start is not None and (src_end_s - run_start) >= min_duration:
        silence_spans.append((run_start, src_end_s))

    if not silence_spans:
        return {"removed": [], "count": 0}

    # Convert to timeline positions and apply padding
    cuts: list[tuple[float, float]] = []
    for (ss, se) in silence_spans:
        tl_s = tl_start + (ss - in_point) + padding
        tl_e = tl_start + (se - in_point) - padding
        if tl_e - tl_s > 0.05:  # at least 50ms after padding
            cuts.append((tl_s, tl_e))

    removed = []
    # Process back-to-front to keep earlier indices valid
    for (cut_s, cut_e) in reversed(cuts):
        # Re-fetch to get current clip indices
        tr_clips = _call("get_clips", {"track": track})

        # Find clip covering cut_e (split right boundary first)
        right_clip = next((i for i, cl in enumerate(tr_clips)
                           if cl["start"] <= cut_e < cl["end"]), None)
        if right_clip is not None and abs(tr_clips[right_clip]["end"] - cut_e) > 0.02:
            _call("split_clip", {"track": track, "clip": right_clip, "time": cut_e})
            tr_clips = _call("get_clips", {"track": track})

        # Find clip covering cut_s (split left boundary)
        left_clip = next((i for i, cl in enumerate(tr_clips)
                          if cl["start"] <= cut_s < cl["end"]), None)
        if left_clip is not None and abs(tr_clips[left_clip]["start"] - cut_s) > 0.02:
            _call("split_clip", {"track": track, "clip": left_clip, "time": cut_s})
            tr_clips = _call("get_clips", {"track": track})

        # Find and delete the silence segment (clip whose start >= cut_s and end <= cut_e)
        seg_idx = next((i for i, cl in enumerate(tr_clips)
                        if cl["start"] >= cut_s - 0.02 and cl["end"] <= cut_e + 0.02), None)
        if seg_idx is not None:
            _call("delete_clip", {"track": track, "clip": seg_idx})
            removed.append({"start": round(cut_s, 3), "end": round(cut_e, 3),
                            "duration": round(cut_e - cut_s, 3)})

    return {"removed": list(reversed(removed)), "count": len(removed)}


# ── cut_filler_words ──────────────────────────────────────────────────────────

_DEFAULT_FILLERS = [
    "um", "uh", "uh-huh", "like", "you know", "kind of", "sort of",
    "basically", "literally", "actually", "right", "okay",
]


async def _cut_filler_words(arguments: dict) -> dict:
    track    = int(arguments["track"])
    clip_idx = int(arguments["clip"])
    fillers  = [w.lower() for w in arguments.get("words", _DEFAULT_FILLERS)]
    padding  = float(arguments.get("padding", 0.04))
    dry_run  = bool(arguments.get("dry_run", False))

    tr_res = _call("get_transcript", {})
    if tr_res.get("status") != "ready":
        raise ValueError("no transcript available — run trigger_pipeline first and wait for it to complete")

    clips = _call("get_clips", {"track": track})
    if clip_idx >= len(clips):
        raise ValueError(f"clip {clip_idx} does not exist on track {track}")

    c        = clips[clip_idx]
    tl_start = float(c["start"])
    tl_end   = float(c["end"])
    in_point = float(c["in_point"])
    duration = float(c["duration"])

    words = tr_res["words"]

    # Build multi-word filler list (longest first so greedy matching works)
    multi_fillers = sorted([f for f in fillers if " " in f], key=len, reverse=True)
    single_fillers = [f for f in fillers if " " not in f]

    # Filter words to those within this clip's source range
    src_end = in_point + duration
    clip_words = [w for w in words if in_point <= float(w["start"]) < src_end]

    # Greedy match: walk through clip_words looking for filler runs
    matches: list[dict] = []
    i = 0
    while i < len(clip_words):
        matched = False
        # Try multi-word fillers first
        for mf in multi_fillers:
            mf_parts = mf.split()
            n = len(mf_parts)
            if i + n <= len(clip_words):
                window = [clip_words[i + j]["word"].lower().strip(".,!?") for j in range(n)]
                if window == mf_parts:
                    w_start = float(clip_words[i]["start"])
                    w_end   = float(clip_words[i + n - 1]["end"])
                    tl_s    = tl_start + (w_start - in_point)
                    tl_e    = tl_start + (w_end   - in_point)
                    matches.append({"word": mf, "source_start": w_start, "source_end": w_end,
                                    "timeline_start": round(tl_s, 3), "timeline_end": round(tl_e, 3)})
                    i += n
                    matched = True
                    break
        if not matched:
            w = clip_words[i]["word"].lower().strip(".,!?")
            if w in single_fillers:
                w_start = float(clip_words[i]["start"])
                w_end   = float(clip_words[i]["end"])
                tl_s    = tl_start + (w_start - in_point)
                tl_e    = tl_start + (w_end   - in_point)
                matches.append({"word": clip_words[i]["word"], "source_start": w_start, "source_end": w_end,
                                "timeline_start": round(tl_s, 3), "timeline_end": round(tl_e, 3)})
            i += 1

    if dry_run:
        return {"matches": matches}

    if not matches:
        return {"removed": [], "count": 0}

    removed = []
    for m in reversed(matches):
        cut_s = m["timeline_start"] - padding
        cut_e = m["timeline_end"]   + padding

        tr_clips = _call("get_clips", {"track": track})

        right_clip = next((i for i, cl in enumerate(tr_clips)
                           if cl["start"] <= cut_e < cl["end"]), None)
        if right_clip is not None and abs(tr_clips[right_clip]["end"] - cut_e) > 0.02:
            _call("split_clip", {"track": track, "clip": right_clip, "time": cut_e})
            tr_clips = _call("get_clips", {"track": track})

        left_clip = next((i for i, cl in enumerate(tr_clips)
                          if cl["start"] <= cut_s < cl["end"]), None)
        if left_clip is not None and abs(tr_clips[left_clip]["start"] - cut_s) > 0.02:
            _call("split_clip", {"track": track, "clip": left_clip, "time": cut_s})
            tr_clips = _call("get_clips", {"track": track})

        seg_idx = next((i for i, cl in enumerate(tr_clips)
                        if cl["start"] >= cut_s - 0.02 and cl["end"] <= cut_e + 0.02), None)
        if seg_idx is not None:
            _call("delete_clip", {"track": track, "clip": seg_idx})
            removed.append({"word": m["word"], "start": round(cut_s, 3), "end": round(cut_e, 3)})

    return {"removed": list(reversed(removed)), "count": len(removed)}


# ── apply_multicam_cuts ───────────────────────────────────────────────────────

async def _apply_multicam_cuts(arguments: dict) -> dict:
    cuts          = arguments["cuts"]           # [{time, track}]
    camera_tracks = [int(t) for t in arguments["camera_tracks"]]

    if not cuts:
        raise ValueError("cuts list is empty")
    cuts = sorted(cuts, key=lambda c: float(c["time"]))

    # Determine end time from camera track clips
    end_time = 0.0
    for ct in camera_tracks:
        for cl in _call("get_clips", {"track": ct}):
            end_time = max(end_time, float(cl["end"]))

    if end_time == 0.0:
        raise ValueError("no clips found on camera_tracks")

    # Build windows: [{start, end, track}]
    windows = []
    for i, cut in enumerate(cuts):
        t_start = float(cut["time"])
        t_end   = float(cuts[i + 1]["time"]) if i + 1 < len(cuts) else end_time
        windows.append({"start": t_start, "end": t_end, "track": int(cut["track"])})

    # All split times (exclude 0 and end_time)
    split_times = sorted({float(c["time"]) for c in cuts if float(c["time"]) > 0.001})
    split_times = [t for t in split_times if t < end_time - 0.02]

    # Phase 1: split all camera clips at every cut time (forward order)
    for t in split_times:
        for ct in camera_tracks:
            tr_clips = _call("get_clips", {"track": ct})
            clip_at  = next((i for i, cl in enumerate(tr_clips)
                             if float(cl["start"]) < t < float(cl["end"])), None)
            if clip_at is not None:
                _call("split_clip", {"track": ct, "clip": clip_at, "time": t})

    # Phase 2: delete unwanted segments back-to-front
    deleted = 0
    for w in reversed(windows):
        w_start  = w["start"]
        w_end    = w["end"]
        keep_trk = w["track"]

        for ct in camera_tracks:
            if ct == keep_trk:
                continue
            tr_clips = _call("get_clips", {"track": ct})

            # Find segment(s) fully within [w_start, w_end]
            segs = [i for i, cl in enumerate(tr_clips)
                    if float(cl["start"]) >= w_start - 0.02
                    and float(cl["end"]) <= w_end + 0.02]
            for seg_idx in reversed(segs):
                _call("delete_clip", {"track": ct, "clip": seg_idx})
                deleted += 1

    return {"windows": windows, "deleted_count": deleted}


# ── add_chapter_marker / remove_chapter_marker / generate_chapters ───────────

async def _add_chapter_marker(arguments: dict) -> dict:
    return _call("add_marker", {
        "time":  float(arguments["time"]),
        "label": arguments.get("label", ""),
        "color": arguments.get("color", "#4A90E2"),
    })


async def _remove_chapter_marker(arguments: dict) -> dict:
    return _call("remove_marker", {"index": int(arguments["index"])})


async def _generate_chapters(arguments: dict) -> dict:
    tr = _call("get_transcript", {})
    if tr.get("status") != "ready":
        raise ValueError(f"transcript not ready (status={tr.get('status')})")

    words = tr.get("words", [])
    if not words:
        raise ValueError("transcript has no words")

    # Remove existing markers
    existing = _call("get_markers", {}).get("markers", [])
    for m in reversed(existing):
        _call("remove_marker", {"index": m["index"]})

    # Compute inter-word gaps
    gaps = []
    for i in range(len(words) - 1):
        gap = float(words[i + 1]["start"]) - float(words[i]["end"])
        gaps.append((gap, i))  # (gap_duration, index_of_word_before_gap)

    num_chapters = arguments.get("num_chapters")
    min_pause    = float(arguments.get("min_pause_seconds", 3.0))

    if num_chapters is not None:
        num_chapters = int(num_chapters)
        # Pick the N-1 longest gaps as boundaries
        sorted_gaps = sorted(gaps, key=lambda g: g[0], reverse=True)
        boundary_indices = sorted({idx for _, idx in sorted_gaps[:max(0, num_chapters - 1)]})
    else:
        boundary_indices = sorted(idx for gap, idx in gaps if gap >= min_pause)

    # Build chapter list: first chapter at t=0
    def first_words(start_idx: int, n: int = 4) -> str:
        parts = []
        for w in words[start_idx:start_idx + n]:
            parts.append(str(w.get("word", w.get("text", ""))).strip(" .,!?"))
        return " ".join(p for p in parts if p)[:40]

    chapters = [{"time": 0.0, "label": first_words(0)}]
    for idx in boundary_indices:
        # Chapter starts at the word *after* the gap
        next_word_idx = idx + 1
        if next_word_idx >= len(words):
            continue
        t = float(words[next_word_idx]["start"])
        label = first_words(next_word_idx)
        chapters.append({"time": round(t, 3), "label": label})

    # Insert markers via IPC
    for ch in chapters:
        _call("add_marker", {"time": ch["time"], "label": ch["label"]})

    return {"chapters": chapters}


# ── add_callout ───────────────────────────────────────────────────────────────

async def _add_callout(arguments: dict) -> dict:
    track      = int(arguments["track"])
    start      = float(arguments["start"])
    end        = float(arguments["end"])
    text       = str(arguments["text"])
    pos_x      = float(arguments["pos_x"])
    pos_y      = float(arguments["pos_y"])
    arrow_x    = arguments.get("arrow_x")
    arrow_y    = arguments.get("arrow_y")
    style      = int(arguments.get("callout_style", 1))
    font_size  = float(arguments.get("font_size", 0.04))
    bg_color   = arguments.get("bg_color", [0.05, 0.05, 0.05, 0.82])

    # Create the text clip
    clip_idx = _call("add_clip", {
        "track": track,
        "type":  "text",
        "start": start,
        "end":   end,
        "text":  text,
    })["clip"]

    # Set positioning + callout fields
    props = [
        {"track": track, "clip": clip_idx, "prop": "sub_pos",     "value": 3},
        {"track": track, "clip": clip_idx, "prop": "sub_pos_x",   "value": pos_x},
        {"track": track, "clip": clip_idx, "prop": "sub_pos_y",   "value": pos_y},
        {"track": track, "clip": clip_idx, "prop": "font_size",   "value": font_size},
        {"track": track, "clip": clip_idx, "prop": "callout_style","value": style},
    ]
    if arrow_x is not None and arrow_y is not None:
        props += [
            {"track": track, "clip": clip_idx, "prop": "callout_arrow", "value": True},
            {"track": track, "clip": clip_idx, "prop": "arrow_tx",      "value": float(arrow_x)},
            {"track": track, "clip": clip_idx, "prop": "arrow_ty",      "value": float(arrow_y)},
        ]
    _call("set_clip_props", {"ops": props})

    # Set text style: background enabled with given color, corner radius for pill/bubble
    corner = 8.0 if style == 1 else (999.0 if style == 2 else 6.0)
    _call("set_text_style", {
        "track": track,
        "clip":  clip_idx,
        "bg_enabled": True,
        "bg_col": bg_color,
        "bg_pad_x": 10.0,
        "bg_pad_y": 6.0,
        "bg_corner": corner,
    })

    return {"track": track, "clip": clip_idx}


# ── add_body_fx_brick / process_body_fx_masks ─────────────────────────────────

async def _add_body_fx_brick(arguments: dict) -> dict:
    track   = int(arguments["track"])
    start   = float(arguments["start"])
    end     = float(arguments["end"])
    fx_type = str(arguments.get("fx_type", "NeonOutline"))
    amount  = float(arguments.get("amount", 0.8))

    with _batch("Add BodyFX brick"):
        clip_idx = _call("add_clip", {
            "track": track,
            "type":  "body_fx",
            "start": start,
            "end":   end,
        })["clip"]

        props = [
            {"track": track, "clip": clip_idx, "prop": "body_fx_type",   "value": fx_type},
            {"track": track, "clip": clip_idx, "prop": "body_fx_amount",  "value": amount},
        ]
        for i, key in enumerate(["param_0", "param_1", "param_2", "param_3"]):
            if key in arguments:
                props.append({"track": track, "clip": clip_idx,
                              "prop": f"body_fx_{key}", "value": float(arguments[key])})
        _call("set_clip_props", {"ops": props})

    return {"track": track, "clip": clip_idx}


# ── remove_background ────────────────────────────────────────────────────────

async def _remove_background(arguments: dict) -> dict:
    track = int(arguments["track"])
    clip  = int(arguments["clip"])

    clips_list = _call("get_clips", {"track": track})
    if clip >= len(clips_list):
        raise ValueError(f"clip {clip} not found on track {track}")
    clip_info = clips_list[clip]
    start = float(clip_info["start"])
    end   = float(clip_info["end"])

    with _batch("Add RemoveBackground brick"):
        result = _call("add_clip", {
            "track": track,
            "type":  "body_fx",
            "start": start,
            "end":   end,
        })
    return {"brick_clip": result["clip"], "track": track, "start": start, "end": end}


# ── add_clip (with auto-extract guard) ────────────────────────────────────────

async def _add_clip(arguments: dict) -> dict:
    """
    Intercept video add_clip calls: if the source file is more than 2× longer
    than the segment we actually need, extract that segment first so the proxy
    generator only has to process a short file instead of the full source.
    """
    # Overlap guardrail: reject if any existing clip on the target track
    # occupies any part of [start, end], regardless of clip type.
    new_start = float(arguments.get("start", 0))
    new_end   = float(arguments.get("end",   0))
    track_args: dict = {}
    if "track" in arguments:
        track_args["track"] = arguments["track"]
    elif "track_name" in arguments:
        track_args["track_name"] = arguments["track_name"]
    if track_args:
        try:
            fps = float(_call("get_project", {}).get("fps", 30))
            frame_dur = 1.0 / fps
            existing_clips = _call("get_clips", track_args)
            # Match C++ snap_to_frame (roundf) and snap_end_to_frame (ceilf - ε).
            snap_start = round(new_start * fps) / fps
            snap_end   = math.ceil(new_end * fps - 1e-4) / fps
            # Magnetic abutting: if snap_start lands within one frame before an
            # existing clip's end (because ceil pushed that end past our intended
            # boundary), snap start up to that end so clips abut cleanly.
            for cl in existing_clips:
                cl_end = float(cl.get("end", 0))
                if 0 < cl_end - snap_start <= frame_dur + 1e-6:
                    snap_start = cl_end
                    new_start  = cl_end
                    arguments  = {**arguments, "start": new_start}
                    break
            for cl in existing_clips:
                cl_start = float(cl.get("start", 0))
                cl_end   = float(cl.get("end",   0))
                if cl_start < snap_end and snap_start < cl_end:
                    raise ValueError(
                        f"Clip overlap on track: requested [{new_start:.3f}s – {new_end:.3f}s] "
                        f"collides with existing clip [{cl_start:.3f}s – {cl_end:.3f}s]. "
                        f"Place clips on separate tracks or adjust the time range."
                    )
        except ValueError:
            raise
        except Exception:
            pass  # if we can't fetch clips, allow the call through

    clip_type = arguments.get("type", "")
    text = arguments.get("text", "")

    if clip_type == "video" and text:
        in_point = float(arguments.get("in_point", 0.0))
        clip_duration = float(arguments["end"]) - float(arguments["start"])
        needed_end = in_point + clip_duration + 2.0  # 2 s safety buffer

        try:
            info = _call("get_media_info", {"path": text})
            source_dur = float(info.get("duration", 0.0))
        except Exception:
            source_dur = 0.0

        already_extracted = bool(re.search(r'_\d+_\d+\.(webm|flac|mp3|wav|ogg|aac)$', text))
        if not already_extracted and source_dur > needed_end * 2:
            p = Path(text)
            s_int = int(in_point)
            e_int = int(needed_end) + 1
            cache_dir = p.parent / p.stem
            # Audio-only files must use a compatible container; inherit source ext
            has_video = bool(_call("get_media_info", {"path": text}).get("has_video", True))
            seg_ext = p.suffix  # inherit source container so stream-copy stays codec-compatible
            dst = str(cache_dir / f"{p.stem}_{s_int}_{e_int}{seg_ext}")
            if not Path(dst).exists():
                cache_dir.mkdir(parents=True, exist_ok=True)
                _call("extract_clip_segment", {
                    "src":   text,
                    "dst":   dst,
                    "start": in_point,
                    "end":   needed_end,
                })
            arguments = {**arguments, "text": dst, "in_point": 0.0}

    return _call("add_clip", arguments)


# ── find_and_add_clip ─────────────────────────────────────────────────────────

def _words_cache_paths(audio_path: str) -> tuple[Path, Path]:
    """Return (canonical, search) words.json paths for a given audio file.

    Canonical (_words.json) is owned by the full pipeline (do_transcribe):
    DTW + forced alignment + segment splits. Authoritative when present.
    Search (_words_search.json) is the windowed find_and_add_clip output:
    best-effort, used as fallback when canonical hasn't been produced yet.
    """
    p = Path(audio_path)
    return (
        p.parent / p.stem / f"{p.stem}_words.json",
        p.parent / p.stem / f"{p.stem}_words_search.json",
    )


def _best_words_cache(audio_path: str) -> Path | None:
    """Pick the best available words.json for an audio file (canonical > search)."""
    canon, search = _words_cache_paths(audio_path)
    if canon.exists():  return canon
    if search.exists(): return search
    return None


def _search_transcript_in_words(words: list, query: str) -> tuple[float, float, str, float, bool]:
    """Search a words list for a query. Returns (start, end, excerpt, score, truncated).
    end is the end of the last matched query word, not the end of the search window.
    truncated=True means the match is partial AND lands near the tail of the word list,
    which indicates the source chunk was cut off before capturing all query words."""
    query_words_list = query.lower().split()
    query_words_set  = set(query_words_list)
    best_score, best_start, best_end, best_text = -1.0, 0.0, 0.0, ""
    window_size = max(len(query_words_list) * 2, 20)
    for i in range(len(words)):
        window = words[i:i + window_size]
        if not window:
            break
        text  = " ".join(w["word"] for w in window).lower()
        score = len(query_words_set & set(text.split())) / len(query_words_set) if query_words_set else 0.0
        if score > best_score:
            best_score = score
            # Narrow to the tightest subspan that achieves the window's score.
            # Previously this required score >= 1.0, which left s_idx,e_idx at the
            # full window edges on partial matches — callers then mistook the
            # window span (often ~20 words / ~15-20s wide) for the located phrase
            # and cut/extracted at the wrong timestamp. Now we pick the highest
            # subspan-score and break ties by tighter span.
            s_idx, e_idx     = 0, len(window) - 1
            best_span_score  = -1.0
            for s in range(len(window)):
                for e in range(s, len(window)):
                    span_text  = " ".join(w["word"] for w in window[s:e + 1]).lower()
                    span_score = (
                        len(query_words_set & set(span_text.split())) / len(query_words_set)
                        if query_words_set else 0.0
                    )
                    tighter = (e - s) < (e_idx - s_idx)
                    if span_score > best_span_score or (span_score == best_span_score and tighter):
                        best_span_score, s_idx, e_idx = span_score, s, e
                    if span_score >= 1.0:
                        # full match — can't do better; stop scanning end positions
                        break
            best_start = float(window[s_idx]["start"])
            best_end   = float(window[e_idx]["end"])
            best_text  = " ".join(w["word"] for w in window[s_idx:e_idx + 1])
    # Flag chunk-boundary truncation: partial match that runs right up to the end of available words
    truncated = (
        0.0 < best_score < 1.0
        and bool(words)
        and float(words[-1]["end"]) - best_end < 4.0
    )
    return best_start, best_end, best_text[:200], best_score, truncated


async def _find_and_add_clip(arguments: dict) -> dict:
    """
    Step 1: check cache or start transcription search. Returns immediately.
    If cached=true, returns found result so Claude can call extract_clip_segment + add_clip.
    If cached=false, starts background search; Claude polls get_search_status then continues.
    """
    path    = arguments.get("path", "")
    query   = arguments.get("query", "")
    track   = int(arguments.get("track", 0))
    padding = float(arguments.get("padding", 5.0))

    if not path:
        raise ValueError("path is required")
    if not query:
        raise ValueError("query is required")

    p = Path(path)

    # Check disk cache first.
    # PROVENANCE: prefer the canonical full-pipeline cache (_words.json) over
    # the windowed-search cache (_words_search.json).  The full pipeline runs
    # DTW + forced alignment and is authoritative.  Search cache is best-effort
    # — used only when the canonical cache hasn't been produced yet.
    canonical_words_path, search_words_path = _words_cache_paths(path)
    cached_words_path = _best_words_cache(path) or search_words_path
    words = None
    if cached_words_path.exists():
        with open(cached_words_path) as f:
            words = json.load(f)

    if not words:
        proj = _call("get_project", {})
        tr   = _call("get_transcript", {})
        if tr.get("status") == "ready" and proj.get("audio_path", "") == path:
            words = tr["words"]

    if words:
        start, end, excerpt, score, truncated = _search_transcript_in_words(words, query)

        # Cached words may only cover an earlier scan window — e.g. a previous
        # find_and_add_clip transcribed words 0–60s of a 200s file, and now the
        # caller asks for a phrase that's actually at 150s. Detect "cache doesn't
        # cover the file" and trigger a windowed re-scan, regardless of where the
        # weak match landed inside the cache.
        try:
            file_dur = float(_call("get_media_info", {"path": path}).get("duration", 0.0))
        except Exception:
            file_dur = 0.0
        cache_covers_file = bool(words) and (
            file_dur <= 0.0 or float(words[-1]["end"]) >= file_dur - 5.0
        )
        cache_incomplete_match = (not cache_covers_file) and score < 0.85

        if score < 0.3 and cache_covers_file:
            raise ValueError(f"could not find '{query}' in transcript (best score: {score:.2f})")

        # Sanity guard: if the returned span is implausibly wide for the query length,
        # the actual phrase location is ambiguous. Cap at ~1.5s per query word with a
        # 6s floor. Wider than that almost always means a partial/noisy match — the
        # caller should treat start/end as approximate and not extract at exact bounds.
        n_qwords      = max(len(query.split()), 1)
        max_plausible = max(6.0, 1.5 * n_qwords)
        partial_match = score < 1.0 or (end - start) > max_plausible

        # Partial match at a chunk boundary — the cached word list was cut off before capturing
        # the trailing query word(s) — OR the cache covers only an earlier slice of the file and
        # the query lives in the un-transcribed region. Re-run the windowed search so the scanner
        # extends the cache and returns true word-boundary timestamps.
        if truncated or cache_incomplete_match:
            reason = "near cache tail" if truncated else "cache doesn't cover full file"
            print(f"[find_and_add_clip] partial match ({score:.2f}) — re-scanning ({reason})", flush=True)
            _call("search_transcript", {
                "path":        path,
                "query_words": query.lower().split(),
                "buffer_sec":  60.0,
            })
            last_msg = ""
            st: dict = {}
            for _ in range(120):
                await asyncio.sleep(2)
                st = _call("get_search_status", {})
                msg = st.get("message", "")
                if msg and msg != last_msg:
                    print(f"[find_and_add_clip lookahead] {msg}", flush=True)
                last_msg = await _send_search_progress(st, last_msg)
                if not st.get("running", False):
                    break
            if st.get("found"):
                start   = float(st.get("start", start))
                end     = float(st.get("end",   end))
                excerpt = st.get("excerpt", excerpt)
                # Reload the cache if the search extended it, then re-score
                # against the refreshed words so partial_match reflects the
                # extended search result, not the stale pre-rescan score.
                # The re-scan always writes to _words_search.json (the
                # canonical _words.json is owned by the full pipeline), so
                # read from the search path here regardless of which cache
                # we originally loaded.
                reload_path = search_words_path
                if reload_path.exists():
                    with open(reload_path) as f:
                        refreshed = json.load(f)
                    if refreshed and words and float(refreshed[-1]["end"]) > float(words[-1]["end"]):
                        words = refreshed
                        rs, re_, rex, rscore, _ = _search_transcript_in_words(words, query)
                        if rscore >= score:
                            start, end, excerpt, score = rs, re_, rex, rscore
                            partial_match = score < 1.0 or (end - start) > max_plausible

        # Check if any previously extracted segment covers the needed range
        seg_start = max(0.0, start - padding)
        seg_end   = end + padding
        cache_dir = p.parent / p.stem
        existing_dst = None
        seg_ext_escaped = re.escape(p.suffix.lstrip("."))
        seg_pat = re.compile(rf"^{re.escape(p.stem)}_(\d+)_(\d+)\.{seg_ext_escaped}$")
        if cache_dir.is_dir():
            for f in cache_dir.iterdir():
                m = seg_pat.match(f.name)
                if m and int(m.group(1)) <= seg_start and int(m.group(2)) >= seg_end:
                    existing_dst = str(f)
                    break
        if existing_dst:
            dur_result = _call("get_media_info", {"path": existing_dst})
            duration   = float(dur_result.get("duration", seg_end - seg_start))
            # in_point into the found file (content starts at seg_start - file_seg_start)
            file_seg_start = int(seg_pat.match(Path(existing_dst).name).group(1))
            return {
                "status":        "found",
                "cached":        True,
                "extracted":     True,
                "path":          path,
                "track":         track,
                "padding":       padding,
                "start":         round(start, 3),
                "end":           round(end, 3),
                "excerpt":       excerpt,
                "dst":           existing_dst,
                "clip_duration": round(duration, 3),
                "in_point":      round(seg_start - file_seg_start, 3),
                "score":         round(score, 2),
                "partial_match": partial_match,
            }

        # Auto-extract the segment so the caller never needs to call extract_clip_segment manually
        seg_start = max(0.0, start - padding)
        seg_end   = end + padding
        s_int     = int(seg_start)
        e_int     = int(seg_end) + 1
        cache_dir = p.parent / p.stem
        seg_ext   = p.suffix
        dst = str(cache_dir / f"{p.stem}_{s_int}_{e_int}{seg_ext}")
        expected_dur = seg_end - seg_start
        dst_path = Path(dst)
        need_extract = not dst_path.exists()
        if not need_extract:
            # Validate the cached file has roughly the expected duration; re-extract if stale/corrupt
            existing_dur = float(_call("get_media_info", {"path": dst}).get("duration", 0))
            if existing_dur < 1.0 or existing_dur > expected_dur * 2:
                dst_path.unlink(missing_ok=True)
                need_extract = True
        if need_extract:
            cache_dir.mkdir(parents=True, exist_ok=True)
            _call("extract_clip_segment", {"src": path, "dst": dst, "start": seg_start, "end": seg_end})
        dur_result = _call("get_media_info", {"path": dst})
        duration   = float(dur_result.get("duration", expected_dur))
        return {
            "status":        "found",
            "cached":        True,
            "extracted":     True,
            "path":          path,
            "track":         track,
            "padding":       padding,
            "start":         round(start, 3),
            "end":           round(end, 3),
            "excerpt":       excerpt,
            "dst":           dst,
            "clip_duration": round(duration, 3),
            "in_point":      round(start - seg_start, 3),
            "score":         round(score, 2),
            "partial_match": partial_match,
        }

    # No transcript — start transcription search and block until done
    _call("search_transcript", {
        "path":        path,
        "query_words": query.lower().split(),
        "buffer_sec":  padding + 30.0,
    })
    last_msg = ""
    while True:
        await asyncio.sleep(3)
        status = _call("get_search_status", {})
        msg = status.get("message", "")
        if msg and msg != last_msg:
            print(f"[find_and_add_clip] {msg}", flush=True)
        last_msg = await _send_search_progress(status, last_msg)
        if not status.get("running", False):
            break

    if not status.get("found", False):
        # Surface top-N fuzzy candidates instead of a blank failure.  The C++
        # search returns up to 3 disjoint windows with coverage >= 0.5 so the
        # agent / human can disambiguate ("did you mean phrase X at 0:42?")
        # without re-running another search.
        cands = status.get("candidates", []) or []
        if cands:
            tip = ", ".join(
                f"{round(c.get('start',0.0),2)}s '{(c.get('excerpt','') or '').strip()[:60]}' (score={c.get('score',0):.2f})"
                for c in cands[:3]
            )
            raise ValueError(
                f"could not find '{query}' exactly. Closest fuzzy hits: {tip}"
            )
        raise ValueError(f"could not find '{query}' in transcript: {status.get('error', 'not found')}")

    start   = float(status["start"])
    end     = float(status["end"])
    excerpt = status.get("excerpt", "")
    return {
        "status":  "found",
        "cached":  False,
        "path":    path,
        "track":   track,
        "padding": padding,
        "start":   round(start, 3),
        "end":     round(end, 3),
        "excerpt": excerpt,
    }


# ── search_transcript ─────────────────────────────────────────────────────────

def _all_transcript_matches(words: list, phrase: str, threshold: float = 0.6) -> list[dict]:
    """Return all non-overlapping windows that match phrase above threshold, sorted by time."""
    query_words = phrase.lower().split()
    if not query_words:
        return []
    qset = set(query_words)
    window_size = max(len(query_words) * 2, 8)
    matches: list[dict] = []
    i = 0
    while i < len(words):
        window = words[i:i + window_size]
        if not window:
            break
        text  = " ".join(w["word"] for w in window).lower()
        score = len(qset & set(text.split())) / len(qset)
        if score >= threshold:
            # Narrow the window to just the matched phrase span
            wtext = text.split()
            # Find best contiguous subspan covering query words
            best_span = (0, len(window) - 1)
            for s in range(len(window)):
                for e in range(s, len(window)):
                    span_text = " ".join(w["word"] for w in window[s:e+1]).lower()
                    span_score = len(qset & set(span_text.split())) / len(qset)
                    if span_score >= 1.0:
                        best_span = (s, e)
                        break
                else:
                    continue
                break
            s_idx, e_idx = best_span
            matches.append({
                "start":   round(float(window[s_idx]["start"]), 3),
                "end":     round(float(window[e_idx]["end"]), 3),
                "excerpt": " ".join(w["word"] for w in window[s_idx:e_idx+1]),
                "score":   round(score, 2),
            })
            i += e_idx + 1  # skip past this match
        else:
            i += 1
    # Deduplicate overlapping entries (keep highest score)
    deduped: list[dict] = []
    for m in sorted(matches, key=lambda x: x["start"]):
        if deduped and m["start"] < deduped[-1]["end"]:
            if m["score"] > deduped[-1]["score"]:
                deduped[-1] = m
        else:
            deduped.append(m)
    return deduped


def _search_transcript(arguments: dict) -> dict:
    phrase = arguments.get("phrase", "")
    path   = arguments.get("path", "") or _call("get_project", {}).get("audio_path", "")
    if not path:
        raise ValueError("No path provided and no audio_path on project")
    p = Path(_resolve_path(path))
    words_path = _best_words_cache(str(p))
    if words_path is None:
        raise ValueError(f"No transcript for {p.name} — run trigger_pipeline first")
    with open(words_path) as f:
        all_words = json.load(f)
    matches = _all_transcript_matches(all_words, phrase)
    return {"phrase": phrase, "count": len(matches), "matches": matches}


# ── cut_at_phrase ──────────────────────────────────────────────────────────────

def _cut_at_phrase(arguments: dict) -> dict:
    track  = int(arguments["track"])
    clip   = int(arguments["clip"])
    phrase = arguments["phrase"]
    side   = arguments.get("side", "after")
    occurrence = arguments.get("occurrence", None)

    proj = _call("get_project", {})
    audio_path = proj.get("audio_path", "")
    if not audio_path:
        raise ValueError("No audio_path on project — add a video clip first")
    if not proj.get("transcript_ready"):
        raise ValueError("Transcript not ready — run trigger_pipeline first")

    p = Path(_resolve_path(audio_path))
    words_path = _best_words_cache(str(p))
    if words_path is None:
        raise ValueError(f"No transcript for {p.name}")
    with open(words_path) as f:
        all_words = json.load(f)

    matches = _all_transcript_matches(all_words, phrase)
    if not matches:
        raise ValueError(f"Phrase not found in transcript: {phrase!r}")

    if len(matches) > 1 and occurrence is None:
        return {
            "ambiguous": True,
            "message": f"Found {len(matches)} occurrences — re-call with occurrence=N (0-indexed)",
            "matches": matches,
        }

    idx = int(occurrence) if occurrence is not None else 0
    if idx >= len(matches):
        raise ValueError(f"occurrence={idx} out of range (found {len(matches)} matches)")

    match = matches[idx]

    # Get clip in_point to convert source → timeline time
    in_point = 0.0
    clip_start = 0.0
    track_clips = _call("get_clips", {"track": track})
    if clip < len(track_clips):
        cl = track_clips[clip]
        in_point  = float(cl.get("in_point", 0))
        clip_start = float(cl.get("start", 0))

    if side == "after":
        timeline_t = clip_start + (match["end"] - in_point)
        with _batch("cut_at_phrase") as result:
            _call("trim_clip", {"track": track, "clip": clip, "end": round(timeline_t, 3)})
        return {"trimmed_end": round(timeline_t, 3), "excerpt": match["excerpt"], **result}
    else:
        timeline_t = clip_start + (match["start"] - in_point)
        with _batch("cut_at_phrase") as result:
            _call("trim_clip", {"track": track, "clip": clip, "start": round(timeline_t, 3)})
        return {"trimmed_start": round(timeline_t, 3), "excerpt": match["excerpt"], **result}


@server.call_tool()
async def call_tool(name: str, arguments: dict) -> list[TextContent]:
    if name == "trigger_pipeline":
        proj = _call("get_project", {})
        if not proj.get("audio_path") and not arguments.get("path"):
            raise ValueError(
                "trigger_pipeline called with no audio on the timeline.\n"
                "Add the audio file first:\n"
                "  1. begin_batch('Add audio') → add_track('Audio') → end_batch()\n"
                "  2. begin_batch('Add audio clip') → add_clip(type='audio', text=<path>, start=0, end=<duration>) → end_batch()\n"
                "Then call trigger_pipeline."
            )
        target_path = arguments.get("path") or proj.get("audio_path")
        if target_path:
            p = Path(target_path)
            cached_words = p.parent / p.stem / f"{p.stem}_words.json"
            if cached_words.exists():
                raise ValueError(
                    f"Transcript already cached for {p.name} at {cached_words}.\n"
                    "Call get_transcript() to read it — re-transcribing is wasted work.\n"
                    "If the cached transcript is wrong, you have two cleaner options than re-transcribing:\n"
                    "  - shift_transcript(source_path=..., offset=..., start=..., end=...) to repurpose another file's transcript\n"
                    "  - set_transcript(words=[...]) to install a hand-edited word list\n"
                    f"Or to force a fresh re-transcription, delete {cached_words} first."
                )
        duration = float(proj.get("duration") or 0)
        if duration > 300 and not arguments.get("path"):
            raise ValueError(
                f"trigger_pipeline on a {duration:.0f}s clip is too slow. "
                "If you only need a section (e.g. 'beginning to phrase X'), use this pattern instead:\n"
                "  1. find_and_add_clip(phrase='X', path=<audio_path>) — windowed search, finds the timestamp fast\n"
                "  2. extract_clip_segment(src=<audio_path>, dst=..., start=0, end=<found_end + 0.3>)\n"
                "  3. Replace the audio clip on the timeline with the extracted segment\n"
                "  4. Then call trigger_pipeline on the short clip\n"
                "Only call trigger_pipeline directly if you genuinely need a full transcript of everything."
            )
        if not arguments.get("path") and proj.get("audio_path") and 30 < duration <= 300:
            src_info = _call("get_media_info", {"path": proj["audio_path"]})
            src_duration = float(src_info.get("duration") or 0)
            if src_duration > 0 and duration / src_duration >= 0.9:
                raise ValueError(
                    f"trigger_pipeline called with {duration:.0f}s on the timeline "
                    f"({duration / src_duration:.0%} of the source file, which is {src_duration:.0f}s). "
                    "The audio clip covers nearly the full source, which means find_and_add_clip "
                    "was probably skipped.\n\n"
                    "If you only need a section of this file, use this pattern first:\n"
                    "  1. find_and_add_clip(path=<audio_path>, query='<end phrase>') — locates the exact timestamp\n"
                    "  2. extract_clip_segment(src=<audio_path>, dst=..., start=<start>, end=<found_end>)\n"
                    "  3. Replace the timeline audio clip with the trimmed segment\n"
                    "  4. Then call trigger_pipeline\n\n"
                    "If you genuinely need the full file transcribed, pass path=<audio_path> "
                    "to trigger_pipeline directly to bypass this check."
                )
        # Kick off the pipeline and return immediately. The engine runs separation
        # + transcription in a background thread; the caller polls get_pipeline_status
        # until stage='done', then (for modes 'both' / 'transcribe_only') calls
        # generate_typography(preset=...) to lay the lyric bricks.
        result = _call("trigger_pipeline", arguments)
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    if name == "remove_silence":
        result = await _remove_silence(arguments)
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    if name == "cut_filler_words":
        result = await _cut_filler_words(arguments)
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    if name == "apply_multicam_cuts":
        result = await _apply_multicam_cuts(arguments)
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    if name == "add_chapter_marker":
        result = await _add_chapter_marker(arguments)
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    if name == "remove_chapter_marker":
        result = await _remove_chapter_marker(arguments)
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    if name == "generate_chapters":
        result = await _generate_chapters(arguments)
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    if name == "add_callout":
        result = await _add_callout(arguments)
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    if name == "add_body_fx_brick":
        result = await _add_body_fx_brick(arguments)
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    if name == "process_body_fx_masks":
        result = _call("start_body_fx_process", arguments)
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    if name == "extract_clip_segment":
        dst = arguments.get("dst", "")
        if dst:
            Path(dst).parent.mkdir(parents=True, exist_ok=True)
        result = _call("extract_clip_segment", arguments)
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    if name == "add_clip":
        result = await _add_clip(arguments)
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    if name == "remove_background":
        result = await _remove_background(arguments)
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    if name == "find_and_add_clip":
        result = await _find_and_add_clip(arguments)
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    if name == "find_audio_cue":
        result = await _find_audio_cue(arguments)
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    if name == "find_video_moment":
        result = await _find_video_moment(arguments)
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    if name == "download_vision_model":
        result = await _download_vision_model()
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    if name == "search_transcript":
        result = _search_transcript(arguments)
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    if name == "cut_at_phrase":
        result = _cut_at_phrase(arguments)
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    if name == "read_transcript_context":
        path   = _resolve_path(arguments["path"])
        time   = float(arguments["time"])
        before = float(arguments.get("before", 30))
        after  = float(arguments.get("after",  60))
        gap_threshold = float(arguments.get("gap_threshold", 0.8))
        p = Path(path)
        words_path = _best_words_cache(str(p))
        if words_path is None:
            raise ValueError(f"no transcript cached for {p.name} — run find_and_add_clip or trigger_pipeline first")
        with open(words_path) as f:
            all_words = json.load(f)
        lo, hi = time - before, time + after
        window = [w for w in all_words if lo <= float(w["start"]) <= hi]

        # Group into utterances split by pauses >= gap_threshold seconds.
        # gap_before > 0 on an utterance marks a likely speaker change.
        utterances = []
        current: list = []
        for w in window:
            if current:
                gap = float(w["start"]) - float(current[-1]["end"])
                if gap >= gap_threshold:
                    utterances.append({
                        "start":      float(current[0]["start"]),
                        "end":        float(current[-1]["end"]),
                        "gap_before": round(gap, 3),
                        "text":       " ".join(x["word"] for x in current),
                    })
                    current = []
            current.append(w)
        if current:
            utterances.append({
                "start":      float(current[0]["start"]),
                "end":        float(current[-1]["end"]),
                "gap_before": 0.0,
                "text":       " ".join(x["word"] for x in current),
            })

        return [TextContent(type="text", text=json.dumps({
            "utterances": utterances,
            "words":      window,
        }, indent=2))]
    if name == "get_search_status":
        # Long-poll: block until the message changes or the search finishes,
        # so rapid successive calls don't return stale identical results.
        first = _call("get_search_status", {})
        if not first.get("running", False):
            return [TextContent(type="text", text=json.dumps(first, indent=2))]
        last_msg = first.get("message", "")
        for _ in range(60):  # wait up to 30s for a change
            await asyncio.sleep(0.5)
            st = _call("get_search_status", {})
            if not st.get("running", False) or st.get("message", "") != last_msg:
                return [TextContent(type="text", text=json.dumps(st, indent=2))]
        return [TextContent(type="text", text=json.dumps(st, indent=2))]
    if name == "get_vision_model_status":
        result = _call("get_vision_model_status", {})
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    if name == "take_snapshot":
        ipc_args = {}
        if "time" in arguments:
            ipc_args["time"] = arguments["time"]
        if "source" in arguments:
            ipc_args["source"] = arguments["source"]
        _call("take_snapshot", ipc_args)
        for _ in range(50):
            await asyncio.sleep(0.2)
            st = _call("get_snapshot_status", {})
            if st.get("done"):
                if "error" in st:
                    raise ValueError(f"Snapshot failed: {st['error']}")
                path = st["path"]
                with open(path, "rb") as f:
                    raw = f.read()
                try:
                    from PIL import Image as _PILImage
                    img = _PILImage.open(io.BytesIO(raw))
                    img.thumbnail((540, 960))
                    buf = io.BytesIO()
                    img.save(buf, format="PNG")
                    raw = buf.getvalue()
                except Exception:
                    pass
                return [
                    TextContent(type="text", text=path),
                    ImageContent(type="image", data=base64.b64encode(raw).decode(), mimeType="image/png"),
                ]
        raise RuntimeError("take_snapshot timed out")
    if name == "verify_clips":
        async def _snap_at(t: float) -> str:
            _call("seek", {"time": t})
            _call("take_snapshot", {})
            for _ in range(50):
                await asyncio.sleep(0.2)
                st = _call("get_snapshot_status", {})
                if st.get("done"):
                    if "error" in st:
                        raise ValueError(f"Snapshot failed at t={t}: {st['error']}")
                    return st["path"]
            raise RuntimeError(f"verify_clips snapshot timed out at t={t}")
        content = []
        for t in arguments["times"]:
            path = await _snap_at(t)
            with open(path, "rb") as f:
                raw = f.read()
            try:
                from PIL import Image as _PILImage
                img = _PILImage.open(io.BytesIO(raw))
                img.thumbnail((540, 960))
                buf = io.BytesIO()
                img.save(buf, format="PNG")
                raw = buf.getvalue()
            except Exception:
                pass
            content.append(TextContent(type="text", text=f"t={t} → {path}"))
            content.append(ImageContent(type="image", data=base64.b64encode(raw).decode(), mimeType="image/png"))
        return content
    if name == "get_stills":
        result = _call("get_stills", {"paths": arguments["paths"]})
        content = []
        for entry in result.get("stills", []):
            path  = entry["path"]
            still = entry.get("still", "")
            ok    = entry.get("ok", False)
            if not ok or not still:
                content.append(TextContent(type="text", text=f"{path}: no still available"))
                continue
            try:
                with open(still, "rb") as f:
                    raw = f.read()
                from PIL import Image as _PILImage
                img = _PILImage.open(io.BytesIO(raw))
                img.thumbnail((540, 960))
                buf = io.BytesIO()
                img.save(buf, format="JPEG")
                raw = buf.getvalue()
            except Exception:
                pass
            content.append(TextContent(type="text", text=Path(path).name))
            content.append(ImageContent(type="image", data=base64.b64encode(raw).decode(), mimeType="image/jpeg"))
        return content
    if name == "crop_media":
        import cv2 as _cv2

        src = arguments["source_path"]
        aspect_str  = arguments.get("aspect", "square")
        face_detect = arguments.get("face_detect", True)
        pad_top     = float(arguments.get("pad_top",    0.4))
        pad_bottom  = float(arguments.get("pad_bottom", 0.3))
        x_pct       = float(arguments.get("x_pct", 0.5))
        y_pct       = float(arguments.get("y_pct", 0.5))

        aspect_map = {"square": (1, 1), "vertical": (9, 16), "horizontal": (16, 9)}
        if aspect_str in aspect_map:
            ar_w, ar_h = aspect_map[aspect_str]
        else:
            parts = aspect_str.split(":")
            ar_w, ar_h = int(parts[0]), int(parts[1])

        is_image = Path(src).suffix.lower() in {".heic", ".heif", ".jpg", ".jpeg",
                                                 ".png", ".bmp", ".webp", ".tiff"}
        rot = 0  # display rotation; only set for video with rotation metadata
        if is_image:
            out = subprocess.check_output(
                ["magick", "identify", "-format", "%w %h", src + "[0]"],
                stderr=subprocess.DEVNULL).decode().split()
            src_w, src_h = int(out[0]), int(out[1])
        else:
            probe = subprocess.check_output(
                ["ffprobe", "-v", "quiet", "-print_format", "json", "-show_streams", src],
                stderr=subprocess.DEVNULL)
            streams = json.loads(probe)["streams"]
            vs = next(s for s in streams if s.get("codec_type") == "video")
            src_w, src_h = int(vs["width"]), int(vs["height"])
            # apply frame cropping side data (e.g. MOV files with encoded borders)
            for sd in vs.get("side_data_list", []):
                if sd.get("side_data_type") == "Frame Cropping":
                    src_w -= int(sd.get("crop_left", 0)) + int(sd.get("crop_right", 0))
                    src_h -= int(sd.get("crop_top", 0)) + int(sd.get("crop_bottom", 0))
            # track rotation: check tags first, then Display Matrix side data
            rot = int(float(vs.get("tags", {}).get("rotate", "0")))
            if rot == 0:
                for sd in vs.get("side_data_list", []):
                    if sd.get("side_data_type") == "Display Matrix" and "rotation" in sd:
                        rot = int(sd["rotation"])
                        break
            if abs(rot) == 90 or abs(rot) == 270:
                src_w, src_h = src_h, src_w

        # ── Face detection ───────────────────────────────────────────────────
        face_cx, face_cy = None, None
        face_detected = False
        if face_detect:
            try:
                # get a representative frame (image or mid-video frame)
                if is_image:
                    tmp_frame = f"/tmp/_crop_face_probe_{Path(src).stem}.png"
                    subprocess.run(["magick", src + "[0]", "-resize", "1308x1744>",
                                    tmp_frame], capture_output=True)
                else:
                    tmp_frame = f"/tmp/_crop_face_probe_{Path(src).stem}.png"
                    mid = subprocess.check_output(
                        ["ffprobe", "-v", "quiet", "-show_entries", "format=duration",
                         "-print_format", "json", src], stderr=subprocess.DEVNULL)
                    dur = float(json.loads(mid)["format"].get("duration", 1.0))
                    subprocess.run(
                        ["ffmpeg", "-y", "-i", src, "-ss", str(dur * 0.3),
                         "-frames:v", "1", tmp_frame],
                        capture_output=True)

                frame = _cv2.imread(tmp_frame)
                if frame is not None:
                    fh, fw = frame.shape[:2]
                    # scale probe frame to match post-rotation display dimensions
                    if fw != src_w or fh != src_h:
                        frame = _cv2.resize(frame, (src_w, src_h))
                    gray = _cv2.cvtColor(frame, _cv2.COLOR_BGR2GRAY)
                    det  = _cv2.CascadeClassifier(
                        _cv2.data.haarcascades + "haarcascade_frontalface_default.xml")
                    faces = det.detectMultiScale(
                        gray, scaleFactor=1.05, minNeighbors=3,
                        minSize=(int(min(src_w, src_h) * 0.1), int(min(src_w, src_h) * 0.1)))
                    if len(faces) > 0:
                        # pick largest face (cast to int to avoid numpy int32 serialization issues)
                        fx, fy, fw2, fh2 = (int(v) for v in max(faces, key=lambda f: f[2] * f[3]))
                        # add padding: more above (hair) than below (chin)
                        pad_px_top    = int(fh2 * pad_top)
                        pad_px_bottom = int(fh2 * pad_bottom)
                        pad_px_side   = int(fw2 * 0.15)
                        face_cx = fx + fw2 // 2
                        face_cy = (fy - pad_px_top + fy + fh2 + pad_px_bottom) // 2
                        # make the crop square (or target aspect) around the padded face
                        padded_h = fh2 + pad_px_top + pad_px_bottom
                        padded_w = fw2 + pad_px_side * 2
                        # expand to match target aspect ratio
                        if padded_w / padded_h > ar_w / ar_h:
                            crop_w = padded_w
                            crop_h = int(crop_w * ar_h / ar_w)
                        else:
                            crop_h = padded_h
                            crop_w = int(crop_h * ar_w / ar_h)
                        # clamp to source dimensions while preserving aspect ratio
                        if crop_w > src_w:
                            crop_w = src_w
                            crop_h = int(crop_w * ar_h / ar_w)
                        if crop_h > src_h:
                            crop_h = src_h
                            crop_w = int(crop_h * ar_w / ar_h)
                        x = max(0, min(face_cx - crop_w // 2, src_w - crop_w))
                        y = max(0, min(face_cy - crop_h // 2, src_h - crop_h))
                        face_detected = True
            except Exception as _e:
                pass  # fall through to manual pct path

        # ── Manual pct fallback ──────────────────────────────────────────────
        if not face_detected:
            if src_w / src_h > ar_w / ar_h:
                crop_h = src_h
                crop_w = int(crop_h * ar_w / ar_h)
            else:
                crop_w = src_w
                crop_h = int(crop_w * ar_h / ar_w)
            x = max(0, min(int((src_w - crop_w) * x_pct), src_w - crop_w))
            y = max(0, min(int((src_h - crop_h) * y_pct), src_h - crop_h))

        p = Path(src)
        out_ext = ".png" if is_image else ".mp4"
        out_path = str(p.parent / f"{p.stem}_crop{out_ext}")

        if is_image:
            subprocess.run(
                ["magick", src + "[0]", "-crop", f"{crop_w}x{crop_h}+{x}+{y}",
                 "+repage", out_path],
                check=True, stderr=subprocess.DEVNULL)
        else:
            # Prepend a transpose filter so crop sees display-orientation coords.
            # rotate=0 clears the stored rotation metadata (pixels are already rotated).
            if rot == -90 or rot == 270:
                vf = f"transpose=1,crop={crop_w}:{crop_h}:{x}:{y}"
            elif rot == 90 or rot == -270:
                vf = f"transpose=2,crop={crop_w}:{crop_h}:{x}:{y}"
            elif abs(rot) == 180:
                vf = f"vflip,hflip,crop={crop_w}:{crop_h}:{x}:{y}"
            else:
                vf = f"crop={crop_w}:{crop_h}:{x}:{y}"
            subprocess.run(
                ["ffmpeg", "-y", "-i", src,
                 "-vf", vf,
                 "-metadata:s:v:0", "rotate=0",
                 "-c:v", "libx264", "-crf", "18", "-preset", "fast",
                 "-c:a", "copy", out_path],
                check=True, stderr=subprocess.DEVNULL)

        # ── Inline thumbnail ─────────────────────────────────────────────────
        thumb_parts = []
        try:
            from PIL import Image as _PilImage
            if is_image:
                im = _PilImage.open(out_path)
            else:
                tmp_thumb = f"/tmp/_crop_thumb_{Path(src).stem}.png"
                subprocess.run(
                    ["ffmpeg", "-y", "-i", out_path, "-ss", str(0.3 * 2.0),
                     "-frames:v", "1", tmp_thumb],
                    capture_output=True)
                im = _PilImage.open(tmp_thumb)
            im.thumbnail((540, 540))
            buf = io.BytesIO()
            im.save(buf, format="PNG")
            thumb_parts = [ImageContent(type="image", data=base64.b64encode(buf.getvalue()).decode(),
                                        mimeType="image/png")]
        except Exception:
            pass

        result_text = json.dumps({
            "path": out_path,
            "width": crop_w, "height": crop_h,
            "face_detected": face_detected,
            "crop": {"x": x, "y": y, "w": crop_w, "h": crop_h},
        }, indent=2)
        return [TextContent(type="text", text=result_text)] + thumb_parts
    if name == "get_transcript":
        forward = {}
        if "path" in arguments:
            forward["path"] = arguments["path"]
        raw = _call("get_transcript", forward)
        slim = {"status": raw.get("status", "idle")}
        if "words" in raw:
            slim["words"] = raw["words"]
        if "path" in raw:
            slim["path"] = raw["path"]
        if "error" in raw:
            slim["error"] = raw["error"]
        # If IPC says idle, the words_json_path may not be synced to state yet
        # even though the pipeline already wrote the file. Fall back to deriving
        # the path from the explicit `path` arg, or the project audio_path.
        if slim["status"] == "idle":
            try:
                fallback_audio = arguments.get("path")
                if not fallback_audio:
                    proj = _call("get_project", {})
                    fallback_audio = proj.get("audio_path", "")
                if fallback_audio:
                    words_path = _best_words_cache(str(fallback_audio))
                    if words_path is not None:
                        with open(words_path) as f:
                            slim = {
                                "status":     "ready",
                                "words":      json.load(f),
                                "path":       str(words_path),
                                "provenance": "search" if words_path.name.endswith("_words_search.json") else "canonical",
                            }
            except Exception:
                pass
        return [TextContent(type="text", text=json.dumps(slim, indent=2))]
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
