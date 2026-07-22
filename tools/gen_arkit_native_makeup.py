#!/usr/bin/env python3
"""ARKit-native makeup atlas painter — the looks painted DIRECTLY in ARKit
UV space. No MP-UV plate, no warp map: the warp's bilinear resample over
already-blurred MP art is what made the collection read as smudged decals
on a real face. Native painting keeps liner razor-anchored to the eye-hole
rim quads and gradients crisp at full 1024 resolution.

Geometry sources (all topology constants of ARKit's 1220-vert face):
  - eye hole rims: boundary rings; upper-arc vertex ids are constants
    (ARC_UP_R/L below, see arkit_map_smoke.cpp); lower arc = rest of ring.
  - mouth hole rim: boundary ring near verts 249/684.
  - cheek/nose/chin/jaw anchors: MP landmarks carried into ARKit 3D by the
    TPS correspondence (build_correspondence), snapped to nearest vertex.

Output: models/face/arkit/makeup_<id>.png in the pms-ios assets (same
filenames as the warped plates — the tier-1 renderer loads these).

Usage: tools/gen_arkit_native_makeup.py [--only id ...] [--assets <dir>]
"""
import argparse
import hashlib
import math
import os
import random
import sys

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import gen_arkit_mp_map as G
import gen_arkit_makeup as B   # parse_uvs, build_correspondence, SIZE

SIZE = B.SIZE

# ── ARKit topology constants ────────────────────────────────────────────────
ARC_UP_R = [1100, 1099, 1098, 1097, 1096, 1095, 1094, 1093, 1092, 1091]
ARC_UP_L = [1070, 1071, 1072, 1073, 1074, 1075, 1076, 1077, 1078, 1079]
CORNERS_R = (1101, 1090)   # outer, inner
CORNERS_L = (1069, 1080)
MOUTH_TARGET = (249, 684)  # verts next to the mouth hole (ring lookup)

# MP landmark ids for face anchors (carried over by the correspondence)
MP_CHEEK = (50, 280)
MP_UNDER_EYE = (100, 329)
MP_NOSE_TIP = 1
MP_NOSE_BRIDGE = 6
MP_CHIN = 152
MP_JAW = (172, 397)
MP_FOREHEAD = 10
MP_CUPID = 0


def seed_of(s):
    return int(hashlib.sha256(s.encode()).hexdigest(), 16) % (2 ** 32)


class Canvas:
    """All geometry an element needs, computed once per bake."""

    def __init__(self):
        ak_uv, ak_tris, _, _ = B.parse_uvs()
        self.ak_uv = ak_uv
        self.px = ak_uv * (SIZE - 1)
        ak_v, mp_w = B.build_correspondence()

        def snap(mp_i):
            d = ((ak_v - mp_w[mp_i]) ** 2).sum(1)
            return self.px[int(np.argmin(d))]

        self.cheek = (snap(MP_CHEEK[0]), snap(MP_CHEEK[1]))
        self.under_eye = (snap(MP_UNDER_EYE[0]), snap(MP_UNDER_EYE[1]))
        self.nose_tip = snap(MP_NOSE_TIP)
        self.nose_bridge = snap(MP_NOSE_BRIDGE)
        self.chin = snap(MP_CHIN)
        self.jaw = (snap(MP_JAW[0]), snap(MP_JAW[1]))
        self.forehead = snap(MP_FOREHEAD)
        self.cupid = snap(MP_CUPID)
        self.face_mid = (self.nose_tip + self.chin) * 0.5

        rings = G.boundary_rings(ak_tris, 1220)

        # eyes: full hole ring near the corner pair; upper arc ids known,
        # lower arc = ring minus (upper + corners).
        self.eyes = []
        for corners, up_arc in ((CORNERS_R, ARC_UP_R), (CORNERS_L, ARC_UP_L)):
            center3 = ak_v[list(corners)].mean(0)
            ring = G.ring_near(rings, ak_v, center3)
            ring_set = [v for v in ring if v not in up_arc and v not in corners]
            # order the lower arc outer->inner by projecting onto the
            # corner-to-corner axis
            o, i = self.px[corners[0]], self.px[corners[1]]
            ax = i - o
            ax = ax / (np.linalg.norm(ax) + 1e-9)
            low = sorted(ring_set, key=lambda v: float((self.px[v] - o) @ ax))
            rim = [self.px[corners[0]]] + [self.px[v] for v in up_arc] \
                + [self.px[corners[1]]]
            lower = [self.px[corners[0]]] + [self.px[v] for v in low] \
                + [self.px[corners[1]]]
            self.eyes.append(EyeGeom(np.array(rim), np.array(lower)))

        # mouth: hole ring near the mouth verts, ordered by angle
        mc = ak_v[list(MOUTH_TARGET)].mean(0)
        mring = G.ring_near(rings, ak_v, mc)
        pts = self.px[np.array(mring)]
        c = pts.mean(0)
        ang = np.arctan2(pts[:, 1] - c[1], pts[:, 0] - c[0])
        self.mouth = pts[np.argsort(ang)]
        self.mouth_c = c


class EyeGeom:
    def __init__(self, rim, lower):
        self.rim = rim            # upper rim, outer -> inner
        self.lower = lower        # lower rim, outer -> inner
        self.center = rim.mean(0)
        u = rim[-1] - rim[0]
        self.u_len = np.linalg.norm(u) + 1e-9
        self.u_ax = u / self.u_len
        self.v_ax = np.array([-self.u_ax[1], self.u_ax[0]])
        us = (rim - rim[0]) @ self.u_ax
        vs = (rim - rim[0]) @ self.v_ax
        self.qa, self.qb, self.qc = np.polyfit(us, vs, 2)

    def curve(self, t):
        uu = t * self.u_len
        vv = self.qa * uu * uu + self.qb * uu + self.qc
        return self.rim[0] + self.u_ax * uu + self.v_ax * vv

    def tangent(self, t):
        uu = t * self.u_len
        d = self.u_ax + self.v_ax * (2 * self.qa * uu + self.qb)
        return d / (np.linalg.norm(d) + 1e-9)

    def outward(self, p):
        d = p - self.center
        return d / (np.linalg.norm(d) + 1e-9)


