#pragma once
// canvas.h — preview canvas rendering

#include "studio_types.h"
#include "app.h"
#include "../face_track.h"
#include "../engine_seams.h"   // CanvasHandleGeom / MirrorDebugGeom + accessors
#include <imgui.h>

void draw_preview(AppState& state, ImVec2 p, float w, float h);

// "source: canvas" snapshot: reads the preview rect from the back buffer after
// ImGui has rendered — the exact pixels the user sees. Call from the main loop
// between ImGui_ImplOpenGL3_RenderDrawData() and glfwSwapBuffers(). No-op
// unless draw_preview armed a capture this frame.
void canvas_capture_after_render(AppState& state);
void draw_canvas_handles(AppState& state, ImDrawList* dl, ImVec2 p, float w, float h);
// Request pen-draw mode for the currently-selected Shape clip (entered on the
// next draw_canvas_handles frame). Also entered by pressing 'P' with a Shape
// clip selected. No-op if the selection isn't a Shape clip.
void canvas_request_shape_pen();
// True while the canvas pen-draw mode is active (so the inspector can badge
// its "Edit Path" button).
bool canvas_shape_pen_active();

// Transform-handle + camera-mirror debug geometry types and accessors are
// declared in engine_seams.h (the IPC dispatcher — engine side — consumes
// them; canvas.cpp — app side — fills them).

// s_scrub_until — owned here, read by screen_studio coordinator
extern double s_scrub_until;
