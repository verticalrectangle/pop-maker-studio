#include "face_filters.h"
#include "face_cache.h"
#include "paths.h"
#include <GL/gl.h>
#include <GL/glext.h>
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
    "Chrome", "Neon", "Cyborg", "Hologram", "Rave",
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
struct BeautyLook {
    float smooth = 0, brighten = 0, warmth = 0, eye_pop = 0, blush = 0, lip = 0;
    float eyes = 0, cheek = 0, vline = 0, nose = 0, lips_plump = 0;
    float blush_col[3] = {1.f, 0.45f, 0.55f};   // default rosy
    float lip_col[3]   = {0.95f, 0.25f, 0.35f};
    // cyber
    float eye_glow = 0; float eye_glow_col[3] = {0.2f, 0.9f, 1.f};
    float skin_tint = 0; float tint_col[3] = {0.7f, 0.8f, 1.f};
    float desat = 0, chrome = 0, scanlines = 0;
};
static void set3(float* d, float r, float g, float b) { d[0]=r; d[1]=g; d[2]=b; }
static bool beauty_look_for(int filter_id, BeautyLook& L) {
    switch ((FaceFilter)filter_id) {
        case FaceFilter::Pretty:     // "Natural" — believable everyday clean-up
            L = {0.42f, 0.16f, 0.08f, 0.15f, 0.10f, 0.08f,  0.06f, 0.03f, 0.05f, 0.10f, 0.03f};
            return true;
        case FaceFilter::Glam:       // "Douyin" — the reference look
            L = {0.75f, 0.36f, 0.06f, 0.40f, 0.45f, 0.40f,  0.15f, 0.09f, 0.22f, 0.20f, 0.05f};
            return true;
        case FaceFilter::Porcelain:  // maximum skin, cool light, shape untouched
            L = {0.92f, 0.38f, 0.00f, 0.28f, 0.14f, 0.10f,  0.03f, 0.f,   0.f,   0.f,   0.f};
            return true;
        case FaceFilter::Sculpt:     // "Soft Glam" — makeup-forward, moderate shape
            L = {0.55f, 0.28f, 0.12f, 0.34f, 0.34f, 0.30f,  0.10f, 0.07f, 0.10f, 0.14f, 0.06f};
            return true;
        case FaceFilter::Honey:      // warm golden glow, soft everything
            L = {0.60f, 0.34f, 0.45f, 0.24f, 0.24f, 0.16f,  0.08f, 0.04f, 0.06f, 0.08f, 0.05f};
            return true;

        // ── Makeup looks ─────────────────────────────────────────────────
        case FaceFilter::Peach:      // sunny coral — summer skin
            L = {0.58f, 0.30f, 0.30f, 0.25f, 0.50f, 0.42f,  0.08f, 0.05f, 0.07f, 0.10f, 0.05f};
            set3(L.blush_col, 1.f, 0.55f, 0.38f);
            set3(L.lip_col,   1.f, 0.42f, 0.30f);
            return true;
        case FaceFilter::Cherry:     // K-drama cherry lips on pale skin
            L = {0.72f, 0.34f, 0.02f, 0.28f, 0.22f, 0.62f,  0.09f, 0.05f, 0.09f, 0.12f, 0.06f};
            set3(L.blush_col, 1.f, 0.55f, 0.62f);
            set3(L.lip_col,   0.85f, 0.08f, 0.16f);
            return true;
        case FaceFilter::Goth:       // pale, cool, plum-black lips
            L = {0.62f, 0.20f, 0.00f, 0.35f, 0.10f, 0.75f,  0.07f, 0.05f, 0.08f, 0.10f, 0.03f};
            set3(L.blush_col, 0.75f, 0.55f, 0.70f);
            set3(L.lip_col,   0.28f, 0.05f, 0.14f);
            L.desat = 0.28f;
            return true;
        case FaceFilter::Barbie:     // maximum pink everything
            L = {0.78f, 0.42f, 0.10f, 0.42f, 0.60f, 0.55f,  0.14f, 0.08f, 0.12f, 0.16f, 0.08f};
            set3(L.blush_col, 1.f, 0.45f, 0.75f);
            set3(L.lip_col,   1.f, 0.25f, 0.60f);
            return true;
        case FaceFilter::Bronze:     // golden-hour bronze glow
            L = {0.60f, 0.30f, 0.55f, 0.30f, 0.38f, 0.30f,  0.08f, 0.06f, 0.08f, 0.10f, 0.04f};
            set3(L.blush_col, 0.95f, 0.60f, 0.35f);
            set3(L.lip_col,   0.80f, 0.42f, 0.28f);
            return true;

        // ── Cyber looks ──────────────────────────────────────────────────
        case FaceFilter::Chrome:     // liquid-metal skin
            L = {0.85f, 0.00f, 0.f, 0.f, 0.f, 0.f,  0.06f, 0.04f, 0.06f, 0.f, 0.f};
            L.desat = 0.85f; L.chrome = 0.60f; L.skin_tint = 0.22f;
            set3(L.tint_col, 0.75f, 0.82f, 0.95f);
            L.eye_glow = 0.25f; set3(L.eye_glow_col, 0.8f, 0.9f, 1.f);
            return true;
        case FaceFilter::Neon:       // electric magenta/cyan club face
            L = {0.55f, 0.15f, 0.f, 0.f, 0.55f, 0.60f,  0.10f, 0.05f, 0.08f, 0.10f, 0.05f};
            set3(L.blush_col, 0.25f, 0.85f, 1.f);        // cyan cheek light
            set3(L.lip_col,   1.f, 0.10f, 0.80f);        // electric magenta
            L.eye_glow = 0.55f; set3(L.eye_glow_col, 1.f, 0.15f, 0.85f);
            L.skin_tint = 0.10f; set3(L.tint_col, 0.75f, 0.65f, 1.f);
            return true;
        case FaceFilter::Cyborg:     // cold steel + red optics
            L = {0.75f, 0.05f, 0.f, 0.f, 0.f, 0.f,  0.05f, 0.04f, 0.05f, 0.f, 0.f};
            L.desat = 0.7f; L.chrome = 0.45f; L.skin_tint = 0.35f;
            set3(L.tint_col, 0.62f, 0.72f, 0.85f);
            L.eye_glow = 0.75f; set3(L.eye_glow_col, 1.f, 0.12f, 0.10f);
            return true;
        case FaceFilter::Hologram:   // scanlined cyan projection
            L = {0.50f, 0.20f, 0.f, 0.f, 0.f, 0.f,  0.f, 0.f, 0.f, 0.f, 0.f};
            L.skin_tint = 0.55f; set3(L.tint_col, 0.35f, 0.95f, 1.f);
            L.desat = 0.5f; L.scanlines = 0.85f;
            L.eye_glow = 0.35f; set3(L.eye_glow_col, 0.4f, 1.f, 1.f);
            return true;
        case FaceFilter::Rave:       // UV blacklight — purple skin, acid accents
            L = {0.55f, 0.10f, 0.f, 0.f, 0.55f, 0.60f,  0.10f, 0.f, 0.f, 0.f, 0.06f};
            L.skin_tint = 0.40f; set3(L.tint_col, 0.55f, 0.35f, 1.f);
            set3(L.blush_col, 0.35f, 1.f, 0.45f);        // acid green cheeks
            set3(L.lip_col,   0.95f, 0.95f, 0.20f);      // yellow lip
            L.eye_glow = 0.45f; set3(L.eye_glow_col, 0.5f, 1.f, 0.3f);
            L.scanlines = 0.20f;
            return true;
        default: return false;
    }
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
            // face_beauty_apply). Same layered face-basis recipe, scaled per
            // look — Porcelain barely warps, Sculpt carves.
            BeautyLook L;
            if (!beauty_look_for(filter_id, L)) break;
            float d[2];
            if (L.eyes > 0.f) {
                bump(a.eyeA, eyeR, L.eyes * amt, 0, 0);
                bump(a.eyeB, eyeR, L.eyes * amt, 0, 0);
            }
            // Cheek slim: MID-jaw (mesh 132/361), gentle, small radius — the
            // old low-jaw squeeze at 0.55ed radius mushed into the chin.
            if (L.cheek > 0.f) {
                float cheekL[2] = {obs.pts[132][0], obs.pts[132][1]};
                float cheekR[2] = {obs.pts[361][0], obs.pts[361][1]};
                inward(cheekL, L.cheek * amt, d);
                bump(cheekL, a.eyeDist * 0.52f * ih, 0.f, d[0], d[1]);
                inward(cheekR, L.cheek * amt, d);
                bump(cheekR, a.eyeDist * 0.52f * ih, 0.f, d[0], d[1]);
            }
            // V-line: pull the lower jaw toward a point slightly BELOW the
            // chin tip — the jaw tapers into a point instead of the chin
            // being pushed up (which flattened it square).
            if (L.vline > 0.f) {
                float vx = a.chin[0] - a.up[0] * a.eyeDist * 0.25f;
                float vy = a.chin[1] - a.up[1] * a.eyeDist * 0.25f;
                auto toward_v = [&](const float* pnt, float k, float rad) {
                    float dx2 = (vx - pnt[0]) * k, dy2 = (vy - pnt[1]) * k;
                    bump(pnt, a.eyeDist * rad * ih, 0.f, dx2, dy2);
                };
                // Graded taper down the jawline: low jaw, then the chin
                // SIDES pull hardest — that's what turns a square chin into
                // a point instead of leaving flat corners.
                float chinSideL[2] = {obs.pts[148][0], obs.pts[148][1]};
                float chinSideR[2] = {obs.pts[377][0], obs.pts[377][1]};
                toward_v(a.jawL,   L.vline * amt,        0.46f);
                toward_v(a.jawR,   L.vline * amt,        0.46f);
                toward_v(chinSideL, L.vline * amt * 1.3f, 0.30f);
                toward_v(chinSideR, L.vline * amt * 1.3f, 0.30f);
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
            break;
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
uintptr_t face_filter_apply_obs(int filter_id, float amount, const FaceObs& obs,
                                float anim_t, uintptr_t tex,
                                int slot, int w, int h) {
    if (filter_id == 0 || w <= 0 || h <= 0 || !obs.valid) return tex;
    // Beauty looks: GPU skin pass first (own buffer), then the shape warp
    // reads its output — smoothing samples the undistorted image.
    BeautyLook L;
    if (beauty_look_for(filter_id, L)) {
        Anchors a = anchors_from(obs);
        FaceBeautyParams bp;
        bp.smooth   = L.smooth   * amount;
        bp.brighten = L.brighten * amount;
        bp.warmth   = L.warmth   * amount;
        bp.eye_pop  = L.eye_pop  * amount;
        bp.blush    = L.blush    * amount;
        bp.lip_tint = L.lip      * amount;
        bp.cheekL_x = obs.pts[50][0];  bp.cheekL_y = obs.pts[50][1];   // mesh cheeks
        bp.cheekR_x = obs.pts[280][0]; bp.cheekR_y = obs.pts[280][1];
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
        bp.face_cx = cx; bp.face_cy = cy;
        bp.face_rx = half_w; bp.face_ry = half_h;
        bp.eyeL_x = a.eyeA[0]; bp.eyeL_y = a.eyeA[1];
        bp.eyeR_x = a.eyeB[0]; bp.eyeR_y = a.eyeB[1];
        bp.eye_r  = a.eyeDist * 0.30f;
        bp.brow_r = a.eyeDist * 0.28f;
        bp.mouth_x = a.mouth[0]; bp.mouth_y = a.mouth[1];
        bp.mouth_r = a.mouthW * 0.62f;
        // Lip ellipse: width from the mouth corners, height from the OUTER
        // lip verticals (mesh 0 = upper outer mid, 17 = lower outer mid).
        {
            float lx = obs.pts[0][0] - obs.pts[17][0];
            float ly = obs.pts[0][1] - obs.pts[17][1];
            bp.mouth_sw = a.mouthW * 0.58f;
            bp.mouth_sh = sqrtf(lx*lx + ly*ly) * 0.72f;
        }
        tex = face_beauty_apply(tex, slot, w, h, bp);
    }
    FaceWarpBump bumps[MAX_FACE_BUMPS];
    int nb = face_filter_bumps(filter_id, amount, obs, bumps);
    if (nb > 0)
        tex = face_warp_apply(tex, slot, w, h, (const float*)bumps, nb);
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
                                 uintptr_t tex, int video_slot, int w, int h) {
    if (cl.face_filter == 0 || cl.text.empty() || w <= 0 || h <= 0 ||
        !face_track_available())
        return tex;
    int rot_q = ((int)lroundf(cl.rotation / 90.f) % 4 + 4) % 4;
    face_cache_request(cl.text, rot_q);          // no-op once built
    FaceObs obs;
    if (!face_cache_obs(cl.text, rot_q, src_t, obs)) return tex;
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
    glGenTextures(1, &s_facep_base);
    glBindTexture(GL_TEXTURE_2D, s_facep_base);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, face_preview_w, face_preview_h, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, face_preview_rgb);
    glBindTexture(GL_TEXTURE_2D, 0);
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
