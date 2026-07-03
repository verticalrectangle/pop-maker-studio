#pragma once
// platform.h — compile-time capability switches for the port.
//
// PMS_HEADLESS (set by CMake -DPMS_HEADLESS=ON, or auto on iOS) builds the
// engine with NO ffmpeg, NO OpenGL, NO fftw, NO process spawning — the media
// decode/encode and GL render paths become signature-preserving stubs so the
// static lib LINKS on platforms that forbid those (iOS: no ffmpeg binary, no
// GL, no fork/exec). Real impls arrive with the Metal RenderSurface (Phase 3)
// and AVFoundation MediaBackend (Phase 4). Desktop (Linux/macOS) leaves all
// capabilities ON — unchanged behavior.
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#  ifndef PMS_HEADLESS
#    define PMS_HEADLESS 1
#  endif
#endif
#endif

#if defined(PMS_HEADLESS) && PMS_HEADLESS
#  define PMS_HAS_FFMPEG 0
#  define PMS_HAS_GL     0
#  define PMS_HAS_FFTW   0
#  define PMS_HAS_SPAWN  0
#else
#  define PMS_HAS_FFMPEG 1
#  define PMS_HAS_GL     1
#  define PMS_HAS_FFTW   1
#  define PMS_HAS_SPAWN  1
#endif
