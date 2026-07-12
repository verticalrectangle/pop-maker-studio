#include "arkit_face.h"
#include "face_filters.h"
#include "generated/arkit_landmark_map.h"
#include "generated/face_uv_mesh.h"
#include <cmath>
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

// Map MediaPipe landmark indices → ARKit mesh vertex indices.
// Returns 0 when unmapped (0 is never a trusted semantic vertex here).
static int arkit_index_for_mp(int mp) {
    switch (mp) {
        case 0: return ARKIT_LIP_RING[3];
        case 1: return ARKIT_NOSE_TIP;
        case 10: return ARKIT_FOREHEAD;
        case 13: return ARKIT_LIP_UPPER;
        case 14: return ARKIT_LIP_LOWER;
        case 17: return ARKIT_LIP_RING[9];

        // MediaPipe landmarks use IMAGE/viewer perspective (U=0 = left of
        // image = person's RIGHT). ARKit constants use PERSON's perspective.
        // So MP "left" indices (low U: 33,7,61,50,98,132,234...) map to
        // ARKit _R constants, and MP "right" indices (high U: 263,249,291,
        // 280,327,361,454...) map to ARKit _L constants.
        //
        // Person's right eye (image-left, low U): upper lid outer→inner.
        case 33:  return ARKIT_LID_R[0];          // outer corner
        case 161: return ARKIT_LID_R[1];
        case 160: return ARKIT_LID_R[2];
        case 159: return ARKIT_LID_R[3];
        case 158: return ARKIT_LID_R[4];
        case 157: return ARKIT_LID_R[5];
        case 133: return ARKIT_LID_R[6];          // inner corner
        case 7:   return ARKIT_LOWER_LID_R[1];
        case 163: return ARKIT_LOWER_LID_R[2];
        case 144: return ARKIT_LOWER_LID_R[3];
        case 145: return ARKIT_LOWER_LID_R[4];
        case 153: return ARKIT_LOWER_LID_R[5];
        case 154: return ARKIT_LOWER_LID_R[6];
        case 155: return ARKIT_LOWER_LID_R[7];

        // Person's left eye (image-right, high U): upper lid outer→inner.
        case 263: return ARKIT_LID_L[0];
        case 388: return ARKIT_LID_L[1];
        case 387: return ARKIT_LID_L[2];
        case 386: return ARKIT_LID_L[3];
        case 385: return ARKIT_LID_L[4];
        case 384: return ARKIT_LID_L[5];
        case 362: return ARKIT_LID_L[6];
        case 249: return ARKIT_LOWER_LID_L[1];
        case 390: return ARKIT_LOWER_LID_L[2];
        case 373: return ARKIT_LOWER_LID_L[3];
        case 374: return ARKIT_LOWER_LID_L[4];
        case 380: return ARKIT_LOWER_LID_L[5];
        case 381: return ARKIT_LOWER_LID_L[6];
        case 382: return ARKIT_LOWER_LID_L[7];

        // Lips (outer ring). ARKIT_LIP_RING is clockwise from person's L
        // corner (249). MP indices are image-space: 61=left of image=
        // person's R corner, 291=right of image=person's L corner.
        case 37:  return ARKIT_LIP_RING[4];    // image-left → person's R upper
        case 40:  return ARKIT_LIP_RING[5];    // image-left → person's R upper
        case 61:  return ARKIT_LIP_RING[6];    // image-left → person's R corner
        case 84:  return ARKIT_LIP_RING[8];    // image-left → person's R lower
        case 91:  return ARKIT_LIP_RING[7];    // image-left → person's R lower
        case 267: return ARKIT_LIP_RING[2];    // image-right → person's L upper
        case 270: return ARKIT_LIP_RING[1];    // image-right → person's L upper
        case 291: return ARKIT_LIP_RING[0];    // image-right → person's L corner
        case 314: return ARKIT_LIP_RING[10];   // image-right → person's L lower
        case 321: return ARKIT_LIP_RING[11];   // image-right → person's L lower

        // Runtime-filled semantic points (constants are 0 placeholders).
        // Image-left indices → person's RIGHT, image-right → person's LEFT.
        case 50:  return ARKIT_CHEEK_R;     // image-left → person's R cheek
        case 98:  return ARKIT_NOSE_R;      // image-left → person's R nose
        case 132: return ARKIT_JAW_CHAIN_R[0];
        case 136: return ARKIT_JAW_CHAIN_R[2];
        case 149: return ARKIT_JAW_CHAIN_R[3];
        case 152: return ARKIT_CHIN;
        case 172: return ARKIT_JAW_CHAIN_R[1];
        case 176: return ARKIT_JAW_CHAIN_R[4];
        case 234: return ARKIT_FACE_R;      // image-left → person's R face
        case 280: return ARKIT_CHEEK_L;     // image-right → person's L cheek
        case 327: return ARKIT_NOSE_L;      // image-right → person's L nose
        case 361: return ARKIT_JAW_CHAIN_L[0];
        case 365: return ARKIT_JAW_CHAIN_L[2];
        case 378: return ARKIT_JAW_CHAIN_L[3];
        case 397: return ARKIT_JAW_CHAIN_L[1];
        case 400: return ARKIT_JAW_CHAIN_L[4];
        case 454: return ARKIT_FACE_L;      // image-right → person's L face

        // Irises: 468=image-left=person's R, 473=image-right=person's L
        case 468: return ARKIT_IRIS_R;
        case 473: return ARKIT_IRIS_L;
        default: return 0;
    }
}

