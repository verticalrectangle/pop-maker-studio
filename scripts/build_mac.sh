#!/usr/bin/env bash
# build_mac.sh — configure + build pms-engine and its Metal gates on macOS.
# Phase 2.1/2.2 of the iOS port (docs in pms-ios). Headless = the iOS engine
# configuration (no ffmpeg / Skia / desktop deps), so the gates exercise
# exactly what ships. Homebrew provides ORT/whisper/ggml, located explicitly
# (they live in kegs off the default search path). Run from the repo root.
#
#   brew install pkg-config onnxruntime whisper-cpp
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

# Stale CMake caches (e.g. from a different generator) break reconfiguration.
rm -rf build-mac
"$CMAKE" -B build-mac -S . -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DPMS_ENGINE_ONLY=ON \
    -DPMS_HEADLESS=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DONNXRUNTIME_ROOT="$PREFIX/opt/onnxruntime" \
    -Dwhisper_DIR="$PREFIX/opt/whisper-cpp/lib/cmake/whisper" \
    -DCMAKE_CXX_FLAGS="-I$PREFIX/opt/ggml/include"

"$NINJA" -C build-mac engine-smoke
"$NINJA" -C build-mac metal-render-test
"$NINJA" -C build-mac arkit-native-replay
echo "built: build-mac/engine-smoke + metal-render-test + arkit-native-replay"

if [[ "${1:-}" == "--run" ]]; then
    rm -rf /tmp/pms-engine-smoke
    ./build-mac/engine-smoke

    # Metal regression gates for the iOS render path (pms-ios assets + shaders).
    export PMS_ASSET_ROOT="${PMS_ASSET_ROOT:-$HOME/dev/pms-ios/Engine/EngineAssets}"
    export PMS_SHADER_DIR="${PMS_SHADER_DIR:-$HOME/dev/pms-ios/Shaders/msl}"
    if [ -d "$PMS_ASSET_ROOT" ]; then
        echo "running: arkit-native-replay synth (E-Girl on the canonical head)"
        rm -rf /tmp/pms-arkit-synth && mkdir -p /tmp/pms-arkit-synth
        ./build-mac/arkit-native-replay synth /tmp/pms-arkit-synth egirl
        echo "running: metal-render-test (assets=$PMS_ASSET_ROOT, shaders=$PMS_SHADER_DIR)"
        ./build-mac/metal-render-test
    else
        echo "warning: PMS_ASSET_ROOT ($PMS_ASSET_ROOT) not found; Metal gates skipped"
    fi
fi
