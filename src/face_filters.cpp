#include "face_filters.h"
#include "face_cache.h"
#include "paths.h"
#include "gl_compat.h"
#include "stb_image.h"
#include <cmath>
#include <string>
#include <map>

// Anchors come from MediaPipe mesh canonical indices (see face_track.h):
// iris centers 468/473, nose tip 1, nose wings 98/327, inner lip mids 13/14,
// mouth corners 61/291, chin 152, lower jaw 172/397.

static const char* k_names[] = {
    "None", "Natural", "Big Eyes", "Tiny Face", "Big Mouth", "Alien", "Doggy",
    "Douyin", "Porcelain", "Soft Glam", "Honey",
    "Peach", "Cherry", "Goth", "Barbie", "Bronze",
    "Chrome", "Neon", "Cyborg", "Hologram", "Rave", "Baddie", "E-Girl",
    "Doll", "Coquette", "Latte", "Cat Eye", "Peachy Glow", "Cold Beauty",
    "Sunset", "Angel", "Cyber Doll", "Neon Cat", "Void", "Pixel Pop", "Belle",
};
const char* face_filter_name(int id) {
    if (id < 0 || id >= (int)(sizeof(k_names)/sizeof(k_names[0]))) return "?";
    return k_names[id];
}
int face_filter_count() { return (int)(sizeof(k_names)/sizeof(k_names[0])); }

struct Anchors {
    float eyeA[2], eyeB[2], eyeMid[2], eyeDist;
    float nose[2], noseL[2], noseR[2];
    float mouth[2], mouthW;
    float chin[2], jawL[2], jawR[2], faceC[2];
    float up[2], right[2];   // unit face basis (up = chin→eyes)
};

static Anchors anchors_from(const FaceObs& o) {
    Anchors a;
    a.eyeA[0] = o.pts[468][0]; a.eyeA[1] = o.pts[468][1];   // iris centers
    a.eyeB[0] = o.pts[473][0]; a.eyeB[1] = o.pts[473][1];
    a.eyeMid[0] = (a.eyeA[0] + a.eyeB[0]) * 0.5f;
    a.eyeMid[1] = (a.eyeA[1] + a.eyeB[1]) * 0.5f;
    float dx = a.eyeB[0] - a.eyeA[0], dy = a.eyeB[1] - a.eyeA[1];
    a.eyeDist = sqrtf(dx*dx + dy*dy);
    a.nose[0]  = o.pts[1][0];   a.nose[1]  = o.pts[1][1];    // tip
    a.noseL[0] = o.pts[98][0];  a.noseL[1] = o.pts[98][1];   // wings
    a.noseR[0] = o.pts[327][0]; a.noseR[1] = o.pts[327][1];
    a.mouth[0] = (o.pts[13][0] + o.pts[14][0]) * 0.5f;       // inner lip mids
    a.mouth[1] = (o.pts[13][1] + o.pts[14][1]) * 0.5f;
    a.chin[0] = o.pts[152][0]; a.chin[1] = o.pts[152][1];
    a.jawL[0] = o.pts[172][0]; a.jawL[1] = o.pts[172][1];
    a.jawR[0] = o.pts[397][0]; a.jawR[1] = o.pts[397][1];
    a.faceC[0] = (a.eyeMid[0] + a.chin[0]) * 0.5f;
    a.faceC[1] = (a.eyeMid[1] + a.chin[1]) * 0.5f;
    // Face basis — frame axes lie when the camera is rotated.
    float ux = a.eyeMid[0] - a.chin[0], uy = a.eyeMid[1] - a.chin[1];
    float ul = sqrtf(ux*ux + uy*uy); if (ul < 1.f) ul = 1.f;
    a.up[0] = ux / ul; a.up[1] = uy / ul;
    float rx = a.eyeB[0] - a.eyeA[0], ry = a.eyeB[1] - a.eyeA[1];
    float rl = sqrtf(rx*rx + ry*ry); if (rl < 1.f) rl = 1.f;
    a.right[0] = rx / rl; a.right[1] = ry / rl;
    // Mouth width = corner-to-corner (61/291) — pose-stable.
    float mwx = o.pts[291][0] - o.pts[61][0];
    float mwy = o.pts[291][1] - o.pts[61][1];
    a.mouthW = sqrtf(mwx*mwx + mwy*mwy);
    return a;
}

// Blendshape helper: 0 when the observation carries none.
static inline float bl(const FaceObs& o, int idx) {
    return o.has_blend ? o.blend[idx] : 0.f;
}

