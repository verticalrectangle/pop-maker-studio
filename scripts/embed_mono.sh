#!/usr/bin/env bash
# embed_mono.sh <regular.ttf> <output.h>
set -e
INPUT="$1"
OUTPUT="$2"

SIZE=$(wc -c < "$INPUT")
{
    echo "#pragma once"
    echo "// Auto-generated — do not edit. Run embed_mono.sh to regenerate."
    echo "static const unsigned int jetbrains_mono_regular_ttf_size = ${SIZE};"
    echo "static const unsigned char jetbrains_mono_regular_ttf[] = {"
    xxd -i "$INPUT" | grep -v "^unsigned\|^};" | sed 's/^/  /'
    echo "};"
} > "$OUTPUT"

echo "Wrote $OUTPUT"
