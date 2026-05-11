#!/usr/bin/env python3
"""
voice_convert.py — AI voice conversion for Pop Maker Studio
Usage: voice_convert.py <input.wav> <model_path_or_preset> <output.wav>

<model_path_or_preset> can be:
  - path to an RVC .pth model file
  - "rvc:<huggingface_model_id>" to auto-download a public RVC model
  - (future) other voice conversion backends

Requires: pip install rvc-python  (auto-installed on first run)
"""

import sys
import os
import subprocess
import tempfile
import shutil


def ensure_rvc():
    try:
        import rvc
        return True
    except ImportError:
        pass
    print("PROGRESS 0.05", flush=True)
    rc = subprocess.call(
        [sys.executable, "-m", "pip", "install", "--quiet", "rvc-python"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    return rc == 0


def convert_with_rvc(input_wav, model_path, output_wav):
    from rvc import Config, load_hubert, get_vc, rvc_infer
    print("PROGRESS 0.20", flush=True)

    cfg     = Config()
    hubert  = load_hubert(cfg)
    print("PROGRESS 0.40", flush=True)

    cpt, version, net_g, tgt_sr, vc = get_vc(model_path, cfg, hubert)
    print("PROGRESS 0.60", flush=True)

    rvc_infer(
        index_file  = "",
        index_rate  = 0,
        input_path  = input_wav,
        output_path = output_wav,
        pitch_change= 0,
        f0_method   = "rmvpe",
        cpt=cpt, version=version, net_g=net_g, filter_radius=3,
        tgt_sr=tgt_sr, rms_mix_rate=0.25, protect=0.33,
        crepe_hop_length=128, vc=vc, hubert_model=hubert,
    )
    print("PROGRESS 1.00", flush=True)


def fallback_convert(input_wav, output_wav):
    """Fallback: simple formant shift via sox or ffmpeg if RVC unavailable."""
    import shutil
    # Try sox pitch shift as a minimal fallback
    if shutil.which("sox"):
        rc = subprocess.call(
            ["sox", input_wav, output_wav, "pitch", "200"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        if rc == 0:
            print("PROGRESS 1.00", flush=True)
            return
    # ffmpeg rubberband
    if shutil.which("ffmpeg"):
        rc = subprocess.call(
            ["ffmpeg", "-y", "-i", input_wav,
             "-af", "asetrate=44100*1.2,aresample=44100",
             output_wav],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        if rc == 0:
            print("PROGRESS 1.00", flush=True)
            return
    print("ERROR No voice conversion backend available. Install rvc-python or sox.",
          file=sys.stderr)
    sys.exit(1)


def main():
    if len(sys.argv) < 4:
        print("Usage: voice_convert.py <input.wav> <model> <output.wav>", file=sys.stderr)
        sys.exit(1)

    input_wav  = sys.argv[1]
    model      = sys.argv[2]
    output_wav = sys.argv[3]

    print("PROGRESS 0.02", flush=True)

    if not os.path.exists(input_wav):
        print(f"ERROR input not found: {input_wav}", file=sys.stderr)
        sys.exit(1)

    # Try RVC
    if ensure_rvc():
        if model.startswith("rvc:"):
            # Auto-download from HuggingFace
            from huggingface_hub import hf_hub_download
            repo_id = model[4:]
            cache   = os.path.join(os.path.expanduser("~"), ".cache",
                                   "pop-maker-studio", "rvc")
            os.makedirs(cache, exist_ok=True)
            model = hf_hub_download(repo_id=repo_id, filename="model.pth",
                                    cache_dir=cache)

        if not os.path.isfile(model):
            print(f"ERROR model not found: {model}", file=sys.stderr)
            fallback_convert(input_wav, output_wav)
            return

        convert_with_rvc(input_wav, model, output_wav)
    else:
        print("WARNING rvc-python not available — using fallback", file=sys.stderr)
        fallback_convert(input_wav, output_wav)


if __name__ == "__main__":
    main()
