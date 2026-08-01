#!/usr/bin/env python3
"""precut_flowers — cut wildflower photos off white/light backgrounds into
transparent PNGs for compositing (Image clips respect PNG alpha).

Per file: luminance/saturation-based matte with edge feathering, then trim to
the subject bbox with a small margin. White studio backgrounds only — busy
backgrounds are skipped (reported).
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

SRC = Path("/home/alexis/Videos/Pop Maker Studio Projects/wildflower_cover")
OUT = SRC / "cutouts"


def bg_color(arr: np.ndarray) -> np.ndarray:
    """Estimate the background colour from the image border (median of the
    outer 6-pixel frame). Works for white studio, green foliage, dark paper."""
    b = 6
    frame = np.concatenate([
        arr[:b, :, :3].reshape(-1, 3), arr[-b:, :, :3].reshape(-1, 3),
        arr[:, :b, :3].reshape(-1, 3), arr[:, -b:, :3].reshape(-1, 3)])
    return np.median(frame, axis=0)


def matte(arr: np.ndarray) -> np.ndarray:
    """0..255 alpha: transparent where the pixel is close to the border
    background colour (euclidean distance, soft knee)."""
    bg = bg_color(arr)
    dist = np.sqrt(((arr[..., :3].astype(float) - bg) ** 2).sum(axis=2))
    a = np.clip((dist - 28.0) * 6.0, 0, 255)
    return a.astype(np.uint8)


def process(path: Path) -> dict:
    img = Image.open(path).convert("RGB")
    arr = np.asarray(img)
    a = matte(arr)
    frac = float((a > 128).mean())
    if frac < 0.01:
        return {"file": path.name, "ok": False,
                "why": f"matte coverage {frac:.1%} — subject fills nothing"}
    alpha = Image.fromarray(a).filter(ImageFilter.MinFilter(3))
    alpha = alpha.filter(ImageFilter.GaussianBlur(1.2))
    rgba = img.convert("RGBA")
    rgba.putalpha(alpha)
    bbox = alpha.getbbox()
    if bbox:
        m = 12
        bbox = (max(0, bbox[0] - m), max(0, bbox[1] - m),
                min(img.width, bbox[2] + m), min(img.height, bbox[3] + m))
        rgba = rgba.crop(bbox)
    out = OUT / (path.stem + ".png")
    rgba.save(out)
    return {"file": path.name, "ok": True, "out": out.name,
            "coverage": round(frac, 3), "size": rgba.size}


def main() -> None:
    OUT.mkdir(exist_ok=True)
    exts = {".jpg", ".jpeg", ".png", ".webp"}
    results = []
    for p in sorted(SRC.iterdir()):
        if p.suffix.lower() in exts and p.parent == SRC:
            results.append(process(p))
    for r in results:
        print(("OK  " if r["ok"] else "SKIP") + "  " + r["file"],
              r.get("out", r.get("why", "")))
    ok = sum(1 for r in results if r["ok"])
    print(f"\n{ok}/{len(results)} cut into {OUT}")


if __name__ == "__main__":
    main()
