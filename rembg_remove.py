#!/usr/bin/env python3
"""
Pop Maker Studio — background removal script.
Processes frames of a video (MJPEG proxy or original) with rembg and writes
alpha-channel frames as a streaming MJPEG (bg_masks.mjpeg) to output_dir.

Usage:
    rembg_remove.py --input <video_path> --output <dir>

Progress is reported as JSON lines to stdout:
    {"progress": -1}          # model loading / first-run download
    {"progress": 0.45}        # frame processing
    {"done": true}
    {"error": "message"}
"""
import sys, os, json, subprocess, io

# ── Maximise CPU use before ORT loads ────────────────────────────────────────
_cpus = str(os.cpu_count() or 4)
os.environ.setdefault("OMP_NUM_THREADS",      _cpus)
os.environ.setdefault("OPENBLAS_NUM_THREADS", _cpus)
os.environ.setdefault("MKL_NUM_THREADS",      _cpus)

def log(obj):
    print(json.dumps(obj), flush=True)

args = sys.argv[1:]
def get_arg(key):
    if key in args:
        idx = args.index(key)
        if idx + 1 < len(args):
            return args[idx + 1]
    return None

input_path      = get_arg("--input")
output_dir      = get_arg("--output")
start_time      = float(get_arg("--start_time")  or "0")
duration        = float(get_arg("--duration")    or "0")
start_frame_arg = get_arg("--start_frame")
num_frames_arg  = get_arg("--num_frames")

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

# ── Probe fps (for fps.txt used by hires render path) ────────────────────────
fps = 30.0
try:
    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=r_frame_rate",
         "-of", "csv=p=0", input_path],
        capture_output=True, text=True, timeout=10
    )
    fps_str = probe.stdout.strip()
    if "/" in fps_str:
        num, den = fps_str.split("/")
        fps = float(num) / float(den)
    else:
        fps = float(fps_str) if fps_str else 30.0
except Exception:
    pass

with open(os.path.join(output_dir, "fps.txt"), "w") as f:
    f.write(f"{fps:.6f}\n")

# ── Warmup: force model download before any frame progress is reported ────────
log({"progress": -1})
try:
    import onnxruntime as ort
    opts = ort.SessionOptions()
    opts.intra_op_num_threads = os.cpu_count() or 8
    opts.inter_op_num_threads = 1
    opts.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    session = new_session("u2netp", sess_options=opts)
except Exception:
    session = new_session("silueta")
try:
    dummy = Image.new("RGB", (32, 32), (0, 255, 0))
    remove(dummy, session=session)
except Exception as e:
    log({"error": f"Failed to load rembg model: {e}"}); sys.exit(1)

import tempfile
from pathlib import Path

# start_frame: proxy frame index of the first mask frame, passed from C++ so it
# uses the exact same integer arithmetic as video_get_texture.
if start_frame_arg is not None:
    start_frame = int(start_frame_arg)
else:
    start_frame = round(start_time * fps) if fps > 0 else 0

# Write start_frame.txt so C++ knows the mapping without parsing filenames.
with open(os.path.join(output_dir, "start_frame.txt"), "w") as f:
    f.write(f"{start_frame}\n")

mjpeg_path = os.path.join(output_dir, "bg_masks.mjpeg")

with tempfile.TemporaryDirectory() as tmpdir:
    # Use frame-number selection (not time-based seeking) so the first extracted
    # frame is exactly proxy frame start_frame, regardless of fps metadata in the proxy.
    ffmpeg_cmd = ["ffmpeg", "-i", input_path]
    if start_frame_arg is not None and num_frames_arg is not None:
        end_frame = start_frame + int(num_frames_arg) - 1
        ffmpeg_cmd += ["-vf", f"select='between(n,{start_frame},{end_frame})'",
                       "-vsync", "vfr"]
    elif start_time > 0.001:
        ffmpeg_cmd += ["-ss", f"{start_time:.6f}"]
        if duration > 0.001:
            ffmpeg_cmd += ["-t", f"{duration:.6f}"]
    ffmpeg_cmd += ["-f", "image2", os.path.join(tmpdir, "%06d.jpg")]
    subprocess.run(ffmpeg_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    frames = sorted(Path(tmpdir).glob("*.jpg"))
    total = max(1, len(frames))

    with open(mjpeg_path, "wb") as mjpeg_f:
        for i, fp in enumerate(frames):
            try:
                img    = Image.open(fp)
                result = remove(img, session=session)
                alpha  = result.getchannel("A")
                buf    = io.BytesIO()
                alpha.save(buf, format="JPEG", quality=90)
                mjpeg_f.write(buf.getvalue())
                mjpeg_f.flush()
            except Exception as e:
                log({"error": f"frame {i}: {e}"})
            log({"progress": (i + 1) / total})

log({"done": True})
