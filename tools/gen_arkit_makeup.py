#!/usr/bin/env python3
"""Bake ARKit-UV makeup atlases (docs/ARKIT_NATIVE_PLAN.md Phase 2).

The native tier-1 path renders ARKit's own face mesh textured in ARKit UV
space. Two atlas sources:

  1. Plate PNGs (MakeupStudio, authored in MediaPipe canonical UV space):
     resampled through a precomputed ARKit-UV -> MP-UV warp map derived from
     the canonical-mesh correspondence (offline use, where its error is a
     small pigment-placement offset, not per-frame motion).
  2. Builtin FaceFilter looks (procedural lash/liner/shadow/lip/blush/...):
     parsed straight out of face_filters.cpp, painted into MP UV space from
     landmark UV geometry, then warped like plates. Output: arkit/look_<id>.png

Also emits arkit/checker.png (Phase-1 alignment QA texture).

Usage: tools/gen_arkit_makeup.py [--assets ~/dev/pms-ios/Engine/EngineAssets]
"""
import argparse
import os
import re
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from scipy.spatial import cKDTree

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import gen_arkit_mp_map as G  # parse_obj / parse_header_array / umeyama / tps

SRC_GEN = os.path.join(here, "..", "src", "generated")
SIZE = 1024

# MP landmark index geometry (person's R = viewer-left in canonical UV).
LIP_OUT = [61, 185, 40, 39, 37, 0, 267, 269, 270, 409, 291,
           375, 321, 405, 314, 17, 84, 181, 91, 146]
LIP_IN = [78, 191, 80, 81, 82, 13, 312, 311, 310, 415, 308,
          324, 318, 402, 317, 14, 87, 178, 88, 95]
LASH_R = [33, 246, 161, 160, 159, 158, 157, 173, 133]
LASH_L = [263, 466, 388, 387, 386, 385, 384, 398, 362]
BROW_R = 105
BROW_L = 334
CHEEK_R = 50
CHEEK_L = 280
UNDEREYE_R = 100
UNDEREYE_L = 329
NOSE_TIP = 1
NOSE_BRIDGE = 6


def build_correspondence():
    """MP verts warped into ARKit space.

    Similarity fit + TPS with the MP lash contours PINNED to the ARKit
    eye-hole rims and the inner-lip ring pinned to the mouth hole. For the
    live RENDER that pinning was wrong (the hole edge is not the moving
    lash line) — but for a STATIC atlas bake it is exactly right: plate art
    authored along the MP lash line must land on the rim, which is where
    the lash roots live on the mesh. Similarity-only left ~5mm RMS and
    plate eyeliner floated above the lash line on device.
    """
    ak_v = G.parse_obj(os.path.join(here, "arkit_face_canonical.obj"))
    mp_v = G.parse_obj(os.path.join(here, "canonical_face_model.obj"))
    ak_tris = G.parse_header_array(
        os.path.join(SRC_GEN, "arkit_face_mesh.h"),
        "k_arkit_tris", (2304, 3)).astype(int)
    mi = [m for m, _ in G.ANCHORS]
    ai = [a for _, a in G.ANCHORS]
    s, R, t = G.umeyama(mp_v[mi], ak_v[ai])
    mp_w = s * (R @ mp_v.T).T + t

    ak_rings = G.boundary_rings(ak_tris, 1220)
    MP_EYE_R = [33, 246, 161, 160, 159, 158, 157, 173, 133,
                155, 154, 153, 145, 144, 163, 7]
    MP_EYE_L = [263, 466, 388, 387, 386, 385, 384, 398, 362,
                382, 381, 380, 374, 373, 390, 249]
    MP_LIPS_IN = [78, 191, 80, 81, 82, 13, 312, 311, 310, 415, 308,
                  324, 318, 402, 317, 14, 87, 178, 88, 95]
    pairs = [
        (MP_EYE_R, (1101, 1090), 33, 1101),
        (MP_EYE_L, (1069, 1080), 263, 1069),
        (MP_LIPS_IN, (249, 684), 78, 249),
    ]
    src_ctrl = [mp_w[mi]]
    dst_ctrl = [ak_v[ai]]
    # Identity pins on the brows: the rim pinning below must stay LOCAL to
    # the eyes — without these the TPS drags the whole brow region up with
    # the upper lash correction and plate brow art rendered ~1cm above the
    # real brows on device. (The similarity fit places brows about right:
    # the live overlay's brow landmarks sat on the user's brows.)
    BROWS = [70, 63, 105, 66, 107, 46, 53, 52, 65, 55,
             300, 293, 334, 296, 336, 276, 283, 282, 295, 285]
    src_ctrl.append(mp_w[BROWS])
    dst_ctrl.append(mp_w[BROWS])
    for mring, ak_t, mp_c, ak_c in pairs:
        aring = G.ring_near(ak_rings, ak_v, ak_v[list(ak_t)].mean(0))
        mring = G.orient_ring(list(mring), mp_w, mp_w[mp_c])
        aring = G.orient_ring(aring, ak_v, ak_v[ak_c])
        mpts = mp_w[mring]
        seg = np.sqrt(((np.roll(mpts, -1, 0) - mpts) ** 2).sum(1))
        ts = np.concatenate([[0.0], np.cumsum(seg)[:-1]]) / seg.sum()
        src_ctrl.append(mpts)
        dst_ctrl.append(G.ring_resample(ak_v[aring], ts))
    mp_w = G.tps_warp(np.vstack(src_ctrl), np.vstack(dst_ctrl), mp_w)
    return ak_v, mp_w