// One parametric beauty engine, five looks. Skin params feed the GPU beauty
// pass (face_beauty_apply); shape scalars feed the same warp bumps the old
// Pretty used. Values tuned against the picker preview face.
// Reference aesthetic: Douyin beauty. The rules that matter — the chin gets
// NARROWER AND POINTED (V-line), never shorter (pushing it up reads as a
// square flat chin); cheek slimming sits at the mid-jaw, gently; skin runs
// porcelain-bright; and makeup (blush, lip tint, eye pop) does the heavy
// lifting that warps used to overdo.
static void set3(float* d, float r, float g, float b) { d[0]=r; d[1]=g; d[2]=b; }
bool beauty_look_for(int filter_id, BeautyLook& L) {
    switch ((FaceFilter)filter_id) {
        case FaceFilter::Pretty:     // "Natural" — believable everyday clean-up
            L = {0.42f, 0.16f, 0.08f, 0.15f, 0.10f, 0.08f,  0.06f, 0.03f, 0.05f, 0.10f, 0.03f};
                        L.lash = 0.25f; L.liner = 0.15f; L.lash_wing = 0.10f;
            return true;
        case FaceFilter::Glam:       // "Douyin" — the reference look
            // Alien-avoidance: pale but NOT flat — moderate brighten, a hint
            // of warmth, and the under-jaw shadow restores the dimension the
            // smoothing removes. Chin tuck + shade hide a double chin.
            // The painted UV texture carries lips/liner/lash/blush; the
            // procedural layers keep skin, shape, and the chin treatment
            // (light procedural echoes underlay the texture).
            L = {0.68f, 0.30f, 0.05f, 0.42f, 0.15f, 0.12f,  0.13f, 0.09f, 0.24f, 0.18f, 0.05f};
            set3(L.blush_col, 1.f, 0.55f, 0.60f);
            set3(L.lip_col,   0.88f, 0.16f, 0.24f);
            L.chin_smooth = 0.92f; L.jaw_shade = 0.f;
            L.makeup_tex = "makeup_douyin.png";
            L.lash = 0.15f; L.liner = 0.00f; L.lash_wing = 0.20f;
            return true;
        case FaceFilter::Porcelain:  // maximum skin, cool light, shape untouched
            L = {0.92f, 0.38f, 0.00f, 0.28f, 0.14f, 0.10f,  0.03f, 0.f,   0.f,   0.f,   0.f};
                        L.lash = 0.22f; L.liner = 0.12f; L.lash_wing = 0.08f;
            return true;
        case FaceFilter::Sculpt:     // "Soft Glam" — makeup-forward, moderate shape
            L = {0.55f, 0.28f, 0.12f, 0.34f, 0.34f, 0.30f,  0.10f, 0.07f, 0.10f, 0.14f, 0.06f};
            L.chin_smooth = 0.46f; L.jaw_shade = 0.f;
                        L.lash = 0.55f; L.liner = 0.45f; L.lash_wing = 0.40f;
            return true;
        case FaceFilter::Honey:      // warm golden glow, soft everything
            L = {0.60f, 0.34f, 0.45f, 0.24f, 0.24f, 0.16f,  0.08f, 0.04f, 0.06f, 0.08f, 0.05f};
                        L.lash = 0.40f; L.liner = 0.25f; L.lash_wing = 0.20f;
            return true;

        // ── Makeup looks ─────────────────────────────────────────────────
        case FaceFilter::Peach:      // sunny coral — summer skin
            L = {0.58f, 0.30f, 0.30f, 0.25f, 0.50f, 0.42f,  0.08f, 0.05f, 0.07f, 0.10f, 0.05f};
            set3(L.blush_col, 1.f, 0.55f, 0.38f);
            set3(L.lip_col,   1.f, 0.42f, 0.30f);
                        L.lash = 0.45f; L.liner = 0.30f; L.lash_wing = 0.25f;
            return true;
        case FaceFilter::Cherry:     // K-drama cherry lips on pale skin
            L = {0.72f, 0.34f, 0.02f, 0.28f, 0.22f, 0.62f,  0.09f, 0.05f, 0.09f, 0.12f, 0.06f};
            set3(L.blush_col, 1.f, 0.55f, 0.62f);
            set3(L.lip_col,   0.85f, 0.08f, 0.16f);
                        L.lash = 0.50f; L.liner = 0.40f; L.lash_wing = 0.30f;
            return true;
        case FaceFilter::Goth:       // pale, cool, plum-black lips
            L = {0.62f, 0.20f, 0.00f, 0.35f, 0.10f, 0.75f,  0.07f, 0.05f, 0.08f, 0.10f, 0.03f};
            set3(L.blush_col, 0.75f, 0.55f, 0.70f);
            set3(L.lip_col,   0.28f, 0.05f, 0.14f);
            L.desat = 0.28f;
                        L.lash = 0.80f; L.liner = 0.85f; L.lash_wing = 0.60f;
            return true;
        case FaceFilter::Barbie:     // maximum pink everything
            L = {0.78f, 0.42f, 0.10f, 0.42f, 0.60f, 0.55f,  0.14f, 0.08f, 0.12f, 0.16f, 0.08f};
            set3(L.blush_col, 1.f, 0.45f, 0.75f);
            set3(L.lip_col,   1.f, 0.25f, 0.60f);
                        L.lash = 0.70f; L.liner = 0.50f; L.lash_wing = 0.40f;
            return true;
        case FaceFilter::Bronze:     // golden-hour bronze glow
            L = {0.60f, 0.30f, 0.55f, 0.30f, 0.38f, 0.30f,  0.08f, 0.06f, 0.08f, 0.10f, 0.04f};
            set3(L.blush_col, 0.95f, 0.60f, 0.35f);
            set3(L.lip_col,   0.80f, 0.42f, 0.28f);
                        L.lash = 0.50f; L.liner = 0.40f; L.lash_wing = 0.45f;
            return true;

        // ── Cyber looks ──────────────────────────────────────────────────
        case FaceFilter::Chrome:     // liquid-metal skin
            L = {0.85f, 0.00f, 0.f, 0.f, 0.f, 0.f,  0.06f, 0.04f, 0.06f, 0.f, 0.f};
            L.desat = 0.85f; L.chrome = 0.60f; L.skin_tint = 0.22f;
            set3(L.tint_col, 0.75f, 0.82f, 0.95f);
            L.eye_glow = 0.25f; set3(L.eye_glow_col, 0.8f, 0.9f, 1.f);
                        L.lash = 0.30f; L.liner = 0.40f; L.lash_wing = 0.50f;
            return true;
        case FaceFilter::Neon:       // electric magenta/cyan club face
            L = {0.55f, 0.15f, 0.f, 0.f, 0.55f, 0.60f,  0.10f, 0.05f, 0.08f, 0.10f, 0.05f};
            set3(L.blush_col, 0.25f, 0.85f, 1.f);        // cyan cheek light
            set3(L.lip_col,   1.f, 0.10f, 0.80f);        // electric magenta
            L.eye_glow = 0.55f; set3(L.eye_glow_col, 1.f, 0.15f, 0.85f);
            L.skin_tint = 0.10f; set3(L.tint_col, 0.75f, 0.65f, 1.f);
                        L.lash = 0.60f; L.liner = 0.70f; L.lash_wing = 0.80f;
            return true;
        case FaceFilter::Cyborg:     // cold steel + red optics
            L = {0.75f, 0.05f, 0.f, 0.f, 0.f, 0.f,  0.05f, 0.04f, 0.05f, 0.f, 0.f};
            L.desat = 0.7f; L.chrome = 0.45f; L.skin_tint = 0.35f;
            set3(L.tint_col, 0.62f, 0.72f, 0.85f);
            L.eye_glow = 0.75f; set3(L.eye_glow_col, 1.f, 0.12f, 0.10f);
                        L.lash = 0.30f; L.liner = 0.50f; L.lash_wing = 0.40f;
            return true;
        case FaceFilter::Hologram:   // scanlined cyan projection
            L = {0.50f, 0.20f, 0.f, 0.f, 0.f, 0.f,  0.f, 0.f, 0.f, 0.f, 0.f};
            L.skin_tint = 0.55f; set3(L.tint_col, 0.35f, 0.95f, 1.f);
            L.desat = 0.5f; L.scanlines = 0.85f;
            L.eye_glow = 0.35f; set3(L.eye_glow_col, 0.4f, 1.f, 1.f);
                        L.lash = 0.35f; L.liner = 0.40f; L.lash_wing = 0.40f;
            return true;
        case FaceFilter::Baddie:     // 2016 Instagram: matte nude lip, contour, bronze
            L = {0.70f, 0.30f, 0.35f, 0.38f, 0.40f, 0.60f,  0.10f, 0.12f, 0.14f, 0.18f, 0.14f};
            set3(L.blush_col, 0.80f, 0.52f, 0.38f);      // bronze contour tone
            set3(L.lip_col,   0.70f, 0.44f, 0.40f);      // matte nude mauve
            L.lip_grad = 0.15f;                          // full matte coverage
            L.blush_raise = 0.05f;                       // low = contour line
            L.chin_smooth = 0.52f; L.jaw_shade = 0.f;    // contour look = strong shade
                        L.lash = 0.75f; L.liner = 0.60f; L.lash_wing = 0.60f;
            return true;
        case FaceFilter::EGirl:      // nose blush, faux freckles, glossy pink lip
            L = {0.55f, 0.28f, 0.10f, 0.35f, 0.50f, 0.45f,  0.12f, 0.04f, 0.08f, 0.10f, 0.10f};
            set3(L.blush_col, 1.f, 0.44f, 0.44f);        // warm sunburn pink
            set3(L.lip_col,   0.95f, 0.40f, 0.46f);      // glossy pink
            L.lip_grad = 0.40f;
            L.blush_raise = 0.55f;                       // high, under the eyes
            L.nose_blush  = 0.60f;                       // across the nose
            L.freckles    = 0.55f;
                        L.lash = 0.65f; L.liner = 0.70f; L.lash_wing = 0.55f;
            return true;
        case FaceFilter::Rave:       // UV blacklight — purple skin, acid accents
            L = {0.55f, 0.10f, 0.f, 0.f, 0.55f, 0.60f,  0.10f, 0.f, 0.f, 0.f, 0.06f};
            L.skin_tint = 0.40f; set3(L.tint_col, 0.55f, 0.35f, 1.f);
            set3(L.blush_col, 0.35f, 1.f, 0.45f);        // acid green cheeks
            set3(L.lip_col,   0.95f, 0.95f, 0.20f);      // yellow lip
            L.eye_glow = 0.45f; set3(L.eye_glow_col, 0.5f, 1.f, 0.3f);
            L.scanlines = 0.20f;
                        L.lash = 0.60f; L.liner = 0.60f; L.lash_wing = 0.70f;
            return true;
        // ── Lash + blush generation ──────────────────────────────────────
        case FaceFilter::Doll:       // porcelain doll: max lashes, round pink
            L = {0.70f, 0.34f, 0.05f, 0.45f, 0.55f, 0.45f,  0.16f, 0.06f, 0.10f, 0.14f, 0.08f};
            set3(L.blush_col, 1.f, 0.50f, 0.62f);
            set3(L.lip_col,   0.96f, 0.35f, 0.45f);
            L.lash = 0.85f; L.lash_wing = 0.35f; L.blush_raise = 0.50f; L.liner = 0.50f;
            L.chin_smooth = 0.40f;
            return true;
        case FaceFilter::Coquette:   // soft bows-and-blush: rosy, gentle lash
            L = {0.60f, 0.28f, 0.12f, 0.30f, 0.55f, 0.40f,  0.09f, 0.05f, 0.08f, 0.10f, 0.06f};
            set3(L.blush_col, 1.f, 0.48f, 0.52f);
            set3(L.lip_col,   0.92f, 0.34f, 0.40f);
            L.lash = 0.55f; L.lash_wing = 0.20f; L.blush_raise = 0.55f; L.liner = 0.30f;
            L.nose_blush = 0.35f;
            return true;
        case FaceFilter::Latte:      // warm browns: bronze blush, nude lip, brown lash
            L = {0.62f, 0.28f, 0.35f, 0.30f, 0.42f, 0.45f,  0.08f, 0.07f, 0.10f, 0.12f, 0.06f};
            set3(L.blush_col, 0.85f, 0.58f, 0.40f);
            set3(L.lip_col,   0.72f, 0.46f, 0.36f);
            L.lash = 0.60f; L.lash_wing = 0.40f; L.lip_grad = 0.25f; L.liner = 0.45f;
            L.blush_raise = 0.20f; L.jaw_shade = 0.f;
            return true;
        case FaceFilter::CatEye:     // the dramatic wing: liner-first, red lip
            L = {0.60f, 0.26f, 0.06f, 0.40f, 0.20f, 0.55f,  0.12f, 0.06f, 0.12f, 0.14f, 0.04f};
            set3(L.blush_col, 1.f, 0.55f, 0.55f);
            set3(L.lip_col,   0.80f, 0.10f, 0.16f);
            L.lash = 0.90f; L.lash_wing = 0.95f; L.lip_grad = 0.20f; L.liner = 1.0f;
            return true;
        case FaceFilter::PeachyGlow: // juicy peach blush everywhere, gloss
            L = {0.62f, 0.34f, 0.30f, 0.32f, 0.65f, 0.42f,  0.10f, 0.05f, 0.08f, 0.10f, 0.08f};
            set3(L.blush_col, 1.f, 0.58f, 0.42f);
            set3(L.lip_col,   1.f, 0.48f, 0.36f);
            L.lash = 0.45f; L.lash_wing = 0.15f; L.blush_raise = 0.45f;
            L.nose_blush = 0.30f;
            return true;
        case FaceFilter::ColdBeauty: // pale gray-cool, wine lip, sharp lash
            L = {0.72f, 0.30f, 0.00f, 0.38f, 0.16f, 0.55f,  0.12f, 0.08f, 0.16f, 0.16f, 0.03f};
            set3(L.blush_col, 0.85f, 0.60f, 0.70f);
            set3(L.lip_col,   0.52f, 0.10f, 0.20f);
            L.lash = 0.75f; L.lash_wing = 0.55f; L.desat = 0.18f; L.liner = 0.75f;
            L.lip_grad = 0.30f; L.jaw_shade = 0.f;
            return true;
        case FaceFilter::Sunset:     // orange-pink heat: heavy warm blush, coral lip
            L = {0.60f, 0.32f, 0.40f, 0.32f, 0.70f, 0.45f,  0.10f, 0.05f, 0.08f, 0.10f, 0.07f};
            set3(L.blush_col, 1.f, 0.44f, 0.34f);
            set3(L.lip_col,   0.98f, 0.36f, 0.28f);
            L.lash = 0.50f; L.lash_wing = 0.30f; L.blush_raise = 0.40f;
            L.nose_blush = 0.45f;
            return true;
        case FaceFilter::Angel:      // luminous white-pink: bright, soft lash, glow
            L = {0.75f, 0.44f, 0.02f, 0.45f, 0.40f, 0.35f,  0.13f, 0.05f, 0.10f, 0.12f, 0.06f};
            set3(L.blush_col, 1.f, 0.60f, 0.68f);
            set3(L.lip_col,   0.95f, 0.45f, 0.52f);
            L.lash = 0.50f; L.lash_wing = 0.25f; L.blush_raise = 0.60f; L.liner = 0.30f;
            L.eye_glow = 0.20f; set3(L.eye_glow_col, 1.f, 0.92f, 0.95f);
            return true;

        // ── Lash + cyber mixes ───────────────────────────────────────────
        case FaceFilter::CyberDoll:  // doll lashes on chrome-pink android skin
            L = {0.75f, 0.20f, 0.f, 0.35f, 0.45f, 0.40f,  0.15f, 0.06f, 0.10f, 0.14f, 0.06f};
            set3(L.blush_col, 1.f, 0.35f, 0.75f);
            set3(L.lip_col,   1.f, 0.30f, 0.70f);
            L.lash = 0.85f; L.lash_wing = 0.40f; L.liner = 0.55f;
            L.skin_tint = 0.22f; set3(L.tint_col, 1.f, 0.80f, 0.92f);
            L.eye_glow = 0.40f; set3(L.eye_glow_col, 1.f, 0.35f, 0.80f);
            return true;
        case FaceFilter::NeonCat:    // huge wing + cyan glow, electric lip
            L = {0.60f, 0.20f, 0.f, 0.40f, 0.30f, 0.55f,  0.13f, 0.06f, 0.12f, 0.12f, 0.04f};
            set3(L.blush_col, 0.30f, 0.85f, 1.f);
            set3(L.lip_col,   0.95f, 0.12f, 0.75f);
            L.lash = 0.95f; L.lash_wing = 1.0f; L.liner = 0.90f;
            L.eye_glow = 0.55f; set3(L.eye_glow_col, 0.2f, 0.95f, 1.f);
            L.skin_tint = 0.08f; set3(L.tint_col, 0.80f, 0.75f, 1.f);
            return true;
        case FaceFilter::Void:       // goth-cyber: black lip, heavy lash, purple glow
            L = {0.65f, 0.16f, 0.f, 0.40f, 0.12f, 0.70f,  0.10f, 0.07f, 0.12f, 0.12f, 0.03f};
            set3(L.blush_col, 0.60f, 0.45f, 0.70f);
            set3(L.lip_col,   0.12f, 0.04f, 0.10f);
            L.lash = 0.90f; L.lash_wing = 0.65f; L.lip_grad = 0.10f; L.liner = 0.85f;
            L.desat = 0.35f; L.eye_glow = 0.45f; set3(L.eye_glow_col, 0.55f, 0.25f, 0.95f);
            L.jaw_shade = 0.f;
            return true;
        case FaceFilter::PixelPop:   // arcade: light scanlines, candy blush, teal lip
            L = {0.55f, 0.28f, 0.05f, 0.35f, 0.55f, 0.50f,  0.11f, 0.04f, 0.08f, 0.10f, 0.06f};
            set3(L.blush_col, 1.f, 0.45f, 0.55f);
            set3(L.lip_col,   0.15f, 0.80f, 0.75f);
            L.lash = 0.55f; L.lash_wing = 0.35f; L.lip_grad = 0.15f;
            L.scanlines = 0.30f; L.eye_glow = 0.30f; set3(L.eye_glow_col, 1.f, 0.85f, 0.30f);
            return true;
        case FaceFilter::Belle:      // huge doll eyes, max lash, porcelain-pink
            L = {0.72f, 0.40f, 0.02f, 0.50f, 0.60f, 0.45f,  0.25f, 0.05f, 0.10f, 0.12f, 0.10f};
            set3(L.blush_col, 1.f, 0.42f, 0.55f);        // hot pink
            set3(L.lip_col,   0.98f, 0.38f, 0.48f);      // glossy pink
            L.lash = 0.95f; L.liner = 0.80f; L.lash_wing = 0.50f;
            L.blush_raise = 0.55f; L.nose_blush = 0.50f;
            L.chin_smooth = 0.46f;
            return true;
        default: return false;
    }
}

