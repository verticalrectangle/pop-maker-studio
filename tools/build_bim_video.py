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

    # ── Kaleidoscope FX track (top, frontmost) — peak bars 9-10 (0-indexed).
    # Hard cut in/out (chain-brick amounts aren't keyframable; cuts fit the
    # edit style anyway).
    fx_ti = new_track("BIM · Kaleidoscope")
    peak_a, peak_b = bar_at(9), bar_at(10)
    call("add_effect_brick", {"track": fx_ti, "fx_type": "kaleidoscope",
                              "start": peak_a["start"], "end": peak_b["end"],
                              "params": {"amount": 0.5}})

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
    # Bloom schedule: 3 during the lull (bars 0-2), 5 at the lift (bar 3),
    # the rest through bars 4-8. Each flower: bloom-in scale pop on a beat,
    # beat pops after, rotation + pos drift. One track per flower.
    n = len(flowers)
    sched = ([(b, 0.35) for b in (0, 1, 2)] +           # lull: 3, bigger
             [(3, 0.26)] * 5 +                          # lift: 5
             [(b, 0.22) for b in (4, 5, 6, 7, 8)][: max(0, n - 8)])
    # jittered grid placement — even full-frame coverage (uniform random
    # clusters; the cover look is a SCATTERED field)
    gcols, grows = 3, 4
    cells = [(c, r) for r in range(grows) for c in range(gcols)]
    rng.shuffle(cells)
    cw, ch = 0.8 / gcols, 0.72 / grows
    for j, fp in enumerate(flowers):
        bar_idx, base_s = sched[j % len(sched)]
        gc, gr = cells[j % len(cells)]
        px = round(0.10 + (gc + 0.5) * cw + rng.uniform(-0.3, 0.3) * cw, 3)
        py = round(0.14 + (gr + 0.5) * ch + rng.uniform(-0.3, 0.3) * ch, 3)
        # first flower blooms on the section's first beat — never a cold open
        bloom_beat = (bar_at(0)["beats"][0] if j == 0
                      else rng.choice(bar_at(bar_idx)["beats"]))
        base_s = round(base_s * 1.25, 4)  # larger heroes — fill the frame
        scale = round(base_s * rng.uniform(0.85, 1.2), 4)
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
    print(f"  {n} flower layers")

    # ── Backdrop (back) ──────────────────────────────────────────────────────
    d_ti = new_track("BIM · Backdrop")
    call("add_shape", {"track": d_ti, "start": SEC_START, "end": SEC_END,
                       "preset": "square"})
    call("set_shape_style", {"track": d_ti, "clip": 0, "fill_on": True,
                             "fill_col": [0.02, 0.01, 0.04, 1.0],
                             "stroke_on": False})
    for prop, v in (("pos_x", 0.5), ("pos_y", 0.5), ("scale_x", 1.15),
                    ("scale_y", 2.05)):
        call("set_clip_prop", {"track": d_ti, "clip": 0, "prop": prop,
                               "value": v})

    call("save_project", {"path": "/home/alexis/dev/pop-maker-studio/build/bim/because_im_me.pms"})
    print("saved. tracks:", ti)


if __name__ == "__main__":
    main()
