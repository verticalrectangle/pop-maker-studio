cmake_minimum_required(VERSION 3.20)
project(PopMakerStudio VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(OpenGL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLFW REQUIRED glfw3)
pkg_check_modules(AVCODEC  REQUIRED libavcodec)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVUTIL   REQUIRED libavutil)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
pkg_check_modules(SWSCALE   REQUIRED libswscale)
pkg_check_modules(FREETYPE  REQUIRED freetype2)

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
    src/main.cpp
    src/app.cpp
    src/audio.cpp
    src/transcribe.cpp
    src/stems.cpp
    src/render.cpp
    src/history.cpp
    src/filepicker.cpp
    src/video.cpp
    src/proxy.cpp
    src/waveform.cpp
    src/blender_export.cpp
    src/bg_remove.cpp
    src/noise_reduce.cpp
    src/ui/screen_splash.cpp
    src/ui/screen_setup.cpp
    src/ui/screen_studio.cpp
    src/ui/theme.cpp
    src/project.cpp
    src/fx_shader.cpp
    src/presets.cpp
)

add_executable(pop-maker-studio ${SOURCES} ${IMGUI_SOURCES})

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
    dl pthread
)

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
target_include_directories(pop-maker-studio PRIVATE ${CMAKE_BINARY_DIR})

# Embed portrait preview photo as raw RGB
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/portrait_preview.h
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/embed_portrait.sh
            ${CMAKE_SOURCE_DIR}/assets/imgs/portrait.jpg
            ${CMAKE_BINARY_DIR}/portrait_preview.h
    DEPENDS ${CMAKE_SOURCE_DIR}/assets/imgs/portrait.jpg
    COMMENT "Embedding portrait preview image"
)
add_custom_target(embed_portrait DEPENDS ${CMAKE_BINARY_DIR}/portrait_preview.h)
add_dependencies(pop-maker-studio embed_portrait)

# Embed ml_pipeline.py as a compiled byte array
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/ml_pipeline_embedded.h
    COMMAND ${CMAKE_COMMAND}
        -DINPUT=${CMAKE_SOURCE_DIR}/ml_pipeline.py
        -DVAR=ml_pipeline_py
        -DOUTPUT=${CMAKE_BINARY_DIR}/ml_pipeline_embedded.h
        -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/ml_pipeline.py
    COMMENT "Embedding ml_pipeline.py"
)
add_custom_target(embed_pipeline DEPENDS ${CMAKE_BINARY_DIR}/ml_pipeline_embedded.h)
add_dependencies(pop-maker-studio embed_pipeline)

# Embed ml_prefetch.py as a compiled byte array
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/ml_prefetch_embedded.h
    COMMAND ${CMAKE_COMMAND}
        -DINPUT=${CMAKE_SOURCE_DIR}/ml_prefetch.py
        -DVAR=ml_prefetch_py
        -DOUTPUT=${CMAKE_BINARY_DIR}/ml_prefetch_embedded.h
        -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/ml_prefetch.py
    COMMENT "Embedding ml_prefetch.py"
)
add_custom_target(embed_prefetch DEPENDS ${CMAKE_BINARY_DIR}/ml_prefetch_embedded.h)
add_dependencies(pop-maker-studio embed_prefetch)

# Embed ml_setup.py as a compiled byte array
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/ml_setup_embedded.h
    COMMAND ${CMAKE_COMMAND}
        -DINPUT=${CMAKE_SOURCE_DIR}/ml_setup.py
        -DVAR=ml_setup_py
        -DOUTPUT=${CMAKE_BINARY_DIR}/ml_setup_embedded.h
        -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/ml_setup.py
    COMMENT "Embedding ml_setup.py"
)
add_custom_target(embed_setup DEPENDS ${CMAKE_BINARY_DIR}/ml_setup_embedded.h)
add_dependencies(pop-maker-studio embed_setup)

# Embed beat_detect.py
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/beat_detect_embedded.h
    COMMAND ${CMAKE_COMMAND}
        -DINPUT=${CMAKE_SOURCE_DIR}/beat_detect.py
        -DVAR=beat_detect_py
        -DOUTPUT=${CMAKE_BINARY_DIR}/beat_detect_embedded.h
        -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/beat_detect.py
    COMMENT "Embedding beat_detect.py"
)
add_custom_target(embed_beat_detect DEPENDS ${CMAKE_BINARY_DIR}/beat_detect_embedded.h)
add_dependencies(pop-maker-studio embed_beat_detect)

# Embed rembg_remove.py
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/rembg_remove_embedded.h
    COMMAND ${CMAKE_COMMAND}
        -DINPUT=${CMAKE_SOURCE_DIR}/rembg_remove.py
        -DVAR=rembg_remove_py
        -DOUTPUT=${CMAKE_BINARY_DIR}/rembg_remove_embedded.h
        -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/rembg_remove.py
    COMMENT "Embedding rembg_remove.py"
)
add_custom_target(embed_rembg DEPENDS ${CMAKE_BINARY_DIR}/rembg_remove_embedded.h)
add_dependencies(pop-maker-studio embed_rembg)

# Embed noise_reduce.py
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/noise_reduce_embedded.h
    COMMAND ${CMAKE_COMMAND}
        -DINPUT=${CMAKE_SOURCE_DIR}/noise_reduce.py
        -DVAR=noise_reduce_py
        -DOUTPUT=${CMAKE_BINARY_DIR}/noise_reduce_embedded.h
        -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/noise_reduce.py
    COMMENT "Embedding noise_reduce.py"
)
add_custom_target(embed_noise_reduce DEPENDS ${CMAKE_BINARY_DIR}/noise_reduce_embedded.h)
add_dependencies(pop-maker-studio embed_noise_reduce)

