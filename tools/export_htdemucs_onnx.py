#!/usr/bin/env python3
"""Export the htdemucs checkpoint to ONNX with the spectrogram front/back end
outside the graph: the C++ runtime (src/separate4.cpp) does STFT (FFTW) ->
ONNX -> iSTFT, exactly like sevagh/demucs.onnx.

What gets exported
------------------
The HTDemucs core as a pure real-valued graph::

    spec_cac [1, 4, 2048, F]   complex-as-channels spectrogram of the
                               reflect-padded segment (C++ does _spec:
                               reflect-pad time by 1536, STFT n_fft=4096,
                               hop=1024, periodic Hann, normalized=True,
                               center=True reflect, drop Nyquist bin and
                               guard frames 0/1 and F+2/F+3)
      -> freq-branch normalisation (mean/std of THIS segment, computed
         inside the graph exactly as forward() does)
      + mix [1, 2, T]          raw waveform segment (model applies its own
         time normalisation inside the graph; T = 343980 samples)
      -> HTDemucs core (spec branch + time branch + cross-transformer,
         denormalised by the same frozen mean/std)
      -> zout [1, 4, 2, 2048, F]  complex-as-channels output spectrogram
         (CaC: network output IS the complex spectrogram; C++ does _mask
         reshape + _ispec: pad freq to 2049, pad time guards, iSTFT,
         strip 1536-sample pad)

F = 336 time frames for the single legal length T = 343980 samples
(segment=39/5 s at 44.1 kHz; valid_length() pads anything shorter and
raises past it, so the graph has exactly one shape).
Output: stems-waveform is NOT in the graph; C++ finishes the job.
Inputs:  mix [1, 2, 343980] float32, spec [1, 4, 2048, 336] float32.
Output:  zout [1, 4, 2, 2048, 336] float32 (CaC spectrogram per source).

Trace input: the first 343980 samples of clip.wav (real music, 44.1 kHz
stereo), so LayerNorm-free statistics recorded in the trace match audio.
The transformer carries cached sparse masks sized to the trace-time
sequence length, hence the single fixed shape.

Usage:
    export_htdemucs_onnx.py [--out PATH] [--opset N]
        [--calib PATH] [--check-seconds S]

Conversion command (reference; the committed htdemucs.onnx was built this way):
    ~/Projects/seen-and-not-seen/.venv/bin/python tools/export_htdemucs_onnx.py \
        --out /home/alexis/dev/pms-wt/shared-models/htdemucs.onnx

Source checkpoint: the `htdemucs` bag of the demucs 4.1.0 Python package
(MIT licence), single model signature 955717e8, downloaded from
https://dl.fbaipublicfiles.com/demucs/hybrid_transformer/955717e8-8726e21a.th
via torch.hub (cached at ~/.cache/torch/hub/checkpoints/).

Size: ~340 MB (float32, 4 sources x hybrid conv-transformer).
Licence: MIT (Meta Demucs package, LICENSE file in the demucs repo).
"""
import argparse
import math
import os
import sys

import torch
from torch.nn import functional as F


N_FFT = 4096
HOP = N_FFT // 4
SPEC_PAD = HOP // 2 * 3          # 1536: HTDemucs._spec reflect pad
SEGMENT = 39 / 5                 # htdemucs segment (s); training_length below
SR = 44100
TRAIN_LEN = int(SEGMENT * SR)    # 343980 samples; the single graph length
SOURCES = ["drums", "bass", "other", "vocals"]


