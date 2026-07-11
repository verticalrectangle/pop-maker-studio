#!/usr/bin/env bash
# embed_fx_face.sh <input.jpg> <output.h>
# Center-crops a portrait to 9:16, scales to the FX-preview size, embeds as raw
# RGB. Used as the preview source for face-centric effects (Skin Smooth, Glow Up)
# so their result is legible on an actual face.
set -e

INPUT="$1"
OUTPUT="$2"
PW=270
PH=480
# BSD mktemp (macOS) requires the Xs at the END of the template.
TMP=$(mktemp /tmp/fx_face_XXXXXX).rgb
trap 'rm -f "${TMP%.rgb}" "$TMP"' EXIT

ffmpeg -y -i "$INPUT" \
    -vf "crop=ih*9/16:ih,scale=${PW}:${PH}:flags=lanczos" \
    -f rawvideo -pix_fmt rgb24 "$TMP" \
    2>/dev/null

SIZE=$(wc -c < "$TMP")

{
    echo "#pragma once"
    echo "// Auto-generated — do not edit. Run embed_fx_face.sh to regenerate."
    echo "static const int fx_face_w = ${PW};"
    echo "static const int fx_face_h = ${PH};"
    echo "static const unsigned int fx_face_size = ${SIZE};"
    echo "static const unsigned char fx_face_rgb[] = {"
    xxd -i "$TMP" | grep -v "^unsigned\|^};" | sed 's/^/  /'
    echo "};"
} > "$OUTPUT"

rm "$TMP"
echo "Wrote $OUTPUT (${PW}x${PH} face, ${SIZE} bytes raw RGB)"
