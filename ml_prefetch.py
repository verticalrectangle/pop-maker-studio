#!/usr/bin/env python3
"""
Pop Maker Studio — model prefetch script.
Downloads Whisper and Demucs weights on first run.
Prints progress lines that the host app parses.
"""
import sys, os

def log(msg):
    print(msg, flush=True)

log("STAGE:whisper")
log("Downloading Whisper large-v2 model…")
try:
    import whisper
    whisper.load_model("large-v2")
    log("OK:whisper")
except Exception as e:
    log(f"ERROR:whisper:{e}")
    sys.exit(1)

log("STAGE:demucs")
log("Downloading Demucs htdemucs model…")
try:
    from demucs.pretrained import get_model
    get_model("htdemucs")
    log("OK:demucs")
except Exception as e:
    log(f"ERROR:demucs:{e}")
    sys.exit(1)

log("DONE")
