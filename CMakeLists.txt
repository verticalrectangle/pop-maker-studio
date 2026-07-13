cmake_minimum_required(VERSION 3.20)
project(PopMakerStudio VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
find_package(whisper CONFIG QUIET)
# freetype backs ImGui's rasterizer on desktop; headless/iOS falls back to
# ImGui's built-in stb_truetype (the engine has no direct freetype use).
if(NOT PMS_HEADLESS)
    pkg_check_modules(FREETYPE REQUIRED freetype2)
endif()
if(NOT PMS_ENGINE_ONLY)
    pkg_check_modules(GLFW REQUIRED glfw3)
    pkg_check_modules(VTERM REQUIRED vterm)
endif()
# ffmpeg / aubio / fftw / turbojpeg / OpenGL — desktop capabilities the
# headless (iOS) build stubs out (PMS_HEADLESS). Skip finding them there.
if(NOT PMS_HEADLESS)
    find_package(OpenGL REQUIRED)
    pkg_check_modules(AVCODEC  REQUIRED libavcodec)
    pkg_check_modules(AVFORMAT REQUIRED libavformat)
    pkg_check_modules(AVUTIL   REQUIRED libavutil)
    pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
    pkg_check_modules(SWSCALE   REQUIRED libswscale)
    pkg_check_modules(AUBIO     REQUIRED aubio)
    pkg_check_modules(TURBOJPEG REQUIRED libturbojpeg)
    pkg_check_modules(FFTW      REQUIRED fftw3f)
endif()
# Native low-latency audio backend (perf mode); optional — the miniaudio
# duplex path is the fallback when PipeWire is absent.
if(NOT APPLE AND NOT PMS_HEADLESS)
    pkg_check_modules(PIPEWIRE libpipewire-0.3)
endif()

if(PMS_HEADLESS)
    add_compile_definitions(PMS_HEADLESS=1)
endif()

# pkg_check_modules splits keg -L paths into *_LIBRARY_DIRS; on Linux the
# libs sit in /usr/lib (default search) so this is a no-op, but Homebrew
# kegs (macOS) live off-path and the linker needs these. Empty vars are
# harmless. Applies to every target defined below.
link_directories(
    ${AVCODEC_LIBRARY_DIRS} ${AVFORMAT_LIBRARY_DIRS} ${AVUTIL_LIBRARY_DIRS}
    ${SWRESAMPLE_LIBRARY_DIRS} ${SWSCALE_LIBRARY_DIRS} ${FREETYPE_LIBRARY_DIRS}
    ${AUBIO_LIBRARY_DIRS} ${TURBOJPEG_LIBRARY_DIRS} ${FFTW_LIBRARY_DIRS}
    ${GLFW_LIBRARY_DIRS} ${VTERM_LIBRARY_DIRS} ${PIPEWIRE_LIBRARY_DIRS}
)

# ── ImGui ─────────────────────────────────────────────────────────────────────
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/vendor/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
if(NOT PMS_HEADLESS)
    list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp)
endif()
# GLFW backend is app-only — the engine never touches windowing.
set(IMGUI_APP_SOURCES
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
)

# ── Sources ───────────────────────────────────────────────────────────────────
# ENGINE sources: portable core (timeline, serializer, FX, ML, audio, IPC
# dispatcher). Must not include "ui/..." headers or the GLFW backend — every
# app-side symbol they need is declared in src/engine_seams.h, and
# tools/check_engine_deps.sh enforces the boundary at build time. This is the
# set that becomes pms_engine.xcframework for iOS (docs/IOS_PORT_PLAN.md).
# PipeWire backend is Linux-desktop-only (excluded on Apple + headless/iOS).
if(NOT APPLE AND NOT PMS_HEADLESS)
    list(APPEND PMS_PLATFORM_ENGINE_SOURCES src/audio_pw.cpp)
endif()
# Metal RenderSurface (Phase 3) — Apple only.
if(APPLE)
    list(APPEND PMS_PLATFORM_ENGINE_SOURCES src/metal_render.mm)
