#!/usr/bin/env python3
"""
noise_reduce.py — Room noise cleanup using noisereduce (spectral gating).

Called by Pop Maker Studio before transcription to improve WhisperX accuracy
on rough recordings (room hum, AC noise, mic self-noise).

Usage:
  python noise_reduce.py --input <audio_path> --output <out_path>

Progress is written as JSON lines: {"progress": 0.5}
Done:                               {"done": true, "output": "<path>"}
Error:                              {"error": "<msg>"}
"""

import sys
import json
import argparse
import traceback

def emit(obj):
    print(json.dumps(obj), flush=True)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input",  required=True)
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    try:
        import numpy as np
        emit({"progress": 0.05})

        import soundfile as sf
        emit({"progress": 0.10})

        import noisereduce as nr
        emit({"progress": 0.15})

        data, rate = sf.read(args.input, always_2d=True)
        emit({"progress": 0.30})

        # noisereduce works per-channel; mix to mono for noise profile estimation
        # then apply to all channels independently
        mono = data.mean(axis=1)

        # Stationary noise reduction — estimates noise from the whole file.
        # prop_decrease=0.75 is conservative: removes most room noise without
        # coloring the voice. n_std_thresh_stationary=1.5 keeps transients.
        reduced_channels = []
        n_ch = data.shape[1]
        for i in range(n_ch):
            emit({"progress": 0.30 + 0.55 * (i / n_ch)})
            reduced = nr.reduce_noise(
                y=data[:, i],
                sr=rate,
                y_noise=mono,
                stationary=True,
                prop_decrease=0.75,
                n_std_thresh_stationary=1.5,
            )
            reduced_channels.append(reduced)

        emit({"progress": 0.90})
        out = np.stack(reduced_channels, axis=1)
        sf.write(args.output, out, rate)
        emit({"progress": 1.0})
        emit({"done": True, "output": args.output})

    except Exception as e:
        emit({"error": traceback.format_exc()})
        sys.exit(1)

if __name__ == "__main__":
    main()
