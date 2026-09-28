#!/usr/bin/env python3
"""Standalone parity check for audio analysis v2 (th/audio acceptance).

Runs the compiled `analysis-parity` tool (which drives audio_analysis_run
in-process) on clip.wav with the 7 LINES lyrics, once with injected htdemucs
stems and once with the no-stems fallback, then compares against
~/Projects/seen-and-not-seen/public/timeline.json:

  beats ±20 ms, same downbeat phase, per kind >=85% of reference hits with
  s>=0.5 matched within 30 ms, >=90% word starts within 80 ms.

Usage: tools/analysis_parity.py [--binary PATH] [--clip PATH] [--ref PATH]
                                [--stems DIR] [--out DIR]
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

TH_AUDIO = Path("/home/alexis/Projects/seen-and-not-seen/pipeline/audio.py")
REF_DEFAULT = Path("/home/alexis/Projects/seen-and-not-seen/public/timeline.json")
CLIP_DEFAULT = Path("/home/alexis/Projects/seen-and-not-seen/public/audio/clip.wav")


def load_lines():
    src = TH_AUDIO.read_text()
    g = {"__file__": str(TH_AUDIO)}
    exec(compile(src, str(TH_AUDIO), "exec"), g)
    return list(g["LINES"])


def run_analysis(binary, clip, stems, out_json, lyrics, separate=False, windows=None,
                 span=None):
    # lyrics sidecar the C++ driver reads (<out>.lyrics.json): plain strings,
    # or {"text","t0","t1"} objects with a coarse window in source seconds.
    # windows: optional parallel list of [t0,t1] (or None) per line; when every
    # line has one the analysis aligns exactly like the reference pipeline.
    if windows:
        payload = [{"text": t, "t0": w[0], "t1": w[1]} if w else t
                   for t, w in zip(lyrics, windows)]
    else:
        payload = list(lyrics)
    Path(out_json + ".lyrics.json").write_text(json.dumps(payload))
    cmd = [str(binary), str(clip), str(out_json)]
    if separate:
        cmd += ["--separate"]
    elif stems:
        cmd += ["--stems-dir", str(stems)]
    if span:
        cmd += ["--range", f"{span[0]},{span[1]}"]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=1200)
    sys.stdout.write(proc.stdout[-2000:] if len(proc.stdout) > 2000 else proc.stdout)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr[-4000:])
        raise SystemExit(f"analysis tool failed ({proc.returncode})")
    # lyrics are passed via a sidecar: the tool reads <out>.lyrics.json
    return json.loads(Path(out_json).read_text())


def shift_ref(ref, dt):
    """Return a copy of the reference with beats/downbeats/hits/words shifted by dt."""
    import copy
    if not dt:
        return ref
    ref = copy.deepcopy(ref)
    ref["beats"] = [t + dt for t in ref["beats"]]
    ref["downbeats"] = [t + dt for t in ref["downbeats"]]
    for lst in ref["hits"].values():
        for h in lst:
            h["t"] += dt
    for w in ref["words"]:
        w["t0"] += dt
        w["t1"] += dt
    return ref


def metrics(got, ref):
    out = {}
    gb = got["beats"]
    rb = ref["beats"]
    if not gb or not rb:
        out["beats"] = {"n_got": len(gb), "n_ref": len(rb), "pass": False}
    else:
        errs = []
        for t in rb:
            errs.append(min(abs(t - g) for g in gb))
        worst = max(errs)
        out["beats"] = {
            "n_got": len(gb), "n_ref": len(rb),
            "worst_ms": round(worst * 1000, 1),
            "pass": worst <= 0.020,
        }
    # downbeat phase: song-relative beat index of the first downbeat.
    # Ranged analyses start mid-song, so the span grid's index 0 is NOT the
    # song's beat 0 — compare (first_downbeat - first_beat) / beat_period
    # mod 4 instead of the raw span-local index.
    def phase(beats, downs):
        if not downs or not beats:
            return None
        try:
            return beats.index(downs[0]) % 4
        except ValueError:
            near = min(beats, key=lambda b: abs(b - downs[0]))
            return beats.index(near) % 4
    def song_phase(beats, downs):
        # Song-relative downbeat phase: which song beat (±period/4) the first
        # downbeat sits on, measured from the reference grid origin. Identical
        # grids give identical phases regardless of span start.
        if not downs or len(beats) < 2 or not rb:
            return phase(beats, downs)
        period = beats[1] - beats[0]
        if period <= 0:
            return phase(beats, downs)
        d0 = min(downs, key=lambda d: abs(d - beats[0]))
        return round((d0 - rb[0]) / period) % 4
    gp, rp = song_phase(gb, got.get("downbeats", [])), song_phase(rb, ref.get("downbeats", []))
    out["downbeat"] = {"got": gp, "ref": rp, "pass": gp == rp}
    hits = {}
    allpass = True
    for kind, rh in ref["hits"].items():
        gh = got["hits"].get(kind, [])
        strong = [h for h in rh if h["s"] >= 0.5]
        match = 0
        for h in strong:
            if any(abs(h["t"] - g["t"]) <= 0.030 for g in gh):
                match += 1
        frac = match / len(strong) if strong else 1.0
        ok = frac >= 0.85
        allpass = allpass and ok
        hits[kind] = {"ref_strong": len(strong), "matched": match,
                      "frac": round(frac, 4), "n_got": len(gh), "pass": ok}
    out["hits"] = hits
    out["hits_pass"] = allpass
    gw = got.get("words", [])
    rw = ref.get("words", [])
    if gw and len(gw) == len(rw):
        errs = [abs(g["t0"] - r["t0"]) for g, r in zip(gw, rw)]
        frac = sum(1 for e in errs if e <= 0.080) / len(errs)
        out["words"] = {"n": len(rw), "frac80": round(frac, 4),
                        "worst_ms": round(max(errs) * 1000, 1), "pass": frac >= 0.90}
    else:
        out["words"] = {"n_got": len(gw), "n_ref": len(rw), "pass": False}
    out["pass"] = (out["beats"]["pass"] and out["downbeat"]["pass"]
                   and out["hits_pass"] and out["words"]["pass"])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="build/analysis-parity")
    ap.add_argument("--clip", default=str(CLIP_DEFAULT))
    ap.add_argument("--ref", default=str(REF_DEFAULT))
    ap.add_argument("--stems", default="",
                    help="dir with precomputed {drums,bass,other,vocals}.wav; "
                         "when omitted the C++ htdemucs separation runs in-process")
    ap.add_argument("--separate", action="store_true",
                    help="run the C++ separate_stems4 separation in-process "
                         "(acceptance path) instead of --stems-dir injection")
    ap.add_argument("--out", default="/tmp/audio-work/parity")
    ap.add_argument("--lyrics", default="",
                    help="path to a JSON list of lyric lines (default: pipeline LINES)")
    ap.add_argument("--windows", default="",
                    help="JSON list parallel to the lyric lines: [t0,t1] source-second "
                         "coarse window per line (or null); enables the reference "
                         "windowed path instead of the whisper coarse pass")
    ap.add_argument("--range", default="",
                    help="source-second span t0,t1: decode/separate/analyse/normalise "
                         "only that range (times stay absolute, like the reference "
                         "80-130 s segment)")
    ap.add_argument("--ref-shift", type=float, default=0.0,
                    help="seconds to add to reference times before comparing "
                         "(88 for the full song: timeline.json is clip-relative, "
                         "the ranged analysis reports source seconds)")
    args = ap.parse_args()
    outdir = Path(args.out)
    outdir.mkdir(parents=True, exist_ok=True)

    if args.lyrics:
        lines = json.loads(Path(args.lyrics).read_text())
    else:
        lines = load_lines()
    (outdir / "lines.json").write_text(json.dumps(lines))
    ref = json.loads(Path(args.ref).read_text())

    span = None
    if args.range:
        span = [float(x) for x in args.range.split(",")]
        assert len(span) == 2 and span[1] > span[0] and span[0] >= 0, "--range t0,t1"
    wins = json.loads(Path(args.windows).read_text()) if args.windows else None
    if wins is not None and len(wins) != len(lines):
        raise SystemExit(f"--windows has {len(wins)} entries for {len(lines)} lines")
    ref = shift_ref(ref, args.ref_shift)
    results = {}
    sep = bool(args.separate)
    # Acceptance: --separate runs the C++ separate_stems4 path; --stems DIR
    # only injects precomputed stems (a reuse feature, not the acceptance
    # path). With --separate the fallback row still runs without stems.
    for tag, stems in (("stems", None if sep else (args.stems or None)), ("fallback", None)):
        out_json = str(outdir / f"analysis_{tag}.json")
        print(f"=== [{tag}] running analysis ===", flush=True)
        got = run_analysis(args.binary, args.clip,
                           None if tag == "fallback" else stems,
                           out_json, lines,
                           separate=(sep and tag == "stems"),
                           windows=wins, span=span)
        m = metrics(got, ref)
        results[tag] = m
        print(f"=== [{tag}] beats worst {m['beats'].get('worst_ms')}ms "
              f"pass={m['beats']['pass']}; downbeat {m['downbeat']} "
              f"hits_pass={m['hits_pass']}; words {m['words']} "
              f"OVERALL pass={m['pass']}", flush=True)
        for k, h in m["hits"].items():
            print(f"    {k:6s} {h['matched']:4d}/{h['ref_strong']:4d} "
                  f"({h['frac']*100:.1f}%) n_got={h['n_got']} pass={h['pass']}")
    (outdir / "parity.json").write_text(json.dumps(results, indent=1))
    print(json.dumps(results, indent=1))
    if not all(r["pass"] for r in results.values()):
        print("PARITY: FAIL")
        return 1
    print("PARITY: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
