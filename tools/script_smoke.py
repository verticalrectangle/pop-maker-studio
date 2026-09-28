#!/usr/bin/env python3
"""Script clip smoke test: Canvas 2D semantics, post shader, errors, hot reload
and pms.audio mapping, end to end through IPC (docs/SCRIPT_API.md).

Starts its own headless instance (xvfb-run, PMS_SOCK), builds a square 1080x1080
project with one Script clip per case, renders stills with render_still and
checks pixels:
  - multi-stop linear gradient (unpremultiplied interpolation)
  - evenodd clip leaves the inner box untouched
  - 'lighter' composite adds
  - drawImage with imageSmoothingEnabled=false is pixel exact (nearest)
  - path points use the transform in effect when they are added
  - center-aligned text is centred; kerning narrows 'AV'; letterSpacing adds per char
  - straight-alpha layer composites over a lower track correctly
  - post shader: v_uv origin top-left, u_tex row 0 = top, output replaces the frame
  - errors: file/line reported, error card drawn, purity guard, unknown member guard
  - hot reload on module edit
  - pms.audio: event times shifted by the audio clip's in_point (timeline seconds)

Usage:
  python3 tools/script_smoke.py [--exe ./build/pop-maker-studio] [--sock PATH] [--keep]
Exits non-zero on failure.
"""
import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import wave
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W = H = 1080
T = 0.5  # still time (seconds)


def ipc(sock, method, params=None, timeout=120):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(sock)
    s.sendall((json.dumps({"id": "1", "method": method, "params": params or {}}) + "\n").encode())
    buf = b""
    while True:
        while b"\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                raise RuntimeError(f"{method}: connection closed")
            buf += chunk
        nl = buf.index(b"\n")
        line, buf = buf[:nl], buf[nl + 1:]
        r = json.loads(line.decode())
        if r.get("type") == "progress":
            continue
        s.close()
        if "error" in r:
            raise RuntimeError(f"{method}: {r['error']}")
        return r.get("result", {})


def write_png(path, w, h, pixels):
    """pixels: list of rows, each a list of (r, g, b, a)."""
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in pixels)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw)))
        f.write(chunk(b"IEND", b""))


def read_png(path):
    """Decode an 8-bit RGB/RGBA non-interlaced PNG → (w, h, rows of (r, g, b))."""
    with open(path, "rb") as f:
        data = f.read()
    pos, idat, w, h, ctype = 8, b"", 0, 0, 0
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        if tag == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            assert depth == 8, "8-bit PNG expected"
        elif tag == b"IDAT":
            idat += body
        pos += 12 + n
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        ft = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                line[i] = (line[i] + a) & 255
            elif ft == 2:
                line[i] = (line[i] + b) & 255
            elif ft == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(line[x * bpp:x * bpp + 3]) for x in range(w)])
        prev = line
    return w, h, rows


class Smoke:
    def __init__(self, sock, work):
        self.sock, self.work, self.failures, self.n = sock, work, [], 0

    def call(self, method, params=None):
        return ipc(self.sock, method, params)

    def check(self, name, ok, detail=""):
        self.n += 1
        if not ok:
            self.failures.append(f"{name}: {detail}")
        print(f"  {'ok  ' if ok else 'FAIL'} {name}" + (f" — {detail}" if not ok and detail else ""))

    def script(self, name, body):
        path = os.path.join(self.work, name)
        with open(path, "w") as f:
            f.write(body)
        return path

    def still(self, tag):
        out = os.path.join(self.work, f"{tag}.png")
        self.call("render_still", {"t": T, "path": out, "format": "square"})
        return read_png(out)[2]

    def errors(self, clip_key):
        for c in self.call("get_script_errors")["clips"]:
            if c["clip"] == clip_key:
                return c
        return {"errors": [], "log": []}


