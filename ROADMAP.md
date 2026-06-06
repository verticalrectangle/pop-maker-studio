# Pop Maker Studio — Roadmap

Local-first music video and lyric video creation tool for pop artists.
No internet required. No uploads. Everything runs on your machine.

---

## Shipped

### Core Editor
- **NLE studio** — single-workspace editor, no wizard flow
- **Splash screen** — full-screen, auto-advances
- **Multi-track timeline** — Subtitle / Audio / Video tracks, unlimited rows
- **Clip editing** — drag to move, drag edges to trim, split (S), delete (Del)
- **Right-click context menus** — on clips, track labels, empty timeline space
- **Speed control** — per-clip playback speed (0.25×–4×), applied on render
- **Volume per clip** — per-clip gain slider on Audio tracks
- **Opacity per video clip** — with fade in/out ramps
- **Transitions** — crossfade between adjacent video clips, duration per transition
- **Blend modes** — Normal / Multiply / Screen / Overlay per video clip
- **Keyframe animation** — per-clip property curves (position, scale, rotation, opacity)

### ML Pipeline
- **Transcribe + Separate Vocals** — transcription + Demucs, runs locally
- **Transcribe only** — transcription on original file
- **Separate Vocals only** — Demucs, no subtitles
- **Beat detection** — librosa, beat markers on timeline ruler, BPM in toolbar, snap-to-beat
- **Amplitude envelope extraction** — per-audio RMS curve, drives reactive effects
- **Background removal** — rembg (u2net_human_seg), frame-accurate streaming preview
  - 2× supersampling + Lanczos downsample + σ=0.7 Gaussian for edge quality
  - Mask stored as streaming grayscale MJPEG, updates canvas in real-time while processing
  - Per-clip brick — each brick can have independent bg removal
  - **Bounding box crop** — "Limit area" rectangle zeroes alpha outside L/R/T/B bounds; fixes complex backgrounds with nearby objects
- **Noise reduction** — noisereduce spectral gating, one-click room noise cleanup before transcription

### Creative Effects (FX tab)
- **Color grading** — brightness, contrast, saturation, hue
- **Blur** — Gaussian, per-clip
- **Chroma key** — color picker, threshold + softness
- **Glitch** — chroma shift, row jitter, JPEG block corruption with transparency bleed
- **VHS** — chroma bleed, grain, tracking warp
- **Datamosh** — temporal ghost buffer, multi-key chroma chaos, bleedback
- **Zoom Punch** — beat-synced scale spike + shake
- **Light Leak** — procedural film flare synced to amplitude envelope

### Subtitle System
- **Eight animation styles** — Fade, Glitch, Typewriter, Bounce, Scale, Slide, Stack, Block
- **Grouping modes** — Word / Phrase / Line / Segment / Custom N words
- **All outputs on pipeline completion** — `_words.json`, `_segments.json`, `vocals.wav`, `instrumental.wav`, multiple SRT formats
- **Subtitle position per clip** — override vertical slot (bottom / center / top / custom Y%) per clip
- **Subtitle color + opacity per clip** — override global white per word or phrase

### Export
- **MP4 render** — H.264/AAC, subtitle overlay burned in, background-removed clips composited
- **Blender export** — generates `.py` script for lyric-video-blender addon
- **Format presets** — 9:16, 16:9, 1:1

### Onboarding
- **Tutorial / first project** — floating step panel (Help → Getting Started): drop footage → trim → beat sync → bg remove → export; auto-advances on state detection

---

## Later

| Feature | Notes |
|---|---|
| **GPU background removal** | BiRefNet-general/portrait are a different quality tier but need CUDA. Benchmarked at 33–63× slower than u2net_human_seg on CPU. Natural fit for a pro tier. |
| **Object tracking** | Attach a subtitle or graphic to a moving subject. Requires per-frame detection — significant complexity. |
| **Split screen / PiP** | Composite multiple video clips side-by-side or picture-in-picture. |
| **Auto-color grading** | LUT application. Out of scope until the effects system matures further. |

---

## Platform

| Platform | Status |
|---|---|
| Linux | Primary dev target — fully working |
| macOS | GitHub Actions build (macos-14) — needs testing |
| Windows | GitHub Actions build (windows-2022 + vcpkg) — file picker stub needs Win32 impl |