def parse_uvs():
    ak_uv = G.parse_header_array(os.path.join(SRC_GEN, "arkit_face_mesh.h"),
                                 "k_arkit_uv", (1220, 2))
    ak_tris = G.parse_header_array(os.path.join(SRC_GEN, "arkit_face_mesh.h"),
                                   "k_arkit_tris", (2304, 3)).astype(int)
    mp_uv = G.parse_header_array(os.path.join(SRC_GEN, "face_uv_mesh.h"),
                                 "k_face_uv", (468, 2))
    mp_tris = G.parse_header_array(os.path.join(SRC_GEN, "face_uv_mesh.h"),
                                   "k_face_tris", (898, 3)).astype(int)
    return ak_uv, ak_tris, mp_uv, mp_tris


def rasterize_positions(uv, tris, verts, size):
    """Per-texel 3D position map for a mesh in its own UV space.
    Returns (pos[size,size,3], mask[size,size])."""
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
        sel_y, sel_x = ys[inside], xs[inside]
        pos[sel_y, sel_x] = pt[inside]
        mask[sel_y, sel_x] = True
    return pos, mask


def build_warp_map(cache_path):
    """warp[y,x] = MP-UV pixel coords sampling the same skin as ARKit texel."""
    if os.path.exists(cache_path):
        z = np.load(cache_path)
        return z["warp"], z["mask"]
    ak_v, mp_w = build_correspondence()
    ak_uv, ak_tris, mp_uv, mp_tris = parse_uvs()
    print("rasterizing ARKit UV positions...")
    ak_pos, ak_mask = rasterize_positions(ak_uv, ak_tris, ak_v, SIZE)
    print("rasterizing MP UV positions...")
    mp_pos, mp_mask = rasterize_positions(mp_uv, mp_tris, mp_w, SIZE)
    mp_yx = np.argwhere(mp_mask)
    tree = cKDTree(mp_pos[mp_mask])
    q = ak_pos[ak_mask]
    print(f"kd-query {len(q)} texels...")
    dist, idx = tree.query(q, workers=-1)
    warp = np.zeros((SIZE, SIZE, 2), np.float32)
    warp[ak_mask] = mp_yx[idx][:, ::-1]  # store as (x, y)
    # Texels whose skin is far from any MP surface stay unmapped ->
    # transparent in every atlas. Tight tolerance: nearest-surface lookups
    # past the MP mesh edge SMEAR edge pixels (liner ink!) sideways across
    # the temple — on device that read as a rigid wing spike at 3/4 views.
    far = np.zeros((SIZE, SIZE), bool)
    far[ak_mask] = dist > 5.0  # mm
    mask = ak_mask & ~far
    np.savez_compressed(cache_path, warp=warp, mask=mask)
    print(f"cached {cache_path}")
    return warp, mask