def layer():
    return Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))


def over(base, l):
    return Image.alpha_composite(base, l)


def clampc(c):
    return tuple(max(0, min(255, int(x))) for x in c)


def rgba(c, a):
    return clampc(c) + (max(0, min(255, int(a))),)


def blur(l, px):
    return l.filter(ImageFilter.GaussianBlur(float(px)))


# ── elements (all painted in ARKit UV) ──────────────────────────────────────

def el_blush(cv, img, style, color, alpha, seed=0):
    rng = random.Random(11)
    l = layer()
    d = ImageDraw.Draw(l)
    eye_d = np.linalg.norm(cv.eyes[0].center - cv.eyes[1].center)

    def blob(c, rx, ry, a):
        ox, oy = rng.uniform(-2, 2), rng.uniform(-2, 2)
        d.ellipse([c[0] - rx + ox, c[1] - ry + oy,
                   c[0] + rx + ox, c[1] + ry + oy], fill=rgba(color, a))

    for cheek, jaw, eye in zip(cv.cheek, cv.jaw, cv.eyes):
        if style == "band":
            mid = (cheek + cv.nose_bridge) * 0.5
            blob(cheek, eye_d * 0.30, eye_d * 0.16, alpha * 0.7)
            blob(mid, eye_d * 0.20, eye_d * 0.10, alpha)
        elif style == "lifted":
            c = cheek * 0.7 + eye.center * 0.3
            blob(c, eye_d * 0.20, eye_d * 0.12, alpha * 0.75)
            blob(c * 0.9 + eye.center * 0.1, eye_d * 0.12, eye_d * 0.08, alpha)
        elif style == "apple":
            c = cheek * 0.6 + cv.face_mid * 0.4
            blob(c, eye_d * 0.17, eye_d * 0.14, alpha * 0.75)
            blob(c, eye_d * 0.10, eye_d * 0.08, alpha)
        elif style == "sunkissed":
            blob(cheek, eye_d * 0.28, eye_d * 0.14, alpha * 0.7)
            blob((cheek + cv.nose_tip) * 0.5, eye_d * 0.18, eye_d * 0.09,
                 alpha * 0.8)
        else:  # cheeks
            blob(cheek, eye_d * 0.24, eye_d * 0.16, alpha * 0.7)
            blob(cheek, eye_d * 0.13, eye_d * 0.09, alpha)
    if style == "band":
        blob(cv.nose_bridge, eye_d * 0.22, eye_d * 0.07, alpha * 0.6)
    return over(img, blur(l, eye_d * 0.055))


def el_contour(cv, img, style="soft", color=None, alpha=40, areas=None, seed=0):
    if color is None:
        color = {"warm": (120, 85, 70), "cool": (90, 70, 80),
                 "soft": (105, 85, 78)}[style]
    if areas is None:
        areas = ["cheek", "jaw", "temple", "nose"]
    l = layer()
    d = ImageDraw.Draw(l)
    eye_d = np.linalg.norm(cv.eyes[0].center - cv.eyes[1].center)
    for cheek, jaw, eye in zip(cv.cheek, cv.jaw, cv.eyes):
        if "cheek" in areas:
            c = cheek * 0.55 + jaw * 0.45
            d.ellipse([c[0] - eye_d * 0.22, c[1] - eye_d * 0.08,
                       c[0] + eye_d * 0.22, c[1] + eye_d * 0.08],
                      fill=rgba(color, alpha))
        if "jaw" in areas:
            c = jaw * 0.6 + cv.chin * 0.4
            d.ellipse([c[0] - eye_d * 0.16, c[1] - eye_d * 0.07,
                       c[0] + eye_d * 0.16, c[1] + eye_d * 0.07],
                      fill=rgba(color, alpha * 0.8))
        if "temple" in areas:
            c = cheek * 0.5 + eye.center * 0.5
            c = (c[0] + (c[0] - cv.face_mid[0]) * 0.35, c[1] - eye_d * 0.10)
            d.ellipse([c[0] - eye_d * 0.12, c[1] - eye_d * 0.09,
                       c[0] + eye_d * 0.12, c[1] + eye_d * 0.09],
                      fill=rgba(color, alpha * 0.6))
    if "nose" in areas:
        nb = cv.nose_bridge * 0.4 + cv.nose_tip * 0.6
        for sgn in (-1, 1):
            c = (nb[0] + sgn * eye_d * 0.075, nb[1])
            d.ellipse([c[0] - eye_d * 0.045, c[1] - eye_d * 0.16,
                       c[0] + eye_d * 0.045, c[1] + eye_d * 0.16],
                      fill=rgba(color, alpha * 0.7))
    return over(img, blur(l, eye_d * 0.06))


def _band(eye, t0_t1, lift0, lift1):
    """Polygon between the rim curve (t0..t1) and its outward offset."""
    ts = np.linspace(t0_t1[0], t0_t1[1], 16)
    base = [eye.curve(t) for t in ts]
    top = []
    for t in ts:
        p = eye.curve(t)
        lift = lift0 + (lift1 - lift0) * (t - t0_t1[0]) / (t0_t1[1] - t0_t1[0])
        top.append(tuple(p + eye.outward(p) * lift))
    return [tuple(p) for p in base] + top[::-1]


