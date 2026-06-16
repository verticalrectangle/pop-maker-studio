cmake_minimum_required(VERSION 3.20)
project(PopMakerStudio VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(OpenGL REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(whisper CONFIG QUIET)
pkg_check_modules(GLFW REQUIRED glfw3)
pkg_check_modules(AVCODEC  REQUIRED libavcodec)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVUTIL   REQUIRED libavutil)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
pkg_check_modules(SWSCALE   REQUIRED libswscale)
pkg_check_modules(FREETYPE  REQUIRED freetype2)
pkg_check_modules(AUBIO     REQUIRED aubio)
pkg_check_modules(VTERM     REQUIRED vterm)
pkg_check_modules(TURBOJPEG REQUIRED libturbojpeg)
# Native low-latency audio backend (perf mode); optional — the miniaudio
# duplex path is the fallback when PipeWire is absent.
pkg_check_modules(PIPEWIRE libpipewire-0.3)

# ── ImGui ─────────────────────────────────────────────────────────────────────
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/vendor/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp
)

# ── Sources ───────────────────────────────────────────────────────────────────
set(SOURCES
    src/paths.cpp
    src/rmvpe_onnx.cpp
    src/runtime_fx.cpp
    src/ipc_server.cpp
    src/main.cpp
    src/app.cpp
    src/audio.cpp
    src/audio_pw.cpp
    src/face_track.cpp
    src/face_cache.cpp
    src/face_filters.cpp
    src/recorder.cpp
    src/video_recorder.cpp
    src/av_sync.cpp
    src/av_measure.cpp
    src/transcribe.cpp
    src/render.cpp
    src/history.cpp
    src/filepicker.cpp
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
    src/ui/screen_splash.cpp
    src/ui/screen_setup.cpp
    src/ui/screen_home.cpp
    src/terminal.cpp
    src/ui/screen_studio.cpp
    src/ui/studio_shared.cpp
    src/ui/canvas.cpp
    src/ui/timeline.cpp
    src/ui/pipeline.cpp
    src/ui/panel_clip.cpp
    src/ui/panel_animation.cpp
    src/ui/panel_fx.cpp
    src/ui/panel_media.cpp
    src/ui/export_ui.cpp
    src/ui/panel_terminal.cpp
    src/ui/panel_agent.cpp
    src/agent_harness.cpp
    src/ui/theme.cpp
    src/project.cpp
    src/fx_shader.cpp
    src/presets.cpp
    src/overlay_renderer.cpp
    src/text_renderer.cpp
)

add_executable(pop-maker-studio ${SOURCES} ${IMGUI_SOURCES})

if(PIPEWIRE_FOUND)
    target_compile_definitions(pop-maker-studio PRIVATE HAVE_PIPEWIRE)
endif()

target_include_directories(pop-maker-studio PRIVATE
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
    ${PIPEWIRE_INCLUDE_DIRS}
)

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
    fftw3f
    dl pthread
)

if(DEFINED ONNXRUNTIME_ROOT)
    target_include_directories(pop-maker-studio PRIVATE ${ONNXRUNTIME_ROOT}/include)
    target_link_directories(pop-maker-studio PRIVATE ${ONNXRUNTIME_ROOT}/lib)
else()
    target_include_directories(pop-maker-studio PRIVATE /usr/include/onnxruntime)
endif()

target_compile_definitions(pop-maker-studio PRIVATE IMGUI_ENABLE_FREETYPE)

target_compile_options(pop-maker-studio PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    $<$<CONFIG:Release>:-O2>
)

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
add_dependencies(pop-maker-studio embed_font)

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
add_dependencies(pop-maker-studio embed_mono)

target_include_directories(pop-maker-studio PRIVATE ${CMAKE_BINARY_DIR})

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
add_dependencies(pop-maker-studio embed_portrait)

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
add_dependencies(pop-maker-studio embed_face)

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
    add_dependencies(pop-maker-studio check_agent_tools)
endif()

install(TARGETS pop-maker-studio DESTINATION bin)

# Copy bundled models to build dir so the binary can find them next to itself
add_custom_command(TARGET pop-maker-studio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:pop-maker-studio>/models
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/models $<TARGET_FILE_DIR:pop-maker-studio>/models
    COMMENT "Copying bundled models to build dir"
)

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
