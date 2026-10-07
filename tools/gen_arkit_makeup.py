#!/usr/bin/env python3
"""ARKit makeup mask baker — a look's geometry, painted on ARKit's canonical head.

Every mask is computed ON the canonical ARKit face (tools/arkit_face_canonical.obj,
millimetres) and rasterized through the mesh's own triangles into ARKit UV space,
so placement rides the topology the device mesh shares:

  * the eye-hole rims are the lash lines (liner + lashes are drawn live from
    them by the engine; masks here only place the shadow around them);
  * the concentric 36-vertex loops around the mouth are the lip anatomy —
    loop 3 traces the vermilion border (its upper half carries the cupid's
    bow and is the lip's most protruding ridge), so the lip SDF is defined on
    the loop field and follows every wearer's fitted lips.

The engine (src/arkit_makeup.mm) samples two RGBA atlases per look:

  <id>_a.png  1024²  r skin (smoothing region)  g blush  b eyeshadow  a brow region
  <id>_b.png  2048²  r lip SDF  g freckles  b gloss/highlighter  a inner-corner light

Lip SDF = 0.5 + d/16, d = signed distance (mm) to the vermilion border,
negative inside the lips; the engine shifts the border by the look's
`overline_mm`. Colors and amounts live in the look JSON next to the masks
(models/face/arkit/<id>.json); this tool owns geometry only. Masks are padded
past their UV islands so GPU mipmaps never pull in the empty atlas background.

Usage: tools/gen_arkit_makeup.py [--assets <pms-ios Engine/EngineAssets>] [--only id ...]
"""
import argparse
import os
import re
from collections import defaultdict, deque

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC_GEN = os.path.join(HERE, "..", "src", "generated")

# ── ARKit topology (canonical head faces +z, +y up, +x = the person's LEFT) ──
UPPER = {"R": [1101, 1100, 1099, 1098, 1097, 1096, 1095, 1094, 1093, 1092, 1091, 1090],
         "L": [1069, 1070, 1071, 1072, 1073, 1074, 1075, 1076, 1077, 1078, 1079, 1080]}
LOWER = {"R": [1101, 1102, 1103, 1104, 1105, 1106, 1107, 1108, 1085, 1086, 1087, 1088, 1089, 1090],
         "L": [1069, 1068, 1067, 1066, 1065, 1064, 1063, 1062, 1061, 1084, 1083, 1082, 1081, 1080]}
MOUTH_HALF_W = 21.5          # canonical mouth corner-to-center, mm


# ── mesh I/O ──────────────────────────────────────────────────────────────────
def parse_obj(path):
    verts = []
    with open(path) as f:
        for line in f:
            if line.startswith("v "):
                verts.append([float(x) for x in line.split()[1:4]])
    return np.array(verts, np.float64)


def parse_header_array(path, name, shape):
    text = open(path).read()
    start = text.index("{", text.index(name))
    nums = re.findall(r"-?\d+\.?\d*(?:e-?\d+)?", text[start:text.index("};", start)])
    return np.array([float(x) for x in nums], np.float64).reshape(shape)


