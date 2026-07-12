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
    float  blend[ARKIT_NBLEND] = {}; // ARKit blendshape coefficients
    bool   has_blend = false;
    float  score = 1.0f;          // ARKit always reports confidence
    int    w = 0, h = 0;          // frame size the pts live in
};

// Submit a fresh ARKit face observation (up to ARKIT_MAX_FACES faces).
// n_faces=0 clears the slot. Thread-safe.
void arkit_face_submit(const ARKitFaceObs* obs, int n_faces);

// Take the latest ARKit observations. If fresh, copies them to `out` (up to
// max_n), marks the slot consumed, and returns the count written. If stale,
// returns 0. Thread-safe.
int arkit_face_take(ARKitFaceObs* out, int max_n);

// True if fresh ARKit data is available and has not yet been consumed.
bool arkit_face_available();

// Clear the ARKit slot.
void arkit_face_clear();
