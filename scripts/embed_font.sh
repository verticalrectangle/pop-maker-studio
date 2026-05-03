#!/usr/bin/env bash
# embed_font.sh <regular.ttf> <bold.ttf> <black.ttf> <output.h>
set -e
REGULAR="$1"
BOLD="$2"
BLACK="$3"
OUTPUT="$4"

xxd_to_array() {
    local file="$1"
    local name="$2"
    local size
    size=$(wc -c < "$file")
    echo "static const unsigned int ${name}_size = ${size};"
    echo "static const unsigned char ${name}[] = {"
    xxd -i "$file" \
        | grep -v "^unsigned\|^};" \
        | sed 's/^/  /'
    echo "};"
}

{
    echo "#pragma once"
    echo "// Auto-generated — do not edit. Run embed_font.sh to regenerate."
    xxd_to_array "$REGULAR" "inter_regular_ttf"
    echo ""
    xxd_to_array "$BOLD" "inter_bold_ttf"
    echo ""
    xxd_to_array "$BLACK" "inter_black_ttf"
} > "$OUTPUT"

echo "Wrote $OUTPUT"
