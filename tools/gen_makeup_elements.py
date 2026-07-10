#!/usr/bin/env python3
"""Makeup look-texture generator — the variant factory (MAKEUP_PLAN Stage 3).

Paints UV-space makeup textures (MediaPipe canonical space, exact alignment
with the tracked mesh by construction) from a parameterized LOOKS table:
each look = a recipe of elements (blush style/color, eyeshadow, aegyo-sal,
freckles, lip style/color, liner/wing, lashes, contour, highlight, brow
shade). Adding a variant = adding a spec line + rerun; no shader work.

Output: models/face/makeup_<id>.png (RGBA 1024², authored for ~0.5-luma skin;
the mesh pass's lighting adaptation handles the rest). Copy the set into
pms-ios/Engine/EngineAssets/models/face/ for the app bundle.

Deterministic (seeded per look) so regeneration is reproducible.
"""
import math, os, random
from PIL import Image, ImageChops, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT  = os.path.join(HERE, "..", "models", "face")
SZ = 1024

# ── canonical UVs (same loader as paint_makeup_douyin.py) ────────────────────
vt_raw, v2vt = [], {}
for line in open(os.path.join(HERE, "canonical_face_model.obj")):
    p = line.split()
    if not p: continue
    if p[0] == "vt": vt_raw.append((float(p[1]), float(p[2])))
    elif p[0] == "f":
        for x in p[1:4]:
            vi, ti = x.split("/")[:2]
            v2vt[int(vi) - 1] = int(ti) - 1
uv = [(vt_raw[v2vt[i]][0] * SZ, (1.0 - vt_raw[v2vt[i]][1]) * SZ) for i in range(468)]

def P(i): return uv[i]
def lerp(a, b, t): return (a[0] + (b[0]-a[0])*t, a[1] + (b[1]-a[1])*t)
def dist(a, b): return math.hypot(a[0]-b[0], a[1]-b[1])

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

def layer(): return Image.new("RGBA", (SZ, SZ), (0, 0, 0, 0))
def over(base, l): return Image.alpha_composite(base, l)

def soft_ellipse(center, rx, ry, color, alpha, blur):
    l = layer(); d = ImageDraw.Draw(l)
    d.ellipse([center[0]-rx, center[1]-ry, center[0]+rx, center[1]+ry],
              fill=tuple(color) + (alpha,))
    return l.filter(ImageFilter.GaussianBlur(blur))

def lid_stroke(color, alpha, width, raise_amt, blur):
    l = layer(); d = ImageDraw.Draw(l)
    for lids in (LID_L, LID_R):
        pts = [(x, y - ed * raise_amt) for x, y in (P(i) for i in lids)]
        d.line(pts, fill=tuple(color) + (alpha,), width=int(ed * width), joint="curve")
    return l.filter(ImageFilter.GaussianBlur(ed * blur))

# ── elements ─────────────────────────────────────────────────────────────────

def el_blush(img, style, color, alpha):
    if style == "cheeks":       # rides high, douyin placement
        for cheek, eye in ((P(50), eyeL_c), (P(280), eyeR_c)):
            c = lerp(lerp(cheek, eye, 0.38), face_mid, 0.16)
            img = over(img, soft_ellipse(c, ed*0.25, ed*0.18, color, alpha, ed*0.13))
    elif style == "band":       # across nose + both cheeks, one wash
        for cheek, eye in ((P(50), eyeL_c), (P(280), eyeR_c)):
            c = lerp(lerp(cheek, eye, 0.30), face_mid, 0.12)
            img = over(img, soft_ellipse(c, ed*0.28, ed*0.16, color, alpha, ed*0.12))
        img = over(img, soft_ellipse(nose_bridge, ed*0.24, ed*0.11, color,
                                     int(alpha*0.75), ed*0.10))
    elif style == "contour":    # under-cheekbone shade, not color
        for cheek, jaw in ((P(50), JAW_L), (P(280), JAW_R)):
            c = lerp(cheek, jaw, 0.42)
            img = over(img, soft_ellipse(c, ed*0.26, ed*0.12, color, alpha, ed*0.12))
    return img

def el_shadow(img, color, alpha, raise_amt=0.085, width=0.16):
    return over(img, lid_stroke(color, alpha, width, raise_amt, 0.09))