// Shape half of a parametric beauty look — shared by the enum path below and
// the Makeup Studio's custom looks (face_fx live entries on Metal).
int face_filter_bumps_look(const BeautyLook& L, float amount, const FaceObs& obs,
                           FaceWarpBump* out) {
    if (!obs.valid || obs.w <= 0 || obs.h <= 0) return 0;
    Anchors a = anchors_from(obs);
    const float iw = 1.f / obs.w, ih = 1.f / obs.h;
    const float eyeR  = a.eyeDist * 0.40f * ih;
    const float amt   = amount;
    int n = 0;
    auto bump = [&](const float* c, float radius, float scale,
                    float dxp, float dyp) {
        if (n >= MAX_FACE_BUMPS) return;
        out[n].cx = c[0] * iw;  out[n].cy = c[1] * ih;
        out[n].radius = radius;
        out[n].scale  = scale;
        out[n].dx = dxp * iw;   out[n].dy = dyp * ih;
        ++n;
    };
    auto inward = [&](const float* p, float k, float* dxy) {
        float vx = a.faceC[0] - p[0], vy = a.faceC[1] - p[1];
        float along = vx * a.up[0] + vy * a.up[1];
        dxy[0] = (vx - along * a.up[0]) * k;
        dxy[1] = (vy - along * a.up[1]) * k;
    };
            float d[2];
            if (L.eyes > 0.f) {
                bump(a.eyeA, eyeR, L.eyes * amt, 0, 0);
                bump(a.eyeB, eyeR, L.eyes * amt, 0, 0);
            }
            // Lower-face shaping: ONE smooth graded field along the whole jaw
            // contour. The previous version used four strong pulls with
            // small radii — the outline kinked into sharp corners ("square
            // sharp jaw") and the flesh between pull points lagged and
            // bulged ("droops my cheeks like a dog"). Here every contour
            // point gets a LOW-strength pull with a LARGE radius; the
            // overlapping fields blend into a clean taper.
            if (L.vline > 0.f || L.cheek > 0.f) {
                float vx = a.chin[0] - a.up[0] * a.eyeDist * 0.25f;
                float vy = a.chin[1] - a.up[1] * a.eyeDist * 0.25f;
                // Jaw contour ear→chin, with a grade t (0 near ear, 1 at the
                // chin sides). V-line strength rises toward the chin; cheek
                // slimming fades out toward it.
                static const int   kJawL[5] = {132, 172, 136, 149, 176};
                static const int   kJawR[5] = {361, 397, 365, 378, 400};
                static const float kGrade[5] = {0.25f, 0.45f, 0.65f, 0.85f, 1.0f};
                for (int j = 0; j < 5; ++j) {
                    for (int side = 0; side < 2; ++side) {
                        const float* pnt = obs.pts[side ? kJawR[j] : kJawL[j]];
                        float g = kGrade[j];
                        // toward the V point, graded up the chain
                        float k1 = L.vline * amt * 0.45f * (g * g);
                        float dxv = (vx - pnt[0]) * k1, dyv = (vy - pnt[1]) * k1;
                        // inward (cheek slim), fading toward the chin
                        inward(pnt, L.cheek * amt * (1.f - g) * 0.9f, d);
                        bump(pnt, a.eyeDist * 0.60f * ih, 0.f,
                             dxv + d[0], dyv + d[1]);
                    }
                }
            }
            // Chin tuck: the double-chin fold sits BELOW the mesh, at
            // chin - up*~0.45ed. Pull that region up toward the chin so the
            // fold compresses under the jawline.
            if (L.chin_tuck > 0.f) {
                float fold[2] = {a.chin[0] - a.up[0] * a.eyeDist * 0.45f,
                                 a.chin[1] - a.up[1] * a.eyeDist * 0.45f};
                float k = L.chin_tuck * amt * 0.48f;
                // ONE tight bump. The second, lower pass reached the
                // necklace/chest — warped neck texture wobbles with head
                // motion and reads as a band. The fold sits right under the
                // chin; that's the only place the tuck belongs.
                bump(fold, a.eyeDist * 0.50f * ih, 0.f,
                     (a.chin[0] - fold[0]) * k, (a.chin[1] - fold[1]) * k);
            }
            if (L.nose > 0.f) {
                bump(a.noseL, a.eyeDist * 0.22f * ih, 0.f,
                     (a.nose[0] - a.noseL[0]) * L.nose * amt,
                     (a.nose[1] - a.noseL[1]) * L.nose * amt);
                bump(a.noseR, a.eyeDist * 0.22f * ih, 0.f,
                     (a.nose[0] - a.noseR[0]) * L.nose * amt,
                     (a.nose[1] - a.noseR[1]) * L.nose * amt);
            }
            if (L.lips_plump > 0.f)
                bump(a.mouth, a.mouthW * 0.55f * ih, L.lips_plump * amt, 0, 0);
    return n;
}

