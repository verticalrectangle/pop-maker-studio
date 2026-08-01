"""Deterministic style-recipe → IPC-op expander for Pop Maker Studio.

Turns a declarative style recipe (mcp_server/recipes/*.json) plus a section of
the timeline (beat timestamps, per-second RMS energy) into an ordered list of
ordinary, fully-editable IPC ops — shape clips with pos/scale/rotation/opacity
keyframes, path-morph keyframes, shape colour keyframes, mirror fold/reflect —
that the MCP server replays inside ONE undo batch (label "Animate section (<id>)").

Everything is driven through ``random.Random(seed)`` and pure math, so the same
inputs always produce byte-identical ops. Nothing here talks to the app; the
server executes the returned ops verbatim via the IPC socket.

Energy routing: per-second RMS is normalised to the section maximum first; a bar
whose mean energy is > 0.66 draws a motif from recipe.motifs.high, > 0.33 from
mid, else from low. Bars are bpb beats wide, starting at the first beat >=
section.start (the first bar is treated as the section's downbeat).
"""

from __future__ import annotations

import math
import random
from typing import Any, Callable

# ── Motif vocabulary ─────────────────────────────────────────────────────────
# Every motif emits ops for its clip(s) on a given track spanning the bar window:
#   fn(track, clip, bar, colors, rng, ctx) -> list[{method, params}]
# where `clip` is the index of the motif's FIRST clip — add_shape returns
# consecutive indices as ops replay inside one batch, so the expander advances
# a cursor by the clips each motif creates (multi-clip motifs use clip+j).
MOTIF_VOCABULARY = (
    "bloom_ring",
    "petal_burst",
    "star_morph",
    "pulse_bloom",
    "orbit_petals",
    "draw_on_vine",
    "slow_bloom",
    "flower_scatter",
    "ribbon_waves",
    "butterfly",
)


# ── Small geometry / colour helpers ──────────────────────────────────────────

def _hex_to_rgba(h: str) -> list[float]:
    """'#RRGGBB' (or '#RGB') → [r, g, b, a] floats in 0..1."""
    h = h.lstrip("#").strip()
    if len(h) == 3:
        h = "".join(c * 2 for c in h)
    if len(h) != 6:
        raise ValueError(f"not a hex colour: {h!r}")
    return [round(int(h[i:i + 2], 16) / 255.0, 4) for i in (0, 2, 4)] + [1.0]


def _luminance(rgba: list[float]) -> float:
    return 0.2126 * rgba[0] + 0.7152 * rgba[1] + 0.0722 * rgba[2]


def _darkest(colors: list[list[float]], n: int) -> list[list[float]]:
    """The n least-luminous palette colours (stable order)."""
    return sorted(colors, key=_luminance)[:n]


