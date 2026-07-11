#!/usr/bin/env bash
# build_mac.sh — configure + build pms-engine and engine-smoke on macOS.
# Phase 2.1/2.2 of the iOS port (docs in pms-ios). Homebrew provides the
# desktop deps; ORT/whisper/ggml live in kegs off the default search path,
# so their locations are passed explicitly. Run from the repo root.
#
#   brew install pkg-config ffmpeg fftw aubio freetype jpeg-turbo \
#                onnxruntime whisper-cpp
#   scripts/build_mac.sh [--run]
set -euo pipefail
cd "$(dirname "$0")/.."

BREW="$(command -v brew || echo /usr/local/bin/brew)"
eval "$("$BREW" shellenv)"
PREFIX="$($BREW --prefix)"

# Portable cmake/ninja if the system lacks them (we ship them under ~/tools).
CMAKE="$(command -v cmake || echo "$HOME/tools/cmake-3.31.4-macos-universal/CMake.app/Contents/bin/cmake")"
NINJA="$(command -v ninja || echo "$HOME/tools/ninja")"

git submodule update --init --depth 1 vendor/imgui

"$CMAKE" -B build-mac -S . -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DPMS_ENGINE_ONLY=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DONNXRUNTIME_ROOT="$PREFIX/opt/onnxruntime" \
    -Dwhisper_DIR="$PREFIX/opt/whisper-cpp/lib/cmake/whisper" \
    -DCMAKE_CXX_FLAGS="-I$PREFIX/opt/ggml/include"

"$NINJA" -C build-mac engine-smoke
"$NINJA" -C build-mac metal-render-test
echo "built: build-mac/engine-smoke + metal-render-test"

if [[ "${1:-}" == "--run" ]]; then
    rm -rf /tmp/pms-engine-smoke
    ./build-mac/engine-smoke

    # Metal regression gate for the iOS render path (uses pms-ios assets for face models).
    export PMS_ASSET_ROOT="${PMS_ASSET_ROOT:-$HOME/dev/pms-ios/Engine/EngineAssets}"
    export PMS_SHADER_DIR="${PMS_SHADER_DIR:-$HOME/dev/pms-ios/Shaders/msl}"
    if [ -d "$PMS_ASSET_ROOT" ]; then
        echo "running: metal-render-test (assets=$PMS_ASSET_ROOT, shaders=$PMS_SHADER_DIR)"
        ./build-mac/metal-render-test
    else
        echo "warning: PMS_ASSET_ROOT ($PMS_ASSET_ROOT) not found; metal-render-test skipped"
    fi
fi
