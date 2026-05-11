#!/usr/bin/env python3
"""
piper_tts.py — text-to-speech for Pop Maker Studio
Usage: piper_tts.py <text> <voice> <output.wav>

<voice> can be:
  - a Piper model name  (e.g. "en_US-amy-medium")
  - an absolute path to a .onnx model file
  - one of the built-in aliases: female, male, whisper, narrator

Piper is installed automatically on first run via pip.
"""

import sys
import os
import subprocess
import json
import urllib.request
import tempfile

VOICE_ALIASES = {
    "female":   "en_US-amy-medium",
    "male":     "en_US-ryan-medium",
    "whisper":  "en_US-lessac-medium",
    "narrator": "en_GB-alan-medium",
    "default":  "en_US-amy-medium",
}

PIPER_HF_BASE = "https://huggingface.co/rhasspy/piper-voices/resolve/main"


def ensure_piper():
    try:
        import piper
        return True
    except ImportError:
        pass
    print("PROGRESS 0.05", flush=True)
    rc = subprocess.call(
        [sys.executable, "-m", "pip", "install", "--quiet", "piper-tts"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    return rc == 0


def model_cache_dir():
    base = os.environ.get("XDG_CACHE_HOME",
                          os.path.join(os.path.expanduser("~"), ".cache"))
    d = os.path.join(base, "pop-maker-studio", "piper")
    os.makedirs(d, exist_ok=True)
    return d


def download_model(voice_id):
    """Download .onnx + .onnx.json for a piper voice. Returns (onnx_path, json_path)."""
    cache = model_cache_dir()
    onnx = os.path.join(cache, voice_id + ".onnx")
    cfg  = onnx + ".json"

    # Derive HuggingFace path: en_US-amy-medium → en/en_US/en_US-amy-medium/medium/
    parts  = voice_id.split("-")
    lang   = parts[0]                           # e.g. en_US
    lang2  = lang.split("_")[0]                 # e.g. en
    quality = parts[-1]                         # e.g. medium
    hf_dir = f"{lang2}/{lang}/{voice_id}/{quality}"

    for fname, local in [(voice_id + ".onnx", onnx), (voice_id + ".onnx.json", cfg)]:
        if not os.path.exists(local):
            url = f"{PIPER_HF_BASE}/{hf_dir}/{fname}"
            print(f"PROGRESS 0.10", flush=True)
            try:
                urllib.request.urlretrieve(url, local)
            except Exception as e:
                print(f"ERROR download failed: {e}", file=sys.stderr)
                sys.exit(1)

    return onnx, cfg


def synthesize_piper(text, onnx_path, cfg_path, output_wav):
    from piper import PiperVoice
    print("PROGRESS 0.40", flush=True)
    voice = PiperVoice.load(onnx_path, config_path=cfg_path)
    print("PROGRESS 0.60", flush=True)
    import wave
    with wave.open(output_wav, "wb") as wf:
        voice.synthesize(text, wf)
    print("PROGRESS 1.00", flush=True)


def main():
    if len(sys.argv) < 4:
        print("Usage: piper_tts.py <text> <voice> <output.wav>", file=sys.stderr)
        sys.exit(1)

    text       = sys.argv[1]
    voice      = sys.argv[2]
    output_wav = sys.argv[3]

    # Resolve alias
    voice = VOICE_ALIASES.get(voice, voice)

    # Absolute path to existing .onnx — use directly
    if os.path.isfile(voice) and voice.endswith(".onnx"):
        cfg = voice + ".json"
        if not os.path.exists(cfg):
            print(f"ERROR missing {cfg}", file=sys.stderr)
            sys.exit(1)
        onnx, cfg_path = voice, cfg
    else:
        # Named voice — download if needed
        onnx, cfg_path = download_model(voice)

    print("PROGRESS 0.20", flush=True)
    if not ensure_piper():
        print("ERROR could not install piper-tts", file=sys.stderr)
        sys.exit(1)

    print("PROGRESS 0.30", flush=True)
    synthesize_piper(text, onnx, cfg_path, output_wav)


if __name__ == "__main__":
    main()
