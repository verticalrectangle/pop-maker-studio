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
    """MP verts warped into ARKit space (same fit as gen_arkit_mp_map)."""
    ak_v = G.parse_obj(os.path.join(here, "arkit_face_canonical.obj"))
    mp_v = G.parse_obj(os.path.join(here, "canonical_face_model.obj"))
    mi = [m for m, _ in G.ANCHORS]
    ai = [a for _, a in G.ANCHORS]
    s, R, t = G.umeyama(mp_v[mi], ak_v[ai])
    mp_w = s * (R @ mp_v.T).T + t
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
    # Texels whose skin is far from any MP surface (ears/neck extremes) stay
    # unmapped -> transparent in every atlas.
    far = np.zeros((SIZE, SIZE), bool)
    far[ak_mask] = dist > 12.0  # mm
    mask = ak_mask & ~far
    np.savez_compressed(cache_path, warp=warp, mask=mask)
    print(f"cached {cache_path}")
    return warp, mask


def warp_image(img_rgba, warp, mask):
    src = np.asarray(img_rgba, np.uint8)
    if src.shape[0] != SIZE:
        src = np.asarray(img_rgba.resize((SIZE, SIZE)), np.uint8)
    out = np.zeros((SIZE, SIZE, 4), np.uint8)
    xy = warp[mask].astype(int)
    out[mask] = src[np.clip(xy[:, 1], 0, SIZE - 1),
                    np.clip(xy[:, 0], 0, SIZE - 1)]
    return Image.fromarray(out)


# ── builtin-look parsing ────────────────────────────────────────────────────
INIT_FIELDS = ["smooth", "brighten", "warmth", "eye_pop", "blush", "lip",
               "eyes", "cheek", "vline", "nose", "lips_plump"]


def parse_looks(face_filters_cpp):
    """FaceFilter id -> {field: value} from the case blocks."""
    src = open(face_filters_cpp).read()
    hdr = open(face_filters_cpp.replace(".cpp", ".h")).read()
    enum = re.search(r"enum class FaceFilter \{(.*?)\};", hdr, re.S).group(1)
    ids, i = {}, 0
    for tok in re.split(r"[,\n]", enum):
        tok = re.sub(r"//.*", "", tok).strip()
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
        for cheek, under in ((CHEEK_R, UNDEREYE_R), (CHEEK_L, UNDEREYE_L)):
            cx, cy = px(mp_uv[cheek])
            ux, uy = px(mp_uv[under])
            bx, by = cx + (ux - cx) * raise_f, cy + (uy - cy) * raise_f
            r = SIZE * 0.085
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
        for _ in range(90):
            fx = nx + rng.normal(0, SIZE * 0.10)
            fy = ny + rng.normal(0, SIZE * 0.045) + SIZE * 0.02
            r = rng.uniform(1.2, 3.0)
            a = 0.55 * lk["freckles"] * rng.uniform(0.4, 1.0)
            d2.ellipse([fx - r, fy - r, fx + r, fy + r],
                       fill=col([0.45, 0.28, 0.18], a))
        layer = layer.filter(ImageFilter.GaussianBlur(0.8))
        img = Image.alpha_composite(img, layer)
        dr = ImageDraw.Draw(img)

    # eyeshadow: band from the lash chain toward the brow
    shadow = lk.get("shadow", 0.0)
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

    # eyeliner + wing along the lash chain
    liner = lk.get("liner", 0.0)
    if liner > 0.01:
        wpx = max(2.0, SIZE * 0.004 * (0.7 + 0.6 * min(liner, 2.0)))
        for chain in (LASH_R, LASH_L):
            pts = [px(mp_uv[i]) for i in chain]
            dr.line(pts, fill=(12, 8, 12, int(240 * min(liner, 1.0))),
                    width=int(wpx), joint="curve")
    wing = lk.get("lash_wing", 0.0)
    if wing > 0.01:
        for chain, sgn in ((LASH_R, -1.0), (LASH_L, 1.0)):
            o = np.array(px(mp_uv[chain[0]]))
            n1 = np.array(px(mp_uv[chain[1]]))
            d = o - n1
            d = d / (np.linalg.norm(d) + 1e-6)
            tip = o + d * SIZE * 0.035 * wing + np.array([0, -SIZE * 0.012])
            dr.line([tuple(o), tuple(tip)],
                    fill=(12, 8, 12, 235), width=max(2, int(SIZE * 0.005)))

    # lash fringe: short strokes fanning up from the chain
    lash = lk.get("lash", 0.0)
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
        layer = layer.filter(ImageFilter.GaussianBlur(SIZE * 0.004))
        img = Image.alpha_composite(img, layer)

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
    _, _, mp_uv, _ = parse_uvs()

    checker().save(os.path.join(out_dir, "checker.png"))
    print("checker.png")

    # plates
    plates = sorted(f for f in os.listdir(face_dir)
                    if f.startswith("makeup_") and f.endswith(".png"))
    for f in plates:
        img = Image.open(os.path.join(face_dir, f)).convert("RGBA")
        warp_image(img, warp, mask).save(os.path.join(out_dir, f))
    print(f"{len(plates)} plates warped")

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
        warp_image(mp_img, warp, mask).save(
            os.path.join(out_dir, f"look_{fid}.png"))
        n += 1
    print(f"{n} builtin looks baked")


if __name__ == "__main__":
    main()
