#!/usr/bin/env python3
"""
Pop Maker Studio — package setup script.
Installs torch, whisperx, and demucs with correct CUDA/CPU variant.
Emits structured progress lines parsed by the host app.
"""
import subprocess, sys, shutil, re

def log(msg): print(msg, flush=True)

def run_pip(*args):
    subprocess.run(
        [sys.executable, "-m", "pip", "install", "--progress-bar", "off"] + list(args),
        check=True
    )

# ── Detect CUDA version ───────────────────────────────────────────────────────

def cuda_version():
    """Returns (major, minor) if nvidia-smi is present, else None."""
    if not shutil.which("nvidia-smi"):
        return None
    try:
        out = subprocess.check_output(
            ["nvidia-smi", "--query-gpu=driver_version", "--format=csv,noheader"],
            stderr=subprocess.DEVNULL, text=True
        ).strip()
        # nvidia-smi also reports CUDA version in plain output
        out2 = subprocess.check_output(
            ["nvidia-smi"], stderr=subprocess.DEVNULL, text=True
        )
        m = re.search(r"CUDA Version:\s*(\d+)\.(\d+)", out2)
        if m:
            return int(m.group(1)), int(m.group(2))
    except Exception:
        pass
    return None

cv = cuda_version()
if cv is None:
    torch_index = "https://download.pytorch.org/whl/cpu"
    log("No CUDA detected — installing CPU torch.")
elif cv[0] >= 12 and cv[1] >= 8:
    torch_index = "https://download.pytorch.org/whl/cu128"
    log(f"CUDA {cv[0]}.{cv[1]} detected — installing CUDA 12.8 torch.")
elif cv[0] >= 12 and cv[1] >= 4:
    torch_index = "https://download.pytorch.org/whl/cu124"
    log(f"CUDA {cv[0]}.{cv[1]} detected — installing CUDA 12.4 torch.")
elif cv[0] >= 12 and cv[1] >= 1:
    torch_index = "https://download.pytorch.org/whl/cu121"
    log(f"CUDA {cv[0]}.{cv[1]} detected — installing CUDA 12.1 torch.")
elif cv[0] >= 11 and cv[1] >= 8:
    torch_index = "https://download.pytorch.org/whl/cu118"
    log(f"CUDA {cv[0]}.{cv[1]} detected — installing CUDA 11.8 torch.")
else:
    torch_index = "https://download.pytorch.org/whl/cpu"
    log(f"CUDA {cv[0]}.{cv[1]} too old — installing CPU torch.")

# ── Step 1: torch + torchaudio ────────────────────────────────────────────────

log("STAGE:torch")
log("Installing torch 2.8.0…")
try:
    run_pip("torch==2.8.0", "torchaudio==2.8.0",
            "--index-url", torch_index)
    log("OK:torch")
except subprocess.CalledProcessError as e:
    log(f"ERROR:torch:{e}")
    sys.exit(1)

# ── Step 2: whisperx ──────────────────────────────────────────────────────────

log("STAGE:whisperx")
log("Installing whisperx 3.8.5…")
try:
    run_pip("whisperx==3.8.5")
    log("OK:whisperx")
except subprocess.CalledProcessError as e:
    log(f"ERROR:whisperx:{e}")
    sys.exit(1)

# ── Step 3: demucs ────────────────────────────────────────────────────────────

log("STAGE:demucs")
log("Installing demucs 4.0.1…")
try:
    run_pip("demucs==4.0.1")
    log("OK:demucs")
except subprocess.CalledProcessError as e:
    log(f"ERROR:demucs:{e}")
    sys.exit(1)

# ── Step 4: rembg ─────────────────────────────────────────────────────────────

log("STAGE:rembg")
log("Installing rembg…")
try:
    run_pip("rembg[gpu]" if cv and cv[0] >= 11 else "rembg")
    log("OK:rembg")
except subprocess.CalledProcessError as e:
    log(f"ERROR:rembg:{e}")
    sys.exit(1)

log("DONE")