def warp_image(img_rgba, warp, mask):
    src = np.asarray(img_rgba, np.uint8).astype(np.float32)
    if src.shape[0] != SIZE:
        src = np.asarray(img_rgba.resize((SIZE, SIZE)), np.uint8).astype(np.float32)
    out = np.zeros((SIZE, SIZE, 4), np.float32)
    xy = warp[mask]
    x0 = np.clip(np.floor(xy[:, 0]).astype(int), 0, SIZE - 2)
    y0 = np.clip(np.floor(xy[:, 1]).astype(int), 0, SIZE - 2)
    fx = np.clip(xy[:, 0] - x0, 0, 1)[:, None]
    fy = np.clip(xy[:, 1] - y0, 0, 1)[:, None]
    out[mask] = (src[y0, x0] * (1 - fx) * (1 - fy)
                 + src[y0, x0 + 1] * fx * (1 - fy)
                 + src[y0 + 1, x0] * (1 - fx) * fy
                 + src[y0 + 1, x0 + 1] * fx * fy)
    return Image.fromarray(out.astype(np.uint8))


# ── builtin-look parsing ────────────────────────────────────────────────────
INIT_FIELDS = ["smooth", "brighten", "warmth", "eye_pop", "blush", "lip",
               "eyes", "cheek", "vline", "nose", "lips_plump"]


def parse_looks(face_filters_cpp):
    """FaceFilter id -> {field: value} from the case blocks."""
    src = open(face_filters_cpp).read()
    hdr = open(face_filters_cpp.replace(".cpp", ".h")).read()
    enum = re.search(r"enum class FaceFilter \{(.*?)\};", hdr, re.S).group(1)
    ids, i = {}, 0
    # strip comments LINE-WISE first: enum comments contain commas and a
    # comma-split would mint phantom entries ("bronze contour"), shifting
    # every id after them.
    clean = "\n".join(re.sub(r"//.*", "", ln) for ln in enum.splitlines())
    for tok in clean.split(","):
        tok = tok.strip()
        if not tok:
            continue
        if "=" in tok:
            name, val = tok.split("=")
            i = int(val)
            ids[name.strip()] = i
        else:
            ids[tok] = i
        i += 1
    looks = {}
    for m in re.finditer(
            r"case FaceFilter::(\w+):(.*?)return true;", src, re.S):
        name, body = m.group(1), m.group(2)
        if name not in ids:
            continue
        lk = {}
        init = re.search(r"L\s*=\s*\{([^}]*)\}", body)
        if init:
            vals = [float(x) for x in re.findall(r"-?[\d.]+", init.group(1))]
            for k, v in zip(INIT_FIELDS, vals):
                lk[k] = v
        for mm in re.finditer(r"set3\(L\.(\w+),\s*([^)]*)\)", body):
            lk[mm.group(1)] = [float(x) for x in
                               re.findall(r"-?[\d.]+", mm.group(2))]
        for mm in re.finditer(r"L\.(\w+)\s*=\s*(-?[\d.]+)f?", body):
            lk[mm.group(1)] = float(mm.group(2))
        looks[ids[name]] = lk
    return looks


# ── MP-UV painter ───────────────────────────────────────────────────────────
def px(uv_pt):
    return (float(uv_pt[0]) * (SIZE - 1), float(uv_pt[1]) * (SIZE - 1))


