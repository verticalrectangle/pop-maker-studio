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
from contextlib import contextmanager
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
    payload = json.dumps({"id": req_id, "method": method, "params": params or {}}, ensure_ascii=False) + "\n"

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


@contextmanager
def _batch(label: str):
    _call("begin_batch", {"label": label})
    try:
        yield
    finally:
        _call("end_batch", {})


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
                "Add a video or audio clip first — audio_path is set automatically from the first clip. "
                "Requires batch. (Use find_and_add_clip to do all of this in one step.)"
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
                "Add a Multi-FX brick containing an ordered chain of sub-effects. "
                "Use instead of multiple overlapping add_effect_brick calls.\n\n"
                "GLASS behaviour: if placed on the same track as a video/audio clip it overlaps, "
                "it becomes a 'glass' FX and applies only to that clip. "
                "Place on a separate FX track for a global (all-layers) effect.\n\n"
                "effects array: each entry is an object with:\n"
                "  fx_type (required) — any fx_type from add_effect_brick, or 'body_fx'\n"
                "  body_fx_type — required when fx_type is 'body_fx' (see add_body_fx_brick for valid names)\n"
                "  rel_start (default 0) — seconds from brick start\n"
                "  rel_end   (default 0 = until brick end) — seconds from brick start\n"
                "  params — same param dict as add_effect_brick for the given fx_type\n\n"
                "⚠️ BodyFX constraint: if any sub-effect has fx_type 'body_fx', the MultiFX brick "
                "MUST be placed on the same track as a video clip (glass mode only). "
                "BodyFX requires a sibling video clip to source the mask from.\n\n"
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
                "digital_noise (amount,intensity,color_sep,luma_bias)\n\n"
                "Requires batch."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track": {"type": "integer"},
                    "fx_type": {"type": "string", "description": "Snake_case FX type name (see description)"},
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
            name="get_vision_model_status",
            description=(
                "Check whether the local Moondream2 vision model (~1.1 GB) is installed. "
                "Returns {status: 'ready'|'downloading'|'idle'|'error', progress?, message?}. "
                "Call this before find_video_moment to decide if download_vision_model is needed."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="download_vision_model",
            description=(
                "Download the Moondream2 q4 ONNX vision model (~1.1 GB one-time download). "
                "Checks the local HuggingFace cache first — if the weights are already there "
                "it just copies them (~seconds). Otherwise downloads from HuggingFace. "
                "The app shows a progress bar while this runs. "
                "Blocks until the download is complete or fails (up to 5 minutes)."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="find_video_moment",
            description=(
                "Analyse a video file with the local Moondream2 vision model (runs on device, "
                "no API cost) and find the timestamp(s) that best match a natural-language query. "
                "The app must be running. Analysis takes up to ~3 minutes for a long video.\n\n"
                "query examples: 'close-up face reaction', 'crowd shot', 'energetic dancing', "
                "'aerial establishing shot', 'someone laughing'\n\n"
                "Returns a list of up to 3 matches sorted by relevance: "
                "{timestamp, description, confidence}."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "path": {"type": "string", "description": "Absolute path to the video file"},
                    "query": {"type": "string", "description": "Natural-language description of the moment to find"},
                },
                "required": ["path", "query"],
            },
        ),
        Tool(
            name="find_and_add_clip",
            description=(
                "Find a specific moment in a video file by searching the transcript, then add "
                "it to the timeline already trimmed to that segment. Avoids generating a proxy "
                "for the full file — proxy only generates for the trimmed clip.\n\n"
                "Workflow (all internal):\n"
                "  1. Add video to a new track → audio_path auto-set\n"
                "  2. Run transcription pipeline and wait for completion\n"
                "  3. Search transcript for query text\n"
                "  4. Trim the clip to the matched segment (+ context padding)\n\n"
                "query examples: 'I did not wake up a loser', 'talking about failure', "
                "'the part where he mentions semiconductors'\n\n"
                "Returns: {track, clip, start, end, transcript_excerpt, found_at_source}"
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "path":    {"type": "string", "description": "Absolute path to the video file"},
                    "query":   {"type": "string", "description": "What to search for in the transcript"},
                    "track":   {"type": "integer", "description": "Track to add the clip to (default 0)"},
                    "padding": {"type": "number",  "description": "Seconds of context before/after the matched segment (default 5.0)"},
                },
                "required": ["path", "query"],
            },
        ),
        Tool(
            name="get_transcript",
            description=(
                "Return the word-level transcript produced by the transcription pipeline. "
                "Returns {status: 'idle'|'ready'|'error', words?: [{word, start, end}]}. "
                "Run trigger_pipeline first if status is 'idle'. "
                "Word timestamps are source-file-relative seconds."
            ),
            inputSchema={"type": "object", "properties": {}},
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
                "fx_type options (40 effects, case-sensitive names from the BodyFX library):\n"
                "  Retro: RetroTV, VHSGlitch, Scanlines, Halftone, CRTDistort\n"
                "  Depth: DepthBlur, DepthFog, TiltShift, CinematicDOF\n"
                "  Glitch: GlitchDisplace, ChromaShift, SignalNoise, DataBurst\n"
                "  Color: NeonOutline, ThermalCamera, XRayBody, InfraredGlow\n"
                "  Light: AuraGlow, HoloShimmer, LightTrails, RimLight\n"
                "  Abstract: LiquidMorph, ParticleDissolve, PixelSort, FractalEdge\n"
                "  Party: DiscoBall, Confetti, RainbowAura, GlitterBurst\n"
                "(If unsure, use 'NeonOutline' or 'DepthBlur'.)\n\n"
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
                "brick's right edge past the already-processed range."
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


# ── vision model download ─────────────────────────────────────────────────────

async def _download_vision_model() -> dict:
    status = _call("get_vision_model_status", {})
    if status.get("status") == "ready":
        return {"status": "ready", "message": "Vision model already installed."}

    _call("download_vision_model", {})

    for _ in range(300):  # up to 5 minutes
        await asyncio.sleep(1.0)
        st = _call("get_vision_model_status", {})
        s = st.get("status")
        if s == "ready":
            return {"status": "ready", "message": "Vision model installed successfully."}
        if s == "error":
            raise ValueError("Vision model download failed: " + st.get("error", "unknown"))
    raise TimeoutError("Vision model download timed out after 5 minutes")


# ── find_video_moment ─────────────────────────────────────────────────────────

def _tfidf_score(query: str, text: str) -> float:
    """Simple word-overlap score between query and text (0–1)."""
    q_words = set(query.lower().split())
    t_words = set(text.lower().split())
    if not q_words:
        return 0.0
    return len(q_words & t_words) / len(q_words)


async def _find_video_moment(arguments: dict) -> list[dict]:
    path        = arguments.get("path", "")
    query       = arguments.get("query", "")
    if not path:
        raise ValueError("path is required")
    if not query:
        raise ValueError("query is required")

    # Gate on vision model being installed
    st = _call("get_vision_model_status", {})
    if st.get("status") != "ready":
        raise ValueError(
            "Vision model not installed. Call download_vision_model first "
            "(~1.1 GB one-time download — the app will show a progress bar)."
        )

    # Trigger analysis (or pick up a previously completed one)
    try:
        _call("describe_video", {"path": path})
    except ValueError as e:
        if "already running" not in str(e):
            raise

    # Poll until done (up to 180 s)
    for _ in range(360):
        await asyncio.sleep(0.5)
        res = _call("get_video_description", {})
        status = res.get("status")
        if status == "done":
            break
        if status == "error":
            raise ValueError("scene analysis failed: " + res.get("message", "unknown"))
    else:
        raise TimeoutError("scene analysis timed out after 180 s")

    frames = res.get("frames", [])
    if not frames:
        return [{"timestamp": 0.0, "description": "(no frames)", "confidence": 0.0}]

    # Score each frame against the query
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

    proj = _call("get_project", {})
    tracks = proj.get("tracks", [])
    if track >= len(tracks):
        raise ValueError(f"track {track} does not exist")
    clips = tracks[track].get("clips", [])
    if clip_idx >= len(clips):
        raise ValueError(f"clip {clip_idx} does not exist on track {track}")

    c = clips[clip_idx]
    source   = c.get("source", "")
    tl_start = float(c["start"])
    in_point = float(c["in_point"])
    duration = float(c["duration"])

    if not source:
        raise ValueError("clip has no source file (is it a video/audio clip?)")

    # Analyze audio
    _call("analyze_audio", {"path": source})
    for _ in range(240):
        await asyncio.sleep(0.5)
        res = _call("get_audio_analysis", {})
        if res.get("status") == "done":
            break
        if res.get("status") == "error":
            raise ValueError("audio analysis failed for: " + source)
    else:
        raise TimeoutError("audio analysis timed out")

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
        proj2 = _call("get_project", {})
        tr_clips = proj2["tracks"][track]["clips"]

        # Find clip covering cut_e (split right boundary first)
        right_clip = next((i for i, cl in enumerate(tr_clips)
                           if cl["start"] <= cut_e < cl["end"]), None)
        if right_clip is not None and abs(tr_clips[right_clip]["end"] - cut_e) > 0.02:
            _call("split_clip", {"track": track, "clip": right_clip, "time": cut_e})
            proj2 = _call("get_project", {})
            tr_clips = proj2["tracks"][track]["clips"]

        # Find clip covering cut_s (split left boundary)
        left_clip = next((i for i, cl in enumerate(tr_clips)
                          if cl["start"] <= cut_s < cl["end"]), None)
        if left_clip is not None and abs(tr_clips[left_clip]["start"] - cut_s) > 0.02:
            _call("split_clip", {"track": track, "clip": left_clip, "time": cut_s})
            proj2 = _call("get_project", {})
            tr_clips = proj2["tracks"][track]["clips"]

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

    proj = _call("get_project", {})
    tracks = proj.get("tracks", [])
    if track >= len(tracks):
        raise ValueError(f"track {track} does not exist")
    clips = tracks[track].get("clips", [])
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

        proj2    = _call("get_project", {})
        tr_clips = proj2["tracks"][track]["clips"]

        right_clip = next((i for i, cl in enumerate(tr_clips)
                           if cl["start"] <= cut_e < cl["end"]), None)
        if right_clip is not None and abs(tr_clips[right_clip]["end"] - cut_e) > 0.02:
            _call("split_clip", {"track": track, "clip": right_clip, "time": cut_e})
            proj2    = _call("get_project", {})
            tr_clips = proj2["tracks"][track]["clips"]

        left_clip = next((i for i, cl in enumerate(tr_clips)
                          if cl["start"] <= cut_s < cl["end"]), None)
        if left_clip is not None and abs(tr_clips[left_clip]["start"] - cut_s) > 0.02:
            _call("split_clip", {"track": track, "clip": left_clip, "time": cut_s})
            proj2    = _call("get_project", {})
            tr_clips = proj2["tracks"][track]["clips"]

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

    # Fetch project to find the overall time range
    proj   = _call("get_project", {})
    tracks = proj.get("tracks", [])

    # Determine end time from camera track clips
    end_time = 0.0
    for ct in camera_tracks:
        if ct < len(tracks):
            for cl in tracks[ct].get("clips", []):
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
            proj2    = _call("get_project", {})
            tr_clips = proj2["tracks"][ct]["clips"] if ct < len(proj2["tracks"]) else []
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
            proj2    = _call("get_project", {})
            tr_clips = proj2["tracks"][ct]["clips"] if ct < len(proj2["tracks"]) else []

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


# ── find_and_add_clip ─────────────────────────────────────────────────────────

async def _find_and_add_clip(arguments: dict) -> dict:
    path    = arguments.get("path", "")
    query   = arguments.get("query", "")
    track   = int(arguments.get("track", 0))
    padding = float(arguments.get("padding", 5.0))

    if not path:
        raise ValueError("path is required")
    if not query:
        raise ValueError("query is required")

    # Check for a cached words JSON next to the source file (saved by a previous transcription)
    p = Path(path)
    cached_words_path = p.parent / p.stem / f"{p.stem}_words.json"  # convention used by the app
    words = None
    if cached_words_path.exists():
        with open(cached_words_path) as f:
            words = json.load(f)

    # Fall back to app transcript state if no cache on disk
    if not words:
        proj = _call("get_project", {})
        tr = _call("get_transcript", {})
        if tr.get("status") == "ready" and proj.get("audio_path", "") == path:
            words = tr["words"]

    if words:
        # Search cached transcript
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
                best_start = float(window[0]["start"])
                best_end   = float(window[-1]["end"])
                best_text  = " ".join(w["word"] for w in window)

        if best_score < 0.3:
            raise ValueError(f"could not find '{query}' in transcript (best match score: {best_score:.2f})")
    else:
        # No cached transcript — use chunked Whisper search with early exit (no clip added)
        sr = _call("search_transcript", {
            "path":        path,
            "query_words": query.lower().split(),
            "buffer_sec":  padding + 30.0,
        })
        best_start = float(sr["start"])
        best_end   = float(sr["end"])
        best_text  = sr.get("excerpt", "")

    source_start = max(0.0, best_start - padding)
    source_end   = best_end + padding
    duration     = source_end - source_start

    # Extract the short segment to a new file so the proxy stays small
    dst = str(p.parent / p.stem / f"{p.stem}_{int(source_start)}_{int(source_end)}.webm")
    extract_result = _call("extract_clip_segment", {
        "src": path, "dst": dst,
        "start": source_start, "end": source_end,
    })

    # Ensure the track exists (may already exist if we went through transcription path)
    proj = _call("get_project", {})
    while len(proj.get("tracks", [])) <= track:
        with _batch("find_and_add_clip: add track"):
            _call("add_track", {"name": f"Track {track}", "position": track})
        proj = _call("get_project", {})

    with _batch(f"find_and_add_clip: add '{query}'"):
        clip_result = _call("add_clip", {
            "track": track, "type": "video",
            "text": dst, "start": 0, "end": duration,
        })
    clip_idx = clip_result["clip"]

    return {
        "track":              track,
        "clip":               clip_idx,
        "start":              0.0,
        "end":                round(duration, 3),
        "found_at_source":    round(best_start, 3),
        "segment_file":       dst,
        "transcript_excerpt": best_text[:300],
        "match_score":        round(best_score, 3),
    }


@server.call_tool()
async def call_tool(name: str, arguments: dict) -> list[TextContent]:
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
    if name == "get_vision_model_status":
        result = _call("get_vision_model_status", {})
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