def _cycle_color(colors: list[list[float]], period: int, beat_index: int) -> list[float]:
    """Palette colour for a section-local beat index under the recipe's colour
    motion envelope (palette_cycle: one hue per `period` beats)."""
    return colors[(beat_index // period) % len(colors)]


def _op(method: str, params: dict) -> dict:
    return {"method": method, "params": params}


def _star_path(pts: int, inner: float, r: float = 0.42,
               cx: float = 0.5, cy: float = 0.5, w: float = 0.008) -> list[dict]:
    """Star outline: 2*pts alternating outer/inner vertices, tip up."""
    path = []
    for i in range(2 * pts):
        ang = math.pi * i / pts - math.pi / 2.0
        rad = r if i % 2 == 0 else r * inner
        path.append({"x": round(cx + rad * math.cos(ang), 4),
                     "y": round(cy + rad * math.sin(ang), 4), "w": w})
    return path


def _poly_path(sides: int, r: float = 0.42,
               cx: float = 0.5, cy: float = 0.5, w: float = 0.008) -> list[dict]:
    """Regular polygon outline."""
    path = []
    for i in range(sides):
        ang = 2.0 * math.pi * i / sides - math.pi / 2.0
        path.append({"x": round(cx + r * math.cos(ang), 4),
                     "y": round(cy + r * math.sin(ang), 4), "w": w})
    return path


def _teardrop_path(n: int, cx: float = 0.5, cy: float = 0.5,
                   w: float = 0.006) -> list[dict]:
    """Teardrop petal: sharp tip at top, round belly at the bottom (closed)."""
    n = int(max(6, min(10, n)))
    pts = [{"x": round(cx, 4), "y": round(cy - 0.45, 4), "w": w}]  # tip
    for i in range(1, n - 1):
        t = math.pi * (i / (n - 2))          # 0..pi across the belly
        x = cx - 0.36 * math.cos(t)          # left → right
        y = cy + 0.26 * math.sin(t)          # dips to cy+0.26 at the middle
        pts.append({"x": round(x, 4), "y": round(y, 4), "w": w})
    return pts


def _vine_path(n: int, w: float = 0.004) -> list[dict]:
    """Open sine-ish wavy polyline across the local square (open, not closed)."""
    n = int(max(12, min(20, n)))
    pts = []
    for i in range(n):
        t = i / (n - 1)
        pts.append({"x": round(0.08 + 0.84 * t, 4),
                    "y": round(0.5 + 0.14 * math.sin(2.2 * math.pi * t + 0.7), 4),
                    "w": w})
    return pts


def _flower_path(petals: int, r0: float = 0.42, cx: float = 0.5, cy: float = 0.5,
                 w: float = 0.008, n: int = 36) -> list[dict]:
    """Flat rounded flower (polar rose): r(θ) = r0·(0.55 + 0.45·|cos(petals·θ/2)|),
    sampled closed. Rounded petals, not pointy star spikes."""
    petals = int(petals)
    pts = []
    for i in range(n):
        th = 2.0 * math.pi * i / n
        r = r0 * (0.55 + 0.45 * abs(math.cos(petals * th / 2.0)))
        pts.append({"x": round(cx + r * math.cos(th), 4),
                    "y": round(cy + r * math.sin(th), 4), "w": w})
    return pts


def _ribbon_path(y_top: float, amp: float, phase: float, wavelength: float,
                 y_bottom: float = 1.5, n: int = 24, w: float = 0.004) -> list[dict]:
    """Closed horizontal wavy band: top edge = sine curve, bottom edge extended
    below the canvas so the band reads as a flowing ribbon field."""
    pts = []
    for i in range(n + 1):
        x = i / n
        y = y_top + amp * math.sin(2.0 * math.pi * x / wavelength + phase)
        pts.append({"x": round(x, 4), "y": round(y, 4), "w": w})
    pts.append({"x": 1.0, "y": y_bottom, "w": w})
    pts.append({"x": 0.0, "y": y_bottom, "w": w})
    return pts


def _catmull_rom(points: list[tuple[float, float]],
                 samples: int = 4) -> list[tuple[float, float]]:
    """Smooth closed polyline through control points (uniform Catmull-Rom)."""
    n = len(points)
    out = []
    for i in range(n):
        p0 = points[(i - 1) % n]
        p1, p2 = points[i], points[(i + 1) % n]
        p3 = points[(i + 2) % n]
        for j in range(samples):
            t = j / samples
            t2, t3 = t * t, t * t * t
            x = 0.5 * ((2 * p1[0]) + (-p0[0] + p2[0]) * t +
                       (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2 +
                       (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * t3)
            y = 0.5 * ((2 * p1[1]) + (-p0[1] + p2[1]) * t +
                       (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2 +
                       (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * t3)
            out.append((x, y))
    return out


# Butterfly silhouette control points (right half, offsets from centre,
# x scaled by wing openness at build time): swept forewing with a pointed
# apex, an OUTER-edge scoop separating the hindwing fan (that scoop is what
# reads as two wings — a waist notch alone still blobs), Catmull-Rom sampled
# for a smooth, finely-detailed outline.
_BUTTERFLY_HALF = [
    (0.000, -0.300), (0.016, -0.300), (0.024, -0.290),   # head/neck
    (0.050, -0.300), (0.130, -0.325), (0.220, -0.345),   # leading edge up-out
    (0.300, -0.340), (0.350, -0.310),                    # forewing apex (point)
    (0.320, -0.250), (0.250, -0.185), (0.165, -0.130),   # trailing edge dropping
    (0.100, -0.085), (0.075, -0.045),                    # outer scoop (concavity)
    (0.045, -0.005),                                     # waist at body
    (0.070,  0.050), (0.115,  0.110), (0.165,  0.160),   # hindwing out
    (0.190,  0.205), (0.175,  0.250), (0.130,  0.275),   # hindwing fan
    (0.080,  0.275), (0.035,  0.255),                    # hindwing in
    (0.000,  0.265),                                     # tail
]


def _butterfly_path(openness: float, cx: float = 0.5, cy: float = 0.5,
                    w: float = 0.004) -> list[dict]:
    """Detailed butterfly outline (~130 pts, Catmull-Rom smoothed): body +
    forewing/hindwing lobes, parameterised by wing openness ∈ [0.3, 1.0]
    (wing x-extent scales with it — 0.3 = wings folded back mid-flutter)."""
    o = max(0.3, min(1.0, float(openness)))
    right = [(cx + dx * o, cy + dy) for dx, dy in _BUTTERFLY_HALF]
    # closed loop: head → right side → tail → mirrored left side → head
    loop = right + [(2 * cx - x, y) for x, y in reversed(right[1:-1])]
    smooth = _catmull_rom(loop, samples=4)
    return [{"x": round(x, 4), "y": round(y, 4), "w": w} for x, y in smooth]


def _butterfly_spots(openness: float, cx: float = 0.5, cy: float = 0.5,
                     w: float = 0.004) -> list[list[dict]]:
    """Wing-spot accent paths (forewing + hindwing, both wings): small filled
    ellipses that flutter in sync with the body (x scales with openness)."""
    o = max(0.3, min(1.0, float(openness)))
    spots = []
    for scx, scy, r in ((0.210, -0.230, 0.045), (0.125, 0.165, 0.035)):
        for side in (1, -1):
            pts = []
            for i in range(12):
                a = 2.0 * math.pi * i / 12
                pts.append({"x": round(cx + side * scx * o + r * math.cos(a), 4),
                            "y": round(cy + scy + r * math.sin(a), 4), "w": w})
            spots.append(pts)
    return spots


def _tealish(colors: list[list[float]]) -> list[float]:
    """Palette entry closest to saturated teal/cyan (max g+b−r)."""
    return max(colors, key=lambda c: c[1] + c[2] - c[0])


def _contrast_color(colors: list[list[float]], base: list[float]) -> list[float]:
    """Palette entry with maximum RGB distance from `base` (contrasting stroke)."""
    return max(colors, key=lambda c: (abs(c[0] - base[0]) + abs(c[1] - base[1])
                                      + abs(c[2] - base[2])))


# ── Bar building + energy routing ────────────────────────────────────────────

def _normalize_rms(rms: list[float], start: float, end: float) -> list[float]:
    """RMS values rescaled so the section's maximum becomes 1.0 (0 if silent)."""
    if not rms:
        return []
    lo = max(0, int(math.floor(start)))
    hi = min(len(rms), int(math.ceil(end)) + 1)
    window = rms[lo:hi] if hi > lo else rms
    m = max(window) if window else 0.0
    if m <= 0.0:
        return [0.0] * len(rms)
    return [min(1.0, x / m) for x in rms]


def _build_bars(beats: list[float], start: float, end: float, bpb: int,
                rms_norm: list[float]) -> list[dict]:
    """Group section beats into bars of `bpb` beats, each with an energy 0..1.

    Bar i starts at the i-th section beat; a bar's end is the next bar's start
    (or the section end). `next_end` = the end of the following bar (or the
    section end) so a motif may span up to two bars. The first bar is the
    section's downbeat.
    """
    sec = [b for b in beats if start - 1e-3 <= b < end]
    if not sec:
        return []
    bars = []
    for i in range(0, len(sec), bpb):
        group = sec[i:i + bpb]
        b_start = group[0]
        b_end = sec[i + bpb] if i + bpb < len(sec) else end
        b_end = min(b_end, end)
        if b_end - b_start < 1e-6:
            continue
        nxt2 = sec[i + 2 * bpb] if i + 2 * bpb < len(sec) else end
        vals = [rms_norm[s] for s in range(int(math.floor(b_start)),
                                           int(math.ceil(b_end)))
                if 0 <= s < len(rms_norm)]
        energy = round(sum(vals) / len(vals), 4) if vals else 0.0
        bars.append({
            "index": len(bars),
            "start": round(b_start, 4),
            "end": round(b_end, 4),
            "beat_index": i,                    # section-local index of first beat
            "beats": [round(x, 4) for x in group],
            "downbeat": len(bars) == 0,
            "energy": energy,
            "next_end": round(min(nxt2, end), 4),
            "motif": None,
        })
    return bars


def _pick_motif(recipe: dict, bucket: str, prev: str | None,
                rng: random.Random) -> str | None:
    """Seeded motif pick for a bucket, avoiding an immediate repeat.

    Honors the optional recipe.motif_weights[bucket] map ({motif: weight});
    absent weights mean equal probability. When the bucket falls back to
    another tier, that tier's weights (if any) apply."""
    motifs = recipe.get("motifs") or {}
    src = bucket
    choices = [m for m in motifs.get(src, []) if m in MOTIF_VOCABULARY]
    if not choices:
        for alt in ("mid", "high", "low"):
            choices = [m for m in motifs.get(alt, []) if m in MOTIF_VOCABULARY]
            if choices:
                src = alt
                break
    if not choices:
        return None
    pool = [m for m in choices if m != prev] or choices
    wmap = (recipe.get("motif_weights") or {}).get(src, {})
    if wmap:
        weights = [float(wmap.get(m, 1.0)) for m in pool]
        if any(w > 0 for w in weights):
            return rng.choices(pool, weights=weights, k=1)[0]
    return rng.choice(pool)


# ── Motifs ───────────────────────────────────────────────────────────────────
# Each motif: (track:int, clip:int, bar:dict, colors:list[list[float]],
#              rng:random.Random, ctx:dict) -> list[dict]
# ctx carries {period, fold, reflect, section_start, section_end, fps}.

def _motif_bloom_ring(track, clip, bar, colors, rng, ctx):
    """Central star/burst with mirror fold; scale pulses on every beat, colour
    keys cycle the palette on the bar's beats, path morphs star → polygon →
    star across the window."""
    start, end, dur = bar["start"], bar["end"], round(bar["end"] - bar["start"], 4)
    beats = bar["beats"]
    ops = []
    pts = rng.choice([5, 6])
    # add_shape's star bake treats params[1] as an ABSOLUTE inner radius
    # (outer = 0.5, default 0.22), despite the 'ratio' doc — a value ≥ 0.42
    # reads as a wobbly polygon, not a star. Convert our ratio to absolute.
    inner_ratio = rng.uniform(0.42, 0.55)
    inner = round(0.5 * inner_ratio, 3)
    ops.append(_op("add_shape", {"track": track, "start": start, "end": end,
                                 "preset": "star", "params": [pts, inner]}))
    bi = bar["beat_index"]
    if ctx.get("flat"):
        # Flat compositions (album-cover look): no gradient, no glow — they
        # flatten the star silhouette into a fuzzy orb.
        ops.append(_op("set_shape_style", {
            "track": track, "clip": clip,
            "fill_on": True, "fill_col": _cycle_color(colors, ctx["period"], bi),
            "stroke_on": False,
        }))
    else:
        ops.append(_op("set_shape_style", {
            "track": track, "clip": clip,
            "fill_on": True, "fill_col": _cycle_color(colors, ctx["period"], bi),
            "grad_mode": 2, "grad_col2": _cycle_color(colors, ctx["period"], bi + 1),
            "stroke_on": True, "stroke_col": _cycle_color(colors, ctx["period"], bi + 1),
            "stroke_width": 0.006,
            "glow_on": True, "glow_col": _cycle_color(colors, ctx["period"], bi + 2),
            "glow_radius": 0.045, "glow_intensity": 0.6,
        }))
    ops.append(_op("set_clip_prop", {"track": track, "clip": clip,
                                     "prop": "shape_mirror_fold",
                                     "value": float(ctx["fold"])}))
    if ctx["reflect"]:
        ops.append(_op("set_clip_prop", {"track": track, "clip": clip,
                                         "prop": "shape_mirror_reflect",
                                         "value": 1.0}))
    # 0.45 → 0.8 → 0.45 per beat (stays inside the canvas when the fold
    # replicates it); the beat key carries ease_out so the attack is punchy
    # and the decay eases back down. A ctx.scale_override (set by the field
    # composition's peak blooms) remaps the pulse around a base scale.
    lo_s, hi_s = 0.45, 0.8
    so = ctx.get("scale_override")
    if so is not None:
        lo_s = round(so * 0.8, 4)
        hi_s = round(so * 1.2, 4)
    sx, sy = [], []
    for k, b in enumerate(beats):
        t = _clamp_t(b - start, dur)
        nxt = beats[k + 1] if k + 1 < len(beats) else end
        attack = min(0.12, (nxt - b) * 0.25)
        for track_prop, keys in (("scale_x", sx), ("scale_y", sy)):
            keys.append({"t": t, "v": lo_s, "interp": "ease_out"})
            keys.append({"t": _clamp_t(t + attack, dur), "v": hi_s,
                         "interp": "ease_out"})
    sx.append({"t": dur, "v": lo_s, "interp": "ease_out"})
    sy.append({"t": dur, "v": lo_s, "interp": "ease_out"})
    ops.append(_op("set_clip_keyframes", {"track": track, "clip": clip,
                                          "prop": "scale_x", "keys": sx}))
    ops.append(_op("set_clip_keyframes", {"track": track, "clip": clip,
                                          "prop": "scale_y", "keys": sy}))
    # colour cycle on the bar's beats (downbeat = bar start)
    ck = [{"t": _clamp_t(b - start, dur),
           "v": _cycle_color(colors, ctx["period"], bi + k), "interp": "ease_both"}
          for k, b in enumerate(beats)]
    ops.append(_op("set_shape_color_keyframes", {"track": track, "clip": clip,
                                                 "prop": "fill_col", "keys": ck}))
    # star → polygon(pts+2) → star across the window (engine resamples counts).
    # ctx.no_morph (field-composition peak blooms) keeps the star silhouette —
    # the polygon phase reads as a flat blob on flat-colour compositions.
    if not ctx.get("no_morph"):
        half = round(dur / 2.0, 4)
        ops.append(_op("set_shape_keyframes", {"track": track, "clip": clip, "keys": [
            {"t": 0.0, "points": _star_path(pts, inner_ratio), "closed": True,
             "interp": "ease_both"},
            {"t": half, "points": _poly_path(pts + 2), "closed": True,
             "interp": "ease_both"},
            {"t": dur, "points": _star_path(pts, inner_ratio), "closed": True,
             "interp": "ease_both"},
        ]}))
    return ops


def _motif_petal_burst(track, clip, bar, colors, rng, ctx):
    """One teardrop petal path placed off-centre; the mirror fold radiates N
    copies around the circle. Opacity pulses in per beat, fill/stroke take
    alternating palette colours."""
    start, end, dur = bar["start"], bar["end"], round(bar["end"] - bar["start"], 4)
    beats = bar["beats"]
    ops = []
    ops.append(_op("add_shape", {"track": track, "start": start, "end": end,
                                 "preset": "circle"}))
    ops.append(_op("set_shape_path", {"track": track, "clip": clip,
                                      "points": _teardrop_path(rng.randint(6, 10)),
                                      "closed": True}))
    bi = bar["beat_index"]
    fill = _cycle_color(colors, ctx["period"], bi)
    stroke = _cycle_color(colors, ctx["period"], bi + 1)
    ops.append(_op("set_shape_style", {
        "track": track, "clip": clip,
        "fill_on": True, "fill_col": fill,
        "stroke_on": True, "stroke_col": stroke, "stroke_width": 0.005,
        "glow_on": True, "glow_col": stroke, "glow_radius": 0.06,
        "glow_intensity": 0.7,
    }))
    ops.append(_op("set_clip_prop", {"track": track, "clip": clip,
                                     "prop": "shape_mirror_fold",
                                     "value": float(ctx["fold"])}))
    if ctx["reflect"]:
        ops.append(_op("set_clip_prop", {"track": track, "clip": clip,
                                         "prop": "shape_mirror_reflect",
                                         "value": 1.0}))
    # Radial position, held with keys (the fold spins copies around it).
    ang = rng.uniform(0.0, 2.0 * math.pi)
    rad = round(rng.uniform(0.10, 0.22), 4)
    px = round(0.5 + rad * math.cos(ang), 4)
    py = round(0.5 + rad * math.sin(ang), 4)
    for prop, v in (("pos_x", px), ("pos_y", py)):
        ops.append(_op("set_clip_keyframes", {
            "track": track, "clip": clip, "prop": prop,
            "keys": [{"t": 0.0, "v": v, "interp": "hold"},
                     {"t": dur, "v": v, "interp": "hold"}],
        }))
    # Opacity staggered per beat: fade in at the beat, fade out before the next.
    ok = []
    for k, b in enumerate(beats):
        t = _clamp_t(b - start, dur)
        nxt = beats[k + 1] if k + 1 < len(beats) else end
        peak = _clamp_t(t + min(0.08, (nxt - b) * 0.3), dur)
        ok.append({"t": t, "v": 0.0, "interp": "ease_out"})
        ok.append({"t": peak, "v": 1.0, "interp": "ease_out"})
        ok.append({"t": _clamp_t(nxt - start, dur), "v": 0.0, "interp": "ease_out"})
    ops.append(_op("set_clip_keyframes", {"track": track, "clip": clip,
                                          "prop": "opacity", "keys": ok}))
    return ops


def _motif_star_morph(track, clip, bar, colors, rng, ctx):
    """Single centred star that morphs its point count on every beat
    (resample-tolerant path keys), slowly spins, and cycles fill colour."""
    start, end, dur = bar["start"], bar["end"], round(bar["end"] - bar["start"], 4)
    beats = bar["beats"]
    ops = []
    pts0 = rng.randint(4, 6)
    inner = round(rng.uniform(0.45, 0.60), 3)
    ops.append(_op("add_shape", {"track": track, "start": start, "end": end,
                                 "preset": "star", "params": [pts0, inner]}))
    bi = bar["beat_index"]
    ops.append(_op("set_shape_style", {
        "track": track, "clip": clip,
        "fill_on": True, "fill_col": _cycle_color(colors, ctx["period"], bi),
        "grad_mode": 2, "grad_col2": _cycle_color(colors, ctx["period"], bi + 1),
        "glow_on": True, "glow_col": _cycle_color(colors, ctx["period"], bi),
        "glow_radius": 0.08, "glow_intensity": 0.6,
    }))
    morph = []
    for k, b in enumerate(beats):
        pts_k = pts0 + (k % 3)  # cycle 0..2 extra points across the bar
        morph.append({"t": _clamp_t(b - start, dur),
                      "points": _star_path(pts_k, inner), "closed": True,
                      "interp": "ease_both"})
    ops.append(_op("set_shape_keyframes", {"track": track, "clip": clip,
                                           "keys": morph}))
    # slow spin: 45° per beat
    spin = round(45.0 * len(beats), 2)
    ops.append(_op("set_clip_keyframes", {
        "track": track, "clip": clip, "prop": "rotation",
        "keys": [{"t": 0.0, "v": 0.0, "interp": "linear"},
                 {"t": dur, "v": spin, "interp": "linear"}],
    }))
    # hue-ish palette cycle through fill_col per beat
    ck = [{"t": _clamp_t(b - start, dur),
           "v": _cycle_color(colors, ctx["period"], bi + k), "interp": "ease_both"}
          for k, b in enumerate(beats)]
    ops.append(_op("set_shape_color_keyframes", {"track": track, "clip": clip,
                                                 "prop": "fill_col", "keys": ck}))
    return ops


def _motif_pulse_bloom(track, clip, bar, colors, rng, ctx):
    """Circle whose scale snaps on every beat (hold attack, ease_out decay),
    stroke width riding the bar's RMS energy, one palette colour per bar."""
    start, end, dur = bar["start"], bar["end"], round(bar["end"] - bar["start"], 4)
    beats = bar["beats"]
    ops = []
    ops.append(_op("add_shape", {"track": track, "start": start, "end": end,
                                 "preset": "circle"}))
    bi = bar["beat_index"]
    fill = _cycle_color(colors, ctx["period"], bi)
    stroke = _cycle_color(colors, ctx["period"], bi + 1)
    ops.append(_op("set_shape_style", {
        "track": track, "clip": clip,
        "fill_on": True, "fill_col": fill,
        "grad_mode": 2, "grad_col2": stroke,
        "stroke_on": True, "stroke_col": stroke, "stroke_width": 0.012,
    }))
    # scale: peak snaps at each beat (the low key before it holds), then eases
    # out down to the low value well before the next beat.
    sx, sy = [], []
    for k, b in enumerate(beats):
        t = _clamp_t(b - start, dur)
        nxt = beats[k + 1] if k + 1 < len(beats) else end
        decay = _clamp_t(t + max(0.05, (nxt - b) * 0.6), dur)
        for keys in (sx, sy):
            keys.append({"t": t, "v": 1.15, "interp": "ease_out"})
            keys.append({"t": decay, "v": 0.7, "interp": "hold"})
    for prop, keys in (("scale_x", sx), ("scale_y", sy)):
        ops.append(_op("set_clip_keyframes", {"track": track, "clip": clip,
                                              "prop": prop, "keys": keys}))
    # stroke width rides the bar energy (static across the bar)
    mul = round(0.5 + 1.5 * bar["energy"], 4)
    ops.append(_op("set_clip_keyframes", {
        "track": track, "clip": clip, "prop": "shape_stroke_width_mul",
        "keys": [{"t": 0.0, "v": mul, "interp": "linear"},
                 {"t": dur, "v": mul, "interp": "linear"}],
    }))
    # colour cycle per bar
    ops.append(_op("set_shape_color_keyframes", {
        "track": track, "clip": clip, "prop": "fill_col",
        "keys": [{"t": 0.0, "v": fill, "interp": "ease_both"},
                 {"t": dur, "v": fill, "interp": "ease_both"}],
    }))
    return ops


def _motif_orbit_petals(track, clip, bar, colors, rng, ctx):
    """2-4 small shapes whose pos_x/pos_y keys trace circular orbits (one
    revolution per beat, 4-8 linear keys per revolution), mirror fold 2..4,
    staggered phase per petal."""
    start, end, dur = bar["start"], bar["end"], round(bar["end"] - bar["start"], 4)
    beats = bar["beats"]
    ops = []
    count = rng.randint(2, 4)
    fold = rng.randint(2, 4)
    kpr = rng.randint(4, 8)  # keys per revolution
    base_phase = rng.uniform(0.0, 2.0 * math.pi)
    for j in range(count):
        c = _cycle_color(colors, ctx["period"], bar["beat_index"] + j)
        ops.append(_op("add_shape", {"track": track, "start": start, "end": end,
                                     "preset": rng.choice(["circle", "heart",
                                                           "diamond", "triangle"])}))
        ops.append(_op("set_shape_style", {
            "track": track, "clip": clip + j,
            "fill_on": True, "fill_col": c,
            "stroke_on": True, "stroke_col": c, "stroke_width": 0.005,
        }))
        ops.append(_op("set_clip_prop", {"track": track, "clip": clip + j,
                                         "prop": "shape_mirror_fold",
                                         "value": float(fold)}))
        if ctx["reflect"]:
            ops.append(_op("set_clip_prop", {"track": track, "clip": clip + j,
                                             "prop": "shape_mirror_reflect",
                                             "value": 1.0}))
        radius = round(rng.uniform(0.10, 0.20), 4)
        phase = base_phase + j * (2.0 * math.pi / count)
        revs = max(1, len(beats))
        period = dur / revs
        px, py = [], []
        for r in range(revs):
            for k in range(kpr):
                t = _clamp_t((r + k / kpr) * period, dur)
                ang = phase + 2.0 * math.pi * (r + k / kpr)
                px.append({"t": t, "v": round(0.5 + radius * math.cos(ang), 4),
                           "interp": "linear"})
                py.append({"t": t, "v": round(0.5 + radius * math.sin(ang), 4),
                           "interp": "linear"})
        px.append({"t": dur, "v": px[0]["v"], "interp": "linear"})
        py.append({"t": dur, "v": py[0]["v"], "interp": "linear"})
        for prop, keys in (("pos_x", px), ("pos_y", py)):
            ops.append(_op("set_clip_keyframes", {"track": track, "clip": clip + j,
                                                  "prop": prop, "keys": keys}))
    return ops


def _motif_draw_on_vine(track, clip, bar, colors, rng, ctx):
    """Open wavy vine path drawn on via shape_stroke_length 0→1 across 1-2
    bars: thin stroke, glow, no fill."""
    start = bar["start"]
    end = max(bar["end"], bar["next_end"])  # up to two bars
    dur = round(end - start, 4)
    ops = []
    ops.append(_op("add_shape", {"track": track, "start": start, "end": end,
                                 "preset": "circle"}))
    ops.append(_op("set_shape_path", {"track": track, "clip": clip,
                                      "points": _vine_path(rng.randint(12, 20)),
                                      "closed": False}))
    bi = bar["beat_index"]
    stroke = _cycle_color(colors, ctx["period"], bi)
    glow = _cycle_color(colors, ctx["period"], bi + 1)
    ops.append(_op("set_shape_style", {
        "track": track, "clip": clip,
        "fill_on": False, "stroke_on": True, "stroke_col": stroke,
        "stroke_width": 0.005,
        "glow_on": True, "glow_col": glow, "glow_radius": 0.05,
        "glow_intensity": 0.8,
    }))
    # draw-on reveal across the window
    draw_end = _clamp_t(dur * 0.9, dur)
    ops.append(_op("set_clip_keyframes", {
        "track": track, "clip": clip, "prop": "shape_stroke_length",
        "keys": [{"t": 0.0, "v": 0.0, "interp": "ease_out"},
                 {"t": draw_end, "v": 1.0, "interp": "hold"}],
    }))
    # gentle sway
    ops.append(_op("set_clip_keyframes", {
        "track": track, "clip": clip, "prop": "rotation",
        "keys": [{"t": 0.0, "v": -8.0, "interp": "linear"},
                 {"t": dur, "v": 8.0, "interp": "linear"}],
    }))
    return ops


def _motif_slow_bloom(track, clip, bar, colors, rng, ctx):
    """Big low-opacity polygon in the palette's dark colours: radial gradient,
    slow rotation, gentle 0.9↔1.05 breathing with ease_both."""
    start, end, dur = bar["start"], bar["end"], round(bar["end"] - bar["start"], 4)
    beats = bar["beats"]
    ops = []
    sides = rng.randint(6, 12)
    ops.append(_op("add_shape", {"track": track, "start": start, "end": end,
                                 "preset": "polygon", "params": [float(sides)]}))
    dark = _darkest(colors, 2)
    ops.append(_op("set_shape_style", {
        "track": track, "clip": clip,
        "fill_on": True, "fill_col": dark[0],
        "grad_mode": 2, "grad_col2": dark[1],
        "glow_on": True, "glow_col": dark[0], "glow_radius": 0.14,
        "glow_intensity": 0.5,
    }))
    # breathing scale, half-beat cadence, ease_both
    steps = max(2, len(beats) * 2)
    bk = []
    for k in range(steps + 1):
        t = _clamp_t(k * (dur / steps), dur)
        bk.append({"t": t, "v": 1.05 if k % 2 == 0 else 0.9, "interp": "ease_both"})
    for prop in ("scale_x", "scale_y"):
        ops.append(_op("set_clip_keyframes", {"track": track, "clip": clip,
                                              "prop": prop, "keys": bk}))
    ops.append(_op("set_clip_keyframes", {
        "track": track, "clip": clip, "prop": "rotation",
        "keys": [{"t": 0.0, "v": 0.0, "interp": "linear"},
                 {"t": dur, "v": 30.0, "interp": "linear"}],
    }))
    ops.append(_op("set_clip_keyframes", {
        "track": track, "clip": clip, "prop": "opacity",
        "keys": [{"t": 0.0, "v": 0.35, "interp": "ease_both"},
                 {"t": dur, "v": 0.35, "interp": "ease_both"}],
    }))
    return ops


def _motif_flower_scatter(track, clip, bar, colors, rng, ctx):
    """Flat-collage flower field: N rounded-petal flowers (4-9, density-scaled)
    scattered on a seeded jittered grid, one clip per flower. Same track + same
    bar window is legal — shape clips don't occupy a row (row_overlap is a
    no-op for ClipType::Shape). No mirror fold: flat album-cover look."""
    start, end, dur = bar["start"], bar["end"], round(bar["end"] - bar["start"], 4)
    beats = bar["beats"]
    n = min(9, max(4, round(3 + 6 * ctx["density"])))
    bi = bar["beat_index"]
    cols = 3
    rows = max(1, math.ceil(n / cols))
    cells = [(ci, ri) for ri in range(rows) for ci in range(cols)]
    rng.shuffle(cells)
    ops = []
    for j in range(n):
        ci, ri = cells[j]
        px = round((ci + 0.5 + rng.uniform(-0.14, 0.14)) / cols, 4)
        py = round((ri + 0.5 + rng.uniform(-0.14, 0.14)) / rows, 4)
        petals = rng.choice([5, 6])
        size = round(rng.uniform(0.5, 1.1), 4)
        fill = _cycle_color(colors, ctx["period"], bi + j)
        stroke = _contrast_color(colors, fill)
        ops.append(_op("add_shape", {"track": track, "start": start, "end": end,
                                     "preset": "circle"}))
        ops.append(_op("set_shape_path", {"track": track, "clip": clip + j,
                                           "points": _flower_path(petals),
                                           "closed": True}))
        ops.append(_op("set_shape_style", {
            "track": track, "clip": clip + j,
            "fill_on": True, "fill_col": fill,
            "stroke_on": True, "stroke_col": stroke, "stroke_width": 0.006,
        }))
        ops.append(_op("set_clip_prop", {"track": track, "clip": clip + j,
                                         "prop": "pos_x", "value": px}))
        ops.append(_op("set_clip_prop", {"track": track, "clip": clip + j,
                                         "prop": "pos_y", "value": py}))
        # scale breathe: 0.85·size ↔ 1.0·size, period 1-2 beats, phase-offset
        bl = (beats[1] - beats[0]) if len(beats) > 1 else dur / max(1, len(beats))
        period = bl * rng.choice([1.0, 2.0])
        start_low = rng.uniform(0.0, 2.0 * math.pi) < math.pi
        lo, hi = round(size * 0.85, 4), round(size, 4)
        sk, t = [], 0.0
        while t < dur - 1e-4:
            sk.append({"t": round(t, 4), "v": lo if start_low else hi,
                       "interp": "ease_both"})
            start_low = not start_low
            t += period
        sk.append({"t": dur, "v": lo if start_low else hi, "interp": "ease_both"})
        for prop in ("scale_x", "scale_y"):
            ops.append(_op("set_clip_keyframes", {"track": track, "clip": clip + j,
                                                  "prop": prop, "keys": sk}))
        # slow rotation drift (±~15° over the bar)
        drift = round(rng.uniform(10.0, 15.0), 2)
        if rng.random() < 0.5:
            drift = -drift
        ops.append(_op("set_clip_keyframes", {
            "track": track, "clip": clip + j, "prop": "rotation",
            "keys": [{"t": 0.0, "v": drift, "interp": "ease_both"},
                     {"t": dur, "v": -drift, "interp": "ease_both"}],
        }))
        # occasional Hold-interp colour swap mid-clip
        if rng.random() < 0.4:
            swap = _cycle_color(colors, ctx["period"], bi + j + n)
            ops.append(_op("set_shape_color_keyframes", {
                "track": track, "clip": clip + j, "prop": "fill_col",
                "keys": [{"t": 0.0, "v": fill, "interp": "hold"},
                         {"t": round(dur * 0.5, 4), "v": swap, "interp": "hold"}],
            }))
    return ops


def _motif_ribbon_waves(track, clip, bar, colors, rng, ctx):
    """3 layered wavy ribbon bands (closed sine-edge slabs extending below the
    canvas), back-to-front so later clips composite on top; phase drifts by half
    a wavelength over 1-2 bars via path-morph keys. Warm palette sequence."""
    start = bar["start"]
    end = max(bar["end"], bar["next_end"])  # 1-2 bars
    dur = round(end - start, 4)
    ops = []
    bi = bar["beat_index"]
    crests = (0.80, 0.55, 0.30)  # back → front; deep band emitted first
    for layer, crest in enumerate(crests):
        amp = round(rng.uniform(0.10, 0.16), 4)
        wl = round(rng.uniform(0.9, 1.3), 3)
        phase = round(rng.uniform(0.0, 2.0 * math.pi), 4)
        # after bbox-normalisation the wave crest sits at local y=0; position it
        # at `crest` and let the band body extend below the canvas.
        s_y = round((1.0 - crest) * 1.2, 4)
        p_y = round(crest + 0.5 * s_y, 4)
        fill = _cycle_color(colors, ctx["period"], bi + layer)
        ops.append(_op("add_shape", {"track": track, "start": start, "end": end,
                                     "preset": "circle"}))
        ops.append(_op("set_shape_path", {"track": track, "clip": clip + layer,
                                           "points": _ribbon_path(crest, amp, phase, wl),
                                           "closed": True}))
        ops.append(_op("set_shape_style", {"track": track, "clip": clip + layer,
                                            "fill_on": True, "fill_col": fill,
                                            "stroke_on": False}))
        ops.append(_op("set_clip_prop", {"track": track, "clip": clip + layer,
                                         "prop": "scale_x", "value": 2.0}))
        ops.append(_op("set_clip_prop", {"track": track, "clip": clip + layer,
                                         "prop": "scale_y", "value": s_y}))
        ops.append(_op("set_clip_prop", {"track": track, "clip": clip + layer,
                                         "prop": "pos_x", "value": 0.5}))
        ops.append(_op("set_clip_prop", {"track": track, "clip": clip + layer,
                                         "prop": "pos_y", "value": p_y}))
        # drift the sine phase by half a wavelength across the window
        ops.append(_op("set_shape_keyframes", {"track": track, "clip": clip + layer, "keys": [
            {"t": 0.0, "points": _ribbon_path(crest, amp, phase, wl),
             "closed": True, "interp": "ease_both"},
            {"t": round(dur * 0.5, 4), "points": _ribbon_path(crest, amp, phase + math.pi / 2, wl),
             "closed": True, "interp": "ease_both"},
            {"t": dur, "points": _ribbon_path(crest, amp, phase + math.pi, wl),
             "closed": True, "interp": "ease_both"},
        ]}))
        op = round(0.9 - 0.1 * layer, 4)  # back ~0.9, front lower
        ops.append(_op("set_clip_keyframes", {
            "track": track, "clip": clip + layer, "prop": "opacity",
            "keys": [{"t": 0.0, "v": op, "interp": "ease_both"},
                     {"t": dur, "v": op, "interp": "ease_both"}],
        }))
    return ops


def _motif_butterfly(track, clip, bar, colors, rng, ctx):
    """One butterfly: teal/blue palette fill with a contrasting stroke; fast
    path-morph flutter between wing-open/closed poses (4 keys per beat, Hold
    snap at the beat), drifting ±0.05 via pos keys."""
    start, end, dur = bar["start"], bar["end"], round(bar["end"] - bar["start"], 4)
    beats = bar["beats"]
    ops = []
    ops.append(_op("add_shape", {"track": track, "start": start, "end": end,
                                 "preset": "circle"}))
    fill = _tealish(colors)
    stroke = _contrast_color(colors, fill)
    ops.append(_op("set_shape_path", {"track": track, "clip": clip,
                                      "points": _butterfly_path(1.0), "closed": True}))
    ops.append(_op("set_shape_style", {
        "track": track, "clip": clip,
        "fill_on": True, "fill_col": fill,
        "stroke_on": True, "stroke_col": stroke, "stroke_width": 0.006,
    }))
    # Cover-art scale: the butterfly is a small collage element, not a
    # canvas-filling bloom (default scale 1.0 = fit min(canvas) = huge).
    ops.append(_op("set_clip_prop", {"track": track, "clip": clip,
                                     "prop": "scale_x", "value": 0.32}))
    ops.append(_op("set_clip_prop", {"track": track, "clip": clip,
                                     "prop": "scale_y", "value": 0.32}))
    # flutter: snap open at the beat (Hold), then two quick flaps per beat
    morph = []
    for k, b in enumerate(beats):
        t0 = _clamp_t(b - start, dur)
        nxt = beats[k + 1] if k + 1 < len(beats) else end
        bl = max(0.05, nxt - b)
        morph.append({"t": t0, "points": _butterfly_path(1.0),
                      "closed": True, "interp": "hold"})
        morph.append({"t": _clamp_t(t0 + 0.25 * bl, dur), "points": _butterfly_path(0.45),
                      "closed": True, "interp": "linear"})
        morph.append({"t": _clamp_t(t0 + 0.5 * bl, dur), "points": _butterfly_path(1.0),
                      "closed": True, "interp": "linear"})
        morph.append({"t": _clamp_t(t0 + 0.75 * bl, dur), "points": _butterfly_path(0.5),
                      "closed": True, "interp": "hold"})
    ops.append(_op("set_shape_keyframes", {"track": track, "clip": clip, "keys": morph}))
    # slow drift: pos wanders ±0.05, ease_both
    sx = round(0.5 + rng.uniform(-0.05, 0.05), 4)
    sy = round(0.5 + rng.uniform(-0.05, 0.05), 4)
    ex = round(0.5 + rng.uniform(-0.05, 0.05), 4)
    ey = round(0.5 + rng.uniform(-0.05, 0.05), 4)
    for prop, s, e in (("pos_x", sx, ex), ("pos_y", sy, ey)):
        ops.append(_op("set_clip_keyframes", {
            "track": track, "clip": clip, "prop": prop,
            "keys": [{"t": 0.0, "v": s, "interp": "ease_both"},
                     {"t": dur, "v": e, "interp": "ease_both"}],
        }))
    return ops


MOTIF_FUNCTIONS: dict[str, Callable[..., list[dict]]] = {
    "bloom_ring": _motif_bloom_ring,
    "petal_burst": _motif_petal_burst,
    "star_morph": _motif_star_morph,
    "pulse_bloom": _motif_pulse_bloom,
    "orbit_petals": _motif_orbit_petals,
    "draw_on_vine": _motif_draw_on_vine,
    "slow_bloom": _motif_slow_bloom,
    "flower_scatter": _motif_flower_scatter,
    "ribbon_waves": _motif_ribbon_waves,
    "butterfly": _motif_butterfly,
}


# ── Public entry point ───────────────────────────────────────────────────────

def _snap_bars(bars: list[dict], fps: float) -> list[dict]:
    """Snap bar bounds to the project frame grid, replicating the engine's
    add_shape snapping (start rounds half away from zero, end ceils —
    engine_seams.h snap_to_frame/snap_end_to_frame) so the clip the engine
    creates has EXACTLY the duration the motifs key against. Re-snapping an
    already-snapped value is idempotent, so the engine won't shift it again.
    Interior bar ends = the next bar's snapped start: the clips tile with no
    gap and no overlap."""
    for bar in bars:
        bar["start"] = math.floor(bar["start"] * fps + 0.5) / fps
    for i in range(len(bars) - 1):
        bars[i]["end"] = bars[i + 1]["start"]
    if bars:
        bars[-1]["end"] = math.ceil(bars[-1]["end"] * fps - 1e-4) / fps
    return [b for b in bars if b["end"] - b["start"] > 1e-6]


def _expand_field(recipe: dict, section: dict, beats: list[float], rms: list[float],
                  fps: float, density: float, seed: int, colors: list[list[float]],
                  track: int, clip_base: int, track_name: str | None) -> dict:
    """Field composition — an animated Wildflower-album-cover look. Emits, in
    z-order (later on top): warm ribbon bands in the bottom fraction of the
    canvas, a grid-scattered field of flat flowers (each its own clip, all
    overlapping in time — legal for shapes), wandering butterflies, and
    kaleidoscope blooms only at energy peaks. Deterministic per seed; every
    key t is clamped to its clip's (frame-snapped) duration."""
    start, end = float(section["start"]), float(section["end"])
    s_start = math.floor(start * fps + 0.5) / fps          # engine-snapped
    s_end = math.ceil(end * fps - 1e-4) / fps              # full-section clips
    dur = round(s_end - s_start, 4)
    rng = random.Random(seed)

    warm = [_hex_to_rgba(h) for h in (recipe.get("warm_palette") or recipe.get("palette") or [])]
    if len(colors) < 2 or not warm:
        raise ValueError("field recipe needs palette (>= 2 colours) and warm_palette")
    bpb = max(1, int(recipe.get("time_signature", 4)))
    color_motion = recipe.get("color_motion") or {}
    period = max(1, int(color_motion.get("period_beats", 1)))

    rms_norm = _normalize_rms([float(x) for x in rms], start, end)
    bars = _build_bars(beats, start, end, bpb, rms_norm)
    if not bars:
        raise ValueError(f"no beats in section [{start}, {end}) — check start/end "
                         "against the beat grid")
    _snap_bars(bars, fps)
    n_bars = len(bars)
    sec_beats = [b for b in beats if start - 1e-3 <= b < end]
    diffs = [sec_beats[i + 1] - sec_beats[i] for i in range(len(sec_beats) - 1)]
    beat_len = sorted(diffs)[len(diffs) // 2] if diffs else 0.5

    # ── Layered emission ─────────────────────────────────────────────────────
    # Shape clips NEVER overlap on a track (engine-enforced), so every
    # full-section element gets its own layer track. Layers are built
    # FRONT→BACK (track index 0 = frontmost): each layer = one add_track op at
    # its final index, then that track's clip ops (clip indices restart at 0).
    base = track
    layers: list[tuple[str, int, list[dict]]] = []
    _li = 0

    def _layer(label: str, layer_ops: list[dict]) -> None:
        nonlocal _li
        layers.append((label, base + _li, layer_ops))
        _li += 1

    # ── peak selection (up front: the blooms layer only exists when a bar
    # qualifies) ─────────────────────────────────────────────────────────────
    peaks_cfg = recipe.get("peaks") or {}
    min_energy = float(peaks_cfg.get("min_energy", 0.8))
    max_blooms = max(0, int(peaks_cfg.get("max_blooms", 3)))
    bloom_motif = peaks_cfg.get("motif", "bloom_ring")
    cands = sorted((b for b in bars
                    if b["energy"] >= min_energy
                    and b["index"] >= int(peaks_cfg.get("min_bar", 0))),
                   key=lambda b: b["energy"], reverse=True)
    picked = []
    for b in cands:
        if len(picked) >= max_blooms:
            break
        if any(abs(b["index"] - p["index"]) <= 1 for p in picked):
            continue  # spread apart — no adjacent bars
        picked.append(b)
    peak_ctx = {"period": period, "fold": int(peaks_cfg.get("fold", 6)),
                "reflect": bool(peaks_cfg.get("reflect", False)),
                "section_start": s_start, "section_end": s_end, "fps": fps,
                "density": density,
                "scale_override": float(peaks_cfg.get("scale", 0.5)),
                "no_morph": bool(peaks_cfg.get("no_morph", False)),
                "flat": bool(peaks_cfg.get("flat", False))}

    # ── (front) peak blooms ──────────────────────────────────────────────────
    if picked:
        bops: list[dict] = []
        bclip = 0
        for b in picked:
            before = len(bops)
            bops.extend(MOTIF_FUNCTIONS[bloom_motif](base + _li, bclip, b,
                                                     colors, rng, peak_ctx))
            bclip += sum(1 for o in bops[before:] if o["method"] == "add_shape")
            b["motif"] = f"peak:{bloom_motif}"
        _layer("Blooms", bops)

    # ── butterflies: spots (front) + body, one layer per part ────────────────
    # Each butterfly is FIVE layers: four wing-spot accents + the smoothed
    # silhouette body, sharing scale/flutter/wander tracks.
    n_butterflies = max(0, int(recipe.get("butterflies", 0)))
    for _ in range(n_butterflies):
        b_scale = round(rng.uniform(0.28, 0.34), 4)
        fill = _tealish(colors)
        spot_fill = max(colors, key=_luminance)  # lightest palette entry
        # flutter schedule: open holds at the beat, folded (0.45) mid-beat
        sched = []
        for k, b in enumerate(sec_beats):
            t0 = _clamp_t(b - s_start, dur)
            nxt = sec_beats[k + 1] if k + 1 < len(sec_beats) else s_end
            bl = max(0.05, nxt - b)
            sched.append((t0, 1.0, "hold"))
            sched.append((_clamp_t(t0 + 0.5 * bl, dur), 0.45, "linear"))
        # wander: waypoints every 2 bars over the full frame, avoid edges
        wts = [0.0] + [_clamp_t(bars[i]["start"] - s_start, dur)
                       for i in range(2, n_bars, 2)]
        if wts[-1] < dur - 1e-3:
            wts.append(dur)
        wander = {}
        for prop in ("pos_x", "pos_y"):
            wander[prop] = [{"t": _clamp_t(wt, dur),
                             "v": round(rng.uniform(0.12, 0.88), 4),
                             "interp": "ease_both"} for wt in wts]

        def butterfly_part(ti: int, path_at, fill_col) -> list[dict]:
            pops = [_op("add_shape", {"track": ti, "start": s_start,
                                      "end": s_end, "preset": "circle"}),
                    _op("set_shape_path", {"track": ti, "clip": 0,
                                           "points": path_at(1.0),
                                           "closed": True}),
                    _op("set_shape_style", {
                        "track": ti, "clip": 0,
                        "fill_on": True, "fill_col": fill_col,
                        "stroke_on": False,  # stroke over notch cusps spikes
                    })]
            for prop in ("scale_x", "scale_y"):
                pops.append(_op("set_clip_prop", {"track": ti, "clip": 0,
                                                  "prop": prop,
                                                  "value": b_scale}))
            pops.append(_op("set_shape_keyframes", {
                "track": ti, "clip": 0,
                "keys": [{"t": t, "points": path_at(o), "closed": True,
                          "interp": it} for t, o, it in sched]}))
            for prop, keys in wander.items():
                pops.append(_op("set_clip_keyframes", {
                    "track": ti, "clip": 0, "prop": prop, "keys": keys}))
            return pops

        # si ∈ 0..3 = (forewing L, forewing R, hindwing L, hindwing R)
        for si in range(4):
            _layer(f"Butterfly spot {si + 1}",
                   butterfly_part(base + _li,
                                  lambda o, si=si: _butterfly_spots(o)[si],
                                  spot_fill))
        _layer("Butterfly", butterfly_part(base + _li, _butterfly_path, fill))

    # ── flower field: one layer per flower ───────────────────────────────────
    field_cfg = recipe.get("field") or {}
    n_flowers = max(1, int(field_cfg.get("flower_count", 30)))
    min_s = max(0.02, float(field_cfg.get("min_scale", 0.06)))
    max_s = min(0.6, float(field_cfg.get("max_scale", 0.22)))
    petals_choices = [p for p in field_cfg.get("petals", [5, 6]) if isinstance(p, int)] or [5]
    breathe = float(field_cfg.get("breathe", 0.06))
    drift = float(field_cfg.get("drift", 0.02))
    pop = 0.08
    # even colour coverage: a seeded shuffle of the palette repeated to count
    pool = (colors * (n_flowers // len(colors) + 1))[:n_flowers]
    rng.shuffle(pool)
    cols, rows = 6, 6
    cells = [(ci, ri) for ri in range(rows) for ci in range(cols)]
    rng.shuffle(cells)
    cell_w = (0.94 - 0.06) / cols
    cell_h = (0.96 - 0.04) / rows
    downbeats = [b["start"] for b in bars]
    n_pops_max = round(0.3 * len(downbeats))
    for j in range(n_flowers):
        ci, ri = cells[j]
        hx = 0.06 + (ci + 0.5) * cell_w
        hy = 0.04 + (ri + 0.5) * cell_h
        px = round(hx + rng.uniform(-0.4, 0.4) * cell_w, 4)
        py = round(hy + rng.uniform(-0.4, 0.4) * cell_h, 4)
        petals = rng.choice(petals_choices)
        scale = round(rng.uniform(min_s, max_s), 4)
        fill = pool[j]
        fti = base + _li

        fops = [_op("add_shape", {"track": fti, "start": s_start, "end": s_end,
                                  "preset": "circle"}),
                _op("set_shape_path", {"track": fti, "clip": 0,
                                       "points": _flower_path(petals),
                                       "closed": True}),
                # Flat fill only — the polar-rose petal notches are cusps, and
                # a stroke over them miter-spikes into fuzz (cover = flat).
                _op("set_shape_style", {"track": fti, "clip": 0,
                                        "fill_on": True, "fill_col": fill,
                                        "stroke_on": False})]

        # scale track: bloom-in (0 → final over ~0.3s, staggered across the
        # first ~2 bars), then ±breathe ease_both, then kick pops on a seeded
        # ~30% subset of downbeats. First key at t=0: content from the start.
        delay = round(min(rng.uniform(0.0, 2.0 * beat_len * bpb), dur * 0.6), 4)
        bloom_end = round(min(delay + 0.3, dur), 4)
        pb = rng.choice([2, 3, 4])           # breathe period in beats
        bper = pb * beat_len
        n_breathe = 2 * (dur - bloom_end) / bper if bper > 0 else 0.0
        if n_breathe + 3 + 3 * n_pops_max > 36:  # keep total keys < 40
            pb = 4
            bper = 4 * beat_len
            n_breathe = 2 * (dur - bloom_end) / bper if bper > 0 else 0.0
        start_high = rng.random() < 0.5
        hi = round(scale + breathe, 4)
        lo = round(scale - breathe, 4)
        half = bper / 2.0 if bper > 0 else dur
        track_scale: dict[float, tuple[float, str]] = {}
        track_scale[0.0] = (0.0, "ease_out" if delay < 0.01 else "hold")
        if delay >= 0.01:
            track_scale[delay] = (0.0, "ease_out")
        track_scale[bloom_end] = (scale, "ease_out")
        t = bloom_end
        k = 0
        while t < dur - 1e-4:
            t += half
            if t >= dur - 1e-4:
                break
            v = hi if (k % 2 == 0) == start_high else lo
            track_scale[round(t, 4)] = (v, "ease_both")
            k += 1

        def _breathe_value(at: float) -> float:
            if at < bloom_end:
                return 0.0
            idx = int(math.floor((at - bloom_end) / half)) if half > 0 else 0
            return hi if (idx % 2 == 0) == start_high else lo

        n_pops = 0
        for tb in downbeats:
            if n_pops >= n_pops_max:
                break
            if rng.random() >= 0.3:
                continue
            tr = round(tb - s_start, 4)
            if tr < 0.0 or tr > dur:
                continue
            bval = _breathe_value(tr)
            track_scale[tr] = (bval, "ease_out")
            track_scale[_clamp_t(tr + 0.06, dur)] = (round(bval * (1.0 + pop), 4), "ease_out")
            track_scale[_clamp_t(tr + 0.12, dur)] = (bval, "ease_out")
            n_pops += 1
        track_scale[dur] = (_breathe_value(dur), "ease_both")
        sk = [{"t": t, "v": v, "interp": it} for t, (v, it) in sorted(track_scale.items())]
        for prop in ("scale_x", "scale_y"):
            fops.append(_op("set_clip_keyframes", {"track": fti, "clip": 0,
                                                   "prop": prop, "keys": sk}))

        # rotation drift ±8° over the section, direction seeded
        rot = round(8.0 * rng.choice([-1.0, 1.0]), 2)
        fops.append(_op("set_clip_keyframes", {
            "track": fti, "clip": 0, "prop": "rotation",
            "keys": [{"t": 0.0, "v": rot, "interp": "ease_both"},
                     {"t": round(dur * 0.5, 4), "v": -rot, "interp": "ease_both"},
                     {"t": dur, "v": rot, "interp": "ease_both"}],
        }))
        # position drift ±drift around home, 4 keys ease_both
        for prop, home in (("pos_x", px), ("pos_y", py)):
            keys = [{"t": 0.0, "v": home, "interp": "ease_both"}]
            for f in (0.33, 0.66):
                keys.append({"t": _clamp_t(dur * f, dur),
                             "v": round(home + rng.uniform(-drift, drift), 4),
                             "interp": "ease_both"})
            keys.append({"t": dur,
                         "v": round(home + rng.uniform(-drift, drift), 4),
                         "interp": "ease_both"})
            fops.append(_op("set_clip_keyframes", {"track": fti, "clip": 0,
                                                   "prop": prop, "keys": keys}))
        _layer(f"Flower {j + 1:02d}", fops)

    # ── ribbons: one layer per band ──────────────────────────────────────────
    # Geometry: the path's sine midline sits at local y=0.3 (body extends to
    # 1.5), and pos_y/scale_y place that midline at canvas fraction c_i — the
    # crest is applied ONCE. Canvas y of a local point = p_y*H + (y-0.5)*s_y*W,
    # so p_y = c_i + 0.2*s_y*(W/H).
    ribbons_cfg = recipe.get("ribbons") or {}
    n_ribbons = max(1, int(ribbons_cfg.get("count", 4)))
    bottom = max(0.1, min(0.9, float(ribbons_cfg.get("bottom_fraction", 0.35))))
    fill_to = ribbons_cfg.get("fill_to")       # final top-of-fill (screen fraction)
    rise = bool(ribbons_cfg.get("rise", False))  # bands rise from below, fluid-style
    aspect = float(recipe.get("canvas_aspect", 0.5625))  # W/H, vertical default
    s_y = 0.8
    for i in range(n_ribbons):
        if fill_to is not None:
            crest = float(fill_to) + (i + 0.5) * (0.95 - float(fill_to)) / n_ribbons
        else:
            crest = 1.0 - bottom + (i + 0.5) * bottom / n_ribbons
        amp = round(rng.uniform(0.10, 0.18), 4)
        wl = round(rng.uniform(0.9, 1.4), 3)
        phase = round(rng.uniform(0.0, 2.0 * math.pi), 4)
        p_y = round(crest + 0.2 * s_y * aspect, 4)
        fill = warm[i % len(warm)]
        rti = base + _li
        rops = [_op("add_shape", {"track": rti, "start": s_start, "end": s_end,
                                  "preset": "circle"}),
                _op("set_shape_path", {"track": rti, "clip": 0,
                                       "points": _ribbon_path(0.3, amp, phase, wl),
                                       "closed": True}),
                _op("set_shape_style", {"track": rti, "clip": 0,
                                        "fill_on": True, "fill_col": fill,
                                        "stroke_on": False}),
                _op("set_clip_prop", {"track": rti, "clip": 0,
                                      "prop": "scale_x", "value": 2.2}),
                _op("set_clip_prop", {"track": rti, "clip": 0,
                                      "prop": "scale_y", "value": s_y}),
                _op("set_clip_prop", {"track": rti, "clip": 0,
                                      "prop": "pos_x", "value": 0.5})]
        if rise:
            # Fluid fill: the band starts with its midline just below the
            # frame and rises to its final stratum; ease_both = slow start,
            # steady rise, soft settle. Front bands settle slightly later.
            p_y_start = round(1.10 + 0.2 * s_y * aspect, 4)
            settle = round(dur * (0.72 + 0.22 * (i + 1) / n_ribbons), 4)
            rops.append(_op("set_clip_keyframes", {
                "track": rti, "clip": 0, "prop": "pos_y",
                "keys": [{"t": 0.0, "v": p_y_start, "interp": "ease_both"},
                         {"t": _clamp_t(settle, dur), "v": p_y,
                          "interp": "ease_both"}],
            }))
        else:
            rops.append(_op("set_clip_prop", {"track": rti, "clip": 0,
                                              "prop": "pos_y", "value": p_y}))
        # phase drifts every 2 bars (ease_both smooth drift)
        morph = [{"t": 0.0, "points": _ribbon_path(0.3, amp, phase, wl),
                  "closed": True, "interp": "ease_both"}]
        ph = phase
        for bi in range(2, n_bars, 2):
            ph = round(ph + math.pi / 2, 4)
            morph.append({"t": _clamp_t(bars[bi]["start"] - s_start, dur),
                          "points": _ribbon_path(0.3, amp, ph, wl),
                          "closed": True, "interp": "ease_both"})
        ph = round(ph + math.pi / 2, 4)
        morph.append({"t": dur, "points": _ribbon_path(0.3, amp, ph, wl),
                      "closed": True, "interp": "ease_both"})
        rops.append(_op("set_shape_keyframes", {"track": rti, "clip": 0,
                                                "keys": morph}))
        opac = round(0.85 - 0.05 * i, 4)  # back most opaque → front lightest
        rops.append(_op("set_clip_keyframes", {
            "track": rti, "clip": 0, "prop": "opacity",
            "keys": [{"t": 0.0, "v": opac, "interp": "ease_both"},
                     {"t": dur, "v": opac, "interp": "ease_both"}],
        }))
        _layer(f"Ribbon {i + 1}", rops)

    # ── (back) backdrop: flat full-canvas colour ─────────────────────────────
    backdrop = recipe.get("backdrop")
    if backdrop:
        dti = base + _li
        _layer("Backdrop", [
            _op("add_shape", {"track": dti, "start": s_start, "end": s_end,
                              "preset": "square"}),
            _op("set_shape_style", {"track": dti, "clip": 0,
                                    "fill_on": True,
                                    "fill_col": _hex_to_rgba(backdrop),
                                    "stroke_on": False}),
            _op("set_clip_prop", {"track": dti, "clip": 0,
                                  "prop": "pos_x", "value": 0.5}),
            _op("set_clip_prop", {"track": dti, "clip": 0,
                                  "prop": "pos_y", "value": 0.5}),
            _op("set_clip_prop", {"track": dti, "clip": 0,
                                  "prop": "scale_x", "value": 1.15}),
            _op("set_clip_prop", {"track": dti, "clip": 0,
                                  "prop": "scale_y",
                                  "value": round(1.15 / float(recipe.get("canvas_aspect", 0.5625)), 4)}),
        ])

    # ── assemble: add_track per layer (at its final index), then its ops ─────
    ops: list[dict] = []
    track_names: list[str] = []
    for label, ti, layer_ops in layers:
        name = f"{track_name or 'Field'} · {label}"
        track_names.append(name)
        ops.append(_op("add_track", {"name": name, "position": ti}))
        ops.extend(layer_ops)

    # ── summary ──────────────────────────────────────────────────────────────
    bar_map_lines = [
        f"{recipe.get('id', 'recipe')} · field composition · section "
        f"{s_start:g}–{s_end:g}s · {n_bars} bars · {len(layers)} layer tracks",
        f"ribbons: {n_ribbons} wavy bands "
        + (f"rising to fill {float(fill_to):.0%}→95% of frame" if fill_to is not None
           else f"in the bottom {bottom:.0%}")
        + " (warm palette)",
        f"flowers: {n_flowers} flat flowers (grid {cols}x{rows}, bloom-in staggered "
        f"over the first ~2 bars)",
        f"butterflies: {n_butterflies} (5 layers each: body + 4 wing spots)",
    ]
    if picked:
        bar_map_lines.append("peak blooms: " + ", ".join(
            f"bar {b['index'] + 1} (energy {b['energy']:.2f}) → {bloom_motif}"
            for b in picked))
    else:
        bar_map_lines.append("peak blooms: none (no bar reached energy >= "
                             f"{min_energy:g})")

    return {"ops": ops, "bars": bars, "bar_map": "\n".join(bar_map_lines),
            "fx": None, "tracks": track_names}


def expand(recipe: dict, section: dict, beats: list[float], rms: list[float],
           fps: float = 30.0, density: float = 0.7, seed: int = 0,
           palette: list[str] | None = None, track: int = 0,
           clip_base: int = 0, track_name: str | None = None) -> dict:
    """Expand a recipe over a timeline section into replayable IPC ops.

    Args:
        recipe: parsed recipe dict (see mcp_server/recipes/*.json).
        section: {"start": float, "end": float} — absolute timeline seconds.
        beats: absolute beat timestamps (seconds).
        rms: per-second audio energy 0..1.
        fps: project framerate (used to space snapshots; kept for symmetry).
        density: 0..1 — fraction of bars that receive a motif.
        seed: deterministic RNG seed.
        palette: optional hex-colour override for recipe.palette.
        track: integer index of the target track the shapes land on (the FX
            track, when present, is inserted directly above it).
        clip_base: number of clips already on the target track — the first
            motif's clip gets this index (ops replay in order inside one batch).
        track_name: target track name, used to name the optional FX track.

    Returns:
        {"ops": [{method, params}...], "bars": [...], "bar_map": str,
         "fx": {...} | None}. Raises ValueError on bad input (no beats, no bars,
        empty palette, unknown motif names in the recipe).
    """
    if not (0.0 <= density <= 1.0):
        raise ValueError(f"density must be 0..1, got {density}")
    start, end = float(section["start"]), float(section["end"])
    if end <= start:
        raise ValueError(f"section end ({end}) must be after start ({start})")
    beats = sorted(float(b) for b in beats)
    if not beats:
        raise ValueError("no beats provided — run analyze_audio on the project audio first")

    colors = [_hex_to_rgba(h) for h in (palette or recipe.get("palette") or [])]
    if len(colors) < 2:
        raise ValueError("palette needs at least 2 colours (recipe.palette or palette override)")

    sym = recipe.get("symmetry") or {}
    try:
        fold = max(1, int(sym.get("fold", 1)))
    except (TypeError, ValueError):
        raise ValueError("recipe.symmetry.fold must be an integer >= 1") from None
    reflect = bool(sym.get("reflect", False))

    try:
        bpb = max(1, int(recipe.get("time_signature", 4)))
    except (TypeError, ValueError):
        raise ValueError("recipe.time_signature must be an integer") from None

    color_motion = recipe.get("color_motion") or {}
    period = max(1, int(color_motion.get("period_beats", 1)))

    unknown = sorted({m for bucket in (recipe.get("motifs") or {}).values()
                      for m in bucket if m not in MOTIF_VOCABULARY})
    peaks_m = (recipe.get("peaks") or {}).get("motif")
    if peaks_m and peaks_m not in MOTIF_VOCABULARY:
        unknown.append(peaks_m)
    if unknown:
        raise ValueError(f"unknown motif name(s) {sorted(set(unknown))} in recipe — "
                         f"vocabulary: {', '.join(MOTIF_VOCABULARY)}")

    if recipe.get("composition") == "field":
        return _expand_field(recipe, {"start": start, "end": end}, beats,
                             [float(x) for x in rms], fps, density, seed,
                             colors, track, clip_base, track_name)

    rng = random.Random(seed)
    rms_norm = _normalize_rms([float(x) for x in rms], start, end)
    bars = _build_bars(beats, start, end, bpb, rms_norm)
    if not bars:
        raise ValueError(f"no beats in section [{start}, {end}) — check start/end "
                         "against the beat grid")
    _snap_bars(bars, fps)

    ctx = {"period": period, "fold": fold, "reflect": reflect,
           "section_start": start, "section_end": end, "fps": fps,
           "density": density}

    # Energy routing: bucket each bar, pick a seeded motif, avoid repeats.
    prev: str | None = None
    for bar in bars:
        energy = bar["energy"]
        bucket = "high" if energy > 0.66 else ("mid" if energy > 0.33 else "low")
        motif = _pick_motif(recipe, bucket, prev, rng)
        if motif is None:
            continue
        if rng.random() >= density:
            continue
        bar["motif"] = motif
        prev = motif

    # Emit ops: one clip per planned motif, indices sequential from clip_base.
    # The engine hands back consecutive clip indices as add_shape ops replay
    # inside the batch, so the cursor advances by the clips each motif creates
    # (multi-clip motifs like orbit_petals / flower_scatter emit several).
    ops: list[dict] = []
    clip = clip_base
    for bar in (b for b in bars if b["motif"]):
        fn = MOTIF_FUNCTIONS[bar["motif"]]
        before = len(ops)
        ops.extend(fn(track=track, clip=clip, bar=bar, colors=colors, rng=rng, ctx=ctx))
        clip += sum(1 for o in ops[before:] if o["method"] == "add_shape")

    # Fx layer: one brick over the whole section on its own track, inserted
    # directly above the shape track so it washes everything below (including
    # the new shapes). Appended AFTER the shape ops so track indices hold.
    fx_info = None
    fx_layer = recipe.get("fx_layer") or {}
    fx_id = fx_layer.get("fx_id")
    if fx_id:
        amount = float(fx_layer.get("amount", 1.0))
        fx_ti = track  # inserted at the shape track's position → lands above it
        ops.append(_op("add_track", {"name": f"{track_name or 'Section'} FX",
                                     "position": fx_ti}))
        ops.append(_op("add_effect_brick", {
            "track": fx_ti, "fx_type": fx_id,
            "start": round(start, 4), "end": round(end, 4),
            "params": {"amount": round(amount, 4)},
        }))
        fx_info = {"fx_id": fx_id, "amount": round(amount, 4), "track": fx_ti}

    # Human-readable summary of what was planned.
    bar_map_lines = [
        f"{recipe.get('id', 'recipe')} · section {start:g}–{end:g}s · "
        f"{len(bars)} bars · fold {fold} · density {density:g}",
    ]
    for bar in bars:
        motif = bar["motif"] or "—"
        level = ("HIGH" if bar["energy"] > 0.66
                 else "mid" if bar["energy"] > 0.33 else "low")
        bar_map_lines.append(
            f"bar {bar['index'] + 1}  {bar['start']:7.2f}–{bar['end']:7.2f}s  "
            f"energy {bar['energy']:.2f} {level:<4} → {motif}")
    skipped = [bar for bar in bars if not bar["motif"]]
    if skipped:
        bar_map_lines.append(f"({len(skipped)} bar(s) skipped by density "
                             f"{density:g}: bars "
                             + ", ".join(str(b["index"] + 1) for b in skipped) + ")")
    if fx_info:
        bar_map_lines.append(f"fx layer: {fx_info['fx_id']} amount "
                             f"{fx_info['amount']:g} over the section")

    return {"ops": ops, "bars": bars, "bar_map": "\n".join(bar_map_lines),
            "fx": fx_info}


def _clamp_t(t: float, dur: float) -> float:
    """Keyframe time relative to a clip, clamped to [0, clip duration]."""
    return round(max(0.0, min(t, dur)), 4)
