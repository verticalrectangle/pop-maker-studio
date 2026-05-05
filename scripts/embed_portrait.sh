#!/usr/bin/env bash
# embed_portrait.sh <input.jpg> <output.h>
# Center-crops to 9:16 portrait, scales to 108x192, embeds as raw RGB C array.
set -e

INPUT="$1"
OUTPUT="$2"
TMP=$(mktemp /tmp/portrait_preview_XXXXXX.rgb)

# Center-crop to 9:16 using input height as reference, scale to 108x192
ffmpeg -y -i "$INPUT" \
    -vf "crop=ih*9/16:ih,scale=108:192:flags=lanczos" \
    -f rawvideo -pix_fmt rgb24 "$TMP" \
    2>/dev/null

SIZE=$(wc -c < "$TMP")

{
    echo "#pragma once"
    echo "// Auto-generated — do not edit. Run embed_portrait.sh to regenerate."
    echo "static const int portrait_preview_w = 108;"
    echo "static const int portrait_preview_h = 192;"
    echo "static const unsigned int portrait_preview_size = ${SIZE};"
    echo "static const unsigned char portrait_preview_rgb[] = {"
    xxd -i "$TMP" | grep -v "^unsigned\|^};" | sed 's/^/  /'
    echo "};"
} > "$OUTPUT"

rm "$TMP"
echo "Wrote $OUTPUT (108x192 portrait, ${SIZE} bytes raw RGB)"
