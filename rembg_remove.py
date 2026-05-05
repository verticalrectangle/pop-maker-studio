#!/usr/bin/env python3
"""
Pop Maker Studio — background removal script.
Processes every frame of a video (MJPEG proxy or original) with rembg and
writes per-frame RGBA PNG masks to output_dir.

Usage:
    rembg_remove.py --input <video_path> --output <dir>

Progress is reported as JSON lines to stdout:
    {"progress": 0.45}
    {"done": true}
    {"error": "message"}
"""
import sys, os, json, subprocess, tempfile
from pathlib import Path

def log(obj):
    print(json.dumps(obj), flush=True)

args = sys.argv[1:]
def get_arg(key):
    if key in args:
        idx = args.index(key)
        if idx + 1 < len(args):
            return args[idx + 1]
    return None

input_path = get_arg("--input")
output_dir = get_arg("--output")

if not input_path or not output_dir:
    log({"error": "Missing --input or --output"}); sys.exit(1)

if not os.path.exists(input_path):
    log({"error": f"Input not found: {input_path}"}); sys.exit(1)

try:
    from rembg import remove, new_session
    from PIL import Image
except ImportError as e:
    log({"error": f"rembg/Pillow not installed: {e}"}); sys.exit(1)

os.makedirs(output_dir, exist_ok=True)

# Probe fps via ffprobe and write fps.txt so the host can use it at export time.
try:
    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=r_frame_rate",
         "-of", "csv=p=0", input_path],
        capture_output=True, text=True, timeout=10
    )
    fps_str = probe.stdout.strip()  # e.g. "30000/1001" or "30/1"
    if "/" in fps_str:
        num, den = fps_str.split("/")
        fps = float(num) / float(den)
    else:
        fps = float(fps_str) if fps_str else 30.0
except Exception:
    fps = 30.0

with open(os.path.join(output_dir, "fps.txt"), "w") as f:
    f.write(f"{fps:.6f}\n")

# Load model once — downloads on first use, cached afterwards.
try:
    session = new_session()
except Exception as e:
    log({"error": f"Failed to load rembg model: {e}"}); sys.exit(1)

with tempfile.TemporaryDirectory() as tmpdir:
    # Extract every frame as JPEG. Works for MJPEG proxy and any video container.
    ret = subprocess.run(
        ["ffmpeg", "-i", input_path, "-f", "image2",
         os.path.join(tmpdir, "%06d.jpg")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    frames = sorted(Path(tmpdir).glob("*.jpg"))
    total = max(1, len(frames))

    for i, fp in enumerate(frames):
        try:
            img = Image.open(fp)
            result = remove(img, session=session)
            out_path = os.path.join(output_dir, f"{i:06d}.png")
            result.save(out_path, "PNG")
        except Exception as e:
            log({"error": f"frame {i}: {e}"})
        log({"progress": (i + 1) / total})

log({"done": True})
