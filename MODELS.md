# Models

All ML models run locally. Most sit in `models/` next to the binary (see `app_models_dir()` in `src/paths.cpp`); voice models downloaded via the HuggingFace browser go to `~/.cache/pop-maker-studio/rvc/`. No model data is uploaded or transmitted anywhere.

---

## Whisper — Speech Transcription

| | |
|---|---|
| **File** | `ggml-large-v3-turbo-q5_0.bin` |
| **Size** | ~584 MB |
| **Path** | `models/` next to the binary |
| **Source** | [huggingface.co/ggerganov/whisper.cpp](https://huggingface.co/ggerganov/whisper.cpp) |
| **Original model** | OpenAI Whisper large-v3-turbo (quantised to Q5_0 by ggerganov) |
| **License** | [MIT](https://github.com/openai/whisper/blob/main/LICENSE) |
| **When downloaded** | Setup screen, or automatically on first transcription |

Used for word-level transcription with DTW token timestamps via whisper.cpp. No external forced aligner — timestamps come from the model's own attention heads.

---

## Kim_Vocal_2 — Vocal Separation

| | |
|---|---|
| **File** | `Kim_Vocal_2.onnx` |
| **Size** | ~64 MB |
| **Path** | `models/` next to the binary |
| **Source** | [huggingface.co/Politrees/UVR_resources](https://huggingface.co/Politrees/UVR_resources/resolve/main/models/MDXNet/Kim_Vocal_2.onnx) |
| **Original model** | MDX-Net architecture, trained by KimberleyJensen — widely used UVR5 community model |
| **License** | Community model; see source repository |
| **When downloaded** | Automatically on first vocal separation |

---

## htdemucs — 4-Stem Music Separation

| | |
|---|---|---|
| **File** | `htdemucs.onnx` |
| **Size** | ~167 MB |
| **Path** | `models/` next to the binary (hard-linked from the shared-models store) |
| **Source** | Meta Demucs `htdemucs` checkpoint (demucs 4.1.0 `htdemucs` bag, single model signature `955717e8`), via torch.hub: `https://dl.fbaipublicfiles.com/demucs/hybrid_transformer/955717e8-8726e21a.th` |
| **Conversion** | `~/Projects/seen-and-not-seen/.venv/bin/python tools/export_htdemucs_onnx.py --out /home/alexis/dev/pms-wt/shared-models/htdemucs.onnx` (HTDemucs core as a real-valued ONNX graph — spectrogram front/back end in C++/FFTW; the committed file was built this way) |
| **License** | [MIT](https://github.com/facebookresearch/demucs/blob/main/LICENSE) |
| **When downloaded** | Must be placed manually — not auto-downloaded |

Separates drums/bass/other/vocals (`separate_stems4` in `src/separate4.cpp`): ffmpeg decode to 44.1 kHz stereo, `python -m demucs` input normalisation (mono-mix mean/std, undone on the stems), 7.8 s training-length segments with 25% overlap and triangle crossfade (exactly `apply_model(shifts=0, split=True, overlap=0.25)`), FFTW STFT/iSTFT (n_fft 4096, hop 1024) around the ONNX core, weighted overlap-add. Parity: ≥ 71 dB per-stem SNR vs the PyTorch `Separator` on both reference excerpts (see `tools/separate4_parity.py`).

---

## u2net_human_seg — Background Removal

| | |
|---|---|
| **File** | `u2net_human_seg.onnx` |
| **Size** | ~176 MB |
| **Path** | `models/` next to the binary (rembg's `~/.u2net/` copy can be hardlinked in) |
| **Source** | [github.com/danielgatis/rembg](https://github.com/danielgatis/rembg/releases/download/v0.0.0/u2net_human_seg.onnx) |
| **Original model** | U²-Net (Qin et al., 2020), fine-tuned for human segmentation |
| **License** | [Apache 2.0](https://github.com/danielgatis/rembg/blob/main/LICENSE) |
| **When downloaded** | Setup screen, or automatically on first background removal |

Used for per-frame alpha mask generation. Output is streamed as grayscale MJPEG so the canvas updates in real time while processing. Inference runs via ONNX Runtime with 2× supersampling and Lanczos downsampling for edge quality.

---

## HuBERT — Voice Conversion Feature Extraction

| | |
|---|---|
| **File** | `hubert.onnx` |
| **Size** | ~190 MB |
| **Path** | `models/` next to the binary |
| **Source** | Must be placed manually — not auto-downloaded |
| **Original model** | Soft-VC HuBERT content encoder, as used by RVC |
| **License** | [MIT](https://github.com/bshall/soft-vc) |
| **When used** | Voice conversion (FX panel → Voice Convert) |

HuBERT extracts phonetic content embeddings from the source audio. The voice model (`.pth`) is loaded and exported to ONNX entirely in C++ — no Python, no libtorch. HuBERT itself must be provided as a pre-exported ONNX file.

**To enable voice conversion:** download `hubert_base.pt` from an RVC distribution (e.g. [huggingface.co/lj1995/VoiceConversionWebUI](https://huggingface.co/lj1995/VoiceConversionWebUI)) and export it with the bundled tool: `./build/export-hubert hubert_base.pt models/hubert.onnx`.

---

## Piper — Text-to-Speech

| | |
|---|---|
| **Files** | `{voice_id}.onnx` + `{voice_id}.onnx.json` |
| **Cache path** | `models/piper/` next to the binary |
| **Source** | [huggingface.co/rhasspy/piper-voices](https://huggingface.co/rhasspy/piper-voices) |
| **License** | [MIT](https://github.com/rhasspy/piper/blob/master/LICENSE.md) (Piper); individual voice licenses vary — see source repository |
| **When downloaded** | Automatically on first TTS use for a given voice |

Built-in voice aliases:

| Alias | Voice ID |
|---|---|
| `female` | `en_US-amy-medium` |
| `male` | `en_US-ryan-medium` |
| `whisper` | `en_US-lessac-medium` |
| `narrator` | `en_GB-alan-medium` |

Any Piper voice ID or absolute `.onnx` path can be used directly.

---

## RVC Voice Models — User-Provided

| | |
|---|---|
| **Format** | `.pth` (PyTorch checkpoint) |
| **Cache path** | `~/.cache/pop-maker-studio/rvc/` |
| **Source** | User-provided; browsable via the built-in HuggingFace model browser |
| **License** | Varies per model — check the source repository |

RVC `.pth` models are loaded and exported to ONNX entirely in C++ (zip+pickle parser + hand-rolled VITS ONNX builder). No Python or libtorch required at any point. The exported `.onnx` is cached alongside the `.pth` and reused on subsequent runs.

---

## SCRFD + 2d106det — Face Tracking (Live Filters)

| | |
|---|---|
| **Files** | `face/scrfd_10g.onnx` (~17 MB), `face/2d106det.onnx` (~5 MB), `face/sprite_ear.png`, `face/sprite_nose.png`, `face/sprite_tongue.png` |
| **Cache path** | `models/face/` next to the binary |
| **Source** | [huggingface.co/verticalrectangle/pop-maker-studio-models](https://huggingface.co/verticalrectangle/pop-maker-studio-models) (`face/`) |
| **Original models** | InsightFace SCRFD-10G face detector + 2d106det 106-point landmarks |
| **License** | InsightFace models — non-commercial research (check upstream) |
| **When used** | Camera-brick face filters (Pretty, Doggy, …) — optional; filters hide when absent |

The detector runs sparse (re-detect on loss or every ~2 s), the landmark net dense at camera rate, on a worker thread. The 2d106 contour ordering is a zig-zag (chin = point 0, jawlines 9–16 / 25–32) — see `src/face_track.h`. Sprite PNGs are pre-rendered art for the Doggy overlay.

---

## Summary

| Model | Size | Auto-download | Required for |
|---|---|---|---|
| Whisper ggml-large-v3-turbo-q5_0 | ~584 MB | Yes (Setup screen) | Transcription |
| Kim_Vocal_2 MDX-Net | ~64 MB | Yes (on first use) | Vocal separation |
| htdemucs | ~167 MB | **No — manual** | 4-stem music separation |
| u2net_human_seg | ~176 MB | Yes (Setup screen) | Background removal |
| HuBERT | ~190 MB | **No — manual** | Voice conversion |
| Piper voices | ~30–60 MB each | Yes (on first use) | TTS |
| RVC voice models | Varies | Via HF browser | Voice conversion |
| SCRFD + 2d106det + sprites | ~22 MB | No — ships in `models/face/` | Face filters (optional) |
