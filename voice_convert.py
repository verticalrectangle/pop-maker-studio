#!/usr/bin/env python3
"""
voice_convert.py — RVC V2 voice conversion for Pop Maker Studio

Uses torchaudio (HuBERT) for feature extraction — no fairseq dependency.
Python interpreter must be the song2subs venv which has torch/torchaudio/pyworld.

Usage: voice_convert.py <input.wav> <model.pth> <output.wav>

Progress is printed as:  PROGRESS 0.45
Errors as:               ERROR <message>
"""

import sys, os

# Add song2subs venv site-packages so we get torch/torchaudio/pyworld/librosa
VENV_SITE = "/home/alexis/dev/song2subs/venv/lib/python3.11/site-packages"
if VENV_SITE not in sys.path:
    sys.path.insert(0, VENV_SITE)

import types

# Stub only the broken sub-modules so the package itself remains a real package.
# fairseq 0.12.2 fails on Python 3.11 dataclasses; stub checkpoint_utils only.
import sys as _sys

def _make_stub(name):
    m = types.ModuleType(name)
    _sys.modules[name] = m
    return m

# Pre-stub fairseq hierarchy to prevent the dataclass error
for _s in ("fairseq", "fairseq.checkpoint_utils",
           "fairseq.distributed", "fairseq.dataclass",
           "fairseq.dataclass.configs", "fairseq.logging"):
    if _s not in _sys.modules:
        _make_stub(_s)

# Pre-stub the rvc_python sub-modules that import fairseq
for _s in ("rvc_python.infer",
           "rvc_python.modules",
           "rvc_python.modules.vc",
           "rvc_python.modules.vc.modules",
           "rvc_python.modules.vc.utils"):
    if _s not in _sys.modules:
        _make_stub(_s)

import numpy as np
import torch
import soundfile as sf
import librosa

def progress(p): print(f"PROGRESS {p:.2f}", flush=True)
def error(msg):  print(f"ERROR {msg}", flush=True)


# ── HuBERT feature extraction (via torchaudio, no fairseq) ───────────────────

_hubert_model = None

def get_hubert(device):
    global _hubert_model
    if _hubert_model is None:
        import torchaudio
        bundle = torchaudio.pipelines.HUBERT_BASE
        _hubert_model = bundle.get_model().to(device).eval()
    return _hubert_model

@torch.no_grad()
def extract_features(wav16k_np, device):
    """wav16k_np: 1D float32 numpy at 16 kHz → tensor [1, T', 768]"""
    model = get_hubert(device)
    waveform = torch.from_numpy(wav16k_np).unsqueeze(0).to(device)
    features, _ = model.extract_features(waveform)
    return features[-1]   # last layer, [1, T', 768]


# ── F0 extraction via pyworld ─────────────────────────────────────────────────

def extract_f0(wav_np, sr, n_frames, up_key=0):
    """Returns (f0_coarse [n_frames], f0_bak [n_frames]) at the requested length."""
    import pyworld as pw

    f0_min, f0_max = 50.0, 1100.0
    frame_period = 1000.0 * 160.0 / sr   # 160 samples per frame at sr

    f0, t = pw.dio(wav_np.astype(np.float64), sr,
                   f0_floor=f0_min, f0_ceil=f0_max,
                   frame_period=frame_period)
    f0 = pw.stonemask(wav_np.astype(np.float64), f0, t, sr)
    f0 *= 2 ** (up_key / 12.0)

    # Quantise into [1..255] for the pitch embedding
    f0_mel_min = 1127 * np.log(1 + f0_min / 700)
    f0_mel_max = 1127 * np.log(1 + f0_max / 700)
    f0_mel = 1127 * np.log(1 + f0 / 700)
    f0_mel[f0_mel > 0] = (f0_mel[f0_mel > 0] - f0_mel_min) * 254 / (
        f0_mel_max - f0_mel_min) + 1
    f0_mel = np.clip(f0_mel, 1, 255)
    f0_coarse = np.rint(f0_mel).astype(np.int64)
    f0_bak    = f0.astype(np.float32)

    # Trim/pad to n_frames
    def fit(a, n, pad_val=0):
        if len(a) >= n: return a[:n]
        return np.pad(a, (0, n - len(a)), constant_values=pad_val)

    return fit(f0_coarse, n_frames), fit(f0_bak, n_frames, 0.0)


# ── VITS synthesizer ──────────────────────────────────────────────────────────

