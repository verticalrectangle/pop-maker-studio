#include "face_filters.h"
#include "paths.h"
#include <GL/gl.h>
#include <GL/glext.h>
#include "stb_image.h"
#include <cmath>
#include <string>
#include <map>

// Landmark groups (see face_track.h): eyes 33–42 / 87–96, brows 43–51 /
// 97–105, mouth 52–71, nose 72–86, contour 0–32 (chin ≈ 16).

static const char* k_names[] = {
    "None", "Pretty", "Big Eyes", "Tiny Face", "Big Mouth", "Alien", "Doggy",
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

static void mean_of(const FaceObs& o, int lo, int hi, float* out) {
    out[0] = out[1] = 0.f;
    for (int k = lo; k < hi; ++k) { out[0] += o.pts[k][0]; out[1] += o.pts[k][1]; }
    out[0] /= (hi - lo); out[1] /= (hi - lo);
}

static Anchors anchors_from(const FaceObs& o) {
    Anchors a;
    mean_of(o, 33, 43, a.eyeA);
    mean_of(o, 87, 97, a.eyeB);
    a.eyeMid[0] = (a.eyeA[0] + a.eyeB[0]) * 0.5f;
    a.eyeMid[1] = (a.eyeA[1] + a.eyeB[1]) * 0.5f;
    float dx = a.eyeB[0] - a.eyeA[0], dy = a.eyeB[1] - a.eyeA[1];
    a.eyeDist = sqrtf(dx*dx + dy*dy);
    mean_of(o, 72, 87, a.nose);
    // nose wings = extreme-x nose points
    a.noseL[0] = a.noseR[0] = o.pts[72][0];
    a.noseL[1] = a.noseR[1] = o.pts[72][1];
    for (int k = 72; k < 87; ++k) {
        if (o.pts[k][0] < a.noseL[0]) { a.noseL[0] = o.pts[k][0]; a.noseL[1] = o.pts[k][1]; }
        if (o.pts[k][0] > a.noseR[0]) { a.noseR[0] = o.pts[k][0]; a.noseR[1] = o.pts[k][1]; }
    }
    mean_of(o, 52, 72, a.mouth);
    float mw = 0.f;
    for (int k = 52; k < 72; ++k) {
        float d = fabsf(o.pts[k][0] - a.mouth[0]);
        if (d > mw) mw = d;
    }
    a.mouthW = mw * 2.f;
    a.chin[0] = o.pts[0][0];  a.chin[1] = o.pts[0][1];
    a.jawL[0] = o.pts[13][0]; a.jawL[1] = o.pts[13][1];
    a.jawR[0] = o.pts[29][0]; a.jawR[1] = o.pts[29][1];
    a.faceC[0] = (a.eyeMid[0] + a.chin[0]) * 0.5f;
    a.faceC[1] = (a.eyeMid[1] + a.chin[1]) * 0.5f;
    // Face basis — frame axes lie when the camera is rotated.
    float ux = a.eyeMid[0] - a.chin[0], uy = a.eyeMid[1] - a.chin[1];
    float ul = sqrtf(ux*ux + uy*uy); if (ul < 1.f) ul = 1.f;
    a.up[0] = ux / ul; a.up[1] = uy / ul;
    float rx = a.eyeB[0] - a.eyeA[0], ry = a.eyeB[1] - a.eyeA[1];
    float rl = sqrtf(rx*rx + ry*ry); if (rl < 1.f) rl = 1.f;
    a.right[0] = rx / rl; a.right[1] = ry / rl;
    return a;
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
        case FaceFilter::Pretty: {
            // CapCut-style beauty: subtle, layered, face-basis shifts.
            float d[2];
            bump(a.eyeA, eyeR, 0.11f * amt, 0, 0);              // eyes a touch bigger
            bump(a.eyeB, eyeR, 0.11f * amt, 0, 0);
            inward(a.jawL, 0.085f * amt, d);                    // jaw slim
            bump(a.jawL, a.eyeDist * 0.55f * ih, 0.f, d[0], d[1]);
            inward(a.jawR, 0.085f * amt, d);
            bump(a.jawR, a.eyeDist * 0.55f * ih, 0.f, d[0], d[1]);
            float k = a.eyeDist * 0.045f * amt;                 // shorter chin
            bump(a.chin, a.eyeDist * 0.45f * ih, 0.f, a.up[0]*k, a.up[1]*k);
            bump(a.noseL, a.eyeDist * 0.22f * ih, 0.f,          // nose slim
                 (a.nose[0] - a.noseL[0]) * 0.22f * amt,
                 (a.nose[1] - a.noseL[1]) * 0.22f * amt);
            bump(a.noseR, a.eyeDist * 0.22f * ih, 0.f,
                 (a.nose[0] - a.noseR[0]) * 0.22f * amt,
                 (a.nose[1] - a.noseR[1]) * 0.22f * amt);
            bump(a.mouth, a.mouthW * 0.55f * ih, 0.05f * amt, 0, 0); // soft lip plump
            break;
        }
        case FaceFilter::BigEyes:
            bump(a.eyeA, eyeR * 1.25f, 0.42f * amt, 0, 0);
            bump(a.eyeB, eyeR * 1.25f, 0.42f * amt, 0, 0);
            break;
        case FaceFilter::TinyFace: {
            float c[2] = {a.faceC[0], a.faceC[1]};
            bump(c, a.eyeDist * 1.6f * ih, -0.28f * amt, 0, 0);
            break;
        }
        case FaceFilter::BigMouth:
            bump(a.mouth, a.mouthW * 1.1f * ih, 0.55f * amt, 0, 0);
            break;
        case FaceFilter::Alien: {
            float d[2];
            float ke = a.eyeDist * 0.04f * amt;   // eyes drift up + huge
            bump(a.eyeA, eyeR * 1.5f, 0.5f * amt, a.up[0]*ke, a.up[1]*ke);
            bump(a.eyeB, eyeR * 1.5f, 0.5f * amt, a.up[0]*ke, a.up[1]*ke);
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

void face_filter_draw_doggy(ImDrawList* dl, const FaceObs& obs, float amount,
                            const std::function<ImVec2(float, float)>& to_screen) {
    if (!obs.valid) return;
    Anchors a = anchors_from(obs);
    const float iw = 1.f / obs.w, ih = 1.f / obs.h;
    const float ed = a.eyeDist;
    float upx = a.up[0], upy = a.up[1];
    float rx = a.right[0], ry = a.right[1];

    // Draw a sprite: center/size in face-basis units, tilt in degrees
    // (positive leans outward to the sprite's right). Corners go through
    // to_screen, so mirror flip + camera rotation come for free.
    auto sprite = [&](const char* name, float cx_fb, float cy_fb,
                      float width_fb, float tilt_deg, bool flip) {
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
        auto S = [&](float ox, float oy) {
            return to_screen((cx + axx * ox + ayx * oy) * iw,
                             (cy + axy * ox + ayy * oy) * ih);
        };
        ImVec2 tl = S(-hw,  hh), tr = S(hw,  hh);
        ImVec2 br = S( hw, -hh), bl = S(-hw, -hh);
        float u0 = flip ? 1.f : 0.f, u1 = flip ? 0.f : 1.f;
        dl->AddImageQuad((ImTextureID)(intptr_t)tex, tl, tr, br, bl,
                         {u0, 0}, {u1, 0}, {u1, 1}, {u0, 1});
    };

    float sc = 0.6f + 0.4f * amount;   // strength scales the costume
    sprite("sprite_ear.png", -1.35f, 1.55f, 1.25f * sc,  20.f, true);
    sprite("sprite_ear.png",  1.35f, 1.55f, 1.25f * sc, -20.f, false);
    // Tongue first, nose on top (Snapchat layering). Openness from the INNER
    // lip ring (64–71) — the outer ring includes lip thickness and fired on
    // a closed mouth.
    {
        float inner_c[2];
        mean_of(obs, 64, 72, inner_c);
        float open_span = 0.f;
        for (int k = 64; k < 72; ++k) {
            float vx = obs.pts[k][0] - inner_c[0];
            float vy = obs.pts[k][1] - inner_c[1];
            open_span = fmaxf(open_span, fabsf(vx * upx + vy * upy));
        }
        float openness = 2.f * open_span / (a.mouthW > 1.f ? a.mouthW : 1.f);
        if (openness > 0.22f) {
            float mx = a.mouth[0] - a.eyeMid[0], my = a.mouth[1] - a.eyeMid[1];
            float ox = (mx * rx + my * ry) / ed;
            float oy = (mx * upx + my * upy) / ed;
            sprite("sprite_tongue.png", ox, oy - 0.50f, 0.85f * sc, 0.f, false);
        }
    }
    {
        float nx = a.nose[0] - a.eyeMid[0], ny = a.nose[1] - a.eyeMid[1];
        float ox = (nx * rx + ny * ry) / ed, oy = (nx * upx + ny * upy) / ed;
        sprite("sprite_nose.png", ox, oy, 0.95f * sc, 0.f, false);
    }
}
