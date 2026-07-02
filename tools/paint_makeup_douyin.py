#!/usr/bin/env python3
"""Paint the Douyin makeup texture in MediaPipe canonical UV space.
Every element is positioned from the landmark UVs, so alignment with the
tracked mesh is exact by construction. Output: models/face/makeup_douyin.png
(RGBA 1024x1024, authored for neutral ~0.5-luma skin — the shader's lighting
adaptation handles the rest)."""
import os, math, random
from PIL import Image, ImageDraw, ImageFilter

here = os.path.dirname(os.path.abspath(__file__))
SZ = 1024
vt_raw, v2vt = [], {}
for line in open(os.path.join(here, "canonical_face_model.obj")):
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

eyeL_o, eyeL_i = P(33), P(133)
eyeR_o, eyeR_i = P(263), P(362)
eyeL_c = lerp(eyeL_o, eyeL_i, 0.5)
eyeR_c = lerp(eyeR_o, eyeR_i, 0.5)
ed = dist(eyeL_c, eyeR_c)               # inter-eye distance in texture px

img = Image.new("RGBA", (SZ, SZ), (0, 0, 0, 0))

def soft_ellipse(center, rx, ry, color, alpha, blur):
    l = Image.new("RGBA", (SZ, SZ), (0, 0, 0, 0))
    d = ImageDraw.Draw(l)
    d.ellipse([center[0]-rx, center[1]-ry, center[0]+rx, center[1]+ry],
              fill=color + (alpha,))
    return l.filter(ImageFilter.GaussianBlur(blur))

def over(base, layer):
    return Image.alpha_composite(base, layer)

# ── 1. Blush: pink, riding high (under-eye/cheekbone) + nose bridge ──────────
cheekL, cheekR = P(50), P(280)
bl_L = lerp(cheekL, eyeL_c, 0.38)
bl_R = lerp(cheekR, eyeR_c, 0.38)
face_mid = lerp(P(1), P(152), 0.15)
bl_L = lerp(bl_L, face_mid, 0.16)      # pull inward: the mesh silhouette
bl_R = lerp(bl_R, face_mid, 0.16)      # clips anything painted past it
pink = (255, 118, 138)
img = over(img, soft_ellipse(bl_L, ed*0.25, ed*0.18, pink, 58, ed*0.13))
img = over(img, soft_ellipse(bl_R, ed*0.25, ed*0.18, pink, 58, ed*0.13))
nose_bridge = lerp(lerp(eyeL_c, eyeR_c, 0.5), P(1), 0.45)
img = over(img, soft_ellipse(nose_bridge, ed*0.20, ed*0.10, pink, 34, ed*0.10))

# ── 2. Eyeshadow: soft rose wash above the upper lid ─────────────────────────
LID_L = [33, 161, 160, 159, 158, 157, 133]
LID_R = [263, 388, 387, 386, 385, 384, 362]
def shadow(lids, up_sign):
    l = Image.new("RGBA", (SZ, SZ), (0, 0, 0, 0))
    d = ImageDraw.Draw(l)
    pts = [P(i) for i in lids]
    off = [(x, y - ed*0.085) for x, y in pts]
    d.line(off, fill=(196, 110, 116, 60), width=int(ed*0.16), joint="curve")
    return l.filter(ImageFilter.GaussianBlur(ed*0.09))
img = over(img, shadow(LID_L, -1))
img = over(img, shadow(LID_R, +1))

# ── 3. Aegyo-sal: bright line under the eye + faint crease below ─────────────
LOW_L = [33, 7, 163, 144, 145, 153, 154, 155, 133]
LOW_R = [263, 249, 390, 373, 374, 380, 381, 382, 362]
def aegyo(lows):
    l = Image.new("RGBA", (SZ, SZ), (0, 0, 0, 0))
    d = ImageDraw.Draw(l)
    pts = [(x, y + ed*0.045) for x, y in (P(i) for i in lows)]
    d.line(pts, fill=(255, 238, 240, 30), width=int(ed*0.075), joint="curve")
    crease = [(x, y + ed*0.10) for x, y in (P(i) for i in lows)]
    d.line(crease, fill=(150, 100, 105, 18), width=int(ed*0.028), joint="curve")
    return l.filter(ImageFilter.GaussianBlur(ed*0.035))
img = over(img, aegyo(LOW_L))
img = over(img, aegyo(LOW_R))

