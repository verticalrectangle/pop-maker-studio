#!/usr/bin/env python3
"""Generate src/generated/arkit_mp_map.h — exact ARKit↔MediaPipe correspondence.

Replaces the runtime heuristic chain in arkit_face.cpp (hand-typed landmark
table + geometric guessing + IDW + screen-space snap + first-frame UV cache)
with one static table computed offline from the CANONICAL rest-pose meshes:

  k_mp_from_arkit[478]     {i0,i1,i2, w0,w1,w2} barycentric weights over
                           ARKit vertices for each MediaPipe landmark — the
                           runtime evaluates 478 exact screen positions from
                           the projected ARKit mesh. All rendering (beauty,
                           makeup mesh, warp) then runs on the hole-free
                           MediaPipe topology; the raw ARKit mesh is never
                           drawn (its eye cutouts reach ~5mm below the lower
                           lash line and would leave the plates' under-eye
                           concealer zone unpainted).

Inputs:
  tools/arkit_face_canonical.obj   ARKit neutral face geometry, 1220 verts +
                                   ARKit textureCoordinates (public
                                   ARFaceGeometry dump; Apple topology).
  tools/canonical_face_model.obj   MediaPipe canonical model (Apache 2.0).
  src/generated/face_uv_mesh.h     k_face_uv / k_face_tris (MediaPipe atlas).
  src/generated/arkit_face_mesh.h  k_arkit_tris (runtime render topology).

Alignment: Umeyama similarity fit (scale+R+t) on 10 anatomical anchor pairs.
Both canonical meshes face +z with +y up, so +x is the person's LEFT in both.
The fit is validated against the x-flipped hypothesis — the correct (unflipped)
correspondence must win by a wide margin, which pins the L/R convention that
the old hand-typed table got backwards.
"""
import os
import re
import sys
import numpy as np

here = os.path.dirname(os.path.abspath(__file__))
src_gen = os.path.join(here, "..", "src", "generated")

# Anatomical anchors: (MediaPipe idx, ARKit idx). Person's L/R, derived from
# canonical geometry signs (+x = person's left in BOTH meshes):
#   MP  33/133 person's RIGHT eye outer/inner  ↔  ARKit 1101/1090 (x<0)
#   MP 263/362 person's LEFT  eye outer/inner  ↔  ARKit 1069/1080 (x>0)
#   MP  61 person's RIGHT mouth corner         ↔  ARKit  249 (x<0)
#   MP 291 person's LEFT  mouth corner         ↔  ARKit  684 (x>0)
ANCHORS = [
    (1, 9),      # nose tip
    (13, 24),    # upper inner lip mid
    (14, 25),    # lower inner lip mid
    (10, 20),    # forehead top
    (33, 1101), (133, 1090),   # person's right eye outer/inner
    (263, 1069), (362, 1080),  # person's left eye outer/inner
    (61, 249), (291, 684),     # mouth corners R/L
]

# Iris centers (MediaPipe refined-landmark convention used engine-wide:
# 468 = person's RIGHT iris, 473 = person's LEFT iris). The MP canonical
# model has no iris verts; use eye-contour centroids.
IRIS_R_RING = [33, 133, 159, 145]    # person's right eye
IRIS_L_RING = [263, 362, 386, 374]   # person's left eye


def parse_obj(path):
    v = []
    for line in open(path):
        p = line.split()
        if p and p[0] == "v":
            v.append([float(x) for x in p[1:4]])
    return np.asarray(v, dtype=np.float64)