class Head:
    """Canonical ARKit head: geometry, rings, the mouth loop field."""

    def __init__(self):
        self.V = parse_obj(os.path.join(HERE, "arkit_face_canonical.obj"))
        header = os.path.join(SRC_GEN, "arkit_face_mesh.h")
        self.UV = parse_header_array(header, "k_arkit_uv", (1220, 2))
        self.T = parse_header_array(header, "k_arkit_tris", (2304, 3)).astype(np.int64)
        assert self.V.shape == (1220, 3), self.V.shape

        edges = defaultdict(int)
        nbr = defaultdict(set)
        for a, b, c in self.T:
            for u, v in ((a, b), (b, c), (c, a)):
                edges[(min(u, v), max(u, v))] += 1
                nbr[u].add(v)
                nbr[v].add(u)
        adj = defaultdict(list)
        for (u, v), n in edges.items():
            if n == 1:
                adj[u].append(v)
                adj[v].append(u)
        rings, seen = [], set()
        for s in adj:
            if s in seen:
                continue
            ring, prev, cur = [s], None, s
            seen.add(s)
            while True:
                nxt = [n for n in adj[cur] if n != prev and n not in seen]
                if not nxt:
                    break
                prev, cur = cur, nxt[0]
                ring.append(cur)
                seen.add(cur)
            rings.append(ring)
        by_len = sorted(rings, key=len)
        self.mouth_ring = next(r for r in rings if len(r) == 36)
        self.outer_ring = by_len[-1]
        self.eye_rings = [r for r in rings if len(r) == 24]
        assert len(self.eye_rings) == 2

        # Mouth loop field: BFS level from the mouth hole (0) outward.
        level = np.full(1220, 12.0)
        q = deque()
        for v in self.mouth_ring:
            level[v] = 0.0
            q.append(v)
        while q:
            v = q.popleft()
            if level[v] >= 11.0:
                continue
            for n in nbr[v]:
                if level[n] > level[v] + 1.0:
                    level[n] = level[v] + 1.0
                    q.append(n)
        self.loop = level
        self.mouth_c = self.V[self.mouth_ring].mean(0)
        self.nose_tip = self.V[int(np.argmax(self.V[:, 2]))]

        n = np.zeros_like(self.V)
        fn = np.cross(self.V[self.T[:, 1]] - self.V[self.T[:, 0]],
                      self.V[self.T[:, 2]] - self.V[self.T[:, 0]])
        for k in range(3):
            np.add.at(n, self.T[:, k], fn)
        self.N = n / (np.linalg.norm(n, axis=1, keepdims=True) + 1e-12)

    def surface_point(self, x, y):
        """Front-surface point of the canonical head nearest (x, y) in the
        frontal plane (vertex-accurate; masks here are soft)."""
        d = (self.V[:, 0] - x) ** 2 + (self.V[:, 1] - y) ** 2
        front = self.N[:, 2] > 0.2
        d = np.where(front, d, np.inf)
        return self.V[int(np.argmin(d))]