def paint_look_mp(lk, mp_uv):
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    dr = ImageDraw.Draw(img)

    def col(c, a):
        return (int(c[0] * 255), int(c[1] * 255), int(c[2] * 255),
                int(np.clip(a, 0, 1) * 255))

    # blush (soft radial blobs, mid-cheek raised toward under-eye)
    blush = lk.get("blush", 0.0)
    if blush > 0.01:
        c = lk.get("blush_col", [1.0, 0.45, 0.55])
        raise_f = lk.get("blush_raise", 0.40)
        layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
        d2 = ImageDraw.Draw(layer)
        # blush apple: between the outer eye corner and the mouth corner,
        # pushed laterally outward — the cheek-center landmark sat too
        # medial on real faces (blobs hugged the nose).
        for eye_c, mouth_c, sgn in ((33, 61, -1.0), (263, 291, 1.0)):
            ex, ey = px(mp_uv[eye_c])
            mx2, my2 = px(mp_uv[mouth_c])
            bx = ex * 0.52 + mx2 * 0.48 + sgn * SIZE * 0.045
            by = ey * 0.55 + my2 * 0.45 - SIZE * 0.01 * raise_f
            r = SIZE * 0.075
            d2.ellipse([bx - r, by - r, bx + r, by + r],
                       fill=col(c, min(0.62 * blush, 0.75)))
        layer = layer.filter(ImageFilter.GaussianBlur(SIZE * 0.035))
        img = Image.alpha_composite(img, layer)
        dr = ImageDraw.Draw(img)

    # freckles / nose blush (e-girl)
    if lk.get("nose_blush", 0.0) > 0.01:
        layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
        d2 = ImageDraw.Draw(layer)
        nx, ny = px(mp_uv[NOSE_BRIDGE])
        r = SIZE * 0.055
        d2.ellipse([nx - r * 1.6, ny - r * 0.5, nx + r * 1.6, ny + r * 0.9],
                   fill=col([0.95, 0.45, 0.45], 0.5 * lk["nose_blush"]))
        layer = layer.filter(ImageFilter.GaussianBlur(SIZE * 0.02))
        img = Image.alpha_composite(img, layer)
        dr = ImageDraw.Draw(img)
    if lk.get("freckles", 0.0) > 0.01:
        rng = np.random.default_rng(7)
        layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
        d2 = ImageDraw.Draw(layer)
        nx, ny = px(mp_uv[NOSE_BRIDGE])
        for _ in range(55):
            fx = nx + rng.normal(0, SIZE * 0.062)
            fy = ny + rng.normal(0, SIZE * 0.026) + SIZE * 0.015
            r = rng.uniform(0.5, 1.1)
            a = 0.30 * lk["freckles"] * rng.uniform(0.4, 1.0)
            d2.ellipse([fx - r, fy - r, fx + r, fy + r],
                       fill=col([0.45, 0.28, 0.18], a))
        layer = layer.filter(ImageFilter.GaussianBlur(0.8))
        img = Image.alpha_composite(img, layer)
        dr = ImageDraw.Draw(img)

    # (eyeshadow/liner/lash/wing are painted DIRECTLY in ARKit UV along the
    # real eye-hole rim — see paint_eyes_arkit — so the precision-critical
    # elements never pass through the warp map.)
    shadow = 0.0
    _unused_shadow = lk.get("shadow", 0.0)
    if shadow > 0.01:
        c = lk.get("shadow_col", [0.35, 0.22, 0.30])
        layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
        d2 = ImageDraw.Draw(layer)
        for chain, brow in ((LASH_R, BROW_R), (LASH_L, BROW_L)):
            pts = [px(mp_uv[i]) for i in chain]
            bx, by = px(mp_uv[brow])
            mx = np.mean([p[0] for p in pts])
            my = np.mean([p[1] for p in pts])
            h = min(0.42 + 0.30 * shadow, 0.85)
            up = [(p[0] + (bx - mx) * h, p[1] + (by - my) * h) for p in pts]
            d2.polygon(pts + up[::-1], fill=col(c, min(0.60 * shadow, 0.78)))
        layer = layer.filter(ImageFilter.GaussianBlur(SIZE * 0.014))
        img = Image.alpha_composite(img, layer)
        dr = ImageDraw.Draw(img)

    liner = 0.0
    _unused_liner = lk.get("liner", 0.0)
    if liner > 0.01:
        wpx = max(2.0, SIZE * 0.004 * (0.7 + 0.6 * min(liner, 2.0)))
        for chain in (LASH_R, LASH_L):
            pts = [px(mp_uv[i]) for i in chain]
            dr.line(pts, fill=(12, 8, 12, int(240 * min(liner, 1.0))),
                    width=int(wpx), joint="curve")
    wing = 0.0
    _unused_wing = lk.get("lash_wing", 0.0)
    if wing > 0.01:
        for chain, sgn in ((LASH_R, -1.0), (LASH_L, 1.0)):
            o = np.array(px(mp_uv[chain[0]]))
            n1 = np.array(px(mp_uv[chain[1]]))
            d = o - n1
            d = d / (np.linalg.norm(d) + 1e-6)
            tip = o + d * SIZE * 0.035 * wing + np.array([0, -SIZE * 0.012])
            dr.line([tuple(o), tuple(tip)],
                    fill=(12, 8, 12, 235), width=max(2, int(SIZE * 0.005)))

    lash = 0.0
    _unused_lash = lk.get("lash", 0.0)
    if lash > 0.01:
        rng = np.random.default_rng(3)
        for chain in (LASH_R, LASH_L):
            pts = np.array([px(mp_uv[i]) for i in chain], float)
            n = int(10 + 8 * min(lash, 2.0))
            for k in range(n):
                t = (k + 0.5) / n
                seg = t * (len(pts) - 1)
                i0 = min(int(seg), len(pts) - 2)
                f = seg - i0
                base = pts[i0] * (1 - f) + pts[i0 + 1] * f
                dirv = pts[i0 + 1] - pts[i0]
                nrm = np.array([-dirv[1], dirv[0]])
                nrm = nrm / (np.linalg.norm(nrm) + 1e-6)
                if nrm[1] > 0:
                    nrm = -nrm  # up = toward brow (smaller y in MP UV? sign-checked below)
                ln = SIZE * (0.010 + 0.008 * min(lash, 1.6)) \
                    * rng.uniform(0.7, 1.25)
                tip = base + nrm * ln + dirv / (np.linalg.norm(dirv) + 1e-6) \
                    * ln * 0.35 * (1 - t)
                dr.line([tuple(base), tuple(tip)],
                        fill=(10, 7, 10, int(200 * min(lash, 1.0))), width=2)

    # lip: outer ring filled, inner ring (aperture) cleared
    lip = lk.get("lip", 0.0)
    if lip > 0.01:
        c = lk.get("lip_col", [0.95, 0.25, 0.35])
        layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
        d2 = ImageDraw.Draw(layer)
        d2.polygon([px(mp_uv[i]) for i in LIP_OUT],
                   fill=col(c, min(0.80 * lip, 0.9)))
        d2.polygon([px(mp_uv[i]) for i in LIP_IN], fill=(0, 0, 0, 0))
        layer = layer.filter(ImageFilter.GaussianBlur(SIZE * 0.0022))
        img = Image.alpha_composite(img, layer)

    return img