def parse_header_array(path, name, shape):
    text = open(path).read()
    m = re.search(re.escape(name) + r"\s*\[[^=]*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        raise RuntimeError(f"{name} not found in {path}")
    nums = re.findall(r"-?\d+\.?\d*(?:[eE][-+]?\d+)?", m.group(1))
    a = np.asarray([float(x) for x in nums], dtype=np.float64)
    return a.reshape(shape)


def umeyama(src, dst):
    """Similarity transform (s, R, t) minimizing ||dst - (s R src + t)||."""
    mu_s, mu_d = src.mean(0), dst.mean(0)
    sc, dc = src - mu_s, dst - mu_d
    cov = dc.T @ sc / len(src)
    U, D, Vt = np.linalg.svd(cov)
    S = np.eye(3)
    if np.linalg.det(U) * np.linalg.det(Vt) < 0:
        S[2, 2] = -1
    R = U @ S @ Vt
    var_s = (sc ** 2).sum() / len(src)
    s = np.trace(np.diag(D) @ S) / var_s
    t = mu_d - s * R @ mu_s
    return s, R, t


def closest_on_tri(p, a, b, c):
    """Closest point on triangle abc to p; returns (point, w0, w1, w2)."""
    ab, ac, ap = b - a, c - a, p - a
    d1, d2 = ab @ ap, ac @ ap
    if d1 <= 0 and d2 <= 0:
        return a, 1.0, 0.0, 0.0
    bp = p - b
    d3, d4 = ab @ bp, ac @ bp
    if d3 >= 0 and d4 <= d3:
        return b, 0.0, 1.0, 0.0
    vc = d1 * d4 - d3 * d2
    if vc <= 0 and d1 >= 0 and d3 <= 0:
        v = d1 / (d1 - d3)
        return a + v * ab, 1 - v, v, 0.0
    cp = p - c
    d5, d6 = ab @ cp, ac @ cp
    if d6 >= 0 and d5 <= d6:
        return c, 0.0, 0.0, 1.0
    vb = d5 * d2 - d1 * d6
    if vb <= 0 and d2 >= 0 and d6 <= 0:
        w = d2 / (d2 - d6)
        return a + w * ac, 1 - w, 0.0, w
    va = d3 * d6 - d5 * d4
    if va <= 0 and (d4 - d3) >= 0 and (d5 - d6) >= 0:
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6))
        return b + w * (c - b), 0.0, 1 - w, w
    denom = 1.0 / (va + vb + vc)
    v, w = vb * denom, vc * denom
    return a + ab * v + ac * w, 1 - v - w, v, w


def nearest_surface(p, verts, tris):
    """Nearest point over a triangle mesh → (tri_index, w0, w1, w2, dist)."""
    # Prune by vertex distance: only test triangles touching the K nearest verts.
    d2 = ((verts - p) ** 2).sum(1)
    near = set(np.argpartition(d2, 40)[:40])
    best = (None, 0, 0, 0, np.inf)
    for ti, (i0, i1, i2) in enumerate(tris):
        if i0 not in near and i1 not in near and i2 not in near:
            continue
        q, w0, w1, w2 = closest_on_tri(p, verts[i0], verts[i1], verts[i2])
        d = ((q - p) ** 2).sum()
        if d < best[4]:
            best = (ti, w0, w1, w2, d)
    return best


def boundary_rings(tris, nverts):
    """Closed boundary loops (each a vertex-index list) of a triangle mesh."""
    from collections import defaultdict
    count = defaultdict(int)
    for a, b, c in tris:
        for e in ((a, b), (b, c), (c, a)):
            count[tuple(sorted(e))] += 1
    adj = defaultdict(list)
    for (a, b), n in count.items():
        if n == 1:
            adj[a].append(b)
            adj[b].append(a)
    seen, rings = set(), []
    for start in list(adj):
        if start in seen:
            continue
        ring, cur, prev = [start], start, -1
        seen.add(start)
        while True:
            nxt = [n for n in adj[cur] if n != prev]
            if not nxt or nxt[0] == start:
                break
            prev, cur = cur, nxt[0]
            ring.append(cur)
            seen.add(cur)
        if len(ring) >= 4:
            rings.append(ring)
    return rings


def ring_near(rings, verts, target):
    """Ring whose centroid is nearest to target (excluding the outer ring,
    which is by far the longest)."""
    inner = sorted(rings, key=len)[:-1]
    return min(inner, key=lambda r: ((verts[r].mean(0) - target) ** 2).sum())


def ring_resample(ring_pts, ts):
    """Sample a closed polyline at normalized arc-length positions ts."""
    seg = np.sqrt(((np.roll(ring_pts, -1, 0) - ring_pts) ** 2).sum(1))
    cum = np.concatenate([[0.0], np.cumsum(seg)])
    total = cum[-1]
    out = np.empty((len(ts), 3))
    for k, t in enumerate(ts):
        d = (t % 1.0) * total
        i = np.searchsorted(cum, d, side="right") - 1
        i = min(i, len(ring_pts) - 1)
        f = (d - cum[i]) / max(seg[i], 1e-9)
        out[k] = ring_pts[i] * (1 - f) + ring_pts[(i + 1) % len(ring_pts)] * f
    return out