class Raster:
    """Covered texels of a size² ARKit-UV atlas: their triangle, barycentrics
    and 3D position (mm). Texel (i, j) samples at UV ((j+.5)/size, (i+.5)/size)
    — the convention Metal's normalized coordinates use."""

    def __init__(self, head, size):
        self.size = size
        tri = np.full((size, size), -1, np.int32)
        bary = np.zeros((size, size, 3), np.float32)
        puv = head.UV * size - 0.5          # texel-center space
        for t, (a, b, c) in enumerate(head.T):
            p0, p1, p2 = puv[a], puv[b], puv[c]
            x0 = max(int(np.floor(min(p0[0], p1[0], p2[0]))), 0)
            x1 = min(int(np.ceil(max(p0[0], p1[0], p2[0]))), size - 1)
            y0 = max(int(np.floor(min(p0[1], p1[1], p2[1]))), 0)
            y1 = min(int(np.ceil(max(p0[1], p1[1], p2[1]))), size - 1)
            if x1 < x0 or y1 < y0:
                continue
            xs, ys = np.meshgrid(np.arange(x0, x1 + 1), np.arange(y0, y1 + 1))
            det = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1])
            if abs(det) < 1e-12:
                continue
            w1 = ((xs - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (ys - p0[1])) / det
            w2 = ((p1[0] - p0[0]) * (ys - p0[1]) - (xs - p0[0]) * (p1[1] - p0[1])) / det
            w0 = 1.0 - w1 - w2
            inside = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
            if not inside.any():
                continue
            iy, ix = ys[inside], xs[inside]
            tri[iy, ix] = t
            bary[iy, ix] = np.stack([w0[inside], w1[inside], w2[inside]], 1)
        self.iy, self.ix = np.nonzero(tri >= 0)
        self.tri = tri[self.iy, self.ix]
        self.bary = bary[self.iy, self.ix].astype(np.float64)
        self.head = head
        self.P = self.interp(head.V)
        self.Nrm = self.interp(head.N)
        self.Nrm /= np.linalg.norm(self.Nrm, axis=1, keepdims=True) + 1e-12

    def interp(self, attr):
        idx = self.head.T[self.tri]
        if attr.ndim == 1:
            return (attr[idx] * self.bary).sum(1)
        return (attr[idx] * self.bary[..., None]).sum(1)

    def smooth_uv(self, values, sigma):
        """Coverage-normalized Gaussian blur of per-texel values in UV space
        (σ in texels) — rounds the polygonal facets an iso-line on the
        piecewise-linear loop field picks up from the mesh."""
        size = self.size
        num = np.zeros((size, size), np.float32)
        den = np.zeros((size, size), np.float32)
        num[self.iy, self.ix] = values
        den[self.iy, self.ix] = 1.0
        rad = int(np.ceil(3 * sigma))
        k = np.exp(-0.5 * (np.arange(-rad, rad + 1) / sigma) ** 2).astype(np.float32)
        for axis in (0, 1):
            n2, d2 = np.zeros_like(num), np.zeros_like(den)
            for i, w in enumerate(k):
                n2 += w * np.roll(num, i - rad, axis)
                d2 += w * np.roll(den, i - rad, axis)
            num, den = n2, d2
        out = num[self.iy, self.ix] / np.maximum(den[self.iy, self.ix], 1e-6)
        return out.astype(np.float64)

    def image(self, channels):
        """Covered-texel values (list of 4 arrays) → padded uint8 RGBA."""
        size = self.size
        img = np.zeros((size, size, 4), np.float32)
        filled = np.zeros((size, size), bool)
        for k, ch in enumerate(channels):
            img[self.iy, self.ix, k] = ch
        filled[self.iy, self.ix] = True
        img = pad(img, filled, iters=max(8, size // 64))
        return Image.fromarray(np.clip(np.rint(img * 255.0), 0, 255).astype(np.uint8), "RGBA")


def pad(img, filled, iters):
    """Grow covered texels outward (mean of covered 8-neighbors) so mipmaps
    and bilinear taps at island edges see real values, not the background."""
    img = img.copy()
    filled = filled.copy()
    for _ in range(iters):
        acc = np.zeros_like(img)
        cnt = np.zeros(filled.shape, np.float32)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dx == 0 and dy == 0:
                    continue
                f = np.roll(np.roll(filled, dy, 0), dx, 1)
                acc += np.roll(np.roll(img, dy, 0), dx, 1) * f[..., None]
                cnt += f
        grow = (~filled) & (cnt > 0)
        if not grow.any():
            break
        img[grow] = acc[grow] / cnt[grow][:, None]
        filled |= grow
    return img


# ── geometry helpers ──────────────────────────────────────────────────────────
def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def gauss(P, center, sigma):
    d2 = ((P - np.asarray(center)) ** 2).sum(1)
    return np.exp(-d2 / (2.0 * sigma * sigma))


def polyline_dist(P, poly):
    """Distance (mm) to a 3D polyline, arc-length t ∈ [0,1] of the closest
    point, and the signed side (+ = above the line in the frontal plane)."""
    seg_len = np.linalg.norm(np.diff(poly, axis=0), axis=1)
    cum = np.concatenate([[0.0], np.cumsum(seg_len)]) / seg_len.sum()
    best = np.full(len(P), np.inf)
    best_t = np.zeros(len(P))
    side = np.zeros(len(P))
    for k in range(len(poly) - 1):
        a, b = poly[k], poly[k + 1]
        e = b - a
        t = np.clip(((P - a) @ e) / (e @ e), 0.0, 1.0)
        q = a + t[:, None] * e
        d = np.linalg.norm(P - q, axis=1)
        m = d < best
        best[m] = d[m]
        best_t[m] = cum[k] + t[m] * (cum[k + 1] - cum[k])
        up = np.cross([0.0, 0.0, 1.0], e / np.linalg.norm(e))
        if up[1] < 0:
            up = -up
        side[m] = ((P[m] - q[m]) @ up)
    return best, best_t, side


def soft_union(*masks):
    out = np.zeros_like(masks[0])
    for m in masks:
        out = 1.0 - (1.0 - out) * (1.0 - np.clip(m, 0.0, 1.0))
    return out


# ── look painters (geometry only; colors live in the look JSON) ───────────────
class Masks:
    def __init__(self, head):
        self.head = head
        self.ra = Raster(head, 1024)
        self.rb = Raster(head, 2048)

    def lip_sdf(self, r, upper_level=3.0, lower_level=3.3, corner_level=1.4):
        """Signed distance (mm) to the vermilion border on the mouth-loop
        field. Upper border = loop 3 (cupid's bow is in the topology), lower
        border slightly past loop 3 where the lower lip turns under; both
        converge on the commissures."""
        h = self.head
        s = r.interp(h.loop)
        P = r.P
        # per-triangle |∇s| (levels per mm) → distance scale
        tv = h.V[h.T]
        tl = h.loop[h.T]
        e1, e2 = tv[:, 1] - tv[:, 0], tv[:, 2] - tv[:, 0]
        n = np.cross(e1, e2)
        area2 = np.linalg.norm(n, axis=1) + 1e-12
        nh = n / area2[:, None]
        g = (np.cross(nh, e2) * (tl[:, 1] - tl[:, 0])[:, None]
             + np.cross(e1, nh) * (tl[:, 2] - tl[:, 0])[:, None]) / area2[:, None]
        grad = np.maximum(np.linalg.norm(g, axis=1), 0.12)[r.tri]
        xn = np.abs(P[:, 0] - h.mouth_c[0]) / MOUTH_HALF_W
        upper = P[:, 1] > h.mouth_c[1]
        border = np.where(upper,
                          upper_level - (upper_level - corner_level) * smooth(0.55, 1.0, xn),
                          lower_level - (lower_level - corner_level) * smooth(0.50, 1.0, xn))
        d = (s - border) / grad
        far = s >= 11.0
        d[far] = 8.0
        return np.clip(d, -8.0, 8.0)

    def egirl(self):
        """The E-Girl look: big saturated apples + nose flush, faint freckles
        across the nose and upper cheeks, soft warm-brown crease and outer
        smudge, nose-tip highlight, glossy full lips, brown brows."""
        h = self.head
        out = {}

        # ── atlas A (1024²) ──
        r = self.ra
        P = r.P
        brow = np.zeros(len(P))
        shadow = np.zeros(len(P))
        eye_excl = np.ones(len(P))
        for side, sx in (("R", -1.0), ("L", 1.0)):
            up = h.V[UPPER[side]]
            lo = h.V[LOWER[side]]
            d_up, t_up, s_up = polyline_dist(P, up)
            d_lo, t_lo, s_lo = polyline_dist(P, lo)
            above = smooth(-0.3, 0.6, s_up)
            below = smooth(-0.3, 0.6, -s_lo)
            near_eye = np.minimum(d_up, d_lo)
            eye_excl *= smooth(1.2, 3.5, near_eye)
            outer = 1.0 - smooth(0.25, 0.75, t_up)
            crease = (smooth(2.6, 4.6, d_up) * (1.0 - smooth(7.0, 10.0, d_up))
                      * smooth(0.3, 1.2, s_up) * (1.0 - smooth(0.72, 0.95, t_up)))
            v_fill = (1.0 - smooth(5.0, 9.0, d_up)) * (1.0 - smooth(0.05, 0.35, t_up)) * above
            smudge = ((1.0 - smooth(1.4, 4.0, d_lo)) * (1.0 - smooth(0.30, 0.62, t_lo))
                      * below * 0.75)
            shadow = soft_union(shadow, crease * (0.35 + 0.65 * outer), v_fill * 0.8, smudge)
            # brow line: ~14 mm above the lash line, arched toward the outer
            # third, tail running past the outer corner (frontal-plane distance)
            ts = np.linspace(-0.15, 1.08, 40)
            seg = np.linalg.norm(np.diff(up, axis=0), axis=1)
            cum = np.concatenate([[0.0], np.cumsum(seg)]) / seg.sum()
            bx = np.interp(np.clip(ts, 0, 1), cum, up[:, 0])
            by = np.interp(np.clip(ts, 0, 1), cum, up[:, 1])
            ext_out = np.minimum(ts, 0.0)                       # past the outer corner
            ext_in = np.maximum(ts - 1.0, 0.0)                  # past the inner corner
            axis = (up[0] - up[-1])[:2] / np.linalg.norm((up[0] - up[-1])[:2])
            bx = bx + axis[0] * (-ext_out) * 40.0 - axis[0] * ext_in * 40.0
            by = by + axis[1] * (-ext_out) * 40.0 - axis[1] * ext_in * 40.0
            arch = 13.5 + 2.2 * np.exp(-((ts - 0.35) / 0.3) ** 2) - 5.0 * np.maximum(-ts, 0.0)
            line = np.stack([bx, by + arch], 1)
            d2 = np.full(len(P), np.inf)
            for k in range(len(line) - 1):
                a, b = line[k], line[k + 1]
                e = b - a
                t = np.clip(((P[:, :2] - a) @ e) / (e @ e), 0.0, 1.0)
                d2 = np.minimum(d2, np.linalg.norm(P[:, :2] - (a + t[:, None] * e), axis=1))
            brow = np.maximum(brow, (1.0 - smooth(3.6, 6.8, d2)) * (r.Nrm[:, 2] > 0.1))

        # blush: round apples under the outer half of the eye at nose-tip
        # height, plus the flush band across the nose (lighter on the bridge)
        apples = np.zeros(len(P))
        for sx in (-1.0, 1.0):
            c = h.surface_point(33.0 * sx, -3.0)
            d2 = (((P[:, 0] - c[0]) / 10.5) ** 2 + ((P[:, 1] - c[1]) / 9.5) ** 2
                  + ((P[:, 2] - c[2]) / 10.5) ** 2)
            g = np.exp(-0.5 * d2)
            apples = soft_union(apples, 1.0 - (1.0 - g) ** 1.6)
        band = (np.exp(-0.5 * ((P[:, 1] - 3.0) / 5.5) ** 2)
                * (0.5 + 0.5 * smooth(5.0, 18.0, np.abs(P[:, 0])))
                * (1.0 - smooth(26.0, 36.0, np.abs(P[:, 0])))
                * (r.Nrm[:, 2] > 0.0))
        blush = soft_union(apples, 0.8 * band)

        lip_a = self.lip_sdf(r)
        boundary = h.V[h.outer_ring + h.outer_ring[:1]]
        d_edge, _, _ = polyline_dist(P, boundary)
        skin = (smooth(3.0, 12.0, d_edge) * eye_excl * smooth(0.5, 2.0, lip_a)
                * (1.0 - 0.9 * brow))
        out["a"] = r.image([skin, blush, shadow, brow])

        # ── atlas B (2048²) ──
        r = self.rb
        P = r.P
        lip = r.smooth_uv(self.lip_sdf(r), sigma=4.0)
        rng = np.random.default_rng(20261007)

        # freckles: sparse, small, warm — nose bridge and the upper cheeks
        zone = np.maximum(
            ((np.abs(P[:, 0]) < 13.0) & (P[:, 1] > -3.0) & (P[:, 1] < 16.0)).astype(float),
            np.maximum(1.0 - smooth(8.0, 16.0, np.hypot(P[:, 0] - 27.0, P[:, 1] - 6.0)),
                       1.0 - smooth(8.0, 16.0, np.hypot(P[:, 0] + 27.0, P[:, 1] - 6.0))))
        zone *= (r.Nrm[:, 2] > 0.15)
        freck = np.zeros(len(P))
        centers = []
        cand = rng.permutation(np.nonzero(zone > 0.05)[0])
        for i in cand:
            if len(centers) >= 72:
                break
            if rng.random() > zone[i]:
                continue
            p = P[i]
            if centers and np.min(np.linalg.norm(np.asarray(centers) - p, axis=1)) < 2.3:
                continue
            centers.append(p)
        texel_of = np.full((r.size, r.size), -1, np.int64)
        texel_of[r.iy, r.ix] = np.arange(len(P))
        for p in centers:
            rad = 0.33 + 0.45 * rng.random() ** 2
            op = 0.4 + 0.5 * rng.random()
            i0 = int(np.argmin(np.linalg.norm(P - p, axis=1)))
            y, x = r.iy[i0], r.ix[i0]
            win = texel_of[max(y - 14, 0):y + 15, max(x - 14, 0):x + 15].ravel()
            win = win[win >= 0]
            d = np.linalg.norm(P[win] - p, axis=1)
            freck[win] = np.maximum(freck[win], op * (1.0 - smooth(rad - 0.15, rad + 0.25, d)))

        # gloss on the lip centers (lower lip fuller), highlighter on the nose
        # tip (the look's signature shine), bridge, cheekbone tops, cupid's bow
        xn = np.abs(P[:, 0] - h.mouth_c[0]) / MOUTH_HALF_W
        upper_w = smooth(-1.5, 1.5, P[:, 1] - h.mouth_c[1])
        gloss = (smooth(-0.5, -1.7, lip) * (1.0 - 0.5 * smooth(0.3, 0.9, xn))
                 * (1.0 - 0.3 * upper_w))
        tip = h.nose_tip + np.array([0.0, 1.5, -0.6])
        hl = soft_union(gauss(P, tip, 2.8),
                        0.45 * gauss(P, h.surface_point(0.0, 14.0), 2.4),
                        0.30 * gauss(P, h.surface_point(-40.0, 13.0), 5.0),
                        0.30 * gauss(P, h.surface_point(40.0, 13.0), 5.0),
                        0.40 * gauss(P, h.surface_point(0.0, h.mouth_c[1] + 8.8), 1.6))
        gloss_hl = np.maximum(gloss, hl * smooth(0.4, 1.4, lip))

        inner = np.zeros(len(P))
        for side, sx in (("R", -1.0), ("L", 1.0)):
            corner = h.V[UPPER[side][-1]] + np.array([-sx * 1.6, -0.6, 0.0])
            inner = soft_union(inner, gauss(P, corner, 2.2))

        out["b"] = r.image([np.clip(0.5 + lip / 16.0, 0.0, 1.0), freck, gloss_hl, inner])
        return out


LOOKS = {"egirl": Masks.egirl}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--assets", default=os.path.expanduser("~/dev/pms-ios/Engine/EngineAssets"))
    ap.add_argument("--only", nargs="*", default=None)
    args = ap.parse_args()
    out_dir = os.path.join(args.assets, "models", "face", "arkit")
    os.makedirs(out_dir, exist_ok=True)
    head = Head()
    masks = Masks(head)
    for look_id in args.only or list(LOOKS):
        imgs = LOOKS[look_id](masks)
        for key, im in imgs.items():
            path = os.path.join(out_dir, f"{look_id}_{key}.png")
            im.save(path, optimize=True)
            print(f"  {os.path.basename(path)} {im.size[0]}² ({os.path.getsize(path) // 1024} KB)")
    print("done")


if __name__ == "__main__":
    main()
