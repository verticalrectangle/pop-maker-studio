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
pkg_check_modules(SWSCALE  REQUIRED libswscale)

# ── ImGui ─────────────────────────────────────────────────────────────────────
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/vendor/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# ── Sources ───────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/app.cpp
    src/audio.cpp
    src/transcribe.cpp
    src/stems.cpp
    src/render.cpp
    src/filepicker.cpp
    src/video.cpp
    src/blender_export.cpp
    src/ui/screen_splash.cpp
    src/ui/screen_studio.cpp
    src/ui/theme.cpp
)

add_executable(pop-maker-studio ${SOURCES} ${IMGUI_SOURCES})

target_include_directories(pop-maker-studio PRIVATE
    src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_SOURCE_DIR}/vendor
    ${GLFW_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
)

target_link_libraries(pop-maker-studio PRIVATE
    OpenGL::GL
    ${GLFW_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    dl pthread
)

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

# Copy ml_pipeline.py next to the binary so the app can find it
add_custom_command(TARGET pop-maker-studio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/ml_pipeline.py
        $<TARGET_FILE_DIR:pop-maker-studio>/ml_pipeline.py
)

install(TARGETS pop-maker-studio DESTINATION bin)
install(FILES ml_pipeline.py DESTINATION bin)