class HTDemucsCore(torch.nn.Module):
    """HTDemucs core as a pure real-valued ONNX graph.

    spec [B, 2*C, Fq, T] is the complex-as-channels spectrogram the C++
    runtime builds with _spec (reflect-pad, STFT, guard-frame slice);
    mix [B, C, L] is the raw waveform segment (time-branch input).
    Returns (zout, xt): zout [B, S, C, Fq, T, 2] (dim 2 of size C holds
    re0,im0,re1,im1; last dim is the (real, imag) pair from
    _mask's interleave, kept real for ONNX) which the C++ runtime turns
    complex and runs through _ispec; xt [B, S, C, L] is the denormalised
    time-branch waveform the C++ runtime adds to the iSTFT output.
    Everything else mirrors HTDemucs.forward in eval.
    """

    def __init__(self, model: torch.nn.Module):
        super().__init__()
        self.model = model
        nfft, hl = model.nfft, model.hop_length
        assert nfft == N_FFT and hl == HOP, (nfft, hl)
        assert model.cac and model.wiener_iters == 0
        assert int(model.segment * model.samplerate) == TRAIN_LEN, (
            model.segment, TRAIN_LEN)

    def forward(self, mix, mag):
        from einops import rearrange
        model = self.model
        # --- freq branch normalisation (this segment's own stats) ---
        x = mag
        B, C, Fq, T = x.shape
        mean = x.mean(dim=(1, 2, 3), keepdim=True)
        std = x.std(dim=(1, 2, 3), keepdim=True)
        x = (x - mean) / (1e-5 + std)
        # --- time branch input ---
        xt = mix
        meant = xt.mean(dim=(1, 2), keepdim=True)
        stdt = xt.std(dim=(1, 2), keepdim=True)
        xt = (xt - meant) / (1e-5 + stdt)

        saved, saved_t, lengths, lengths_t = [], [], [], []
        for idx, encode in enumerate(model.encoder):
            lengths.append(x.shape[-1])
            inject = None
            if idx < len(model.tencoder):
                lengths_t.append(xt.shape[-1])
                tenc = model.tencoder[idx]
                xt = tenc(xt)
                if not tenc.empty:
                    saved_t.append(xt)
                else:
                    inject = xt
            x = encode(x, inject)
            if idx == 0 and model.freq_emb is not None:
                frs = torch.arange(x.shape[-2], device=x.device)
                emb = model.freq_emb(frs).t()[None, :, :, None].expand_as(x)
                x = x + model.freq_emb_scale * emb
            saved.append(x)
        if model.bottom_channels:
            b, c, f, t = x.shape
            x = rearrange(x, "b c f t-> b c (f t)")
            x = model.channel_upsampler(x)
            x = rearrange(x, "b c (f t)-> b c f t", f=f)
            xt = model.channel_upsampler_t(xt)
        x, xt = model.crosstransformer(x, xt)
        if model.bottom_channels:
            x = rearrange(x, "b c f t-> b c (f t)")
            x = model.channel_downsampler(x)
            x = rearrange(x, "b c (f t)-> b c f t", f=f)
            xt = model.channel_downsampler_t(xt)

        for idx, decode in enumerate(model.decoder):
            skip = saved.pop(-1)
            x, pre = decode(x, skip, lengths.pop(-1))
            offset = model.depth - len(model.tdecoder)
            if idx >= offset:
                tdec = model.tdecoder[idx - offset]
                length_t = lengths_t.pop(-1)
                if tdec.empty:
                    pre = pre[:, :, 0]
                    xt, _ = tdec(pre, None, length_t)
                else:
                    skip = saved_t.pop(-1)
                    xt, _ = tdec(xt, skip, length_t)

        S = len(model.sources)
        x = x.view(B, S, -1, Fq, T)
        x = x * std[:, None] + mean[:, None]
        # --- CaC: network output IS the complex spectrogram; keep real ---
        Bm, Sm, Cm, Frm, Tm = x.shape
        zout = x.view(Bm, Sm, -1, 2, Frm, Tm).permute(0, 1, 2, 4, 5, 3).contiguous()
        # --- time branch denormalised waveform (C++ adds iSTFT output) ---
        length = mix.shape[-1]
        xt = xt.view(B, S, -1, length)
        xt = xt * stdt[:, None] + meant[:, None]
        return zout, xt