static void set_mp(FaceObs& mp, bool* known, int idx, float x, float y) {
    if (idx < 0 || idx >= FT_NPTS) return;
    mp.pts[idx][0] = x;
    mp.pts[idx][1] = y;
    known[idx] = true;
}

static void set_mp_from(FaceObs& mp, bool* known, int idx,
                        const ARKitFaceObs& obs, int ai) {
    if (ai <= 0 || ai >= ARKIT_NPTS) return;
    set_mp(mp, known, idx, obs.pts[ai][0], obs.pts[ai][1]);
}

// Find the mesh vertex nearest to a target that also satisfies a predicate.
// Returns -1 if none.
template <typename Pred>
static int nearest_pred(const ARKitFaceObs& obs, float tx, float ty, Pred ok) {
    float best = 1e30f;
    int bi = -1;
    for (int i = 0; i < ARKIT_NPTS; ++i) {
        if (!ok(i, obs.pts[i][0], obs.pts[i][1])) continue;
        float dx = obs.pts[i][0] - tx, dy = obs.pts[i][1] - ty;
        float d = dx * dx + dy * dy;
        if (d < best) { best = d; bi = i; }
    }
    return bi;
}

// Compute landmarks that are stubbed (index 0) in arkit_landmark_map.h by
// searching the live ARKit mesh. Also discovers brows (no stable hard indices
// without rest-pose geometry).
//
// Coordinate contract: ARKit selfie projection is portrait, UNMIRRORED.
// Person's left appears on the RIGHT side of the screen (larger X). Y grows
// downward. L/R labels are the PERSON's left/right.
static void compute_mesh_landmarks(const ARKitFaceObs& obs, FaceObs& mp,
                                   bool* known) {
    // Seed landmarks already filled by arkit_index_for_mp.
    float eyeLx = mp.pts[473][0], eyeLy = mp.pts[473][1]; // person's left iris (MP 473=image-right)
    float eyeRx = mp.pts[468][0], eyeRy = mp.pts[468][1]; // person's right iris (MP 468=image-left)
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

    // Person's left along the screen right-vector (pose-robust).
    float eyeL_proj = (eyeLx - noseX) * rightX + (eyeLy - noseY) * rightY;
    float leftSgn = (eyeL_proj >= 0.f) ? 1.f : -1.f;

    // Chin: furthest "down" from the mouth.
    {
        float best = -1e9f; int bi = -1;
        for (int i = 0; i < ARKIT_NPTS; ++i) {
            float dx = obs.pts[i][0] - mouthX, dy = obs.pts[i][1] - mouthY;
            float d = -(dx * upX + dy * upY);
            if (d > best) { best = d; bi = i; }
        }
        if (bi >= 0) set_mp_from(mp, known, 152, obs, bi);
    }

    // Face sides: widest vertices perpendicular to the up axis.
    {
        float bestL = -1e9f, bestR = -1e9f; int iL = -1, iR = -1;
        for (int i = 0; i < ARKIT_NPTS; ++i) {
            float dx = obs.pts[i][0] - noseX, dy = obs.pts[i][1] - noseY;
            float proj = dx * rightX + dy * rightY;
            float upProj = dx * upX + dy * upY;
            if (upProj < -upLen * 0.3f) continue; // exclude neck
            float sL = proj * leftSgn, sR = -proj * leftSgn;
            if (sL > bestL) { bestL = sL; iL = i; }
            if (sR > bestR) { bestR = sR; iR = i; }
        }
        if (iL >= 0) set_mp_from(mp, known, 454, obs, iL);  // person L → MP 454 (image-right)
        if (iR >= 0) set_mp_from(mp, known, 234, obs, iR);  // person R → MP 234 (image-left)
    }

    float faceCx = (eyeMidX + mp.pts[152][0]) * 0.5f;
    float faceCy = (eyeMidY + mp.pts[152][1]) * 0.5f;

    // Cheeks: below each eye, laterally outward.
    for (int side = 0; side < 2; ++side) {
        float eyeX = (side == 0) ? eyeLx : eyeRx;
        float eyeY = (side == 0) ? eyeLy : eyeRy;
        float sgn = (side == 0) ? leftSgn : -leftSgn;
        float best = -1e9f; int bi = -1;
        for (int i = 0; i < ARKIT_NPTS; ++i) {
            float dx = obs.pts[i][0] - faceCx, dy = obs.pts[i][1] - faceCy;
            float upProj = dx * upX + dy * upY;
            if (upProj > 0.f || upProj < -upLen * 0.8f) continue;
            float latProj = (obs.pts[i][0] - eyeX) * rightX
                          + (obs.pts[i][1] - eyeY) * rightY;
            float score = latProj * sgn;
            if (score > best) { best = score; bi = i; }
        }
        if (bi >= 0) set_mp_from(mp, known, (side == 0) ? 280 : 50, obs, bi);  // person L→280, R→50
    }

    // Nose wings: lateral to nose tip, similar height.
    for (int side = 0; side < 2; ++side) {
        float sgn = (side == 0) ? leftSgn : -leftSgn;
        float best = -1e9f; int bi = -1;
        for (int i = 0; i < ARKIT_NPTS; ++i) {
            float dx = obs.pts[i][0] - noseX, dy = obs.pts[i][1] - noseY;
            float upProj = dx * upX + dy * upY;
            if (fabsf(upProj) > upLen * 0.3f) continue;
            float latProj = dx * rightX + dy * rightY;
            float score = latProj * sgn;
            // Prefer near-wing distances; fall back to any lateral hit.
            if (score <= 0.f) continue;
            if (score < upLen * 0.35f && score > best) { best = score; bi = i; }
        }
        if (bi < 0) {
            // Fallback: nearest vertex slightly lateral of the tip.
            float tx = noseX + rightX * leftSgn * (side == 0 ? 1.f : -1.f) * upLen * 0.12f;
            float ty = noseY;
            bi = nearest_pred(obs, tx, ty, [&](int, float x, float y) {
                float dx = x - noseX, dy = y - noseY;
                float lat = dx * rightX + dy * rightY;
                return lat * ((side == 0) ? leftSgn : -leftSgn) > 0.f;
            });
        }
        if (bi >= 0) set_mp_from(mp, known, (side == 0) ? 327 : 98, obs, bi);  // person L→327, R→98
    }

    // Jaw sides + ear→chin chains.
    for (int side = 0; side < 2; ++side) {
        float sgn = (side == 0) ? leftSgn : -leftSgn;
        float best = -1e9f; int bi = -1;
        for (int i = 0; i < ARKIT_NPTS; ++i) {
            float dx = obs.pts[i][0] - faceCx, dy = obs.pts[i][1] - faceCy;
            float upProj = dx * upX + dy * upY;
            if (upProj > -upLen * 0.1f || upProj < -upLen * 0.7f) continue;
            float latProj = dx * rightX + dy * rightY;
            if (latProj * sgn <= 0.f) continue;
            float score = fabsf(latProj);
            if (score > best) { best = score; bi = i; }
        }
        int jawMp = (side == 0) ? 397 : 172;  // person L→397, R→172
        if (bi >= 0) set_mp_from(mp, known, jawMp, obs, bi);

        float jawX = mp.pts[jawMp][0], jawY = mp.pts[jawMp][1];
        float chinX = mp.pts[152][0], chinY = mp.pts[152][1];
        static const int kJawL[5] = {361, 397, 365, 378, 400};  // person L → image-right indices
        static const int kJawR[5] = {132, 172, 136, 149, 176};  // person R → image-left indices
        const int* chain = (side == 0) ? kJawL : kJawR;
        for (int j = 0; j < 5; ++j) {
            float t = (float)j / 4.f;
            float tx = jawX * (1.f - t) + chinX * t;
            float ty = jawY * (1.f - t) + chinY * t;
            int ni = nearest_pred(obs, tx, ty, [](int, float, float) { return true; });
            if (ni >= 0) set_mp_from(mp, known, chain[j], obs, ni);
        }
    }

    // Brows: 5 points per side above the upper lid, outer→inner.
    // After L/R swap: kLidL indices (33,161,...) hold person's RIGHT eye
    // positions, kLidR (263,388,...) hold person's LEFT. Side 0 = person L,
    // so must use kLidR for lid positions and kBrowR for brow MP indices.
    static const int kBrowL[5] = {300, 293, 334, 296, 336};  // person L → image-right
    static const int kBrowR[5] = {70, 63, 105, 66, 107};     // person R → image-left
    static const int kLidL[7] = {263, 388, 387, 386, 385, 384, 362};  // person L (image-right)
    static const int kLidR[7] = {33, 161, 160, 159, 158, 157, 133};   // person R (image-left)
    for (int side = 0; side < 2; ++side) {
        const int* lid = (side == 0) ? kLidL : kLidR;
        const int* brow = (side == 0) ? kBrowL : kBrowR;
        float irisX = (side == 0) ? eyeLx : eyeRx;
        float irisY = (side == 0) ? eyeLy : eyeRy;
        // Sample 5 targets above evenly spaced upper-lid points.
        for (int j = 0; j < 5; ++j) {
            // Map j=0..4 onto lid indices 0,1,3,5,6 (outer→inner).
            static const int kLidPick[5] = {0, 1, 3, 5, 6};
            int li = lid[kLidPick[j]];
            if (!known[li]) continue;
            float lx = mp.pts[li][0], ly = mp.pts[li][1];
            // Target sits above the lid by ~0.22 of inter-eye distance.
            float tx = lx + upX * upLen * 0.22f;
            float ty = ly + upY * upLen * 0.22f;
            // Also nudge slightly outward on the outer brow.
            float outSgn = (side == 0) ? leftSgn : -leftSgn;
            float outAmt = (j == 0) ? 0.08f : (j == 1 ? 0.04f : 0.f);
            tx += rightX * outSgn * upLen * outAmt;
            ty += rightY * outSgn * upLen * outAmt;
            int bi = nearest_pred(obs, tx, ty, [&](int, float x, float y) {
                // Must be above the lid (toward forehead) and not below iris.
                float dx = x - lx, dy = y - ly;
                float upProj = dx * upX + dy * upY;
                if (upProj < upLen * 0.05f || upProj > upLen * 0.55f) return false;
                // Keep on the correct half relative to nose for outer samples.
                float fromNose = (x - noseX) * rightX + (y - noseY) * rightY;
                if (j <= 1 && fromNose * outSgn < 0.f) return false;
                (void)irisX; (void)irisY;
                return true;
            });
            if (bi >= 0) set_mp_from(mp, known, brow[j], obs, bi);
        }
    }
}

