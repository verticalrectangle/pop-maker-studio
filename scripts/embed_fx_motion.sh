#!/usr/bin/env bash
# embed_fx_motion.sh <input.webm> <output.h>
# Embeds the FX-preview motion source clip as a compiled byte array.
set -e
INPUT="$1"
OUTPUT="$2"

SIZE=$(wc -c < "$INPUT")
{
    echo "#pragma once"
    echo "// Auto-generated — do not edit. Run embed_fx_motion.sh to regenerate."
    echo "// FX-preview motion source (CC BY 3.0 — Hackensack Meridian Health; see CREDITS)."
    echo "static const unsigned int fx_motion_webm_size = ${SIZE};"
    echo "static const unsigned char fx_motion_webm[] = {"
    xxd -i "$INPUT" | grep -v "^unsigned\|^};" | sed 's/^/  /'
    echo "};"
} > "$OUTPUT"

echo "Wrote $OUTPUT"
