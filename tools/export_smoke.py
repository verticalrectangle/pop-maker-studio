#!/usr/bin/env python3
"""Export smoke test: colour fidelity, A/V sync and multi-format via IPC.

Builds a synthetic project on a headless instance (PMS_SOCK, xvfb-run):
  - solid colour-patch stills (R/G/B/grey/skin tones) as full-canvas images
  - a generated click-track WAV (7 clicks, 1 per second) as the audio clip
  - white flash clips placed at the click times

Exports with the `x` preset in both 9:16 and 16:9, then asserts:
  - ffprobe tags: yuv420p, bt709 (space/primaries/transfer), tv range,
    high profile, valid level, fps, 48 kHz audio
  - colour patches decode back within +/-3 levels per channel
  - audio offset vs the source click track < 5 ms by cross-correlation
  - each flash lands within 1 frame of its click

Prints a summary; exits non-zero on failure.

Usage:
  PMS_SOCK=/tmp/pms-export.sock xvfb-run -a ... ./build/pop-maker-studio &
  python3 tools/export_smoke.py [--sock PATH] [--keep] [--exe PATH]
  (default: starts + stops its own headless instance; --keep leaves it up)

  python3 tools/export_smoke.py --exe ./build/pop-maker-studio
"""
import argparse
import json
import math
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import wave

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "mcp_server"))

SR = 48000
DUR = 8.0
CLICKS = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0]
FPS = 30
# (name, (r, g, b)) — primaries, grey ramp, skin tones (matrix-sensitive)
PATCHES = [
    ("red",   (255, 0, 0)),
    ("green", (0, 255, 0)),
    ("blue",  (0, 0, 255)),
    ("grey",  (128, 128, 128)),
    ("skin1", (216, 160, 130)),
    ("skin2", (140, 95, 70)),
]
TOL = 3  # +/- levels per channel
MAX_OFFSET_MS = 5.0


def sh(*args, **kw):
    return subprocess.run(args, capture_output=True, **kw)


def ipc(sock, method, params=None, timeout=600):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(sock)
    s.sendall((json.dumps({"id": "1", "method": method,
                           "params": params or {}}) + "\n").encode())
    buf = b""
    while True:
        while b"\n" not in buf:
            buf += s.recv(65536)
        nl = buf.index(b"\n")
        line, buf = buf[:nl], buf[nl + 1:]
        r = json.loads(line.decode())
        if r.get("type") == "progress":
            continue
        s.close()
        if "error" in r:
            raise RuntimeError(f"{method}: {r['error']}")
        return r.get("result", {})


def write_click_wav(path):
    n = int(SR * DUR)
    frames = bytearray()
    for i in range(n):
        t = i / SR
        v = 0.0
        for c in CLICKS:
            dt = t - c
            if 0 <= dt < 0.03:
                v += math.sin(2 * math.pi * 1000 * dt) * math.exp(-dt * 180)
        v = max(-1.0, min(1.0, v))
        s = int(v * 32767)
        frames += struct.pack("<hh", s, s)
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(bytes(frames))


def write_patch_png(path, rgb, w=64, h=64):
    # minimal RGB PNG via zlib (no PIL needed)
    import zlib
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw))
           + chunk(b"IEND", b""))
    open(path, "wb").write(png)


def ffprobe_entries(path, entries):
    p = sh("ffprobe", "-v", "error", "-show_entries", f"stream={entries}",
           "-of", "default=noprint_wrappers=1", path, text=True)
    out = {}
    for line in p.stdout.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out.setdefault(k.strip(), []).append(v.strip())
    return out


def decode_rgb_frame(path, w, h, frame=0):
    # select frame N, raw rgb24
    p = sh("ffmpeg", "-hide_banner", "-loglevel", "error", "-i", path,
           "-vf", f"select=eq(n\\,{frame})", "-vframes", "1",
           "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1")
    assert len(p.stdout) == w * h * 3, (len(p.stdout), w, h, p.stderr[:200])
    px = bytearray(p.stdout)
    return [px[i:i + 3] for i in range(0, len(px), 3)]


