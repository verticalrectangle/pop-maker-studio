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

Used for word-level transcription with DTW token timestamps via whisper.cpp. No external forced aligner — timestamps come from the model's own attention heads. Also provides the coarse pass for audio analysis v2 word alignment (per-line windows from an order-constrained fuzzy match onto the decoded word stream; see `src/audio_whisper_coarse.cpp`).

---

## wav2vec2 CTC — Word Forced Alignment

| | |
|---|---|
| **File** | `wav2vec2_ctc_float.onnx` (float32; falls back to `wav2vec2_ctc.onnx` quantised when absent) + `wav2vec2_vocab_float.json` (`wav2vec2_vocab.json` for the quant model) |
| **Size** | ~378 MB float / ~95 MB quantised |
| **Path** | `models/` next to the binary (hard-linked from the shared-models store) |
| **Source** | torchaudio pipeline `WAV2VEC2_ASR_BASE_960H` (facebook/wav2vec2-base-960h weights) |
| **Conversion** | `~/Projects/seen-and-not-seen/.venv/bin/python tools/export_wav2vec2_onnx.py --out <dir>/wav2vec2_ctc_float.onnx --vocab <dir>/wav2vec2_vocab_float.json` (logits-only wrapper, opset 14, dynamic sequence length) |
| **License** | [MIT](https://github.com/pytorch/fairseq/blob/main/LICENSE) (model weights: [CC-BY-NC-4.0 for LibriSpeech-derived fine-tune](https://huggingface.co/facebook/wav2vec2-base-960h) — local analysis use; check before redistributing) |
| **When downloaded** | Must be placed manually — not auto-downloaded |

Aligns lyric lines to the vocal stem inside Whisper coarse windows (torchaudio stay-advance trellis reimplemented in `src/audio_analysis_run.cpp`). The quantised export collapses first-word emissions on sung onsets, so the float model is preferred when installed (`wav2vec2_ctc_path()` in `src/paths.cpp`).

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
| **Size** | ~174 MB |
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

## selfie_multiclass_256x256 — Skin Segmentation

| | |
|---|---|
| **File** | `selfie_multiclass_256x256.onnx` |
| **Size** | ~16 MB (15.7 MiB on disk) |
| **Path** | `models/` next to the binary (hard-linked from shared-models; `models/*.onnx` are git-excluded) |
| **Source** | [storage.googleapis.com/mediapipe-models](https://storage.googleapis.com/mediapipe-models/image_segmenter/selfie_multiclass_256x256/float32/1/selfie_multiclass_256x256.tflite) (source `.tflite` SHA256 pinned as `TFLITE_SHA256` in the script) |
| **Original model** | MediaPipe selfie_multiclass_256x256 Image Segmenter — 6 classes: background, hair, body-skin, face-skin, clothes, others |
| **Conversion** | `python3 tools/convert_selfie_multiclass.py --out <dir>/selfie_multiclass_256x256.onnx` (tf2onnx, opset 16; the script verifies input `input_29` [1,256,256,3] RGB float32 in [0,1] and output `Identity` [1,256,256,6] per-class logits before writing) |
| **License** | [Apache 2.0](https://github.com/google-ai-edge/mediapipe/blob/master/LICENSE) |
| **When used** | Skin-gated beauty shaders (Skin Smooth, Glass Skin) via the async `src/skin_mask_cache.*` mask cache, and IPC `segment_image` — optional; shaders fall back to their fixed YCbCr window when the file is missing |

Runtime contract (`src/skin_segment.h`): input `input_29` [1,256,256,3] RGB float32 in [0,1]; output `Identity` [1,256,256,6] per-class LOGITS in order background, hair, body-skin, face-skin, clothes, others (softmax over the last axis gives confidence). `skin_segment_image` writes per-class 8-bit confidence PNGs at display resolution — the same files IPC `segment_image` returns.

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
| Whisper ggml-large-v3-turbo-q5_0 | ~584 MB | Yes (Setup screen) | Transcription + analysis coarse pass |
| wav2vec2 CTC float (quant fallback) | ~378 MB (~95 MB) | **No — manual** | Word alignment |
| Kim_Vocal_2 MDX-Net | ~64 MB | Yes (on first use) | Vocal separation |
| htdemucs | ~174 MB | **No — manual** | 4-stem music separation |
| u2net_human_seg | ~176 MB | Yes (Setup screen) | Background removal |
| selfie_multiclass_256x256 | ~16 MB | **No — hard-link from shared-models** | Skin FX masks, segment_image (optional) |
| HuBERT | ~190 MB | **No — manual** | Voice conversion |
| Piper voices | ~30–60 MB each | Yes (on first use) | TTS |
| RVC voice models | Varies | Via HF browser | Voice conversion |
| SCRFD + 2d106det + sprites | ~22 MB | No — ships in `models/face/` | Face filters (optional) |
