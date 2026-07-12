#include "arkit_face.h"
#include "face_filters.h"
#include "generated/arkit_landmark_map.h"
#include <cstring>
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
    if (!g_arkit_slot.fresh || g_arkit_slot.n_faces <= 0) return 0;
    int n = g_arkit_slot.n_faces;
    if (n > max_n) n = max_n;
    for (int i = 0; i < n; ++i) out[i] = g_arkit_slot.faces[i];
    g_arkit_slot.fresh = false;
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

// Map the MediaPipe indices used by face_filter_build_plan_look to the
// equivalent ARKit mesh vertex indices. Missing/unmapped indices return 0.
static int arkit_index_for_mp(int mp) {
    switch (mp) {
        case 0: return ARKIT_LIP_RING[3];
        case 1: return ARKIT_NOSE_TIP;
        case 10: return ARKIT_FOREHEAD;
        case 13: return ARKIT_LIP_MID_L;
        case 14: return ARKIT_LIP_MID_R;
        case 17: return ARKIT_LIP_RING[9];
        case 33: return ARKIT_LID_L[0];
        case 37: return ARKIT_LIP_RING[2];
        case 40: return ARKIT_LIP_RING[1];
        case 50: return ARKIT_CHEEK_L;
        case 61: return ARKIT_LIP_RING[0];
        case 84: return ARKIT_LIP_RING[10];
        case 91: return ARKIT_LIP_RING[11];
        case 98: return ARKIT_NOSE_L;
        case 132: return ARKIT_JAW_CHAIN_L[0];
        case 133: return ARKIT_LID_L[6];
        case 136: return ARKIT_JAW_CHAIN_L[2];
        case 149: return ARKIT_JAW_CHAIN_L[3];
        case 152: return ARKIT_CHIN;
        case 157: return ARKIT_LID_L[5];
        case 158: return ARKIT_LID_L[4];
        case 159: return ARKIT_LID_L[3];
        case 160: return ARKIT_LID_L[2];
        case 161: return ARKIT_LID_L[1];
        case 172: return ARKIT_JAW_CHAIN_L[1];
        case 176: return ARKIT_JAW_CHAIN_L[4];
        case 234: return ARKIT_FACE_L;
        case 263: return ARKIT_LID_R[0];
        case 267: return ARKIT_LIP_RING[4];
        case 270: return ARKIT_LIP_RING[5];
        case 280: return ARKIT_CHEEK_R;
        case 291: return ARKIT_LIP_RING[6];
        case 314: return ARKIT_LIP_RING[8];
        case 321: return ARKIT_LIP_RING[7];
        case 327: return ARKIT_NOSE_R;
        case 361: return ARKIT_JAW_CHAIN_R[0];
        case 362: return ARKIT_LID_R[6];
        case 365: return ARKIT_JAW_CHAIN_R[2];
        case 378: return ARKIT_JAW_CHAIN_R[3];
        case 384: return ARKIT_LID_R[5];
        case 385: return ARKIT_LID_R[4];
        case 386: return ARKIT_LID_R[3];
        case 387: return ARKIT_LID_R[2];
        case 388: return ARKIT_LID_R[1];
        case 397: return ARKIT_JAW_CHAIN_R[1];
        case 400: return ARKIT_JAW_CHAIN_R[4];
        case 454: return ARKIT_FACE_R;
        case 468: return ARKIT_IRIS_L;
        case 473: return ARKIT_IRIS_R;
        default: return 0;
    }
}

// Build an ARKit-aware render plan by translating the ARKit mesh into the
// MediaPipe coordinate system that face_filter_build_plan_look expects, then
// copying the full 1220-pt mesh into the ARKit plan.
bool face_filter_build_plan_arkit(const BeautyLook& L, float amount,
                                  const ARKitFaceObs& obs, int w, int h,
                                  ARKitFaceRenderPlan& out) {
    out = ARKitFaceRenderPlan{};
    if (w <= 0 || h <= 0 || !obs.valid) return false;

    FaceObs mp_obs{};
    mp_obs.valid = true;
    mp_obs.w = obs.w;
    mp_obs.h = obs.h;
    mp_obs.score = obs.score;
    mp_obs.has_blend = obs.has_blend;
    for (int i = 0; i < FT_NBLEND; ++i) mp_obs.blend[i] = obs.blend[i];

    for (int i = 0; i < FT_NPTS; ++i) {
        int ai = arkit_index_for_mp(i);
        if (ai > 0 && ai < ARKIT_NPTS) {
            mp_obs.pts[i][0] = obs.pts[ai][0];
            mp_obs.pts[i][1] = obs.pts[ai][1];
        }
    }

    FaceRenderPlan mp_plan;
    if (!face_filter_build_plan_look(L, amount, mp_obs, w, h, mp_plan) || !mp_plan.valid)
        return false;

    out.valid = true;
    out.has_beauty = mp_plan.has_beauty;
    out.beauty = mp_plan.beauty;
    out.makeup_tex = mp_plan.makeup_tex;
    out.makeup_opacity = mp_plan.makeup_opacity;
    out.makeup_adapt = mp_plan.makeup_adapt;
    out.n_bumps = mp_plan.n_bumps;
    for (int i = 0; i < out.n_bumps && i < MAX_FACE_BUMPS; ++i) out.bumps[i] = mp_plan.bumps[i];

    float sx_ = (obs.w > 0) ? (float)w / (float)obs.w : 1.f;
    float sy_ = (obs.h > 0) ? (float)h / (float)obs.h : 1.f;
    for (int i = 0; i < ARKIT_NPTS; ++i) {
        out.mesh_pts[i][0] = obs.pts[i][0] * sx_;
        out.mesh_pts[i][1] = obs.pts[i][1] * sy_;
    }
    return true;
}
