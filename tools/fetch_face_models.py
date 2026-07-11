#!/usr/bin/env python3
"""Fetch + convert the face-tracking models face_track.cpp expects.

Produces (into --out, default models/face/):
  yunet.onnx              — OpenCV Zoo face_detection_yunet_2023mar (MIT).
                            Used as-is; already ONNX with the cls/obj/bbox/
                            kps_{8,16,32} heads the engine decodes.
  face_landmarks_v2.onnx  — MediaPipe Face Landmarker v2 mesh net
                            (Apache-2.0), converted from the tflite inside
                            Google's face_landmarker.task bundle via tf2onnx.
                            Contract: input "input_12" [1,256,256,3] RGB 0-1,
                            outputs "Identity" (478×3 crop px) +
                            "Identity_1" (face flag logit).
  face_blendshapes.onnx   — MediaPipe blendshape head from the same bundle.
                            Contract: input "serving_default_input_points:0"
                            [1,146,2], output "StatefulPartitionedCall:0"
                            [1,52].

Conversion needs: pip install tf2onnx tensorflow-cpu onnxruntime "numpy<2".
Every artifact is verified against the engine's tensor contract before it is
written to --out — a wrong conversion fails here, not at runtime.

iOS bundling: copy the three .onnx into
pms-ios/Engine/EngineAssets/models/face/ (project.yml ships EngineAssets as a
folder resource; paths.cpp app_models_dir() prefers asset-root models).
"""
import argparse, hashlib, os, shutil, subprocess, sys, tempfile, urllib.request, zipfile

YUNET_URL = ("https://github.com/opencv/opencv_zoo/raw/main/models/"
             "face_detection_yunet/face_detection_yunet_2023mar.onnx")
TASK_URL = ("https://storage.googleapis.com/mediapipe-models/face_landmarker/"
            "face_landmarker/float16/latest/face_landmarker.task")


def fetch(url, dst):
    if os.path.exists(dst):
        print(f"  have {os.path.basename(dst)}")
        return
    print(f"  fetch {url}")
    urllib.request.urlretrieve(url, dst)


def tf2onnx_convert(tflite, out):
    subprocess.run([sys.executable, "-m", "tf2onnx.convert",
                    "--tflite", tflite, "--output", out, "--opset", "16"],
                   check=True, capture_output=True, text=True)


def verify(path, want_in, want_out, in_shape=None):
    import onnxruntime as ort
    s = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
    ins  = {i.name: i.shape for i in s.get_inputs()}
    outs = [o.name for o in s.get_outputs()]
    assert want_in in ins, f"{path}: input '{want_in}' missing (has {list(ins)})"
    for o in want_out:
        assert o in outs, f"{path}: output '{o}' missing (has {outs})"
    if in_shape is not None:
        got = [d if isinstance(d, int) else 1 for d in ins[want_in]]
        assert got == in_shape, f"{path}: input shape {got} != {in_shape}"
    print(f"  ok {os.path.basename(path)}  in={want_in} out={want_out}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "models", "face"))
    ap.add_argument("--work", default=None, help="scratch dir (kept if given)")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    work = args.work or tempfile.mkdtemp(prefix="pms-face-models-")
    os.makedirs(work, exist_ok=True)

    # 1. YuNet — already ONNX.
    yunet = os.path.join(work, "yunet.onnx")
    fetch(YUNET_URL, yunet)
    verify(yunet, "input",
           ["cls_8", "cls_16", "cls_32", "obj_8", "obj_16", "obj_32",
            "bbox_8", "bbox_16", "bbox_32", "kps_8", "kps_16", "kps_32"],
           [1, 3, 640, 640])
    shutil.copy(yunet, os.path.join(args.out, "yunet.onnx"))

    # 2. + 3. MediaPipe task bundle → tflite pair → ONNX.
    task = os.path.join(work, "face_landmarker.task")
    fetch(TASK_URL, task)
    with zipfile.ZipFile(task) as z:
        z.extract("face_landmarks_detector.tflite", work)
        z.extract("face_blendshapes.tflite", work)

    lmk = os.path.join(work, "face_landmarks_v2.onnx")
    if not os.path.exists(lmk):
        print("  convert face_landmarks_detector.tflite → onnx")
        tf2onnx_convert(os.path.join(work, "face_landmarks_detector.tflite"), lmk)
    verify(lmk, "input_12", ["Identity", "Identity_1"], [1, 256, 256, 3])
    shutil.copy(lmk, os.path.join(args.out, "face_landmarks_v2.onnx"))

    bls = os.path.join(work, "face_blendshapes.onnx")
    if not os.path.exists(bls):
        print("  convert face_blendshapes.tflite → onnx")
        tf2onnx_convert(os.path.join(work, "face_blendshapes.tflite"), bls)
    verify(bls, "serving_default_input_points:0", ["StatefulPartitionedCall:0"],
           [1, 146, 2])
    shutil.copy(bls, os.path.join(args.out, "face_blendshapes.onnx"))

    for f in ("yunet.onnx", "face_landmarks_v2.onnx", "face_blendshapes.onnx"):
        p = os.path.join(args.out, f)
        h = hashlib.sha256(open(p, "rb").read()).hexdigest()[:16]
        print(f"  {f}: {os.path.getsize(p)} bytes sha256:{h}")
    print(f"done → {args.out}")


if __name__ == "__main__":
    main()
