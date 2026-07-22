#!/usr/bin/env python3
"""ARKit makeup atlas painter — 3D-native. Every element is painted ON the
canonical ARKit head in MILLIMETERS (lid rim polylines, lip ring, cheek
anchors), then rasterized through the mesh's own triangles into its UV
texture — the same paint-on-the-model workflow face-texture artists use,
which makes placement correct by construction. This replaces the UV-space
curve painter: 2D rim fits in texture space flipped/misplaced liner on
device ("eyeliner all fucked up").

Per-texel 3D position comes from rasterize_positions() (same code the warp
map uses). Distances are anatomic: liner width in mm at the lash roots,
shadow bands in mm above the lid rim, lip height in mm off the mouth ring.

Output: models/face/arkit/makeup_<id>.png (RGBA 1024^2).
Usage: tools/gen_arkit_3d_makeup.py [--only id ...] [--assets <dir>]
"""
import argparse
import hashlib
import os
import sys

import numpy as np
from PIL import Image

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import gen_arkit_mp_map as G
import gen_arkit_makeup as B   # parse_uvs, rasterize_positions, SIZE

SIZE = B.SIZE

# ARKit topology constants (validated in arkit_map_smoke.cpp)
ARC_UP_R = [1100, 1099, 1098, 1097, 1096, 1095, 1094, 1093, 1092, 1091]
ARC_UP_L = [1070, 1071, 1072, 1073, 1074, 1075, 1076, 1077, 1078, 1079]
CORNERS_R = (1101, 1090)   # outer, inner
CORNERS_L = (1069, 1080)
MOUTH_TARGET = (249, 684)

# canonical head (mm)
AK_V = G.parse_obj(os.path.join(here, "arkit_face_canonical.obj"))
AK_UV, AK_TRIS, _, _ = B.parse_uvs()

print("rasterizing canonical head into UV (once)...")
POS, MASK = B.rasterize_positions(AK_UV, AK_TRIS, AK_V, SIZE)
TX = POS[MASK]                       # (N,3) texel 3D positions, mm
NTX = TX.shape[0]

rings = G.boundary_rings(AK_TRIS, 1220)


def ring_3d(target_verts):
    c = AK_V[list(target_verts)].mean(0)
    return G.ring_near(rings, AK_V, c)


class Eye:
    def __init__(self, corners, up_arc):
        ring = ring_3d(corners)
        up = [AK_V[c] for c in [corners[0]] + list(up_arc) + [corners[1]]]
        low_ids = [v for v in ring if v not in up_arc and v not in corners]
        o, i = AK_V[corners[0]], AK_V[corners[1]]
        ax = i - o
        ax = ax / np.linalg.norm(ax)
        low_ids.sort(key=lambda v: float((AK_V[v] - o) @ ax))
        low = [AK_V[corners[0]]] + [AK_V[v] for v in low_ids] + [AK_V[corners[1]]]
        self.up = np.array(up)          # upper rim polyline, outer->inner
        self.low = np.array(low)        # lower rim polyline, outer->inner
        self.center = self.up.mean(0)
        self.outer = AK_V[corners[0]]
        self.inner = AK_V[corners[1]]


EYES = [Eye(CORNERS_R, ARC_UP_R), Eye(CORNERS_L, ARC_UP_L)]
MOUTH_IDS = ring_3d(MOUTH_TARGET)
_mpts = AK_V[np.array(MOUTH_IDS)]
_mc = _mpts.mean(0)
_ang = np.arctan2(_mpts[:, 1] - _mc[1], _mpts[:, 0] - _mc[0])
MOUTH = _mpts[np.argsort(_ang)]       # mouth ring, CCW around center
MOUTH_C = _mc

# face anchors (mm) — from the canonical head directly
EYE_D = float(np.linalg.norm(EYES[0].center - EYES[1].center))
NOSE_TIP = AK_V[int(np.argmax(AK_V[:, 2]))]
NOSE_BRIDGE = (EYES[0].center + EYES[1].center + NOSE_TIP) / 3.0
CHIN = AK_V[int(np.argmin(AK_V[:, 1]))]
CHEEK = [AK_V[311], AK_V[746]]        # cheekbones (validated ref verts)
JAW = [AK_V[62], AK_V[511]]
FOREHEAD = AK_V[956]
CUPID = MOUTH_C + np.array([0, 4.0, 3.0])