def el_aegyo(img, alpha=30):
    l = layer(); d = ImageDraw.Draw(l)
    for lows in (LOW_L, LOW_R):
        pts = [(x, y + ed*0.045) for x, y in (P(i) for i in lows)]
        d.line(pts, fill=(255, 238, 240, alpha), width=int(ed*0.075), joint="curve")
        crease = [(x, y + ed*0.10) for x, y in (P(i) for i in lows)]
        d.line(crease, fill=(150, 100, 105, int(alpha*0.6)), width=int(ed*0.028), joint="curve")
    return over(img, l.filter(ImageFilter.GaussianBlur(ed*0.035)))

def el_freckles(img, density, color=(150, 92, 66), alpha=110, seed=7):
    rng = random.Random(seed)
    l = layer(); d = ImageDraw.Draw(l)
    zones = [(nose_bridge, ed*0.55, ed*0.28)]
    for cheek, eye in ((P(50), eyeL_c), (P(280), eyeR_c)):
        zones.append((lerp(cheek, eye, 0.30), ed*0.30, ed*0.22))
    for c, rx, ry in zones:
        for _ in range(int(density * 26)):
            a = rng.uniform(0, 2*math.pi); r = rng.uniform(0, 1) ** 0.5
            x = c[0] + math.cos(a) * rx * r
            y = c[1] + math.sin(a) * ry * r
            rad = ed * rng.uniform(0.006, 0.016)
            op = int(alpha * rng.uniform(0.5, 1.0))
            d.ellipse([x-rad, y-rad, x+rad, y+rad], fill=tuple(color) + (op,))
    return over(img, l.filter(ImageFilter.GaussianBlur(ed*0.006)))

def el_lip(img, style, color, alpha):
    l = layer(); d = ImageDraw.Draw(l)
    outer = [P(i) for i in LIP_OUTER]
    if style == "overline":     # glam: slightly expanded ring
        cx = sum(x for x, _ in outer) / len(outer)
        cy = sum(y for _, y in outer) / len(outer)
        outer = [(cx + (x-cx)*1.10, cy + (y-cy)*1.14) for x, y in outer]
    d.polygon(outer, fill=tuple(color) + (alpha,))
    blur = {"matte": 0.015, "full": 0.022, "gloss": 0.022,
            "bitten": 0.022, "overline": 0.018}[style]
    l = l.filter(ImageFilter.GaussianBlur(ed * blur))
    if style == "bitten":       # dense core at the seam
        core = layer(); dc = ImageDraw.Draw(core)
        inner = [P(i) for i in LIP_INNER]
        cx = sum(x for x, _ in inner) / len(inner)
        cy = sum(y for _, y in inner) / len(inner)
        dc.ellipse([cx-ed*0.30, cy-ed*0.15, cx+ed*0.30, cy+ed*0.15],
                   fill=tuple(color) + (min(255, alpha+20),))
        l = Image.alpha_composite(l, core.filter(ImageFilter.GaussianBlur(ed*0.05)))
    if style == "gloss":        # specular streak on the lower lip
        gl = layer(); dg = ImageDraw.Draw(gl)
        low = lerp(P(17), P(14), 0.45)
        dg.ellipse([low[0]-ed*0.16, low[1]-ed*0.035, low[0]+ed*0.16, low[1]+ed*0.035],
                   fill=(255, 255, 255, 88))
        l = Image.alpha_composite(l, gl.filter(ImageFilter.GaussianBlur(ed*0.02)))
    # clear the lip seam (open-mouth smear guard — douyin painter lesson)
    inner_pts = [P(i) for i in LIP_INNER]
    hole = Image.new("L", (SZ, SZ), 255)
    dh = ImageDraw.Draw(hole)
    dh.line(inner_pts[:11], fill=0, width=int(ed*0.035))
    hole = hole.filter(ImageFilter.GaussianBlur(ed*0.012))
    r, g, b, a = l.split()
    a = ImageChops.multiply(a, hole)
    return over(img, Image.merge("RGBA", (r, g, b, a)))

def el_liner(img, style, color=(26, 14, 18), alpha=210):
    l = layer(); d = ImageDraw.Draw(l)
    wing = {"soft": 0.0, "wing": 0.28, "siren": 0.42, "graphic": 0.34}[style]
    wid  = {"soft": 0.030, "wing": 0.040, "siren": 0.034, "graphic": 0.055}[style]
    for lids, outer_i, sign in ((LID_L, 33, -1), (LID_R, 263, +1)):
        pts = [P(i) for i in lids]
        d.line(pts, fill=tuple(color) + (alpha,), width=int(ed * wid), joint="curve")
        if wing > 0:
            o = P(outer_i)
            tip = (o[0] + sign * ed * wing, o[1] - ed * wing * 0.55)
            d.line([o, tip], fill=tuple(color) + (alpha,), width=int(ed * wid * 0.9))
    return over(img, l.filter(ImageFilter.GaussianBlur(ed*0.008)))

