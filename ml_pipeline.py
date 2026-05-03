#!/usr/bin/env python3
"""
Pop Maker Studio — ML pipeline
Called as a subprocess from the C++ app.
Runs Demucs vocal separation and/or WhisperX forced-alignment transcription.

Progress markers written to stderr:
  [MODEL]      loading models
  [SEPARATE]   demucs separation
  [TRANSCRIBE] whisperx transcription
  [ALIGN]      forced alignment
  [DONE]       finished

Output files (all written to --outdir):
  <stem>_words.json        word-level timestamps  (transcribe/both)
  <stem>_segments.json     sentence-level segments (transcribe/both)
  vocals.wav               isolated vocal stem     (separate/both)
  instrumental.wav         instrumental stem       (separate/both)
"""
import argparse
import json
import sys
import subprocess
import tempfile
import shutil
from pathlib import Path

import torch
import whisperx

WHISPER_MODEL = "large-v3"
LANGUAGE      = "en"
DEMUCS_MODEL  = "htdemucs"
DEVICE        = "cuda" if torch.cuda.is_available() else "cpu"
COMPUTE_TYPE  = "float16" if DEVICE == "cuda" else "float32"


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def run_separate(audio_path: Path, outdir: Path) -> Path:
    """Run Demucs, copy vocals.wav and instrumental.wav to outdir. Returns vocals path."""
    log("[SEPARATE] Running Demucs (htdemucs two-stem)...")
    tmpdir = Path(tempfile.mkdtemp(prefix="pms_"))
    try:
        subprocess.run(
            [
                sys.executable, "-m", "demucs",
                "--two-stems=vocals",
                "-n", DEMUCS_MODEL,
                "-o", str(tmpdir / "sep"),
                str(audio_path),
            ],
            check=True, stderr=sys.stderr,
        )
        stem        = audio_path.stem
        sep_base    = tmpdir / "sep" / DEMUCS_MODEL / stem
        vocals_src  = sep_base / "vocals.wav"
        instru_src  = sep_base / "no_vocals.wav"

        if not vocals_src.exists():
            matches = list((tmpdir / "sep" / DEMUCS_MODEL).rglob("vocals.wav"))
            if not matches:
                log(f"[ERROR] vocals.wav not found under {tmpdir}")
                sys.exit(1)
            vocals_src = matches[0]
            sep_base   = vocals_src.parent

        outdir.mkdir(parents=True, exist_ok=True)
        vocals_dst = outdir / "vocals.wav"
        instru_dst = outdir / "instrumental.wav"

        shutil.copy2(str(vocals_src), str(vocals_dst))
        log(f"[SEPARATE] Saved vocals → {vocals_dst}")

        # no_vocals.wav name varies by demucs version
        for candidate in [instru_src, sep_base / "no_vocals.wav",
                           sep_base / "other.wav"]:
            if candidate.exists():
                shutil.copy2(str(candidate), str(instru_dst))
                log(f"[SEPARATE] Saved instrumental → {instru_dst}")
                break

        return vocals_dst
    finally:
        shutil.rmtree(str(tmpdir), ignore_errors=True)


def run_transcribe(vocals_path: Path, outdir: Path, stem: str):
    """Run WhisperX on vocals, write _words.json and _segments.json to outdir."""
    log(f"[MODEL] Loading WhisperX {WHISPER_MODEL} on {DEVICE}...")
    model = whisperx.load_model(
        WHISPER_MODEL, DEVICE,
        compute_type=COMPUTE_TYPE,
        language=LANGUAGE,
    )
    log("[MODEL] Loading alignment model...")
    align_model, align_metadata = whisperx.load_align_model(
        language_code=LANGUAGE, device=DEVICE
    )

    log("[TRANSCRIBE] Running WhisperX transcription...")
    audio  = whisperx.load_audio(str(vocals_path))
    result = model.transcribe(audio, batch_size=16, language=LANGUAGE)

    log("[ALIGN] Running forced alignment...")
    result = whisperx.align(
        result["segments"], align_model, align_metadata, audio, DEVICE
    )

    # ── Word-level JSON ───────────────────────────────────────────────────────
    words = []
    for seg in result.get("segments", []):
        for w in seg.get("words", []):
            text = w.get("word", "").strip()
            if not text:
                continue
            words.append({
                "word":  text,
                "start": float(w.get("start", 0)),
                "end":   float(w.get("end", w.get("start", 0) + 0.2)),
            })

    words_path = outdir / f"{stem}_words.json"
    words_path.write_text(json.dumps(words, indent=2))
    log(f"[DONE] {len(words)} words → {words_path}")

    # ── Segment-level JSON ────────────────────────────────────────────────────
    segments = []
    for seg in result.get("segments", []):
        text = seg.get("text", "").strip()
        if not text:
            continue
        segments.append({
            "text":  text,
            "start": float(seg.get("start", 0)),
            "end":   float(seg.get("end",   0)),
        })

    segs_path = outdir / f"{stem}_segments.json"
    segs_path.write_text(json.dumps(segments, indent=2))
    log(f"[DONE] {len(segments)} segments → {segs_path}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("audio",
        help="Input audio or video file")
    parser.add_argument("--outdir", required=True,
        help="Output directory for all generated files")
    parser.add_argument("--mode", default="both",
        choices=["both", "separate", "transcribe"],
        help="Which stages to run (default: both)")
    parser.add_argument("--vocals",
        help="Skip separation — use this existing vocals WAV for transcription")
    args = parser.parse_args()

    audio_path = Path(args.audio)
    outdir     = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    vocals_path = None

    if args.mode in ("both", "separate"):
        vocals_path = run_separate(audio_path, outdir)

    if args.mode in ("both", "transcribe"):
        if vocals_path is None:
            if args.vocals:
                vocals_path = Path(args.vocals)
                log(f"[SEPARATE] Using provided vocals: {vocals_path}")
            else:
                # No separation — transcribe the original file directly
                vocals_path = audio_path
                log(f"[SEPARATE] No separation — transcribing original file")
        run_transcribe(vocals_path, outdir, audio_path.stem)

    if args.mode == "separate":
        log("[DONE] Separation complete")


if __name__ == "__main__":
    main()
