#pragma once
// ARKit face-tracked observation slot. Shared between the C ABI
// (pms_submit_arkit_face), the render backend (metal_render.mm), and the
// ARKit-aware render plan builder (face_filters.cpp).
#include "generated/arkit_face_mesh.h"  // ARKIT_NPTS, ARKIT_NTRI, k_arkit_*

static constexpr int ARKIT_NBLEND = 52;
static constexpr int ARKIT_MAX_FACES = 4;

struct ARKitFaceObs {
    bool   valid = false;
    float  pts[ARKIT_NPTS][2];    // 2D pixel coords in the submitted frame
    float  uvs[ARKIT_NPTS][2];    // ARKit textureCoordinates (constant topology)
    float  blend[ARKIT_NBLEND] = {}; // ARKit blendshape coefficients
    bool   has_blend = false;
    float  score = 1.0f;          // ARKit always reports confidence
    int    w = 0, h = 0;          // frame size the pts live in
};

// Submit a fresh ARKit face observation (up to ARKIT_MAX_FACES faces).
// n_faces=0 clears the slot. Thread-safe.
void arkit_face_submit(const ARKitFaceObs* obs, int n_faces);
// Take the latest ARKit observations. Returns the cached faces (up to max_n)
// whenever n_faces > 0, regardless of freshness — ARKit delivers face anchors
// asynchronously on its own delegate queue, and the render loop polls every
// frame, so gating on a consumed "fresh" flag caused frames with no makeup
// between deliveries (flicker). Face loss is signaled by arkit_face_submit
// with null/0, which clears n_faces. Thread-safe.
int arkit_face_take(ARKitFaceObs* out, int max_n);

// True if fresh ARKit data is available and has not yet been consumed.
bool arkit_face_available();

// Clear the ARKit slot.
void arkit_face_clear();

// Staleness guard: pms_submit_camera_frame notes each frame's host time and
// arkit_face_submit stamps the current one. arkit_face_take returns 0 when
// the newest camera frame is >0.15s past the last ARKit submission — a
// stalled anchor stream (fast motion, tracking loss) must never keep
// painting frozen landmarks onto fresh video.
void arkit_face_note_camera_time(double host_time);

// ── Native 3D path (tier-1 rewrite) ─────────────────────────────────────
// Full ARKit face state: model-space vertices + the transform chain + eye
// poses. The engine renders the ARKit mesh itself with these matrices —
// no 2D projection in Swift, no landmark correspondence in the render.
struct ARKitFace3D {
    bool  valid = false;
    bool  has_blend = false;
    float verts[ARKIT_NPTS][3];   // face-anchor model space (meters)
    float model[16];              // anchor transform  (column-major)
    float view[16];               // camera view matrix
    float proj[16];               // projection for the portrait viewport
    float eye_l[16], eye_r[16];   // eyeball transforms (anchor space)
    float blend[ARKIT_NBLEND];
    int   w = 0, h = 0;           // viewport the projection targets
};

void arkit_face3d_submit(const ARKitFace3D* f);   // null/!valid clears
bool arkit_face3d_take(ARKitFace3D* out);         // false when empty/stale