# ── 4. Bitten lip: rose, dense at the inner ring, alpha-0 mouth opening ──────
OUTER = [61, 40, 37, 0, 267, 270, 291, 321, 314, 17, 84, 91]
INNER = [78, 191, 80, 81, 82, 13, 312, 311, 310, 415, 308,
         324, 318, 402, 317, 14, 87, 178, 88, 95]
lipL = Image.new("RGBA", (SZ, SZ), (0, 0, 0, 0))
dl = ImageDraw.Draw(lipL)
rose = (158, 28, 50)
dl.polygon([P(i) for i in OUTER], fill=rose + (82,))           # sheer wash
lipL = lipL.filter(ImageFilter.GaussianBlur(ed*0.022))
core = Image.new("RGBA", (SZ, SZ), (0, 0, 0, 0))
dc = ImageDraw.Draw(core)
inner_pts = [P(i) for i in INNER]
cx = sum(x for x, _ in inner_pts) / len(inner_pts)
cy = sum(y for _, y in inner_pts) / len(inner_pts)
# The canonical mesh has a CLOSED mouth — the inner ring is nearly a flat
# line, so the bitten core is an ellipse around the lip seam, not a scaled
# ring (that produced banding).
dc.ellipse([cx - ed*0.30, cy - ed*0.15, cx + ed*0.30, cy + ed*0.15],
           fill=rose + (100,))
core = core.filter(ImageFilter.GaussianBlur(ed*0.05))
lipL = Image.alpha_composite(lipL, core)
# Clear a thin band along the lip seam: when the mouth opens, the triangles
# between the inner lip lines stretch across it — pigment there smears over
# the teeth.
hole = Image.new("L", (SZ, SZ), 255)
dh = ImageDraw.Draw(hole)
dh.line(inner_pts[:11], fill=0, width=int(ed*0.035))
hole = hole.filter(ImageFilter.GaussianBlur(ed*0.012))
r, g, b, a = lipL.split()
from PIL import ImageChops
a = ImageChops.multiply(a, hole)
lipL = Image.merge("RGBA", (r, g, b, a))
img = over(img, lipL)

# ── 5. Liner + lash hairs (crisp, painted last) ──────────────────────────────
ink = (26, 14, 18)
random.seed(7)
def liner(lids, sign):
    l = Image.new("RGBA", (SZ, SZ), (0, 0, 0, 0))
    d = ImageDraw.Draw(l)
    pts = [P(i) for i in lids]                     # outer→inner
    base = [(x, y - ed*0.012) for x, y in pts]
    # tapered liner: draw as segments with shrinking width inward
    for i in range(len(base) - 1):
        wdt = int(ed * (0.052 - 0.028 * (i / (len(base) - 1))))
        d.line([base[i], base[i+1]], fill=ink + (235,), width=max(2, wdt))
    # wing: outward + up from the outer corner
    o = base[0]
    dirx = (pts[0][0] - pts[-1][0])
    diry = (pts[0][1] - pts[-1][1])
    dn = math.hypot(dirx, diry); dirx, diry = dirx/dn, diry/dn
    tip = (o[0] + (dirx*0.85)*ed*0.30, o[1] + (diry*0.85 - 0.55)*ed*0.30)
    mid = lerp(o, tip, 0.5)
    d.line([o, mid], fill=ink + (235,), width=int(ed*0.045))
    d.line([mid, tip], fill=ink + (235,), width=max(2, int(ed*0.018)))
    # lash hairs on the outer 65%, fanning toward the outer corner
    out_sign = 1.0 if dirx > 0 else -1.0
    for k in range(10):
        t = 0.02 + 0.63 * (k / 9.0)          # 0 = outer corner
        seg = t * (len(base) - 1)
        i0 = min(int(seg), len(base) - 2)
        root = lerp(base[i0], base[i0+1], seg - i0)
        theta = math.radians(64.0 - 40.0 * t)    # tilt from vertical, outward
        ln = ed * (0.055 + 0.05 * (1 - t) + random.uniform(-0.008, 0.008))
        tip2 = (root[0] + math.sin(theta) * ln * out_sign,
                root[1] - math.cos(theta) * ln)
        d.line([root, tip2], fill=ink + (200,), width=2)
    return l
img = over(img, liner(LID_L, -1))
img = over(img, liner(LID_R, +1))

out = os.path.join(here, "..", "build", "models", "face", "makeup_douyin.png")
img.save(out)
print("wrote", out, f"(eye distance {ed:.0f}px in UV space)")