# Embed envelope_extract.py
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/envelope_extract_embedded.h
    COMMAND ${CMAKE_COMMAND}
        -DINPUT=${CMAKE_SOURCE_DIR}/envelope_extract.py
        -DVAR=envelope_extract_py
        -DOUTPUT=${CMAKE_BINARY_DIR}/envelope_extract_embedded.h
        -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/envelope_extract.py
    COMMENT "Embedding envelope_extract.py"
)
add_custom_target(embed_envelope DEPENDS ${CMAKE_BINARY_DIR}/envelope_extract_embedded.h)
add_dependencies(pop-maker-studio embed_envelope)

# ── Embed Python 3.11 standalone runtime (multiplatform) ─────────────────────
set(PYTHON_STANDALONE_VERSION "3.11.15")
set(PYTHON_STANDALONE_DATE    "20260414")

# Resolve platform + arch suffix for python-build-standalone
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(_PY_PLATFORM "aarch64-unknown-linux-gnu")
    else()
        set(_PY_PLATFORM "x86_64-unknown-linux-gnu")
    endif()
    set(_PY_EXT "tar.gz")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_PY_PLATFORM "aarch64-apple-darwin")
    else()
        set(_PY_PLATFORM "x86_64-apple-darwin")
    endif()
    set(_PY_EXT "tar.gz")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(_PY_PLATFORM "x86_64-pc-windows-msvc-shared")
    set(_PY_EXT "zip")
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

set(PYTHON_STANDALONE_URL
    "https://github.com/indygreg/python-build-standalone/releases/download/${PYTHON_STANDALONE_DATE}/cpython-${PYTHON_STANDALONE_VERSION}+${PYTHON_STANDALONE_DATE}-${_PY_PLATFORM}-install_only_stripped.${_PY_EXT}")
set(PYTHON_STANDALONE_TARBALL "${CMAKE_BINARY_DIR}/python311.${_PY_EXT}")

if(NOT EXISTS "${PYTHON_STANDALONE_TARBALL}")
    message(STATUS "Downloading Python ${PYTHON_STANDALONE_VERSION} (${_PY_PLATFORM}, ~31 MB)…")
    file(DOWNLOAD "${PYTHON_STANDALONE_URL}" "${PYTHON_STANDALONE_TARBALL}"
         SHOW_PROGRESS STATUS _dl_status)
    list(GET _dl_status 0 _dl_code)
    if(NOT _dl_code EQUAL 0)
        message(FATAL_ERROR "Failed to download Python standalone: ${_dl_status}")
    endif()
endif()

set(PYTHON_BLOB_OBJ "${CMAKE_BINARY_DIR}/python311_blob.o")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Use objcopy to embed directly into an ELF object — fast, no C parsing.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(_OBJCOPY_ARCH "aarch64")
        set(_OBJCOPY_OUT  "elf64-littleaarch64")
    else()
        set(_OBJCOPY_ARCH "i386:x86-64")
        set(_OBJCOPY_OUT  "elf64-x86-64")
    endif()
    add_custom_command(
        OUTPUT  "${PYTHON_BLOB_OBJ}"
        COMMAND objcopy
                --input  binary
                --output ${_OBJCOPY_OUT}
                --binary-architecture ${_OBJCOPY_ARCH}
                "python311.${_PY_EXT}"
                "${PYTHON_BLOB_OBJ}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        DEPENDS "${PYTHON_STANDALONE_TARBALL}"
        COMMENT "Embedding Python 3.11 tarball (objcopy, Linux)"
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # ld -r -sectcreate embeds a file as a Mach-O section.
    # Access via getsectbyname("__DATA","__python_tar") at runtime.
    add_custom_command(
        OUTPUT  "${PYTHON_BLOB_OBJ}"
        COMMAND ld -r -sectcreate __DATA __python_tar
                    "python311.${_PY_EXT}"
                    -o "${PYTHON_BLOB_OBJ}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        DEPENDS "${PYTHON_STANDALONE_TARBALL}"
        COMMENT "Embedding Python 3.11 tarball (ld sectcreate, macOS)"
    )
else()
    # Windows: fall back to the portable CMake C-array embedder.
    set(PYTHON_BLOB_OBJ "")   # no .o; use a generated header instead
    set(PYTHON_BLOB_H "${CMAKE_BINARY_DIR}/python311_embedded.h")
    add_custom_command(
        OUTPUT  "${PYTHON_BLOB_H}"
        COMMAND ${CMAKE_COMMAND}
                -DINPUT="${PYTHON_STANDALONE_TARBALL}"
                -DVAR=python311_zip
                -DOUTPUT="${PYTHON_BLOB_H}"
                -P "${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake"
        DEPENDS "${PYTHON_STANDALONE_TARBALL}"
        COMMENT "Embedding Python 3.11 zip (C array, Windows)"
    )
    add_custom_target(embed_python_win DEPENDS "${PYTHON_BLOB_H}")
    add_dependencies(pop-maker-studio embed_python_win)
    target_compile_definitions(pop-maker-studio PRIVATE PYTHON_EMBED_CARRAY=1)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_custom_target(embed_python DEPENDS "${PYTHON_BLOB_OBJ}")
    add_dependencies(pop-maker-studio embed_python)
    target_link_libraries(pop-maker-studio PRIVATE "${PYTHON_BLOB_OBJ}")
endif()

install(TARGETS pop-maker-studio DESTINATION bin)