int face_filter_bumps(int filter_id, float amount, const FaceObs& obs,
                      FaceWarpBump* out) {
    if (!obs.valid || obs.w <= 0 || obs.h <= 0) return 0;
    Anchors a = anchors_from(obs);
    const float iw = 1.f / obs.w, ih = 1.f / obs.h;
    // radii in frame-height units so they're aspect-stable
    const float eyeR  = a.eyeDist * 0.40f * ih;
    const float amt   = amount;
    int n = 0;
    auto bump = [&](const float* c, float radius, float scale,
                    float dxp, float dyp) {
        if (n >= MAX_FACE_BUMPS) return;
        out[n].cx = c[0] * iw;  out[n].cy = c[1] * ih;
        out[n].radius = radius;
        out[n].scale  = scale;
        out[n].dx = dxp * iw;   out[n].dy = dyp * ih;
        ++n;
    };
    // Push a point toward the face's central axis: take the component of
    // (faceC - p) perpendicular to `up` — rotation-proof inward direction.
    auto inward = [&](const float* p, float k, float* dxy) {
        float vx = a.faceC[0] - p[0], vy = a.faceC[1] - p[1];
        float along = vx * a.up[0] + vy * a.up[1];
        dxy[0] = (vx - along * a.up[0]) * k;
        dxy[1] = (vy - along * a.up[1]) * k;
    };

    switch ((FaceFilter)filter_id) {
        case FaceFilter::Pretty:
        case FaceFilter::Glam:
        case FaceFilter::Porcelain:
        case FaceFilter::Sculpt:
        case FaceFilter::Honey: {
            // Shape half of the beauty looks (skin half runs on the GPU in
            // face_beauty_apply) — shared with the Studio path.
            BeautyLook L;
            if (!beauty_look_for(filter_id, L)) break;
            return face_filter_bumps_look(L, amount, obs, out);
        }
        case FaceFilter::BigEyes: {
            // Expression-reactive: widen with eyeWide, deflate on blinks —
            // the cartoon eyes squash when you blink instead of bulging shut.
            float wide  = (bl(obs, FB_EYE_WIDE_L) + bl(obs, FB_EYE_WIDE_R)) * 0.5f;
            float blink = (bl(obs, FB_EYE_BLINK_L) + bl(obs, FB_EYE_BLINK_R)) * 0.5f;
            float k = (1.f + 0.6f * wide) * (1.f - 0.75f * blink);
            bump(a.eyeA, eyeR * 1.25f, 0.42f * amt * k, 0, 0);
            bump(a.eyeB, eyeR * 1.25f, 0.42f * amt * k, 0, 0);
            break;
        }
        case FaceFilter::TinyFace: {
            float c[2] = {a.faceC[0], a.faceC[1]};
            bump(c, a.eyeDist * 1.6f * ih, -0.28f * amt, 0, 0);
            break;
        }
        case FaceFilter::BigMouth: {
            // Opens with the jaw: shouting blows the mouth up further.
            float k = 1.f + 0.9f * bl(obs, FB_JAW_OPEN);
            bump(a.mouth, a.mouthW * 1.1f * ih * (1.f + 0.3f * bl(obs, FB_JAW_OPEN)),
                 0.55f * amt * k, 0, 0);
            break;
        }
        case FaceFilter::Alien: {
            float d[2];
            // Raised brows inflate the alien eyes further (surprise!).
            float brow = bl(obs, FB_BROW_INNER_UP);
            float ke = a.eyeDist * 0.04f * amt;   // eyes drift up + huge
            bump(a.eyeA, eyeR * 1.5f, (0.5f + 0.25f * brow) * amt, a.up[0]*ke, a.up[1]*ke);
            bump(a.eyeB, eyeR * 1.5f, (0.5f + 0.25f * brow) * amt, a.up[0]*ke, a.up[1]*ke);
            bump(a.mouth, a.mouthW * 0.8f * ih, -0.30f * amt, 0, 0);
            inward(a.jawL, 0.22f * amt, d);
            bump(a.jawL, a.eyeDist * 0.6f * ih, 0.f, d[0], d[1]);
            inward(a.jawR, 0.22f * amt, d);
            bump(a.jawR, a.eyeDist * 0.6f * ih, 0.f, d[0], d[1]);
            float kc = a.eyeDist * 0.10f * amt;   // long chin (push down = −up)
            bump(a.chin, a.eyeDist * 0.5f * ih, 0.f, -a.up[0]*kc, -a.up[1]*kc);
            break;
        }
        case FaceFilter::Doggy:
            // gentle puppy snout: slight nose enlarge — the rest is overlay
            bump(a.nose, a.eyeDist * 0.30f * ih, 0.15f * amt, 0, 0);
            break;
        default: break;
    }
    return n;
}