def el_shadow(cv, img, lid=None, crease=None, outer=None, inner=None,
              shimmer=None, lower=None, height=1.0, seed=0):
    """Layered eye: lid wash, crease depth, outer-V, inner light, lower."""
    eye_d = np.linalg.norm(cv.eyes[0].center - cv.eyes[1].center)
    H = eye_d * 0.34 * height
    for eye in cv.eyes:
        if crease:
            c, a = crease
            l = layer()
            ImageDraw.Draw(l).polygon(
                _band(eye, (0.04, 0.96), H * 0.55, H * 1.05), fill=rgba(c, a))
            img = over(img, blur(l, eye_d * 0.045))
        if lid:
            c, a = lid
            l = layer()
            ImageDraw.Draw(l).polygon(
                _band(eye, (0.02, 0.98), H * 0.10, H * 0.60), fill=rgba(c, a))
            img = over(img, blur(l, eye_d * 0.030))
        if outer:
            c, a = outer
            l = layer()
            ImageDraw.Draw(l).polygon(
                _band(eye, (0.0, 0.30), H * 0.15, H * 0.85), fill=rgba(c, a))
            img = over(img, blur(l, eye_d * 0.040))
        if lower:
            c, a = lower
            l = layer()
            d = ImageDraw.Draw(l)
            pts = np.linspace(0.06, 0.94, 12)
            n = len(eye.lower) - 1
            low_pts = []
            for t in pts:
                p = eye.lower[int(t * n)] * (1 - t * n % 1) \
                    + eye.lower[min(int(t * n) + 1, n)] * (t * n % 1)
                low_pts.append(p)
            band = [tuple(p) for p in low_pts] + [
                tuple(p + eye.outward(p) * H * 0.22) for p in low_pts[::-1]]
            d.polygon(band, fill=rgba(c, a))
            img = over(img, blur(l, eye_d * 0.035))
        if inner:
            c, a = inner
            l = layer()
            d = ImageDraw.Draw(l)
            p = eye.curve(0.97)
            r = eye_d * 0.075
            d.ellipse([p[0] - r, p[1] - r * 0.7, p[0] + r, p[1] + r * 0.7],
                      fill=rgba(c, a))
            img = over(img, blur(l, eye_d * 0.03))
        if shimmer:
            c, a = shimmer
            l = layer()
            d = ImageDraw.Draw(l)
            p = eye.curve(0.5) + eye.outward(eye.curve(0.5)) * H * 0.3
            rx, ry = eye_d * 0.10, eye_d * 0.05
            d.ellipse([p[0] - rx, p[1] - ry, p[0] + rx, p[1] + ry],
                      fill=rgba(c, a))
            img = over(img, blur(l, eye_d * 0.02))
    return img


LINER = {
    "tightline": dict(w=0.006, wing=0.0, alpha=150),
    "soft":      dict(w=0.010, wing=0.0, alpha=180),
    "wing":      dict(w=0.012, wing=0.24, alpha=225),
    "siren":     dict(w=0.011, wing=0.34, alpha=230),
    "graphic":   dict(w=0.016, wing=0.28, alpha=235),
}


def el_liner(cv, img, style="soft", color=(16, 10, 14), alpha=None, seed=0):
    st = LINER[style]
    a = alpha if alpha is not None else st["alpha"]
    eye_d = np.linalg.norm(cv.eyes[0].center - cv.eyes[1].center)
    w = max(3, int(eye_d * st["w"] * 2.2))
    l = layer()
    d = ImageDraw.Draw(l)
    for eye in cv.eyes:
        pts = []
        if st["wing"] > 0:
            tip = eye.curve(0.03) - eye.tangent(0.03) * eye_d * 0.06 * st["wing"] \
                + eye.outward(eye.curve(0.03)) * eye_d * 0.02 * st["wing"]
            pts.append(tuple(tip))
        for t in np.linspace(0.03, 0.97, 28):
            p = eye.curve(t)
            pts.append(tuple(p - eye.outward(p) * w * 0.35))
        d.line(pts, fill=rgba(color, a), width=w, joint="curve")
    return over(img, blur(l, 0.8))


def el_lashes(cv, img, strength=0.8, style="doll", lower=False,
              color=(12, 8, 12), seed=0):
    rng = random.Random(seed + 5)
    eye_d = np.linalg.norm(cv.eyes[0].center - cv.eyes[1].center)
    l = layer()
    d = ImageDraw.Draw(l)
    n_per = {"wispy": 13, "doll": 9, "cat": 11, "stage": 15}[style]
    for eye in cv.eyes:
        n = int(n_per * min(strength, 1.5))
        for k in range(n):
            if style == "cat":
                t = 0.04 + 0.55 * (k + 0.5) / n      # biased to outer half
            else:
                t = 0.06 + 0.88 * (k + 0.5) / n
            base = eye.curve(t)
            out = eye.outward(base)
            tan = eye.tangent(t)
            ln = eye_d * 0.030 * (0.6 + 0.5 * min(strength, 1.4)) \
                * rng.uniform(0.7, 1.3)
            if style == "doll":
                ln *= 1.15
            sweep = rng.uniform(-0.30, 0.10) if style != "cat" else -0.45
            tip = base + out * ln + tan * ln * sweep
            base = base + out * 1.0
            d.line([tuple(base), tuple(tip)], fill=rgba(color, 225), width=3)
        if lower:
            m = max(4, int(6 * strength))
            npts = len(eye.lower) - 1
            for k in range(m):
                t = 0.15 + 0.7 * (k + 0.5) / m
                f = t * npts
                i0 = min(int(f), npts - 1)
                p = eye.lower[i0] * (1 - f + i0) + eye.lower[i0 + 1] * (f - i0)
                out = eye.outward(p)
                ln = eye_d * 0.016 * rng.uniform(0.7, 1.2)
                d.line([tuple(p), tuple(p + out * ln)],
                       fill=rgba(color, 170), width=2)
    return over(img, blur(l, 0.7))


