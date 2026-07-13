#include "arkit_face.h"
#include "face_filters.h"
#include "generated/arkit_mp_map.h"
#include <cmath>
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

static struct {
    std::mutex mtx;
    ARKitFace3D face;
    double submit_t = 0.0;
} g_arkit3d;

void arkit_face_note_camera_time(double host_time) {
    {
        std::lock_guard<std::mutex> lk(g_arkit_slot.mtx);
        g_arkit_slot.cam_t = host_time;
    }
    // (single writer per frame; the 3D slot reads the 2D slot's clock)
}

void arkit_face3d_submit(const ARKitFace3D* f) {
    std::lock_guard<std::mutex> lk(g_arkit3d.mtx);
    if (!f || !f->valid) { g_arkit3d.face.valid = false; return; }
    g_arkit3d.face = *f;
    std::lock_guard<std::mutex> lk2(g_arkit_slot.mtx);
    g_arkit3d.submit_t = g_arkit_slot.cam_t;
}

bool arkit_face3d_take(ARKitFace3D* out) {
    if (!out) return false;
    double cam_t;
    {
        std::lock_guard<std::mutex> lk2(g_arkit_slot.mtx);
        cam_t = g_arkit_slot.cam_t;
    }
    std::lock_guard<std::mutex> lk(g_arkit3d.mtx);
    if (!g_arkit3d.face.valid) return false;
    if (cam_t - g_arkit3d.submit_t > 0.15) return false;   // stale (see 2D slot)
    *out = g_arkit3d.face;
    return true;
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
            float cy = mp_obs.pts[e.iris0][1];
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

    // Lash-line coherence: the bridged chains ride ARKit's hole-arc verts
    // exactly, which is right on average but passes through ARKit's
    // per-vertex tracking noise — each chain point wiggled independently,
    // so lash strokes anchored along the chain dropped one by one instead
    // of moving as a fringe. Fit each lid chain to a quadratic in the
    // eye's corner-to-corner frame and snap the interior points onto the
    // fit: the chain moves as one smooth curve, corners stay exact, and no
    // temporal filtering means no added latency.
    {
        static const int kChains[4][7] = {
            {33, 161, 160, 159, 158, 157, 133},    // R upper
            {263, 388, 387, 386, 385, 384, 362},   // L upper
            {33, 163, 144, 145, 153, 154, 133},    // R lower
            {263, 390, 373, 374, 380, 381, 362},   // L lower
        };
        for (const int* ch : kChains) {
            float ox = mp_obs.pts[ch[0]][0], oy = mp_obs.pts[ch[0]][1];
            float ax = mp_obs.pts[ch[6]][0] - ox, ay = mp_obs.pts[ch[6]][1] - oy;
            float al = sqrtf(ax * ax + ay * ay);
            if (al < 1e-3f) continue;
            ax /= al; ay /= al;
            float u[7], v[7];
            for (int k = 0; k < 7; ++k) {
                float px = mp_obs.pts[ch[k]][0] - ox;
                float py = mp_obs.pts[ch[k]][1] - oy;
                u[k] = px * ax + py * ay;
                v[k] = -px * ay + py * ax;
            }
            // Least-squares v = a + b*u + c*u^2 over the 7 points.
            double S0 = 7, S1 = 0, S2 = 0, S3 = 0, S4 = 0;
            double T0 = 0, T1 = 0, T2 = 0;
            for (int k = 0; k < 7; ++k) {
                double uu = u[k];
                S1 += uu; S2 += uu * uu; S3 += uu * uu * uu;
                S4 += uu * uu * uu * uu;
                T0 += v[k]; T1 += v[k] * uu; T2 += v[k] * uu * uu;
            }
            double m[3][4] = {{S0, S1, S2, T0},
                              {S1, S2, S3, T1},
                              {S2, S3, S4, T2}};
            // Gaussian elimination (3x3, well-conditioned: u spans the eye).
            for (int c2 = 0; c2 < 3; ++c2) {
                int piv = c2;
                for (int r2 = c2 + 1; r2 < 3; ++r2)
                    if (fabs(m[r2][c2]) > fabs(m[piv][c2])) piv = r2;
                for (int c3 = 0; c3 < 4; ++c3) {
                    double t2 = m[c2][c3]; m[c2][c3] = m[piv][c3]; m[piv][c3] = t2;
                }
                if (fabs(m[c2][c2]) < 1e-9) { m[c2][c2] = 1e-9; }
                for (int r2 = 0; r2 < 3; ++r2) {
                    if (r2 == c2) continue;
                    double f = m[r2][c2] / m[c2][c2];
                    for (int c3 = c2; c3 < 4; ++c3) m[r2][c3] -= f * m[c2][c3];
                }
            }
            double A = m[0][3] / m[0][0], B = m[1][3] / m[1][1],
                   C = m[2][3] / m[2][2];
            for (int k = 1; k < 6; ++k) {   // interior points only
                float vf = (float)(A + B * u[k] + C * u[k] * u[k]);
                mp_obs.pts[ch[k]][0] = ox + ax * u[k] - ay * vf;
                mp_obs.pts[ch[k]][1] = oy + ay * u[k] + ax * vf;
            }
        }
    }

    bool ok = face_filter_build_plan_look(L, amount, mp_obs, w, h, out);
    if (ok && out.makeup_tex)
        out.has_arkit_mesh = true;
    return ok;
}