// ── Doggy overlay ─────────────────────────────────────────────────────────────
// An homage, drawn with the draw list: floppy brown ears pinned above the
// brows, a big rounded snout-nose, and a tongue when the mouth opens.

// Sprites generated offline (models/face/sprite_*.png) — pre-rendered art
// looks like the actual filter; flat polygons looked like bandages.
static GLuint sprite_tex(const char* name, int& w, int& h) {
    struct Slot { GLuint tex = 0; int w = 0, h = 0; bool tried = false; };
    static std::map<std::string, Slot> cache;
    auto& sl = cache[name];
    if (!sl.tried) {
        sl.tried = true;
        std::string path = app_models_dir() + "/face/" + name;
        int c = 0;
        unsigned char* px = stbi_load(path.c_str(), &sl.w, &sl.h, &c, 4);
#if PMS_HAS_GL
        if (px) {
            glGenTextures(1, &sl.tex);
            glBindTexture(GL_TEXTURE_2D, sl.tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sl.w, sl.h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, px);
            glBindTexture(GL_TEXTURE_2D, 0);
            stbi_image_free(px);
        }
#else
        if (px) stbi_image_free(px);
#endif
    }
    w = sl.w; h = sl.h;
    return sl.tex;
}

int face_filter_doggy_quads(const FaceObs& obs, float amount, float t,
                            FaceSpriteQuad* out, int max_out) {
    if (!obs.valid || obs.w <= 0 || obs.h <= 0 || max_out <= 0) return 0;
    Anchors a = anchors_from(obs);
    const float iw = 1.f / obs.w, ih = 1.f / obs.h;
    const float ed = a.eyeDist;
    float upx = a.up[0], upy = a.up[1];
    float rx = a.right[0], ry = a.right[1];
    int n = 0;

    // One sprite: center/size in face-basis units, tilt in degrees (positive
    // leans outward to the sprite's right). Corners land in frame UV — the
    // mirror maps them through to_screen, the compositor uses them directly.
    // pin_top: (cx_fb, cy_fb) anchors the TOP-CENTER edge instead of the
    // center — the sprite hangs from that point and tilt swings around it.
    auto sprite = [&](const char* name, float cx_fb, float cy_fb,
                      float width_fb, float tilt_deg, bool flip,
                      bool pin_top = false) {
        if (n >= max_out) return;
        int sw = 0, sh = 0;
        GLuint tex = sprite_tex(name, sw, sh);
        if (!tex || sw <= 0) return;
        float t = tilt_deg * 3.14159265f / 180.f;
        float axx = rx * cosf(t) + upx * sinf(t);    // sprite local +x
        float axy = ry * cosf(t) + upy * sinf(t);
        float ayx = -rx * sinf(t) + upx * cosf(t);   // sprite local +y (up)
        float ayy = -ry * sinf(t) + upy * cosf(t);
        float hw = width_fb * ed * 0.5f;
        float hh = hw * (float)sh / (float)sw;
        float cx = a.eyeMid[0] + rx * cx_fb * ed + upx * cy_fb * ed;
        float cy = a.eyeMid[1] + ry * cx_fb * ed + upy * cy_fb * ed;
        if (pin_top) {                // top-center = anchor for any tilt
            cx -= ayx * hh;
            cy -= ayy * hh;
        }
        FaceSpriteQuad& q = out[n++];
        q.tex = tex;
        const float oxs[4] = {-hw,  hw,  hw, -hw};   // tl tr br bl
        const float oys[4] = { hh,  hh, -hh, -hh};   // +hh along local up = art top
        for (int i = 0; i < 4; ++i) {
            q.p[i][0] = (cx + axx * oxs[i] + ayx * oys[i]) * iw;
            q.p[i][1] = (cy + axy * oxs[i] + ayy * oys[i]) * ih;
        }
        q.u0 = flip ? 1.f : 0.f;
        q.u1 = flip ? 0.f : 1.f;
    };

    float sc = 0.6f + 0.4f * amount;   // strength scales the costume
    // Ears sit at the CROWN: high enough (2.0 ed above the eye line) to read
    // as on top of the head — at 1.55 the downhill ear landed on the hair.
    sprite("sprite_ear.png", -1.15f, 2.0f, 1.25f * sc,  20.f, true);
    sprite("sprite_ear.png",  1.15f, 2.0f, 1.25f * sc, -20.f, false);
    // Tongue before nose (Snapchat layering — nose draws on top). Openness
    // from the INNER lip ring (64–71) — the outer ring includes lip thickness
    // and fired on a closed mouth.
    {
        // Expression-reactive: jawOpen IS the openness signal when blendshapes
        // are present (deterministic across mirror / playback / export — the
        // coefficient rides the cache). Geometric inner-lip gap (13↔14) is the
        // fallback for blend-less observations.
        float openness;
        if (obs.has_blend) {
            openness = obs.blend[FB_JAW_OPEN];
        } else {
            float gx = obs.pts[14][0] - obs.pts[13][0];
            float gy = obs.pts[14][1] - obs.pts[13][1];
            float gap = fabsf(gx * upx + gy * upy);
            openness = gap / (a.mouthW > 1.f ? a.mouthW : 1.f);
        }
        float ext = (openness - 0.12f) / 0.30f;
        ext = fmaxf(0.f, fminf(1.f, ext));
        ext = ext * ext * (3.f - 2.f * ext);          // smoothstep
        if (ext > 0.02f) {
            // Root the tongue at the INNER LOWER LIP (mesh 14), tucked 0.10 ed
            // upward so the root hides behind the lip; growth extends downward
            // from there and the wag swings around the root.
            float lip_oy;
            {
                float vx = obs.pts[14][0] - a.eyeMid[0];
                float vy = obs.pts[14][1] - a.eyeMid[1];
                lip_oy = (vx * upx + vy * upy) / ed;
            }
            float mx = a.mouth[0] - a.eyeMid[0], my = a.mouth[1] - a.eyeMid[1];
            float ox = (mx * rx + my * ry) / ed;
            float wag = sinf(t * 9.f) * 7.f * ext;    // degrees
            sprite("sprite_tongue.png", ox, lip_oy + 0.10f,
                   0.85f * sc * (0.45f + 0.55f * ext), wag, false,
                   /*pin_top=*/true);
        }
    }
    {
        float nx = a.nose[0] - a.eyeMid[0], ny = a.nose[1] - a.eyeMid[1];
        float ox = (nx * rx + ny * ry) / ed, oy = (nx * upx + ny * upy) / ed;
        sprite("sprite_nose.png", ox, oy, 0.95f * sc, 0.f, false);
    }
    return n;
}