# ── ARKit-direct eye painter ────────────────────────────────────────────────
# The eye-hole rims ARE the lash lines on the live mesh; painting along them
# in ARKit UV puts liner/lash/shadow exactly at the roots with zero warp
# error. Arc indices are constants of ARKit topology (x-ordered outer→inner
# per eye; see arkit_map_smoke.cpp).
# Arcs listed OUTER -> INNER to match the (outer, inner) corner pairs — the
# x-sorted list runs the other way for the left eye, and a mismatched rim
# order draws the liner as a diagonal slice across the eye.
ARC_UP_R = [1100, 1099, 1098, 1097, 1096, 1095, 1094, 1093, 1092, 1091]
ARC_UP_L = [1070, 1071, 1072, 1073, 1074, 1075, 1076, 1077, 1078, 1079]
CORNERS_R = (1101, 1090)   # outer, inner
CORNERS_L = (1069, 1080)


def paint_eyes_arkit(lk, ak_uv):
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    dr = ImageDraw.Draw(img)

    def puv(i):
        return (float(ak_uv[i][0]) * (SIZE - 1), float(ak_uv[i][1]) * (SIZE - 1))

    def col(c, a):
        return (int(c[0] * 255), int(c[1] * 255), int(c[2] * 255),
                int(np.clip(a, 0, 1) * 255))

    for arc, corners in ((ARC_UP_R, CORNERS_R), (ARC_UP_L, CORNERS_L)):
        rim = [puv(corners[0])] + [puv(i) for i in arc] + [puv(corners[1])]
        rim_np = np.array(rim, float)
        center = rim_np.mean(0)
        # "up" in UV = away from the hole center at the rim (per point)
        def outward(p):
            d = p - center
            n = np.linalg.norm(d) + 1e-6
            return d / n

        # eyeshadow: soft band hugging the rim, taller mid-eye
        shadow = lk.get("shadow", 0.0)
        if shadow > 0.01:
            c = lk.get("shadow_col", [0.35, 0.22, 0.30])
            layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
            d2 = ImageDraw.Draw(layer)
            band = SIZE * (0.028 + 0.020 * min(shadow, 2.0))
            upper = []
            for k2, p2 in enumerate(rim_np):
                t = k2 / (len(rim_np) - 1)
                lift = np.sin(np.pi * t) * band + band * 0.25
                upper.append(tuple(p2 + outward(p2) * lift))
            d2.polygon([tuple(p2) for p2 in rim_np] + upper[::-1],
                       fill=col(c, min(0.62 * shadow, 0.80)))
            layer = layer.filter(ImageFilter.GaussianBlur(SIZE * 0.011))
            img = Image.alpha_composite(img, layer)
            dr = ImageDraw.Draw(img)

        # Smooth rim curve: quadratic fit through the rim points in the
        # corner-to-corner frame. Drawing vertex-to-vertex bent into hooks
        # at the ends where the rim verts stack near the corners.
        u_ax = rim_np[-1] - rim_np[0]
        u_len = np.linalg.norm(u_ax) + 1e-6
        u_ax = u_ax / u_len
        v_ax = np.array([-u_ax[1], u_ax[0]])
        us = (rim_np - rim_np[0]) @ u_ax
        vs = (rim_np - rim_np[0]) @ v_ax
        qa, qb, qc = np.polyfit(us, vs, 2)

        def rim_curve(t):
            uu = t * u_len
            vv = qa * uu * uu + qb * uu + qc
            return rim_np[0] + u_ax * uu + v_ax * vv

        def rim_tangent(t):
            uu = t * u_len
            d = u_ax + v_ax * (2 * qa * uu + qb)
            return d / (np.linalg.norm(d) + 1e-6)

        # liner: along the fitted curve, inset toward the hole, trimmed
        # short of the oversized corners; the wing is the curve's own
        # tangent continued past the outer end — no separate segment, so
        # no kink.
        liner = lk.get("liner", 0.0)
        if liner > 0.01:
            w = max(4, int(SIZE * 0.005 * (0.7 + 0.6 * min(liner, 2.0))))
            pts2 = []
            wing = lk.get("lash_wing", 0.0)
            if wing > 0.01:
                tip = rim_curve(0.08) - rim_tangent(0.08) * SIZE * 0.014 * wing
                tip = tip + outward(rim_curve(0.08)) * SIZE * 0.004 * wing
                pts2.append(tuple(tip))
            for t in np.linspace(0.08, 0.92, 22):
                p2 = rim_curve(t) - outward(rim_curve(t)) * w * 0.45
                pts2.append(tuple(p2))
            dr.line(pts2, fill=(12, 8, 12, int(235 * min(liner, 1.0))),
                    width=w, joint="curve")

        # wing: from the outer corner, along the rim tangent, gentle lift
        # lash fringe: strokes rooted on the fitted curve, fanning outward
        lash = lk.get("lash", 0.0)
        if lash > 0.01:
            rng = np.random.default_rng(11)
            n = int(12 + 9 * min(lash, 2.0))
            for k2 in range(n):
                t = 0.08 + 0.84 * (k2 + 0.5) / n
                base = rim_curve(t)
                d = outward(base)
                tangent = rim_tangent(t)
                ln = SIZE * (0.008 + 0.006 * min(lash, 1.6)) \
                    * rng.uniform(0.7, 1.25)
                base = base - d * SIZE * 0.0015
                tip = base + d * ln + tangent * ln * rng.uniform(-0.25, 0.25)
                dr.line([tuple(base), tuple(tip)],
                        fill=(10, 7, 10, int(210 * min(lash, 1.0))), width=3)
    return img