endif()

option(PMS_ENGINE_ONLY "Build only pms-engine + engine-smoke (no desktop app; macOS Phase 2 bring-up)" OFF)
option(PMS_HEADLESS "Engine with no ffmpeg/GL/fftw/process-spawn — iOS-shaped stubs (Phase 2.3)" OFF)

set(PMS_ENGINE_SOURCES
    src/paths.cpp
    src/timeline_core.cpp
    src/media_bin.cpp
    src/ui_geom.cpp
    src/pipeline_core.cpp
    src/app_state.cpp
    src/project_meta.cpp
    src/typography_core.cpp
    src/stb_impl.cpp
    src/pms_engine.cpp
    src/engine_runtime.cpp
    src/rmvpe_onnx.cpp
    src/runtime_fx.cpp
    src/ipc_server.cpp
    src/audio.cpp
    src/loudness.cpp
    src/face_track.cpp
    src/face_cache.cpp
    src/face_filters.cpp
    src/arkit_face.cpp
    src/recorder.cpp
    src/video_recorder.cpp
    src/av_sync.cpp
    src/av_measure.cpp
    src/text_anim.cpp
    src/transcribe.cpp
    src/render.cpp
    src/history.cpp
    src/video.cpp
    src/proxy.cpp
    src/conform.cpp
    src/waveform.cpp
    src/beat_detect.cpp
    src/blender_export.cpp
    src/bg_remove.cpp
    src/scene_detect.cpp
    src/vision_caption.cpp
    src/body_fx.cpp
    src/noise_reduce.cpp
    src/audio_fx.cpp
    src/hf_api.cpp
    src/vc_job.cpp
    src/vc_onnx.cpp
    src/faiss_ivf.cpp
    src/pth_reader.cpp
    src/rvc_onnx.cpp
    src/hubert_onnx.cpp
    src/separate.cpp
    src/forced_align.cpp
    src/agent_harness.cpp
    src/project.cpp
    src/fx_shader.cpp
    src/presets.cpp
    src/overlay_renderer.cpp
    src/text_renderer.cpp
)


# APP sources: desktop shell — windowing, panels, pickers, terminal.
set(PMS_APP_SOURCES
    src/main.cpp
    src/app.cpp
    src/filepicker.cpp
    src/terminal.cpp
    src/ui/screen_splash.cpp
    src/ui/screen_setup.cpp
    src/ui/screen_home.cpp
    src/ui/screen_studio.cpp
    src/ui/studio_shared.cpp
    src/ui/canvas.cpp
    src/ui/timeline.cpp
    src/ui/pipeline_ui.cpp
    src/ui/panel_clip.cpp
    src/ui/panel_animation.cpp
    src/ui/panel_fx.cpp
    src/ui/panel_media.cpp
    src/ui/export_ui.cpp
    src/ui/panel_terminal.cpp
    src/ui/panel_agent.cpp
    src/ui/theme.cpp
)

# The engine as a static library: the compile-time boundary the iOS port
# builds on. Links standalone once engine_seams.h empties out; until then the
# app's objects satisfy the seam symbols at executable link time.
# miniaudio's Apple backend pulls in CoreAudio/Foundation (Objective-C);
# audio.cpp must compile as Objective-C++ on Apple.
if(APPLE)
    set_source_files_properties(src/audio.cpp PROPERTIES COMPILE_FLAGS "-x objective-c++")
endif()
add_library(pms-engine STATIC ${PMS_ENGINE_SOURCES} ${PMS_PLATFORM_ENGINE_SOURCES} ${IMGUI_SOURCES})

# miniaudio's Apple backend needs the CoreAudio/AVFoundation stack. macOS
# auto-links some; iOS requires them explicit.
if(APPLE)
    target_link_libraries(pms-engine PUBLIC
        "-framework AVFoundation" "-framework AudioToolbox"
        "-framework CoreAudio" "-framework CoreFoundation"
        "-framework Metal" "-framework QuartzCore")