// Fill unmapped MediaPipe landmarks by inverse-distance weighting from known
// landmarks in canonical UV space. Known points are tracked by mask so a
// legitimate pixel at (0, y) is not treated as missing.
static void interpolate_missing_landmarks(FaceObs& mp, const bool* known) {
    float known_uv[FT_NPTS][2];
    float known_px[FT_NPTS][2];
    int n_known = 0;
    for (int i = 0; i < FACE_UV_NPTS; ++i) {
        if (!known[i]) continue;
        known_uv[n_known][0] = k_face_uv[i][0];
        known_uv[n_known][1] = k_face_uv[i][1];
        known_px[n_known][0] = mp.pts[i][0];
        known_px[n_known][1] = mp.pts[i][1];
        ++n_known;
    }
    // Iris centers live past FACE_UV_NPTS (468/473) but still help eye ring fill.
    for (int i = FACE_UV_NPTS; i < FT_NPTS; ++i) {
        if (!known[i]) continue;
        // No canonical UV for iris extras beyond 467; skip UV-space IDW seeds.
        // (468/473 already used as beauty anchors; mesh pass only draws 468.)
        (void)i;
    }
    if (n_known < 3) return;

    constexpr int K = 8;
    float w[K];
    int idx[K];

    for (int i = 0; i < FACE_UV_NPTS; ++i) {
        if (known[i]) continue;
        float u = k_face_uv[i][0], v = k_face_uv[i][1];

        float dists[K];
        for (int k = 0; k < K; ++k) { dists[k] = 1e9f; idx[k] = 0; }
        for (int j = 0; j < n_known; ++j) {
            float du = known_uv[j][0] - u, dv = known_uv[j][1] - v;
            float d = du * du + dv * dv;
            int worst = 0;
            for (int k = 1; k < K; ++k) if (dists[k] > dists[worst]) worst = k;
            if (d < dists[worst]) { dists[worst] = d; idx[worst] = j; }
        }

        float wsum = 0.f;
        float px = 0.f, py = 0.f;
        bool exact = false;
        for (int k = 0; k < K; ++k) {
            if (dists[k] >= 1e9f) continue;
            float d = sqrtf(dists[k]);
            if (d < 1e-6f) {
                px = known_px[idx[k]][0];
                py = known_px[idx[k]][1];
                exact = true;
                break;
            }
            w[k] = 1.f / (d * d);
            wsum += w[k];
        }
        if (!exact && wsum > 0.f) {
            px = 0.f; py = 0.f;
            for (int k = 0; k < K; ++k) {
                if (dists[k] >= 1e9f) continue;
                float wk = w[k] / wsum;
                px += wk * known_px[idx[k]][0];
                py += wk * known_px[idx[k]][1];
            }
        }
        mp.pts[i][0] = px;
        mp.pts[i][1] = py;
    }
}

