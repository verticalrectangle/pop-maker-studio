#include "arkit_face.h"
#include "face_filters.h"
#include "generated/arkit_mp_map.h"
#include <mutex>

struct ARKitFaceSlot {
    std::mutex mtx;
    ARKitFaceObs faces[ARKIT_MAX_FACES];
    int n_faces = 0;
    bool fresh = false;
};

static ARKitFaceSlot g_arkit_slot;

void arkit_face_submit(const ARKitFaceObs* obs, int n_faces) {
    std::lock_guard<std::mutex> lk(g_arkit_slot.mtx);
    g_arkit_slot.fresh = false;
    g_arkit_slot.n_faces = 0;
    if (!obs || n_faces <= 0) return;
    int n = n_faces;
    if (n > ARKIT_MAX_FACES) n = ARKIT_MAX_FACES;
    for (int i = 0; i < n; ++i) {
        g_arkit_slot.faces[i] = obs[i];
        g_arkit_slot.faces[i].valid = true;
    }
    g_arkit_slot.n_faces = n;
    g_arkit_slot.fresh = true;
}

int arkit_face_take(ARKitFaceObs* out, int max_n) {
    if (!out || max_n <= 0) return 0;
    std::lock_guard<std::mutex> lk(g_arkit_slot.mtx);
    if (g_arkit_slot.n_faces <= 0) return 0;
    int n = g_arkit_slot.n_faces;
    if (n > max_n) n = max_n;
    for (int i = 0; i < n; ++i) out[i] = g_arkit_slot.faces[i];
    return n;
}

bool arkit_face_available() {
    std::lock_guard<std::mutex> lk(g_arkit_slot.mtx);
    return g_arkit_slot.fresh && g_arkit_slot.n_faces > 0;
}

void arkit_face_clear() {
    std::lock_guard<std::mutex> lk(g_arkit_slot.mtx);
    g_arkit_slot.fresh = false;
    g_arkit_slot.n_faces = 0;
}

// Build a MediaPipe-format FaceRenderPlan from an ARKit observation.
//
// All 478 MediaPipe landmark positions are evaluated EXACTLY from the
// projected ARKit mesh via k_mp_from_arkit — static barycentric weights
// computed offline from the two canonical rest-pose meshes
// (tools/gen_arkit_mp_map.py). L/R is anatomical on both sides of the
// correspondence, so it is independent of preview mirroring. Downstream
// rendering is pure MediaPipe topology (hole-free — ARKit's oversized eye
// cutouts would leave the under-eye concealer zone unpainted);
// has_arkit_mesh only marks the plan as latency-free for the blink fade.
bool face_filter_build_plan_from_arkit(const BeautyLook& L, float amount,
                                       const ARKitFaceObs& obs, int w, int h,
                                       FaceRenderPlan& out) {
    if (w <= 0 || h <= 0 || !obs.valid) return false;

    FaceObs mp_obs{};
    mp_obs.valid = true;
    mp_obs.w = obs.w;
    mp_obs.h = obs.h;
    mp_obs.score = obs.score;
    mp_obs.has_blend = obs.has_blend;
    for (int i = 0; i < FT_NBLEND; ++i) mp_obs.blend[i] = obs.blend[i];

    for (int i = 0; i < FT_NPTS; ++i) {
        const ArkitMpBary& b = k_mp_from_arkit[i];
        mp_obs.pts[i][0] = b.w0 * obs.pts[b.i0][0] + b.w1 * obs.pts[b.i1][0]
                         + b.w2 * obs.pts[b.i2][0];
        mp_obs.pts[i][1] = b.w0 * obs.pts[b.i0][1] + b.w1 * obs.pts[b.i1][1]
                         + b.w2 * obs.pts[b.i2][1];
    }

    bool ok = face_filter_build_plan_look(L, amount, mp_obs, w, h, out);
    if (ok && out.makeup_tex)
        out.has_arkit_mesh = true;
    return ok;
}