endif()

if(NOT PMS_ENGINE_ONLY)
add_executable(pop-maker-studio ${PMS_APP_SOURCES} ${IMGUI_APP_SOURCES})
target_link_libraries(pop-maker-studio PRIVATE pms-engine)
endif()
add_custom_target(check_engine_deps
    COMMAND ${CMAKE_SOURCE_DIR}/tools/check_engine_deps.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking engine/app boundary (engine_seams.h rules)"
)
add_dependencies(pms-engine check_engine_deps)

# Phase 0 proof: the engine driven headless through the C ABI (no app, no
# socket, no window). CI-able: build + run = the boundary holds.
add_executable(engine-smoke tools/engine_smoke.cpp)
target_link_libraries(engine-smoke PRIVATE pms-engine)
target_include_directories(engine-smoke PRIVATE src ${CMAKE_SOURCE_DIR}/vendor)

# face-smoke: functional gate for the face-tracking stack (MAKEUP_PLAN Stage 1)
# — loads assets/test_face.png and runs the real YuNet → landmarks →
# blendshapes pipeline through face_track_run_sync. Requires models/face/*
# (tools/fetch_face_models.py).
add_executable(face-smoke tools/face_smoke.cpp)
target_link_libraries(face-smoke PRIVATE pms-engine)
target_include_directories(face-smoke PRIVATE src ${CMAKE_SOURCE_DIR}/vendor)

# arkit-map-smoke: regression gate for the static ARKit->MediaPipe
# correspondence (tools/gen_arkit_mp_map.py). Projects the canonical ARKit
# mesh into a synthetic frame and asserts anatomical landmark placement.
# No models needed. Run: arkit-map-smoke tools/arkit_face_canonical.obj
add_executable(arkit-map-smoke tools/arkit_map_smoke.cpp)
target_link_libraries(arkit-map-smoke PRIVATE pms-engine)
target_include_directories(arkit-map-smoke PRIVATE src ${CMAKE_SOURCE_DIR}/vendor ${CMAKE_SOURCE_DIR}/vendor/imgui)

# metal-render-test: numeric offscreen assertions for the Metal scene
# compositor + FX runner (the iOS render path) on a macOS host GPU. Drives the
# real C ABI (pms_create/pms_command/pms_submit_layer_frame/pms_render) and
# reads pixels back. Regression gate for FX-not-rendering class bugs; set
# PMS_SHADER_DIR to the transpiled Shaders/msl dir (default ~/dev/pms-ios/…).
if(APPLE)
    add_executable(metal-render-test tools/metal_render_test.mm)
    target_link_libraries(metal-render-test PRIVATE pms-engine
        "-framework Metal" "-framework CoreVideo" "-framework IOSurface"
        "-framework QuartzCore" "-framework Foundation")
    target_include_directories(metal-render-test PRIVATE src ${CMAKE_SOURCE_DIR}/vendor)
    target_compile_options(metal-render-test PRIVATE -Wall -Wextra -Wno-unused-parameter)

    add_executable(face-fx-smoke tools/face_fx_smoke.mm)
    target_link_libraries(face-fx-smoke PRIVATE pms-engine
        "-framework Metal" "-framework CoreVideo" "-framework IOSurface"
        "-framework QuartzCore" "-framework Foundation")
    target_include_directories(face-fx-smoke PRIVATE src ${CMAKE_SOURCE_DIR}/vendor)

    add_executable(arkit-native-replay tools/arkit_native_replay.mm)
    target_link_libraries(arkit-native-replay PRIVATE pms-engine
        "-framework Metal" "-framework CoreVideo" "-framework IOSurface"
        "-framework QuartzCore" "-framework Foundation")
    target_include_directories(arkit-native-replay PRIVATE src ${CMAKE_SOURCE_DIR}/vendor)

    add_executable(face-fx-photo tools/face_fx_photo.mm)
    target_link_libraries(face-fx-photo PRIVATE pms-engine
        "-framework Metal" "-framework CoreVideo" "-framework IOSurface"
        "-framework QuartzCore" "-framework Foundation")
    target_include_directories(face-fx-photo PRIVATE src ${CMAKE_SOURCE_DIR}/vendor)
