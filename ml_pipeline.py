#!/usr/bin/env python3
"""
Lyric Video Blender — ML pipeline
Called as a subprocess with the venv Python (not Blender's Python).
Runs Demucs vocal separation + WhisperX forced-alignment transcription.
Progress is written to stderr. Word JSON is written to --output path.
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("audio", help="Input audio/video file")
    parser.add_argument("--output", required=True, help="Output JSON path for word list")
    parser.add_argument("--vocals", help="Skip separation, use this vocals WAV directly")
    parser.add_argument("--vocals-out", dest="vocals_out",
                        help="Copy the vocals WAV to this path after separation")
    args = parser.parse_args()

    audio_path = Path(args.audio)
    out_path   = Path(args.output)
    tmpdir     = None

    try:
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

        if args.vocals:
            vocals_path = Path(args.vocals)
            log(f"[SEPARATE] Using provided vocals: {vocals_path}")
        else:
            log("[SEPARATE] Running Demucs (vocals only stem)...")
            tmpdir  = tempfile.mkdtemp(prefix="lvb_")
            sep_out = Path(tmpdir) / "sep"
            subprocess.run(
                [
                    sys.executable, "-m", "demucs",
                    "--two-stems=vocals",
                    "-n", DEMUCS_MODEL,
                    "-o", str(sep_out),
                    str(audio_path),
                ],
                check=True,
                stderr=sys.stderr,
            )
            stem        = audio_path.stem
            vocals_path = sep_out / DEMUCS_MODEL / stem / "vocals.wav"
            if not vocals_path.exists():
                matches = list((sep_out / DEMUCS_MODEL).rglob("vocals.wav"))
                if not matches:
                    log(f"[ERROR] Vocals WAV not found under {sep_out}")
                    sys.exit(1)
                vocals_path = matches[0]
            log(f"[SEPARATE] Vocals: {vocals_path}")

        if args.vocals_out:
            dest = Path(args.vocals_out)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(str(vocals_path), str(dest))
            log(f"[SEPARATE] Saved vocals to {dest}")

        log("[TRANSCRIBE] Running WhisperX transcription...")
        audio  = whisperx.load_audio(str(vocals_path))
        result = model.transcribe(audio, batch_size=16, language=LANGUAGE)

        log("[ALIGN] Running forced alignment...")
        result = whisperx.align(
            result["segments"], align_model, align_metadata, audio, DEVICE
        )

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

        out_path.write_text(json.dumps(words, indent=2))
        log(f"[DONE] {len(words)} words written to {out_path}")

    finally:
        if tmpdir:
            shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == "__main__":
    main()
