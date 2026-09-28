"""Album-portrait asset generator (Remain in Light look) for script-clip scenes.

From a short clip (e.g. an iPhone Live Photo .mov) this writes, under --out:
  live/f_NN.jpg          every source frame (display orientation, real VFR times)
  img/s*_*.png           grading stages of the album-framed crop of the snap frame
  img/px_*.png           block pixelation stages (dark-biased, palette-quantised)
  img/blink_*.png        aligned half/closed blink patches (RGBA, eye region only)
  portrait.json          manifest: crop, palette, stage paths + histograms, laser burn
                         order of the red mask blocks, per-frame landmarks/eye openness,
                         face-mesh edges

Face landmarks, eye openness and mesh edges come from PMS (`get_media_face`), class masks
from PMS (`segment_image`), so the running PMS instance must be reachable (PMS_SOCK).

Usage: python tools/album_portrait.py --media clip.mov --out project/assets/portrait
Requires: numpy, opencv-python-headless.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mcp_server.server import _call  # noqa: E402  (PMS IPC client)

SIZE = 1080  # portrait layer resolution (square)
GRID = 108  # final block grid: 10 px blocks at 1080
PIXEL_STEPS = [270, 135, 54, GRID]  # pixelation beats: 4 px, 8 px, 20 px overshoot, settle 10 px
LIVE_WIDTH = 1080

# Sampled from the Remain in Light cover (k-means over the portrait panels).
PALETTE = ["#120e0e", "#2e2829", "#545c64", "#6d879d", "#a7bccb"]  # dark -> light
BG_BLUE = "#5095d5"
RED = "#c9150d"
# Face-skin blocks darker than this luminance percentile stay unmasked (brows, nostrils,
# lip line, glasses, shading), which is what makes the cover's red read as a face.
RED_PERCENTILE = 36

# Landmarks that do not move when eyes/mouth/brows move: used to register blink frames.
STABLE = [10, 151, 9, 8, 168, 6, 197, 195, 5, 4, 1, 234, 454, 93, 323, 132, 361, 58, 288, 152]
EYE_L = [33, 7, 163, 144, 145, 153, 154, 155, 133, 173, 157, 158, 159, 160, 161, 246]
EYE_R = [263, 249, 390, 373, 374, 380, 381, 382, 362, 398, 384, 385, 386, 387, 388, 466]
BROW_L = [70, 63, 105, 66, 107, 55, 65, 52, 53, 46]
BROW_R = [300, 293, 334, 296, 336, 285, 295, 282, 283, 276]


def hex_rgb(h: str) -> np.ndarray:
    return np.array([int(h[i : i + 2], 16) for i in (1, 3, 5)], dtype=np.float32) / 255.0


def save(path: Path, rgb: np.ndarray, quality: int | None = None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    bgr = cv2.cvtColor((np.clip(rgb, 0, 1) * 255).round().astype(np.uint8), cv2.COLOR_RGB2BGR)
    params = [cv2.IMWRITE_JPEG_QUALITY, quality] if quality else [cv2.IMWRITE_PNG_COMPRESSION, 9]
    cv2.imwrite(str(path), bgr, params)


def save_rgba(path: Path, rgba: np.ndarray) -> None:
    out = (np.clip(rgba, 0, 1) * 255).round().astype(np.uint8)
    cv2.imwrite(str(path), cv2.cvtColor(out, cv2.COLOR_RGBA2BGRA), [cv2.IMWRITE_PNG_COMPRESSION, 9])


def load_rgb(path: Path) -> np.ndarray:
    return cv2.cvtColor(cv2.imread(str(path)), cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0


def extract_frames(media: Path, frames_dir: Path) -> list[float]:
    """Decode every source frame (rotation applied) and return presentation times in seconds."""
    frames_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["ffmpeg", "-v", "error", "-y", "-i", str(media), "-fps_mode", "passthrough", str(frames_dir / "f_%03d.png")],
        check=True,
    )
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v", "-show_entries", "frame=pts_time", "-of", "csv=p=0", str(media)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return [round(float(t), 4) for t in out.replace(",", " ").split()]


def track(media: Path, times: list[float]) -> tuple[list[np.ndarray], list[float], dict]:
    """Per-frame 478-point landmarks (normalised) + eye openness from PMS's face track,
    matched to each extracted frame by nearest source time."""
    face = _call("get_media_face", {"path": str(media), "wait": True})
    if face.get("status") != "ready":
        raise SystemExit(f"face tracking failed for {media}: {face.get('status')}")
    ft = np.array(face["times"], dtype=np.float64)
    landmarks, openness = [], []
    for t in times:
        i = int(np.argmin(np.abs(ft - t)))
        landmarks.append(np.array(face["landmarks"][i], dtype=np.float32))
        openness.append(float(face["eyeOpen"][i]))
    return landmarks, openness, face["mesh"]


def segment(frame_path: Path, work: Path) -> dict[str, np.ndarray]:
    """Soft class maps (0..1) from PMS's multiclass selfie segmenter."""
    res = _call("segment_image", {"path": str(frame_path), "out_dir": str(work / "classes")})
    return {name: cv2.imread(p, cv2.IMREAD_GRAYSCALE).astype(np.float32) / 255.0 for name, p in res["classes"].items()}


