# Pop Maker Studio — Agent Guide

## Architecture

A native C++ video editor controlled through an MCP server over a Unix socket IPC layer.
The app runs locally; Claude connects via `mcp_server/server.py`.

## Two rules that apply everywhere

**1. Batches are optional for single edits** (auto-applied and returns updated state).
Use `begin_batch("label")` → edits → `end_batch()` only when grouping multiple edits into one undo step.
Read-only calls (get_project, get_pipeline_status, etc.) need no batch.

**2. Long-running ops block until complete — no manual polling needed.**
These tools handle polling internally and return only when done:

| Tool | Returns when done | Then |
|------|------------------|------|
| `trigger_pipeline` | stage=`done` result | `generate_typography` |
| `analyze_audio(path)` | status=`done` result with beats/rms | `find_audio_cue` |
| `find_and_add_clip(path, query)` | status=`found` result | `extract_clip_segment` → `add_clip` |
| `remove_background` | status=`ready` | — |

Still requires manual polling (vision tools — slower cadence):

| Start | Poll until | Then |
|-------|-----------|------|
| `describe_video(path)` | `get_video_description` status=`done` | `find_video_moment` |
| `download_vision_model` | `get_vision_model_status` status=`ready` | use vision tools |

## Verifying clip placement

After placing video clips from transcript timestamps, always call `verify_clips` with the midpoint of each clip before proceeding. This catches wrong timestamps (e.g. wrong speaker) without stopping to ask.

## Searching vs. transcribing

**Searching for a specific moment** → always use `find_and_add_clip`. It does windowed search and stops when the match is found. Fast on long files. Never add the full source video to the timeline just to transcribe it.

**Generating subtitles/karaoke for clips already on the timeline** → use `trigger_pipeline`. This transcribes the full audio and is slow on long files.

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
