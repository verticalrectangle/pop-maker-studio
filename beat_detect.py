#!/usr/bin/env python3
import sys, json
try:
    import librosa, numpy as np
    audio_path  = sys.argv[1]
    output_path = sys.argv[2]
    y, sr = librosa.load(audio_path, sr=None, mono=True)
    tempo, beat_frames = librosa.beat.beat_track(y=y, sr=sr)
    beat_times = librosa.frames_to_time(beat_frames, sr=sr).tolist()
    with open(output_path, "w") as f:
        json.dump({"bpm": float(tempo), "beats": beat_times}, f)
    print("DONE")
except Exception as e:
    print(f"ERROR: {e}")
    sys.exit(1)
