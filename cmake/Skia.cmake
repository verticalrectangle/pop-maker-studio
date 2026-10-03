# Skia (Canvas 2D backend for Script clips, docs/SCRIPT_API.md §4).
#
# Prebuilt static Skia from the aseprite/skia release builds (Ganesh GL,
# SkShaper + HarfBuzz, ICU). Pass -DPMS_SKIA_DIR=<unzipped release> to use a
# local copy; otherwise the pinned release is downloaded (hash-checked) into
# the build tree. FreeType and HarfBuzz come from the system (the same
# libraries ImGui links), so the bundled copies in the release are not used.
#
# Defines the INTERFACE target `pms_skia`.

set(PMS_SKIA_RELEASE "m151-a90155cff0")

if(NOT PMS_SKIA_DIR)
    if(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            set(_skia_asset "Skia-macOS-Release-arm64.zip")
            set(_skia_sha256 "c930944180c14cc7b5de4e3d1fe63efad8b9e1e1edcd7709abafa696e44b4d42")
        else()
            set(_skia_asset "Skia-macOS-Release-x64.zip")
            set(_skia_sha256 "c82392a634a86ec9307cb445f00472a482621a94753ad9271251e6cc594ccccf")
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        set(_skia_asset "Skia-Linux-Release-x64.zip")
        set(_skia_sha256 "456ba97a057338a8d58631a962f8012dc7ecba580713034bb351d223fed5f9e2")
    else()
        message(FATAL_ERROR "No prebuilt Skia for ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}; "
                            "build Skia and pass -DPMS_SKIA_DIR=<dir>")
    endif()
    include(FetchContent)
    FetchContent_Declare(pms_skia_prebuilt
        URL "https://github.com/aseprite/skia/releases/download/${PMS_SKIA_RELEASE}/${_skia_asset}"
        URL_HASH SHA256=${_skia_sha256}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(pms_skia_prebuilt)
    set(PMS_SKIA_DIR "${pms_skia_prebuilt_SOURCE_DIR}")
endif()

if(APPLE)
    set(_skia_libdir "${PMS_SKIA_DIR}/out/Release-${CMAKE_SYSTEM_PROCESSOR}")
    if(NOT EXISTS "${_skia_libdir}")
        set(_skia_libdir "${PMS_SKIA_DIR}/out/Release-arm64")
    endif()
else()
    set(_skia_libdir "${PMS_SKIA_DIR}/out/Release-x64")
endif()
if(NOT EXISTS "${_skia_libdir}/libskia.a")
    message(FATAL_ERROR "Skia static libs not found in ${_skia_libdir}")
endif()

pkg_check_modules(HARFBUZZ REQUIRED harfbuzz)

add_library(pms_skia INTERFACE)
target_include_directories(pms_skia SYSTEM INTERFACE "${PMS_SKIA_DIR}")
# Link order matters for static archives: users before providers.
# On Linux the prebuilt's GrGLMakeGLXInterface.o references glXGetProcAddress /
# glXGetCurrentContext; GLX lives in libGL (already linked via OpenGL::GL on
# desktop targets), but pms_skia must carry it so static-only consumers
# (engine-smoke, headless) also resolve it.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(OpenGL REQUIRED COMPONENTS OpenGL GLX)
endif()
target_link_libraries(pms_skia INTERFACE
    "${_skia_libdir}/libskshaper.a"
    "${_skia_libdir}/libskunicode_icu.a"
    "${_skia_libdir}/libskunicode_core.a"
    "${_skia_libdir}/libicu.a"
    "${_skia_libdir}/libskia.a"
    # AFTER libskia.a: its GrGLMakeGLXInterface.o needs glXGetProcAddress /
    # glXGetCurrentContext. Order matters for --as-needed linkers (Ubuntu):
    # a GLX appearing only before libskia.a gets dropped before the archive
    # that needs it is seen. (Locally OpenGL::GL dragged GLX in late by luck.)
    $<$<STREQUAL:${CMAKE_SYSTEM_NAME},Linux>:OpenGL::GLX>
    ${HARFBUZZ_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    ${CMAKE_DL_LIBS})
target_link_directories(pms_skia INTERFACE ${HARFBUZZ_LIBRARY_DIRS} ${FREETYPE_LIBRARY_DIRS})
# Must match the flags the prebuilt was compiled with (SkUserConfig defaults +
# the GN defines that change class layouts).
target_compile_definitions(pms_skia INTERFACE
    SK_GANESH SK_GL SK_SHAPER_HARFBUZZ_AVAILABLE SK_SHAPER_UNICODE_AVAILABLE
    SK_UNICODE_AVAILABLE SK_UNICODE_ICU_IMPLEMENTATION SK_RELEASE)