endif()


if(PIPEWIRE_FOUND AND NOT PMS_HEADLESS)
if(NOT PMS_ENGINE_ONLY)
    target_compile_definitions(pop-maker-studio PRIVATE HAVE_PIPEWIRE)
endif()
    target_compile_definitions(pms-engine PRIVATE HAVE_PIPEWIRE)
endif()

if(PMS_ENGINE_ONLY)
    set(PMS_ALL_TARGETS pms-engine)
else()
    set(PMS_ALL_TARGETS pop-maker-studio pms-engine)
endif()
foreach(_pms_tgt ${PMS_ALL_TARGETS})
target_include_directories(${_pms_tgt} PRIVATE
    src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMGUI_DIR}/misc/freetype
    ${CMAKE_SOURCE_DIR}/vendor
    ${GLFW_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${AUBIO_INCLUDE_DIRS}
    ${VTERM_INCLUDE_DIRS}
    ${TURBOJPEG_INCLUDE_DIRS}
    ${FFTW_INCLUDE_DIRS}
    ${PIPEWIRE_INCLUDE_DIRS}
)
endforeach()

target_link_libraries(pms-engine PUBLIC
    onnxruntime
    $<IF:$<TARGET_EXISTS:whisper::whisper>,whisper::whisper,whisper>
    pthread
)
if(NOT PMS_HEADLESS)
    target_link_libraries(pms-engine PUBLIC
        ${FREETYPE_LIBRARIES}
        OpenGL::GL
        ${AVCODEC_LIBRARIES} ${AVFORMAT_LIBRARIES} ${AVUTIL_LIBRARIES}
        ${SWRESAMPLE_LIBRARIES} ${SWSCALE_LIBRARIES}
        ${AUBIO_LIBRARIES} ${TURBOJPEG_LIBRARIES} ${PIPEWIRE_LIBRARIES}
        ${FFTW_LIBRARIES}
        dl
    )
endif()