def near(px, want, tol=4):
    return all(abs(a - b) <= tol for a, b in zip(px, want))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sock", default="/tmp/pms-script-smoke.sock")
    ap.add_argument("--exe", default=os.path.join(REPO, "build", "pop-maker-studio"))
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    work = tempfile.mkdtemp(prefix="script_smoke_")
    if os.path.exists(args.sock):
        os.unlink(args.sock)
    app = subprocess.Popen(["xvfb-run", "-a", "-s", "-screen 0 1920x1080x24", args.exe, "--new"],
                           env={**os.environ, "PMS_SOCK": args.sock}, start_new_session=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sm = Smoke(args.sock, work)
    try:
        for _ in range(100):
            if os.path.exists(args.sock):
                break
            time.sleep(0.3)
        else:
            print("FAIL: app did not create socket", args.sock)
            return 1
        time.sleep(3)
        sm.call("set_format", {"format": "square"})
        sm.call("set_fps", {"fps": 60})
        sm.call("add_track", {"name": "under"})
        sm.call("add_track", {"name": "script"})  # track 0 (front), "under" = 1
        first = sm.script("blank.js", "export function render(f) {}\n")
        sm.call("add_script_clip", {"track": 0, "start": 0, "duration": 2, "path": first})

        def run(name, body, params=None):
            p = sm.script(name, body)
            sm.call("set_script_clip", {"track": 0, "clip": 0, "path": p, "params": params or {}})
            return sm.still(name.replace(".js", ""))

        print("Canvas 2D")
        px = run("gradient.js", """
export function render(f) {
  const g = pms.canvas;
  const gr = g.createLinearGradient(0, 0, f.width, 0);
  gr.addColorStop(0, '#ff0000'); gr.addColorStop(0.5, 'rgba(0, 255, 0, 1)'); gr.addColorStop(1, 'blue');
  g.fillStyle = gr; g.fillRect(0, 0, f.width, f.height);
}
""")
        sm.check("gradient stop 0", near(px[540][1], (255, 0, 0), 6), px[540][1])
        sm.check("gradient stop 0.5", near(px[540][540], (0, 255, 0), 6), px[540][540])
        sm.check("gradient mid 0.25", near(px[540][270], (128, 128, 0), 6), px[540][270])
        sm.check("gradient stop 1", near(px[540][1078], (0, 0, 255), 6), px[540][1078])

        px = run("evenodd.js", """
export function render(f) {
  const g = pms.canvas;
  g.fillStyle = '#000'; g.fillRect(0, 0, f.width, f.height);
  g.beginPath(); g.rect(100, 100, 880, 880); g.rect(400, 400, 280, 280); g.clip('evenodd');
  g.fillStyle = '#fff'; g.fillRect(0, 0, f.width, f.height);
}
""")
        sm.check("evenodd clip ring filled", near(px[200][200], (255, 255, 255)), px[200][200])
        sm.check("evenodd clip hole kept", near(px[540][540], (0, 0, 0)), px[540][540])
        sm.check("evenodd clip outside kept", near(px[50][50], (0, 0, 0)), px[50][50])

        px = run("lighter.js", """
export function render(f) {
  const g = pms.canvas;
  g.fillStyle = '#000'; g.fillRect(0, 0, f.width, f.height);
  g.globalCompositeOperation = 'lighter';
  g.fillStyle = 'rgb(100, 40, 10)'; g.fillRect(0, 0, 600, 600); g.fillRect(300, 300, 600, 600);
}
""")
        sm.check("lighter adds", near(px[450][450], (200, 80, 20)), px[450][450])

        img = os.path.join(work, "quad.png")
        write_png(img, 2, 2, [[(255, 0, 0, 255), (0, 255, 0, 255)], [(0, 0, 255, 255), (255, 255, 255, 255)]])
        px = run("nearest.js", f"""
const img = pms.image({json.dumps(img)});
export function render(f) {{
  const g = pms.canvas;
  g.imageSmoothingEnabled = false;
  g.drawImage(img, 0, 0, 1080, 1080);
}}
""")
        sm.check("nearest: quadrant exact", px[10][10] == (255, 0, 0) and px[10][1070] == (0, 255, 0)
                 and px[1070][10] == (0, 0, 255) and px[1070][1070] == (255, 255, 255),
                 (px[10][10], px[10][1070], px[1070][10], px[1070][1070]))
        sm.check("nearest: no blending at the seam", px[538][10] == (255, 0, 0) and px[541][10] == (0, 0, 255),
                 (px[538][10], px[541][10]))

        px = run("path_ctm.js", """
export function render(f) {
  const g = pms.canvas;
  g.fillStyle = '#000'; g.fillRect(0, 0, f.width, f.height);
  g.translate(500, 0); g.beginPath(); g.rect(0, 0, 100, 100); g.resetTransform();
  g.fillStyle = '#fff'; g.fill();
}
""")
        sm.check("path uses transform at add time", near(px[50][550], (255, 255, 255)) and near(px[50][50], (0, 0, 0)),
                 (px[50][550], px[50][50]))

        px = run("text.js", """
export function render(f) {
  const g = pms.canvas;
  g.fillStyle = '#000'; g.fillRect(0, 0, f.width, f.height);
  g.font = 'bold 120px Inter'; g.textAlign = 'center'; g.fillStyle = '#fff';
  g.fillText('HOHOH', 540, 600);
  const av = g.measureText('AV').width, a = g.measureText('A').width, v = g.measureText('V').width;
  const plain = g.measureText('HOHOH').width;
  g.letterSpacing = '10px';
  const spaced = g.measureText('HOHOH').width;
  pms.log(JSON.stringify({kern: a + v - av, spacing: spaced - plain}));
}
""")
        ink = [x for x in range(W) if px[560][x][0] > 128]
        mid = (ink[0] + ink[-1]) / 2 if ink else -1
        sm.check("textAlign center", abs(mid - 540) <= 4, f"ink centre {mid}")
        log = sm.errors("0:0")["log"]
        m = json.loads(log[-1]) if log else {}
        sm.check("kerning narrows AV", m.get("kern", 0) > 1, m)
        sm.check("letterSpacing adds per char", abs(m.get("spacing", 0) - 50) < 0.01, m)

        # Straight alpha: a half-transparent red script layer over a white one.
        under = sm.script("white.js", "export function render(f) { pms.canvas.fillStyle = '#fff'; pms.canvas.fillRect(0, 0, f.width, f.height); }\n")
        sm.call("add_script_clip", {"track": 1, "start": 0, "duration": 2, "path": under})
        px = run("half.js", "export function render(f) { pms.canvas.fillStyle = 'rgba(255, 0, 0, 0.5)'; pms.canvas.fillRect(0, 0, f.width, f.height); }\n")
        sm.check("straight-alpha layer over white", near(px[540][540], (255, 128, 128), 5), px[540][540])
        sm.call("delete_clip", {"track": 1, "clip": 0})

        print("Post shader")
        px = run("post_uv.js", """
export function render(f) {
  pms.canvas.fillStyle = '#f00'; pms.canvas.fillRect(0, 0, f.width, f.height / 2);
  pms.canvas.fillStyle = '#00f'; pms.canvas.fillRect(0, f.height / 2, f.width, f.height / 2);
  pms.post('void main() { vec4 c = texture(u_tex, v_uv); fragColor = vec4(c.r, v_uv.y * u_res.y / 1080.0, c.b, 1.0); }', {});
}
""")
        sm.check("post: u_tex row 0 is the top", px[5][540][0] > 250 and px[1075][540][2] > 250, (px[5][540], px[1075][540]))
        sm.check("post: v_uv origin top-left", px[5][540][1] < 5 and px[1075][540][1] > 250, (px[5][540], px[1075][540]))
        px = run("post_bad.js", "export function render(f) { pms.post('void main() { fragColor = oops; }', {}); }\n")
        errs = sm.errors("0:0")["errors"]
        sm.check("post: compile error reported", any("post shader" in e["message"] for e in errs), errs)

        print("Errors and guards")
        bad = run("throws.js", "export function render(f) {\n  const x = 1;\n  missingFunction(x);\n}\n")
        errs = sm.errors("0:0")["errors"]
        sm.check("error file/line", bool(errs) and errs[0]["file"].endswith("throws.js") and errs[0]["line"] == 3, errs[:1])
        sm.check("error card drawn", bad[540][30][0] > 80 and bad[540][30][1] < 40, bad[540][30])
        run("random.js", "export function render(f) { Math.random(); }\n")
        errs = sm.errors("0:0")["errors"]
        sm.check("Math.random throws", bool(errs) and "non-deterministic" in errs[0]["message"], errs[:1])
        run("unknown.js", "export function render(f) { pms.canvas.fillRectangle(0, 0, 1, 1); }\n")
        errs = sm.errors("0:0")["errors"]
        sm.check("unknown member throws", bool(errs) and "no member 'fillRectangle'" in errs[0]["message"], errs[:1])
        run("assign.js", "export function render(f) { pms.canvas.fillColour = 'red'; }\n")
        errs = sm.errors("0:0")["errors"]
        sm.check("unknown assignment throws", bool(errs) and "fillColour" in errs[0]["message"], errs[:1])

        print("Hot reload")
        hot = sm.script("hot.js", "export function render(f) { pms.canvas.fillStyle = '#f00'; pms.canvas.fillRect(0, 0, f.width, f.height); }\n")
        sm.call("set_script_clip", {"track": 0, "clip": 0, "path": hot, "params": {}})
        a = sm.still("hot1")[540][540]
        with open(hot, "w") as f:
            f.write("export function render(f) { pms.canvas.fillStyle = '#00f'; pms.canvas.fillRect(0, 0, f.width, f.height); }\n")
        st = os.stat(hot)
        os.utime(hot, ns=(st.st_atime_ns, st.st_mtime_ns + 2_000_000_000))
        b = sm.still("hot2")[540][540]
        sm.check("module edit reloads", near(a, (255, 0, 0)) and near(b, (0, 0, 255)), (a, b))

        print("pms.audio mapping")
        wav = os.path.join(work, "tone.wav")
        with wave.open(wav, "wb") as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(48000)
            wf.writeframes(b"\x00\x00" * 48000 * 12)
        analysis = os.path.join(work, "analysis.json")
        with open(analysis, "w") as f:
            json.dump({"version": 2, "source": wav, "duration": 12.0, "bpm": 120, "fps": 60,
                       "beats": [2.5, 3.0, 3.5], "downbeats": [2.5],
                       "hits": {"kick": [{"t": 3.0, "s": 1.0}]}, "env": {"mix": [0.0] * 720},
                       "lines": ["hello"], "words": [{"w": "hello", "line": 0, "i": 0, "t0": 3.25, "t1": 3.5, "conf": 1}]}, f)
        sm.call("load_audio_analysis", {"path": analysis})
        sm.call("add_track", {"name": "audio", "position": 2})
        # Source 2 s → timeline 0: trim the head off, then slide the clip back to 0.
        sm.call("add_clip", {"track": 2, "type": "audio", "text": wav, "start": 0, "end": 10})
        sm.call("trim_clip", {"track": 2, "clip": 0, "start": 2.0})
        sm.call("move_clip", {"track": 2, "clip": 0, "start": 0.0})
        run("audio.js", """
export function render(f) {
  const a = pms.audio;
  pms.log(JSON.stringify({offset: a.offset, beat0: a.beats[0], kick: a.hits.kick[0].t, word: pms.words[0].t0,
                          env: a.env.mix.length, same: a === pms.audio}));
}
""")
        log = sm.errors("0:0")["log"]
        m = json.loads(log[-1]) if log else {}
        sm.check("offset = in_point - start", abs(m.get("offset", -1) - 2.0) < 1e-6, m)
        sm.check("events in timeline seconds", abs(m.get("beat0", -1) - 0.5) < 1e-6 and abs(m.get("kick", -1) - 1.0) < 1e-6
                 and abs(m.get("word", -1) - 1.25) < 1e-6, m)
        sm.check("env re-indexed to timeline 0", m.get("env") == 720 - 120, m)
        sm.check("pms.audio identity-stable", m.get("same") is True, m)

        if sm.failures:
            print(f"FAIL: {len(sm.failures)}/{sm.n} checks")
            for f in sm.failures:
                print("  - " + f)
            return 1
        print(f"PASS: {sm.n} checks")
        return 0
    finally:
        if not args.keep:
            os.killpg(app.pid, 15)
            app.wait(10)


if __name__ == "__main__":
    sys.exit(main())