// ── Screen-space snap to ARKit mesh ─────────────────────────────────────────
// After IDW interpolation in MediaPipe UV space (which is correct, not
// mirrored), snap each MediaPipe vertex's estimated screen position to the
// nearest real ARKit vertex in screen space. This eliminates ARKit UV space
// entirely (the source of the L/R mirroring bug) and gives real positions
// without IDW smearing. The ARKit mesh (1220 verts) is 2.6× denser than
// MediaPipe (468), so the nearest ARKit vertex is almost always within 1-2
// pixels of the correct location.
static void snap_to_arkit(const ARKitFaceObs& obs, FaceObs& mp,
                          const bool* known) {
    for (int i = 0; i < FACE_UV_NPTS; ++i) {
        if (known[i]) continue;  // already a real ARKit position
        float px = mp.pts[i][0], py = mp.pts[i][1];
        float best = 1e30f;
        int best_ai = -1;
        for (int j = 0; j < ARKIT_NPTS; ++j) {
            float dx = obs.pts[j][0] - px;
            float dy = obs.pts[j][1] - py;
            float d = dx * dx + dy * dy;
            if (d < best) { best = d; best_ai = j; }
        }
        if (best_ai >= 0) {
            mp.pts[i][0] = obs.pts[best_ai][0];
            mp.pts[i][1] = obs.pts[best_ai][1];
        }
    }
}

