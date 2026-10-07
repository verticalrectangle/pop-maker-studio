#pragma once
// ARKit face slot (iOS TrueDepth front camera). The Swift capture submits one
// ARFrame's full face state — model-space mesh, transform chain, blendshapes,
// light estimate — through pms_submit_arkit_face_3d; the Metal makeup
// renderer (arkit_makeup.mm) takes the newest state every render. The engine
// draws ARKit's own mesh with ARKit's own camera: no 2D projection and no
// landmark correspondence anywhere on this path.
#include "generated/arkit_face_mesh.h"  // ARKIT_NPTS, ARKIT_NTRI, k_arkit_uv/tris

static constexpr int ARKIT_NBLEND = 52;

// ARFrame.lightEstimate. Face tracking delivers an ARDirectionalLightEstimate
// (primary light + 2nd-order spherical harmonics); `directional` is false when
// only the ambient terms are available.
struct ARKitLight {
    bool  directional = false;
    float dir[3] = {0.f, 0.f, -1.f};  // world space; the direction light travels
    float intensity = 0.f;            // primary light, lumens
    float sh[27] = {};                // ARKit sphericalHarmonicsCoefficients
    float ambient = 1000.f;           // lumens (1000 = neutral)
    float kelvin = 6500.f;            // ambient color temperature
};

struct ARKitFace3D {
    bool  valid = false;
    bool  has_blend = false;
    float verts[ARKIT_NPTS][3];   // face-anchor model space (meters)
    float model[16];              // anchor transform (column-major)
    float view[16];               // camera view matrix (portrait)
    float proj[16];               // projection for the portrait viewport
    float blend[ARKIT_NBLEND];    // MediaPipe blendshape order (_neutral = 0)
    ARKitLight light;
    int   w = 0, h = 0;           // viewport the projection targets
};

// Staleness guard: pms_submit_camera_frame notes each frame's host time and
// arkit_face3d_submit stamps the newest one. arkit_face3d_take returns false
// when the newest camera frame is >0.15 s past the last face submission — a
// stalled anchor stream (fast motion, tracking loss) must never keep painting
// frozen geometry onto fresh video.
void arkit_face_note_camera_time(double host_time);
void arkit_face3d_submit(const ARKitFace3D* f);   // null / !valid clears
bool arkit_face3d_take(ARKitFace3D* out);         // false when empty or stale

// Debug overlay for the makeup renderer (the "face_overlay" IPC command):
// draws the mesh as a UV checker plus the live lash-line polylines — on-device
// alignment QA.
extern bool g_face_overlay;