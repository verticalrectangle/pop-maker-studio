#!/usr/bin/env bash
# Enforce the engine/app boundary: engine sources (PMS_ENGINE_SOURCES) must
# not include desktop-app headers. Every legitimate cross-boundary need is
# declared in src/engine_seams.h instead. Wired as a build-time check, same
# pattern as the agent_tools sync check.
set -u
cd "$(dirname "$0")/.."
# Engine list = the PMS_ENGINE_SOURCES block in CMakeLists.txt
files=$(sed -n '/set(PMS_ENGINE_SOURCES/,/^)/p' CMakeLists.txt | grep -oE 'src/[a-z0-9_]+\.cpp')
bad=0
for f in $files; do
    [ -f "$f" ] || continue
    hits=$(grep -nE '#include "(ui/|\.\./ui/)|imgui_impl_glfw|GLFW/' "$f")
    if [ -n "$hits" ]; then
        echo "ENGINE BOUNDARY VIOLATION in $f:"
        echo "$hits"
        bad=1
    fi
done
if [ "$bad" -ne 0 ]; then
    echo "Engine sources must not reach into the app layer — declare the"
    echo "needed symbol in src/engine_seams.h instead (see that header's rules)."
    exit 1
fi
echo "engine/app boundary clean ($(echo "$files" | wc -l) engine sources)"