def load_htdemucs():
    from demucs.pretrained import get_model
    model = get_model("htdemucs")
    if hasattr(model, "models"):  # BagOfModels -> the single htdemucs member
        assert len(model.models) == 1, [str(m) for m in model.models]
        model = model.models[0]
    from demucs.htdemucs import HTDemucs
    assert isinstance(model, HTDemucs), type(model)
    assert list(model.sources) == SOURCES, model.sources
    model.eval()
    return model


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default="htdemucs.onnx")
    ap.add_argument("--opset", type=int, default=17)
    ap.add_argument("--calib", default="",
                    help="44.1 kHz stereo wav for calibration "
                         "(default: seen-and-not-seen clip.wav)")
    ap.add_argument("--check-seconds", type=float, default=5.0)
    args = ap.parse_args()

    model = load_htdemucs()
    core = HTDemucsCore(model).eval()

    import soundfile as sf
    calib = (args.calib or os.path.expanduser(
        "~/Projects/seen-and-not-seen/public/audio/clip.wav"))
    # (demucs' own test wav roberta.wav is not shipped in the pip package;
    # clip.wav is 35 s of 44.1 kHz stereo, more than enough.)
    wav, sr = sf.read(calib, dtype="float32", always_2d=True)
    assert sr == SR, (calib, sr)
    wav = torch.from_numpy(wav.T)[None]  # [1, 2, N]
    seg = wav[:, :, :TRAIN_LEN].contiguous()

    # Reference _spec in torch (mirrors HTDemucs._spec + _magnitude exactly);
    # the C++ runtime replays this bit-for-bit with FFTW (see separate4.cpp).
    def torch_spec(mix):
        from demucs.spec import spectro
        length = mix.shape[-1]
        le = int(math.ceil(length / HOP))
        pad = HOP // 2 * 3
        x = F.pad(mix, (pad, pad + le * HOP - length), mode="reflect")
        z = spectro(x, N_FFT, HOP)[..., :-1, :]
        assert z.shape[-1] == le + 4, (z.shape, mix.shape, le)
        z = z[..., 2: 2 + le]
        B, C, Fr, T = z.shape
        m = torch.view_as_real(z).permute(0, 1, 4, 2, 3)
        return m.reshape(B, C * 2, Fr, T), (le, z.shape)

    # Reference _ispec + time-branch sum in torch (mirrors forward's eval
    # tail); the C++ runtime replays this with FFTW iSTFT.
    def torch_finish(zout_real, xt, length):
        from demucs.spec import ispectro
        B, S, C, Fq, T, _ = zout_real.shape
        zout = torch.view_as_complex(zout_real.contiguous())
        zz = F.pad(zout, (0, 0, 0, 1))
        zz = F.pad(zz, (2, 2))
        pad = HOP // 2 * 3
        le = HOP * int(math.ceil(length / HOP)) + 2 * pad
        x = ispectro(zz.reshape(B * S * C, Fq + 1, T + 4), HOP, length=le)
        x = x.view(B, S, C, le)
        x = x[..., pad: pad + length]
        return xt + x

    with torch.no_grad():
        mag, (le, zshape) = torch_spec(seg)
        print(f"trace spec: {tuple(mag.shape)} (le={le})")
        assert (le, mag.shape[-2], mag.shape[-1]) == (336, 2048, 336), \
            (le, tuple(mag.shape))
        zout, xt = core(seg, mag)
        print(f"trace outputs: zout {tuple(zout.shape)} xt {tuple(xt.shape)}")
        ref = torch_finish(zout, xt, TRAIN_LEN)
        ref2 = model(seg)
        err = ((ref - ref2).abs().max() / ref2.abs().max()).item()
        print(f"core+torch-spec/ispec vs forward: rel err {err:.3e}")
        assert err < 1e-5, err

    torch.onnx.export(
        core, (seg, mag),
        args.out,
        input_names=["mix", "spec"], output_names=["zout", "xt"],
        dynamic_axes=None,
        opset_version=args.opset,
        do_constant_folding=True,
    )
    print("exported", args.out,
          f"{os.path.getsize(args.out) / 1e6:.0f} MB")

    # ONNX check on a fresh excerpt (not the trace input): full pipeline
    # torch-spec -> ORT core -> torch-ispec vs model.forward.
    import numpy as np
    import onnxruntime as ort
    start = int(2.0 * SR)
    n = int(args.check_seconds * SR)
    seg2 = wav[:, :, start:start + n]
    assert seg2.shape[-1] == n, "calibration file too short"
    chunk = torch.zeros(1, 2, TRAIN_LEN)
    chunk[..., :n] = seg2  # zero-tail like TensorChunk.padded at song start
    with torch.no_grad():
        want = model(chunk).numpy()
        mag2, _ = torch_spec(chunk)
        sess = ort.InferenceSession(args.out, providers=["CPUExecutionProvider"])
        zout2, xt2 = sess.run(None, {"mix": chunk.numpy(),
                                     "spec": mag2.numpy()})
        got = torch_finish(torch.from_numpy(zout2),
                           torch.from_numpy(xt2), TRAIN_LEN).numpy()
    num = np.abs(got - want) ** 2
    den = np.maximum(np.abs(want) ** 2, 1e-12)
    snr = float(10 * np.log10(den.sum() / num.sum()))
    per = [float(10 * np.log10(
        np.maximum(np.abs(want[:, i]) ** 2, 1e-12).sum()
        / (np.abs(got[:, i] - want[:, i]) ** 2).sum()))
        for i in range(4)]
    print(f"ONNX check ({args.check_seconds:.0f} s excerpt): "
          f"overall {snr:.1f} dB, per-stem " +
          ", ".join(f"{s} {v:.1f}" for s, v in zip(SOURCES, per)))
    assert snr > 60, snr
    return 0


if __name__ == "__main__":
    sys.exit(main())