def seed_of(s):
    return int(hashlib.sha256(s.encode()).hexdigest(), 16) % (2 ** 32)


def new_img():
    return np.zeros((NTX, 4), np.float64)


def to_img(arr):
    out = np.zeros((SIZE, SIZE, 4), np.uint8)
    out[MASK] = np.clip(arr, 0, 255).astype(np.uint8)
    return Image.fromarray(out)


def over(base, l):
    """Alpha-composite layer l over base (NTX,4 float arrays)."""
    fa = l[:, 3:4] / 255.0
    ba = base[:, 3:4] / 255.0
    oa = fa + ba * (1 - fa)
    rgb = (l[:, :3] * fa + base[:, :3] * ba * (1 - fa)) / np.maximum(oa, 1e-6)
    return np.concatenate([rgb, oa * 255.0], 1)


def dist_to_polyline(P, poly):
    """(N,3) points to 3D polyline: min distance, and segment t of closest."""
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
        # global parameter along the whole polyline (corner->corner)
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


# ── elements ────────────────────────────────────────────────────────────────

def el_blush(cv, img, style, color, alpha, seed=0):
    rng = np.random.default_rng(seed + 1)
    l = new_img()
    for cheek, eye in zip(CHEEK, EYES):
        if style == "band":
            l = over(l, blob(cheek, 14, color, alpha * 0.65))
            l = over(l, blob((cheek + NOSE_BRIDGE) * 0.5, 10, color, alpha))
        elif style == "lifted":
            l = over(l, blob(cheek * 0.7 + eye.center * 0.3, 11, color, alpha))
        elif style == "apple":
            l = over(l, blob(cheek * 0.6 + MOUTH_C * 0.4, 10, color, alpha))
        elif style == "sunkissed":
            l = over(l, blob(cheek, 15, color, alpha * 0.75))
            l = over(l, blob((cheek + NOSE_TIP) * 0.5, 10, color, alpha * 0.8))
        else:  # cheeks
            l = over(l, blob(cheek, 13, color, alpha))
    if style == "band":
        l = over(l, blob(NOSE_BRIDGE + rng.normal(0, 1, 3), 9, color, alpha * 0.6))
    return over(img, l)


def el_contour(cv, img, style="soft", color=None, alpha=40, areas=None, seed=0):
    if color is None:
        color = {"warm": (120, 85, 70), "cool": (90, 70, 80),
                 "soft": (105, 85, 78)}[style]
    if areas is None:
        areas = ["cheek", "jaw", "temple", "nose"]
    l = new_img()
    for cheek, jaw, eye in zip(CHEEK, JAW, EYES):
        if "cheek" in areas:
            l = over(l, blob(cheek * 0.5 + jaw * 0.5, 12, color, alpha))
        if "jaw" in areas:
            l = over(l, blob(jaw * 0.6 + CHIN * 0.4, 11, color, alpha * 0.8))
        if "temple" in areas:
            t = eye.center + (eye.center - MOUTH_C) * 0.55
            t[1] += 12
            l = over(l, blob(t, 10, color, alpha * 0.6))
    if "nose" in areas:
        nb = NOSE_BRIDGE * 0.4 + NOSE_TIP * 0.6
        side = np.array([1.0, 0, 0])
        for sgn in (-1, 1):
            l = over(l, blob(nb + side * sgn * 6.5, 5, color, alpha * 0.7))
    return over(img, l)


def _shade(alpha, d, inner, outer):
    """Alpha for a pigment band starting at the rim: full inside [inner,outer] mm."""
    return alpha * smooth(inner - 0.4, inner + 0.8, d) * (1 - smooth(outer - 1.2, outer + 0.8, d))


