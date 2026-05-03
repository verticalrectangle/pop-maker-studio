#!/usr/bin/env bash
# embed_font.sh <regular.ttf> <bold.ttf> <output.h>
set -e
REGULAR="$1"
BOLD="$2"
OUTPUT="$3"

xxd_to_array() {
    local file="$1"
    local name="$2"
    local size
    size=$(wc -c < "$file")
    echo "static const unsigned int ${name}_size = ${size};"
    echo "static const unsigned char ${name}[] = {"
    xxd -i < "$file" | grep -v '^[^0-9]' | sed 's/^/    /'
    echo "};"
}

{
    echo "#pragma once"
    echo "// Auto-generated — do not edit. Run embed_font.sh to regenerate."
    xxd_to_array "$REGULAR" "inter_regular_ttf"
    echo ""
    xxd_to_array "$BOLD" "inter_bold_ttf"
} > "$OUTPUT"

echo "Wrote $OUTPUT"
