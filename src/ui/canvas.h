#pragma once
// canvas.h — preview canvas rendering

#include "studio_types.h"
#include "app.h"
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

// Transform-handle geometry drawn last frame for the selected clip (screen
// px). Lets IPC clients (agents driving the UI via ui_input) find the rotate
// knob / bbox without pixel-hunting a screenshot. valid=false when no
// handles were drawn (nothing selected, crop mode, text clip).
struct CanvasHandleGeom {
    bool  valid = false;
    float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;  // selection bbox
    float rot_x = 0, rot_y = 0;                // rotate knob center
    float cx = 0, cy = 0;                      // rotation pivot (clip center)
};
CanvasHandleGeom canvas_handle_geom();

// Camera-mirror geometry + face landmarks from the last drawn frame. Lets the
// face-filter test rig predict overlay screen positions numerically instead
// of pixel-hunting screenshots. Landmarks are raw full-res frame coords
// (post rotation remap), i.e. exactly what the doggy overlay consumes.
struct MirrorDebugGeom {
    bool  valid = false;
    float cx = 0, cy = 0;       // mirror quad center, screen px
    float hw = 0, hh = 0;       // half extents of the unrotated quad, screen px
    float rot_deg = 0;          // brick rotation (screen draw uses -rot)
    int   cam_w = 0, cam_h = 0; // raw frame size
    bool  face_valid = false;
    float pts[106][2];          // raw-frame landmark px
};
MirrorDebugGeom mirror_debug_geom();

// s_scrub_until — owned here, read by screen_studio coordinator
extern double s_scrub_until;
