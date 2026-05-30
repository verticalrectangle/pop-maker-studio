# Pop Maker Studio — Agent Guide

## Architecture

A native C++ video editor controlled through an MCP server over a Unix socket IPC layer.
The app runs locally; Claude connects via `mcp_server/server.py`.

## Two rules that apply everywhere

**1. Edits require a batch.**
All mutations (add_clip, set_clip_prop, move_clip, delete_clip, etc.) must be wrapped:
`begin_batch("label")` → edits → `end_batch()`
Read-only calls (get_project, get_pipeline_status, etc.) need no batch.

**2. Long-running ops are async — start, then poll.**
Every background operation returns `{status: 'started'}` immediately.
Poll the matching `get_*` tool every 2–3 seconds and report progress to the user.

| Start | Poll until | Then |
|-------|-----------|------|
| `trigger_pipeline` | `get_pipeline_status` stage=`done` | `generate_typography` |
| `analyze_audio(path)` | `get_audio_analysis` status=`done` | `find_audio_cue` |
| `find_and_add_clip(path, query)` | `get_search_status` running=`false` | `extract_clip_segment` → `add_clip` |
| `describe_video(path)` | `get_video_description` status=`done` | `find_video_moment` |
| `download_vision_model` | `get_vision_model_status` status=`ready` | use vision tools |

## Track layering

Track 0 = top (foreground). Highest index = bottom (background).
- Text, FX, overlays → low-index tracks (0, 1, 2…)
- Video, background → high-index tracks

## Canvas formats

`set_format` presets: `vertical` (9:16 TikTok/Reels), `horizontal` (16:9 YouTube), `square` (1:1 Instagram)

## File path conventions

- Transcripts: `{parent}/{stem}/{stem}_words.json`
- Extracted segments: `{parent}/{stem}/{stem}_{start_int}_{end_int}.webm`
- `find_and_add_clip` only adds the short extracted segment — never the full source file

## Audio positioning

To sync an audio track to a video at a specific source moment, use a negative start:
`clip start = -source_timestamp, clip end = video_duration - source_timestamp`

## Clip props reference

`volume` (0–2), `speed` (0.25–4), `opacity` (0–1), `muted` (bool), `in_point` (source offset seconds),
`fade_in`, `fade_out`, `pos_x`, `pos_y`, `scale_x`, `scale_y`, `rotation`, `text`, `font_size` (0=auto)

Text layout: `sub_pos` (0=bottom 1=center 2=top 3=custom), `sub_pos_x/y` (0–1), `sub_anchor_h` (0=left 1=center 2=right), `sub_wrap_w` (0–1)

Color arrays: `[r, g, b, a]` with values 0–1. Props: `sub_color`, `karaoke_highlight_color`

Animation: `clip_style` (none|fade|glitch|typewriter|bounce|scale|slide|stack|block), `karaoke` (bool), `blend_mode` (normal|add|multiply|screen|overlay)
