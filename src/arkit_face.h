#pragma once
// ARKit face-tracked observation slot. Shared between the C ABI
// (pms_submit_arkit_face), the render backend (metal_render.mm), and the
// ARKit-aware render plan builder (face_filters.cpp).
#include "generated/arkit_face_mesh.h"  // ARKIT_NPTS, ARKIT_NTRI, k_arkit_*
#include "generated/face_uv_mesh.h"    // FACE_UV_NPTS

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

// Build (if needed) and copy out MediaPipe-mesh screen positions for each of
// the 468 MediaPipe vertices, looked up from the ARKit mesh via a cached
// UV-space correspondence. The mapping (MediaPipe index → ARKit vertex index)
// is built once from the ~46 hard-mapped landmark pairs as IDW control points
// in MediaPipe UV space: each MediaPipe UV is mapped to an ARKit UV, then
// snapped to the nearest ARKit vertex in ARKit UV space. ARKit
// textureCoordinates and MediaPipe canonical UVs are both constant per
// topology, so the mapping is stable across frames and faces.
//
// This gives the MediaPipe mesh (exact UVs for makeup PNGs) real ARKit
// screen positions (no IDW position interpolation) — both UVs and positions
// are correct. Returns false if ARKit UVs are not yet available (all zeros),
// meaning the caller should fall back to IDW-interpolated positions.
bool arkit_face_mp_positions(const ARKitFaceObs& obs, float out[FACE_UV_NPTS][2]);