if(NOT PMS_ENGINE_ONLY)
target_link_libraries(pop-maker-studio PRIVATE
    OpenGL::GL
    ${GLFW_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    ${AUBIO_LIBRARIES}
    ${VTERM_LIBRARIES}
    ${TURBOJPEG_LIBRARIES}
    ${PIPEWIRE_LIBRARIES}
    onnxruntime
    $<IF:$<TARGET_EXISTS:whisper::whisper>,whisper::whisper,whisper>
    ${FFTW_LIBRARIES}
    dl pthread
)
endif()

if(DEFINED ONNXRUNTIME_ROOT)
if(NOT PMS_ENGINE_ONLY)
    target_include_directories(pop-maker-studio PRIVATE ${ONNXRUNTIME_ROOT}/include)
endif()
if(NOT PMS_ENGINE_ONLY)
    target_link_directories(pop-maker-studio PRIVATE ${ONNXRUNTIME_ROOT}/lib)
endif()
    target_include_directories(pms-engine PRIVATE ${ONNXRUNTIME_ROOT}/include)
    # Homebrew's keg nests headers one level deeper (include/onnxruntime/*.h)
    if(EXISTS ${ONNXRUNTIME_ROOT}/include/onnxruntime/onnxruntime_cxx_api.h)
        target_include_directories(pms-engine PRIVATE ${ONNXRUNTIME_ROOT}/include/onnxruntime)
    endif()
    # PUBLIC: engine-smoke / the app link the static pms-engine and must
    # inherit the keg -L (Linux masked this — ORT in /usr/lib).
    target_link_directories(pms-engine PUBLIC ${ONNXRUNTIME_ROOT}/lib)
else()
if(NOT PMS_ENGINE_ONLY)
    target_include_directories(pop-maker-studio PRIVATE /usr/include/onnxruntime)
endif()
    target_include_directories(pms-engine PRIVATE /usr/include/onnxruntime)
endif()

if(NOT PMS_ENGINE_ONLY)
target_compile_definitions(pop-maker-studio PRIVATE IMGUI_ENABLE_FREETYPE)
endif()
if(NOT PMS_HEADLESS)
    target_compile_definitions(pms-engine PRIVATE IMGUI_ENABLE_FREETYPE)
endif()

foreach(_pms_tgt ${PMS_ALL_TARGETS})
target_compile_options(${_pms_tgt} PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    $<$<CONFIG:Release>:-O2>
)
endforeach()

# Embed Inter font as a compiled byte array
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/inter_font.h
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/embed_font.sh
            ${CMAKE_SOURCE_DIR}/assets/fonts/Inter-Regular.ttf
            ${CMAKE_SOURCE_DIR}/assets/fonts/Inter-Bold.ttf
            ${CMAKE_SOURCE_DIR}/assets/fonts/Inter-Black.ttf
            ${CMAKE_BINARY_DIR}/inter_font.h
    DEPENDS
        ${CMAKE_SOURCE_DIR}/assets/fonts/Inter-Regular.ttf
        ${CMAKE_SOURCE_DIR}/assets/fonts/Inter-Bold.ttf
        ${CMAKE_SOURCE_DIR}/assets/fonts/Inter-Black.ttf
    COMMENT "Embedding Inter fonts"
)
add_custom_target(embed_font DEPENDS ${CMAKE_BINARY_DIR}/inter_font.h)
if(NOT PMS_ENGINE_ONLY)
add_dependencies(pop-maker-studio embed_font)
endif()
add_dependencies(pms-engine embed_font)
add_dependencies(pms-engine embed_font)

# Embed JetBrains Mono as a compiled byte array (used by the terminal panel)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/mono_font.h
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/embed_mono.sh
            ${CMAKE_SOURCE_DIR}/assets/fonts/JetBrainsMono-Regular.ttf
            ${CMAKE_BINARY_DIR}/mono_font.h
    DEPENDS
        ${CMAKE_SOURCE_DIR}/assets/fonts/JetBrainsMono-Regular.ttf
    COMMENT "Embedding JetBrains Mono font"
)
add_custom_target(embed_mono DEPENDS ${CMAKE_BINARY_DIR}/mono_font.h)
if(NOT PMS_ENGINE_ONLY)
add_dependencies(pop-maker-studio embed_mono)
endif()

