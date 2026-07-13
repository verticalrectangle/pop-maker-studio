#!/usr/bin/env python3
'''Makeup look-texture generator — variant factory (MAKEUP_PLAN Stage 1/3/4).

Paints UV-space makeup textures (MediaPipe canonical space) from a data-driven
LOOKS table. Each look is a recipe of translucent, layered elements: lid/crease/
outer-V/inner-light eyeshadow, directional blush, separate contour, multi-layer
lips, tapered lashes, seeded brow hairs, irregular freckles, and low-opacity
highlights.

Output: models/face/makeup_<id>.png (RGBA 1024², ~0.5-luma skin). Also copied to
../pms-ios/Engine/EngineAssets/models/face for the app bundle.

Deterministic (seeded per look) so regeneration is byte-reproducible.
'''
import math, os, random, hashlib, shutil
from PIL import Image, ImageChops, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'models', 'face')
PMS_IOS_OUT = os.path.normpath(os.path.join(HERE, '..', '..', 'pms-ios', 'Engine', 'EngineAssets', 'models', 'face'))
SZ = 1024

# ── canonical UVs (same loader as paint_makeup_douyin.py) ────────────────────
vt_raw, v2vt = [], {}
for line in open(os.path.join(HERE, 'canonical_face_model.obj')):
    p = line.split()
    if not p:
        continue
    if p[0] == 'vt':
        vt_raw.append((float(p[1]), float(p[2])))
    elif p[0] == 'f':
        for x in p[1:4]:
            vi, ti = x.split('/')[:2]
            v2vt[int(vi) - 1] = int(ti) - 1
uv = [(vt_raw[v2vt[i]][0] * SZ, (1.0 - vt_raw[v2vt[i]][1]) * SZ) for i in range(468)]

def P(i): return uv[i]
def lerp(a, b, t): return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
def dist(a, b): return math.hypot(a[0] - b[0], a[1] - b[1])

eyeL_c = lerp(P(33), P(133), 0.5)
eyeR_c = lerp(P(263), P(362), 0.5)
ed = dist(eyeL_c, eyeR_c)
face_mid = lerp(P(1), P(152), 0.15)

LID_L = [33, 161, 160, 159, 158, 157, 133]
LID_R = [263, 388, 387, 386, 385, 384, 362]
LOW_L = [33, 7, 163, 144, 145, 153, 154, 155, 133]
LOW_R = [263, 249, 390, 373, 374, 380, 381, 382, 362]
LIP_OUTER = [61, 40, 37, 0, 267, 270, 291, 321, 314, 17, 84, 91]
LIP_INNER = [78, 191, 80, 81, 82, 13, 312, 311, 310, 415, 308,
             324, 318, 402, 317, 14, 87, 178, 88, 95]
BROW_L = [70, 63, 105, 66, 107]
BROW_R = [300, 293, 334, 296, 336]
JAW_L, JAW_R = P(172), P(397)
CHIN = P(152)
NOSE_TIP = P(1)
nose_bridge = lerp(lerp(eyeL_c, eyeR_c, 0.5), NOSE_TIP, 0.45)

def layer(): return Image.new('RGBA', (SZ, SZ), (0, 0, 0, 0))
def over(base, l): return Image.alpha_composite(base, l)

def look_seed(s):
    return int(hashlib.sha256(s.encode()).hexdigest(), 16) % (2 ** 32)

def _clamp(c):
    return tuple(max(0, min(255, int(ch))) for ch in c)

def _darken(c, f):
    return _clamp(tuple(int(ch * f) for ch in c))

def _lighten(c, target, f):
    return _clamp(tuple(int(ch + (t - ch) * f) for ch, t in zip(c, target)))

def soft_ellipse(center, rx, ry, color, alpha, blur):
    l = layer()
    d = ImageDraw.Draw(l)
    d.ellipse([center[0] - rx, center[1] - ry, center[0] + rx, center[1] + ry],
              fill=_clamp(color) + (max(0, min(255, int(alpha))),))
    return l.filter(ImageFilter.GaussianBlur(blur))

def poly_blur(poly, color, alpha, blur):
    l = layer()
    d = ImageDraw.Draw(l)
    d.polygon(poly, fill=_clamp(color) + (max(0, min(255, int(alpha))),))
    return l.filter(ImageFilter.GaussianBlur(blur))

def point_on_poly(pts, t):
    n = len(pts) - 1
    if n <= 0:
        return pts[0]
    t = max(0.0, min(1.0, t))
    idx = min(n - 1, int(t * n))
    fr = t * n - idx
    return lerp(pts[idx], pts[idx + 1], fr)

def draw_tapered_line(d, p0, p1, w0, w1, fill):
    dx, dy = p1[0] - p0[0], p1[1] - p0[1]
    ln = math.hypot(dx, dy)
    if ln < 0.001:
        return
    ux, uy = -dy / ln, dx / ln
    poly = [
        (p0[0] + ux * w0 / 2.0, p0[1] + uy * w0 / 2.0),
        (p1[0] + ux * w1 / 2.0, p1[1] + uy * w1 / 2.0),
        (p1[0] - ux * w1 / 2.0, p1[1] - uy * w1 / 2.0),
        (p0[0] - ux * w0 / 2.0, p0[1] - uy * w0 / 2.0),
    ]
    d.polygon(poly, fill=fill)

def scale_poly(pts, cx, cy, sx, sy=None):
    if sy is None:
        sy = sx
    return [(cx + (x - cx) * sx, cy + (y - cy) * sy) for x, y in pts]

# ── element helpers ─────────────────────────────────────────────────────────

def _eye_sets(kind):
    if kind == 'lower':
        return [(LOW_L, -1), (LOW_R, +1)]
    return [(LID_L, -1), (LID_R, +1)]

def _draw_eye_line(d, lid, color, alpha, width, raise_amt, blur, offset):
    l = layer()
    dl = ImageDraw.Draw(l)
    for lidpts, sign in lid:
        ox, oy = offset()
        pts = [(P(i)[0] + ox, P(i)[1] - ed * raise_amt + oy) for i in lidpts]
        w = max(1, int(ed * width))
        dl.line(pts, fill=_clamp(color) + (alpha,), width=w, joint='curve')
    return l.filter(ImageFilter.GaussianBlur(ed * blur))

def _draw_outer_v(d, lid, color, alpha, extent, raise_amt, blur, offset):
    l = layer()
    dl = ImageDraw.Draw(l)
    for lidpts, sign in lid:
        ox, oy = offset()
        outer = P(lidpts[0])
        p1 = P(lidpts[1])
        tip = (outer[0] + sign * ed * extent + ox, outer[1] - ed * raise_amt + oy)
        o = (outer[0] + ox, outer[1] + oy)
        p1 = (p1[0] + ox, p1[1] + oy)
        dl.polygon([tip, o, p1], fill=_clamp(color) + (alpha,))
    return l.filter(ImageFilter.GaussianBlur(ed * blur))

def _side_offset(rng, mx=0.015, my=0.008):
    return (rng.uniform(-ed * mx, ed * mx), rng.uniform(-ed * my, ed * my))

# ── elements ───────────────────────────────────────────────────────────────

def el_blush(img, style, color, alpha, seed=0):
    rng = random.Random(seed + 1)
    color = _clamp(color)
    alpha = max(0, min(255, int(alpha)))
    l = layer()

    def make(kind):
        if kind == 'cheeks':
            return [
                ('cheek', 0.38, 0.16, 0.28, 0.20, ed * 0.15, 0.65),
                ('cheek', 0.45, 0.14, 0.18, 0.13, ed * 0.10, 1.0),
            ]
        if kind == 'apple':
            return [
                ('apple', 0.35, 0.0, 0.20, 0.16, ed * 0.13, 0.70),
                ('apple', 0.28, 0.0, 0.14, 0.11, ed * 0.09, 1.0),
            ]
        if kind == 'lifted':
            return [
                ('lift', 0.65, 0.08, 0.15, 0.22, ed * 0.14, 0.75),
                ('lift', 0.70, 0.12, 0.10, 0.15, ed * 0.10, 1.0),
            ]
        if kind in ('band', 'draped'):
            return [
                ('band', 0.30, 0.12, 0.30, 0.18, ed * 0.14, 0.75),
                ('band', 0.35, 0.16, 0.20, 0.12, ed * 0.10, 1.0),
                ('nose', 0.0, 0.0, 0.26, 0.12, ed * 0.12, 0.75),
            ]
        if kind == 'sunkissed':
            return [
                ('sun', 0.22, 0.0, 0.32, 0.20, ed * 0.16, 0.65),
                ('sun', 0.28, 0.0, 0.22, 0.14, ed * 0.12, 0.90),
                ('nose', 0.0, 0.0, 0.28, 0.13, ed * 0.13, 0.80),
            ]
        if kind == 'sculpted':
            return [
                ('hollow', 0.45, 0.0, 0.22, 0.10, ed * 0.14, 0.70),
                ('lift', 0.55, 0.10, 0.16, 0.12, ed * 0.11, 1.0),
            ]
        return make('cheeks')

    for placement, t1, t2, rx, ry, blur, af in make(style):
        for side, eye in ((P(50), eyeL_c), (P(280), eyeR_c)):
            ox, oy = _side_offset(rng)
            if placement == 'cheek':
                c = lerp(lerp(side, eye, t1), face_mid, t2)
            elif placement == 'apple':
                c = lerp(side, face_mid, t1)
            elif placement == 'lift':
                c = lerp(lerp(side, eye, t1), face_mid, t2)
            elif placement == 'band':
                c = lerp(lerp(side, eye, t1), face_mid, t2)
            elif placement == 'sun':
                c = lerp(side, face_mid, t1)
            elif placement == 'hollow':
                c = lerp(side, JAW_L if side == P(50) else JAW_R, t1)
            else:
                c = side
            c = (c[0] + ox, c[1] + oy)
            col = _darken(color, 0.55) if placement == 'hollow' else color
            l = over(l, soft_ellipse(c, ed * rx, ed * ry, col, int(alpha * af), blur))
        if placement == 'nose':
            ox, oy = _side_offset(rng)
            l = over(l, soft_ellipse((nose_bridge[0] + ox, nose_bridge[1] + oy),
                                      ed * rx, ed * ry, color, int(alpha * af), blur))

    return over(img, l)

