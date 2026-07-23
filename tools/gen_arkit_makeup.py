#!/usr/bin/env python3
"""ARKit makeup atlas painter — paint-on-model, single look.

Every element is painted ON the canonical ARKit head in MILLIMETERS (lid-rim
polylines, lip ring, cheek anchors), then rasterized through the mesh's own
triangles into its ARKit-UV texture. Placement is correct by construction:
the eye-hole rim IS the lash line, the mouth ring IS the vermilion border, so
liner rides the lashes and lips hug the mouth — no UV-space curve fitting that
flips or floats on device.

CRISPNESS: the position map + all painting run at SS×SIZE (default 2× = 2048²)
and the result is downsampled to 1024² with Lanczos. Thin features (liner,
lashes) that aliased to nothing at 1024² — because the mesh samples the atlas
at a foreshortened angle near the rim — anti-alias correctly through the
downsample. This is the fix for "liner not a clean line, no lashes."

Output: models/face/arkit/makeup_<id>.png (RGBA 1024²).
Verify on a REAL face fixture via arkit-native-replay before ship — the
canonical head's proportions lie (brows, eye shape, jitter are per-person).

Usage:
  tools/gen_arkit_makeup.py [--assets <dir>] [--ss N] [--only id ...]
"""
import argparse
import hashlib
import os
import sys

import numpy as np
from PIL import Image

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import gen_arkit_mp_map as G   # parse_obj / parse_header_array / boundary_rings / ring_near

SIZE = 1024
SS = 2                          # supersample: paint at SS*SIZE, downsample to SIZE
SRC_GEN = os.path.join(here, "..", "src", "generated")

# ── ARKit topology ─────────────────────────────────────────────────────────
# Validated in arkit_map_smoke + gen_arkit_mp_map anchors. The canonical head
# faces +z with +y up, so +x = the person's LEFT in model space (matches MP).
#   person's RIGHT eye: outer 1101, inner 1090, upper-rim arc 1100..1091 (x<0)
#   person's LEFT  eye: outer 1069, inner 1080, upper-rim arc 1070..1079 (x>0)
#   mouth hole flanked by verts 249 (right corner) / 684 (left corner)
ARC_UP_R = [1100, 1099, 1098, 1097, 1096, 1095, 1094, 1093, 1092, 1091]
ARC_UP_L = [1070, 1071, 1072, 1073, 1074, 1075, 1076, 1077, 1078, 1079]
CORNERS_R = (1101, 1090)        # (outer, inner)
CORNERS_L = (1069, 1080)
MOUTH_TARGET = (249, 684)


# ── mesh → UV rasterization ────────────────────────────────────────────────
def parse_uvs():
    """ARKit UV coords + triangle indices from the generated mesh header."""
    ak_uv = G.parse_header_array(os.path.join(SRC_GEN, "arkit_face_mesh.h"),
                                 "k_arkit_uv", (1220, 2))
    ak_tris = G.parse_header_array(os.path.join(SRC_GEN, "arkit_face_mesh.h"),
                                   "k_arkit_tris", (2304, 3)).astype(int)
    return ak_uv, ak_tris


