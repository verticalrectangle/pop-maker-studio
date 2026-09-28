#!/usr/bin/env python3
"""Parity check: C++ separate_stems4 vs PyTorch apply_model on real audio.

Runs the PMS C++ runtime (build/separate4_cli, which links src/separate4.cpp
against build/models/htdemucs.onnx) and the reference PyTorch pipeline
``apply_model(model, mix, shifts=0, split=True, overlap=0.25)`` on two
excerpts, then reports per-stem SNR of C++ vs PyTorch output:

  1. clip.wav (song 1:28-2:03, the 35 s reference clip), first 30 s
  2. the song 0:30-1:00 (second 30 s excerpt, decoded from the FLAC)

Acceptance: per-stem SNR >= 30 dB on both excerpts. Also reports runtime
per minute of audio and peak RSS of the C++ run.

Usage:
    tools/separate4_parity.py [--clip PATH] [--song PATH] [--work DIR]

Requires: ffmpeg, build/separate4_cli, build/models/htdemucs.onnx.
"""
import argparse
import os
import resource
import subprocess
import sys
import time

import numpy as np
import soundfile as sf
import torch

SR = 44100
SOURCES = ["drums", "bass", "other", "vocals"]
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def decode_wav(path, offset=0.0, dur=30.0):
    """Decode [offset, offset+dur) as 44.1 kHz stereo float32 via ffmpeg."""
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error",
           "-ss", str(offset), "-i", "file:" + path, "-t", str(dur),
           "-vn", "-ar", str(SR), "-ac", "2", "-f", "f32le", "pipe:1"]
    raw = subprocess.run(cmd, stdout=subprocess.PIPE, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).reshape(-1, 2)


def load_htdemucs():
    from demucs.pretrained import get_model
    model = get_model("htdemucs")
    if hasattr(model, "models"):
        assert len(model.models) == 1
        model = model.models[0]
    model.eval()
    return model


def torch_stems(model, mix):
    # Reference = what users get from `python -m demucs`: Separator wraps
    # apply_model with mono-mix mean/std normalisation (api.py
    # separate_tensor: ref = wav.mean(0), out = model((wav-mean)/std)*std+mean).
    # C++ separate_stems4 applies the same normalisation, so this compares
    # like with like; raw apply_model() without it differs by ~55-60 dB SNR.
    from demucs.api import Separator
    sep = Separator(model="htdemucs", shifts=0, split=True, overlap=0.25,
                    progress=False)
    with torch.no_grad():
        _, stems = sep.separate_tensor(torch.from_numpy(mix.T).contiguous())
    return torch.stack([stems[s] for s in SOURCES], 0).permute(0, 2, 1).numpy()


def cpp_stems(cli, work, tag, mix):
    wav_path = os.path.join(work, f"{tag}.wav")
    out_dir = os.path.join(work, f"{tag}_cpp")
    sf.write(wav_path, mix, SR, subtype="FLOAT")
    t0 = time.perf_counter()
    p = subprocess.run([cli, wav_path, out_dir],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    dt = time.perf_counter() - t0
    if p.returncode != 0:
        raise RuntimeError(f"separate4_cli failed: {p.stderr.decode()[-2000:]}")
    stems = []
    for s in SOURCES:
        d, sr = sf.read(os.path.join(out_dir, s + ".wav"), dtype="float32",
                        always_2d=True)
        assert sr == SR, (s, sr)
        stems.append(d)
    return np.stack(stems), dt


def snr_db(got, want):
    num = np.abs(got - want) ** 2
    den = np.maximum(np.abs(want) ** 2, 1e-12)
    with np.errstate(divide="ignore"):
        return float(10 * np.log10(den.sum() / num.sum()))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--clip", default=os.path.expanduser(
        "~/Projects/seen-and-not-seen/public/audio/clip.wav"))
    ap.add_argument("--song", default=os.path.expanduser(
        "/home/alexis/Soulseek Downloads/complete/"
        "06. Seen and Not Seen (2005 Remaster).flac"))
    ap.add_argument("--work", default="/tmp/separate4_parity")
    args = ap.parse_args()

    cli = os.path.join(ROOT, "build", "separate4_cli")
    assert os.path.isfile(cli), f"build {cli} first"
    assert os.path.isfile(os.path.join(ROOT, "build", "models", "htdemucs.onnx")), \
        "link htdemucs.onnx into build/models/"
    os.makedirs(args.work, exist_ok=True)

    model = load_htdemucs()
    cases = [("clip0-30", args.clip, 0.0), ("song30-60", args.song, 30.0)]
    worst, worst_id = 1e9, ""
    total_audio, total_cpp = 0.0, 0.0
    peak_rss = 0.0
    for tag, path, off in cases:
        mix = decode_wav(path, off, 30.0)
        n = len(mix)
        total_audio += n / SR
        print(f"[{tag}] {n / SR:.1f} s from {os.path.basename(path)}@{off:.0f}s",
              flush=True)
        want = torch_stems(model, mix)
        got, dt = cpp_stems(cli, args.work, tag, mix)
        total_cpp += dt
        peak_rss = max(peak_rss,
                       resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)
        assert got.shape == want.shape, (got.shape, want.shape)
        print(f"  C++ time: {dt:.1f} s ({n / SR / dt * 60:.1f}x "
              f"faster than realtime)", flush=True)
        for i, s in enumerate(SOURCES):
            v = snr_db(got[i], want[i])
            print(f"  {s:7s} SNR {v:6.1f} dB", flush=True)
            if v < worst:
                worst, worst_id = v, f"{tag}/{s}"
    print(f"worst: {worst:.1f} dB ({worst_id})")
    print(f"runtime per minute of audio: "
          f"{total_cpp / total_audio * 60:.1f} s/min")
    print(f"peak RSS (children): {peak_rss / 1024:.0f} MB")
    if worst < 30:
        print("FAIL: per-stem SNR < 30 dB", file=sys.stderr)
        return 1
    print("PASS: all stems >= 30 dB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