def crop_box(pts_px: np.ndarray, w: int, h: int) -> tuple[int, int, int]:
    """Album framing: face (forehead->chin) fills ~57% of the square, chin at 80% height."""
    top, chin = pts_px[10], pts_px[152]
    face_h = chin[1] - top[1]
    side = int(round(1.75 * face_h))
    cx = (pts_px[234][0] + pts_px[454][0]) / 2
    x0 = int(round(np.clip(cx - side / 2, 0, w - side)))
    y0 = int(round(np.clip(chin[1] - 0.80 * side, 0, h - side)))
    return x0, y0, side


def crop(img: np.ndarray, box: tuple[int, int, int], interp=cv2.INTER_AREA) -> np.ndarray:
    x0, y0, side = box
    return cv2.resize(img[y0 : y0 + side, x0 : x0 + side], (SIZE, SIZE), interpolation=interp)


class Grade:
    """Luminance normalisation fitted once on the snap portrait and reused for blink frames,
    so every layer shares identical tone mapping."""

    def __init__(self, rgb: np.ndarray, person: np.ndarray):
        wb = self.white_balance(rgb)
        lum = self.luminance(wb)
        vals = lum[person > 0.5]
        self.lo, self.hi = np.percentile(vals, [2, 98])

    @staticmethod
    def white_balance(rgb: np.ndarray) -> np.ndarray:
        means = rgb.reshape(-1, 3).mean(0)
        return np.clip(rgb * (means.mean() / means), 0, 1)

    @staticmethod
    def luminance(rgb: np.ndarray) -> np.ndarray:
        return rgb @ np.array([0.299, 0.587, 0.114], dtype=np.float32)

    def curve(self, rgb: np.ndarray) -> np.ndarray:
        """White-balanced, contrast-curved luminance in [0, 1]."""
        lum = self.luminance(self.white_balance(rgb))
        x = np.clip((lum - self.lo) / (self.hi - self.lo), 0, 1)
        return x * x * (3 - 2 * x)  # smoothstep S-curve


def posterize(x: np.ndarray, levels: int) -> np.ndarray:
    return np.minimum(np.floor(x * levels), levels - 1) / (levels - 1)


def palette_map(x: np.ndarray) -> np.ndarray:
    pal = np.stack([hex_rgb(h) for h in PALETTE])
    idx = np.minimum(np.floor(x * len(PALETTE)), len(PALETTE) - 1).astype(int)
    return pal[idx]


def to_grid(x: np.ndarray, g: int) -> np.ndarray:
    return cv2.resize(x, (g, g), interpolation=cv2.INTER_AREA)


def upscale(x: np.ndarray) -> np.ndarray:
    return cv2.resize(x, (SIZE, SIZE), interpolation=cv2.INTER_NEAREST)


def tone_grid(lum: np.ndarray, g: int) -> np.ndarray:
    """Block tone biased toward each block's darks.

    A plain mean washes thin dark detail (irises, glasses rims, lip line) into the skin
    around it; mixing in the block's 20th percentile keeps those features punching through.
    """
    b = SIZE // g
    blocks = lum.reshape(g, b, g, b).transpose(0, 2, 1, 3).reshape(g, g, b * b)
    return 0.5 * blocks.mean(-1) + 0.5 * np.percentile(blocks, 20, axis=-1)


def pixel_layer(lum: np.ndarray, bg: np.ndarray, g: int) -> np.ndarray:
    """Blocky album layer: tone + background decided per block, never blended."""
    rgb = palette_map(tone_grid(lum, g))
    rgb[to_grid(bg, g) > 0.5] = hex_rgb(BG_BLUE)
    return upscale(rgb)


