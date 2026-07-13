// arkit-map-smoke: regression gate for the ARKit→MediaPipe correspondence.
//
// Projects the canonical ARKit mesh (tools/arkit_face_canonical.obj) into a
// synthetic UNMIRRORED portrait frame (locked contract: person's left = +X
// canonical lands on the RIGHT of the buffer; y grows downward), feeds it
// through face_filter_build_plan_from_arkit, and asserts the evaluated
// MediaPipe landmarks are anatomically placed. This is the gate that would
// have caught the hand-typed landmark table's L/R inversion.
//
// Usage: arkit-map-smoke <path/to/arkit_face_canonical.obj>
#include "arkit_face.h"
#include "face_filters.h"
#include "face_track.h"
#include "generated/arkit_mp_map.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <arkit_face_canonical.obj>\n", argv[0]); return 2; }
    static float verts[ARKIT_NPTS][3];
    int nv = 0;
    FILE* f = fopen(argv[1], "r");
    if (!f) { fprintf(stderr, "arkit map smoke: FAIL — cannot open %s\n", argv[1]); return 1; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'v' && line[1] == ' ' && nv < ARKIT_NPTS) {
            sscanf(line + 2, "%f %f %f", &verts[nv][0], &verts[nv][1], &verts[nv][2]);
            ++nv;
        }
    }
    fclose(f);
    if (nv != ARKIT_NPTS) {
        fprintf(stderr, "arkit map smoke: FAIL — %d verts (want %d)\n", nv, ARKIT_NPTS);
        return 1;
    }

    // Orthographic projection into a 1080x1920 unmirrored portrait frame.
    const int W = 1080, H = 1920;
    const float scale = 4.0f;               // ~145mm face → ~580px wide
    ARKitFaceObs obs{};
    obs.valid = true; obs.w = W; obs.h = H; obs.score = 1.f;
    for (int i = 0; i < ARKIT_NPTS; ++i) {
        obs.pts[i][0] = W * 0.5f + verts[i][0] * scale;   // person's left → +x
        obs.pts[i][1] = H * 0.5f - verts[i][1] * scale;   // canonical +y up → screen y down
    }

    BeautyLook look{};
    FaceRenderPlan plan{};
    if (!face_filter_build_plan_from_arkit(look, 1.f, obs, W, H, plan)) {
        fprintf(stderr, "arkit map smoke: FAIL — plan build rejected\n");
        return 1;
    }

    // Re-evaluate the landmark positions the way the builder does.
    static float mp[FT_NPTS][2];
    for (int i = 0; i < FT_NPTS; ++i) {
        const ArkitMpBary& b = k_mp_from_arkit[i];
        mp[i][0] = b.w0 * obs.pts[b.i0][0] + b.w1 * obs.pts[b.i1][0] + b.w2 * obs.pts[b.i2][0];
        mp[i][1] = b.w0 * obs.pts[b.i0][1] + b.w1 * obs.pts[b.i1][1] + b.w2 * obs.pts[b.i2][1];
    }

    int fails = 0;
    auto expect = [&](bool ok, const char* what) {
        if (!ok) { fprintf(stderr, "  FAIL: %s\n", what); ++fails; }
    };
    float eyeDist = mp[473][0] - mp[468][0];
    // Anatomical L/R under the unmirrored contract (person's right = image left).
    expect(mp[33][0] < mp[263][0], "person's right eye (33) left of left eye (263)");
    expect(mp[468][0] < mp[473][0], "right iris (468) left of left iris (473)");
    expect(mp[61][0] < mp[291][0], "right mouth corner (61) left of left (291)");
    expect(mp[234][0] < mp[454][0], "right face side (234) left of left (454)");
    // Vertical ordering (screen y down).
    expect(mp[10][1] < mp[1][1], "forehead (10) above nose tip (1)");
    expect(mp[1][1] < mp[13][1], "nose tip above upper lip (13)");
    expect(mp[13][1] < mp[14][1], "upper lip above lower lip (14)");
    expect(mp[14][1] < mp[152][1], "lower lip above chin (152)");
    // Irises sit inside their eye corner spans.
    expect(mp[33][0] < mp[468][0] && mp[468][0] < mp[133][0], "right iris within eye corners");
    expect(mp[362][0] < mp[473][0] && mp[473][0] < mp[263][0], "left iris within eye corners");
    // Under-eye structure: the lash line must sit ABOVE the first
    // infraorbital rows with an anatomically plausible band between them —
    // this is the gate for the "makeup ring painted below the eyebag" class
    // of bug (ARKit's oversized eye cutouts must not drag the lid contour
    // down to the hole edge).
    expect(mp[145][1] < mp[230][1] && mp[230][1] < mp[119][1],
           "right under-eye rows ordered lash->ring3->infraorbital");
    expect(mp[374][1] < mp[450][1] && mp[450][1] < mp[348][1],
           "left under-eye rows ordered lash->ring3->infraorbital");
    float band_r = mp[230][1] - mp[145][1];
    expect(band_r > 8.f && band_r < 60.f, "right under-eye band plausible");
    float asym = fabsf((mp[374][1] - mp[145][1]))
               + fabsf(mp[374][0] + mp[145][0] - (float)W);
    expect(asym < 6.f, "lower-lid landmarks L/R symmetric");
    // Weight sanity: unclamped barycentric extrapolation multiplies live
    // tracking noise/deformation. Round 4 shipped weights up to 2.8 and eye
    // makeup jittered and rode blinks on-device.
    float wmax = 0.f;
    for (int i = 0; i < FT_NPTS; ++i) {
        const ArkitMpBary& b = k_mp_from_arkit[i];
        wmax = fmaxf(wmax, fmaxf(fabsf(b.w0), fmaxf(fabsf(b.w1), fabsf(b.w2))));
    }
    expect(wmax < 2.0f, "all barycentric weights bounded (<2.0)");

    // Blink simulation: shift upper-lid verts down 7mm (28px) inside each
    // eye's corner span; lower-lid/under-eye/iris landmarks must hold still
    // while the upper lash follows the lid. Round 4 attached the lower lash
    // to the blinking upper arc (9.7mm jump per 7mm blink) and the static
    // neutral-pose checks above were blind to it.
    {
        ARKitFaceObs blink = obs;
        // Deform the eye-hole UPPER ARC verts directly (fixed constants of
        // ARKit topology, x-ordered outer->inner), with a sine lateral taper
        // — the lid edge travels fully mid-eye and pins at the canthi.
        // Geometric shift-bands sliced through corner verts and broke the
        // stability checks; this is surgical.
        static const int kArcR[10] = {1100, 1099, 1098, 1097, 1096,
                                      1095, 1094, 1093, 1092, 1091};
        static const int kArcL[10] = {1079, 1078, 1077, 1076, 1075,
                                      1074, 1073, 1072, 1071, 1070};
        for (int e = 0; e < 2; ++e) {
            const int* arc = e == 0 ? kArcR : kArcL;
            for (int k = 0; k < 10; ++k)
                blink.pts[arc[k]][1] +=
                    28.f * sinf((float)M_PI * (float)(k + 1) / 11.f);
        }
        auto ev = [&](int i, int axis) {
            const ArkitMpBary& b = k_mp_from_arkit[i];
            return b.w0 * blink.pts[b.i0][axis] + b.w1 * blink.pts[b.i1][axis]
                 + b.w2 * blink.pts[b.i2][axis];
        };
        const int still[] = {145, 374, 230, 450, 468, 473};  // lower lash, under-eye, iris
        for (int k = 0; k < 6; ++k) {
            int i = still[k];
            float dy = fabsf(ev(i, 1) - mp[i][1]);
            char msg[64];
            snprintf(msg, sizeof msg, "mp %d blink-stable (moved %.1fpx)", i, dy);
            expect(dy < 3.f, msg);
        }
        expect(ev(159, 1) - mp[159][1] > 10.f && ev(386, 1) - mp[386][1] > 10.f,
               "upper lash follows the blinking lid");
        // The mid-chain of the lash line must descend UNIFORMLY: mixed
        // attachments (some points on ring pseudo-tris, some surface-snapped
        // to different skin rows) made lash strokes drop chop-by-chop, one
        // stroke at a time, on-device.
        // (Total spread across the chain is legitimate — the deformation has
        // a vertical gradient — but adjacent points must move ALIKE.)
        {
            const int chain[5] = {161, 160, 159, 158, 157};
            float step_max = 0.f, prev = 0.f;
            for (int k = 0; k < 5; ++k) {
                float dy = ev(chain[k], 1) - mp[chain[k]][1];
                if (k > 0) step_max = fmaxf(step_max, fabsf(dy - prev));
                prev = dy;
            }
            char msg[80];
            snprintf(msg, sizeof msg,
                     "lash chain descends smoothly (max adjacent step %.1fpx)",
                     step_max);
            expect(step_max < 8.f, msg);
        }
    }

    // Gaze: the ARKit mesh is eyeball-blind, so the bridge offsets the iris
    // landmarks from the eyeLook* blendshapes — full look-down must move
    // both iris centers DOWN (screen y) and look-in must move them toward
    // the nose. Also pins the blendshape index convention (MediaPipe order,
    // _neutral at 0): a reshuffled array reads the wrong coefficients and
    // this check catches it.
    {
        ARKitFaceObs g = obs;
        g.has_blend = true;
        BeautyLook lk{};
        lk.smooth = 0.5f;   // any beauty element so the plan builds
        FaceRenderPlan p0{};
        expect(face_filter_build_plan_from_arkit(lk, 1.f, g, W, H, p0),
               "baseline plan builds");
        g.blend[FB_EYE_LOOK_DOWN_L] = 1.f;
        g.blend[FB_EYE_LOOK_DOWN_R] = 1.f;
        FaceRenderPlan pd{};
        expect(face_filter_build_plan_from_arkit(lk, 1.f, g, W, H, pd),
               "gaze plan builds");
        expect(pd.beauty.eyeL_y > p0.beauty.eyeL_y + 5.f &&
               pd.beauty.eyeR_y > p0.beauty.eyeR_y + 5.f,
               "irises follow look-down");
        g.blend[FB_EYE_LOOK_DOWN_L] = g.blend[FB_EYE_LOOK_DOWN_R] = 0.f;
        g.blend[FB_EYE_LOOK_IN_L] = 1.f;   // person's left eye looks nose-ward (-x)
        g.blend[FB_EYE_LOOK_IN_R] = 1.f;   // person's right eye nose-ward (+x)
        FaceRenderPlan pi{};
        expect(face_filter_build_plan_from_arkit(lk, 1.f, g, W, H, pi),
               "gaze-in plan builds");
        // eyeA = person's right iris (MP 468), eyeB = left (473)
        expect(pi.beauty.eyeL_x > p0.beauty.eyeL_x + 5.f &&
               pi.beauty.eyeR_x < p0.beauty.eyeR_x - 5.f,
               "irises converge on look-in");

        // Coherence: alternating +/-3px noise on the arc verts (per-vertex
        // ARKit tracking noise) must be rejected by the quadratic chain fit
        // — riding raw verts made lash strokes wobble one by one.
        static const int kArcR2[10] = {1100, 1099, 1098, 1097, 1096,
                                       1095, 1094, 1093, 1092, 1091};
        static const int kArcL2[10] = {1079, 1078, 1077, 1076, 1075,
                                       1074, 1073, 1072, 1071, 1070};
        ARKitFaceObs nz = obs;
        nz.has_blend = false;
        for (int e = 0; e < 2; ++e) {
            const int* arc = e == 0 ? kArcR2 : kArcL2;
            for (int k = 0; k < 10; ++k)
                nz.pts[arc[k]][1] += (k & 1) ? 3.f : -3.f;
        }
        FaceRenderPlan pz{};
        FaceRenderPlan pb{};
        ARKitFaceObs ob2 = obs; ob2.has_blend = false;
        expect(face_filter_build_plan_from_arkit(lk, 1.f, ob2, W, H, pb) &&
               face_filter_build_plan_from_arkit(lk, 1.f, nz, W, H, pz),
               "noise plans build");
        float worst = 0.f;
        for (int k = 0; k < 7; ++k) {
            worst = fmaxf(worst, fabsf(pz.beauty.lidL[k][1] - pb.beauty.lidL[k][1]));
            worst = fmaxf(worst, fabsf(pz.beauty.lidR[k][1] - pb.beauty.lidR[k][1]));
        }
        char msg2[80];
        snprintf(msg2, sizeof msg2,
                 "lash chain rejects per-vertex noise (worst %.1fpx of 3px)",
                 worst);
        expect(worst < 1.8f, msg2);
    }

    // Everything on-frame and face-sized.
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (int i = 0; i < FT_NPTS; ++i) {
        minx = fminf(minx, mp[i][0]); maxx = fmaxf(maxx, mp[i][0]);
        miny = fminf(miny, mp[i][1]); maxy = fmaxf(maxy, mp[i][1]);
    }
    expect(minx > 0 && maxx < W && miny > 0 && maxy < H, "all landmarks on-frame");
    expect(maxx - minx > eyeDist * 2.0f, "face span plausible vs eye distance");
    printf("arkit map smoke: eyeDist=%.0fpx span=[%.0f,%.0f..%.0f,%.0f]\n",
           eyeDist, minx, miny, maxx, maxy);
    if (fails) { fprintf(stderr, "arkit map smoke: FAIL (%d)\n", fails); return 1; }
    printf("arkit map smoke: PASS\n");
    return 0;
}