def el_lip(cv, img, style="satin", color=(190, 60, 80), alpha=130,
           liner_color=None, seed=0):
    ring = cv.mouth
    c = cv.mouth_c
    eye_d = np.linalg.norm(cv.eyes[0].center - cv.eyes[1].center)
    # per-point growth: bottom lip fuller than upper, corners tight
    ang = np.arctan2(ring[:, 1] - c[1], ring[:, 0] - c[0])
    lowness = np.clip(np.sin(ang), 0, 1)      # +y in UV = downward? sign-agnostic: use max growth at both mid-top and mid-bottom
    upness = np.clip(-np.sin(ang), 0, 1)
    base_g = 0.055 + 0.075 * lowness + 0.045 * upness   # fraction of eye_d
    if style == "overline":
        base_g *= 1.35
    grow = ring + (ring - c) / (np.linalg.norm(ring - c, axis=1, keepdims=True) + 1e-9) \
        * (eye_d * base_g)[:, None]

    l = layer()
    d = ImageDraw.Draw(l)
    if liner_color is not None:
        grow_l = ring + (grow - ring) * 1.18
        d.polygon([tuple(p) for p in grow_l], fill=rgba(liner_color, alpha * 0.75))
    d.polygon([tuple(p) for p in grow], fill=rgba(color, alpha))

    # center treatment
    if style in ("gloss", "satin", "lined_gloss"):
        low_mid = ring[int(np.argmax(lowness))]
        p = (low_mid + c) * 0.5
        rx, ry = eye_d * 0.09, eye_d * 0.028
        g = layer()
        ImageDraw.Draw(g).ellipse(
            [p[0] - rx, p[1] - ry, p[0] + rx, p[1] + ry],
            fill=(255, 250, 248, int(alpha * (0.55 if style != "satin" else 0.35))))
        l = over(l, blur(g, eye_d * 0.012))
    elif style == "blurred":
        g = layer()
        p = (ring[int(np.argmax(lowness))] + c) * 0.55
        rx, ry = eye_d * 0.13, eye_d * 0.05
        ImageDraw.Draw(g).ellipse(
            [p[0] - rx, p[1] - ry, p[0] + rx, p[1] + ry],
            fill=rgba(color, int(alpha * 0.9)))
        l = over(l, blur(g, eye_d * 0.03))

    l = blur(l, eye_d * 0.010 if style != "blurred" else eye_d * 0.022)

    # clear the mouth hole so teeth/skin show through the aperture
    hole = Image.new("L", (SIZE, SIZE), 255)
    ImageDraw.Draw(hole).polygon([tuple(p) for p in ring * 0.985 + c * 0.015],
                                 fill=0)
    hole = blur(hole, eye_d * 0.008)
    r, g2, b, a = l.split()
    a = ImageChops.multiply(a, hole)
    return over(img, Image.merge("RGBA", (r, g2, b, a)))


def el_highlight(cv, img, style="satin", color=(255, 246, 240), alpha=45, seed=0):
    l = layer()
    d = ImageDraw.Draw(l)
    eye_d = np.linalg.norm(cv.eyes[0].center - cv.eyes[1].center)

    def lobe(p, rx, ry, a):
        d.ellipse([p[0] - rx, p[1] - ry, p[0] + rx, p[1] + ry],
                  fill=rgba(color, a))

    for cheek, eye in zip(cv.cheek, cv.eyes):
        p = cheek * 0.45 + eye.center * 0.55
        lobe(p, eye_d * 0.14, eye_d * 0.05, alpha)
    lobe(cv.nose_bridge * 0.65 + cv.nose_tip * 0.35,
         eye_d * 0.030, eye_d * 0.10, alpha * 0.8)
    lobe(cv.nose_tip + np.array([0, -eye_d * 0.02]),
         eye_d * 0.028, eye_d * 0.020, alpha * 0.7)
    lobe(cv.cupid + np.array([0, eye_d * 0.01]),
         eye_d * 0.035, eye_d * 0.014, alpha * 0.6)
    return over(img, blur(l, eye_d * 0.018))


def el_freckles(cv, img, density=1.0, color=(150, 95, 70), alpha=100, seed=0):
    rng = random.Random(seed + 7)
    l = layer()
    d = ImageDraw.Draw(l)
    eye_d = np.linalg.norm(cv.eyes[0].center - cv.eyes[1].center)
    n = int(46 * density)
    cx = (cv.cheek[0][0] + cv.cheek[1][0]) / 2
    cy = cv.nose_bridge[1] + eye_d * 0.10
    for _ in range(n):
        fx = cx + rng.gauss(0, eye_d * 0.42)
        fy = cy + rng.gauss(0, eye_d * 0.10)
        r = rng.uniform(0.6, 1.6)
        a = alpha * rng.uniform(0.35, 1.0)
        warm = rng.uniform(0.9, 1.1)
        d.ellipse([fx - r, fy - r, fx + r, fy + r],
                  fill=rgba((color[0] * warm, color[1], color[2]), a))
    return over(img, blur(l, 0.6))


def el_aegyo(cv, img, alpha=18, seed=0):
    eye_d = np.linalg.norm(cv.eyes[0].center - cv.eyes[1].center)
    l = layer()
    d = ImageDraw.Draw(l)
    for eye, ue in zip(cv.eyes, cv.under_eye):
        n = len(eye.lower) - 1
        pts = [eye.lower[int(t * n)] for t in np.linspace(0.1, 0.9, 10)]
        band = [tuple(p) for p in pts] + [
            tuple(p + eye.outward(p) * eye_d * 0.045) for p in pts[::-1]]
        d.polygon(band, fill=(255, 244, 240, int(alpha)))
    return over(img, blur(l, eye_d * 0.015))


# ── recipes ─────────────────────────────────────────────────────────────────
def S(lid=None, crease=None, outer=None, inner=None, shimmer=None, lower=None):
    return dict(lid=lid, crease=crease, outer=outer, inner=inner,
                shimmer=shimmer, lower=lower)