void face_filter_draw_doggy(ImDrawList* dl, const FaceObs& obs, float amount,
                            float t,
                            const std::function<ImVec2(float, float)>& to_screen) {
    FaceSpriteQuad quads[4];
    int n = face_filter_doggy_quads(obs, amount, t, quads, 4);
    for (int i = 0; i < n; ++i) {
        const FaceSpriteQuad& q = quads[i];
        dl->AddImageQuad((ImTextureID)(intptr_t)q.tex,
                         to_screen(q.p[0][0], q.p[0][1]),
                         to_screen(q.p[1][0], q.p[1][1]),
                         to_screen(q.p[2][0], q.p[2][1]),
                         to_screen(q.p[3][0], q.p[3][1]),
                         {q.u0, 0}, {q.u1, 0}, {q.u1, 1}, {q.u0, 1});
    }
}

// ── Face filter baked into a texture (shared by live mirror + take/export) ───

// Warp + doggy sprites for a tracked face, rendered into the slot's FBO.
// `obs` lives in the texture's pixel space (w×h). Returns tex unchanged when
// the filter produces nothing. `anim_t` drives the tongue wag.
// Assemble the platform-neutral render plan (see face_filters.h). This is
// the exact parameter assembly face_filter_apply_obs used inline — moved out
// so the iOS Metal runner shares it with the desktop GL path.
bool face_filter_build_plan_look(const BeautyLook& L, float amount,
                                 const FaceObs& obs, int w, int h,
                                 FaceRenderPlan& out) {
    out = FaceRenderPlan{};
    if (w <= 0 || h <= 0 || !obs.valid) return false;
    // Beauty look: GPU skin pass first (own buffer), then the shape warp
    // reads its output — smoothing samples the undistorted image.
    {
        FaceBeautyParams& bp = out.beauty;
        Anchors a = anchors_from(obs);
        // COORDINATE SPACE: obs landmarks live in the OBSERVATION's pixel
        // space (full-res raw for takes), but the texture being processed is
        // w×h (a half-res proxy in playback). The warp path normalizes to UV
        // so it never noticed; the beauty mask works in texture pixels — the
        // mismatch put every mask off-face on takes ("makeup visible while
        // recording, gone on playback, but the warp still applied").
        float sx_ = (obs.w > 0) ? (float)w / (float)obs.w : 1.f;
        float sy_ = (obs.h > 0) ? (float)h / (float)obs.h : 1.f;
        float sr_ = (sx_ + sy_) * 0.5f;   // proxy scaling is uniform in practice
        auto PX = [&](float v) { return v * sx_; };
        auto PY = [&](float v) { return v * sy_; };
        bp.smooth   = L.smooth   * amount;
        bp.brighten = L.brighten * amount;
        bp.warmth   = L.warmth   * amount;
        bp.eye_pop  = L.eye_pop  * amount;
        bp.blush    = L.blush    * amount;
        bp.lip_tint = L.lip      * amount;
        // Blush rides HIGH (douyin reference: under-eye/cheekbone, not
        // mid-cheek): pull the mesh cheek anchors 40% toward the eyes.
        bp.cheekL_x = PX(obs.pts[50][0]  + (a.eyeA[0] - obs.pts[50][0])  * L.blush_raise);
        bp.cheekL_y = PY(obs.pts[50][1]  + (a.eyeA[1] - obs.pts[50][1])  * L.blush_raise);
        bp.cheekR_x = PX(obs.pts[280][0] + (a.eyeB[0] - obs.pts[280][0]) * L.blush_raise);
        bp.cheekR_y = PY(obs.pts[280][1] + (a.eyeB[1] - obs.pts[280][1]) * L.blush_raise);
        bp.lip_grad = L.lip_grad;
        // Nose bridge: midway between the eye line and the nose tip.
        bp.nose_x = PX((a.eyeMid[0] + a.nose[0]) * 0.5f);
        bp.nose_y = PY((a.eyeMid[1] + a.nose[1]) * 0.5f);
        bp.nose_blush = L.nose_blush * amount;
        bp.freckles   = L.freckles   * amount;
        bp.jaw_shade  = L.jaw_shade  * amount;
        bp.chin_x = PX(a.chin[0]); bp.chin_y = PY(a.chin[1]);
        bp.chin_smooth = L.chin_smooth * amount;
        bp.lash       = L.lash       * amount;
        bp.liner      = L.liner      * amount;
        if (obs.has_blend) {
            bp.blink_l = obs.blend[FB_EYE_BLINK_L];
            bp.blink_r = obs.blend[FB_EYE_BLINK_R];
        }
        bp.lash_wing  = L.lash_wing;             // wing length is a shape, not a mix
        bp.eyeoutL_x = PX(obs.pts[33][0]);  bp.eyeoutL_y = PY(obs.pts[33][1]);
        bp.eyeoutR_x = PX(obs.pts[263][0]); bp.eyeoutR_y = PY(obs.pts[263][1]);
        // Upper-lid chains, outer→inner — the lash/liner ride these.
        static const int kLidL[7] = {33, 161, 160, 159, 158, 157, 133};
        static const int kLidR[7] = {263, 388, 387, 386, 385, 384, 362};
        for (int i = 0; i < 7; ++i) {
            bp.lidL[i][0] = PX(obs.pts[kLidL[i]][0]);
            bp.lidL[i][1] = PY(obs.pts[kLidL[i]][1]);
            bp.lidR[i][0] = PX(obs.pts[kLidR[i]][0]);
            bp.lidR[i][1] = PY(obs.pts[kLidR[i]][1]);
        }
        memcpy(bp.blush_col, L.blush_col, sizeof(bp.blush_col));
        memcpy(bp.lip_col,   L.lip_col,   sizeof(bp.lip_col));
        bp.eye_glow = L.eye_glow * amount;
        memcpy(bp.eye_glow_col, L.eye_glow_col, sizeof(bp.eye_glow_col));
        bp.skin_tint = L.skin_tint * amount;
        memcpy(bp.tint_col, L.tint_col, sizeof(bp.tint_col));
        bp.desat     = L.desat     * amount;
        bp.chrome    = L.chrome    * amount;
        bp.scanlines = L.scanlines * amount;
        bp.upx = a.up[0]; bp.upy = a.up[1];
        // Face ellipse: center midway eyes→chin, sized from chin↔forehead
        // (mesh 10) and the cheek span (234/454), with margin for the jawline.
        float fx0 = obs.pts[10][0],  fy0 = obs.pts[10][1];    // forehead top
        float cx  = (a.eyeMid[0] + a.chin[0]) * 0.5f;
        float cy  = (a.eyeMid[1] + a.chin[1]) * 0.5f;
        float dxs = obs.pts[454][0] - obs.pts[234][0];
        float dys = obs.pts[454][1] - obs.pts[234][1];
        float half_w = 0.5f * sqrtf(dxs*dxs + dys*dys) * 1.10f;
        float dxh = fx0 - a.chin[0], dyh = fy0 - a.chin[1];
        float half_h = 0.5f * sqrtf(dxh*dxh + dyh*dyh) * 1.12f;
        bp.face_cx = PX(cx); bp.face_cy = PY(cy);
        bp.face_rx = half_w * sr_; bp.face_ry = half_h * sr_;
        bp.eyeL_x = PX(a.eyeA[0]); bp.eyeL_y = PY(a.eyeA[1]);
        bp.eyeR_x = PX(a.eyeB[0]); bp.eyeR_y = PY(a.eyeB[1]);
        bp.eye_r  = a.eyeDist * 0.30f * sr_;
        bp.brow_r = a.eyeDist * 0.28f * sr_;
        bp.mouth_x = PX(a.mouth[0]); bp.mouth_y = PY(a.mouth[1]);
        bp.mouth_r = a.mouthW * 0.62f * sr_;
        // Lip ellipse (bitten-lip gradient center) + the outer-lip POLYGON —
        // the polygon is what makes the tint follow a smile.
        {
            float lx = obs.pts[0][0] - obs.pts[17][0];
            float ly = obs.pts[0][1] - obs.pts[17][1];
            bp.mouth_sw = a.mouthW * 0.58f * sr_;
            bp.mouth_sh = sqrtf(lx*lx + ly*ly) * 0.72f * sr_;
            static const int kLipRing[12] = {61, 40, 37, 0, 267, 270, 291,
                                             321, 314, 17, 84, 91};
            for (int i = 0; i < 12; ++i) {
                bp.lip_poly[i][0] = PX(obs.pts[kLipRing[i]][0]);
                bp.lip_poly[i][1] = PY(obs.pts[kLipRing[i]][1]);
            }
        }
        out.has_beauty = true;
        // UV-mapped makeup texture: pre-warp so shape changes deform the
        // pigment with the skin. Landmarks rescaled to texture space.
        if (L.makeup_tex) {
            out.makeup_tex     = L.makeup_tex;
            out.makeup_opacity = amount;
            out.makeup_adapt   = L.makeup_adapt;
            for (int i = 0; i < FT_NPTS; ++i) {
                out.mesh_pts[i][0] = PX(obs.pts[i][0]);
                out.mesh_pts[i][1] = PY(obs.pts[i][1]);
            }
        }
    }
    out.n_bumps = face_filter_bumps_look(L, amount, obs, out.bumps);
    out.valid = out.has_beauty || out.makeup_tex || out.n_bumps > 0;
    return out.valid;
}

