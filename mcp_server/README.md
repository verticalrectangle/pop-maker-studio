# Pop Maker Studio — MCP Server

Exposes the full editing surface of Pop Maker Studio to Claude via the Model Context Protocol.

## Setup

```bash
pip install -r mcp_server/requirements.txt
```

## Running

Start Pop Maker Studio first, then:

```bash
python3 mcp_server/server.py
```

The server reads `/tmp/pop-maker-studio.lock` to find the running app's socket.

## Claude Desktop

Add to `~/.config/claude/claude_desktop_config.json` (Linux) or
`~/Library/Application Support/Claude/claude_desktop_config.json` (macOS):

```json
{
  "mcpServers": {
    "pop-maker-studio": {
      "command": "python3",
      "args": ["/absolute/path/to/pop-maker-studio/mcp_server/server.py"]
    }
  }
}
```

## Claude Code (CLI)

Add to `.mcp.json` in the repo root:

```json
{
  "mcpServers": {
    "pop-maker-studio": {
      "command": "python3",
      "args": ["mcp_server/server.py"]
    }
  }
}
```

## Tools

| Tool | Description |
|------|-------------|
| `get_project` | Full state snapshot (tracks, clips, TextStyle, beats) |
| `get_pipeline_status` | Poll ML pipeline progress |
| `begin_batch` / `end_batch` | Wrap all mutations for undo history |
| `add_clip` | Add text/lyrics/video/audio/effect clip |
| `delete_clip` | Remove a clip |
| `move_clip` | Reposition clip on timeline |
| `trim_clip` | Adjust clip in/out points |
| `split_clip` | Cut clip at playhead time |
| `set_clip_prop` | Set volume/speed/opacity/text/position etc. |
| `set_text_style` | Shadow, stroke, glow, background box |
| `apply_effect` | Any of the 100 shader effects from registry |
| `generate_typography` | Generate lyric clips from word JSON |
| `trigger_pipeline` | Start Whisper transcription + vocal separation |
| `seek` / `play` / `pause` | Playback control |
| `save_project` / `load_project` | Project persistence |
| `validate_glsl` | Test-compile a GLSL shader |

## Agent guidance

Never run ML model inference outside of this server. All transcription, audio analysis, and video analysis runs inside Pop Maker Studio — use `trigger_pipeline`, `get_transcript`, `describe_video`, `get_stills`, and related tools instead.

## Workflow example

```
Claude: begin_batch("Build neon lyric video")
Claude: trigger_pipeline(mode="both")
Claude: [poll get_pipeline_status until done]
Claude: generate_typography()
Claude: [for each lyric clip] set_text_style(glow_enabled=True, glow_r=10, glow_col=[1, 0.2, 0.8, 0.7])
Claude: end_batch()
Claude: save_project()
```

## Effect list

Run `python3 tools/codegen_effects.py` to regenerate `effects/mcp_manifest.json`
after adding effects to `effects/registry.json`.
