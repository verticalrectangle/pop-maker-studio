#!/usr/bin/env bash
# build_xcframework.sh — cross-compile pms-engine for iOS device (arm64) and
# package it as pms_engine.xcframework for the pms-ios app (Playbook P2.3).
# Prereqs (built once, see docs): the iOS whisper install at ~/tools/whisper-ios
# and the ORT iOS pod extracted at ~/tools/ort-ios-cmake (a -L/-l shim over
# onnxruntime.xcframework/ios-arm64). Run from the engine repo root.
set -euo pipefail
cd "$(dirname "$0")/.."
BREW="$(command -v brew || echo /usr/local/bin/brew)"; eval "$("$BREW" shellenv)"
CMAKE="$(command -v cmake || echo "$HOME/tools/cmake-3.31.4-macos-universal/CMake.app/Contents/bin/cmake")"
NINJA="$(command -v ninja || echo "$HOME/tools/ninja")"
TOOLS=${PMS_IOS_TOOLS:-$HOME/tools}
OUT=${1:-$HOME/dev/pms-ios/Engine/build}

git submodule update --init --depth 1 vendor/imgui

"$CMAKE" -B build-ios -S . -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLS/ios.toolchain.cmake" \
    -DPLATFORM=OS64 -DDEPLOYMENT_TARGET=17.0 -DENABLE_BITCODE=OFF \
    -DPMS_ENGINE_ONLY=ON -DPMS_HEADLESS=ON \
    -DONNXRUNTIME_ROOT="$TOOLS/ort-ios-cmake" \
    -Dwhisper_DIR="$TOOLS/whisper-ios/lib/cmake/whisper" \
    -Dggml_DIR="$TOOLS/whisper-ios/lib/cmake/ggml"
"$NINJA" -C build-ios pms-engine

# Merge the engine + whisper/ggml into one static lib (single engine link for
# the app; ORT stays its own xcframework, system frameworks declared in the app).
mkdir -p build-ios/xcf/headers
libtool -static -o build-ios/xcf/libpms-engine-full.a \
    build-ios/libpms-engine.a \
    "$TOOLS"/whisper-ios/lib/libwhisper.a \
    "$TOOLS"/whisper-ios/lib/libggml.a "$TOOLS"/whisper-ios/lib/libggml-cpu.a \
    "$TOOLS"/whisper-ios/lib/libggml-blas.a "$TOOLS"/whisper-ios/lib/libggml-metal.a \
    "$TOOLS"/whisper-ios/lib/libggml-base.a 2>/dev/null || true
cp src/pms_engine.h build-ios/xcf/headers/

rm -rf "$OUT/pms_engine.xcframework"
mkdir -p "$OUT"
xcodebuild -create-xcframework \
    -library build-ios/xcf/libpms-engine-full.a -headers build-ios/xcf/headers \
    -output "$OUT/pms_engine.xcframework"
echo "built: $OUT/pms_engine.xcframework ($(lipo -archs build-ios/xcf/libpms-engine-full.a))"