def el_lashes(img, strength, color=(20, 12, 16)):
    rng = random.Random(3)
    l = layer(); d = ImageDraw.Draw(l)
    for lids, sign in ((LID_L, -1), (LID_R, +1)):
        pts = [P(i) for i in lids]
        for k in range(int(8 * strength) + 4):
            t = k / max(1, int(8 * strength) + 3)
            i = min(len(pts) - 2, int(t * (len(pts) - 1)))
            base = lerp(pts[i], pts[i+1], (t * (len(pts)-1)) % 1.0)
            ln = ed * (0.05 + 0.06 * strength) * (1.25 - 0.5 * t)
            ang = -math.pi/2 + sign * 0.35 * (1 - t) + rng.uniform(-0.12, 0.12)
            tip = (base[0] + math.cos(ang) * ln * (1 if sign > 0 else -1) * 0.4,
                   base[1] + math.sin(ang) * ln)
            d.line([base, tip], fill=tuple(color) + (200,), width=max(2, int(ed*0.010)))
    return over(img, l.filter(ImageFilter.GaussianBlur(ed*0.006)))

def el_highlight(img, color=(255, 250, 240), alpha=50):
    img = over(img, soft_ellipse(nose_bridge, ed*0.05, ed*0.22, color, alpha, ed*0.06))
    for cheek, eye in ((P(50), eyeL_c), (P(280), eyeR_c)):
        c = lerp(lerp(cheek, eye, 0.62), face_mid, -0.10)   # cheekbone top
        img = over(img, soft_ellipse(c, ed*0.14, ed*0.07, color, int(alpha*0.8), ed*0.07))
    img = over(img, soft_ellipse(lerp(CHIN, NOSE_TIP, 0.22), ed*0.08, ed*0.05,
                                 color, int(alpha*0.6), ed*0.06))
    return img

def el_brow(img, color=(70, 48, 40), alpha=90, width=0.075):
    l = layer(); d = ImageDraw.Draw(l)
    for brows in (BROW_L, BROW_R):
        pts = [P(i) for i in brows]
        d.line(pts, fill=tuple(color) + (alpha,), width=int(ed*width), joint="curve")
    return over(img, l.filter(ImageFilter.GaussianBlur(ed*0.030)))

