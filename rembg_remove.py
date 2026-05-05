#!/usr/bin/env python3
"""
Pop Maker Studio — background removal script.
Processes every frame of a video (MJPEG proxy or original) with rembg and
writes per-frame RGBA PNG masks to output_dir.

Usage:
    rembg_remove.py --input <video_path> --output <dir>

Progress is reported as JSON lines to stdout:
    {"progress": -1}          # model loading / first-run download
    {"progress": 0.45}        # frame processing
    {"done": true}
    {"error": "message"}
"""
import sys, os, json, subprocess

# ── Option 5: maximise CPU use before ORT loads ───────────────────────────────
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

# ── Probe fps + dimensions in one ffprobe call ────────────────────────────────
fps   = 30.0
vid_w = 0
vid_h = 0
try:
    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=r_frame_rate,width,height",
         "-of", "json", input_path],
        capture_output=True, text=True, timeout=10
    )
    s = json.loads(probe.stdout).get("streams", [{}])[0]
    fps_str = s.get("r_frame_rate", "30/1")
    vid_w   = int(s.get("width",  0))
    vid_h   = int(s.get("height", 0))
    if "/" in fps_str:
        num, den = fps_str.split("/")
        fps = float(num) / float(den) if float(den) else 30.0
    else:
        fps = float(fps_str) if fps_str else 30.0
except Exception:
    pass

with open(os.path.join(output_dir, "fps.txt"), "w") as f:
    f.write(f"{fps:.6f}\n")

# ── Warmup: force model download before any frame progress is reported ─────────
log({"progress": -1})
try:
    session = new_session()
    dummy = Image.new("RGB", (32, 32), (0, 255, 0))
    remove(dummy, session=session)
except Exception as e:
    log({"error": f"Failed to load rembg model: {e}"}); sys.exit(1)

# ── Option 3: stream raw frames from ffmpeg — no temp disk I/O ────────────────
if vid_w > 0 and vid_h > 0:
    # Count JPEG SOI markers to get exact frame count (fast pure-Python scan,
    # works for MJPEG proxies which carry no duration metadata).
    nb_frames = 0
    try:
        prev = b""
        with open(input_path, "rb") as fh:
            while True:
                chunk = fh.read(65536)
                if not chunk:
                    break
                combined = prev + chunk
                pos = 0
                while True:
                    idx = combined.find(b"\xff\xd8", pos)
                    if idx == -1:
                        break
                    nb_frames += 1
                    pos = idx + 2
                prev = combined[-1:]
    except Exception:
        nb_frames = 0

    frame_bytes = vid_w * vid_h * 3
    proc = subprocess.Popen(
        ["ffmpeg", "-i", input_path,
         "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL
    )

    i = 0
    total = max(1, nb_frames)
    while True:
        data = proc.stdout.read(frame_bytes)
        if len(data) < frame_bytes:
            break
        try:
            img = Image.frombytes("RGB", (vid_w, vid_h), data)
            result = remove(img, session=session)
            result.save(os.path.join(output_dir, f"{i:06d}.png"), "PNG")
        except Exception as e:
            log({"error": f"frame {i}: {e}"})
        i += 1
        log({"progress": min(1.0, i / total)})

    proc.wait()

else:
    # Fallback: couldn't probe dimensions — use temp JPEG files
    import tempfile
    from pathlib import Path
    with tempfile.TemporaryDirectory() as tmpdir:
        subprocess.run(
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
                result.save(os.path.join(output_dir, f"{i:06d}.png"), "PNG")
            except Exception as e:
                log({"error": f"frame {i}: {e}"})
            log({"progress": min(1.0, (i + 1) / total)})

log({"done": True})
