#!/usr/bin/env python3
"""build_bim_video — direct the 'Because I'm Me' wildflower section (0:36-1:04)
via IPC. Photo cutout flowers + real butterfly footage, beat-synced, one undo
batch per stage group, no overlapping bricks anywhere.

Layout (z front→back):
  Kaleidoscope FX (peak bars) · 2 butterfly flights · N flower cutout layers ·
  black backdrop
"""
import json
import math
import random
import sys
import time
from pathlib import Path

sys.path.insert(0, "/home/alexis/dev/pop-maker-studio/mcp_server")
import server  # noqa: E402

AUDIO = "/home/alexis/dev/pop-maker-studio/build/bim/because_im_me.flac"
ASSETS = Path("/home/alexis/Videos/Pop Maker Studio Projects/wildflower_cover")
CUT = ASSETS / "cutouts"
SEC_START, SEC_END = 36.0, 64.0
SEED = 11

FPS = 30.0
snap_s = lambda t: math.floor(t * FPS + 0.5) / FPS      # engine: round
snap_e = lambda t: math.ceil(t * FPS - 1e-4) / FPS      # engine: ceil

call = server._call


def ribbon_pts(y_top, amp, phase, wavelength, y_bottom=1.5, n=24, w=0.004):
    """Closed wavy band: sine top edge, body extended below the canvas."""
    pts = []
    for i in range(n + 1):
        x = i / n
        y = y_top + amp * math.sin(2.0 * math.pi * x / wavelength + phase)
        pts.append({"x": round(x, 4), "y": round(y, 4), "w": w})
    pts.append({"x": 1.0, "y": y_bottom, "w": w})
    pts.append({"x": 0.0, "y": y_bottom, "w": w})
    return pts