def orient_ring(ring, verts, start_target):
    """Rotate the ring to start nearest start_target and orient it CCW as
    seen from +z (both meshes face +z)."""
    pts = verts[ring]
    i0 = int(((pts - start_target) ** 2).sum(1).argmin())
    ring = ring[i0:] + ring[:i0]
    pts = verts[ring]
    area = 0.0
    for i in range(len(ring)):
        a, b = pts[i], pts[(i + 1) % len(ring)]
        area += a[0] * b[1] - b[0] * a[1]
    if area < 0:
        ring = [ring[0]] + ring[1:][::-1]
    return ring


def tps_warp(src_ctrl, dst_ctrl, pts, reg=1e-3):
    """3D thin-plate (biharmonic |r| kernel) warp fitted on control pairs."""
    n = len(src_ctrl)
    K = np.sqrt(((src_ctrl[:, None] - src_ctrl[None]) ** 2).sum(-1))
    K += np.eye(n) * reg
    P = np.hstack([np.ones((n, 1)), src_ctrl])
    A = np.zeros((n + 4, n + 4))
    A[:n, :n] = K
    A[:n, n:] = P
    A[n:, :n] = P.T
    rhs = np.zeros((n + 4, 3))
    rhs[:n] = dst_ctrl
    sol = np.linalg.solve(A, rhs)
    w, a = sol[:n], sol[n:]
    U = np.sqrt(((pts[:, None] - src_ctrl[None]) ** 2).sum(-1))
    return U @ w + np.hstack([np.ones((len(pts), 1)), pts]) @ a