def load_synthesizer(pth_path, device):
    # Import model architecture without triggering rvc_python/__init__.py
    # (which imports rvc_python.infer → fairseq, broken on Python 3.11)
    import importlib.util, types

    def _load_mod(rel_path, name):
        full = os.path.join(VENV_SITE, "rvc_python", rel_path)
        spec = importlib.util.spec_from_file_location(name, full)
        mod  = importlib.util.module_from_spec(spec)
        sys.modules[name] = mod
        spec.loader.exec_module(mod)
        return mod

    # Load dependency chain without touching fairseq
    if "rvc_python.lib.infer_pack.commons" not in sys.modules:
        _load_mod("lib/infer_pack/commons.py",   "rvc_python.lib.infer_pack.commons")
    if "rvc_python.lib.infer_pack.transforms" not in sys.modules:
        _load_mod("lib/infer_pack/transforms.py","rvc_python.lib.infer_pack.transforms")
    if "rvc_python.lib.infer_pack.attentions" not in sys.modules:
        _load_mod("lib/infer_pack/attentions.py","rvc_python.lib.infer_pack.attentions")
    if "rvc_python.lib.infer_pack.modules" not in sys.modules:
        _load_mod("lib/infer_pack/modules.py",   "rvc_python.lib.infer_pack.modules")
    if "rvc_python.lib.infer_pack.models_onnx" not in sys.modules:
        _load_mod("lib/infer_pack/models_onnx.py","rvc_python.lib.infer_pack.models_onnx")

    SynthesizerTrnMsNSFsidM = sys.modules["rvc_python.lib.infer_pack.models_onnx"].SynthesizerTrnMsNSFsidM

    ckpt    = torch.load(pth_path, map_location="cpu", weights_only=False)
    cfg     = ckpt["config"]
    weights = ckpt["weight"]
    sr      = int(ckpt.get("sr", cfg[-1]))
    use_f0  = bool(ckpt.get("f0", 1))

    # Config layout (RVC V2, 18 elements):
    # [spec_ch, seg_sz, inter_ch, hidden_ch, filter_ch, n_heads, n_layers,
    #  kernel_sz, p_dropout, resblock, rb_kernels, rb_dilations,
    #  up_rates, up_init_ch, up_kernels, spk_embed_dim, gin_channels, sr]
    (spec_channels, segment_size, inter_channels, hidden_channels,
     filter_channels, n_heads, n_layers, kernel_size, p_dropout,
     resblock, resblock_kernel_sizes, resblock_dilation_sizes,
     upsample_rates, upsample_initial_channel, upsample_kernel_sizes,
     spk_embed_dim_cfg, gin_channels, _sr) = cfg[:18]

    n_speakers = weights.get("emb_g.weight", torch.zeros(spk_embed_dim_cfg, 1)).shape[0]

    version = ckpt.get("version", "v2")
    net = SynthesizerTrnMsNSFsidM(
        spec_channels, segment_size, inter_channels, hidden_channels,
        filter_channels, n_heads, n_layers, kernel_size, p_dropout,
        resblock, resblock_kernel_sizes, resblock_dilation_sizes,
        upsample_rates, upsample_initial_channel, upsample_kernel_sizes,
        spk_embed_dim=n_speakers, gin_channels=gin_channels,
        sr=sr, version=version, is_half=False,
    )
    net.load_state_dict(weights, strict=False)
    return net.to(device).eval(), sr, use_f0


# ── Main conversion pipeline ──────────────────────────────────────────────────

def convert(input_wav, model_pth, output_wav, f0_up_key=0):
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    progress(0.05)

    wav, src_sr = librosa.load(input_wav, sr=None, mono=True)
    progress(0.10)

    synth, target_sr, use_f0 = load_synthesizer(model_pth, device)
    progress(0.25)

    # 16 kHz mono for HuBERT
    wav16k = librosa.resample(wav, orig_sr=src_sr, target_sr=16000)

    # Extract HuBERT features → [1, T', 768]
    feats = extract_features(wav16k.astype(np.float32), device)
    progress(0.50)

    # RVC doubles the frame rate: repeat each frame to go from 50fps → 100fps
    feats = feats.repeat_interleave(2, dim=1)   # [1, 2T', 768]
    T = feats.shape[1]
    feats_len = torch.tensor([T], dtype=torch.int64, device=device)

    # F0 at 100fps (hop=160 at target_sr ≈ target_sr/100 samples per frame)
    if use_f0:
        wav_tgt = librosa.resample(wav, orig_sr=src_sr, target_sr=target_sr)
        f0_coarse, f0_bak = extract_f0(wav_tgt, target_sr, T, up_key=f0_up_key)
        pitch = torch.from_numpy(f0_coarse).unsqueeze(0).to(device)
        nsff0 = torch.from_numpy(f0_bak).unsqueeze(0).to(device)
    else:
        pitch = torch.zeros(1, T, dtype=torch.int64, device=device)
        nsff0 = torch.zeros(1, T, device=device)

    progress(0.65)

    sid = torch.zeros(1, dtype=torch.int64, device=device)
    rnd = torch.randn(1, 192, T, device=device)

    with torch.no_grad():
        audio_out = synth.forward(feats, feats_len, pitch, nsff0, sid, rnd)

    progress(0.90)

    audio_np = audio_out.squeeze().cpu().float().numpy()
    if target_sr != 44100:
        audio_np = librosa.resample(audio_np, orig_sr=target_sr, target_sr=44100)

    sf.write(output_wav, audio_np, 44100)
    progress(1.00)


def main():
    if len(sys.argv) < 4:
        print("Usage: voice_convert.py <input.wav> <model.pth> <output.wav>",
              file=sys.stderr)
        sys.exit(1)

    try:
        convert(sys.argv[1], sys.argv[2], sys.argv[3])
    except Exception as e:
        import traceback
        error(str(e))
        traceback.print_exc(file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
