#!/usr/bin/env bash
# embed_face.sh <input.jpg> <output.h>
# Center-crops to 3:4 portrait, scales to FW×FH, embeds as raw RGB C array.
# Source face for the camera brick's face-filter preview thumbnails — a frontal
# face the live filter pipeline (landmark detect + warp) renders each filter on.
# Swap assets/imgs/face.jpg with any clear frontal portrait and rebuild.
set -e

INPUT="$1"
OUTPUT="$2"
FW=300
FH=400
TMP=$(mktemp /tmp/face_preview_XXXXXX.rgb)

# Center-crop to 3:4 using input width as reference, scale to FW×FH.
ffmpeg -y -i "$INPUT" \
    -vf "crop=ih*3/4:ih,scale=${FW}:${FH}:flags=lanczos" \
    -f rawvideo -pix_fmt rgb24 "$TMP" \
    2>/dev/null

SIZE=$(wc -c < "$TMP")

{
    echo "#pragma once"
    echo "// Auto-generated — do not edit. Run embed_face.sh to regenerate."
    echo "static const int face_preview_w = ${FW};"
    echo "static const int face_preview_h = ${FH};"
    echo "static const unsigned int face_preview_size = ${SIZE};"
    echo "static const unsigned char face_preview_rgb[] = {"
    xxd -i "$TMP" | grep -v "^unsigned\|^};" | sed 's/^/  /'
    echo "};"
} > "$OUTPUT"

rm "$TMP"
echo "Wrote $OUTPUT (${FW}x${FH} face, ${SIZE} bytes raw RGB)"