def el_shadow(cv, img, lid=None, crease=None, outer=None, inner=None,
              shimmer=None, lower=None, height=1.0, seed=0):
    l = new_img()
    for eye in EYES:
        d, t = dist_to_polyline(TX, eye.up)
        up_dir = TX - eye.center
        up_dir = up_dir / (np.linalg.norm(up_dir, axis=1, keepdims=True) + 1e-9)
        above = (up_dir[:, 1] > -0.15)  # hemisphere gate: lid pigment stays above the rim's underside
        if lid:
            c, a = lid
            al = _shade(a, d, 0.2, 3.2 * height) * above
            lay = new_img(); lay[:, :3] = c; lay[:, 3] = al
            l = over(l, lay)
        if crease:
            c, a = crease
            al = _shade(a, d, 2.6 * height, 7.0 * height) * above
            lay = new_img(); lay[:, :3] = c; lay[:, 3] = al
            l = over(l, lay)
        if outer:
            c, a = outer
            w = np.clip(1.0 - t / 0.38, 0, 1)   # outer third of the rim
            al = _shade(a, d, 0.4, 5.5 * height) * w * above
            lay = new_img(); lay[:, :3] = c; lay[:, 3] = al
            l = over(l, lay)
        if inner:
            c, a = inner
            w = np.clip((t - 0.72) / 0.28, 0, 1)
            al = _shade(a, d, 0.2, 3.0) * w
            lay = new_img(); lay[:, :3] = c; lay[:, 3] = al
            l = over(l, lay)
        if shimmer:
            c, a = shimmer
            w = np.exp(-((t - 0.5) ** 2) / (2 * 0.16 ** 2))
            al = _shade(a, d, 0.8, 3.4 * height) * w * above
            lay = new_img(); lay[:, :3] = c; lay[:, 3] = al
            l = over(l, lay)
        if lower:
            c, a = lower
            dl, tl = dist_to_polyline(TX, eye.low)
            below_dir = TX - eye.center
            below_dir = below_dir / (np.linalg.norm(below_dir, axis=1, keepdims=True) + 1e-9)
            below = (below_dir[:, 1] < 0.25)
            al = _shade(a, dl, 0.2, 2.6) * below
            lay = new_img(); lay[:, :3] = c; lay[:, 3] = al
            l = over(l, lay)
    return over(img, l)


LINER = {
    "tightline": dict(w=0.55, wing=0.0, alpha=170),
    "soft":      dict(w=0.85, wing=0.0, alpha=195),
    "wing":      dict(w=1.05, wing=4.0, alpha=230),
    "siren":     dict(w=0.95, wing=6.0, alpha=235),
    "graphic":   dict(w=1.5,  wing=4.5, alpha=240),
}


def el_liner(cv, img, style="soft", color=(16, 10, 14), alpha=None, seed=0):
    st = LINER[style]
    a = alpha if alpha is not None else st["alpha"]
    l = new_img()
    for eye in EYES:
        d, t = dist_to_polyline(TX, eye.up)
        ink = (1 - smooth(st["w"] - 0.25, st["w"] + 0.35, d)) * a
        if st["wing"] > 0:
            # wing: continuation of the rim past the outer corner, along the
            # rim end tangent with a slight upward lift — measured in mm
            tan = eye.up[0] - eye.up[1]
            tan = tan / np.linalg.norm(tan)
            upv = np.array([0.0, 1.0, 0.0])
            wdir = tan * 0.9 + upv * 0.25
            wdir = wdir / np.linalg.norm(wdir)
            tip = eye.outer + wdir * st["wing"]
            wd, wt = dist_to_polyline(TX, np.array([eye.outer, tip]))
            wing_ink = (1 - smooth(st["w"] * 0.8 - 0.2, st["w"] * 0.8 + 0.3, wd)) \
                * (1 - smooth(0.55, 0.98, wt)) * a
            ink = np.maximum(ink, wing_ink)
        lay = new_img(); lay[:, :3] = color; lay[:, 3] = ink
        l = over(l, lay)
    return over(img, l)