def main():
    ak_v = parse_obj(os.path.join(here, "arkit_face_canonical.obj"))
    mp_v = parse_obj(os.path.join(here, "canonical_face_model.obj"))
    assert ak_v.shape == (1220, 3) and mp_v.shape == (468, 3)

    mp_uv = parse_header_array(os.path.join(src_gen, "face_uv_mesh.h"),
                               "k_face_uv", (468, 2))
    mp_tris = parse_header_array(os.path.join(src_gen, "face_uv_mesh.h"),
                                 "k_face_tris", (898, 3)).astype(int)
    ak_tris = parse_header_array(os.path.join(src_gen, "arkit_face_mesh.h"),
                                 "k_arkit_tris", (2304, 3)).astype(int)

    # Topology/order sanity: k_arkit_tris edges must be short on the obj verts.
    edge = ak_v[ak_tris[:, 0]] - ak_v[ak_tris[:, 1]]
    max_edge = np.sqrt((edge ** 2).sum(1)).max()
    print(f"arkit tri max edge: {max_edge:.1f}mm (mesh width "
          f"{np.ptp(ak_v[:, 0]):.0f}mm)")
    assert max_edge < 0.15 * np.ptp(ak_v[:, 0]), \
        "k_arkit_tris does not match arkit_face_canonical.obj vertex order"

    # Similarity fit MP→ARKit space, self-validated against the flipped
    # hypothesis (catches any L/R error in the anchor table).
    mi = [m for m, _ in ANCHORS]
    ai = [a for _, a in ANCHORS]
    s, R, t = umeyama(mp_v[mi], ak_v[ai])
    rms = np.sqrt((((s * (R @ mp_v[mi].T).T + t) - ak_v[ai]) ** 2)
                  .sum(1).mean())
    flip = mp_v.copy(); flip[:, 0] *= -1
    s2, R2, t2 = umeyama(flip[mi], ak_v[ai])
    rms_f = np.sqrt((((s2 * (R2 @ flip[mi].T).T + t2) - ak_v[ai]) ** 2)
                    .sum(1).mean())
    print(f"anchor RMS: {rms:.2f}mm  (x-flipped hypothesis: {rms_f:.2f}mm)")
    assert rms * 2 < rms_f, "L/R convention check failed — anchors suspect"
    assert rms < 6.0, f"anchor RMS too high ({rms:.2f}mm) — bad correspondence"

    mp_w = s * (R @ mp_v.T).T + t          # MediaPipe verts in ARKit space

    # ── Non-rigid refinement: pin the inner-lip ring ───────────────────────
    # The two canonical heads are different face shapes; a global similarity
    # leaves mm-scale residuals. The mouth hole edge IS the inner lip margin
    # in both meshes, so pinning it (arc-length correspondence from the
    # corner) is semantically exact.
    #
    # The EYE rings are deliberately NOT pinned: ARKit's eye cutouts are cut
    # well outside the palpebral opening (canonical hole 29x10mm vs the MP
    # contour 26x6.7mm, lower edge 5.4mm below the MP lower lash line — and
    # on-device the hole edge visibly sits below the real lash line).
    # Pinning MP's lid contour to the hole edge painted the under-eye makeup
    # band on the upper cheek, skipping the eyebag zone entirely.
    ak_rings = boundary_rings(ak_tris, 1220)
    print(f"arkit boundary rings: {sorted(len(r) for r in ak_rings)}")
    MP_LIPS_IN = [78, 191, 80, 81, 82, 13, 312, 311, 310, 415, 308,
                  324, 318, 402, 317, 14, 87, 178, 88, 95]
    # TPS controls exclude the eye-corner anchors: their ARKit targets are
    # hole-boundary verts ~3.6mm below/outside the true canthi (the cutout is
    # oversized), and pinning them drags the whole eye complex down — the
    # same under-eye displacement the ring pinning caused, just milder. Eye
    # placement comes from MediaPipe's proportions under the global fit.
    EYE_CORNER_MP = {33, 133, 263, 362}
    tps_pairs = [(m, a) for m, a in ANCHORS if m not in EYE_CORNER_MP]
    tmi = [m for m, _ in tps_pairs]
    tai = [a for _, a in tps_pairs]
    src_ctrl = [mp_w[tmi]]
    dst_ctrl = [ak_v[tai]]
    aring = ring_near(ak_rings, ak_v, ak_v[[249, 684]].mean(0))
    mring = orient_ring(list(MP_LIPS_IN), mp_w, mp_w[78])
    aring = orient_ring(aring, ak_v, ak_v[249])
    # Split both rings at the two mouth corners and match each arc
    # separately — whole-ring arc-length matching lets upper/lower
    # vertex-density differences rotate the correspondence asymmetrically.
    mci = mring.index(308)                       # person's left inner corner
    aci = int(np.argmin(((ak_v[aring] - ak_v[684]) ** 2).sum(1)))
    m_arcs = [mring[:mci + 1], mring[mci:] + [mring[0]]]
    a_arcs = [aring[:aci + 1], aring[aci:] + [aring[0]]]
    for m_arc, a_arc in zip(m_arcs, a_arcs):
        mpts = mp_w[m_arc]
        seg = np.sqrt(((mpts[1:] - mpts[:-1]) ** 2).sum(1))
        cum = np.concatenate([[0.0], np.cumsum(seg)])
        ts = cum / max(cum[-1], 1e-9)
        apts = ak_v[a_arc]
        aseg = np.sqrt(((apts[1:] - apts[:-1]) ** 2).sum(1))
        acum = np.concatenate([[0.0], np.cumsum(aseg)])
        atot = max(acum[-1], 1e-9)
        dst = np.empty((len(ts), 3))
        for k, t in enumerate(ts):
            d = t * atot
            j = min(int(np.searchsorted(acum, d, side="right")) - 1,
                    len(apts) - 2)
            f = (d - acum[j]) / max(aseg[j], 1e-9)
            dst[k] = apts[j] * (1 - f) + apts[j + 1] * f
        src_ctrl.append(mpts)
        dst_ctrl.append(dst)
    src_ctrl = np.vstack(src_ctrl)
    dst_ctrl = np.vstack(dst_ctrl)
    mp_w = tps_warp(src_ctrl, dst_ctrl, mp_w)
    rms2 = np.sqrt(((mp_w[tmi] - ak_v[tai]) ** 2).sum(1).mean())
    eye_res = np.sqrt(((mp_w[[33, 133, 263, 362]]
                        - ak_v[[1101, 1090, 1069, 1080]]) ** 2).sum(1))
    print(f"post-TPS anchor RMS: {rms2:.2f}mm; eye-corner offsets vs hole "
          f"corners: {np.round(eye_res, 1)}mm (expected ~3-4, NOT pinned)")
    assert rms2 < rms

    # ── Eye-hole handling ───────────────────────────────────────────────────
    # The true lash lines and the first under-eye row fall INSIDE ARKit's
    # oversized holes, where the mesh has no geometry. Clamped
    # nearest-surface would snap them to whichever hole edge is closer
    # (upper-lid triangles for the lower lash — which then dives on every
    # blink). Instead, landmarks inside a hole get UNCLAMPED plane
    # barycentrics extrapolated from the anatomically-correct side of the
    # hole: the lower arc's triangle fan for the lower-lid complex, the
    # upper arc's for the upper. The runtime evaluation is linear, so
    # negative weights are fine, and the point tracks the tissue it belongs
    # to (under-eye makeup no longer jumps with upper-lid blinks).
    ring_all = set()
    eye_holes = []   # (ordered ring, 2D poly, centroid, ymid)
    for ak_t in ((1101, 1090), (1069, 1080)):
        ring = ring_near(ak_rings, ak_v, ak_v[list(ak_t)].mean(0))
        ring_all |= set(ring)
        eye_holes.append((ring, ak_v[ring][:, :2],
                          ak_v[ring].mean(0), ak_v[ring][:, 1].mean()))

    def in_hole(p):
        for hole in eye_holes:
            ring, poly, cen, ymid = hole
            inside, n = False, len(poly)
            for i2 in range(n):
                a, b = poly[i2], poly[(i2 + 1) % n]
                if (a[1] > p[1]) != (b[1] > p[1]):
                    xin = a[0] + (p[1] - a[1]) * (b[0] - a[0]) / (b[1] - a[1])
                    if p[0] < xin:
                        inside = not inside
            if inside:
                return hole
        return None

    def hole_pseudo_tri(p, hole):
        """Two adjacent ring verts on the landmark's side of the hole plus a
        vert one row outward — a wide, stable triangle whose plane tracks the
        local skin, so the unclamped solve stays symmetric and moderate."""
        ring, poly, cen, ymid = hole
        low = p[1] <= ymid
        arc = [v for v in ring if (ak_v[v][1] <= ymid) == low]
        v0 = min(arc, key=lambda v: ((ak_v[v] - p) ** 2).sum())
        v1 = min((v for v in arc if v != v0),
                 key=lambda v: ((ak_v[v] - ak_v[v0]) ** 2).sum())
        mid = (ak_v[v0] + ak_v[v1]) * 0.5
        outd = mid - cen
        outd /= max(np.linalg.norm(outd), 1e-9)
        tgt = mid + outd * 5.0
        v2 = min((v for v in range(len(ak_v)) if v not in ring_all),
                 key=lambda v: ((ak_v[v] - tgt) ** 2).sum())
        return v0, v1, v2

    def bary2d(p, a, b, c):
        """Weights (sum=1) reproducing p's xy exactly from vert xy — the
        runtime consumes projected 2D positions, so xy is what must match;
        z is deliberately unconstrained."""
        m = np.array([[a[0] - c[0], b[0] - c[0]],
                      [a[1] - c[1], b[1] - c[1]]])
        w01 = np.linalg.solve(m, p[:2] - c[:2])
        return w01[0], w01[1], 1.0 - w01[0] - w01[1]

    def plane_bary(p, a, b, c):
        """Unclamped barycentric of p projected onto plane(a,b,c)."""
        ab, ac = b - a, c - a
        n = np.cross(ab, ac)
        q = p - n * ((p - a) @ n) / max(n @ n, 1e-12)
        d00, d01, d11 = ab @ ab, ab @ ac, ac @ ac
        d20, d21 = (q - a) @ ab, (q - a) @ ac
        den = d00 * d11 - d01 * d01
        v = (d11 * d20 - d01 * d21) / den
        w = (d00 * d21 - d01 * d20) / den
        return 1.0 - v - w, v, w
    # Barycentric weights over ARKit verts per MediaPipe landmark. (The
    # render path draws the hole-free MediaPipe topology positioned by these
    # weights; the raw ARKit mesh is never drawn — its oversized eye cutouts
    # would leave the under-eye concealer zone unpainted.)
    targets = np.zeros((478, 3))
    targets[:468] = mp_w
    targets[468] = targets[469] = targets[470] = targets[471] = targets[472] \
        = mp_w[IRIS_R_RING].mean(0)
    targets[473] = targets[474] = targets[475] = targets[476] = targets[477] \
        = mp_w[IRIS_L_RING].mean(0)
    # Iris centers get a stable pseudo-triangle — the two hole corners plus
    # the upper arc's top vertex (the bary table may reference ANY vertex
    # triple, not just mesh triangles). Solved exactly to the eye-center
    # target; only ~1/5 of the weight rides the blinking upper lid.
    def iris_tri(hole_idx):
        ring, poly, cen, ymid = eye_holes[hole_idx]
        upper = [v for v in ring if ak_v[v][1] > ymid]
        return max(upper, key=lambda v: ak_v[v][1])
    IRIS_TRI = {}
    for ids, (c0, c1), hi in (((468, 469, 470, 471, 472), (1101, 1090), 0),
                              ((473, 474, 475, 476, 477), (1069, 1080), 1)):
        top = iris_tri(hi)
        for i in ids:
            IRIS_TRI[i] = (c0, c1, top)
    rows = []
    n_extrap = 0
    for i, p in enumerate(targets):
        if i in IRIS_TRI:
            c0, c1, top = IRIS_TRI[i]
            w0, w1, w2 = bary2d(p, ak_v[c0], ak_v[c1], ak_v[top])
            rows.append((c0, c1, top, w0, w1, w2))
            continue
        hole = in_hole(p)
        if hole is not None:
            i0, i1, i2 = hole_pseudo_tri(p, hole)
            w0, w1, w2 = bary2d(p, ak_v[i0], ak_v[i1], ak_v[i2])
            assert max(abs(w0), abs(w1), abs(w2)) < 8.0, \
                f"mp {i}: unstable extrapolation " \
                f"w=({w0:.2f},{w1:.2f},{w2:.2f})"
            rows.append((i0, i1, i2, w0, w1, w2))
            n_extrap += 1
            continue
        ti, w0, w1, w2, d = nearest_surface(p, ak_v, ak_tris)
        i0, i1, i2 = ak_tris[ti]
        rows.append((i0, i1, i2, w0, w1, w2))
    print(f"in-hole extrapolated landmarks: {n_extrap}")
    for i in (1, 33, 263, 61, 291, 152, 145, 374, 468, 473):
        i0, i1, i2, w0, w1, w2 = rows[i]
        q = w0 * ak_v[i0] + w1 * ak_v[i1] + w2 * ak_v[i2]
        print(f"  mp {i:3d} → ({i0},{i1},{i2}) w=({w0:+.2f},{w1:+.2f},{w2:+.2f})"
              f" pos=({q[0]:+6.1f},{q[1]:+6.1f}) target_y={targets[i][1]:+6.1f}")

    # L/R spot check: person's right (MP 33) must land at x<0, left (263) x>0.
    def bary_x(r):
        i0, i1, i2, w0, w1, w2 = r
        return (w0 * ak_v[i0] + w1 * ak_v[i1] + w2 * ak_v[i2])[0]
    assert bary_x(rows[33]) < 0 < bary_x(rows[263])
    assert bary_x(rows[468]) < 0 < bary_x(rows[473])

    out = os.path.join(src_gen, "arkit_mp_map.h")
    with open(out, "w") as f:
        f.write("// GENERATED by tools/gen_arkit_mp_map.py — do not edit.\n")
        f.write("// Exact ARKit(1220)→MediaPipe(478) landmark bridge from the\n")
        f.write("// canonical rest-pose meshes (anchor RMS %.2fmm).\n" % rms)
        f.write("#pragma once\n\n")
        f.write("// Barycentric weights over ARKit vertices per MediaPipe\n")
        f.write("// landmark (0..467 mesh, 468/473 iris centers; ring points\n")
        f.write("// duplicate their center).\n")
        f.write("struct ArkitMpBary { unsigned short i0, i1, i2; "
                "float w0, w1, w2; };\n")
        f.write("static const ArkitMpBary k_mp_from_arkit[478] = {\n")
        for i0, i1, i2, w0, w1, w2 in rows:
            f.write(f"    {{{i0}, {i1}, {i2}, "
                    f"{w0:.6f}f, {w1:.6f}f, {w2:.6f}f}},\n")
        f.write("};\n")
    print(f"wrote {out}")


if __name__ == "__main__":
    sys.exit(main())
