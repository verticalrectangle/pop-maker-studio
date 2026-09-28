#!/usr/bin/env python3
"""Regenerate assets/scripts/typography/*.js from src/typography_presets.h.

Mechanical step: parse the preset table (designated initializers) and emit one
JS module per preset exporting `meta` + `config`. The renderer lives in
assets/scripts/typography/lib/typography.js and interprets `config`; this
script carries no hand-authored values — re-run after editing the header and
diff the output.

Usage: python3 tools/gen_typography_presets.py [--check]
"""
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HDR = os.path.join(REPO, "src", "typography_presets.h")
OUT_DIR = os.path.join(REPO, "assets", "scripts", "typography")


def parse_presets(src):
    body = src[src.find("g_typo_presets"):src.find("};", src.find("g_typo_presets"))]
    entries = []
    depth = 0
    cur = None
    for i, ch in enumerate(body):
        if ch == "{":
            if depth == 1:
                cur = [i]
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 1 and cur is not None:
                entries.append(body[cur[0]:i + 1])
                cur = None
    out = []
    for e in entries:
        d = {}

        def s(k):
            m = re.search(r"\." + k + r'\s*=\s*"([^"]*)"', e)
            return m.group(1) if m else None

        def num(k):
            m = re.search(r"\." + k + r"\s*=\s*(-?[\d\.]+)f?", e)
            return float(m.group(1)) if m else None

        d["id"] = s("id")
        d["label"] = s("label")
        d["tagline"] = s("tagline")
        d["category"] = s("category")
        d["font"] = s("font")
        m = re.search(r"\.grouping\s*=\s*SubtitleMode::(\w+)", e)
        d["grouping"] = m.group(1) if m else "Phrase"
        m = re.search(r"\.custom_n\s*=\s*(\d+)", e)
        d["custom_n"] = int(m.group(1)) if m else 3
        for k in ["font_size", "sub_pos_x", "sub_pos_y", "sub_wrap_w",
                  "pause_gap", "tracking", "anim_stagger"]:
            v = num(k)
            if v is not None:
                d[k] = v
        for k in ["sub_pos", "sub_anchor_h", "max_words", "text_case",
                  "ease", "anim_unit", "karaoke_mode", "grad_mode"]:
            m = re.search(r"\." + k + r"\s*=\s*(-?\d+)", e)
            if m:
                d[k] = int(m.group(1))
        m = re.search(r"\.all_caps\s*=\s*(true|false)", e)
        if m:
            d["all_caps"] = (m.group(1) == "true")
        m = re.search(r"\.karaoke\s*=\s*(true|false)", e)
        if m:
            d["karaoke"] = (m.group(1) == "true")
        m = re.search(r"\.style\s*=\s*AnimStyle::(\w+)", e)
        d["style"] = m.group(1) if m else "None"
        m = re.search(r"\.color\s*=\s*\{([^}]*)\}", e)
        if m:
            d["color"] = [float(x) for x in m.group(1).replace("f", "").split(",")]
        m = re.search(r"\.grad_col2\s*=\s*\{([^}]*)\}", e)
        if m:
            d["grad_col2"] = [float(x) for x in m.group(1).replace("f", "").split(",")]
        m = re.search(r"\.karaoke_highlight_color\s*=\s*\{([^}]*)\}", e)
        if m:
            d["karaoke_hi"] = [float(x) for x in m.group(1).replace("f", "").split(",")]
        fxm = re.search(r"\.fx\s*=\s*\{(.*?)\}\s*,\s*\.n_fx\s*=\s*(\d+)", e, re.S)
        if fxm:
            d["n_fx"] = int(fxm.group(2))
            d["fx"] = [{"type": t, "beat": float(b)}
                       for t, b in re.findall(r"FXType::(\w+)\s*,\s*([-\d\.]+)f?",
                                              fxm.group(1))]
        else:
            d["n_fx"] = 0
            d["fx"] = []
        tsm = re.search(r"\.ts\s*=\s*\{(.*?)\}", e, re.S)
        ts = {}
        if tsm:
            t = tsm.group(1)

            def tb(k):
                m = re.search(r"\." + k + r"\s*=\s*(true|false)", t)
                return (m.group(1) == "true") if m else None

            def tn(k):
                m = re.search(r"\." + k + r"\s*=\s*(-?[\d\.]+)f?", t)
                return float(m.group(1)) if m else None

            def tc(k):
                m = re.search(r"\." + k + r"\s*=\s*\{([^}]*)\}", t)
                return ([float(x) for x in m.group(1).replace("f", "").split(",")]
                        if m else None)

            for k in ["shadow_enabled", "stroke_enabled", "glow_enabled", "bg_enabled"]:
                v = tb(k)
                if v is not None:
                    ts[k] = v
            for k in ["shadow_ox", "shadow_oy", "stroke_w", "glow_r",
                      "bg_pad_x", "bg_pad_y", "bg_corner"]:
                v = tn(k)
                if v is not None:
                    ts[k] = v
            for k in ["shadow_col", "stroke_col", "glow_col", "bg_col"]:
                v = tc(k)
                if v is not None:
                    ts[k] = v
        d["ts"] = ts
        out.append(d)
    return out


TEMPLATE = """// {label} — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import {{ renderPreset }} from './lib/typography.js';
export const meta = {meta};
const config = {config};
export function setup(env) {{ renderPreset.setup(env, config); }}
export function render(f) {{ renderPreset(f, config); }}
"""


def emit(presets, check=False):
    bad = 0
    for d in presets:
        meta = {"id": d["id"], "name": d["label"], "category": d["category"],
                "tagline": d["tagline"]}
        cfg = {k: v for k, v in d.items() if k not in ("label",)}
        body = TEMPLATE.format(label=d["label"], meta=json.dumps(meta),
                               config=json.dumps(cfg))
        path = os.path.join(OUT_DIR, d["id"] + ".js")
        if check:
            if not os.path.exists(path) or open(path).read() != body:
                print("STALE: " + path)
                bad += 1
        else:
            with open(path, "w") as fh:
                fh.write(body)
    # remove modules for presets that no longer exist
    want = {d["id"] + ".js" for d in presets}
    for fn in sorted(os.listdir(OUT_DIR)):
        if fn == "lib" or not fn.endswith(".js"):
            continue
        if fn not in want:
            if check:
                print("EXTRA: " + fn)
                bad += 1
            else:
                os.unlink(os.path.join(OUT_DIR, fn))
    return bad


def main():
    check = "--check" in sys.argv
    src = open(HDR).read()
    presets = parse_presets(src)
    assert len(presets) == 76, "expected 76 presets, got %d" % len(presets)
    bad = emit(presets, check)
    if check:
        sys.exit(1 if bad else 0)
    print("wrote %d preset modules to %s" % (len(presets), OUT_DIR))


if __name__ == "__main__":
    main()