def poly_grid(polys: list[np.ndarray], dilate_px: int) -> np.ndarray:
    """Fraction of each grid block covered by (dilated) landmark polygons."""
    m = np.zeros((SIZE, SIZE), np.uint8)
    for p in polys:
        cv2.fillPoly(m, [cv2.convexHull(p.astype(np.int32))], 1)
    if dilate_px:
        m = cv2.dilate(m, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (dilate_px, dilate_px)))
    return to_grid(m.astype(np.float32), GRID)


def red_mask(
    lum: np.ndarray,
    face_skin: np.ndarray,
    pts: np.ndarray,
    threshold: float | None,
    lid: np.ndarray | None = None,
) -> tuple[np.ndarray, float]:
    """Album mask: face skin brighter than a cutoff turns red; darker features punch through.

    `lid` marks blocks where a closing eyelid now covers the open eye: that is skin, so it
    goes red regardless of shading, which is what makes a blink read at block resolution.
    """
    face = to_grid(face_skin, GRID)
    lum_g = to_grid(lum, GRID)
    if threshold is None:
        threshold = float(np.percentile(lum_g[face > 0.5], RED_PERCENTILE))
    eyes = poly_grid([pts[EYE_L], pts[EYE_R]], 10)
    red = (face > 0.45) & (lum_g > threshold) & (eyes < 0.35)
    if lid is not None:
        red |= lid > 0.35
    # Drop specks: the cover's mask is ragged but not noisy.
    n, lab, stats, _ = cv2.connectedComponentsWithStats(red.astype(np.uint8), connectivity=4)
    for i in range(1, n):
        if stats[i, cv2.CC_STAT_AREA] < 3:
            red[lab == i] = False
    return red, threshold


def laser_order(red: np.ndarray) -> list[list[int]]:
    """Engraver raster: rows top->bottom, alternating direction."""
    order = []
    for r in range(GRID):
        cols = np.nonzero(red[r])[0]
        order.extend([int(c), r] for c in (cols if r % 2 == 0 else cols[::-1]))
    return order


def composite(base: np.ndarray, red: np.ndarray) -> np.ndarray:
    out = base.copy()
    out[upscale(red.astype(np.uint8)) > 0] = hex_rgb(RED)
    return out