def el_contour(img, style='soft', color=None, alpha=None, areas=None, seed=0):
    rng = random.Random(seed + 2)
    if color is None:
        color = {'warm': (120, 85, 70), 'cool': (90, 70, 80), 'soft': (105, 85, 78)}.get(style, (105, 85, 78))
    if alpha is None:
        alpha = 45
    color = _clamp(color)
    alpha = max(0, min(255, int(alpha)))
    if areas is None:
        areas = ['cheek', 'jaw', 'temple', 'nose']
    l = layer()
    for area in areas:
        for side, jaw, temple, nose_side in ((P(50), JAW_L, P(70), P(49)),
                                              (P(280), JAW_R, P(300), P(279))):
            ox, oy = _side_offset(rng)
            if area == 'cheek':
                c = lerp(side, jaw, 0.45)
                c = (c[0] + ox, c[1] + oy)
                l = over(l, soft_ellipse(c, ed * 0.22, ed * 0.10, color, alpha, ed * 0.14))
            elif area == 'jaw':
                c = lerp(jaw, CHIN, 0.45)
                c = (c[0] + ox, c[1] + oy)
                l = over(l, soft_ellipse(c, ed * 0.18, ed * 0.08, color, int(alpha * 0.85), ed * 0.12))
            elif area == 'temple':
                c = (temple[0] + ox, temple[1] + oy)
                l = over(l, soft_ellipse(c, ed * 0.14, ed * 0.10, color, int(alpha * 0.70), ed * 0.14))
            elif area == 'nose':
                c = (nose_side[0] + ox, nose_side[1] + oy)
                l = over(l, soft_ellipse(c, ed * 0.06, ed * 0.18, color, int(alpha * 0.75), ed * 0.10))
    return over(img, l)