def el_lashes(cv, img, strength=0.8, style="doll", lower=False,
              color=(12, 8, 12), seed=0):
    rng = np.random.default_rng(seed + 5)
    l = new_img()
    n_per = {"wispy": 13, "doll": 9, "cat": 11, "stage": 15}[style]
    for eye in EYES:
        n = int(n_per * min(strength, 1.5))
        rim = eye.up
        for k in range(n):
            if style == "cat":
                t = 0.04 + 0.55 * (k + 0.5) / n
            else:
                t = 0.06 + 0.88 * (k + 0.5) / n
            f = t * (len(rim) - 1)
            i0 = min(int(f), len(rim) - 2)
            root = rim[i0] * (1 - f + i0) + rim[i0 + 1] * (f - i0)
            out = root - eye.center
            out[1] = abs(out[1]) + 2.0          # fan upward
            out = out / (np.linalg.norm(out) + 1e-9)
            tan = rim[i0 + 1] - rim[i0]
            tan = tan / (np.linalg.norm(tan) + 1e-9)
            ln = 3.2 * (0.6 + 0.5 * min(strength, 1.4)) * rng.uniform(0.7, 1.3)
            if style == "doll":
                ln *= 1.15
            sweep = rng.uniform(-0.30, 0.10) if style != "cat" else -0.45
            tip = root + out * ln + tan * ln * sweep
            d, wt = dist_to_polyline(TX, np.array([root, tip]))
            w = 0.42 * (1 - 0.55 * wt)          # taper to the tip
            ink = (1 - smooth(w - 0.15, w + 0.25, d)) * 230 * min(strength, 1.2)
            lay = new_img(); lay[:, :3] = color; lay[:, 3] = ink
            l = over(l, lay)
        if lower:
            m = max(4, int(6 * strength))
            rim2 = eye.low
            for k in range(m):
                t = 0.15 + 0.7 * (k + 0.5) / m
                f = t * (len(rim2) - 1)
                i0 = min(int(f), len(rim2) - 2)
                root = rim2[i0] * (1 - f + i0) + rim2[i0 + 1] * (f - i0)
                out = root - eye.center
                out[1] = -abs(out[1]) - 2.0
                out = out / (np.linalg.norm(out) + 1e-9)
                ln = 1.8 * rng.uniform(0.7, 1.2)
                tip = root + out * ln
                d, wt = dist_to_polyline(TX, np.array([root, tip]))
                w = 0.32 * (1 - 0.5 * wt)
                ink = (1 - smooth(w - 0.12, w + 0.2, d)) * 180 * min(strength, 1.2)
                lay = new_img(); lay[:, :3] = color; lay[:, 3] = ink
                l = over(l, lay)
    return over(img, l)


def el_lip(cv, img, style="satin", color=(190, 60, 80), alpha=130,
           liner_color=None, seed=0):
    ring = MOUTH
    # signed distance to the mouth ring + angle around the mouth center
    d, _ = dist_to_polyline(TX, np.vstack([ring, ring[:1]]))
    rel = TX - MOUTH_C
    ang = np.arctan2(rel[:, 1], rel[:, 0])
    # outside = farther from center than the ring (ring radius per angle)
    ring_r = np.linalg.norm(ring - MOUTH_C, axis=1)
    ring_ang = np.arctan2(ring[:, 1] - MOUTH_C[1], ring[:, 0] - MOUTH_C[0])
    order = np.argsort(ring_ang)
    rr = np.interp(ang, ring_ang[order], ring_r[order], period=2 * np.pi)
    outside = np.linalg.norm(rel, axis=1) - rr          # >0 outside the ring
    lowness = np.clip(-np.sin(ang), 0, 1)               # head y-up: bottom of lip
    upness = np.clip(np.sin(ang), 0, 1)
    h_full = 3.2 + 2.6 * lowness + 1.6 * upness         # mm of lip skin
    if style == "overline":
        h_full *= 1.35
    edge = smooth(-0.4, 0.3, outside) * (1 - smooth(h_full - 1.0, h_full + 0.8, outside))

    l = new_img()
    lay = new_img(); lay[:, :3] = color; lay[:, 3] = edge * alpha
    if style == "blurred":
        lay[:, 3] = edge * alpha * (0.55 + 0.45 * lowness)
    l = over(l, lay)
    if liner_color is not None:   # liner rides the vermilion edge, on top
        band = smooth(-0.4, 0.2, outside) * (1 - smooth(1.2, 2.2, outside))
        lay = new_img(); lay[:, :3] = liner_color; lay[:, 3] = band * alpha * 0.8
        l = over(l, lay)
    if style in ("gloss", "satin", "lined_gloss"):
        gl = blob(MOUTH_C + np.array([0, -3.5, 4.0]), 3.2, (255, 250, 248),
                  alpha * (0.5 if style != "satin" else 0.32))
        gl[:, 3] *= edge
        l = over(l, gl)
    # clear the aperture
    inside = 1 - smooth(-0.6, 0.2, outside)
    l[:, 3] *= (1 - inside)
    return over(img, l)


