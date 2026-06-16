#!/usr/bin/env bash
# embed_fonts_dir.sh <fonts_dir> <output.h>
# Embeds every *.ttf in <fonts_dir> into one header as byte arrays plus a
# master table (g_embedded_fonts[]) so the app can register them by name at
# runtime. A missing/failed font simply doesn't appear in the table — callers
# fall back gracefully. Array/name = sanitized lowercase basename.
set -e
DIR="$1"
OUT="$2"

sanitize() { echo "$1" | tr 'A-Z' 'a-z' | sed 's/[^a-z0-9]/_/g'; }

{
    echo "#pragma once"
    echo "// Auto-generated from ${DIR} — do not edit. Run embed_fonts_dir.sh."
    names=()
    for f in "$DIR"/*.ttf; do
        [ -e "$f" ] || continue
        base=$(basename "$f" .ttf)
        name=$(sanitize "$base")
        size=$(wc -c < "$f")
        echo "static const unsigned int font_${name}_ttf_size = ${size};"
        echo "static const unsigned char font_${name}_ttf[] = {"
        xxd -i "$f" | grep -v "^unsigned\|^};" | sed 's/^/  /'
        echo "};"
        names+=("$name")
    done
    echo "struct EmbeddedFont { const char* name; const unsigned char* data; unsigned int size; };"
    echo "static const EmbeddedFont g_embedded_fonts[] = {"
    for n in "${names[@]}"; do
        echo "  { \"${n}\", font_${n}_ttf, font_${n}_ttf_size },"
    done
    echo "  { \"\", 0, 0 }"
    echo "};"
    echo "static const int g_n_embedded_fonts = (int)(sizeof(g_embedded_fonts)/sizeof(g_embedded_fonts[0])) - 1;"
} > "$OUT"

echo "Wrote $OUT"
