# Pop Maker Studio — Roadmap

Local-first lyric video and karaoke creation tool for pop artists.
No internet. No uploads. Everything runs on your machine.

---

## Shipped

- **NLE studio** — single-workspace editor, no wizard flow
- **Splash screen** — Inter Black, full-screen, auto-advances
- **Multi-track timeline** — Subtitle / Audio / Video tracks, unlimited rows
- **Clip editing** — drag to move, drag edges to trim, split (S), delete (Del)
- **Right-click context menus** — on clips, track labels, empty timeline space
- **ML pipeline as on-demand tool** — right-click any Audio/Video clip to trigger
  - Transcribe + Separate Vocals (WhisperX + Demucs)
  - Transcribe only (WhisperX on original file)
  - Separate Vocals only (Demucs, no subtitles)
- **Subtitle grouping modes** — Word / Phrase / Line / Segment / Custom N words
- **All outputs saved on pipeline completion**
  - `_words.json`, `_segments.json`
  - `vocals.wav`, `instrumental.wav`
  - `_word.srt`, `_phrase.srt`, `_line.srt`, `_segment.srt`, `_custom.srt`
- **Blender export** — generates a `.py` script for lyric-video-blender addon
- **Right panel tabs** — Clip / Style / Export always accessible
- **Eight animation styles** — Fade, Glitch, Typewriter, Bounce, Scale, Slide, Stack, Block

---

## Up Next — Clip Tab Overhaul

Make the Clip tab fully context-sensitive. Three different faces:

**Audio clip selected**
- File path, timing (start/end), volume slider
- ML Processing section (Transcribe + Separate / Transcribe only / Separate only)
- Inline pipeline progress bar while running
- Speed control

**Video clip selected**
- Same as audio +
- ML Processing includes Remove Background (rembg)
- Opacity slider
- Speed control

**Subtitle clip selected**
- Text input, timing, nudge buttons (existing)
- Position override per clip — Bottom / Center / Top / Custom Y
- Color and opacity override per clip
- Subtitle grouping mode (existing, moved here)

---

## Tier 1 — Video Editor Fundamentals

Standard NLE features every clip-based editor has. No ML required.

| Feature | What it does |
|---|---|
| **Speed control** | Per-clip playback speed 0.25× – 4×. Slow-mo a word drop, double-time a verse. Applied on render. |
| **Volume per clip** | Per-clip gain slider on Audio tracks. Ducking, emphasis, silence. |
| **Subtitle position per clip** | Override vertical slot per clip — bottom / center / top / custom Y percentage. |
| **Subtitle color + opacity per clip** | Override the global white per word or phrase. |
| **Transitions** | Crossfade between adjacent Video clips on the same track. Duration slider per transition. Rendered as alpha blend. |
| **Opacity per video clip** | Semi-transparent artist shot layered over a background or animated text. |

---

## Tier 2 — ML Tools

All run locally via the song2subs Python venv. Same inline progress strip as the pipeline.

| Feature | Model / library | What it does |
|---|---|---|
| **Background removal** | `rembg` | Cut the background out of a video clip frame-by-frame. Artist floats over the lyric animation. Right-click Video clip → Remove background. |
| **Beat detection + BPM snap** | `librosa.beat.beat_track` | Analyse the audio track, mark beat positions on the timeline ruler as tick marks. Optional snap-to-beat when dragging clips. BPM shown in the toolbar. |
| **Noise reduction** | `noisereduce` | One-click clean-up of room noise before transcription. Improves WhisperX accuracy on rough recordings. Available in the ML Processing section of Audio/Video clip tab. |

---

## Tier 3 — Later

| Feature | Notes |
|---|---|
| **Green screen / chroma key** | Key out a specific color per video clip. Heavier to implement correctly (edge feathering, spill suppression). |
| **Auto-color grading** | LUT application, curve adjustments. Out of scope until render pipeline is fully implemented. |
| **Object tracking** | Attach a subtitle or graphic to a moving subject. Requires per-frame detection — significant complexity. |
| **Split screen / PiP** | Composite multiple video clips side-by-side or picture-in-picture. Needs compositor layer in the render pipeline. |

---

## Render Pipeline (ongoing)

The current MP4 render is scaffolded (SRT exports real, video encoding simulated).
Full libavcodec render to close out once Tier 1 and Tier 2 are complete:

- Encode video frames with subtitle overlay burned in
- Apply speed, opacity, volume, and transitions during encode
- Output H.264 / AAC in selected format (9:16, 16:9, 1:1)
- Background-removed clips composited over video or solid fill

---

## Platform targets

| Platform | Status |
|---|---|
| Linux | Primary dev target — fully working |
| macOS | GitHub Actions build (macos-14) — needs testing |
| Windows | GitHub Actions build (windows-2022 + vcpkg) — file picker stub needs Win32 impl |