// Enum-preset wrapper: beauty looks route through the parametric builder;
// pure-warp filters (BigEyes, Alien, ...) carry only their bump set.
bool face_filter_build_plan(int filter_id, float amount, const FaceObs& obs,
                            int w, int h, FaceRenderPlan& out) {
    out = FaceRenderPlan{};
    if (filter_id == 0 || w <= 0 || h <= 0 || !obs.valid) return false;
    BeautyLook L;
    if (beauty_look_for(filter_id, L))
        return face_filter_build_plan_look(L, amount, obs, w, h, out);
    out.n_bumps = face_filter_bumps(filter_id, amount, obs, out.bumps);
    out.valid = out.n_bumps > 0;
    return out.valid;
}

uintptr_t face_filter_apply_obs(int filter_id, float amount, const FaceObs& obs,
                                float anim_t, uintptr_t tex,
                                int slot, int w, int h) {
    if (filter_id == 0 || w <= 0 || h <= 0 || !obs.valid) return tex;
    FaceRenderPlan plan;
    face_filter_build_plan(filter_id, amount, obs, w, h, plan);
    if (plan.has_beauty)
        tex = face_beauty_apply(tex, slot, w, h, plan.beauty);
    if (plan.makeup_tex) {
        int mw = 0, mh = 0;
        GLuint mk = sprite_tex(plan.makeup_tex, mw, mh);
        if (mk)
            tex = face_makeup_apply(tex, slot, w, h, plan.mesh_pts, mk,
                                    plan.makeup_opacity, plan.makeup_adapt,
                                    plan.beauty.eyeL_x, plan.beauty.eyeL_y,
                                    plan.beauty.eyeR_x, plan.beauty.eyeR_y,
                                    plan.beauty.eye_r,
                                    plan.beauty.blink_l, plan.beauty.blink_r);
    }
    if (plan.n_bumps > 0)
        tex = face_warp_apply(tex, slot, w, h,
                              (const float*)plan.bumps, plan.n_bumps);

    if (filter_id == (int)FaceFilter::Doggy) {
        FaceSpriteQuad quads[4];
        int nq = face_filter_doggy_quads(obs, amount, anim_t, quads, 4);
        if (nq > 0)
            tex = face_sprites_apply(tex, slot, w, h, quads, nq);
    }
    return tex;
}