def el_highlight(cv, img, style="satin", color=(255, 246, 240), alpha=45, seed=0):
    l = new_img()
    for cheek, eye in zip(CHEEK, EYES):
        p = cheek * 0.45 + eye.center * 0.55
        p[1] += 6
        l = over(l, blob(p, 8, color, alpha))
    l = over(l, blob(NOSE_BRIDGE * 0.65 + NOSE_TIP * 0.35, 4, color, alpha * 0.8))
    l = over(l, blob(NOSE_TIP + np.array([0, 2, 1]), 2.5, color, alpha * 0.7))
    l = over(l, blob(CUPID, 3, color, alpha * 0.6))
    return over(img, l)


def el_freckles(cv, img, density=1.0, color=(150, 95, 70), alpha=100, seed=0):
    rng = np.random.default_rng(seed + 7)
    l = new_img()
    n = int(46 * density)
    cx = (CHEEK[0][0] + CHEEK[1][0]) / 2
    cy = NOSE_BRIDGE[1] - 2
    for _ in range(n):
        fx = cx + rng.normal(0, 26)
        fy = cy + rng.normal(0, 7)
        p = np.array([fx, fy, NOSE_BRIDGE[2] + rng.normal(2, 3)])
        r = rng.uniform(0.35, 0.9)
        a = alpha * rng.uniform(0.35, 1.0)
        warm = rng.uniform(0.9, 1.1)
        l = over(l, blob(p, r, (color[0] * warm, color[1], color[2]), a))
    return over(img, l)


def el_aegyo(cv, img, alpha=18, seed=0):
    l = new_img()
    for eye in EYES:
        d, t = dist_to_polyline(TX, eye.low)
        below_dir = TX - eye.center
        below = (below_dir[:, 1] < 0)
        w = np.exp(-((t - 0.5) ** 2) / (2 * 0.3 ** 2))
        al = _shade(alpha, d, 0.4, 2.2) * below * w
        lay = new_img(); lay[:, :3] = (255, 244, 240); lay[:, 3] = al
        l = over(l, lay)
    return over(img, l)


# ── recipes ─────────────────────────────────────────────────────────────────
def S(lid=None, crease=None, outer=None, inner=None, shimmer=None, lower=None):
    return dict(lid=lid, crease=crease, outer=outer, inner=inner,
                shimmer=shimmer, lower=lower)


LOOKS = {}


def _load_recipes():
    """Shared recipe table — same art direction as the (retired) UV painter,
    but alphas retuned for the 3D renderer's exact band edges."""
    import importlib
    m = importlib.import_module("gen_arkit_native_makeup")
    return m.LOOKS


EL = {
    "blush": el_blush, "contour": el_contour, "shadow": el_shadow,
    "liner": el_liner, "lashes": el_lashes, "lip": el_lip,
    "highlight": el_highlight, "freckles": el_freckles, "aegyo": el_aegyo,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--assets", default=os.path.expanduser(
        "~/dev/pms-ios/Engine/EngineAssets"))
    ap.add_argument("--only", nargs="*", default=None)
    args = ap.parse_args()
    out_dir = os.path.join(args.assets, "models", "face", "arkit")
    os.makedirs(out_dir, exist_ok=True)

    looks = _load_recipes()
    ids = args.only or list(looks.keys())
    for look_id in ids:
        recipe = looks[look_id]
        img = new_img()
        for name, kw in recipe:
            kw = dict(kw)
            kw.setdefault("seed", seed_of(look_id))
            img = EL[name](None, img, **kw)
        path = os.path.join(out_dir, f"makeup_{look_id}.png")
        to_img(img).save(path, optimize=True)
        print(f"  painted {os.path.basename(path)} "
              f"({os.path.getsize(path) // 1024} KB)")
    print(f"done — {len(ids)} 3D-native atlases")


if __name__ == "__main__":
    main()
