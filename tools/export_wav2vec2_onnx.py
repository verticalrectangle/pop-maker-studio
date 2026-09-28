#!/usr/bin/env python3
"""Export torchaudio WAV2VEC2_ASR_BASE_960H to float ONNX (analysis v2 words).

The committed models/wav2vec2_ctc.onnx is a QUINT8-quantized export whose
first-word emissions collapse on sung onsets (measured on clip.wav line1:
They/had/also at conf 0.00/0.00/0.25 vs 1.00/0.93/1.00 for the float model).
This script exports the same torchaudio weights in float32, with a matching
vocab JSON {token: id} using torchaudio's label order ('-' blank = 0).

Usage:
  .venv/bin/python tools/export_wav2vec2_onnx.py --out <dir>/wav2vec2_ctc_float.onnx --vocab <dir>/wav2vec2_vocab_float.json
"""
import argparse
import json

import torch
import torchaudio


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--vocab", required=True)
    args = ap.parse_args()

    bundle = torchaudio.pipelines.WAV2VEC2_ASR_BASE_960H
    model = bundle.get_model().eval()

    class LogitsOnly(torch.nn.Module):
        def __init__(self, m):
            super().__init__()
            self.m = m

        def forward(self, x):
            emission, _ = self.m(x)
            return emission

    mod = LogitsOnly(model)
    dummy = torch.zeros(1, 16000, dtype=torch.float32)
    torch.onnx.export(
        mod,
        (dummy,),
        args.out,
        input_names=["input_values"],
        output_names=["logits"],
        dynamic_axes={"input_values": {1: "sequence_length"}, "logits": {1: "sequence_length"}},
        opset_version=14,
    )
    labels = bundle.get_labels()
    vocab = {tok: i for i, tok in enumerate(labels)}
    with open(args.vocab, "w") as f:
        json.dump(vocab, f)
    print(f"wrote {args.out} + {args.vocab} ({len(labels)} labels)")


if __name__ == "__main__":
    main()
