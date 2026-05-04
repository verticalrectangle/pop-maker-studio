#!/usr/bin/env python3
import sys, json
try:
    import librosa, numpy as np
    audio_path  = sys.argv[1]
    output_path = sys.argv[2]
    hop = 512
    y, sr = librosa.load(audio_path, sr=None, mono=True)
    rms = librosa.feature.rms(y=y, frame_length=2048, hop_length=hop)[0]
    peak = rms.max()
    if peak > 0:
        rms = rms / peak
    fps = sr / hop
    with open(output_path, "w") as f:
        json.dump({"fps": float(fps), "rms": rms.tolist()}, f)
    print("DONE")
except Exception as e:
    print(f"ERROR: {e}")
    sys.exit(1)
