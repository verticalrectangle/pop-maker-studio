#!/usr/bin/env python3
"""Side-by-side contact sheets: native preset stills vs JS Script-clip renders.

Reads /tmp/typo-work/parity.json rows [preset, fmt, t, psnr, ...] plus
/tmp/typo-ref/base-<p>-<tag>-<t>.png (native) and /tmp/typo-work/js-<p>-<tag>-<t>.png (JS).
Writes one sheet per preset (/tmp/typo-work/sheets/<id>.png: 2 formats x 3 times,
native left / JS right with PSNR labels) plus a per-preset metric table
(/tmp/typo-work/psnr_table.md).

Usage: python3 tools/typo_contact_sheet.py
"""
import json
import math
import os

REF = "/tmp/typo-ref"
JS = "/tmp/typo-work"
OUT = "/tmp/typo-work/sheets"

try:
    from PIL import Image, ImageDraw
    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False


def load(p):
    import numpy as np
    return np.asarray(Image.open(p).convert("RGB")).astype(float)


def psnr(a, b):
    import numpy as np
    mse = ((a - b) ** 2).mean()
    return 99.0 if mse == 0 else 10 * math.log10(65025 / mse)


def main():
    import numpy as np
    rows = json.load(open(os.path.join(JS, "parity.json")))
    presets = json.load(open(os.path.join(REF, "presets.json")))
    times = json.load(open(os.path.join(REF, "times.json")))
    os.makedirs(OUT, exist_ok=True)
    table = ["| preset | 9:16 enter/hold/karaoke | 16:9 enter/hold/karaoke | mean |",
             "|---|---|---|---|"]
    for p in presets:
        cells, scores = [], []
        ok = True
        for fmt in ["9:16", "16:9"]:
            tag = fmt.replace(":", "x")
            for t in times:
                rp = os.path.join(REF, f"base-{p}-{tag}-{t:.3f}.png")
                jp = os.path.join(JS, f"js-{p}-{tag}-{t:.3f}.png")
                try:
                    a, b = load(rp), load(jp)
                except OSError:
                    ok = False
                    continue
                if a.shape != b.shape:
                    cells.append("SIZE")
                    continue
                v = round(psnr(a, b), 1)
                scores.append(v)
                cells.append(f"{v:.1f}")
        mean = round(sum(scores) / len(scores), 1) if scores else 0.0
        table.append(f"| {p} | {' / '.join(cells[:3])} | {' / '.join(cells[3:])} | {mean:.1f} |")
        if not (HAVE_PIL and ok):
            continue
        # 2 formats x 3 times grid; each cell: native | JS at thumb scale.
        thumbs = []
        for fmt in ["9:16", "16:9"]:
            tag = fmt.replace(":", "x")
            row = []
            for t in times:
                rp = os.path.join(REF, f"base-{p}-{tag}-{t:.3f}.png")
                jp = os.path.join(JS, f"js-{p}-{tag}-{t:.3f}.png")
                a = Image.open(rp).convert("RGB")
                b = Image.open(jp).convert("RGB")
                s = 200 / max(a.size[1], 1)
                w = max(1, int(a.size[0] * s))
                h = 200
                row.append((a.resize((w, h)), b.resize((w, h))))
            thumbs.append(row)
        cw = max(t[0].size[0] * 2 for r in thumbs for t in r)
        sheet = Image.new("RGB", (cw * 3 + 40, 200 * 2 + 60), (16, 14, 24))
        d = ImageDraw.Draw(sheet)
        d.text((10, 8), f"{p}  (native | JS)  9:16 top / 16:9 bottom", fill=(220, 220, 230))
        for ri, row in enumerate(thumbs):
            for ci, (a, b) in enumerate(row):
                x = 10 + ci * (cw + 10)
                y = 30 + ri * 210
                sheet.paste(a, (x, y))
                sheet.paste(b, (x + a.size[0], y))
        sheet.save(os.path.join(OUT, f"{p}.png"))
    open(os.path.join(JS, "psnr_table.md"), "w").write("\n".join(table) + "\n")
    print(f"wrote {len(presets)} sheets to {OUT} + psnr_table.md")


if __name__ == "__main__":
    main()
