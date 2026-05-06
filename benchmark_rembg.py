#!/usr/bin/env python3
"""
rembg model benchmark for Pop Maker Studio.

Usage:
    python3 benchmark_rembg.py --input <frame.jpg> [--runs 5] [--output results/]

Runs every practical portrait/subject model against a single proxy frame,
reports median time per frame, saves alpha outputs for visual comparison,
and writes BENCHMARKS.md.
"""
import sys, os, time, json, statistics, argparse, io
from pathlib import Path

# ── Maximise CPU before ORT loads ────────────────────────────────────────────
_cpus = str(os.cpu_count() or 4)
os.environ.setdefault("OMP_NUM_THREADS",      _cpus)
os.environ.setdefault("OPENBLAS_NUM_THREADS", _cpus)
os.environ.setdefault("MKL_NUM_THREADS",      _cpus)

MODELS = [
    ("u2netp",              "Fastest pruned model (current default)"),
    ("u2net",               "Full U²-Net — more accurate, 2× slower"),
    ("u2net_human_seg",     "U²-Net fine-tuned on human portrait data"),
    ("silueta",             "Compact person-specific model"),
    ("isnet-general-use",   "ISNet — newer architecture, strong on clothing edges"),
    ("birefnet-portrait",   "BiRefNet fine-tuned for portraits"),
    ("birefnet-general",    "BiRefNet general — current SOTA for matting"),
    ("birefnet-general-lite","BiRefNet-lite — SOTA quality at reduced cost"),
]

parser = argparse.ArgumentParser()
parser.add_argument("--input",  required=True, help="Path to a single proxy frame (JPEG/PNG)")
parser.add_argument("--runs",   type=int, default=5, help="Timed runs per model (default 5)")
parser.add_argument("--output", default="benchmark_results", help="Output directory")
args = parser.parse_args()

try:
    from rembg import remove, new_session
    from PIL import Image, ImageFilter
    import onnxruntime as ort
    import numpy as np
except ImportError as e:
    print(f"Error: {e}\nInstall rembg and Pillow first."); sys.exit(1)

out_dir = Path(args.output)
out_dir.mkdir(parents=True, exist_ok=True)

img = Image.open(args.input).convert("RGB")
orig_w, orig_h = img.size
img_2x = img.resize((orig_w * 2, orig_h * 2), Image.LANCZOS)
print(f"Input: {args.input}  ({orig_w}×{orig_h}, 2× = {orig_w*2}×{orig_h*2})")
print(f"Runs per model: {args.runs}\n")

def make_session(model_name):
    try:
        opts = ort.SessionOptions()
        opts.intra_op_num_threads = os.cpu_count() or 8
        opts.inter_op_num_threads = 1
        opts.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
        return new_session(model_name, sess_options=opts)
    except Exception:
        return new_session(model_name)

results = []

for model_name, description in MODELS:
    print(f"── {model_name}")
    print(f"   Downloading / loading model…", end="", flush=True)
    try:
        session = make_session(model_name)
        # Warmup (forces model weights into memory, not counted in timing)
        dummy = Image.new("RGB", (64, 64), (0, 180, 0))
        remove(dummy, session=session)
        print(" done")
    except Exception as e:
        print(f" FAILED: {e}")
        results.append({
            "model": model_name,
            "description": description,
            "error": str(e),
        })
        continue

    times = []
    alpha_out = None

    for run in range(args.runs):
        t0 = time.perf_counter()
        result = remove(img_2x, session=session)
        alpha  = result.getchannel("A").resize((orig_w, orig_h), Image.LANCZOS)
        alpha  = alpha.filter(ImageFilter.GaussianBlur(radius=0.7))
        elapsed = time.perf_counter() - t0
        times.append(elapsed)
        if run == 0:
            alpha_out = alpha
        print(f"   run {run+1}/{args.runs}: {elapsed:.3f}s")

    med = statistics.median(times)
    mn  = min(times)
    mx  = max(times)
    print(f"   median {med:.3f}s  min {mn:.3f}s  max {mx:.3f}s\n")

    # Save alpha and composited preview
    if alpha_out:
        alpha_path = out_dir / f"{model_name.replace('-','_')}_alpha.png"
        alpha_out.save(alpha_path)

        # Composite: person on black background for quick visual comparison
        rgba = img.copy().convert("RGBA")
        rgba.putalpha(alpha_out)
        preview = Image.new("RGBA", img.size, (0, 0, 0, 255))
        preview.paste(rgba, mask=alpha_out)
        preview_path = out_dir / f"{model_name.replace('-','_')}_preview.png"
        preview.convert("RGB").save(preview_path)

    results.append({
        "model":       model_name,
        "description": description,
        "median_s":    round(med, 3),
        "min_s":       round(mn,  3),
        "max_s":       round(mx,  3),
    })

# ── Write BENCHMARKS.md ───────────────────────────────────────────────────────

results_sorted = sorted(
    [r for r in results if "median_s" in r],
    key=lambda r: r["median_s"]
)
failed = [r for r in results if "error" in r]

baseline = results_sorted[0]["median_s"] if results_sorted else 1.0

md = []
md.append("# rembg Model Benchmarks\n")
md.append(f"**Test frame:** `{os.path.basename(args.input)}` — {orig_w}×{orig_h}  ")
md.append(f"**Timed runs per model:** {args.runs}  ")
md.append(f"**CPU:** {os.cpu_count()} threads  ")
md.append(f"**Processing pipeline:** 2× supersample → model → Lanczos downsample → σ=0.7 Gaussian\n")
md.append("")
md.append("## Results (sorted fastest → slowest)\n")
md.append("| Model | Median | Min | Max | vs. u2netp | Notes |")
md.append("|-------|--------|-----|-----|------------|-------|")

u2netp_time = next((r["median_s"] for r in results_sorted if r["model"] == "u2netp"), baseline)

for r in results_sorted:
    ratio = r["median_s"] / u2netp_time
    ratio_str = f"{ratio:.1f}×" if ratio >= 1.05 else "baseline"
    md.append(
        f"| `{r['model']}` | {r['median_s']:.3f}s | {r['min_s']:.3f}s "
        f"| {r['max_s']:.3f}s | {ratio_str} | {r['description']} |"
    )

if failed:
    md.append("")
    md.append("## Failed to load\n")
    for r in failed:
        md.append(f"- `{r['model']}` — {r['error']}")

md.append("")
md.append("## Visual outputs\n")
md.append("Preview images (person composited on black) saved alongside this file:\n")
for r in results_sorted:
    fname = r['model'].replace('-','_')
    md.append(f"- `{fname}_preview.png` — `{r['model']}`")

md.append("")
md.append("## Quality notes\n")
md.append("_(Fill in after visual inspection of preview images)_\n")
md.append("")
md.append("| Model | Edge quality | Hair detail | Clothing edges | Artefacts |")
md.append("|-------|-------------|-------------|----------------|-----------|")
for r in results_sorted:
    md.append(f"| `{r['model']}` | | | | |")

md_path = Path("BENCHMARKS.md")
md_path.write_text("\n".join(md) + "\n")

print("─" * 60)
print(f"BENCHMARKS.md written.")
print(f"Preview images written to {out_dir}/")
print()
print("Summary:")
for r in results_sorted:
    ratio = r["median_s"] / u2netp_time
    bar   = "█" * int(ratio * 10)
    print(f"  {r['model']:30s}  {r['median_s']:.3f}s  {bar}")
for r in failed:
    print(f"  {r['model']:30s}  FAILED")
