#!/usr/bin/env python3
"""
Convert an RVC .pth voice model to ONNX for C++ runtime inference.
Also exports / verifies the shared HuBERT feature extractor ONNX.

Usage:
  vc_export.py <model.pth> <voice_out.onnx> <hubert_out.onnx>

Prints PROGRESS:<float> and ERROR:<msg> to stdout.
Exit code 0 on success.
"""
import sys, os, math
sys.path.insert(0, os.path.dirname(__file__))

import json
import torch
import torch.nn as nn
import torch.nn.functional as F
import torchaudio

# Re-use vendored architecture from vc_infer.py
from vc_infer import (
    SynthesizerTrnMsNSFsid,
    sequence_mask,
)

def prog(p): print(f"PROGRESS:{p:.3f}", flush=True)
def err(msg): print(f"ERROR:{msg}", flush=True)

# ── VITS deterministic inference wrapper ──────────────────────────────────────
#
# Differences from SynthesizerTrnMsNSFsid.infer():
#   • No sampling noise (uses distribution mean → deterministic output).
#   • No sequence_mask() call (all-ones mask derived from tensor shape).
#   • Inputs: phone [1,T,D], f0 [1,T], sid [1]
#   • Output: audio [1,1,M]

class VitsOnnxWrapper(nn.Module):
    def __init__(self, net_g):
        super().__init__()
        self.g = net_g

    def forward(self, phone, f0, sid):
        # phone: [1, T, phone_dim]  f0: [1, T]  sid: [1]
        g = self.g.emb_g(sid).unsqueeze(-1) if hasattr(self.g, 'emb_g') else None

        # All-ones mask [1, 1, T] — derived from phone to keep T symbolic in ONNX
        x_mask = phone[:, :, :1].transpose(1, 2).mul(0).add(1)

        # TextEncoder (bypass sequence_mask)
        x = self.g.enc_p.emb_phone(phone).transpose(1, 2)   # [1, hidden, T]
        x = self.g.enc_p.lrelu(x)
        x = self.g.enc_p.encoder(x, x_mask)
        m_p, _logs = self.g.enc_p.proj(x).chunk(2, dim=1)
        m_p = m_p * x_mask

        # Normalizing flow (reverse, deterministic — use mean, skip noise)
        z = self.g.flow(m_p, x_mask, g=g, reverse=True)

        # NSF-HiFiGAN decoder
        return self.g.dec(z * x_mask, f0, g=g)   # [1, 1, M]


# ── HuBERT wrapper ────────────────────────────────────────────────────────────

class HubertWrapper(nn.Module):
    def __init__(self, model):
        super().__init__()
        self.m = model

    def forward(self, x):   # [1, N_16k]
        feats, _ = self.m.extract_features(x)
        return feats[-1]    # [1, T, 768]


# ── Export helpers ────────────────────────────────────────────────────────────

def export_vits(net_g, out_path, tgt_sr, phone_dim):
    wrapper = VitsOnnxWrapper(net_g).eval()

    T_dummy  = 100
    phone    = torch.zeros(1, T_dummy, phone_dim)
    f0       = torch.zeros(1, T_dummy)
    sid      = torch.zeros(1, dtype=torch.long)

    prog(0.30)
    torch.onnx.export(
        wrapper,
        (phone, f0, sid),
        out_path,
        input_names  = ["phone", "f0", "sid"],
        output_names = ["audio"],
        dynamic_axes = {
            "phone": {1: "seq"},
            "f0":    {1: "seq"},
            "audio": {2: "samples"},
        },
        opset_version = 14,
        do_constant_folding = True,
    )
    prog(0.50)

    # Write sidecar JSON so C++ can read target_sr without ONNX metadata API
    sidecar = out_path.replace('.onnx', '.json')
    with open(sidecar, 'w') as f:
        json.dump({"target_sr": tgt_sr, "phone_dim": phone_dim}, f)
    prog(0.55)


def export_hubert(out_path):
    bundle = torchaudio.pipelines.HUBERT_BASE
    model  = bundle.get_model().eval()
    w      = HubertWrapper(model)
    dummy  = torch.zeros(1, 16000)

    prog(0.60)

    exported = False

    # Try PyTorch 2.x dynamo export (proper dynamic shapes)
    try:
        opts = torch.onnx.ExportOptions(dynamic_shapes=True)
        program = torch.onnx.dynamo_export(w, dummy, export_options=opts)
        program.save(out_path)
        exported = True
    except Exception:
        pass

    if not exported:
        # Fall back to legacy torch.onnx.export
        torch.onnx.export(
            w,
            dummy,
            out_path,
            input_names  = ["audio"],
            output_names = ["features"],
            dynamic_axes = {
                "audio":    {1: "time"},
                "features": {1: "frames"},
            },
            opset_version = 14,
        )

    prog(0.90)


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 4:
        print("Usage: vc_export.py <model.pth> <voice.onnx> <hubert.onnx>",
              file=sys.stderr)
        sys.exit(1)

    pth_path    = sys.argv[1]
    voice_out   = sys.argv[2]
    hubert_out  = sys.argv[3]

    prog(0.01)

    # Load checkpoint
    try:
        cpt = torch.load(pth_path, map_location='cpu', weights_only=False)
    except Exception as e:
        err(f"Failed to load checkpoint: {e}")
        sys.exit(1)

    cfg    = cpt.get('config', [])
    tgt_sr = int(cfg[-1]) if cfg else 40000

    w = cpt.get('weight', {})
    phone_dim = 768
    if 'enc_p.emb_phone.weight' in w:
        phone_dim = w['enc_p.emb_phone.weight'].shape[1]

    if phone_dim not in (256, 768):
        err(f"Unknown phone_dim={phone_dim}; only 256 and 768 are supported")
        sys.exit(1)

    prog(0.05)

    # Build model
    try:
        net_g = SynthesizerTrnMsNSFsid(*cfg, phone_dim=phone_dim)
        net_g.eval()
        net_g.load_state_dict(w, strict=False)
    except Exception as e:
        err(f"Failed to build model: {e}")
        sys.exit(1)

    prog(0.10)

    # Export VITS
    try:
        export_vits(net_g, voice_out, tgt_sr, phone_dim)
    except Exception as e:
        err(f"VITS ONNX export failed: {e}")
        sys.exit(1)

    # Export HuBERT (skip if already present)
    if not os.path.exists(hubert_out):
        os.makedirs(os.path.dirname(hubert_out), exist_ok=True)
        try:
            export_hubert(hubert_out)
        except Exception as e:
            err(f"HuBERT ONNX export failed: {e}")
            sys.exit(1)
    else:
        prog(0.90)

    prog(1.00)


if __name__ == '__main__':
    main()