def beats_and_bars():
    s = call("get_beats", {})
    beats = s.get("beats") or []
    a = call("get_audio_analysis", {})
    if not beats and a.get("beats"):
        beats = a["beats"]  # project state only commits beats for timeline audio
    beats = [b for b in beats if SEC_START - 1e-3 <= b < SEC_END]
    rms = a.get("rms") or []
    bars = []
    for i in range(0, len(beats) - 3, 4):
        b0, b4 = beats[i], beats[i + 4] if i + 4 < len(beats) else SEC_END
        e = [rms[int(math.floor(x))] for x in (b0, b4) if 0 <= int(math.floor(x)) < len(rms)]
        bars.append({"start": b0, "end": min(b4, SEC_END),
                     "beats": beats[i:i + 4], "i": i // 4})
    return beats, bars


def kf(prop, keys):
    return {"prop": prop, "keys": keys}


def main() -> None:
    rng = random.Random(SEED)
    flowers = sorted(CUT.glob("*.png"))
    assert flowers, "no cutout PNGs — run precut_flowers.py first"
    # hero first: the red anemone opens the video (bold read on first beat)
    flowers.sort(key=lambda f: 0 if "anemonecoronaria" in f.name else 1)
    print(f"{len(flowers)} flower cutouts, opener: {flowers[0].name}")

    call("new_project", {"force": True})
    call("set_audio_path", {"path": AUDIO})
    call("set_format", {"format": "vertical"})
    a = call("get_audio_analysis", {})
    if a.get("status") != "done":
        call("analyze_audio", {"path": AUDIO})
        a = call("get_audio_analysis", {})
    beats, bars = beats_and_bars()
    print(f"{len(beats)} beats, {len(bars)} bars")
    bar_at = lambda idx: bars[min(idx, len(bars) - 1)]

    ti = 0
    layers = []  # (name, position) — track index = creation order (pos = ti++)

    def new_track(name):
        nonlocal ti
        call("add_track", {"name": name, "position": ti})
        ti += 1
        return ti - 1

    # ── Kaleidoscope FX track (top, frontmost) — stepped psychedelic ramp.
    # Every kaleidoscope param is keyframed (engine routes fx params to the
    # chain sub-clip): amount ratchets 0→1 in visible steps over the 2 bars
    # before the peak, holds full-blown through it, steps back down. Segments
    # multiply with it, rotation spins continuously, zoom swells at the peak.
    fx_ti = new_track("BIM · Kaleidoscope")
    beat_len = bars[0]["beats"][1] - bars[0]["beats"][0] if len(bars[0]["beats"]) > 1 else 0.626
    peak_a, peak_b = bar_at(9), bar_at(10)
    k_start = snap_s(peak_a["start"] - 2 * beat_len)
    k_end = snap_e(peak_b["end"] + 2 * beat_len)
    call("add_effect_brick", {"track": fx_ti, "fx_type": "kaleidoscope",
                              "start": k_start, "end": k_end,
                              "params": {"amount": 0.0}})
    kdur = k_end - k_start
    beat_at = lambda i: round(i * beat_len, 4)
    # stepped amount: hold interp = visible ratchet, one step per beat
    steps = [0.0, 0.0, 0.25, 0.45, 0.65, 0.85, 1.0]
    amt_keys = [{"t": beat_at(i), "v": steps[min(i, len(steps) - 1)],
                 "interp": "hold"} for i in range(7)]
    full_end = round(kdur - 2 * beat_len, 4)
    amt_keys += [{"t": full_end, "v": 1.0, "interp": "hold"},
                 {"t": round(full_end + beat_len, 4), "v": 0.4, "interp": "hold"},
                 {"t": kdur, "v": 0.0, "interp": "ease_in"}]
    call("set_clip_keyframes", {"track": fx_ti, "clip": 0, **kf("amount", amt_keys)})
    # segments multiply in steps as it deepens
    seg_keys = [{"t": beat_at(0), "v": 4.0, "interp": "hold"},
                {"t": beat_at(3), "v": 6.0, "interp": "hold"},
                {"t": beat_at(5), "v": 8.0, "interp": "hold"},
                {"t": beat_at(7), "v": 12.0, "interp": "hold"},
                {"t": full_end, "v": 6.0, "interp": "hold"}]
    call("set_clip_keyframes", {"track": fx_ti, "clip": 0, **kf("segments", seg_keys)})
    # rotation: one slow full turn across the window
    call("set_clip_keyframes", {"track": fx_ti, "clip": 0, **kf("rotation", [
        {"t": 0.0, "v": 0.0, "interp": "linear"},
        {"t": kdur, "v": 360.0, "interp": "linear"}])})
    # zoom: swell at full-blown, back out
    call("set_clip_keyframes", {"track": fx_ti, "clip": 0, **kf("zoom", [
        {"t": 0.0, "v": 1.0, "interp": "ease_both"},
        {"t": beat_at(6), "v": 1.35, "interp": "ease_both"},
        {"t": full_end, "v": 1.35, "interp": "ease_both"},
        {"t": kdur, "v": 1.0, "interp": "ease_both"}])})

    # ── Butterflies: blue L→R from bar 0, orange R→L from bar 6 ─────────────
    for name, mp4, t_in, x0, x1, hue in (
            ("Butterfly blue", "butterfly_blue.mp4", 0, -0.15, 1.15, 0.0),
            ("Butterfly orange", "butterfly_orange.mp4", 6, 1.15, -0.15, 0.0)):
        b_ti = new_track(f"BIM · {name}")
        t0, t1 = snap_s(bar_at(t_in)["start"]), snap_e(SEC_END)
        call("add_clip", {"track": b_ti, "type": "video",
                          "text": str(ASSETS / mp4), "start": t0, "end": t1})
        # wait for proxy, then ML-cut the white bg
        for _ in range(90):
            time.sleep(2)
            try:
                call("start_bg_remove", {"track": b_ti, "clip": 0})
                break
            except ValueError as e:
                if "proxy not ready" not in str(e):
                    raise
        for _ in range(150):
            time.sleep(2)
            st = call("get_bg_remove_status", {"track": b_ti, "clip": 0})
            if st.get("status") in ("ready", "error"):
                break
        print(f"  {name} mask: {st.get('status')}")
        if hue:
            call("set_clip_prop", {"track": b_ti, "clip": 0,
                                   "prop": "grade_hue", "value": hue})
        # flight: waypoints with bobbing y, scale 0.34
        n_wp = 7
        dur = t1 - t0
        for prop, vs in (
                ("pos_x", [x0 + (x1 - x0) * k / (n_wp - 1) for k in range(n_wp)]),
                ("pos_y", [0.42 + 0.10 * math.sin(k * 1.7 + (0 if x0 < 0.5 else 2))
                           for k in range(n_wp)])):
            call("set_clip_keyframes", {"track": b_ti, "clip": 0, **kf(prop, [
                {"t": round(dur * k / (n_wp - 1), 4), "v": round(vs[k], 4),
                 "interp": "ease_both"} for k in range(n_wp)])})
        for prop in ("scale_x", "scale_y"):
            call("set_clip_prop", {"track": b_ti, "clip": 0,
                                   "prop": prop, "value": 0.34})

    # ── Flower layers ────────────────────────────────────────────────────────
    # STRICT cell containment: 5x5 grid, one flower per cell, jitter and scale
    # capped so no flower's bbox can ever touch a neighbour's — no flower ever
    # covers another flower. Bloom-in staggered (lull → lift), beat pops after.
    n = len(flowers)
    gcols, grows = 5, 5
    cells = [(c, r) for r in range(grows) for c in range(gcols)]
    rng.shuffle(cells)
    cw = 0.90 / gcols          # 0.18 of canvas width per cell
    chh = 0.90 / grows         # 0.18 of canvas height per cell
    max_s = round(cw * 0.85, 4)          # x is the binding constraint
    sched = ([(b, 1.0) for b in (0, 1, 2)] +            # lull: 3 flowers
             [(3, 1.0)] * 6 +                           # lift: 6 more
             [(b, 1.0) for b in (4, 5, 6, 7, 8, 9)][: max(0, n - 9)])
    for j, fp in enumerate(flowers):
        bar_idx, _ = sched[j % len(sched)]
        gc, gr = cells[j % len(cells)]
        px = round(0.05 + (gc + 0.5) * cw + rng.uniform(-0.22, 0.22) * cw, 4)
        py = round(0.05 + (gr + 0.5) * chh + rng.uniform(-0.22, 0.22) * chh, 4)
        # first flower blooms on the section's first beat — never a cold open
        bloom_beat = (bar_at(0)["beats"][0] if j == 0
                      else rng.choice(bar_at(bar_idx)["beats"]))
        scale = round(max_s * rng.uniform(0.80, 1.0), 4)
        rot_dir = rng.choice([-1.0, 1.0])
        f_ti = new_track(f"BIM · Flower {j + 1:02d}")
        call("add_clip", {"track": f_ti, "type": "video", "text": str(fp),
                          "start": SEC_START, "end": SEC_END})
        t_rel = lambda tb: round(tb - SEC_START, 4)
        dur = SEC_END - SEC_START
        # scale: hold 0 until the bloom beat, pop to full, then beat pops
        sk = [{"t": 0.0, "v": 0.0, "interp": "hold"},
              {"t": t_rel(bloom_beat), "v": 0.0, "interp": "ease_out"},
              {"t": t_rel(bloom_beat + 0.25), "v": scale, "interp": "ease_out"}]
        for b in beats:
            if b <= bloom_beat + 0.3 or rng.random() >= 0.35:
                continue
            tr = t_rel(b)
            sk += [{"t": tr, "v": scale, "interp": "ease_out"},
                   {"t": round(tr + 0.08, 4), "v": round(scale * 1.07, 4),
                    "interp": "ease_out"},
                   {"t": round(tr + 0.18, 4), "v": scale, "interp": "ease_out"}]
        sk.append({"t": dur, "v": scale, "interp": "ease_both"})
        for prop in ("scale_x", "scale_y"):
            call("set_clip_keyframes", {"track": f_ti, "clip": 0,
                                        **kf(prop, sk)})
        call("set_clip_keyframes", {"track": f_ti, "clip": 0, **kf("rotation", [
            {"t": 0.0, "v": round(6 * rot_dir, 2), "interp": "ease_both"},
            {"t": round(dur / 2, 4), "v": round(-6 * rot_dir, 2),
             "interp": "ease_both"},
            {"t": dur, "v": round(6 * rot_dir, 2), "interp": "ease_both"}])})
        for prop, home in (("pos_x", px), ("pos_y", py)):
            call("set_clip_keyframes", {"track": f_ti, "clip": 0, **kf(prop, [
                {"t": 0.0, "v": home, "interp": "ease_both"},
                {"t": round(dur * 0.4, 4),
                 "v": round(home + rng.uniform(-0.02, 0.02), 4),
                 "interp": "ease_both"},
                {"t": dur, "v": round(home + rng.uniform(-0.02, 0.02), 4),
                 "interp": "ease_both"}])})
    print(f"  {n} flower layers (strict grid, no canvas overlap)")

    # ── Ribbons: 5 warm strata rising like a fluid over the white backdrop,
    # filling the frame by the kaleidoscope peak (settle ~bar 10). Behind the
    # flowers (deeper tracks), in front of the backdrop. ──────────────────────
    WARM = [[0.776, 0.153, 0.118, 1.0],   # C62828
            [0.910, 0.267, 0.180, 1.0],   # E8442E
            [0.941, 0.482, 0.122, 1.0],   # F07B1F
            [0.949, 0.718, 0.020, 1.0],   # F2B705
            [0.961, 0.902, 0.773, 1.0]]   # F5E6C8
    ASPECT = 0.5625     # W/H vertical
    S_Y = 0.8           # band vertical scale (ribbon body extends below frame)
    dur = SEC_END - SEC_START
    for i in range(5):
        crest = 0.06 + (i + 0.5) * (0.92 - 0.06) / 5   # final midline fraction
        amp = round(rng.uniform(0.10, 0.16), 4)
        wl = round(rng.uniform(0.9, 1.3), 3)
        phase = round(rng.uniform(0.0, 2.0 * math.pi), 4)
        p_y_final = round(crest + 0.2 * S_Y * ASPECT, 4)
        p_y_start = round(1.10 + 0.2 * S_Y * ASPECT, 4)
        settle = round(dur * (0.70 + 0.22 * (i + 1) / 5), 4)
        r_ti = new_track(f"BIM · Ribbon {i + 1}")
        call("add_shape", {"track": r_ti, "start": SEC_START, "end": SEC_END,
                           "preset": "circle"})
        call("set_shape_path", {"track": r_ti, "clip": 0,
                                "points": ribbon_pts(0.3, amp, phase, wl),
                                "closed": True})
        call("set_shape_style", {"track": r_ti, "clip": 0, "fill_on": True,
                                 "fill_col": WARM[i], "stroke_on": False})
        for prop, v in (("scale_x", 2.2), ("scale_y", S_Y), ("pos_x", 0.5)):
            call("set_clip_prop", {"track": r_ti, "clip": 0, "prop": prop,
                                   "value": v})
        call("set_clip_keyframes", {"track": r_ti, "clip": 0, **kf("pos_y", [
            {"t": 0.0, "v": p_y_start, "interp": "ease_both"},
            {"t": settle, "v": p_y_final, "interp": "ease_both"}])})
        # wave phase drifts continuously (fluid feel)
        n_ph = 6
        call("set_shape_keyframes", {"track": r_ti, "clip": 0, "keys": [
            {"t": round(dur * k / n_ph, 4),
             "points": ribbon_pts(0.3, amp, round(phase + k * math.pi / 3, 4), wl),
             "closed": True, "interp": "ease_both"} for k in range(n_ph + 1)]})
    print("  5 rising ribbon strata")

    # ── Backdrop (back): WHITE — the rainbow rises over it, colouring it in ──
    d_ti = new_track("BIM · Backdrop")
    call("add_shape", {"track": d_ti, "start": SEC_START, "end": SEC_END,
                       "preset": "square"})
    call("set_shape_style", {"track": d_ti, "clip": 0, "fill_on": True,
                             "fill_col": [1.0, 1.0, 1.0, 1.0],
                             "stroke_on": False})
    for prop, v in (("pos_x", 0.5), ("pos_y", 0.5), ("scale_x", 1.15),
                    ("scale_y", 2.05)):
        call("set_clip_prop", {"track": d_ti, "clip": 0, "prop": prop,
                               "value": v})

    call("save_project", {"path": "/home/alexis/dev/pop-maker-studio/build/bim/because_im_me.pms"})
    print("saved. tracks:", ti)


if __name__ == "__main__":
    main()
