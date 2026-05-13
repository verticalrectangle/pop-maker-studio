#!/usr/bin/env python3
"""
Export an RVC V2 .pth model to ONNX for C++ inference.

Outputs two files next to the .pth:
  <model_dir>/synthesizer.onnx  — VITS synthesizer (phone + pitch → waveform)
  ~/.cache/pop-maker-studio/rvc/vec-768-layer-12.onnx  — ContentVec feature extractor (shared)

Usage:
  <venv>/bin/python rvc_export.py <path/to/model.pth>
  <venv>/bin/python rvc_export.py --all   # export all models in default cache dir
"""

import sys
import os
import urllib.request
import shutil
from pathlib import Path

VENV = "/home/alexis/dev/song2subs/venv/bin/python3"
VEC_URL = "https://huggingface.co/lj1995/VoiceConversionWebUI/resolve/main/vec-768-layer-12.onnx"
VEC_CACHE = Path.home() / ".cache" / "pop-maker-studio" / "rvc" / "vec-768-layer-12.onnx"

RVC_PKG = "/home/alexis/dev/song2subs/venv/lib/python3.11/site-packages/rvc_python"
sys.path.insert(0, str(Path(RVC_PKG).parent))


def ensure_vec_model():
    VEC_CACHE.parent.mkdir(parents=True, exist_ok=True)
    if VEC_CACHE.exists():
        print(f"[vec] already present: {VEC_CACHE}")
        return str(VEC_CACHE)
    print(f"[vec] downloading ContentVec ONNX from HuggingFace…")
    tmp = str(VEC_CACHE) + ".tmp"
    urllib.request.urlretrieve(VEC_URL, tmp)
    shutil.move(tmp, str(VEC_CACHE))
    print(f"[vec] saved: {VEC_CACHE}")
    return str(VEC_CACHE)


def export_model(pth_path: str):
    import torch
    from rvc_python.lib.infer_pack.models_onnx import SynthesizerTrnMsNSFsidM

    pth_path = Path(pth_path)
    out_path = pth_path.parent / "synthesizer.onnx"

    if out_path.exists():
        print(f"[synth] already exported: {out_path}")
        return str(out_path)

    print(f"[synth] loading weights from {pth_path.name} …")
    ckpt = torch.load(str(pth_path), map_location="cpu", weights_only=False)

    cfg      = ckpt["config"]
    weights  = ckpt["weight"]
    sr       = ckpt.get("sr", cfg[-1] if len(cfg) >= 19 else 40000)
    version  = ckpt.get("version", "v2")
    f0       = bool(ckpt.get("f0", 1))

    # Unpack config (19-element list for RVC V2)
    # [spec_ch, seg_sz, inter_ch, hidden_ch, filter_ch, n_heads, n_layers,
    #  kernel_sz, p_dropout, resblock, rb_kernel_sz, rb_dilation_sz,
    #  up_rates, up_init_ch, up_kernel_sz, gin_ch, spk_embed_dim, sr]
    (spec_channels, segment_size, inter_channels, hidden_channels,
     filter_channels, n_heads, n_layers, kernel_size, p_dropout,
     resblock, resblock_kernel_sizes, resblock_dilation_sizes,
     upsample_rates, upsample_initial_channel, upsample_kernel_sizes,
     gin_channels, emb_channels, _sr) = cfg[:18]

    n_speakers = weights.get("emb_g.weight", torch.zeros(1, 1)).shape[0]

    print(f"[synth] sr={sr}  version={version}  f0={f0}  n_speakers={n_speakers}")

    net = SynthesizerTrnMsNSFsidM(
        spec_channels,
        segment_size,
        inter_channels,
        hidden_channels,
        filter_channels,
        n_heads,
        n_layers,
        kernel_size,
        p_dropout,
        resblock,
        resblock_kernel_sizes,
        resblock_dilation_sizes,
        upsample_rates,
        upsample_initial_channel,
        upsample_kernel_sizes,
        spk_embed_dim=n_speakers,
        gin_channels=gin_channels,
        sr=sr,
        use_f0=f0,
    )
    net.eval()

    # Load weights (allow missing keys for discriminator weights not saved in .pth)
    result = net.load_state_dict(weights, strict=False)
    print(f"[synth] loaded. Missing={len(result.missing_keys)}  Unexpected={len(result.unexpected_keys)}")

    # Dummy inputs for export — shape: [batch, seq_len, feature_dim]
    T = 100  # frames
    phone      = torch.randn(1, T, 768)       # contentvec features (768-dim)
    phone_len  = torch.tensor([T], dtype=torch.int64)
    pitch      = torch.zeros(1, T, dtype=torch.int64)   # f0 quantized [0..255]
    nsff0      = torch.zeros(1, T)            # f0 in Hz
    sid        = torch.zeros(1, dtype=torch.int64)       # speaker id
    rnd        = torch.randn(1, 192, T)       # latent noise

    input_names  = ["phone", "phone_lengths", "pitch", "nsff0", "sid", "rnd"]
    output_names = ["audio"]
    dynamic_axes = {
        "phone":        {1: "T"},
        "phone_lengths":{0: "batch"},
        "pitch":        {1: "T"},
        "nsff0":        {1: "T"},
        "rnd":          {2: "T"},
        "audio":        {2: "time"},
    }

    print(f"[synth] exporting to {out_path} …")
    with torch.no_grad():
        torch.onnx.export(
            net,
            (phone, phone_len, pitch, nsff0, sid, rnd),
            str(out_path),
            input_names=input_names,
            output_names=output_names,
            dynamic_axes=dynamic_axes,
            opset_version=17,
            do_constant_folding=True,
        )
    print(f"[synth] done → {out_path}")
    return str(out_path)


def main():
    rvc_cache = Path.home() / ".cache" / "pop-maker-studio" / "rvc"

    if len(sys.argv) == 2 and sys.argv[1] == "--all":
        pth_files = list(rvc_cache.rglob("*.pth"))
    elif len(sys.argv) == 2:
        pth_files = [Path(sys.argv[1])]
    else:
        print(__doc__)
        sys.exit(1)

    ensure_vec_model()

    for pth in pth_files:
        try:
            export_model(str(pth))
        except Exception as e:
            print(f"[error] {pth}: {e}", file=sys.stderr)


if __name__ == "__main__":
    main()