// ── ARKit → MediaPipe UV remap ──────────────────────────────────────────────
// For each of 1220 ARKit vertices, find the containing MediaPipe triangle in
// screen space and interpolate k_face_uv via barycentric weights. This maps
// the existing makeup PNGs (authored in MediaPipe UV space) onto the ARKit
// mesh, giving 1220 real positions + correct UVs — no IDW, no snap, no L/R
// swap issues. The UV mapping is constant per topology, so cache after first
// successful build.
static bool s_remap_cached = false;
static float s_arkit_mp_uv[ARKIT_NPTS][2];

static void arkit_mesh_remap_uvs(const ARKitFaceObs& obs,
                                 const FaceObs& mp) {
    if (s_remap_cached) return;

    // Pre-compute per-triangle bounding boxes for quick rejection.
    struct BBox { float minx, miny, maxx, maxy; };
    BBox tri_bb[FACE_UV_NTRI];
    for (int t = 0; t < FACE_UV_NTRI; ++t) {
        int i0 = k_face_tris[t][0], i1 = k_face_tris[t][1], i2 = k_face_tris[t][2];
        float ax = mp.pts[i0][0], ay = mp.pts[i0][1];
        float bx = mp.pts[i1][0], by = mp.pts[i1][1];
        float cx = mp.pts[i2][0], cy = mp.pts[i2][1];
        tri_bb[t].minx = fminf(fminf(ax, bx), cx);
        tri_bb[t].miny = fminf(fminf(ay, by), cy);
        tri_bb[t].maxx = fmaxf(fmaxf(ax, bx), cx);
        tri_bb[t].maxy = fmaxf(fmaxf(ay, by), cy);
    }

    // For each ARKit vertex, find the containing MediaPipe triangle in screen
    // space (barycentric all-positive). BBox pre-check eliminates ~90% of
    // tests. Fall back to nearest-vertex UV for points outside all triangles.
    for (int ai = 0; ai < ARKIT_NPTS; ++ai) {
        float px = obs.pts[ai][0], py = obs.pts[ai][1];
        float best_d = 1e30f;
        int best_mp = 0;
        bool found = false;

        for (int t = 0; t < FACE_UV_NTRI && !found; ++t) {
            // Bounding-box rejection
            if (px < tri_bb[t].minx || px > tri_bb[t].maxx ||
                py < tri_bb[t].miny || py > tri_bb[t].maxy) continue;

            int i0 = k_face_tris[t][0], i1 = k_face_tris[t][1], i2 = k_face_tris[t][2];
            float ax = mp.pts[i0][0], ay = mp.pts[i0][1];
            float bx = mp.pts[i1][0], by = mp.pts[i1][1];
            float cx = mp.pts[i2][0], cy = mp.pts[i2][1];

            float denom = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
            if (fabsf(denom) < 1e-6f) continue;
            float inv = 1.f / denom;
            float w0 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) * inv;
            float w1 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) * inv;
            float w2 = 1.f - w0 - w1;

            if (w0 >= -0.01f && w1 >= -0.01f && w2 >= -0.01f) {
                s_arkit_mp_uv[ai][0] = w0 * k_face_uv[i0][0] + w1 * k_face_uv[i1][0] + w2 * k_face_uv[i2][0];
                s_arkit_mp_uv[ai][1] = w0 * k_face_uv[i0][1] + w1 * k_face_uv[i1][1] + w2 * k_face_uv[i2][1];
                found = true;
            }
        }

        if (!found) {
            for (int mi = 0; mi < FACE_UV_NPTS; ++mi) {
                float dx = mp.pts[mi][0] - px, dy = mp.pts[mi][1] - py;
                float d = dx * dx + dy * dy;
                if (d < best_d) { best_d = d; best_mp = mi; }
            }
            s_arkit_mp_uv[ai][0] = k_face_uv[best_mp][0];
            s_arkit_mp_uv[ai][1] = k_face_uv[best_mp][1];
        }
    }
    s_remap_cached = true;
}