LOOKS = {
    # retuned rail heroes (crisp native replacements for the warped plates)
    "douyin": [
        ("blush", dict(style="band", color=(255, 118, 138), alpha=50)),
        ("aegyo", dict(alpha=18)),
        ("shadow", S(lid=((225, 135, 145), 60), crease=((195, 105, 120), 46),
                     outer=((178, 88, 102), 40), inner=((255, 240, 245), 46),
                     shimmer=((255, 245, 250), 30))),
        ("liner", dict(style="wing", alpha=225)),
        ("lashes", dict(strength=0.9, style="doll")),
        ("lashes", dict(strength=0.4, style="wispy", lower=True)),
        ("lip", dict(style="blurred", color=(178, 42, 66), alpha=115)),
        ("highlight", dict(style="glass", color=(255, 245, 248), alpha=50)),
    ],
    "doll_pink": [
        ("contour", dict(style="soft", alpha=26, areas=["cheek", "jaw"])),
        ("blush", dict(style="apple", color=(255, 125, 165), alpha=50)),
        ("aegyo", dict(alpha=18)),
        ("shadow", S(lid=((242, 148, 178), 64), crease=((228, 112, 148), 54),
                     outer=((208, 88, 132), 48), inner=((255, 245, 250), 50),
                     shimmer=((255, 248, 252), 38))),
        ("liner", dict(style="wing", alpha=210)),
        ("lashes", dict(strength=1.1, style="doll")),
        ("lashes", dict(strength=0.45, style="wispy", lower=True)),
        ("lip", dict(style="gloss", color=(235, 75, 125), alpha=110)),
        ("highlight", dict(style="satin", color=(255, 245, 248), alpha=55)),
    ],
    "egirl": [
        ("blush", dict(style="band", color=(255, 96, 110), alpha=58)),
        ("freckles", dict(density=1.5, color=(155, 88, 68), alpha=105)),
        ("shadow", S(lid=((238, 138, 158), 55), crease=((222, 98, 128), 48),
                     outer=((202, 72, 108), 45), inner=((255, 240, 245), 45))),
        ("liner", dict(style="wing", alpha=225)),
        ("lashes", dict(strength=0.95, style="doll")),
        ("lashes", dict(strength=0.4, style="wispy", lower=True)),
        ("aegyo", dict(alpha=12)),
        ("lip", dict(style="gloss", color=(222, 85, 105), alpha=100)),
        ("highlight", dict(style="satin", color=(255, 250, 245), alpha=42)),
    ],
    "glam_contour": [
        ("contour", dict(style="warm", alpha=48)),
        ("blush", dict(style="lifted", color=(225, 155, 140), alpha=40)),
        ("shadow", S(lid=((202, 152, 122), 64), crease=((168, 114, 84), 54),
                     outer=((142, 92, 66), 50), inner=((240, 220, 200), 42),
                     shimmer=((255, 245, 235), 34))),
        ("liner", dict(style="siren", alpha=225)),
        ("lashes", dict(strength=1.0, style="stage")),
        ("lashes", dict(strength=0.35, style="wispy", lower=True)),
        ("lip", dict(style="lined_gloss", color=(172, 110, 96), alpha=120,
                     liner_color=(120, 70, 58))),
        ("highlight", dict(style="satin", color=(255, 245, 235), alpha=60)),
        ("aegyo", dict(alpha=12)),
    ],
    "coquette": [
        ("blush", dict(style="apple", color=(255, 140, 160), alpha=50)),
        ("shadow", S(lid=((228, 162, 178), 48), crease=((208, 137, 152), 40),
                     outer=((192, 118, 137), 34), inner=((255, 240, 245), 42),
                     shimmer=((255, 246, 250), 28))),
        ("liner", dict(style="soft", alpha=175)),
        ("lashes", dict(strength=0.75, style="doll")),
        ("lashes", dict(strength=0.3, style="wispy", lower=True)),
        ("aegyo", dict(alpha=15)),
        ("lip", dict(style="blurred", color=(212, 82, 107), alpha=95)),
        ("highlight", dict(style="satin", color=(255, 250, 245), alpha=44)),
    ],
    "goth": [
        ("contour", dict(style="cool", alpha=45)),
        ("blush", dict(style="apple", color=(140, 70, 90), alpha=35)),
        ("shadow", S(lid=((72, 42, 68), 100), crease=((52, 26, 47), 90),
                     outer=((42, 20, 37), 80), inner=((160, 140, 155), 38),
                     lower=((48, 24, 42), 65))),
        ("liner", dict(style="graphic", alpha=240)),
        ("lashes", dict(strength=0.95, style="stage")),
        ("lashes", dict(strength=0.4, style="wispy", lower=True)),
        ("lip", dict(style="matte", color=(58, 12, 32), alpha=175)),
        ("highlight", dict(style="satin", color=(200, 190, 200), alpha=35)),
    ],
    "peach": [
        ("blush", dict(style="apple", color=(255, 140, 96), alpha=55)),
        ("shadow", S(lid=((242, 162, 112), 50), crease=((222, 137, 92), 42),
                     outer=((202, 117, 77), 36), inner=((255, 240, 225), 42),
                     shimmer=((255, 245, 235), 27))),
        ("liner", dict(style="soft", alpha=180)),
        ("lashes", dict(strength=0.65, style="wispy")),
        ("lip", dict(style="satin", color=(245, 110, 80), alpha=105)),
        ("highlight", dict(style="satin", color=(255, 240, 220), alpha=47)),
    ],
    "cold_beauty": [
        ("contour", dict(style="cool", alpha=35, areas=["cheek", "nose"])),
        ("blush", dict(style="apple", color=(210, 160, 175), alpha=36)),
        ("shadow", S(lid=((152, 142, 162), 50), crease=((122, 112, 137), 42),
                     outer=((102, 92, 117), 37), inner=((220, 215, 230), 42))),
        ("liner", dict(style="tightline", alpha=165)),
        ("lashes", dict(strength=0.55, style="wispy")),
        ("lip", dict(style="matte", color=(150, 70, 80), alpha=110)),
        ("highlight", dict(style="satin", color=(240, 235, 245), alpha=47)),
    ],
    "sunset": [
        ("blush", dict(style="band", color=(255, 120, 70), alpha=52)),
        ("shadow", S(lid=((237, 122, 62), 72), crease=((212, 97, 47), 60),
                     outer=((182, 77, 37), 52), inner=((255, 230, 200), 47),
                     shimmer=((255, 245, 220), 37))),
        ("liner", dict(style="wing", alpha=205)),
        ("lashes", dict(strength=0.85, style="cat")),
        ("lip", dict(style="satin", color=(210, 80, 55), alpha=110)),
        ("highlight", dict(style="satin", color=(255, 220, 170), alpha=54)),
    ],
    "angel": [
        ("blush", dict(style="apple", color=(255, 170, 185), alpha=40)),
        ("aegyo", dict(alpha=20)),
        ("shadow", S(lid=((242, 212, 222), 44), crease=((227, 187, 197), 37),
                     inner=((255, 245, 248), 47), shimmer=((255, 255, 255), 37))),
        ("liner", dict(style="soft", alpha=170)),
        ("lashes", dict(strength=0.6, style="wispy")),
        ("lashes", dict(strength=0.28, style="wispy", lower=True)),
        ("lip", dict(style="gloss", color=(230, 130, 145), alpha=75)),
        ("highlight", dict(style="satin", color=(255, 250, 245), alpha=66)),
    ],
    "baddie": [
        ("contour", dict(style="warm", alpha=45)),
        ("blush", dict(style="lifted", color=(235, 120, 100), alpha=48)),
        ("shadow", S(lid=((202, 152, 92), 60), crease=((177, 127, 72), 50),
                     outer=((152, 102, 57), 44), inner=((255, 235, 215), 37),
                     shimmer=((255, 240, 215), 27))),
        ("liner", dict(style="siren", alpha=230)),
        ("lashes", dict(strength=1.0, style="cat")),
        ("lip", dict(style="overline", color=(170, 100, 85), alpha=130)),
        ("highlight", dict(style="satin", color=(255, 245, 235), alpha=52)),
    ],
    "cyber_chrome": [
        ("blush", dict(style="lifted", color=(200, 220, 235), alpha=30)),
        ("shadow", S(lid=((82, 222, 237), 85), crease=((52, 192, 212), 75),
                     outer=((37, 162, 187), 64), inner=((220, 245, 255), 48),
                     shimmer=((200, 245, 255), 52))),
        ("liner", dict(style="graphic", alpha=225)),
        ("lashes", dict(strength=0.8, style="stage")),
        ("lip", dict(style="matte", color=(60, 70, 90), alpha=115)),
        ("highlight", dict(style="glass", color=(220, 245, 255), alpha=72)),
    ],
    "hearts_freckles": [
        ("blush", dict(style="apple", color=(255, 105, 130), alpha=60)),
        ("freckles", dict(density=1.0, color=(170, 90, 80), alpha=95)),
        ("shadow", S(lid=((227, 142, 157), 47), crease=((207, 117, 132), 40),
                     inner=((255, 240, 245), 42))),
        ("liner", dict(style="wing", alpha=205)),
        ("lashes", dict(strength=0.75, style="doll")),
        ("aegyo", dict(alpha=14)),
        ("lip", dict(style="blurred", color=(227, 77, 97), alpha=100)),
        ("highlight", dict(style="satin", color=(255, 250, 245), alpha=47)),
    ],

    # reference-driven collection (mup/)
    "korean_dewy": [
        ("blush", dict(style="cheeks", color=(255, 150, 140), alpha=36)),
        ("aegyo", dict(alpha=16)),
        ("shadow", S(lid=((227, 172, 162), 42), crease=((207, 147, 142), 32),
                     inner=((255, 245, 240), 42), shimmer=((255, 250, 245), 27))),
        ("liner", dict(style="tightline", color=(60, 40, 36), alpha=155)),
        ("lashes", dict(strength=0.55, style="wispy")),
        ("lip", dict(style="blurred", color=(232, 112, 102), alpha=100)),
        ("highlight", dict(style="glass", color=(255, 250, 248), alpha=57)),
    ],
    "chinese_classic": [
        ("contour", dict(style="soft", alpha=25, areas=["cheek", "nose"])),
        ("blush", dict(style="lifted", color=(230, 130, 120), alpha=30)),
        ("shadow", S(lid=((212, 172, 157), 37), crease=((187, 142, 127), 30),
                     inner=((250, 240, 235), 40))),
        ("liner", dict(style="wing", alpha=232)),
        ("lashes", dict(strength=0.85, style="cat")),
        ("lip", dict(style="satin", color=(190, 30, 45), alpha=145,
                     liner_color=(140, 18, 32))),
        ("highlight", dict(style="satin", color=(255, 248, 245), alpha=42)),
    ],
    "indian_bridal": [
        ("contour", dict(style="warm", alpha=40)),
        ("blush", dict(style="cheeks", color=(230, 120, 105), alpha=42)),
        ("shadow", S(lid=((232, 182, 92), 68), crease=((192, 112, 62), 57),
                     outer=((152, 82, 47), 47), inner=((255, 240, 200), 47),
                     shimmer=((255, 230, 160), 42), lower=((60, 40, 40), 58))),
        ("liner", dict(style="wing", alpha=237)),
        ("lashes", dict(strength=1.05, style="stage")),
        ("lashes", dict(strength=0.4, style="wispy", lower=True)),
        ("lip", dict(style="satin", color=(175, 25, 40), alpha=155,
                     liner_color=(120, 20, 30))),
        ("highlight", dict(style="satin", color=(255, 240, 210), alpha=52)),
    ],
    "bollywood": [
        ("blush", dict(style="cheeks", color=(220, 120, 140), alpha=37)),
        ("shadow", S(lid=((122, 92, 182), 62), crease=((82, 62, 142), 57),
                     outer=((47, 37, 92), 52), inner=((230, 220, 255), 42),
                     shimmer=((210, 200, 255), 37), lower=((92, 72, 152), 42))),
        ("liner", dict(style="wing", alpha=232)),
        ("lashes", dict(strength=1.05, style="stage")),
        ("lip", dict(style="gloss", color=(215, 85, 130), alpha=115)),
        ("highlight", dict(style="satin", color=(245, 240, 255), alpha=47)),
    ],
    "latina_glam": [
        ("contour", dict(style="warm", alpha=48)),
        ("blush", dict(style="lifted", color=(225, 140, 115), alpha=44)),
        ("shadow", S(lid=((217, 167, 117), 57), crease=((182, 132, 87), 52),
                     outer=((147, 102, 62), 47), inner=((250, 235, 215), 42),
                     shimmer=((255, 235, 200), 32))),
        ("liner", dict(style="wing", alpha=222)),
        ("lashes", dict(strength=1.0, style="stage")),
        ("lashes", dict(strength=0.35, style="wispy", lower=True)),
        ("lip", dict(style="lined_gloss", color=(185, 130, 110), alpha=115,
                     liner_color=(130, 85, 70))),
        ("highlight", dict(style="satin", color=(255, 245, 230), alpha=54)),
    ],
    "chola": [
        ("contour", dict(style="soft", alpha=40)),
        ("blush", dict(style="lifted", color=(210, 130, 115), alpha=30)),
        ("shadow", S(lid=((207, 162, 132), 42), crease=((177, 127, 97), 37),
                     inner=((245, 230, 215), 37))),
        ("liner", dict(style="siren", alpha=228)),
        ("lashes", dict(strength=0.9, style="cat")),
        ("lip", dict(style="lined_gloss", color=(160, 95, 80), alpha=135,
                     liner_color=(90, 45, 40))),
        ("highlight", dict(style="satin", color=(250, 240, 230), alpha=40)),
    ],
    "pinup": [
        ("contour", dict(style="soft", alpha=30, areas=["cheek"])),
        ("blush", dict(style="apple", color=(235, 130, 120), alpha=40)),
        ("shadow", S(lid=((237, 222, 207), 32), crease=((202, 172, 152), 30),
                     inner=((250, 245, 240), 37))),
        ("liner", dict(style="wing", alpha=237)),
        ("lashes", dict(strength=0.95, style="doll")),
        ("lip", dict(style="matte", color=(190, 25, 40), alpha=175,
                     liner_color=(140, 15, 30))),
        ("highlight", dict(style="satin", color=(255, 248, 242), alpha=34)),
    ],
    "arab_kohl": [
        ("contour", dict(style="warm", alpha=42)),
        ("blush", dict(style="cheeks", color=(215, 130, 115), alpha=34)),
        ("shadow", S(lid=((217, 172, 122), 57), crease=((172, 122, 87), 57),
                     outer=((122, 82, 62), 57), inner=((245, 230, 210), 37),
                     lower=((60, 40, 40), 72))),
        ("liner", dict(style="siren", alpha=237)),
        ("lashes", dict(strength=1.05, style="stage")),
        ("lashes", dict(strength=0.45, style="wispy", lower=True)),
        ("lip", dict(style="satin", color=(165, 95, 90), alpha=125,
                     liner_color=(120, 65, 62))),
        ("highlight", dict(style="satin", color=(255, 242, 225), alpha=47)),
    ],
    "siren_night": [
        ("contour", dict(style="cool", alpha=35)),
        ("blush", dict(style="lifted", color=(200, 120, 140), alpha=30)),
        ("shadow", S(lid=((167, 122, 167), 57), crease=((132, 92, 142), 52),
                     outer=((97, 62, 107), 50), inner=((240, 225, 245), 42),
                     shimmer=((230, 215, 245), 32), lower=((122, 87, 132), 37))),
        ("liner", dict(style="siren", alpha=232)),
        ("lashes", dict(strength=1.05, style="cat")),
        ("lip", dict(style="gloss", color=(140, 40, 70), alpha=135,
                     liner_color=(95, 25, 50))),
        ("highlight", dict(style="satin", color=(245, 240, 250), alpha=47)),
    ],
    "fox_eye": [
        ("contour", dict(style="warm", alpha=30, areas=["cheek", "temple"])),
        ("blush", dict(style="lifted", color=(225, 150, 130), alpha=27)),
        ("shadow", S(lid=((212, 167, 127), 42), crease=((182, 137, 97), 40),
                     outer=((152, 107, 72), 47), inner=((250, 240, 225), 37))),
        ("liner", dict(style="siren", alpha=228)),
        ("lashes", dict(strength=0.95, style="cat")),
        ("lip", dict(style="satin", color=(195, 140, 125), alpha=95)),
        ("highlight", dict(style="satin", color=(255, 245, 235), alpha=44)),
    ],
    "doe_eye": [
        ("blush", dict(style="apple", color=(250, 150, 155), alpha=42)),
        ("aegyo", dict(alpha=22)),
        ("shadow", S(lid=((242, 197, 177), 42), crease=((222, 162, 147), 32),
                     inner=((255, 250, 245), 57), lower=((255, 245, 240), 32))),
        ("liner", dict(style="tightline", color=(50, 35, 32), alpha=155)),
        ("lashes", dict(strength=1.0, style="doll")),
        ("lashes", dict(strength=0.5, style="doll", lower=True)),
        ("lip", dict(style="gloss", color=(240, 140, 140), alpha=90)),
        ("highlight", dict(style="satin", color=(255, 248, 248), alpha=52)),
    ],
    "y2k_glow": [
        ("blush", dict(style="sunkissed", color=(255, 140, 95), alpha=42)),
        ("shadow", S(lid=((247, 192, 102), 57), crease=((237, 122, 72), 52),
                     outer=((212, 102, 57), 42), inner=((255, 245, 220), 47),
                     shimmer=((255, 240, 200), 47))),
        ("liner", dict(style="soft", color=(70, 45, 40), alpha=175)),
        ("lashes", dict(strength=0.75, style="wispy")),
        ("lip", dict(style="gloss", color=(245, 170, 140), alpha=65)),
        ("highlight", dict(style="glass", color=(255, 245, 225), alpha=57)),
    ],
    "nineties_brown": [
        ("contour", dict(style="soft", alpha=35)),
        ("blush", dict(style="cheeks", color=(200, 130, 110), alpha=30)),
        ("shadow", S(lid=((172, 137, 117), 47), crease=((142, 107, 92), 44),
                     outer=((117, 87, 72), 40), inner=((225, 210, 200), 32))),
        ("liner", dict(style="soft", color=(60, 40, 35), alpha=190)),
        ("lashes", dict(strength=0.65, style="wispy")),
        ("lip", dict(style="matte", color=(135, 85, 70), alpha=155,
                     liner_color=(95, 60, 50))),
        ("highlight", dict(style="satin", color=(245, 238, 230), alpha=32)),
    ],
    "eighties_pop": [
        ("blush", dict(style="band", color=(245, 120, 150), alpha=52)),
        ("shadow", S(lid=((92, 122, 222), 62), crease=((122, 82, 192), 57),
                     outer=((82, 52, 152), 52), inner=((220, 225, 255), 42),
                     shimmer=((200, 215, 255), 42), lower=((110, 90, 190), 35))),
        ("liner", dict(style="wing", alpha=222)),
        ("lashes", dict(strength=0.95, style="stage")),
        ("lip", dict(style="gloss", color=(215, 60, 120), alpha=125)),
        ("highlight", dict(style="satin", color=(240, 240, 255), alpha=50)),
    ],
    "seventies_sun": [
        ("blush", dict(style="sunkissed", color=(245, 150, 95), alpha=47)),
        ("shadow", S(lid=((217, 162, 107), 47), crease=((187, 132, 87), 42),
                     outer=((162, 107, 72), 37), inner=((255, 240, 215), 42),
                     lower=((172, 122, 82), 37))),
        ("liner", dict(style="soft", color=(65, 45, 38), alpha=170)),
        ("lashes", dict(strength=0.85, style="doll")),
        ("lashes", dict(strength=0.45, style="wispy", lower=True)),
        ("lip", dict(style="gloss", color=(240, 150, 120), alpha=100)),
        ("highlight", dict(style="satin", color=(255, 235, 200), alpha=57)),
    ],
    "cut_crease": [
        ("contour", dict(style="soft", alpha=30)),
        ("blush", dict(style="lifted", color=(225, 145, 125), alpha=32)),
        ("shadow", S(lid=((247, 227, 202), 52), crease=((152, 117, 97), 78),
                     outer=((122, 92, 77), 62), inner=((255, 245, 235), 47),
                     shimmer=((255, 250, 240), 37))),
        ("liner", dict(style="wing", alpha=228)),
        ("lashes", dict(strength=0.95, style="stage")),
        ("lip", dict(style="lined_gloss", color=(185, 135, 120), alpha=105,
                     liner_color=(140, 95, 85))),
        ("highlight", dict(style="satin", color=(255, 248, 240), alpha=47)),
    ],
    "halo_eye": [
        ("blush", dict(style="cheeks", color=(240, 140, 120), alpha=37)),
        ("shadow", S(lid=((237, 177, 97), 62), crease=((162, 102, 62), 47),
                     outer=((142, 87, 52), 52), inner=((255, 240, 215), 42),
                     shimmer=((255, 225, 160), 52))),
        ("liner", dict(style="soft", color=(55, 40, 35), alpha=185)),
        ("lashes", dict(strength=0.8, style="wispy")),
        ("lip", dict(style="satin", color=(185, 125, 120), alpha=105)),
        ("highlight", dict(style="satin", color=(255, 240, 215), alpha=52)),
    ],
    "editorial_violet": [
        ("contour", dict(style="cool", alpha=25, areas=["cheek"])),
        ("blush", dict(style="cheeks", color=(205, 130, 160), alpha=27)),
        ("shadow", S(lid=((172, 72, 162), 67), crease=((132, 52, 122), 57),
                     outer=((97, 37, 87), 52), inner=((240, 225, 250), 37),
                     lower=((152, 62, 142), 42))),
        ("liner", dict(style="graphic", alpha=228)),
        ("lashes", dict(strength=0.65, style="wispy")),
        ("lip", dict(style="matte", color=(130, 70, 90), alpha=125)),
        ("highlight", dict(style="satin", color=(250, 245, 255), alpha=40)),
    ],
    "soft_bridal": [
        ("blush", dict(style="cheeks", color=(245, 145, 150), alpha=44)),
        ("aegyo", dict(alpha=10)),
        ("shadow", S(lid=((242, 182, 187), 57), crease=((212, 142, 152), 44),
                     outer=((187, 112, 127), 37), inner=((255, 245, 248), 52),
                     shimmer=((255, 250, 252), 42))),
        ("liner", dict(style="wing", alpha=218)),
        ("lashes", dict(strength=1.05, style="doll")),
        ("lashes", dict(strength=0.35, style="wispy", lower=True)),
        ("lip", dict(style="satin", color=(200, 60, 80), alpha=140,
                     liner_color=(160, 40, 60))),
        ("highlight", dict(style="satin", color=(255, 248, 248), alpha=57)),
    ],
    "tribal_earth": [
        ("contour", dict(style="warm", alpha=45)),
        ("blush", dict(style="lifted", color=(200, 125, 95), alpha=35)),
        ("shadow", S(lid=((192, 142, 97), 52), crease=((152, 107, 72), 50),
                     outer=((122, 82, 57), 44), inner=((240, 220, 190), 32))),
        ("liner", dict(style="soft", color=(45, 32, 28), alpha=195)),
        ("lashes", dict(strength=0.7, style="wispy")),
        ("lip", dict(style="matte", color=(140, 90, 75), alpha=140)),
        ("highlight", dict(style="satin", color=(245, 230, 205), alpha=42)),
    ],
}

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

    print("building ARKit geometry canvas...")
    cv = Canvas()
    ids = args.only or list(LOOKS.keys())
    for look_id in ids:
        recipe = LOOKS[look_id]
        img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
        for name, kw in recipe:
            kw = dict(kw)
            kw.setdefault("seed", seed_of(look_id))
            img = EL[name](cv, img, **kw)
        path = os.path.join(out_dir, f"makeup_{look_id}.png")
        img.save(path, optimize=True)
        print(f"  painted {os.path.basename(path)} "
              f"({os.path.getsize(path) // 1024} KB)")
    print(f"done — {len(ids)} native atlases")


if __name__ == "__main__":
    main()