// Playback/export: face filter on a take's decoded frame, via the cached
// landmark pass (kicking the background build if missing).
uintptr_t face_filter_apply_take(const Clip& cl, double src_t,
                                 uintptr_t tex, int video_slot, int w, int h,
                                 bool sync_track) {
    if (cl.face_filter == 0 || cl.text.empty() || w <= 0 || h <= 0 ||
        !face_track_available())
        return tex;
    int rot_q = ((int)lroundf(cl.rotation / 90.f) % 4 + 4) % 4;
    face_cache_request(cl.text, rot_q);          // no-op once built
    FaceObs obs;
    bool have = face_cache_obs(cl.text, rot_q, src_t, obs);
    if (!have) {
        // The bake isn't current (building, or stale version). Track LIVE on
        // this frame instead of showing unfiltered/frozen makeup — the cache
        // is a fast-path, never the only path. Half-res download; the roll
        // ladder inside the tracker handles rotated sources.
        int hw2 = w / 2, hh2 = h / 2;
#if PMS_HAS_GL
        if (hw2 >= 64 && hh2 >= 64) {
            static GLuint s_dl_fbo = 0;
            static std::vector<uint8_t> rgb;
            rgb.resize((size_t)hw2 * hh2 * 3);
            GLint prev_read = 0, prev_draw = 0;
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
            if (!s_dl_fbo) glGenFramebuffers(1, &s_dl_fbo);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s_dl_fbo);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, (GLuint)tex, 0);
            // Direct sub-sampled read: full-res read + CPU decimate is slower
            // than reading every other pixel via a tiny scratch — keep it
            // simple and read full rows at stride 2.
            static std::vector<uint8_t> full;
            full.resize((size_t)w * h * 3);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, full.data());
            glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw);
            for (int y = 0; y < hh2; ++y) {
                const uint8_t* srow = &full[(size_t)(y * 2) * w * 3];
                uint8_t* drow = &rgb[(size_t)y * hw2 * 3];
                for (int x = 0; x < hw2; ++x) {
                    drow[x*3+0] = srow[x*2*3+0];
                    drow[x*3+1] = srow[x*2*3+1];
                    drow[x*3+2] = srow[x*2*3+2];
                }
            }
            if (sync_track) {
                have = face_track_run_sync(rgb.data(), hw2, hh2, obs);
            } else {
                face_track_submit(rgb.data(), hw2, hh2);
                have = face_track_latest(obs) && obs.valid;
            }
        }
#else
        (void)hw2; (void)hh2;   // no GL readback path on iOS (Metal = Phase 3)
#endif
    }
    if (!have) return tex;
    return face_filter_apply_obs(cl.face_filter, cl.face_filter_amt, obs,
                                 (float)src_t, tex,
                                 fx_face_clip_slot(video_slot), w, h);
}

// ── Picker previews: filters rendered onto the embedded base face photo ───────
#include "face_preview.h"   // generated by embed_face.sh (face_preview_rgb/_w/_h)

static GLuint  s_facep_base   = 0;      // base face uploaded as a GL texture
static FaceObs s_facep_obs;             // landmarks detected on the base face
static bool    s_facep_inited = false;  // base uploaded + detection attempted

static void facep_ensure() {
    if (s_facep_inited) return;
    s_facep_inited = true;
#if PMS_HAS_GL
    glGenTextures(1, &s_facep_base);
    glBindTexture(GL_TEXTURE_2D, s_facep_base);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, face_preview_w, face_preview_h, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, face_preview_rgb);
    glBindTexture(GL_TEXTURE_2D, 0);
#endif
    // One synchronous landmark pass on the base face — reused for every filter.
    if (face_track_available())
        face_track_run_sync(face_preview_rgb, face_preview_w, face_preview_h, s_facep_obs);
}

void face_filter_preview_dims(int* w, int* h) {
    if (w) *w = face_preview_w;
    if (h) *h = face_preview_h;
}

uintptr_t face_filter_preview_texture(int filter_id, float amount) {
    facep_ensure();
    if (filter_id <= 0 || !s_facep_obs.valid)
        return (uintptr_t)s_facep_base;     // None, or no face / no models → base
    return face_filter_apply_obs(filter_id, amount, s_facep_obs, 0.f,
                                 (uintptr_t)s_facep_base,
                                 fx_face_preview_slot(filter_id),
                                 face_preview_w, face_preview_h);
}