def rasterize_positions(uv, tris, verts, size):
    """Per-texel 3D position (mm) for a mesh in its own UV space.

    Returns (pos[size,size,3], mask[size,size]). mask=True where a triangle
    covers the texel — i.e. on-face skin (eye/mouth holes stay False, so paint
    never lands inside the apertures)."""
    pos = np.zeros((size, size, 3), np.float64)
    mask = np.zeros((size, size), bool)
    puv = uv * (size - 1)
    for t in tris:
        p0, p1, p2 = puv[t[0]], puv[t[1]], puv[t[2]]
        v0, v1, v2 = verts[t[0]], verts[t[1]], verts[t[2]]
        x0 = int(max(0, np.floor(min(p0[0], p1[0], p2[0]))))
        x1 = int(min(size - 1, np.ceil(max(p0[0], p1[0], p2[0]))))
        y0 = int(max(0, np.floor(min(p0[1], p1[1], p2[1]))))
        y1 = int(min(size - 1, np.ceil(max(p0[1], p1[1], p2[1]))))
        if x1 < x0 or y1 < y0:
            continue
        xs, ys = np.meshgrid(np.arange(x0, x1 + 1), np.arange(y0, y1 + 1))
        d = ((p1[0] - p0[0]) * (p2[1] - p0[1])
             - (p2[0] - p0[0]) * (p1[1] - p0[1]))
        if abs(d) < 1e-9:
            continue
        w1 = ((xs - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (ys - p0[1])) / d
        w2 = ((p1[0] - p0[0]) * (ys - p0[1]) - (xs - p0[0]) * (p1[1] - p0[1])) / d
        w0 = 1.0 - w1 - w2
        inside = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
        if not inside.any():
            continue
        pt = (w0[..., None] * v0 + w1[..., None] * v1 + w2[..., None] * v2)
        pos[ys[inside], xs[inside]] = pt[inside]
        mask[ys[inside], xs[inside]] = True
    return pos, mask


# ── canonical-head geometry (built once) ───────────────────────────────────
print("loading canonical head + rasterizing UV positions "
      f"(SS={SS}, {SS * SIZE}²)...")
AK_V = G.parse_obj(os.path.join(here, "arkit_face_canonical.obj"))
AK_UV, AK_TRIS = parse_uvs()
SSIZE = SS * SIZE
POS, MASK = rasterize_positions(AK_UV, AK_TRIS, AK_V, SSIZE)
TX = POS[MASK]                          # (N,3) per-texel 3D positions, mm
NTX = TX.shape[0]
print(f"  {NTX} on-face texels")


def ring_3d(target_verts):
    c = AK_V[list(target_verts)].mean(0)
    return G.ring_near(G.boundary_rings(AK_TRIS, 1220), AK_V, c)


class Eye:
    """Upper/lower rim polylines (outer→inner) + corners, in mm."""

    def __init__(self, corners, up_arc):
        ring = ring_3d(corners)
        up = [AK_V[c] for c in [corners[0]] + list(up_arc) + [corners[1]]]
        low_ids = [v for v in ring if v not in up_arc and v not in corners]
        o, i = AK_V[corners[0]], AK_V[corners[1]]
        ax = i - o
        ax = ax / np.linalg.norm(ax)
        low_ids.sort(key=lambda v: float((AK_V[v] - o) @ ax))
        low = [AK_V[corners[0]]] + [AK_V[v] for v in low_ids] + [AK_V[corners[1]]]
        self.up = np.array(up)           # upper rim (lash line), outer→inner
        self.low = np.array(low)         # lower rim (waterline), outer→inner
        self.center = self.up.mean(0)
        self.outer = AK_V[corners[0]]
        self.inner = AK_V[corners[1]]


EYES = [Eye(CORNERS_R, ARC_UP_R), Eye(CORNERS_L, ARC_UP_L)]
_mpts = AK_V[np.array(ring_3d(MOUTH_TARGET))]
_mc = _mpts.mean(0)
_ang = np.arctan2(_mpts[:, 1] - _mc[1], _mpts[:, 0] - _mc[0])
MOUTH = _mpts[np.argsort(_ang)]          # mouth ring, CCW
MOUTH_C = _mc

# face anchors (mm), straight off the canonical head
EYE_D = float(np.linalg.norm(EYES[0].center - EYES[1].center))
NOSE_TIP = AK_V[int(np.argmax(AK_V[:, 2]))]
NOSE_BRIDGE = (EYES[0].center + EYES[1].center + NOSE_TIP) / 3.0
CHIN = AK_V[int(np.argmin(AK_V[:, 1]))]
CHEEK = [AK_V[311], AK_V[746]]           # cheekbones
JAW = [AK_V[62], AK_V[511]]
CUPID = MOUTH_C + np.array([0, 4.0, 3.0])


# ── paint helpers (operate on NTX-length float RGBA arrays) ─────────────────
def new_img():
    return np.zeros((NTX, 4), np.float64)


def over(base, l):
    """Straight-alpha composite layer l over base."""
    sa = base[:, 3:4] / 255.0
    la = l[:, 3:4] / 255.0
    oa = la + sa * (1 - la)
    rgb = (l[:, :3] * la + base[:, :3] * sa * (1 - la)) / np.where(oa > 0, oa, 1)
    return np.concatenate([rgb, oa * 255.0], 1)


def to_img(arr):
    """NTX float RGBA → 1024² uint8 PNG (downsampled from SSIZE if SS>1)."""
    full = np.zeros((SSIZE, SSIZE, 4), np.uint8)
    full[MASK] = np.clip(arr, 0, 255).astype(np.uint8)
    im = Image.fromarray(full, "RGBA")
    if SS > 1:
        im = im.resize((SIZE, SIZE), Image.LANCZOS)
    return im


def dist_to_polyline(P, poly):
    """(N,3) points → (min distance to polyline, global t along it [0,1])."""
    best = np.full(P.shape[0], 1e9)
    best_t = np.zeros(P.shape[0])
    for k in range(len(poly) - 1):
        a, b = poly[k], poly[k + 1]
        ab = b - a
        L2 = float(ab @ ab) + 1e-12
        t = np.clip(((P - a) @ ab) / L2, 0, 1)
        d = np.linalg.norm(P - (a + t[:, None] * ab), axis=1)
        m = d < best
        best[m] = d[m]
        best_t[m] = (k + t[m]) / (len(poly) - 1)
    return best, best_t


def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


def blob(center, sigma_mm, color, alpha):
    d = np.linalg.norm(TX - np.array(center), axis=1)
    a = alpha * np.exp(-(d ** 2) / (2 * sigma_mm ** 2))
    l = new_img()
    l[:, :3] = color
    l[:, 3] = a
    return l


def stroke(a, b, width_mm, color, alpha, taper=0.0):
    """A capped line segment a→b of the given width, optional tip taper."""
    d, t = dist_to_polyline(TX, np.array([a, b]))
    w = width_mm * (1.0 - taper * t)
    ink = (1 - smooth(w - 0.18, w + 0.22, d)) * alpha
    l = new_img()
    l[:, :3] = color
    l[:, 3] = ink
    return l


# ── elements ───────────────────────────────────────────────────────────────
def el_blush(img, style, color, alpha):
    l = new_img()
    for cheek, eye in zip(CHEEK, EYES):
        if style == "lifted":
            l = over(l, blob(cheek * 0.72 + eye.center * 0.28, 11, color, alpha))
        elif style == "apple":
            l = over(l, blob(cheek * 0.6 + MOUTH_C * 0.4, 10, color, alpha))
        else:                                   # cheeks
            l = over(l, blob(cheek, 13, color, alpha))
    return over(img, l)


def el_contour(img, style="soft", color=None, alpha=46, areas=None):
    if color is None:
        color = {"warm": (118, 84, 68), "cool": (90, 70, 80),
                 "soft": (104, 84, 76)}[style]
    if areas is None:
        areas = ["cheek", "jaw", "nose"]
    l = new_img()
    for cheek, jaw in zip(CHEEK, JAW):
        if "cheek" in areas:
            l = over(l, blob(cheek * 0.5 + jaw * 0.5, 12, color, alpha))
        if "jaw" in areas:
            l = over(l, blob(jaw * 0.6 + CHIN * 0.4, 11, color, alpha * 0.8))
    if "nose" in areas:
        nb = NOSE_BRIDGE * 0.4 + NOSE_TIP * 0.6
        for sgn in (-1, 1):
            l = over(l, blob(nb + np.array([6.5 * sgn, 0, 0]), 5,
                             color, alpha * 0.7))
    return over(img, l)


def el_shadow(img, lid=None, crease=None, outer=None, inner=None,
              shimmer=None, height=1.0):
    """Layered eye: lid wash, crease depth, outer-V, inner light, shimmer."""
    l = new_img()
    for eye in EYES:
        d, t = dist_to_polyline(TX, eye.up)
        up_dir = TX - eye.center
        up_dir = up_dir / (np.linalg.norm(up_dir, axis=1, keepdims=True) + 1e-9)
        above = up_dir[:, 1] > -0.15          # lid hemisphere (not in the hole)
        if lid:
            c, a = lid
            al = smooth(-0.2, 0.6, d) * (1 - smooth(3.0 * height, 4.4 * height, d)) * above
            al *= a / 255.0
            lay = new_img(); lay[:, :3] = c; lay[:, 3] = al * 255
            l = over(l, lay)
        if crease:
            c, a = crease
            al = smooth(2.2 * height, 3.4 * height, d) * \
                 (1 - smooth(6.6 * height, 8.4 * height, d)) * above
            al *= a / 255.0
            lay = new_img(); lay[:, :3] = c; lay[:, 3] = al * 255
            l = over(l, lay)
        if outer:
            c, a = outer
            w = np.clip(1.0 - t / 0.42, 0, 1)   # outer ~40% of the rim
            al = smooth(0.2, 1.2, d) * (1 - smooth(5.0 * height, 6.6 * height, d)) * w * above
            al *= a / 255.0
            lay = new_img(); lay[:, :3] = c; lay[:, 3] = al * 255
            l = over(l, lay)
        if inner:
            c, a = inner
            w = np.clip((t - 0.70) / 0.30, 0, 1)  # inner ~30%
            al = smooth(-0.2, 0.8, d) * (1 - smooth(2.6, 3.8, d)) * w
            al *= a / 255.0
            lay = new_img(); lay[:, :3] = c; lay[:, 3] = al * 255
            l = over(l, lay)
        if shimmer:
            c, a = shimmer
            w = np.exp(-((t - 0.5) ** 2) / (2 * 0.16 ** 2))
            al = smooth(0.4, 1.4, d) * (1 - smooth(3.0, 4.2, d)) * w * above
            al *= a / 255.0
            lay = new_img(); lay[:, :3] = c; lay[:, 3] = al * 255
            l = over(l, lay)
    return over(img, l)


def el_liner(img, color=(20, 14, 16), alpha=246, width=1.25, wing=5.5,
             lower=True):
    """Crisp upper lash-line liner with a soft outer wing + tightline lower.

    Sits ON eye.up (the lash rim): full opacity at d=0, clean edge. The wing
    continues the outer-corner tangent up & out. Lower is a thin kohl over the
    inner two-thirds of the waterline (the classic tightline)."""
    l = new_img()
    for eye in EYES:
        d, t = dist_to_polyline(TX, eye.up)
        # taper: thicker at the outer corner, finer toward the inner
        w = width * (1.0 - 0.40 * t)
        ink = (1 - smooth(w - 0.18, w + 0.22, d)) * alpha
        lay = new_img(); lay[:, :3] = color; lay[:, 3] = ink
        l = over(l, lay)
        # wing: outer-corner tangent lifted up & out
        if wing > 0:
            tan = eye.up[0] - eye.up[1]
            tan = tan / np.linalg.norm(tan)
            upv = np.array([0.0, 1.0, 0.0])
            wdir = tan * 0.78 + upv * 0.22
            wdir = wdir / np.linalg.norm(wdir)
            tip = eye.outer + wdir * wing
            l = over(l, stroke(eye.outer, tip, width * 0.8, color,
                               alpha, taper=0.55))
        # lower tightline: continuous thin kohl along the waterline, solid
        # over the inner four-fifths then a soft taper past the outer third.
        if lower:
            dl, tl = dist_to_polyline(TX, eye.low)
            lw = width * 0.85
            low_ink = (1 - smooth(lw - 0.18, lw + 0.24, dl)) * alpha * 0.85
            low_ink *= np.clip((1.0 - tl) / 0.22, 0, 1)   # taper only outer tip
            lay = new_img(); lay[:, :3] = color; lay[:, 3] = low_ink
            l = over(l, lay)
    return over(img, l)


def el_lashes(img, strength=1.0, style="natural", color=(14, 10, 14),
              lower=True, seed=0):
    """Tapered lash clusters radiating up & out from the upper rim, plus
    sparse lower lashes. Wide enough (0.85–1.0mm) to survive the mesh sampling
    the atlas at a foreshortened angle."""
    rng = np.random.default_rng(seed + 5)
    n, ln, w, sweep_r = {
        "natural": (30, 5.6, 1.05, (-0.22, 0.12)),
        "doll":    (24, 6.0, 1.15, (-0.12, 0.12)),
        "wispy":   (34, 5.0, 0.95, (-0.30, 0.08)),
        "stage":   (38, 6.4, 1.20, (-0.18, 0.04)),
    }[style]
    l = new_img()
    for eye in EYES:
        rim = eye.up
        nn = int(n * min(strength, 1.4))
        for k in range(nn):
            t = 0.05 + 0.90 * (k + 0.5) / nn
            f = t * (len(rim) - 1)
            i0 = min(int(f), len(rim) - 2)
            root = rim[i0] * (1 - f + i0) + rim[i0 + 1] * (f - i0)
            out = root - eye.center
            out[1] = abs(out[1]) + 2.0                 # up, off the lid
            out = out / np.linalg.norm(out)
            tan = rim[i0 + 1] - rim[i0]
            tan = tan / np.linalg.norm(tan)
            length = ln * rng.uniform(0.78, 1.22) * min(strength, 1.3)
            sweep = rng.uniform(*sweep_r)
            tip = root + out * length + tan * length * sweep
            l = over(l, stroke(root, tip, w * rng.uniform(0.9, 1.15),
                               color, 255 * min(strength, 1.2), taper=0.45))
        if lower:
            m = max(4, int(6 * strength))
            rim2 = eye.low
            for k in range(m):
                t = 0.18 + 0.64 * (k + 0.5) / m
                f = t * (len(rim2) - 1)
                i0 = min(int(f), len(rim2) - 2)
                root = rim2[i0] * (1 - f + i0) + rim2[i0 + 1] * (f - i0)
                out = root - eye.center
                out[1] = -abs(out[1]) - 2.0           # down, off the lower lid
                out = out / np.linalg.norm(out)
                length = 2.0 * rng.uniform(0.8, 1.2)
                tip = root + out * length
                l = over(l, stroke(root, tip, w * 0.7, color,
                                   220 * min(strength, 1.2), taper=0.5))
    return over(img, l)


def el_lip(img, color=(176, 92, 88), alpha=220, liner_color=(120, 56, 58),
           gloss=True):
    """Filled lip painted around the mouth seam (the closed-mouth lip line).

    The canonical head's mouth is a thin sealed slit, so the lip can't be built
    from a mouth-opening ring. Instead: distance to the seam polyline (which
    follows the mouth's curve) split by signed vertical offset — upper lip
    above the seam, a fuller lower lip below. The vermilion liner rides the
    outer edge; gloss pools on the lower-lip center."""
    dy = TX[:, 1] - MOUTH_C[1]            # y-up: + above seam (upper lip)
    dx = TX[:, 0] - MOUTH_C[0]
    half_w = 20.5                          # mouth half-width, mm
    xt = np.clip(1.0 - (np.abs(dx) - 16.5) / 4.0, 0, 1)   # taper into corners
    h_up, h_low = 5.0, 7.5
    # fill both lips outward from the seam (signed dy, not the x-z loop distance)
    fu = (1 - smooth(h_up - 1.2, h_up - 0.2, dy)) * (dy > -0.3)
    fl = (1 - smooth(h_low - 1.2, h_low - 0.2, -dy)) * (dy < 0.3)
    fill = np.clip(fu + fl, 0, 1) * xt

    l = new_img()
    lay = new_img(); lay[:, :3] = color; lay[:, 3] = fill * alpha
    l = over(l, lay)
    if liner_color is not None:            # vermilion liner at the outer edges
        eu = smooth(h_up - 1.4, h_up - 0.6, dy) * (1 - smooth(h_up + 0.1, h_up + 0.9, dy))
        el = smooth(h_low - 1.4, h_low - 0.6, -dy) * (1 - smooth(h_low + 0.1, h_low + 0.9, -dy))
        edge = np.clip(eu + el, 0, 1) * xt
        lay = new_img(); lay[:, :3] = liner_color; lay[:, 3] = edge * 235
        l = over(l, lay)
    if gloss:                              # dimensional gloss, lower-lip center
        gl = blob(MOUTH_C + np.array([0, -3.5, 3.0]), 2.6, (255, 250, 246),
                  alpha * 0.20)
        l = over(l, gl)
    return over(img, l)


def el_highlight(img, color=(255, 246, 240), alpha=52):
    l = new_img()
    for cheek, eye in zip(CHEEK, EYES):
        p = cheek * 0.45 + eye.center * 0.55
        p[1] += 6
        l = over(l, blob(p, 8, color, alpha))         # cheekbone / brow bone
    l = over(l, blob(NOSE_BRIDGE * 0.6 + NOSE_TIP * 0.4, 4, color, alpha * 0.8))
    l = over(l, blob(CUPID, 3, color, alpha * 0.6))
    return over(img, l)


EL = {"blush": el_blush, "contour": el_contour, "shadow": el_shadow,
      "liner": el_liner, "lashes": el_lashes, "lip": el_lip,
      "highlight": el_highlight}


def S(lid=None, crease=None, outer=None, inner=None, shimmer=None):
    return dict(lid=lid, crease=crease, outer=outer, inner=inner, shimmer=shimmer)


# ── the one look ───────────────────────────────────────────────────────────
# Soft Glam / everyday: taupe-brown eye (lid + crease + outer-V + inner
# light), soft warm contour, lifted rose blush, clean wing liner + lower
# tightline, visible natural lashes, nude satin lip with a defined edge.
# Pigment is intentionally solid enough to read through the engine's
# luma-adaptive premult-over composite (the old atlases washed out).
LOOKS = {
    "soft_glam": [
        ("contour", dict(style="warm", alpha=50, areas=["cheek", "jaw", "nose"])),
        ("blush", dict(style="lifted", color=(222, 130, 120), alpha=58)),
        ("shadow", S(lid=((206, 156, 122), 92), crease=((168, 112, 82), 82),
                     outer=((140, 88, 62), 74), inner=((248, 232, 212), 70),
                     shimmer=((255, 244, 232), 48))),
        ("liner", dict(alpha=246, width=1.25, wing=5.5, lower=True)),
        ("lip", dict(color=(172, 100, 92), alpha=220,
                     liner_color=(118, 56, 58), gloss=True)),
        ("highlight", dict(alpha=54)),
        # No texture lashes: on the ARKit native path lashes are 3D geometry
        # generated from the eye-rim mesh vertices (metal_render.mm face_lash_*),
        # so the atlas must not also paint them (no doubling).
    ],
}


def seed_of(s):
    return int(hashlib.sha256(s.encode()).hexdigest(), 16) % (2 ** 32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--assets", default=os.path.expanduser(
        "~/dev/pms-ios/Engine/EngineAssets"))
    ap.add_argument("--ss", type=int, default=2)
    ap.add_argument("--only", nargs="*", default=None)
    args = ap.parse_args()

    global SS, SSIZE, POS, MASK, TX, NTX
    if args.ss != SS:
        SS = args.ss
        SSIZE = SS * SIZE
        POS, MASK = rasterize_positions(AK_UV, AK_TRIS, AK_V, SSIZE)
        TX = POS[MASK]
        NTX = TX.shape[0]
        print(f"re-rasterized at SS={SS} ({SSIZE}², {NTX} texels)")

    out_dir = os.path.join(args.assets, "models", "face", "arkit")
    os.makedirs(out_dir, exist_ok=True)
    ids = args.only or list(LOOKS.keys())
    for look_id in ids:
        img = new_img()
        for name, kw in LOOKS[look_id]:
            kw = dict(kw)
            if name in ("lashes", "freckles"):
                kw.setdefault("seed", seed_of(look_id))
            img = EL[name](img, **kw)
        path = os.path.join(out_dir, f"makeup_{look_id}.png")
        to_img(img).save(path, optimize=True)
        print(f"  painted {os.path.basename(path)} "
              f"({os.path.getsize(path) // 1024} KB)")
    print(f"done — {len(ids)} atlas(es)")


if __name__ == "__main__":
    main()
