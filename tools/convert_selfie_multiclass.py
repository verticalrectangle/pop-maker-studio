#!/usr/bin/env python3
"""Convert MediaPipe selfie_multiclass_256x256 to ONNX for PMS skin segmentation.

Downloads selfie_multiclass_256x256.tflite from
storage.googleapis.com/mediapipe-models (MediaPipe Image Segmenter bundle,
Apache-2.0) and converts it with tf2onnx. The output model has:

  input  "input_29": [1,256,256,3] RGB float32 in [0,1]
  output "Identity":  [1,256,256,6] per-class LOGITS, classes in order
    background, hair, body-skin, face-skin, clothes, others
  (apply softmax over the last axis for confidence maps)

Conversion needs: pip install tf2onnx tensorflow-cpu onnxruntime "numpy<2".
Every artifact is verified against the engine's tensor contract (see
src/skin_segment.h) before it is written to --out — a wrong conversion fails
here, not at runtime.

Model storage: converted .onnx goes in the shared-models dir; hard-link it
into models/ (models/*.onnx are git-excluded). Never commit the model.

iOS bundling: not needed (desktop-only skin segmentation).
"""
import argparse
import hashlib
import os
import subprocess
import sys
import urllib.request

TFLITE_URL = ("https://storage.googleapis.com/mediapipe-models/image_segmenter/"
              "selfie_multiclass_256x256/float32/1/selfie_multiclass_256x256.tflite")
TFLITE_SHA256 = "c6748b1253a99067ef71f7e26ca71096cd449baefa8f101900ea23016507e0e0"

CLASS_NAMES = ["background", "hair", "body_skin", "face_skin", "clothes", "others"]


def fetch(url, dst):
    if os.path.exists(dst):
        return
    print(f"  downloading {url}")
    urllib.request.urlretrieve(url, dst)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def tf2onnx_convert(tflite, out):
    subprocess.run([sys.executable, "-m", "tf2onnx.convert",
                    "--tflite", tflite, "--output", out, "--opset", "16"],
                   check=True, capture_output=True, text=True)


def verify(path):
    import numpy as np
    import onnxruntime as ort
    sess = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
    ins = sess.get_inputs()
    outs = sess.get_outputs()
    assert len(ins) == 1 and ins[0].name == "input_29", [i.name for i in ins]
    assert list(ins[0].shape) == [1, 256, 256, 3], ins[0].shape
    assert len(outs) == 1 and outs[0].name == "Identity", [o.name for o in outs]
    assert list(outs[0].shape) == [1, 256, 256, 6], outs[0].shape
    rng = np.random.default_rng(0)
    y = sess.run(None, {"input_29": rng.random((1, 256, 256, 3),
                                               dtype=np.float32)})[0]
    assert np.isfinite(y).all(), "non-finite logits"
    # Softmax rows must sum to 1 (output is logits, not probabilities).
    e = np.exp(y - y.max(axis=-1, keepdims=True))
    sm = e / e.sum(axis=-1, keepdims=True)
    assert abs(float(sm.mean() * 6) - 1.0) < 1e-5, "softmax sanity failed"
    print(f"  ok {os.path.basename(path)} in=input_29[1,256,256,3] "
          f"out=Identity[1,256,256,6] classes={','.join(CLASS_NAMES)}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="models/selfie_multiclass_256x256.onnx",
                    help="destination .onnx path")
    ap.add_argument("--work", default="/tmp/pms_selfie_convert",
                    help="scratch dir for the downloaded .tflite")
    ap.add_argument("--keep-tflite", action="store_true",
                    help="also copy the source .tflite next to the output")
    args = ap.parse_args()

    os.makedirs(args.work, exist_ok=True)
    tflite = os.path.join(args.work, "selfie_multiclass_256x256.tflite")
    fetch(TFLITE_URL, tflite)
    digest = sha256(tflite)
    if digest != TFLITE_SHA256:
        sys.exit(f"SHA256 mismatch for {tflite}:\n  got      {digest}\n"
                 f"  expected {TFLITE_SHA256}\nRefusing to convert.")
    print(f"  tflite sha256 ok ({digest[:16]}…)")

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    tf2onnx_convert(tflite, args.out)
    verify(args.out)
    if args.keep_tflite:
        dst = os.path.join(os.path.dirname(os.path.abspath(args.out)),
                           "selfie_multiclass_256x256.tflite")
        if os.path.abspath(dst) != os.path.abspath(tflite):
            import shutil
            shutil.copyfile(tflite, dst)
    print(f"done -> {args.out}")


if __name__ == "__main__":
    main()
