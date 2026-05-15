# Models

All ML models run locally. None are bundled with the binary — they are downloaded on first use (or on the Setup screen) and stored in `~/.cache/pop-maker-studio/`. No model data is uploaded or transmitted anywhere.

---

## Whisper — Speech Transcription

| | |
|---|---|
| **File** | `ggml-large-v3-turbo-q5_0.bin` |
| **Size** | ~584 MB |
| **Cache path** | `~/.cache/pop-maker-studio/whisper/` |
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
| **Cache path** | `~/.cache/pop-maker-studio/mdx/` |
| **Source** | [huggingface.co/Politrees/UVR_resources](https://huggingface.co/Politrees/UVR_resources/resolve/main/models/MDXNet/Kim_Vocal_2.onnx) |
| **Original model** | MDX-Net architecture, trained by KimberleyJensen — widely used UVR5 community model |
| **License** | Community model; see source repository |
| **When downloaded** | Automatically on first vocal separation |

Used to separate vocals from instrumental. Instrumental is derived as `original − vocals`. The pipeline uses FFTW3 for STFT/iSTFT; inference runs via ONNX Runtime.

---

## u2net_human_seg — Background Removal

| | |
|---|---|
| **File** | `u2net_human_seg.onnx` |
| **Size** | ~176 MB |
| **Cache path** | `~/.u2net/` (rembg convention) |
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
| **Cache path** | `~/.cache/pop-maker-studio/hubert/` |
| **Source** | Must be placed manually — not auto-downloaded |
| **Original model** | Soft-VC HuBERT content encoder, as used by RVC |
| **License** | [MIT](https://github.com/bshall/soft-vc) |
| **When used** | Voice conversion (FX panel → Voice Convert) |

HuBERT extracts phonetic content embeddings from the source audio. The voice model (`.pth`) is loaded and exported to ONNX entirely in C++ — no Python, no libtorch. HuBERT itself must be provided as a pre-exported ONNX file.

**To enable voice conversion:** download `hubert.onnx` from [github.com/bshall/soft-vc](https://github.com/bshall/soft-vc) or an RVC distribution and place it at `~/.cache/pop-maker-studio/hubert/hubert.onnx`.

---

## Piper — Text-to-Speech

| | |
|---|---|
| **Files** | `{voice_id}.onnx` + `{voice_id}.onnx.json` |
| **Cache path** | `~/.cache/pop-maker-studio/piper/` |
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

## Summary

| Model | Size | Auto-download | Required for |
|---|---|---|---|
| Whisper ggml-large-v3-turbo-q5_0 | ~584 MB | Yes (Setup screen) | Transcription |
| Kim_Vocal_2 MDX-Net | ~64 MB | Yes (on first use) | Vocal separation |
| u2net_human_seg | ~176 MB | Yes (Setup screen) | Background removal |
| HuBERT | ~190 MB | **No — manual** | Voice conversion |
| Piper voices | ~30–60 MB each | Yes (on first use) | TTS |
| RVC voice models | Varies | Via HF browser | Voice conversion |
