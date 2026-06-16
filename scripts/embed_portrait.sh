#!/usr/bin/env bash
# embed_portrait.sh <input.jpg> <output.h>
# Center-crops to 9:16 portrait, scales to PW×PH, embeds as raw RGB C array.
# This is the default source for the FX preview thumbnails AND the big hover
# popover, so it's kept high enough to look crisp blown up (~1:1 at popover size).
set -e

INPUT="$1"
OUTPUT="$2"
PW=270
PH=480
TMP=$(mktemp /tmp/portrait_preview_XXXXXX.rgb)

# Center-crop to 9:16 using input height as reference, scale to PW×PH
ffmpeg -y -i "$INPUT" \
    -vf "crop=ih*9/16:ih,scale=${PW}:${PH}:flags=lanczos" \
    -f rawvideo -pix_fmt rgb24 "$TMP" \
    2>/dev/null

SIZE=$(wc -c < "$TMP")

{
    echo "#pragma once"
    echo "// Auto-generated — do not edit. Run embed_portrait.sh to regenerate."
    echo "static const int portrait_preview_w = ${PW};"
    echo "static const int portrait_preview_h = ${PH};"
    echo "static const unsigned int portrait_preview_size = ${SIZE};"
    echo "static const unsigned char portrait_preview_rgb[] = {"
    xxd -i "$TMP" | grep -v "^unsigned\|^};" | sed 's/^/  /'
    echo "};"
} > "$OUTPUT"

rm "$TMP"
echo "Wrote $OUTPUT (${PW}x${PH} portrait, ${SIZE} bytes raw RGB)"
