#include "arkit_face.h"
#include "face_filters.h"
#include "generated/arkit_mp_map.h"
#include <mutex>

struct ARKitFaceSlot {
    std::mutex mtx;
    ARKitFaceObs faces[ARKIT_MAX_FACES];
    int n_faces = 0;
    bool fresh = false;
    double cam_t = 0.0;      // newest camera frame host time
    double submit_t = 0.0;   // camera time at last ARKit submission
};

static ARKitFaceSlot g_arkit_slot;

void arkit_face_note_camera_time(double host_time) {
    std::lock_guard<std::mutex> lk(g_arkit_slot.mtx);
    g_arkit_slot.cam_t = host_time;
}

void arkit_face_submit(const ARKitFaceObs* obs, int n_faces) {
    std::lock_guard<std::mutex> lk(g_arkit_slot.mtx);
    g_arkit_slot.fresh = false;
    g_arkit_slot.n_faces = 0;
    if (!obs || n_faces <= 0) return;
    g_arkit_slot.submit_t = g_arkit_slot.cam_t;
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
    if (g_arkit_slot.cam_t - g_arkit_slot.submit_t > 0.15) return 0;  // stale
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

    // Gaze: the ARKit mesh does not move with the eyeball, so the bridged
    // iris landmarks are a static socket center — iris-anchored effects
    // painted lid skin instead of the iris and ignored gaze entirely. The
    // eyeLook* blendshapes carry gaze; offset the 10 iris landmarks toward
    // the corner/lid the eye is actually looking at. ("Left"/"Right" name
    // the PERSON's eye; person's left = +x in the unmirrored buffer, screen
    // y grows downward.)
    if (mp_obs.has_blend) {
        struct EyeMap { int iris0; int c_in, c_out, lid_up, lid_dn;
                        int in_bl, out_bl, up_bl, dn_bl; float in_sign; };
        static const EyeMap eyes[2] = {
            // person's RIGHT eye (MP 468): inner corner 133 (+x side of it)
            {468, 133, 33, 159, 145, FB_EYE_LOOK_IN_R, FB_EYE_LOOK_OUT_R,
             FB_EYE_LOOK_UP_R, FB_EYE_LOOK_DOWN_R, +1.f},
            // person's LEFT eye (MP 473): inner corner 362 (-x side of it)
            {473, 362, 263, 386, 374, FB_EYE_LOOK_IN_L, FB_EYE_LOOK_OUT_L,
             FB_EYE_LOOK_UP_L, FB_EYE_LOOK_DOWN_L, -1.f},
        };
        for (const EyeMap& e : eyes) {
            float cx = mp_obs.pts[e.iris0][0], cy = mp_obs.pts[e.iris0][1];
            float half_w = 0.5f * fabsf(mp_obs.pts[e.c_in][0]
                                        - mp_obs.pts[e.c_out][0]);
            float up_span = cy - mp_obs.pts[e.lid_up][1];   // screen y down
            float dn_span = mp_obs.pts[e.lid_dn][1] - cy;
            float dx = (mp_obs.blend[e.in_bl] * e.in_sign
                        - mp_obs.blend[e.out_bl] * e.in_sign) * 0.60f * half_w;
            float dy = mp_obs.blend[e.dn_bl] * 0.90f * dn_span
                     - mp_obs.blend[e.up_bl] * 0.90f * up_span;
            for (int k = 0; k < 5; ++k) {
                mp_obs.pts[e.iris0 + k][0] += dx;
                mp_obs.pts[e.iris0 + k][1] += dy;
            }
        }
    }

    bool ok = face_filter_build_plan_look(L, amount, mp_obs, w, h, out);
    if (ok && out.makeup_tex)
        out.has_arkit_mesh = true;
    return ok;
}