def decode_audio_mono(path):
    p = sh("ffmpeg", "-hide_banner", "-loglevel", "error", "-i", path,
           "-map", "0:a", "-ar", str(SR), "-ac", "1", "-f", "f32le", "pipe:1")
    n = len(p.stdout) // 4
    return struct.unpack(f"<{n}f", p.stdout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sock", default="/tmp/pms-export-smoke.sock")
    ap.add_argument("--exe", default=os.path.join(REPO, "build", "pop-maker-studio"))
    ap.add_argument("--keep", action="store_true",
                    help="leave the headless instance running afterwards")
    args = ap.parse_args()
    sock, failures, notes = args.sock, [], []

    work = tempfile.mkdtemp(prefix="export_smoke_")
    click_wav = os.path.join(work, "click.wav")
    write_click_wav(click_wav)
    patch_paths = {}
    for name, rgb in PATCHES:
        pp = os.path.join(work, f"patch_{name}.png")
        write_patch_png(pp, rgb)
        patch_paths[name] = pp

    # start our own headless instance
    # Own session: terminating only xvfb-run left the app itself running.
    app = subprocess.Popen(
        ["xvfb-run", "-a", "-s", "-screen 0 1920x1080x24", args.exe],
        env={**os.environ, "PMS_SOCK": sock}, start_new_session=True,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if os.path.exists(sock):
                break
            time.sleep(0.3)
        else:
            print("FAIL: app did not create socket", sock)
            return 1
        time.sleep(4)  # let GL settle

        ipc(sock, "new_project", {"force": True})
        # bottom -> top: patches, click audio, white flashes on top
        ipc(sock, "add_track", {"name": "PATCH"})
        ipc(sock, "add_track", {"name": "AUD"})
        ipc(sock, "add_track", {"name": "FLASH"})
        # tracks insert at position 0, so PATCH=2, AUD=1, FLASH=0 after 3 adds
        n_patch = len(PATCHES)
        seg = DUR / n_patch
        for i, (name, rgb) in enumerate(PATCHES):
            ipc(sock, "add_clip", {"track": 2, "type": "video",
                                   "text": patch_paths[name],
                                   "start": i * seg, "end": (i + 1) * seg})
            # full-canvas still: default pos/scale already covers the canvas
        ipc(sock, "add_clip", {"track": 1, "type": "audio", "text": click_wav,
                               "start": 0, "end": DUR})
        flashes = [{"type": "background", "text": "solid",
                    "start": c, "end": c + 0.2} for c in CLICKS]
        ipc(sock, "add_clip_sequence", {"track": 0, "clips": flashes})
        for i in range(len(CLICKS)):
            ipc(sock, "set_clip_prop", {"track": 0, "clip": i,
                                        "prop": "bg_c1", "value": [1, 1, 1, 1]})
        proj = ipc(sock, "get_project")
        assert abs(proj["duration"] - DUR) < 0.05, proj["duration"]

        base = os.path.join(work, "smoke.mp4")
        r = ipc(sock, "trigger_export", {"output_path": base, "platform": "x",
                                         "formats": ["9:16", "16:9"]})
        outputs = r.get("outputs") or [r["output"]]
        assert r.get("success"), r
        # canvas restored after the multi-format run?
        fmt = ipc(sock, "get_project")["format"]
        if fmt != "vertical":
            failures.append(f"canvas not restored after multi-format: {fmt}")
        exp = {"9:16": f"{work}/smoke_9x16.mp4", "16:9": f"{work}/smoke_16x9.mp4"}
        for tag, path in exp.items():
            if path not in outputs:
                failures.append(f"{tag} output missing from result {outputs}")
            if not os.path.exists(path):
                failures.append(f"{tag} file not written: {path}")

        results = {}
        for tag, path in exp.items():
            if not os.path.exists(path):
                continue
            st = ffprobe_entries(
                path, "width,height,pix_fmt,color_space,color_primaries,"
                      "color_transfer,color_range,profile,level,avg_frame_rate,"
                      "codec_name,sample_rate,channels")
            results[tag] = st
            w, h = int(st["width"][0]), int(st["height"][0])
            want = (1080, 1920) if tag == "9:16" else (1920, 1080)
            if (w, h) != want:
                failures.append(f"{tag}: size {w}x{h} != {want[0]}x{want[1]}")
            for k, want_v in [("pix_fmt", "yuv420p"),
                              ("color_space", "bt709"),
                              ("color_primaries", "bt709"),
                              ("color_transfer", "bt709"),
                              ("color_range", "tv"),
                              ("profile", "High")]:
                got = (st.get(k) or ["?"])[0]
                if got != want_v:
                    failures.append(f"{tag}: {k}={got} != {want_v}")
            lvl = int(st.get("level", ["0"])[0])
            # x264/VAAPI level idc must be a legal level >= the content minimum
            if lvl not in (10, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41,
                           42, 50, 51, 52, 60, 61, 62):
                failures.append(f"{tag}: invalid level {lvl}")
            # 1080p30 needs >= 4.0; 1080p60 would need >= 4.2
            if lvl < 40:
                failures.append(f"{tag}: level {lvl/10:.1f} too low for 1080p30")
            fps_s = (st.get("avg_frame_rate") or ["0/1"])[0]
            num, _, den = fps_s.partition("/")
            fps = float(num) / float(den or 1)
            if abs(fps - FPS) > 0.1:
                failures.append(f"{tag}: fps {fps} != {FPS}")
            if (st.get("sample_rate") or ["?"])[0] != "48000":
                failures.append(f"{tag}: audio not 48 kHz: {st.get('sample_rate')}")
            # colour patches: middle of each patch segment (flashes are on
            # top only during click windows — sample mid-segment away from
            # clicks at seg/2 offsets that avoid CLICKS)
            for i, (name, rgb) in enumerate(PATCHES):
                t = (i + 0.5) * seg
                if any(abs(t - c) < 0.15 for c in CLICKS):
                    t += 0.3  # dodge the white flash
                fr = int(round(t * FPS))
                px = decode_rgb_frame(path, w, h, frame=fr)
                cx, cy = w // 2, h // 2
                got = tuple(px[cy * w + cx])
                # patches are uniform except during flashes; use centre pixel
                diffs = [abs(a - b) for a, b in zip(got, rgb)]
                notes.append(f"{tag} {name}@{t:.2f}s: want {rgb} got {got}")
                if max(diffs) > TOL:
                    failures.append(
                        f"{tag} {name}: {got} vs {rgb} (max diff {max(diffs)} > {TOL})")
            # audio offset vs source by cross-correlation of the click trains
            ref = decode_audio_mono(click_wav)
            got_a = decode_audio_mono(path)
            n = min(len(ref), len(got_a))
            ref, got_a = ref[:n], got_a[:n]
            mr, mg = sum(ref) / n, sum(got_a) / n
            # coarse lag search +/-50 ms (AAC priming lives here)
            best_lag, best_v = 0, -1e99
            for lag in range(-2400, 2401):
                v = 0.0
                lo, hi = max(0, -lag), min(n, n - lag)
                for k in range(lo, hi, 7):  # stride for speed
                    v += (got_a[k] - mg) * (ref[k + lag] - mr)
                if v > best_v:
                    best_v, best_lag = v, lag
            off_ms = best_lag / SR * 1000.0
            notes.append(f"{tag} audio offset: {off_ms:.2f} ms (lag {best_lag})")
            if abs(off_ms) > MAX_OFFSET_MS:
                failures.append(f"{tag}: audio offset {off_ms:.2f} ms > {MAX_OFFSET_MS}")
            # flash onsets within 1 frame of clicks (mean luma per frame)
            p = sh("ffmpeg", "-hide_banner", "-loglevel", "error", "-i", path,
                   "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1")
            import numpy as np
            frames = np.frombuffer(p.stdout, dtype=np.uint8).reshape(-1, h, w, 3)
            mean = frames.mean(axis=(1, 2, 3))
            bright = mean > 200
            onsets = [i for i in range(len(bright))
                      if bright[i] and (i == 0 or not bright[i - 1])]
            notes.append(f"{tag} flash onsets: {onsets[:8]}")
            for c, on in zip(CLICKS, onsets):
                if abs(on - round(c * FPS)) > 1:
                    failures.append(
                        f"{tag}: flash for click {c}s at frame {on} "
                        f"(want {round(c * FPS)} +-1)")

        print("== export_smoke summary ==")
        for tag, st in results.items():
            vs = st.get("codec_name", [])
            print(f"[{tag}] {st['width'][0]}x{st['height'][0]} "
                  f"{st['pix_fmt'][0]}/{st['profile'][0]}/level {int(st['level'][0])/10:.1f} "
                  f"{st['color_space'][0]}/{st['color_range'][0]} "
                  f"fps {st['avg_frame_rate'][0]} audio {st['sample_rate'][0]}Hz "
                  f"({','.join(vs)})")
        for n_ in notes:
            print("  " + n_)
        if failures:
            print("FAILURES:")
            for f in failures:
                print("  - " + f)
            return 1
        print(f"PASS: {len(exp)} formats, {n_patch} patches, "
              f"{len(CLICKS)} clicks/flashes")
        return 0
    finally:
        if not args.keep:
            os.killpg(app.pid, 15)
            app.wait(10)


if __name__ == "__main__":
    sys.exit(main())
