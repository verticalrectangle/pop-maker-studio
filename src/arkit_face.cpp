#include "arkit_face.h"
#include "face_filters.h"
#include "generated/arkit_landmark_map.h"
#include "generated/face_uv_mesh.h"
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

// Compute landmarks that are stubbed (index 0) in arkit_landmark_map.h by
// searching the live ARKit mesh. The mesh has 1220 vertices with 2D projected
// positions; we use the known landmarks (eyes, nose, mouth) to establish a
// face coordinate frame, then find the unknowns by direction.
//
// In the ARKit selfie projection (portrait, mirrored), the person's left
// appears on the RIGHT side of the screen (larger X), and Y increases
// downward (screen coordinates).
static void compute_mesh_landmarks(const ARKitFaceObs& obs, FaceObs& mp) {
    // Known landmarks (already filled by arkit_index_for_mp).
    float eyeLx = mp.pts[468][0], eyeLy = mp.pts[468][1]; // person's left iris
    float eyeRx = mp.pts[473][0], eyeRy = mp.pts[473][1]; // person's right iris
    float noseX = mp.pts[1][0],   noseY = mp.pts[1][1];   // nose tip
    float mouthX = (mp.pts[13][0] + mp.pts[14][0]) * 0.5f;
    float mouthY = (mp.pts[13][1] + mp.pts[14][1]) * 0.5f;
    float eyeMidX = (eyeLx + eyeRx) * 0.5f;
    float eyeMidY = (eyeLy + eyeRy) * 0.5f;

    // Face up vector: mouth → eye midpoint (normalized).
    float upX = eyeMidX - mouthX, upY = eyeMidY - mouthY;
    float upLen = sqrtf(upX * upX + upY * upY);
    if (upLen < 1.f) upLen = 1.f;
    upX /= upLen; upY /= upLen;
    // Right vector: perpendicular to up (rotated 90° clockwise in screen space).
    float rightX = -upY, rightY = upX;

    // Determine which screen direction is the person's left.
    // In the mirrored selfie view, person's left = larger X typically, but
    // use the eye projection onto the right vector to be pose-robust.
    float eyeL_proj = (eyeLx - noseX) * rightX + (eyeLy - noseY) * rightY;
    // If eyeL_proj > 0, the person's left is in the +right direction.
    float leftSgn = (eyeL_proj >= 0) ? 1.f : -1.f;

    // Chin: vertex furthest "down" from the mouth (along -up direction).
    if (ARKIT_CHIN == 0) {
        float best = -1e9f; int bi = 0;
        for (int i = 0; i < ARKIT_NPTS; ++i) {
            float dx = obs.pts[i][0] - mouthX, dy = obs.pts[i][1] - mouthY;
            float d = -(dx * upX + dy * upY); // negative up = down
            if (d > best) { best = d; bi = i; }
        }
        mp.pts[152][0] = obs.pts[bi][0];
        mp.pts[152][1] = obs.pts[bi][1];
    }

    // Face sides: widest vertices perpendicular to the up axis.
    if (ARKIT_FACE_L == 0 || ARKIT_FACE_R == 0) {
        float bestL = -1e9f, bestR = -1e9f; int iL = 0, iR = 0;
        for (int i = 0; i < ARKIT_NPTS; ++i) {
            float dx = obs.pts[i][0] - noseX, dy = obs.pts[i][1] - noseY;
            float proj = dx * rightX + dy * rightY;
            // Restrict to upper-mid face (exclude neck/below chin).
            float upProj = dx * upX + dy * upY;
            if (upProj < -upLen * 0.3f) continue;
            if (proj * leftSgn > bestL) { bestL = proj * leftSgn; iL = i; }
            if (-proj * leftSgn > bestR) { bestR = -proj * leftSgn; iR = i; }
        }
        // iL = person's left side, iR = person's right side.
        mp.pts[234][0] = obs.pts[iL][0]; mp.pts[234][1] = obs.pts[iL][1]; // face L
        mp.pts[454][0] = obs.pts[iR][0]; mp.pts[454][1] = obs.pts[iR][1]; // face R
    }

    // Cheeks: below eyes, offset laterally. Find the vertex below each eye
    // that is most distant from the face center in the lateral direction.
    float faceCx = (eyeMidX + mp.pts[152][0]) * 0.5f;
    float faceCy = (eyeMidY + mp.pts[152][1]) * 0.5f;
    if (ARKIT_CHEEK_L == 0 || ARKIT_CHEEK_R == 0) {
        // Search region: between eyes and chin, on each side.
        for (int side = 0; side < 2; ++side) {
            float eyeX = (side == 0) ? eyeLx : eyeRx;
            float eyeY = (side == 0) ? eyeLy : eyeRy;
            float sgn = (side == 0) ? leftSgn : -leftSgn;
            float best = -1e9f; int bi = 0;
            for (int i = 0; i < ARKIT_NPTS; ++i) {
                float dx = obs.pts[i][0] - faceCx, dy = obs.pts[i][1] - faceCy;
                float upProj = dx * upX + dy * upY;
                if (upProj > 0 || upProj < -upLen * 0.8f) continue; // below eyes, above chin
                float latProj = (obs.pts[i][0] - eyeX) * rightX + (obs.pts[i][1] - eyeY) * rightY;
                float score = latProj * sgn;
                if (score > best) { best = score; bi = i; }
            }
            int mpIdx = (side == 0) ? 50 : 280; // MP 50 = cheek L, 280 = cheek R
            mp.pts[mpIdx][0] = obs.pts[bi][0];
            mp.pts[mpIdx][1] = obs.pts[bi][1];
        }
    }

    // Nose wings: vertices lateral to the nose tip at a similar height.
    if (ARKIT_NOSE_L == 0 || ARKIT_NOSE_R == 0) {
        for (int side = 0; side < 2; ++side) {
            float sgn = (side == 0) ? leftSgn : -leftSgn;
            float best = -1e9f; int bi = 0;
            for (int i = 0; i < ARKIT_NPTS; ++i) {
                float dx = obs.pts[i][0] - noseX, dy = obs.pts[i][1] - noseY;
                float upProj = dx * upX + dy * upY;
                // Near the nose tip's height (within 30% of eye distance).
                if (fabsf(upProj) > upLen * 0.3f) continue;
                float latProj = dx * rightX + dy * rightY;
                float score = latProj * sgn;
                if (score > best && score < upLen * 0.25f) { best = score; bi = i; }
            }
            int mpIdx = (side == 0) ? 98 : 327; // MP 98 = nose L, 327 = nose R
            mp.pts[mpIdx][0] = obs.pts[bi][0];
            mp.pts[mpIdx][1] = obs.pts[bi][1];
        }
    }

    // Jaw sides: vertices on the lower face, lateral, between face sides and chin.
    if (ARKIT_JAW_CHAIN_L[1] == 0 || ARKIT_JAW_CHAIN_R[1] == 0) {
        float chinX = mp.pts[152][0], chinY = mp.pts[152][1];
        for (int side = 0; side < 2; ++side) {
            float sgn = (side == 0) ? leftSgn : -leftSgn;
            float best = -1e9f; int bi = 0;
            for (int i = 0; i < ARKIT_NPTS; ++i) {
                float dx = obs.pts[i][0] - faceCx, dy = obs.pts[i][1] - faceCy;
                float upProj = dx * upX + dy * upY;
                // Below face center, above chin.
                if (upProj > -upLen * 0.1f || upProj < -upLen * 0.7f) continue;
                float latProj = dx * rightX + dy * rightY;
                float score = fabsf(latProj);
                if (latProj * sgn > 0 && score > best) { best = score; bi = i; }
            }
            int mpIdx = (side == 0) ? 172 : 397; // MP 172 = jaw L, 397 = jaw R
            mp.pts[mpIdx][0] = obs.pts[bi][0];
            mp.pts[mpIdx][1] = obs.pts[bi][1];
        }
        (void)chinX; (void)chinY;
    }

    // Jaw chains: 5 points from ear→chin on each side. Interpolate along
    // the jaw arc between the jaw side and the chin.
    if (ARKIT_JAW_CHAIN_L[0] == 0) {
        float jawLx = mp.pts[172][0], jawLy = mp.pts[172][1];
        float chinX2 = mp.pts[152][0], chinY2 = mp.pts[152][1];
        // Approximate the jaw chain by finding vertices along the arc.
        for (int j = 0; j < 5; ++j) {
            float t = (float)j / 4.f; // 0 = ear side, 1 = chin
            float targetX = jawLx * (1.f - t) + chinX2 * t;
            float targetY = jawLy * (1.f - t) + chinY2 * t;
            float best = 1e9f; int bi = 0;
            for (int i = 0; i < ARKIT_NPTS; ++i) {
                float dx = obs.pts[i][0] - targetX, dy = obs.pts[i][1] - targetY;
                float d = dx * dx + dy * dy;
                if (d < best) { best = d; bi = i; }
            }
            int mpIdx = (j == 0) ? 132 : (j == 1) ? 172 : (j == 2) ? 136
                       : (j == 3) ? 149 : 176;
            mp.pts[mpIdx][0] = obs.pts[bi][0];
            mp.pts[mpIdx][1] = obs.pts[bi][1];
        }
    }
    if (ARKIT_JAW_CHAIN_R[0] == 0) {
        float jawRx = mp.pts[397][0], jawRy = mp.pts[397][1];
        float chinX2 = mp.pts[152][0], chinY2 = mp.pts[152][1];
        for (int j = 0; j < 5; ++j) {
            float t = (float)j / 4.f;
            float targetX = jawRx * (1.f - t) + chinX2 * t;
            float targetY = jawRy * (1.f - t) + chinY2 * t;
            float best = 1e9f; int bi = 0;
            for (int i = 0; i < ARKIT_NPTS; ++i) {
                float dx = obs.pts[i][0] - targetX, dy = obs.pts[i][1] - targetY;
                float d = dx * dx + dy * dy;
                if (d < best) { best = d; bi = i; }
            }
            int mpIdx = (j == 0) ? 361 : (j == 1) ? 397 : (j == 2) ? 365
                       : (j == 3) ? 378 : 400;
            mp.pts[mpIdx][0] = obs.pts[bi][0];
            mp.pts[mpIdx][1] = obs.pts[bi][1];
        }
    }
}
// Fill unmapped MediaPipe landmarks by barycentric interpolation from known
// landmarks in canonical UV space. We have ~66 known correspondences (canonical
// UV → live 2D). For each of the ~400 unmapped points, find the 3 closest known
// landmarks in canonical UV space and interpolate the live position. This
// produces a piecewise-affine warp from canonical face space to the live frame.
static void interpolate_missing_landmarks(FaceObs& mp) {
    // Collect known landmarks: indices where pts is non-zero.
    float known_uv[FT_NPTS][2];
    float known_px[FT_NPTS][2];
    int n_known = 0;
    for (int i = 0; i < FACE_UV_NPTS; ++i) {
        if (mp.pts[i][0] != 0.f || mp.pts[i][1] != 0.f) {
            known_uv[n_known][0] = k_face_uv[i][0];
            known_uv[n_known][1] = k_face_uv[i][1];
            known_px[n_known][0] = mp.pts[i][0];
            known_px[n_known][1] = mp.pts[i][1];
            ++n_known;
        }
    }
    if (n_known < 3) return;  // not enough anchors to interpolate

    for (int i = 0; i < FACE_UV_NPTS; ++i) {
        if (mp.pts[i][0] != 0.f || mp.pts[i][1] != 0.f) continue;  // already known
        float u = k_face_uv[i][0], v = k_face_uv[i][1];

        // Find the 3 closest known landmarks in canonical UV space.
        float d0 = 1e9f, d1 = 1e9f, d2 = 1e9f;
        int j0 = 0, j1 = 0, j2 = 0;
        for (int j = 0; j < n_known; ++j) {
            float du = known_uv[j][0] - u, dv = known_uv[j][1] - v;
            float d = du * du + dv * dv;
            if (d < d0) { d2 = d1; j2 = j1; d1 = d0; j1 = j0; d0 = d; j0 = j; }
            else if (d < d1) { d2 = d1; j2 = j1; d1 = d; j1 = j; }
            else if (d < d2) { d2 = d; j2 = j; }
        }

        // Barycentric coordinates of (u,v) in triangle (j0, j1, j2).
        float ax = known_uv[j1][0] - known_uv[j0][0], ay = known_uv[j1][1] - known_uv[j0][1];
        float bx = known_uv[j2][0] - known_uv[j0][0], by = known_uv[j2][1] - known_uv[j0][1];
        float px = u - known_uv[j0][0],            py = v - known_uv[j0][1];
        float det = ax * by - ay * bx;
        if (fabsf(det) < 1e-8f) {
            // Degenerate triangle — fall back to nearest known point.
            mp.pts[i][0] = known_px[j0][0];
            mp.pts[i][1] = known_px[j0][1];
            continue;
        }
        float w1 = (px * by - py * bx) / det;
        float w2 = (ax * py - ay * px) / det;
        float w0 = 1.f - w1 - w2;

        mp.pts[i][0] = w0 * known_px[j0][0] + w1 * known_px[j1][0] + w2 * known_px[j2][0];
        mp.pts[i][1] = w0 * known_px[j0][1] + w1 * known_px[j1][1] + w2 * known_px[j2][1];
    }
}

// Build a MediaPipe-format FaceRenderPlan from an ARKit observation.
// Maps ARKit mesh landmarks → MediaPipe indices, computes runtime landmarks
// (chin, cheeks, jaw, nose wings, face sides) from the live mesh, then calls
// face_filter_build_plan_look. The returned plan uses the MediaPipe mesh
// (468 pts) + MediaPipe canonical UVs for the texture pass — makeup PNGs are
// authored for MediaPipe UV space, NOT ARKit UV space.
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
        int ai = arkit_index_for_mp(i);
        if (ai > 0 && ai < ARKIT_NPTS) {
            mp_obs.pts[i][0] = obs.pts[ai][0];
            mp_obs.pts[i][1] = obs.pts[ai][1];
        }
    }
    // Fill in landmarks that are stubbed (index 0) in the landmark map by
    // searching the live mesh geometry.
    compute_mesh_landmarks(obs, mp_obs);
    // Fill remaining unmapped landmarks by barycentric interpolation from
    // known landmarks in canonical UV space. Without this, unmapped points
    // stay at {0,0} and mesh triangles stretch to the top-left corner.
    interpolate_missing_landmarks(mp_obs);

    return face_filter_build_plan_look(L, amount, mp_obs, w, h, out);
}