def histogram(x: np.ndarray, bins: int = 48) -> list[float]:
    h, _ = np.histogram(x, bins=bins, range=(0, 1))
    h = h / h.max()
    return [round(float(v), 3) for v in h]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--media", type=Path, required=True, help="source clip (e.g. Live Photo .mov)")
    ap.add_argument("--out", type=Path, required=True, help="asset directory to write")
    ap.add_argument("--snap", type=int, default=-1, help="frame index of the portrait still (default: last)")
    args = ap.parse_args()
    out = args.out
    work = Path(tempfile.mkdtemp(prefix="album_portrait_"))

    times = extract_frames(args.media, work / "frames")
    paths = sorted((work / "frames").glob("f_*.png"))
    frames = [load_rgb(p) for p in paths]
    H, W = frames[0].shape[:2]
    landmarks, openness, mesh = track(args.media, times)
    snap_i = args.snap % len(frames)
    snap = frames[snap_i]
    pts_px = landmarks[snap_i] * [W, H]
    print(f"{len(frames)} frames {W}x{H}; snap eye openness {openness[snap_i]:.2f}")

    # ---- live frames for the opening -------------------------------------------------
    live_h = int(round(H * LIVE_WIDTH / W))
    for i, f in enumerate(frames):
        save(out / "live" / f"f_{i:02d}.jpg", cv2.resize(f, (LIVE_WIDTH, live_h), interpolation=cv2.INTER_AREA), 92)

    # ---- portrait crop + segmentation ------------------------------------------------
    box = crop_box(pts_px, W, H)
    x0, y0, side = box
    classes = {k: crop(v, box) for k, v in segment(paths[snap_i], work).items()}
    portrait = crop(snap, box)
    pts = (pts_px - [x0, y0]) * (SIZE / side)
    person = 1 - classes["background"]
    grade = Grade(portrait, person)
    lum = grade.curve(portrait)
    lum_smooth = cv2.GaussianBlur(lum, (0, 0), 2.0)
    bg = classes["background"]

    img = out / "img"
    stages = []

    def stage(name: str, rgb: np.ndarray, hist_src: np.ndarray) -> None:
        save(img / f"{name}.png", rgb)
        stages.append({"name": name, "src": f"img/{name}.png", "hist": histogram(hist_src)})

    wb = Grade.white_balance(portrait)
    gray = Grade.luminance(wb)[..., None]
    stage("s0_raw", portrait, Grade.luminance(portrait))
    stage("s1_neutral", gray + 0.35 * (wb - gray), Grade.luminance(wb))
    stage("s2_curve", np.repeat(lum[..., None], 3, 2), lum)
    for levels in (16, 8, 5):
        p = posterize(lum_smooth, levels)
        stage(f"s3_post{levels}", np.repeat(p[..., None], 3, 2), p)
    toned = palette_map(lum_smooth)
    stage("s4_tone", toned, lum_smooth)
    flat = toned.copy()
    flat[bg > 0.5] = hex_rgb(BG_BLUE)
    stage("s5_bg", flat, lum_smooth)

    pixel = []
    for g in PIXEL_STEPS:
        save(img / f"px_{g}.png", pixel_layer(lum, bg, g))
        pixel.append({"grid": g, "src": f"img/px_{g}.png"})

    red, threshold = red_mask(lum, classes["face_skin"], pts, None)
    order = laser_order(red)
    base = pixel_layer(lum, bg, GRID)
    save(out / "preview_final.png", composite(base, red))
    print(f"red threshold {threshold:.3f}; {len(order)} red blocks of {GRID * GRID}")

    # ---- blink patches ---------------------------------------------------------------
    eye_pts = np.concatenate([pts[EYE_L], pts[EYE_R], pts[BROW_L], pts[BROW_R]])
    bx0, by0 = eye_pts.min(0) - [40, 30]
    bx1, by1 = eye_pts.max(0) + [40, 70]
    cell = SIZE / GRID
    rect = [int(bx0 // cell), int(by0 // cell), int(np.ceil(bx1 / cell)), int(np.ceil(by1 / cell))]
    region = np.zeros((SIZE, SIZE), np.float32)
    region[int(rect[1] * cell) : int(rect[3] * cell), int(rect[0] * cell) : int(rect[2] * cell)] = 1

    open_eyes = poly_grid([pts[EYE_L], pts[EYE_R]], 0)
    closed_i = int(np.argmin(openness))
    mid = (openness[closed_i] + openness[snap_i]) / 2
    half_i = int(np.argmin([abs(o - mid) for o in openness]))
    blink_layers = {}
    for name, fi in (("half", half_i), ("closed", closed_i)):
        src_px = landmarks[fi] * [W, H]
        M, _ = cv2.estimateAffinePartial2D(src_px[STABLE], pts_px[STABLE], method=cv2.LMEDS)
        warped = cv2.warpAffine(frames[fi], M, (W, H), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
        w_pts_px = cv2.transform(src_px[None], M)[0]
        b_lum = grade.curve(crop(warped, box))
        b_pts = (w_pts_px - [x0, y0]) * (SIZE / side)
        lid = np.clip(open_eyes - poly_grid([b_pts[EYE_L], b_pts[EYE_R]], 0), 0, 1)
        b_red, _ = red_mask(b_lum, classes["face_skin"], b_pts, threshold, lid)
        b_rgb = composite(pixel_layer(b_lum, bg, GRID), b_red)
        save_rgba(img / f"blink_{name}.png", np.dstack([b_rgb, region]))
        blink_layers[name] = {"src": f"img/blink_{name}.png", "frame": fi, "open": round(openness[fi], 3)}
        save(out / f"preview_blink_{name}.png", np.where(region[..., None] > 0, b_rgb, composite(base, red)))
    print(f"blink frames: half={half_i} ({openness[half_i]:.2f}) closed={closed_i} ({openness[closed_i]:.2f})")

    # ---- metadata --------------------------------------------------------------------
    meta = {
        "size": SIZE,
        "grid": GRID,
        "palette": PALETTE,
        "bg": BG_BLUE,
        "red": RED,
        "crop": {"x": x0 / W, "y": y0 / H, "w": side / W, "h": side / H},
        "frame": {"w": W, "h": H},
        "stages": stages,
        "pixel": pixel,
        "laser": order,
        "blink": {**blink_layers, "rect": rect},
        "portraitLandmarks": np.round(pts / SIZE, 4).tolist(),
        "live": {
            "count": len(frames),
            "snap": snap_i,
            "times": times,
            "src": [f"live/f_{i:02d}.jpg" for i in range(len(frames))],
            "eyeOpen": [round(o, 3) for o in openness],
            "landmarks": [np.round(p, 4).tolist() for p in landmarks],
        },
        "mesh": mesh,
    }
    (out / "portrait.json").write_text(json.dumps(meta, separators=(",", ":")))
    print("wrote", out / "portrait.json", (out / "portrait.json").stat().st_size, "bytes")
    shutil.rmtree(work)


if __name__ == "__main__":
    main()