const float* arkit_mesh_remap_uv() {
    return s_remap_cached ? &s_arkit_mp_uv[0][0] : nullptr;
}

// Build a MediaPipe-format FaceRenderPlan from an ARKit observation.
// Maps ARKit mesh landmarks → MediaPipe indices, computes runtime landmarks
// (chin, cheeks, jaw, nose wings, face sides, brows) from the live mesh, then
// calls face_filter_build_plan_look. The beauty + warp passes use the 468-pt
// MediaPipe landmarks; the textured makeup pass uses the ARKit mesh directly
// (1220 real positions + remapped UVs + 2304 real triangles) when the plan
// has a makeup texture — flagged by has_arkit_mesh.
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

    bool known[FT_NPTS];
    std::memset(known, 0, sizeof(known));

    for (int i = 0; i < FT_NPTS; ++i) {
        int ai = arkit_index_for_mp(i);
        if (ai > 0 && ai < ARKIT_NPTS) {
            mp_obs.pts[i][0] = obs.pts[ai][0];
            mp_obs.pts[i][1] = obs.pts[ai][1];
            known[i] = true;
        }
    }

    // Runtime geometry for stubs + brows.
    compute_mesh_landmarks(obs, mp_obs, known);

    // Remaining verts: IDW from known anchors in canonical UV space.
    // Without this, unmapped points stay at {0,0} and triangles smear to
    // the top-left corner of the frame.
    interpolate_missing_landmarks(mp_obs, known);
    // Snap IDW-estimated positions to the nearest real ARKit vertex in
    // screen space — eliminates IDW smearing without touching ARKit UV space.
    snap_to_arkit(obs, mp_obs, known);

    // Build the ARKit→MediaPipe UV remap (cached after first frame).
    // Uses the 468 MediaPipe screen positions to find containing triangles
    // for each ARKit vertex, then interpolates k_face_uv barycentrically.
    arkit_mesh_remap_uvs(obs, mp_obs);

    bool ok = face_filter_build_plan_look(L, amount, mp_obs, w, h, out);
    if (ok && out.makeup_tex && s_remap_cached)
        out.has_arkit_mesh = true;
    return ok;
}
