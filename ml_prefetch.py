#!/usr/bin/env python3
"""
Pop Maker Studio — model prefetch script.
Downloads faster-whisper large-v3 and Demucs htdemucs weights.
Prints structured progress lines parsed by the host app.
"""
import sys

def log(msg):
    print(msg, flush=True)

log("STAGE:whisper")
log("Downloading faster-whisper large-v3…")
try:
    from faster_whisper import WhisperModel
    WhisperModel("large-v3", device="cpu", compute_type="int8")
    log("OK:whisper")
except Exception as e:
    log(f"ERROR:whisper:{e}")
    sys.exit(1)

log("STAGE:demucs")
log("Downloading Demucs htdemucs…")
try:
    from demucs.pretrained import get_model
    get_model("htdemucs")
    log("OK:demucs")
except Exception as e:
    log(f"ERROR:demucs:{e}")
    sys.exit(1)

log("DONE")