# ── the LOOKS table — add a line, get a filter ───────────────────────────────
# Each entry: ordered element calls. Colors авторed for neutral ~0.5-luma skin.
LOOKS = {
    # Reference image 1: Doll Pink — huge pink energy, doll lashes, gloss.
    "doll_pink": [
        ("blush",   dict(style="band", color=(255, 110, 150), alpha=64)),
        ("shadow",  dict(color=(235, 130, 160), alpha=55)),
        ("aegyo",   dict(alpha=34)),
        ("lip",     dict(style="gloss", color=(235, 70, 120), alpha=95)),
        ("liner",   dict(style="wing")),
        ("lashes",  dict(strength=1.0)),
        ("highlight", dict(alpha=44)),
    ],
    # Reference image 2: E-Girl — nose blush, dense freckles, sharp wing.
    "egirl": [
        ("blush",   dict(style="band", color=(255, 96, 110), alpha=72)),
        ("freckles", dict(density=1.4, seed=11)),
        ("lip",     dict(style="gloss", color=(220, 95, 115), alpha=85)),
        ("liner",   dict(style="siren")),
        ("lashes",  dict(strength=0.8)),
        ("aegyo",   dict(alpha=22)),
    ],
    # Reference image 3: Glam Contour — snatched, nude overline, fluffy brow.
    "glam_contour": [
        ("blush",   dict(style="contour", color=(120, 78, 62), alpha=70)),
        ("shadow",  dict(color=(190, 140, 105), alpha=60)),
        ("brow",    dict(alpha=110, width=0.085)),
        ("lip",     dict(style="overline", color=(172, 110, 96), alpha=120)),
        ("liner",   dict(style="wing")),
        ("lashes",  dict(strength=1.0)),
        ("highlight", dict(alpha=56)),
    ],
    # Reference image 4 aesthetic lives in makeup_douyin.png (existing tool).
    "coquette": [
        ("blush",   dict(style="cheeks", color=(255, 140, 160), alpha=54)),
        ("shadow",  dict(color=(225, 160, 175), alpha=45)),
        ("aegyo",   dict(alpha=28)),
        ("lip",     dict(style="bitten", color=(210, 80, 105), alpha=85)),
        ("liner",   dict(style="soft")),
        ("lashes",  dict(strength=0.6)),
    ],
    "goth": [
        ("blush",   dict(style="contour", color=(96, 70, 92), alpha=52)),
        ("shadow",  dict(color=(70, 40, 66), alpha=95, width=0.20)),
        ("lip",     dict(style="matte", color=(58, 12, 32), alpha=170)),
        ("liner",   dict(style="graphic")),
        ("lashes",  dict(strength=1.0)),
    ],
    "peach": [
        ("blush",   dict(style="cheeks", color=(255, 140, 96), alpha=62)),
        ("shadow",  dict(color=(240, 160, 110), alpha=48)),
        ("lip",     dict(style="full", color=(245, 110, 80), alpha=100)),
        ("liner",   dict(style="soft")),
        ("highlight", dict(color=(255, 240, 220), alpha=40)),
    ],
    "cold_beauty": [
        ("blush",   dict(style="cheeks", color=(210, 160, 175), alpha=36)),
        ("shadow",  dict(color=(150, 140, 160), alpha=48)),
        ("brow",    dict(color=(52, 44, 44), alpha=95, width=0.070)),
        ("lip",     dict(style="matte", color=(150, 70, 80), alpha=105)),
        ("liner",   dict(style="soft")),
    ],
    "sunset": [
        ("blush",   dict(style="band", color=(255, 120, 70), alpha=58)),
        ("shadow",  dict(color=(235, 120, 60), alpha=70, width=0.19)),
        ("lip",     dict(style="full", color=(210, 80, 55), alpha=105)),
        ("highlight", dict(color=(255, 220, 170), alpha=52)),
        ("liner",   dict(style="wing")),
    ],
    "angel": [
        ("blush",   dict(style="cheeks", color=(255, 170, 185), alpha=40)),
        ("shadow",  dict(color=(240, 210, 220), alpha=42)),
        ("aegyo",   dict(alpha=36)),
        ("lip",     dict(style="gloss", color=(230, 130, 145), alpha=70)),
        ("highlight", dict(alpha=64)),
        ("lashes",  dict(strength=0.5)),
    ],
    "baddie": [
        ("blush",   dict(style="contour", color=(118, 76, 56), alpha=76)),
        ("shadow",  dict(color=(200, 150, 90), alpha=58)),
        ("brow",    dict(alpha=120, width=0.080)),
        ("lip",     dict(style="overline", color=(160, 100, 84), alpha=130)),
        ("liner",   dict(style="siren")),
        ("lashes",  dict(strength=0.9)),
    ],
    "cyber_chrome": [
        ("shadow",  dict(color=(80, 220, 235), alpha=80, width=0.18)),
        ("liner",   dict(style="graphic", color=(30, 210, 230), alpha=220)),
        ("highlight", dict(color=(200, 245, 255), alpha=70)),
        ("lip",     dict(style="matte", color=(60, 70, 90), alpha=110)),
    ],
    "hearts_freckles": [   # e-girl variant: heavier cheeks, sparse freckles
        ("blush",   dict(style="cheeks", color=(255, 105, 130), alpha=78)),
        ("freckles", dict(density=0.7, color=(170, 90, 80), seed=23)),
        ("lip",     dict(style="bitten", color=(225, 75, 95), alpha=92)),
        ("liner",   dict(style="wing")),
        ("aegyo",   dict(alpha=26)),
    ],
}

EL = {"blush": el_blush, "shadow": el_shadow, "aegyo": el_aegyo,
      "freckles": el_freckles, "lip": el_lip, "liner": el_liner,
      "lashes": el_lashes, "highlight": el_highlight, "brow": el_brow}

def main():
    os.makedirs(OUT, exist_ok=True)
    for look_id, recipe in LOOKS.items():
        img = Image.new("RGBA", (SZ, SZ), (0, 0, 0, 0))
        for name, kw in recipe:
            img = EL[name](img, **kw)
        path = os.path.join(OUT, f"makeup_{look_id}.png")
        img.save(path, optimize=True)
        print(f"  wrote {os.path.relpath(path, os.path.join(HERE, '..'))}"
              f" ({os.path.getsize(path)//1024} KB)")
    print(f"done — {len(LOOKS)} look textures")

if __name__ == "__main__":
    main()
