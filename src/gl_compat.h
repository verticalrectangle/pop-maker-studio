#pragma once
// gl_compat.h — one place for the OpenGL include dance (iOS Port Phase 2.1).
//
// Linux: Mesa headers want GL_GLEXT_PROTOTYPES for core-profile prototypes.
// macOS: Apple ships GL 4.1 core in OpenGL.framework with different header
// paths and no GLEXT define; GL_SILENCE_DEPRECATION acknowledges that Apple
// deprecated (but still ships) the whole API. This keeps the desktop GL
// renderer building on both — iOS gets no GL at all and uses the Metal
// RenderSurface instead (Playbook Phase 3).
#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
// iOS has no OpenGL at all — the GL renderer is compiled out under
// PMS_HEADLESS; gl_compat.h resolves to nothing here (Metal = Phase 3).
#else
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#include <OpenGL/gl3ext.h>
#endif
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#endif
