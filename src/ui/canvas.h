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
void compute_video_bbox(AppState& state, Clip& cl, ImVec2 p, float w, float h,
                        float& bx0, float& by0, float& bx1, float& by1);

// Transform-handle + camera-mirror debug geometry types and accessors are
// declared in engine_seams.h (the IPC dispatcher — engine side — consumes
// them; canvas.cpp — app side — fills them).

// s_scrub_until — owned here, read by screen_studio coordinator
extern double s_scrub_until;
