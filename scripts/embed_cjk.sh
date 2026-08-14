#!/usr/bin/env bash
# embed_cjk.sh <subset.ttf> <chars.txt> <output.h>
#
# Embeds the Japanese subset font (TrueType outlines — stb_truetype can't
# rasterize CFF, so NOT Noto CJK) plus the exact codepoint list it covers.
# The codepoints drive ImGui's GlyphRangesBuilder so the atlas bakes exactly
# the glyphs present, keeping the CJK atlas cost bounded (kana + Jōyō kanji
# + fullwidth + Latin ≈ 2.5k glyphs).
#
# Regenerating the subset (one-time, needs network + fonttools):
#   1. Fetch a TrueType-outline Japanese font (e.g. M PLUS 1p from
#      google/fonts) and the Jōyō kanji list (KANJIDIC2 grade <= 8), build
#      the char set, then: pyftsubset <full.ttf> --text-file=chars.txt
#      --layout-features='*' --no-hinting -o MPLUS1p-Subset.ttf
#   2. Commit the .ttf + chars.txt; this script runs at build time.
set -e
TTF="$1"
CHARS="$2"
OUTPUT="$3"
SIZE=$(wc -c < "$TTF")

{
    echo "#pragma once"
    echo "// Auto-generated from ${TTF} — do not edit. Run embed_cjk.sh to regenerate."
    echo "static const unsigned int cjk_font_ttf_size = ${SIZE};"
    echo "static const unsigned char cjk_font_ttf[] = {"
    xxd -i "$TTF" | grep -v "^unsigned\|^};" | sed 's/^/  /'
    echo "};"
    echo ""
    echo "// Codepoints the subset covers (UTF-32), for the ImGui range builder."
    echo "static const unsigned int cjk_codepoints_size = $(python3 - "$CHARS" <<'PY'
import sys
s = open(sys.argv[1], encoding='utf-8').read()
print(len(s))
PY
);"
    echo "static const unsigned int cjk_codepoints[] = {"
    python3 - "$CHARS" <<'PY'
import sys
s = open(sys.argv[1], encoding='utf-8').read()
cps = sorted(ord(c) for c in set(s))
print(", ".join(str(c) for c in cps))
PY
    echo "};"
} > "$OUTPUT"

echo "Wrote $OUTPUT"