def checker():
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    a = np.zeros((SIZE, SIZE, 4), np.uint8)
    cell = SIZE // 32
    yy, xx = np.mgrid[0:SIZE, 0:SIZE]
    chk = ((xx // cell + yy // cell) % 2).astype(bool)
    a[chk] = (255, 40, 200, 170)
    a[~chk] = (40, 255, 120, 170)
    return Image.fromarray(a)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--assets", default=os.path.expanduser(
        "~/dev/pms-ios/Engine/EngineAssets"))
    args = ap.parse_args()
    face_dir = os.path.join(args.assets, "models", "face")
    out_dir = os.path.join(face_dir, "arkit")
    os.makedirs(out_dir, exist_ok=True)

    warp, mask = build_warp_map(os.path.join(here, "arkit_uv_warp.npz"))
    ak_uv_g, _, mp_uv, _ = parse_uvs()

    checker().save(os.path.join(out_dir, "checker.png"))
    print("checker.png")

    # plates — with the brow art ERASED first: ARKit fits the face's 3D
    # shape, not where the eyebrow hair sits, so painted brows always fight
    # the user's real brows by a per-person offset. The user has eyebrows.
    brow_mask = Image.new("L", (SIZE, SIZE), 0)
    bd = ImageDraw.Draw(brow_mask)
    for ring in ([70, 63, 105, 66, 107, 55, 65, 52, 53, 46],
                 [300, 293, 334, 296, 336, 285, 295, 282, 283, 276]):
        pts = [px(mp_uv[i]) for i in ring]
        cx = sum(p2[0] for p2 in pts) / len(pts)
        cy = sum(p2[1] for p2 in pts) / len(pts)
        grown = [(cx + (p2[0] - cx) * 1.55, cy + (p2[1] - cy) * 1.9)
                 for p2 in pts]
        bd.polygon(grown, fill=255)
    brow_mask = brow_mask.filter(ImageFilter.GaussianBlur(SIZE * 0.008))
    brow_np = np.asarray(brow_mask, np.float32) / 255.0

    # Wing limiter: plate wing art that extends far past the outer eye
    # corner rides temple geometry that folds at yaw — the tip juts off the
    # face as a rigid spike. Fade plate alpha laterally beyond the corners.
    wing_mask = np.ones((SIZE, SIZE), np.float32)
    yy, xx = np.mgrid[0:SIZE, 0:SIZE].astype(np.float32)
    for outer_i, sgn in ((33, -1.0), (263, 1.0)):
        cx2, cy2 = px(mp_uv[outer_i])
        beyond = (xx - cx2) * sgn - SIZE * 0.022
        band = np.exp(-((yy - cy2) ** 2) / (2 * (SIZE * 0.060) ** 2))
        fade = np.clip(beyond / (SIZE * 0.020), 0.0, 1.0) * band
        wing_mask *= (1.0 - fade)

    # Post-warp limiter in ARKIT UV: eye-corner ink lands on the temple-side
    # vertex rows THROUGH the warp (traced from an on-device fixture — the
    # rigid "wing spike" at 3/4 views), so it must be erased on the output
    # side, laterally beyond the hole corners. Corner UVs are topology
    # constants: R outer 1101 (0.272, 0.651), L outer 1069 (0.728, 0.651).
    out_mask = np.ones((SIZE, SIZE), np.float32)
    yy2, xx2 = np.mgrid[0:SIZE, 0:SIZE].astype(np.float32)
    for (cu, cv), sgn in (((0.272, 0.651), -1.0), ((0.728, 0.651), 1.0)):
        cx3, cy3 = cu * SIZE, cv * SIZE
        beyond = (xx2 - cx3) * sgn - SIZE * 0.012
        band = np.exp(-((yy2 - cy3) ** 2) / (2 * (SIZE * 0.10) ** 2))
        fade = np.clip(beyond / (SIZE * 0.018), 0.0, 1.0) * band
        out_mask *= (1.0 - fade)

    plates = sorted(f for f in os.listdir(face_dir)
                    if f.startswith("makeup_") and f.endswith(".png"))
    for f in plates:
        img = Image.open(os.path.join(face_dir, f)).convert("RGBA")
        arr = np.asarray(img.resize((SIZE, SIZE)), np.uint8).astype(np.float32)
        arr[..., 3] *= (1.0 - brow_np) * wing_mask
        img = Image.fromarray(arr.astype(np.uint8))
        out = warp_image(img, warp, mask)
        oarr = np.asarray(out, np.uint8).astype(np.float32)
        oarr[..., 3] *= out_mask
        Image.fromarray(oarr.astype(np.uint8)).save(os.path.join(out_dir, f))
    print(f"{len(plates)} plates warped (brow erased, wings limited, "
          f"corner spill cut)")

    # builtin looks
    looks = parse_looks(os.path.join(here, "..", "src", "face_filters.cpp"))
    n = 0
    for fid, lk in sorted(looks.items()):
        pigment = any(lk.get(k, 0) > 0.01 for k in
                      ("blush", "lip", "shadow", "liner", "lash",
                       "freckles", "nose_blush"))
        if not pigment:
            continue
        mp_img = paint_look_mp(lk, mp_uv)
        atlas = warp_image(mp_img, warp, mask)
        atlas = Image.alpha_composite(atlas, paint_eyes_arkit(lk, ak_uv_g))
        aarr = np.asarray(atlas, np.uint8).astype(np.float32)
        aarr[..., 3] *= out_mask
        Image.fromarray(aarr.astype(np.uint8)).save(
            os.path.join(out_dir, f"look_{fid}.png"))
        n += 1
    print(f"{n} builtin looks baked")


if __name__ == "__main__":
    main()