# Embed all bundled display typefaces (typography presets) as one byte-array
# header with a master table. Add/remove a .ttf in assets/fonts/display and
# re-run CMake to pick it up.
file(GLOB DISPLAY_FONT_FILES ${CMAKE_SOURCE_DIR}/assets/fonts/display/*.ttf)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/display_fonts.h
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/embed_fonts_dir.sh
            ${CMAKE_SOURCE_DIR}/assets/fonts/display
            ${CMAKE_BINARY_DIR}/display_fonts.h
    DEPENDS ${DISPLAY_FONT_FILES}
    COMMENT "Embedding ${CMAKE_SOURCE_DIR}/assets/fonts/display display typefaces"
)
add_custom_target(embed_display DEPENDS ${CMAKE_BINARY_DIR}/display_fonts.h)
if(NOT PMS_ENGINE_ONLY)
add_dependencies(pop-maker-studio embed_display)
endif()

if(NOT PMS_ENGINE_ONLY)
target_include_directories(pop-maker-studio PRIVATE ${CMAKE_BINARY_DIR})
endif()
target_include_directories(pms-engine PRIVATE ${CMAKE_BINARY_DIR})

# Embed portrait preview photo as raw RGB
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/portrait_preview.h
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/embed_portrait.sh
            ${CMAKE_SOURCE_DIR}/assets/imgs/portrait.jpg
            ${CMAKE_BINARY_DIR}/portrait_preview.h
    DEPENDS ${CMAKE_SOURCE_DIR}/assets/imgs/portrait.jpg
            ${CMAKE_SOURCE_DIR}/scripts/embed_portrait.sh
    COMMENT "Embedding portrait preview image"
)
add_custom_target(embed_portrait DEPENDS ${CMAKE_BINARY_DIR}/portrait_preview.h)
if(NOT PMS_ENGINE_ONLY)
add_dependencies(pop-maker-studio embed_portrait)
endif()
add_dependencies(pms-engine embed_portrait)

# Embed the FX-preview motion source clip (CC BY 3.0 — see CREDITS) as a byte array
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/fx_motion_webm.h
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/embed_fx_motion.sh
            ${CMAKE_SOURCE_DIR}/assets/fx_preview_skater.webm
            ${CMAKE_BINARY_DIR}/fx_motion_webm.h
    DEPENDS ${CMAKE_SOURCE_DIR}/assets/fx_preview_skater.webm
            ${CMAKE_SOURCE_DIR}/scripts/embed_fx_motion.sh
    COMMENT "Embedding FX-preview motion source"
)
add_custom_target(embed_fx_motion DEPENDS ${CMAKE_BINARY_DIR}/fx_motion_webm.h)
if(NOT PMS_ENGINE_ONLY)
add_dependencies(pop-maker-studio embed_fx_motion)
endif()
add_dependencies(pms-engine embed_fx_motion)

# Embed the beauty-FX preview backdrop (assets/imgs/beauty_fx.jpg) as a 9:16
# source for face-centric effects (Skin Smooth, Glow Up)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/fx_face.h
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/embed_fx_face.sh
            ${CMAKE_SOURCE_DIR}/assets/imgs/beauty_fx.jpg
            ${CMAKE_BINARY_DIR}/fx_face.h
    DEPENDS ${CMAKE_SOURCE_DIR}/assets/imgs/beauty_fx.jpg
            ${CMAKE_SOURCE_DIR}/scripts/embed_fx_face.sh
    COMMENT "Embedding beauty-FX preview source"
)
add_custom_target(embed_fx_face DEPENDS ${CMAKE_BINARY_DIR}/fx_face.h)
if(NOT PMS_ENGINE_ONLY)
add_dependencies(pop-maker-studio embed_fx_face)
endif()
add_dependencies(pms-engine embed_fx_face)

# Embed base face photo as raw RGB — source for the camera brick's face-filter
# preview thumbnails (live landmark-warp render). Swap assets/imgs/face.jpg.
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/face_preview.h
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/embed_face.sh
            ${CMAKE_SOURCE_DIR}/assets/imgs/face.jpg
            ${CMAKE_BINARY_DIR}/face_preview.h
    DEPENDS ${CMAKE_SOURCE_DIR}/assets/imgs/face.jpg
            ${CMAKE_SOURCE_DIR}/scripts/embed_face.sh
    COMMENT "Embedding face-filter preview photo"
)
add_custom_target(embed_face DEPENDS ${CMAKE_BINARY_DIR}/face_preview.h)
add_dependencies(pms-engine embed_face)
if(NOT PMS_ENGINE_ONLY)
add_dependencies(pop-maker-studio embed_face)
endif()

# Guard: the in-app agent harness compiles src/generated/agent_tools.h, generated
# from mcp_server/server.py by tools/gen_agent_tools.py. If server.py changed
# without regenerating, fail the build so the harness can't silently ship a stale
# toolset. Regenerate with:  python3 tools/gen_agent_tools.py
# (Degrades to a no-op when the python mcp deps aren't installed — see --check.)
find_program(PYTHON3_EXECUTABLE NAMES python3 python)
if(PYTHON3_EXECUTABLE)
    add_custom_target(check_agent_tools ALL
        COMMAND ${PYTHON3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_agent_tools.py --check
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking agent_tools.h is in sync with mcp_server/server.py"
    )
if(NOT PMS_ENGINE_ONLY)
    add_dependencies(pop-maker-studio check_agent_tools)
endif()
endif()

if(NOT PMS_ENGINE_ONLY)
install(TARGETS pop-maker-studio DESTINATION bin)
endif()

# Copy bundled models to build dir so the binary can find them next to itself
if(NOT PMS_ENGINE_ONLY)
add_custom_command(TARGET pop-maker-studio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:pop-maker-studio>/models
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/models $<TARGET_FILE_DIR:pop-maker-studio>/models
    COMMENT "Copying bundled models to build dir"
)
endif()

# ── export-hubert CLI tool ────────────────────────────────────────────────────
add_executable(export-hubert
    tools/export_hubert.cpp
    src/pth_reader.cpp
    src/hubert_onnx.cpp
)

target_include_directories(export-hubert PRIVATE
    src
    ${AVCODEC_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
)

target_link_libraries(export-hubert PRIVATE
    ${AVCODEC_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    dl pthread
)

target_compile_options(export-hubert PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    $<$<CONFIG:Release>:-O2>
)

# ── forced_align_probe: standalone harness to verify alignment changes ────────
# Runs forced_align() against the real wav2vec2 model on a cached stem, without
# the whole transcription pipeline. See tools/forced_align_probe.cpp.
add_executable(forced_align_probe
    tools/forced_align_probe.cpp
    src/forced_align.cpp
    src/paths.cpp
)
target_include_directories(forced_align_probe PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/vendor
)
if(DEFINED ONNXRUNTIME_ROOT)
    target_include_directories(forced_align_probe PRIVATE ${ONNXRUNTIME_ROOT}/include)
    target_link_directories(forced_align_probe PRIVATE ${ONNXRUNTIME_ROOT}/lib)
else()
    target_include_directories(forced_align_probe PRIVATE /usr/include/onnxruntime)
endif()
target_link_libraries(forced_align_probe PRIVATE onnxruntime dl pthread)
target_compile_options(forced_align_probe PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)

# ── bg_remove_probe: standalone harness for brick-only background removal ─────
# Decodes the cached bg_masks.mjpeg + matching proxy.mjpeg and replays the
# RemoveBackground shader math (alpha = orig.a*mask + box + softness) on CPU —
# verifies the cutout end to end (mask load, mask↔frame index sync, box
# y-orientation, softness, degenerate masks) without GL or the live app.
# See tools/bg_remove_probe.cpp.
add_executable(bg_remove_probe tools/bg_remove_probe.cpp)
target_include_directories(bg_remove_probe PRIVATE ${CMAKE_SOURCE_DIR}/vendor)
target_link_libraries(bg_remove_probe PRIVATE m)
target_compile_options(bg_remove_probe PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)

# ── vc_cli: standalone voice conversion CLI (HuBERT pipeline) ──────────────────
add_executable(vc_cli
    tools/vc_cli.cpp
    src/vc_onnx.cpp
    src/rmvpe_onnx.cpp
    src/rvc_onnx.cpp
    src/pth_reader.cpp
    src/hubert_onnx.cpp
    src/faiss_ivf.cpp
    src/paths.cpp
)
target_include_directories(vc_cli PRIVATE
    src
    ${AVCODEC_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
)
if(DEFINED ONNXRUNTIME_ROOT)
    target_include_directories(vc_cli PRIVATE ${ONNXRUNTIME_ROOT}/include)
    target_link_directories(vc_cli PRIVATE ${ONNXRUNTIME_ROOT}/lib)
else()
    target_include_directories(vc_cli PRIVATE /usr/include/onnxruntime)
endif()
target_link_libraries(vc_cli PRIVATE
    ${AVCODEC_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    onnxruntime fftw3f dl pthread
)
target_compile_options(vc_cli PRIVATE
    -Wall -Wextra -Wno-unused-parameter -O2
)