def el_shadow(img, color=None, alpha=None, palette=None, layers=None, seed=0, width=0.16, raise_amt=0.085, blur=0.09):
    if layers is not None:
        specs = layers
    elif palette is not None:
        order = ['lid', 'crease', 'outer', 'inner', 'shimmer', 'lower']
        specs = []
        for kind in order:
            if kind in palette and palette[kind] is not None:
                val = palette[kind]
                if isinstance(val, (list, tuple)) and len(val) == 2:
                    specs.append({'kind': kind, 'color': val[0], 'alpha': val[1]})
                else:
                    specs.append({'kind': kind, **val})
    elif color is not None and alpha is not None:
        specs = [{'kind': 'lid', 'color': color, 'alpha': alpha, 'width': width, 'raise': raise_amt, 'blur': blur}]
    else:
        return img

    l = layer()
    for i, spec in enumerate(specs):
        rng = random.Random(seed + 10 + i)
        kind = spec['kind']
        color = _clamp(spec['color'])
        alpha = max(0, min(255, int(spec['alpha'])))
        sets = _eye_sets(kind)
        off = lambda: _side_offset(rng)
        if kind in ('lid', 'crease', 'lower'):
            width = spec.get('width', 0.16 if kind == 'lid' else 0.09 if kind == 'crease' else 0.10)
            raise_amt = spec.get('raise', 0.085 if kind == 'lid' else 0.18 if kind == 'crease' else 0.04)
            blur = spec.get('blur', 0.09 if kind == 'lid' else 0.12 if kind == 'crease' else 0.10)
            l = over(l, _draw_eye_line(None, sets, color, alpha, width, raise_amt, blur, off))
        elif kind == 'outer':
            extent = spec.get('extent', 0.22)
            raise_amt = spec.get('raise', 0.08)
            blur = spec.get('blur', 0.11)
            l = over(l, _draw_outer_v(None, sets, color, alpha, extent, raise_amt, blur, off))
        elif kind in ('inner', 'shimmer'):
            rad = spec.get('radius', (0.06, 0.05) if kind == 'inner' else (0.05, 0.04))
            if isinstance(rad, (int, float)):
                rad = (rad, rad)
            blur = spec.get('blur', 0.06 if kind == 'inner' else 0.05)
            for lidpts, sign in sets:
                ox, oy = off()
                if kind == 'inner':
                    c = (P(lidpts[-1])[0] + ox, P(lidpts[-1])[1] + oy)
                else:
                    mid = lidpts[len(lidpts) // 2]
                    c = (P(mid)[0] + ox, P(mid)[1] + oy)
                l = over(l, soft_ellipse(c, ed * rad[0], ed * rad[1], color, alpha, ed * blur))
    return over(img, l)

def el_aegyo(img, alpha=30, seed=0):
    # Bag-safe aegyo: sheer highlight tight under the lash line only.
    # No dark crease — the old light+shadow pair sculpted real bags
    # (highlight on the puff, trough under it). Blur ALPHA ONLY: RGBA
    # GaussianBlur pulls RGB toward 0 and turns a "highlight" into a
    # muddy multiply that the material-aware compositor deepens further.
    l = layer()
    d = ImageDraw.Draw(l)
    a = max(0, min(255, int(alpha)))
    for lows in (LOW_L, LOW_R):
        pts = [(x, y + ed * 0.028) for x, y in (P(i) for i in lows)]
        d.line(pts, fill=(255, 242, 244, a), width=max(1, int(ed * 0.040)), joint='curve')
    _, _, _, al = l.split()
    al = al.filter(ImageFilter.GaussianBlur(ed * 0.028))
    l = Image.merge('RGBA', (
        Image.new('L', l.size, 255),
        Image.new('L', l.size, 242),
        Image.new('L', l.size, 244),
        al))
    return over(img, l)

def el_freckles(img, density=1.0, color=(150, 92, 66), alpha=110, seed=0):
    rng = random.Random(seed + 3)
    color = _clamp(color)
    l = layer()
    d = ImageDraw.Draw(l)
    zones = [(nose_bridge, ed * 0.55, ed * 0.28)]
    for cheek, eye in ((P(50), eyeL_c), (P(280), eyeR_c)):
        zones.append((lerp(cheek, eye, 0.30), ed * 0.30, ed * 0.22))
    for c, rx, ry in zones:
        ox, oy = _side_offset(rng, 0.02, 0.01)
        c = (c[0] + ox, c[1] + oy)
        for _ in range(int(density * 26)):
            a = rng.uniform(0, 2 * math.pi)
            r = rng.uniform(0, 1) ** 0.5
            x = c[0] + math.cos(a) * rx * r
            y = c[1] + math.sin(a) * ry * r
            rad = ed * rng.uniform(0.004, 0.016)
            op = int(alpha * rng.uniform(0.35, 1.0))
            col = tuple(max(0, min(255, color[i] + rng.randint(-18, 18))) for i in range(3))
            d.ellipse([x - rad, y - rad, x + rad, y + rad], fill=col + (op,))
    return over(img, l.filter(ImageFilter.GaussianBlur(ed * 0.006)))

LIP_FINISH = {
    'satin':        {'blur': 0.018, 'base_alpha': 1.0, 'base_scale': 1.0,   'liner_scale': 1.03, 'liner_alpha': 0.55, 'center': 'satin',   'center_color': (255, 250, 245), 'center_alpha': 0.40, 'center_scale': (0.12, 0.04), 'upper': False},
    'gloss':        {'blur': 0.018, 'base_alpha': 1.0, 'base_scale': 1.0,   'liner_scale': 1.03, 'liner_alpha': 0.60, 'center': 'gloss',   'center_color': (255, 255, 255), 'center_alpha': 0.55, 'center_scale': (0.18, 0.04), 'upper': True},
    'matte':        {'blur': 0.024, 'base_alpha': 1.0, 'base_scale': 1.0,   'liner_scale': 1.03, 'liner_alpha': 0.65, 'center': 'none',    'center_color': None,           'center_alpha': 0.0,  'center_scale': (0.0, 0.0),    'upper': False},
    'full':         {'blur': 0.020, 'base_alpha': 1.0, 'base_scale': 1.0,   'liner_scale': 1.03, 'liner_alpha': 0.55, 'center': 'satin',   'center_color': (255, 250, 245), 'center_alpha': 0.30, 'center_scale': (0.10, 0.04), 'upper': False},
    'velvet':       {'blur': 0.026, 'base_alpha': 1.0, 'base_scale': 1.0,   'liner_scale': 1.03, 'liner_alpha': 0.65, 'center': 'dark',    'center_color': None,           'center_alpha': 0.55, 'center_scale': (0.12, 0.04), 'upper': False},
    'bitten':       {'blur': 0.026, 'base_alpha': 0.95, 'base_scale': 1.0,  'liner_scale': None, 'liner_alpha': 0.0,  'center': 'bitten',  'center_color': None,           'center_alpha': 0.85, 'center_scale': (0.28, 0.12), 'upper': False},
    'overline':     {'blur': 0.020, 'base_alpha': 1.0, 'base_scale': (1.10, 1.14), 'liner_scale': None, 'liner_alpha': 0.0,  'center': 'satin',   'center_color': (255, 250, 245), 'center_alpha': 0.45, 'center_scale': (0.12, 0.04), 'upper': False},
    'blurred':      {'blur': 0.032, 'base_alpha': 0.75, 'base_scale': 0.96,  'liner_scale': None, 'liner_alpha': 0.0,  'center': 'blurred', 'center_color': None,           'center_alpha': 1.40, 'center_scale': (0.16, 0.06), 'upper': False},
    'blurred_matte':{'blur': 0.034, 'base_alpha': 0.75, 'base_scale': 0.96,  'liner_scale': None, 'liner_alpha': 0.0,  'center': 'none',    'center_color': None,           'center_alpha': 0.0,  'center_scale': (0.0, 0.0),    'upper': False},
    'satin_balm':   {'blur': 0.016, 'base_alpha': 0.75, 'base_scale': 1.0,   'liner_scale': None, 'liner_alpha': 0.0,  'center': 'gloss',   'center_color': (255, 250, 245), 'center_alpha': 0.50, 'center_scale': (0.16, 0.05), 'upper': True},
    'balm':         {'blur': 0.016, 'base_alpha': 0.70, 'base_scale': 1.0,   'liner_scale': None, 'liner_alpha': 0.0,  'center': 'satin',   'center_color': (255, 250, 245), 'center_alpha': 0.35, 'center_scale': (0.14, 0.05), 'upper': False},
    'lined_gloss':  {'blur': 0.018, 'base_alpha': 1.0, 'base_scale': 1.0,   'liner_scale': 1.05, 'liner_alpha': 0.80, 'center': 'gloss',   'center_color': (255, 255, 255), 'center_alpha': 0.60, 'center_scale': (0.18, 0.04), 'upper': True},
    'iridescent':   {'blur': 0.018, 'base_alpha': 1.0, 'base_scale': 1.0,   'liner_scale': 1.03, 'liner_alpha': 0.60, 'center': 'gloss',   'center_color': (245, 235, 255), 'center_alpha': 0.50, 'center_scale': (0.18, 0.04), 'upper': True},
}

def el_lip(img, style=None, color=None, alpha=None, seed=0, **kw):
    if color is None:
        return img
    if alpha is None:
        alpha = 120
    finish = LIP_FINISH.get(style, LIP_FINISH['satin'])
    color = _clamp(color)
    alpha = max(0, min(255, int(alpha)))
    base_alpha = max(0, min(255, int(alpha * finish['base_alpha'])))
    base = layer()
    d = ImageDraw.Draw(base)
    outer = [P(i) for i in LIP_OUTER]
    cx = sum(x for x, _ in outer) / len(outer)
    cy = sum(y for _, y in outer) / len(outer)
    base_s = finish['base_scale']
    if isinstance(base_s, (int, float)):
        base_s = (base_s, base_s)
    base_outer = scale_poly(outer, cx, cy, base_s[0], base_s[1])

    # liner ring
    if finish['liner_scale'] is not None:
        liner = layer()
        dl = ImageDraw.Draw(liner)
        ls = finish['liner_scale']
        liner_outer = scale_poly(outer, cx, cy, ls, ls)
        liner_color = kw.get('liner_color') or _darken(color, 0.65)
        liner_alpha = max(0, min(255, int(alpha * finish['liner_alpha'])))
        dl.polygon(liner_outer, fill=liner_color + (liner_alpha,))
        base = over(base, liner)

    d.polygon(base_outer, fill=color + (base_alpha,))
    base = base.filter(ImageFilter.GaussianBlur(ed * finish['blur']))

    # center / finish
    center = layer()
    dc = ImageDraw.Draw(center)
    lower = lerp(P(17), P(14), 0.45)
    upper = lerp(P(0), P(13), 0.5)
    mouth = lerp(P(0), P(17), 0.5)
    ckind = finish['center']
    if ckind != 'none':
        ccol = kw.get('center_color') or finish['center_color']
        calpha = max(0, min(255, int(alpha * finish['center_alpha'])))
        cs = finish['center_scale']
        if ckind in ('bitten',):
            pos = mouth
            ccol = ccol or _darken(color, 0.45)
        elif ckind == 'dark':
            pos = lower
            ccol = ccol or _darken(color, 0.55)
        elif ckind == 'blurred':
            pos = lower
            ccol = ccol or color
        else:
            pos = lower
            ccol = ccol or _lighten(color, (255, 255, 255), 0.45)
        dc.ellipse([pos[0] - ed * cs[0], pos[1] - ed * cs[1],
                    pos[0] + ed * cs[0], pos[1] + ed * cs[1]],
                   fill=_clamp(ccol) + (calpha,))
        if finish['upper']:
            dc.ellipse([upper[0] - ed * 0.05, upper[1] - ed * 0.03,
                        upper[0] + ed * 0.05, upper[1] + ed * 0.03],
                       fill=_clamp(ccol) + (int(calpha * 0.7),))
    center = center.filter(ImageFilter.GaussianBlur(ed * 0.02))
    l = over(base, center)

    # clear mouth seam
    inner_pts = [P(i) for i in LIP_INNER]
    hole = Image.new('L', (SZ, SZ), 255)
    dh = ImageDraw.Draw(hole)
    dh.line(inner_pts, fill=0, width=max(1, int(ed * 0.030)))
    hole = hole.filter(ImageFilter.GaussianBlur(ed * 0.012))
    r, g, b, a = l.split()
    a = ImageChops.multiply(a, hole)
    return over(img, Image.merge('RGBA', (r, g, b, a)))

LINER_STYLES = {
    'soft':     {'wing': 0.0,  'width': 0.030, 'alpha': 165, 'blur': 0.008, 'raise': 0.0},
    'tightline':{'wing': 0.0,  'width': 0.012, 'alpha': 140, 'blur': 0.006, 'raise': 0.015},
    'wing':     {'wing': 0.26, 'width': 0.040, 'alpha': 210, 'blur': 0.008, 'raise': 0.0},
    'siren':    {'wing': 0.34, 'width': 0.034, 'alpha': 220, 'blur': 0.008, 'raise': 0.0},
    'graphic':  {'wing': 0.28, 'width': 0.055, 'alpha': 230, 'blur': 0.008, 'raise': 0.0},
    'smudged':  {'wing': 0.26, 'width': 0.038, 'alpha': 190, 'blur': 0.018, 'raise': 0.01},
}

def el_liner(img, style='soft', color=(26, 14, 18), alpha=None, seed=0):
    st = LINER_STYLES.get(style, LINER_STYLES['soft'])
    if alpha is None:
        alpha = st['alpha']
    color = _clamp(color)
    alpha = max(0, min(255, int(alpha)))
    rng = random.Random(seed + 4)
    l = layer()
    d = ImageDraw.Draw(l)
    for lids, sign in ((LID_L, -1), (LID_R, +1)):
        ox, oy = _side_offset(rng, 0.01, 0.005)
        pts = [(P(i)[0] + ox, P(i)[1] + ed * st['raise'] + oy) for i in lids]
        w = max(1, int(ed * st['width']))
        d.line(pts, fill=color + (alpha,), width=w, joint='curve')
        if st['wing'] > 0:
            o = P(lids[0])
            xoff, yoff = _side_offset(rng, 0.01, 0.005)
            tip = (o[0] + sign * ed * st['wing'] + xoff,
                   o[1] - ed * st['wing'] * 0.55 + yoff)
            # Taper the wing: full width at the outer corner, narrowing to
            # zero at the tip with a proportional alpha fade so it feathers
            # instead of ending as a constant-ink spike.
            w_tip = max(1, int(ed * st['width']))
            steps = 8
            for si in range(steps):
                t0 = si / steps
                t1 = (si + 1) / steps
                p0 = (o[0] + (tip[0] - o[0]) * t0, o[1] + (tip[1] - o[1]) * t0)
                p1 = (o[0] + (tip[0] - o[0]) * t1, o[1] + (tip[1] - o[1]) * t1)
                w_seg = max(1, int(w_tip * (1.0 - t0) * 0.9))
                a_seg = int(alpha * (1.0 - t0 * 0.7))
                draw_tapered_line(d, p0, p1, w_seg, max(1, int(w_seg * 0.5)),
                                  color + (a_seg,))
    return over(img, l.filter(ImageFilter.GaussianBlur(ed * st['blur'])))

def el_lashes(img, strength=0.7, style='doll', lower=False, color=(20, 12, 16), seed=0):
    rng = random.Random(seed + 5)
    l = layer()
    d = ImageDraw.Draw(l)
    upper_sets = [(LID_L, -1), (LID_R, +1)]
    lower_sets = [(LOW_L, -1), (LOW_R, +1)]
    sets = lower_sets if lower else upper_sets
    base_ang = math.pi / 2.0 if lower else -math.pi / 2.0

    if style == 'doll':
        n = int(8 + 8 * strength)
        per_cluster = 2
        ang = 0.25
        len_t = lambda t: 1.0 - 0.30 * t
    elif style == 'cat':
        n = int(7 + 6 * strength)
        per_cluster = 2
        ang = 0.40
        len_t = lambda t: (1.0 - t) * (1.0 + 0.5 * (1.0 - t))
    elif style == 'wispy':
        n = int(6 + 7 * strength)
        per_cluster = 2
        ang = 0.20
        len_t = lambda t: rng.uniform(0.55, 1.15)
    elif style == 'stage':
        n = int(12 + 10 * strength)
        per_cluster = 3
        ang = 0.30
        len_t = lambda t: 1.0 - 0.15 * t
    else:
        n = int(8 + 6 * strength)
        per_cluster = 2
        ang = 0.25
        len_t = lambda t: 1.0 - 0.25 * t

    if lower:
        n = max(3, int(n * 0.55))
        per_cluster = min(per_cluster, 2)

    alpha = 180 if lower else 200
    for lidpts, sign in sets:
        ox, oy = _side_offset(rng, 0.012, 0.006)
        pts = [(P(i)[0] + ox, P(i)[1] + oy) for i in lidpts]
        for k in range(n):
            t = (k + rng.uniform(-0.2, 0.2)) / max(1, n - 1)
            t = max(0.02, min(0.98, t))
            for _ in range(per_cluster):
                tt = t + rng.uniform(-0.03, 0.03)
                tt = max(0.02, min(0.98, tt))
                base = point_on_poly(pts, tt)
                lt = len_t(t)
                ln = ed * (0.05 + 0.06 * strength) * lt * (0.5 if lower else 1.0)
                s = -sign if lower else sign
                a = base_ang + s * (ang * (1.0 - t) + rng.uniform(-0.10, 0.10))
                tip = (base[0] + math.cos(a) * ln, base[1] + math.sin(a) * ln)
                w0 = ed * rng.uniform(0.003, 0.010) * (0.7 if lower else 1.0)
                draw_tapered_line(d, base, tip, w0, 0.0, color + (alpha,))
    return over(img, l.filter(ImageFilter.GaussianBlur(ed * 0.006)))

def el_brow(img, color=(70, 48, 40), alpha=90, width=0.065, density=0.6, seed=0):
    rng = random.Random(seed + 6)
    color = _clamp(color)
    alpha = max(0, min(255, int(alpha)))
    l = layer()
    d = ImageDraw.Draw(l)
    for brows, sign in ((BROW_L, -1), (BROW_R, +1)):
        ox, oy = _side_offset(rng, 0.012, 0.006)
        pts = [(P(i)[0] + ox, P(i)[1] + oy) for i in brows]
        d.line(pts, fill=color + (alpha,), width=max(1, int(ed * width)), joint='curve')
        n = int(8 + density * 18)
        for _ in range(n):
            t = rng.uniform(0.08, 0.95)
            base = point_on_poly(pts, t)
            a = -math.pi / 2.0 + sign * rng.uniform(0.08, 0.40) + rng.uniform(-0.08, 0.08)
            ln = ed * rng.uniform(0.015, 0.045)
            tip = (base[0] + math.cos(a) * ln, base[1] + math.sin(a) * ln)
            w0 = ed * rng.uniform(0.003, 0.006)
            draw_tapered_line(d, base, tip, w0, 0.0, color + (alpha,))
    return over(img, l.filter(ImageFilter.GaussianBlur(ed * 0.030)))

def el_highlight(img, color=None, alpha=None, style='satin', seed=0):
    defaults = {'satin': ((255, 250, 240), 50), 'pearl': ((255, 245, 230), 55), 'glass': ((255, 255, 255), 60)}
    if color is None:
        color = defaults.get(style, defaults['satin'])[0]
    if alpha is None:
        alpha = defaults.get(style, defaults['satin'])[1]
    color = _clamp(color)
    alpha = max(0, min(255, int(alpha)))
    rng = random.Random(seed + 7)
    l = layer()
    # nose bridge
    l = over(l, soft_ellipse(nose_bridge, ed * 0.05, ed * 0.22, color, alpha, ed * 0.06))
    # nose tip
    l = over(l, soft_ellipse(NOSE_TIP, ed * 0.05, ed * 0.05, color, int(alpha * 0.6), ed * 0.06))
    # cheekbones
    for cheek, eye in ((P(50), eyeL_c), (P(280), eyeR_c)):
        ox, oy = _side_offset(rng, 0.015, 0.008)
        c = lerp(lerp(cheek, eye, 0.62), face_mid, -0.10)
        c = (c[0] + ox, c[1] + oy)
        l = over(l, soft_ellipse(c, ed * 0.14, ed * 0.07, color, int(alpha * 0.8), ed * 0.07))
    # cupid's bow
    cupid = lerp(P(0), nose_bridge, 0.3)
    l = over(l, soft_ellipse(cupid, ed * 0.04, ed * 0.03, color, int(alpha * 0.5), ed * 0.05))
    # chin
    l = over(l, soft_ellipse(lerp(CHIN, NOSE_TIP, 0.22), ed * 0.08, ed * 0.05, color, int(alpha * 0.6), ed * 0.06))
    if style == 'glass':
        for cheek, eye in ((P(50), eyeL_c), (P(280), eyeR_c)):
            c = lerp(lerp(cheek, eye, 0.62), face_mid, -0.10)
            l = over(l, soft_ellipse(c, ed * 0.07, ed * 0.04, color, int(alpha * 0.5), ed * 0.03))
    return over(img, l)

# ── recipe helpers ─────────────────────────────────────────────────────────

def S(lid, crease=None, outer=None, inner=None, shimmer=None, lower=None):
    p = {'lid': lid}
    if crease is not None: p['crease'] = crease
    if outer is not None: p['outer'] = outer
    if inner is not None: p['inner'] = inner
    if shimmer is not None: p['shimmer'] = shimmer
    if lower is not None: p['lower'] = lower
    return {'palette': p}

# ── the LOOKS table — 32 retuned + 20 new ──────────────────────────────────

LOOKS = {
    'doll_pink': [
        ('contour', dict(style='soft', alpha=30, areas=['cheek', 'jaw'])),
        ('blush', dict(style='cheeks', color=(255, 120, 160), alpha=50)),
        ('aegyo', dict(alpha=20)),
        ('shadow', S(lid=((235, 130, 160), 55), crease=((220, 100, 135), 45), outer=((200, 80, 120), 40), inner=((255, 240, 245), 45), shimmer=((255, 245, 250), 30))),
        ('liner', dict(style='wing', alpha=200)),
        ('brow', dict(color=(75, 45, 40), alpha=95, width=0.065)),
        ('lip', dict(style='gloss', color=(235, 70, 120), alpha=95)),
        ('highlight', dict(style='pearl', color=(255, 245, 245), alpha=50)),
    ],
    'egirl': [
        ('blush', dict(style='band', color=(255, 96, 110), alpha=60)),
        ('freckles', dict(density=1.4, color=(160, 90, 70), alpha=100)),
        ('shadow', S(lid=((235, 130, 150), 45), crease=((220, 95, 125), 40), outer=((200, 70, 100), 38), inner=((255, 240, 245), 40))),
        ('liner', dict(style='wing', alpha=210)),
        ('aegyo', dict(alpha=12)),
        ('lip', dict(style='gloss', color=(220, 85, 105), alpha=90)),
        ('highlight', dict(style='satin', color=(255, 250, 245), alpha=40)),
    ],
    'glam_contour': [
        ('contour', dict(style='warm', alpha=45)),
        ('blush', dict(style='lifted', color=(225, 160, 145), alpha=42)),
        ('shadow', S(lid=((195, 145, 115), 55), crease=((165, 115, 85), 45), outer=((145, 95, 70), 40), inner=((235, 215, 195), 35), shimmer=((255, 245, 235), 25))),
        ('liner', dict(style='wing', alpha=215)),
        ('brow', dict(color=(70, 45, 38), alpha=110, width=0.070)),
        ('lip', dict(style='overline', color=(172, 110, 96), alpha=120)),
        ('highlight', dict(style='satin', color=(255, 245, 235), alpha=55)),
        ('aegyo', dict(alpha=15)),
    ],
    'coquette': [
        ('blush', dict(style='cheeks', color=(255, 140, 160), alpha=48)),
        ('shadow', S(lid=((225, 160, 175), 45), crease=((205, 135, 150), 38), inner=((255, 240, 245), 40))),
        ('liner', dict(style='soft', alpha=170)),
        ('aegyo', dict(alpha=15)),
        ('lip', dict(style='bitten', color=(210, 80, 105), alpha=85)),
        ('highlight', dict(style='satin', color=(255, 250, 245), alpha=42)),
    ],
    'goth': [
        ('contour', dict(style='cool', alpha=45)),
        ('blush', dict(style='cheeks', color=(140, 70, 90), alpha=35)),
        ('shadow', S(lid=((70, 40, 66), 95), crease=((50, 25, 45), 85), outer=((40, 18, 35), 75), inner=((160, 140, 155), 35))),
        ('liner', dict(style='graphic', alpha=240)),
        ('brow', dict(color=(60, 45, 55), alpha=90, width=0.075)),
        ('lip', dict(style='matte', color=(58, 12, 32), alpha=170)),
        ('highlight', dict(style='pearl', color=(200, 190, 200), alpha=35)),
    ],
    'peach': [
        ('blush', dict(style='cheeks', color=(255, 140, 96), alpha=55)),
        ('shadow', S(lid=((240, 160, 110), 48), crease=((220, 135, 90), 40), inner=((255, 240, 225), 40), shimmer=((255, 245, 235), 25))),
        ('liner', dict(style='soft', alpha=175)),
        ('lip', dict(style='full', color=(245, 110, 80), alpha=100)),
        ('highlight', dict(style='satin', color=(255, 240, 220), alpha=45)),
    ],
    'cold_beauty': [
        ('contour', dict(style='cool', alpha=35, areas=['cheek', 'nose'])),
        ('blush', dict(style='cheeks', color=(210, 160, 175), alpha=36)),
        ('shadow', S(lid=((150, 140, 160), 48), crease=((120, 110, 135), 40), outer=((100, 90, 115), 35), inner=((220, 215, 230), 40))),
        ('liner', dict(style='tightline', alpha=160)),
        ('brow', dict(color=(55, 45, 50), alpha=95, width=0.065)),
        ('lip', dict(style='matte', color=(150, 70, 80), alpha=105)),
        ('highlight', dict(style='pearl', color=(240, 235, 245), alpha=45)),
    ],
    'sunset': [
        ('blush', dict(style='band', color=(255, 120, 70), alpha=52)),
        ('shadow', S(lid=((235, 120, 60), 70), crease=((210, 95, 45), 58), outer=((180, 75, 35), 50), inner=((255, 230, 200), 45), shimmer=((255, 245, 220), 35))),
        ('liner', dict(style='wing', alpha=200)),
        ('lip', dict(style='full', color=(210, 80, 55), alpha=105)),
        ('highlight', dict(style='satin', color=(255, 220, 170), alpha=52)),
    ],
    'angel': [
        ('blush', dict(style='cheeks', color=(255, 170, 185), alpha=40)),
        ('aegyo', dict(alpha=20)),
        ('shadow', S(lid=((240, 210, 220), 42), crease=((225, 185, 195), 35), inner=((255, 245, 248), 45), shimmer=((255, 255, 255), 35))),
        ('liner', dict(style='soft', alpha=165)),
        ('brow', dict(color=(70, 50, 50), alpha=80, width=0.060)),
        ('lip', dict(style='gloss', color=(230, 130, 145), alpha=70)),
        ('highlight', dict(style='satin', color=(255, 250, 245), alpha=64)),
    ],
    'baddie': [
        ('contour', dict(style='warm', alpha=45)),
        ('blush', dict(style='lifted', color=(235, 120, 100), alpha=48)),
        ('shadow', S(lid=((200, 150, 90), 58), crease=((175, 125, 70), 48), outer=((150, 100, 55), 42), inner=((255, 235, 215), 35), shimmer=((255, 240, 215), 25))),
        ('liner', dict(style='siren', alpha=225)),
        ('brow', dict(color=(60, 40, 35), alpha=120, width=0.075)),
        ('lip', dict(style='overline', color=(170, 100, 85), alpha=125)),
        ('highlight', dict(style='satin', color=(255, 245, 235), alpha=50)),
    ],
    'cyber_chrome': [
        ('blush', dict(style='lifted', color=(200, 220, 235), alpha=30)),
        ('shadow', S(lid=((80, 220, 235), 80), crease=((50, 190, 210), 70), outer=((35, 160, 185), 60), inner=((220, 245, 255), 45), shimmer=((200, 245, 255), 50))),
        ('liner', dict(style='graphic', alpha=220)),
        ('brow', dict(color=(60, 60, 70), alpha=80, width=0.070)),
        ('lip', dict(style='matte', color=(60, 70, 90), alpha=110)),
        ('highlight', dict(style='glass', color=(220, 245, 255), alpha=70)),
    ],
    'hearts_freckles': [
        ('blush', dict(style='cheeks', color=(255, 105, 130), alpha=60)),
        ('freckles', dict(density=1.0, color=(170, 90, 80), alpha=95)),
        ('shadow', S(lid=((225, 140, 155), 45), crease=((205, 115, 130), 38), inner=((255, 240, 245), 40))),
        ('liner', dict(style='wing', alpha=200)),
        ('aegyo', dict(alpha=14)),
        ('lip', dict(style='bitten', color=(225, 75, 95), alpha=92)),
        ('highlight', dict(style='satin', color=(255, 250, 245), alpha=45)),
    ],
    'clean_girl': [
        ('blush', dict(style='lifted', color=(255, 170, 165), alpha=38)),
        ('brow', dict(color=(75, 55, 50), alpha=85, width=0.055, density=0.5)),
        ('shadow', S(lid=((210, 190, 180), 35))),
        ('liner', dict(style='tightline', alpha=150)),
        ('lip', dict(style='balm', color=(225, 140, 135), alpha=70)),
        ('highlight', dict(style='satin', color=(255, 250, 245), alpha=45)),
    ],
    'soft_glam_nude': [
        ('contour', dict(style='soft', alpha=40)),
        ('blush', dict(style='lifted', color=(215, 165, 150), alpha=42)),
        ('shadow', S(lid=((185, 155, 135), 55), crease=((160, 130, 110), 45), outer=((140, 110, 90), 38), inner=((230, 215, 200), 35), shimmer=((255, 245, 235), 25))),
        ('liner', dict(style='soft', alpha=180)),
        ('brow', dict(color=(75, 55, 50), alpha=100, width=0.065)),
        ('lip', dict(style='gloss', color=(190, 120, 105), alpha=90)),
        ('highlight', dict(style='satin', color=(255, 245, 235), alpha=50)),
    ],
    'bronze_sculpt': [
        ('contour', dict(style='warm', alpha=50)),
        ('blush', dict(style='sunkissed', color=(225, 140, 100), alpha=46)),
        ('shadow', S(lid=((200, 130, 90), 58), crease=((170, 105, 70), 48), outer=((145, 85, 55), 42), inner=((255, 230, 200), 35), shimmer=((255, 235, 200), 30))),
        ('liner', dict(style='wing', alpha=200)),
        ('brow', dict(color=(70, 50, 40), alpha=105, width=0.070)),
        ('lip', dict(style='satin', color=(180, 115, 95), alpha=100)),
        ('highlight', dict(style='satin', color=(255, 235, 205), alpha=55)),
    ],
    'latte': [
        ('contour', dict(style='warm', alpha=45, areas=['cheek', 'jaw'])),
        ('blush', dict(style='apple', color=(170, 125, 105), alpha=45)),
        ('shadow', S(lid=((160, 120, 100), 55), crease=((135, 95, 75), 45), outer=((110, 75, 55), 38), inner=((220, 200, 185), 35))),
        ('liner', dict(style='soft', alpha=170)),
        ('brow', dict(color=(75, 55, 50), alpha=95, width=0.065)),
        ('lip', dict(style='velvet', color=(160, 110, 95), alpha=110)),
        ('highlight', dict(style='satin', color=(255, 240, 225), alpha=45)),
    ],
    'rosewood': [
        ('contour', dict(style='soft', alpha=40)),
        ('blush', dict(style='lifted', color=(215, 130, 135), alpha=45)),
        ('shadow', S(lid=((190, 130, 135), 55), crease=((165, 105, 110), 45), outer=((140, 85, 90), 38), inner=((235, 215, 215), 35), shimmer=((255, 240, 240), 25))),
        ('liner', dict(style='soft', alpha=175)),
        ('brow', dict(color=(75, 50, 50), alpha=100, width=0.065)),
        ('lip', dict(style='satin', color=(190, 100, 105), alpha=95)),
        ('highlight', dict(style='pearl', color=(255, 240, 235), alpha=50)),
    ],
    'champagne_glow': [
        ('blush', dict(style='lifted', color=(255, 210, 175), alpha=42)),
        ('shadow', S(lid=((205, 175, 150), 45), crease=((180, 150, 125), 38), inner=((255, 245, 230), 45), shimmer=((255, 255, 255), 40))),
        ('liner', dict(style='soft', alpha=165)),
        ('brow', dict(color=(80, 60, 55), alpha=80, width=0.060)),
        ('lip', dict(style='gloss', color=(210, 160, 140), alpha=65)),
        ('highlight', dict(style='pearl', color=(255, 245, 225), alpha=60)),
    ],
    'peach_sorbet': [
        ('blush', dict(style='apple', color=(255, 155, 120), alpha=50)),
        ('shadow', S(lid=((245, 160, 120), 48), crease=((230, 135, 95), 40), inner=((255, 240, 225), 40), shimmer=((255, 245, 230), 30))),
        ('liner', dict(style='soft', alpha=170)),
        ('lip', dict(style='blurred', color=(235, 115, 95), alpha=95)),
        ('highlight', dict(style='satin', color=(255, 240, 220), alpha=45)),
        ('brow', dict(color=(75, 55, 50), alpha=85, width=0.060)),
    ],
    'berry_bitten': [
        ('blush', dict(style='apple', color=(220, 90, 105), alpha=42)),
        ('shadow', S(lid=((165, 100, 120), 45), crease=((140, 80, 100), 38), inner=((230, 215, 220), 35))),
        ('liner', dict(style='soft', alpha=160)),
        ('lip', dict(style='bitten', color=(180, 50, 75), alpha=100)),
        ('highlight', dict(style='satin', color=(255, 245, 240), alpha=40)),
        ('brow', dict(color=(75, 50, 55), alpha=85, width=0.060)),
    ],
    'cherry_gloss': [
        ('contour', dict(style='soft', alpha=35, areas=['cheek'])),
        ('blush', dict(style='lifted', color=(235, 100, 105), alpha=48)),
        ('shadow', S(lid=((215, 90, 95), 55), crease=((190, 70, 75), 45), outer=((160, 50, 55), 40), inner=((255, 230, 230), 40), shimmer=((255, 240, 240), 30))),
        ('liner', dict(style='wing', alpha=210)),
        ('brow', dict(color=(70, 45, 45), alpha=95, width=0.065)),
        ('lip', dict(style='gloss', color=(225, 55, 70), alpha=100)),
        ('highlight', dict(style='satin', color=(255, 240, 235), alpha=50)),
    ],
    'terracotta_smoke': [
        ('contour', dict(style='warm', alpha=45)),
        ('blush', dict(style='sunkissed', color=(235, 130, 90), alpha=48)),
        ('shadow', S(lid=((205, 95, 60), 68), crease=((175, 75, 45), 58), outer=((145, 55, 30), 50), inner=((255, 225, 205), 40), shimmer=((255, 235, 210), 30))),
        ('liner', dict(style='smudged', alpha=200)),
        ('lip', dict(style='satin', color=(205, 110, 85), alpha=100)),
        ('highlight', dict(style='satin', color=(255, 230, 195), alpha=50)),
        ('brow', dict(color=(70, 50, 40), alpha=100, width=0.070)),
    ],
    'emerald_smoke': [
        ('contour', dict(style='soft', alpha=35, areas=['cheek', 'nose'])),
        ('blush', dict(style='lifted', color=(190, 140, 140), alpha=36)),
        ('shadow', S(lid=((45, 145, 110), 65), crease=((30, 115, 85), 55), outer=((20, 90, 65), 48), inner=((200, 235, 215), 40), shimmer=((210, 255, 235), 35))),
        ('liner', dict(style='wing', alpha=200)),
        ('brow', dict(color=(55, 65, 60), alpha=85, width=0.065)),
        ('lip', dict(style='gloss', color=(165, 125, 115), alpha=80)),
        ('highlight', dict(style='pearl', color=(225, 255, 240), alpha=50)),
    ],
    'sapphire_night': [
        ('contour', dict(style='cool', alpha=40, areas=['cheek', 'jaw'])),
        ('blush', dict(style='lifted', color=(175, 130, 160), alpha=40)),
        ('shadow', S(lid=((60, 110, 165), 68), crease=((40, 85, 140), 58), outer=((25, 60, 115), 50), inner=((210, 225, 245), 40), shimmer=((220, 240, 255), 35))),
        ('liner', dict(style='wing', alpha=210)),
        ('brow', dict(color=(55, 55, 70), alpha=90, width=0.065)),
        ('lip', dict(style='gloss', color=(175, 125, 120), alpha=85)),
        ('highlight', dict(style='pearl', color=(230, 240, 255), alpha=55)),
    ],
    'plum_velvet': [
        ('contour', dict(style='cool', alpha=45)),
        ('blush', dict(style='lifted', color=(180, 105, 130), alpha=42)),
        ('shadow', S(lid=((130, 70, 100), 62), crease=((100, 50, 80), 52), outer=((80, 35, 65), 45), inner=((225, 205, 220), 35), shimmer=((240, 220, 235), 25))),
        ('liner', dict(style='smudged', alpha=200)),
        ('brow', dict(color=(60, 45, 55), alpha=95, width=0.070)),
        ('lip', dict(style='velvet', color=(140, 60, 90), alpha=120)),
        ('highlight', dict(style='pearl', color=(235, 220, 230), alpha=45)),
    ],
    'mocha_siren': [
        ('contour', dict(style='warm', alpha=45)),
        ('blush', dict(style='sunkissed', color=(210, 135, 105), alpha=45)),
        ('shadow', S(lid=((150, 110, 85), 58), crease=((125, 85, 60), 48), outer=((100, 60, 40), 42), inner=((235, 215, 195), 35), shimmer=((255, 240, 225), 25))),
        ('liner', dict(style='siren', alpha=220)),
        ('brow', dict(color=(70, 50, 42), alpha=105, width=0.070)),
        ('lip', dict(style='lined_gloss', color=(165, 95, 80), alpha=100)),
        ('highlight', dict(style='satin', color=(255, 240, 225), alpha=50)),
    ],
    'romantic_rose': [
        ('contour', dict(style='soft', alpha=35, areas=['cheek'])),
        ('blush', dict(style='apple', color=(235, 150, 165), alpha=46)),
        ('shadow', S(lid=((225, 155, 165), 50), crease=((200, 125, 135), 42), outer=((175, 100, 110), 36), inner=((255, 235, 240), 45), shimmer=((255, 245, 250), 35))),
        ('liner', dict(style='soft', alpha=170)),
        ('brow', dict(color=(75, 50, 55), alpha=90, width=0.065)),
        ('lip', dict(style='gloss', color=(215, 100, 120), alpha=85)),
        ('highlight', dict(style='pearl', color=(255, 240, 245), alpha=55)),
    ],
    'ballet_pink': [
        ('blush', dict(style='cheeks', color=(255, 170, 190), alpha=44)),
        ('shadow', S(lid=((245, 185, 200), 42), crease=((230, 160, 175), 35), inner=((255, 245, 250), 45), shimmer=((255, 255, 255), 35))),
        ('liner', dict(style='soft', alpha=165)),
        ('brow', dict(color=(80, 60, 65), alpha=80, width=0.060)),
        ('lip', dict(style='satin', color=(235, 130, 150), alpha=85)),
        ('highlight', dict(style='pearl', color=(255, 245, 250), alpha=55)),
    ],
    'sunkissed_freckles': [
        ('contour', dict(style='warm', alpha=40, areas=['cheek', 'jaw'])),
        ('blush', dict(style='sunkissed', color=(255, 150, 110), alpha=48)),
        ('freckles', dict(density=0.6, color=(180, 110, 75), alpha=90)),
        ('shadow', S(lid=((205, 145, 110), 45), crease=((185, 120, 85), 38), inner=((255, 235, 215), 40))),
        ('liner', dict(style='soft', alpha=160)),
        ('brow', dict(color=(75, 55, 45), alpha=85, width=0.060)),
        ('lip', dict(style='balm', color=(215, 130, 110), alpha=70)),
        ('highlight', dict(style='satin', color=(255, 235, 205), alpha=50)),
    ],
    'grunge_smoke': [
        ('contour', dict(style='cool', alpha=40)),
        ('blush', dict(style='band', color=(175, 95, 90), alpha=40)),
        ('shadow', S(lid=((90, 75, 75), 70), crease=((70, 55, 55), 58), outer=((55, 40, 40), 50), inner=((200, 190, 190), 30), lower=((80, 70, 70), 35))),
        ('liner', dict(style='smudged', alpha=210)),
        ('brow', dict(color=(60, 50, 50), alpha=90, width=0.075)),
        ('lip', dict(style='blurred_matte', color=(150, 75, 75), alpha=110)),
        ('highlight', dict(style='satin', color=(220, 215, 215), alpha=35)),
    ],
    'midnight_goth': [
        ('contour', dict(style='cool', alpha=50)),
        ('blush', dict(style='cheeks', color=(130, 65, 80), alpha=35)),
        ('shadow', S(lid=((35, 25, 40), 100), crease=((25, 15, 28), 90), outer=((15, 8, 18), 80), inner=((140, 120, 145), 30), shimmer=((180, 160, 175), 25))),
        ('liner', dict(style='graphic', alpha=240)),
        ('brow', dict(color=(55, 40, 50), alpha=100, width=0.080)),
        ('lip', dict(style='satin', color=(75, 15, 35), alpha=140)),
        ('highlight', dict(style='pearl', color=(200, 185, 195), alpha=40)),
    ],
    'opal_fantasy': [
        ('blush', dict(style='lifted', color=(215, 180, 210), alpha=38)),
        ('shadow', S(lid=((180, 160, 210), 48), crease=((155, 135, 190), 40), outer=((130, 110, 170), 35), inner=((230, 225, 245), 45), shimmer=((245, 235, 255), 45))),
        ('liner', dict(style='soft', alpha=165)),
        ('brow', dict(color=(75, 60, 80), alpha=80, width=0.060)),
        ('lip', dict(style='iridescent', color=(195, 150, 180), alpha=80)),
        ('highlight', dict(style='pearl', color=(245, 235, 255), alpha=60)),
    ],
    'teal_wing': [
    ('contour', dict(style='warm', alpha=40)),
    ('blush', dict(style='cheeks', color=(255, 140, 100), alpha=50)),
    ('shadow', S(lid=((200, 120, 80), 60), crease=((170, 95, 60), 50), outer=((145, 75, 45), 42), inner=((255, 230, 200), 40), lower=((40, 180, 200), 55))),
    ('liner', dict(style='wing', alpha=220)),
    ('brow', dict(color=(70, 50, 40), alpha=100, width=0.070)),
    ('lip', dict(style='gloss', color=(200, 120, 80), alpha=95)),
    ('highlight', dict(style='satin', color=(255, 235, 205), alpha=50)),
    ],
    'bronze_glam': [
    ('contour', dict(style='warm', alpha=50)),
    ('blush', dict(style='lifted', color=(225, 160, 145), alpha=48)),
    ('shadow', S(lid=((185, 125, 90), 60), crease=((155, 100, 70), 52), outer=((125, 75, 50), 45), inner=((255, 230, 210), 40), shimmer=((255, 240, 220), 30))),
    ('liner', dict(style='wing', alpha=215)),
    ('brow', dict(color=(75, 50, 42), alpha=105, width=0.070)),
    ('lip', dict(style='satin', color=(170, 110, 90), alpha=100)),
    ('highlight', dict(style='satin', color=(255, 235, 215), alpha=55)),
    ],
    'gamer_girl': [
    ('blush', dict(style='band', color=(255, 100, 110), alpha=55)),
    ('freckles', dict(density=1.2, color=(170, 95, 75), alpha=100)),
    ('shadow', S(lid=((235, 205, 180), 48), crease=((210, 175, 150), 42), outer=((185, 150, 125), 36), inner=((255, 250, 245), 45), shimmer=((255, 255, 255), 35))),
    ('aegyo', dict(alpha=14)),
    ('liner', dict(style='wing', alpha=210)),
    ('brow', dict(color=(75, 55, 50), alpha=90, width=0.065)),
    ('lip', dict(style='gloss', color=(220, 100, 120), alpha=100)),
    ('highlight', dict(style='satin', color=(255, 250, 245), alpha=50)),
    ],
    'copper_smoke': [
    ('contour', dict(style='warm', alpha=40)),
    ('blush', dict(style='sunkissed', color=(235, 140, 100), alpha=50)),
    ('shadow', S(lid=((205, 130, 95), 60), crease=((175, 105, 70), 50), outer=((145, 80, 50), 42), inner=((255, 230, 205), 40), shimmer=((255, 240, 220), 30))),
    ('liner', dict(style='wing', alpha=200)),
    ('brow', dict(color=(75, 50, 40), alpha=100, width=0.070)),
    ('lip', dict(style='gloss', color=(200, 130, 100), alpha=95)),
    ('highlight', dict(style='satin', color=(255, 235, 210), alpha=50)),
    ],
    'coral_flush': [
    ('contour', dict(style='soft', alpha=35, areas=['cheek'])),
    ('blush', dict(style='apple', color=(255, 140, 110), alpha=50)),
    ('shadow', S(lid=((220, 170, 140), 45), crease=((185, 140, 110), 38), outer=((155, 115, 85), 32), inner=((255, 240, 230), 40))),
    ('liner', dict(style='soft', alpha=180)),
    ('brow', dict(color=(80, 60, 55), alpha=80, width=0.060)),
    ('lip', dict(style='blurred', color=(220, 90, 80), alpha=100)),
    ('highlight', dict(style='satin', color=(255, 245, 235), alpha=50)),
    ],
    'champagne_wing': [
    ('blush', dict(style='cheeks', color=(255, 150, 160), alpha=48)),
    ('shadow', S(lid=((230, 200, 185), 50), crease=((205, 175, 155), 42), outer=((180, 150, 130), 36), inner=((255, 250, 245), 45), shimmer=((255, 255, 255), 45))),
    ('liner', dict(style='wing', alpha=210)),
    ('brow', dict(color=(80, 60, 55), alpha=80, width=0.060)),
    ('lip', dict(style='gloss', color=(210, 140, 150), alpha=95)),
    ('highlight', dict(style='pearl', color=(255, 245, 235), alpha=55)),
    ],
    'fairy_doll': [
    ('blush', dict(style='apple', color=(255, 160, 160), alpha=50)),
    ('shadow', S(lid=((250, 175, 180), 45), crease=((230, 150, 160), 38), outer=((210, 125, 135), 34), inner=((255, 245, 250), 48), shimmer=((255, 255, 255), 40))),
    ('aegyo', dict(alpha=16)),
    ('liner', dict(style='soft', alpha=175)),
    ('brow', dict(color=(80, 60, 65), alpha=80, width=0.060)),
    ('lip', dict(style='gloss', color=(230, 130, 140), alpha=95)),
    ('highlight', dict(style='pearl', color=(255, 245, 250), alpha=55)),
    ],
    'warm_sculpt': [
    ('contour', dict(style='warm', alpha=50)),
    ('blush', dict(style='lifted', color=(225, 150, 130), alpha=48)),
    ('shadow', S(lid=((190, 130, 95), 60), crease=((160, 100, 70), 52), outer=((135, 80, 55), 45), inner=((255, 230, 205), 40), shimmer=((255, 240, 215), 30))),
    ('liner', dict(style='wing', alpha=200)),
    ('brow', dict(color=(75, 50, 42), alpha=110, width=0.075)),
    ('lip', dict(style='satin', color=(180, 120, 100), alpha=100)),
    ('highlight', dict(style='satin', color=(255, 235, 210), alpha=55)),
    ],
    'pink_freckle': [
    ('blush', dict(style='band', color=(255, 105, 115), alpha=55)),
    ('freckles', dict(density=1.0, color=(170, 95, 80), alpha=95)),
    ('shadow', S(lid=((235, 165, 175), 45), crease=((215, 135, 150), 38), outer=((195, 110, 125), 34), inner=((255, 245, 250), 40))),
    ('liner', dict(style='wing', alpha=210)),
    ('brow', dict(color=(75, 55, 50), alpha=90, width=0.065)),
    ('lip', dict(style='gloss', color=(215, 90, 110), alpha=100)),
    ('highlight', dict(style='satin', color=(255, 245, 245), alpha=50)),
    ],
    'soft_korean': [
    ('contour', dict(style='soft', alpha=35, areas=['cheek'])),
    ('blush', dict(style='apple', color=(255, 145, 120), alpha=50)),
    ('shadow', S(lid=((220, 180, 155), 42), crease=((190, 150, 125), 36), outer=((160, 120, 95), 30), inner=((255, 245, 240), 45))),
    ('liner', dict(style='soft', alpha=175)),
    ('brow', dict(color=(80, 60, 55), alpha=80, width=0.060)),
    ('lip', dict(style='blurred', color=(210, 80, 70), alpha=95)),
    ('highlight', dict(style='satin', color=(255, 245, 235), alpha=50)),
    ],
    'glitter_wing': [
    ('blush', dict(style='cheeks', color=(255, 150, 160), alpha=48)),
    ('shadow', S(lid=((225, 195, 180), 50), crease=((200, 170, 150), 42), outer=((175, 145, 125), 36), inner=((255, 250, 245), 50), shimmer=((255, 255, 255), 60))),
    ('liner', dict(style='wing', alpha=215)),
    ('brow', dict(color=(80, 60, 55), alpha=80, width=0.060)),
    ('lip', dict(style='gloss', color=(200, 130, 140), alpha=95)),
    ('highlight', dict(style='pearl', color=(255, 245, 235), alpha=60)),
    ],
    'berry_stain': [
    ('blush', dict(style='apple', color=(220, 90, 105), alpha=42)),
    ('shadow', S(lid=((185, 135, 145), 42), crease=((160, 110, 120), 35), outer=((135, 85, 95), 28), inner=((240, 225, 230), 40))),
    ('liner', dict(style='soft', alpha=170)),
    ('brow', dict(color=(75, 50, 55), alpha=80, width=0.060)),
    ('lip', dict(style='bitten', color=(170, 50, 70), alpha=110)),
    ('highlight', dict(style='satin', color=(255, 240, 245), alpha=45)),
    ],
    'sunburn_girl': [
    ('blush', dict(style='sunkissed', color=(255, 110, 120), alpha=55)),
    ('freckles', dict(density=0.8, color=(180, 110, 80), alpha=90)),
    ('shadow', S(lid=((220, 160, 140), 45), crease=((200, 135, 115), 38), outer=((175, 110, 90), 32), inner=((255, 240, 235), 40))),
    ('liner', dict(style='wing', alpha=210)),
    ('brow', dict(color=(75, 55, 50), alpha=90, width=0.065)),
    ('lip', dict(style='gloss', color=(220, 100, 110), alpha=100)),
    ('highlight', dict(style='satin', color=(255, 240, 230), alpha=50)),
    ],
    'mocha_wing': [
    ('contour', dict(style='warm', alpha=45)),
    ('blush', dict(style='sunkissed', color=(210, 130, 100), alpha=46)),
    ('shadow', S(lid=((165, 120, 100), 55), crease=((140, 95, 75), 48), outer=((115, 75, 55), 40), inner=((235, 220, 205), 40), shimmer=((255, 240, 225), 30))),
    ('liner', dict(style='siren', alpha=220)),
    ('brow', dict(color=(75, 50, 42), alpha=105, width=0.070)),
    ('lip', dict(style='lined_gloss', color=(165, 100, 85), alpha=100)),
    ('highlight', dict(style='satin', color=(255, 235, 215), alpha=55)),
    ],
    'rose_liner': [
    ('contour', dict(style='soft', alpha=40, areas=['cheek'])),
    ('blush', dict(style='lifted', color=(220, 130, 140), alpha=45)),
    ('shadow', S(lid=((225, 160, 170), 50), crease=((200, 130, 145), 42), outer=((175, 105, 120), 36), inner=((255, 240, 245), 45), shimmer=((255, 245, 250), 35))),
    ('liner', dict(style='soft', alpha=180)),
    ('brow', dict(color=(80, 55, 60), alpha=90, width=0.065)),
    ('lip', dict(style='satin', color=(200, 100, 110), alpha=95)),
    ('highlight', dict(style='pearl', color=(255, 240, 245), alpha=55)),
    ],
    'amber_smoke': [
    ('contour', dict(style='warm', alpha=45)),
    ('blush', dict(style='sunkissed', color=(230, 140, 95), alpha=50)),
    ('shadow', S(lid=((225, 150, 95), 62), crease=((195, 120, 75), 52), outer=((165, 95, 55), 45), inner=((255, 235, 205), 40), shimmer=((255, 245, 210), 40))),
    ('liner', dict(style='wing', alpha=200)),
    ('brow', dict(color=(75, 50, 40), alpha=100, width=0.070)),
    ('lip', dict(style='satin', color=(190, 125, 95), alpha=100)),
    ('highlight', dict(style='satin', color=(255, 235, 205), alpha=55)),
    ],
    'punk_wing': [
    ('contour', dict(style='cool', alpha=45)),
    ('blush', dict(style='cheeks', color=(150, 70, 80), alpha=40)),
    ('shadow', S(lid=((120, 70, 80), 55), crease=((90, 50, 60), 48), outer=((65, 35, 45), 42), inner=((220, 200, 205), 35))),
    ('liner', dict(style='graphic', alpha=250)),
    ('brow', dict(color=(60, 45, 50), alpha=95, width=0.075)),
    ('lip', dict(style='matte', color=(80, 20, 35), alpha=150)),
    ('highlight', dict(style='pearl', color=(220, 210, 215), alpha=45)),
    ],
    'peach_brim': [
    ('contour', dict(style='soft', alpha=35, areas=['cheek'])),
    ('blush', dict(style='lifted', color=(255, 160, 120), alpha=52)),
    ('shadow', S(lid=((245, 175, 140), 48), crease=((225, 150, 115), 40), outer=((205, 125, 90), 34), inner=((255, 245, 235), 45))),
    ('liner', dict(style='soft', alpha=175)),
    ('brow', dict(color=(80, 60, 55), alpha=80, width=0.060)),
    ('lip', dict(style='gloss', color=(225, 120, 100), alpha=95)),
    ('highlight', dict(style='satin', color=(255, 240, 225), alpha=55)),
    ],
    'honey_freckle': [
    ('blush', dict(style='sunkissed', color=(240, 160, 110), alpha=50)),
    ('freckles', dict(density=0.7, color=(180, 115, 75), alpha=90)),
    ('shadow', S(lid=((225, 175, 125), 48), crease=((205, 150, 100), 42), outer=((185, 125, 75), 36), inner=((255, 245, 230), 45))),
    ('liner', dict(style='soft', alpha=175)),
    ('brow', dict(color=(75, 55, 45), alpha=85, width=0.060)),
    ('lip', dict(style='gloss', color=(200, 140, 100), alpha=95)),
    ('highlight', dict(style='satin', color=(255, 240, 215), alpha=50)),
    ],
    'vivid_glam': [
    ('contour', dict(style='warm', alpha=45)),
    ('blush', dict(style='lifted', color=(245, 120, 120), alpha=55)),
    ('shadow', S(lid=((235, 140, 150), 52), crease=((215, 110, 120), 44), outer=((195, 85, 95), 38), inner=((255, 240, 245), 45), shimmer=((255, 245, 250), 40))),
    ('liner', dict(style='wing', alpha=225)),
    ('brow', dict(color=(75, 45, 50), alpha=100, width=0.070)),
    ('lip', dict(style='gloss', color=(215, 80, 100), alpha=100)),
    ('highlight', dict(style='satin', color=(255, 240, 240), alpha=50)),
    ],

    'anime_doll': [
        ('contour', dict(style='soft', alpha=30, areas=['cheek', 'jaw'])),
        ('blush', dict(style='cheeks', color=(255, 130, 160), alpha=55)),
        ('aegyo', dict(alpha=22)),
        ('shadow', S(lid=((80, 200, 220), 60), crease=((50, 170, 200), 50), outer=((30, 140, 180), 42), inner=((220, 250, 255), 45), shimmer=((200, 245, 255), 40))),
        ('liner', dict(style='graphic', alpha=230)),
        ('lashes', dict(strength=1.0, style='doll')),
        ('brow', dict(color=(70, 45, 40), alpha=85, width=0.050)),
        ('lip', dict(style='gloss', color=(240, 130, 150), alpha=90)),
        ('highlight', dict(style='glass', color=(220, 250, 255), alpha=60)),
    ],
    'kawaii_glitter': [
        ('blush', dict(style='cheeks', color=(255, 120, 150), alpha=58)),
        ('shadow', S(lid=((230, 160, 200), 55), crease=((210, 130, 180), 45), outer=((190, 100, 160), 38), inner=((255, 240, 250), 45), shimmer=((255, 200, 240), 55))),
        ('liner', dict(style='graphic', alpha=235)),
        ('lashes', dict(strength=1.0, style='doll')),
        ('brow', dict(color=(65, 40, 35), alpha=90, width=0.060)),
        ('lip', dict(style='gloss', color=(235, 110, 140), alpha=95)),
        ('highlight', dict(style='glass', color=(255, 220, 240), alpha=65)),
    ],
    'pastel_fairy': [
        ('contour', dict(style='soft', alpha=25)),
        ('blush', dict(style='cheeks', color=(240, 150, 180), alpha=48)),
        ('aegyo', dict(alpha=19)),
        ('shadow', S(lid=((210, 180, 230), 50), crease=((185, 155, 210), 42), outer=((160, 130, 190), 35), inner=((245, 235, 255), 45), shimmer=((255, 230, 250), 45))),
        ('liner', dict(style='wing', alpha=210)),
        ('lashes', dict(strength=0.9, style='wispy')),
        ('brow', dict(color=(50, 35, 40), alpha=110, width=0.075)),
        ('lip', dict(style='gloss', color=(220, 140, 160), alpha=80)),
        ('highlight', dict(style='pearl', color=(245, 230, 255), alpha=60)),
    ],
    'platinum_cat': [
        ('contour', dict(style='cool', alpha=35, areas=['cheek', 'nose'])),
        ('blush', dict(style='lifted', color=(210, 160, 170), alpha=38)),
        ('shadow', S(lid=((200, 190, 200), 42), crease=((175, 165, 180), 35), outer=((150, 140, 160), 30), inner=((240, 235, 245), 38))),
        ('liner', dict(style='siren', alpha=220)),
        ('lashes', dict(strength=0.85, style='cat')),
        ('brow', dict(color=(180, 160, 150), alpha=50, width=0.045, density=0.4)),
        ('lip', dict(style='satin', color=(190, 120, 130), alpha=95)),
        ('highlight', dict(style='pearl', color=(240, 235, 245), alpha=50)),
    ],
    'teal_smoke_doll': [
        ('contour', dict(style='warm', alpha=40)),
        ('blush', dict(style='lifted', color=(220, 150, 130), alpha=42)),
        ('shadow', S(lid=((40, 170, 175), 62), crease=((25, 140, 150), 52), outer=((15, 110, 125), 45), inner=((210, 240, 240), 40), lower=((30, 160, 170), 50), shimmer=((180, 240, 240), 35))),
        ('liner', dict(style='wing', alpha=215)),
        ('lashes', dict(strength=0.95, style='stage')),
        ('brow', dict(color=(60, 45, 40), alpha=105, width=0.070)),
        ('lip', dict(style='gloss', color=(200, 130, 110), alpha=90)),
        ('highlight', dict(style='satin', color=(220, 245, 245), alpha=55)),
    ],
    'rose_gold_doll': [
        ('contour', dict(style='soft', alpha=30)),
        ('blush', dict(style='cheeks', color=(255, 150, 140), alpha=48)),
        ('aegyo', dict(alpha=19)),
        ('shadow', S(lid=((235, 180, 165), 52), crease=((215, 150, 140), 42), outer=((195, 120, 115), 36), inner=((255, 240, 230), 45), shimmer=((255, 220, 200), 45))),
        ('liner', dict(style='wing', alpha=200)),
        ('lashes', dict(strength=0.9, style='doll')),
        ('brow', dict(color=(70, 50, 45), alpha=85, width=0.060)),
        ('lip', dict(style='gloss', color=(225, 130, 130), alpha=85)),
        ('highlight', dict(style='pearl', color=(255, 235, 225), alpha=58)),
    ],
    'chocolate_crease': [
        ('contour', dict(style='warm', alpha=48)),
        ('blush', dict(style='lifted', color=(210, 150, 125), alpha=40)),
        ('shadow', S(lid=((180, 130, 95), 65), crease=((150, 100, 70), 55), outer=((120, 75, 50), 48), inner=((235, 210, 190), 35), shimmer=((255, 240, 220), 30))),
        ('liner', dict(style='wing', alpha=220)),
        ('lashes', dict(strength=0.95, style='stage')),
        ('brow', dict(color=(55, 35, 30), alpha=120, width=0.075)),
        ('lip', dict(style='matte', color=(160, 110, 90), alpha=130)),
        ('highlight', dict(style='satin', color=(255, 235, 215), alpha=50)),
    ],
    'soft_doe_red': [
        ('contour', dict(style='soft', alpha=30, areas=['cheek'])),
        ('blush', dict(style='apple', color=(240, 140, 110), alpha=48)),
        ('shadow', S(lid=((225, 170, 140), 45), crease=((200, 140, 110), 38), inner=((255, 240, 225), 40))),
        ('liner', dict(style='tightline', alpha=160)),
        ('lashes', dict(strength=0.75, style='wispy')),
        ('brow', dict(color=(75, 55, 45), alpha=85, width=0.060, density=0.5)),
        ('lip', dict(style='matte', color=(200, 50, 55), alpha=140)),
        ('highlight', dict(style='satin', color=(255, 240, 225), alpha=45)),
    ],
    'bronze_cat_eye': [
        ('contour', dict(style='warm', alpha=45)),
        ('blush', dict(style='lifted', color=(225, 140, 110), alpha=45)),
        ('shadow', S(lid=((200, 140, 85), 60), crease=((170, 110, 65), 50), outer=((145, 85, 50), 42), inner=((255, 230, 200), 38), shimmer=((255, 235, 200), 35))),
        ('liner', dict(style='siren', alpha=230)),
        ('lashes', dict(strength=1.0, style='cat')),
        ('brow', dict(color=(55, 38, 32), alpha=115, width=0.075)),
        ('lip', dict(style='gloss', color=(210, 130, 120), alpha=90)),
        ('highlight', dict(style='satin', color=(255, 235, 205), alpha=55)),
    ],
    'kawaii_blush': [
        ('blush', dict(style='band', color=(255, 110, 130), alpha=70)),
        ('aegyo', dict(alpha=21)),
        ('shadow', S(lid=((230, 160, 170), 42), crease=((210, 130, 145), 35), inner=((255, 240, 245), 40))),
        ('liner', dict(style='soft', alpha=170)),
        ('lashes', dict(strength=0.85, style='doll')),
        ('brow', dict(color=(65, 45, 40), alpha=90, width=0.065)),
        ('lip', dict(style='gloss', color=(225, 100, 120), alpha=90)),
        ('highlight', dict(style='glass', color=(255, 240, 245), alpha=60)),
    ],
    'peach_egirl_freckle': [
        ('blush', dict(style='band', color=(255, 140, 110), alpha=62)),
        ('freckles', dict(density=1.2, color=(170, 100, 75), alpha=95)),
        ('shadow', S(lid=((235, 160, 120), 48), crease=((215, 130, 90), 40), outer=((195, 105, 70), 34), inner=((255, 240, 225), 42))),
        ('liner', dict(style='wing', alpha=210)),
        ('lashes', dict(strength=0.8, style='wispy')),
        ('brow', dict(color=(70, 50, 45), alpha=90, width=0.060)),
        ('lip', dict(style='balm', color=(225, 120, 100), alpha=75)),
        ('highlight', dict(style='satin', color=(255, 240, 220), alpha=48)),
    ],
    'soft_amber_doll': [
        ('contour', dict(style='soft', alpha=28)),
        ('blush', dict(style='cheeks', color=(250, 160, 130), alpha=48)),
        ('aegyo', dict(alpha=19)),
        ('shadow', S(lid=((235, 175, 130), 48), crease=((215, 145, 100), 40), inner=((255, 240, 220), 42), shimmer=((255, 235, 200), 35))),
        ('liner', dict(style='soft', alpha=170)),
        ('lashes', dict(strength=0.85, style='doll')),
        ('brow', dict(color=(75, 55, 45), alpha=80, width=0.058)),
        ('lip', dict(style='gloss', color=(230, 135, 130), alpha=85)),
        ('highlight', dict(style='satin', color=(255, 240, 220), alpha=52)),
    ],
    'gamer_belle': [
        ('blush', dict(style='band', color=(255, 105, 125), alpha=65)),
        ('freckles', dict(density=0.8, color=(165, 90, 75), alpha=85)),
        ('shadow', S(lid=((230, 150, 170), 48), crease=((210, 120, 145), 40), outer=((190, 95, 120), 34), inner=((255, 240, 245), 42), shimmer=((255, 220, 235), 40))),
        ('liner', dict(style='wing', alpha=220)),
        ('lashes', dict(strength=1.0, style='doll')),
        ('brow', dict(color=(65, 45, 40), alpha=95, width=0.065)),
        ('lip', dict(style='gloss', color=(220, 90, 115), alpha=95)),
        ('highlight', dict(style='glass', color=(255, 230, 240), alpha=58)),
    ],
    'glitter_cat_pink': [
        ('blush', dict(style='cheeks', color=(255, 125, 155), alpha=52)),
        ('shadow', S(lid=((235, 155, 190), 55), crease=((215, 125, 170), 45), outer=((195, 100, 150), 38), inner=((255, 235, 250), 45), shimmer=((255, 200, 235), 60))),
        ('liner', dict(style='graphic', alpha=225)),
        ('lashes', dict(strength=0.95, style='cat')),
        ('brow', dict(color=(60, 40, 35), alpha=100, width=0.068)),
        ('lip', dict(style='gloss', color=(230, 115, 145), alpha=90)),
        ('highlight', dict(style='glass', color=(255, 220, 240), alpha=62)),
    ],
    'pastel_kitten': [
        ('blush', dict(style='cheeks', color=(255, 135, 155), alpha=55)),
        ('aegyo', dict(alpha=18)),
        ('shadow', S(lid=((225, 165, 185), 48), crease=((205, 135, 160), 40), outer=((185, 110, 140), 34), inner=((255, 240, 250), 42), shimmer=((245, 220, 240), 45))),
        ('liner', dict(style='wing', alpha=205)),
        ('lashes', dict(strength=0.85, style='wispy')),
        ('brow', dict(color=(70, 50, 48), alpha=85, width=0.060)),
        ('lip', dict(style='satin', color=(215, 105, 125), alpha=95)),
        ('highlight', dict(style='pearl', color=(255, 235, 245), alpha=55)),
    ],
    'crescent_boho': [
        ('contour', dict(style='soft', alpha=32)),
        ('blush', dict(style='lifted', color=(220, 155, 145), alpha=40)),
        ('shadow', S(lid=((210, 180, 170), 45), crease=((185, 155, 145), 38), outer=((165, 130, 120), 32), inner=((250, 240, 230), 42), shimmer=((255, 245, 235), 35))),
        ('liner', dict(style='soft', alpha=175)),
        ('lashes', dict(strength=0.9, style='stage')),
        ('brow', dict(color=(60, 45, 40), alpha=100, width=0.068)),
        ('lip', dict(style='satin', color=(210, 140, 130), alpha=90)),
        ('highlight', dict(style='pearl', color=(255, 240, 230), alpha=52)),
    ],
    'soft_amber_glow': [
        ('contour', dict(style='soft', alpha=30)),
        ('blush', dict(style='sunkissed', color=(245, 160, 120), alpha=48)),
        ('aegyo', dict(alpha=16)),
        ('shadow', S(lid=((235, 180, 135), 50), crease=((215, 150, 105), 42), outer=((195, 125, 80), 36), inner=((255, 240, 220), 42), shimmer=((255, 235, 200), 40))),
        ('liner', dict(style='soft', alpha=170)),
        ('lashes', dict(strength=0.8, style='wispy')),
        ('brow', dict(color=(75, 55, 45), alpha=82, width=0.058)),
        ('lip', dict(style='gloss', color=(225, 135, 125), alpha=85)),
        ('highlight', dict(style='satin', color=(255, 240, 215), alpha=55)),
    ],
    'dramatic_nude_glam': [
        ('contour', dict(style='warm', alpha=50)),
        ('blush', dict(style='lifted', color=(225, 140, 125), alpha=42)),
        ('shadow', S(lid=((210, 170, 150), 55), crease=((180, 140, 115), 48), outer=((155, 115, 90), 42), inner=((245, 225, 210), 38), shimmer=((255, 245, 235), 35))),
        ('liner', dict(style='wing', alpha=225)),
        ('lashes', dict(strength=1.0, style='stage')),
        ('brow', dict(color=(50, 35, 30), alpha=125, width=0.078)),
        ('lip', dict(style='gloss', color=(210, 150, 140), alpha=85)),
        ('highlight', dict(style='satin', color=(255, 240, 230), alpha=55)),
    ],
    'warm_bronze_teal': [
        ('contour', dict(style='warm', alpha=42)),
        ('blush', dict(style='lifted', color=(225, 145, 115), alpha=45)),
        ('shadow', S(lid=((200, 135, 80), 58), crease=((170, 105, 60), 48), outer=((145, 80, 45), 42), inner=((255, 230, 200), 38), lower=((35, 165, 175), 52), shimmer=((255, 235, 200), 35))),
        ('liner', dict(style='siren', alpha=220)),
        ('lashes', dict(strength=0.95, style='stage')),
        ('brow', dict(color=(55, 38, 32), alpha=110, width=0.072)),
        ('lip', dict(style='gloss', color=(200, 125, 105), alpha=92)),
        ('highlight', dict(style='satin', color=(255, 235, 205), alpha=52)),
    ],
    'grunge_fairy': [
        ('contour', dict(style='cool', alpha=35)),
        ('blush', dict(style='cheeks', color=(200, 120, 130), alpha=38)),
        ('freckles', dict(density=0.6, color=(150, 100, 80), alpha=80)),
        ('shadow', S(lid=((120, 90, 100), 55), crease=((95, 65, 75), 48), outer=((70, 45, 55), 42), inner=((210, 190, 195), 35))),
        ('liner', dict(style='graphic', alpha=230)),
        ('lashes', dict(strength=0.8, style='wispy')),
        ('brow', dict(color=(50, 35, 38), alpha=100, width=0.072)),
        ('lip', dict(style='blurred_matte', color=(170, 80, 85), alpha=110)),
        ('highlight', dict(style='pearl', color=(220, 205, 210), alpha=42)),
    ],
}

EL = {'blush': el_blush, 'shadow': el_shadow, 'aegyo': el_aegyo,
      'freckles': el_freckles, 'lip': el_lip, 'liner': el_liner,
      'lashes': el_lashes, 'highlight': el_highlight, 'brow': el_brow,
      'contour': el_contour}

def main():
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(PMS_IOS_OUT, exist_ok=True)
    for look_id, recipe in LOOKS.items():
        img = Image.new('RGBA', (SZ, SZ), (0, 0, 0, 0))
        for name, kw in recipe:
            kw = dict(kw)
            kw.setdefault('seed', look_seed(look_id))
            img = EL[name](img, **kw)
        path = os.path.join(OUT, f'makeup_{look_id}.png')
        img.save(path, optimize=True)
        shutil.copy2(path, PMS_IOS_OUT)
        print(f'  wrote {os.path.relpath(path, os.path.join(HERE, ".."))} ({os.path.getsize(path)//1024} KB)')
    print(f'done — {len(LOOKS)} look textures')

if __name__ == '__main__':
    main()
