#pragma once
// Face filters: landmark-driven warp recipes (beauty + silly) and the doggy
// overlay. Warps are "bumps" — local radial scale and/or shift fields the
// face_warp shader applies in one pass.
#include "face_track.h"
#include "fx_shader.h"
#include <imgui.h>
#include <functional>

struct FaceWarpBump {
    float cx, cy;        // center, frame UV (0–1)
    float radius;        // falloff radius, frame-height UV units
    float scale;         // +enlarge / −shrink around center
    float dx, dy;        // content shift in UV
};
static const int MAX_FACE_BUMPS = 20;

// Matches the Filters chips on the camera brick panel (order = id).
// Beauty looks are ids 1 (the original Pretty slot, now "Natural") and 7-10 —
// appended so serialized face_filter ints stay stable.
enum class FaceFilter {
    None = 0, Pretty, BigEyes, TinyFace, BigMouth, Alien, Doggy,
    Glam, Porcelain, Sculpt, Honey,
    // Makeup looks (colored blush/lip through the same beauty engine)
    Peach, Cherry, Goth, Barbie, Bronze,
    // Cyber looks (skin tint / chrome / scanlines / eye glow)
    Chrome, Neon, Cyborg, Hologram, Rave,
    Baddie,  // 2016 Instagram: matte nude lip, bronze contour, warm glow
    EGirl,   // nose blush + faux freckles + glossy pink lip
    // Lash + blush generation
    Doll, Coquette, Latte, CatEye, PeachyGlow, ColdBeauty, Sunset, Angel,
    // Lash + cyber mixes
    CyberDoll, NeonCat, Void, PixelPop,
    Belle    // huge doll eyes, max lashes, porcelain + hot pink
};
inline bool face_filter_is_beauty(int id) {
    return id == (int)FaceFilter::Pretty ||
           (id >= (int)FaceFilter::Glam && id <= (int)FaceFilter::Belle);
}
const char* face_filter_name(int id);
int         face_filter_count();

// One parametric beauty engine (skin + shape + makeup scalars). Preset looks
// (beauty_look_for) fill it from the FaceFilter enum; the iOS Makeup Studio
// composes it directly from live-stack params — no enum growth per look.
struct BeautyLook {
    float smooth = 0, brighten = 0, warmth = 0, eye_pop = 0, blush = 0, lip = 0;
    float eyes = 0, cheek = 0, vline = 0, nose = 0, lips_plump = 0;
    float blush_col[3] = {1.f, 0.45f, 0.55f};   // default rosy
    float lip_col[3]   = {0.95f, 0.25f, 0.35f};
    // cyber
    float lip_grad = 1.f;      // 1 = bitten-lip gradient, 0 = full matte
    float nose_blush = 0.f, freckles = 0.f;   // e-girl layer
    float chin_tuck = 0.f, jaw_shade = 0.f;    // (tuck retired: warped the neck)
    float chin_smooth = 0.f;                    // double-chin crease erase
    float lash = 0.f, liner = 0.f, lash_wing = 0.f;   // lashes, eyeliner, wing
    const char* makeup_tex = nullptr;  // UV-mapped makeup PNG (models/face/)
    float makeup_adapt = 1.f;          // lighting adaptation amount
    float blush_raise = 0.40f;  // 0 = mid-cheek (contour), 1 = under-eye
    float eye_glow = 0; float eye_glow_col[3] = {0.2f, 0.9f, 1.f};
    float skin_tint = 0; float tint_col[3] = {0.7f, 0.8f, 1.f};
    float desat = 0, chrome = 0, scanlines = 0;
};
bool beauty_look_for(int filter_id, BeautyLook& L);

// Build the warp set for a filter from a tracked face. Returns bump count.
int face_filter_bumps(int filter_id, float amount, const FaceObs& obs,
                      FaceWarpBump* out);
// Warp set for a parametric beauty look (the shape half of BeautyLook).
int face_filter_bumps_look(const BeautyLook& L, float amount, const FaceObs& obs,
                           FaceWarpBump* out);

// Platform-neutral render plan for a beauty/makeup look: everything the GPU
// passes need (assembled FaceBeautyParams, the UV-mesh landmark positions +
// makeup texture name, and the warp bumps), with NO graphics calls — shared
// by the desktop GL path (face_filter_apply_obs) and the iOS Metal runner
// (metal_render.mm face_fx branch). `w/h` = the target texture size; obs
// landmarks are rescaled from observation space exactly like apply_obs.
struct FaceRenderPlan {
    bool  valid = false;
    bool  has_beauty = false;
    FaceBeautyParams beauty;
    const char* makeup_tex = nullptr;   // models/face PNG, null = none
    float makeup_opacity = 0.f;
    float makeup_adapt   = 1.f;
    float mesh_pts[FT_NPTS][2];         // texture-space px (when makeup_tex)
    FaceWarpBump bumps[MAX_FACE_BUMPS];
    int   n_bumps = 0;
};
bool face_filter_build_plan(int filter_id, float amount, const FaceObs& obs,
                            int w, int h, FaceRenderPlan& out);
// Plan for a parametric look (Makeup Studio path — bypasses the enum).
bool face_filter_build_plan_look(const BeautyLook& L, float amount,
                                 const FaceObs& obs, int w, int h,
                                 FaceRenderPlan& out);

// Doggy overlay sprites (ears, nose, tongue) as frame-UV quads — feed them to
// face_sprites_apply (playback/export) or map through to_screen (mirror).
int face_filter_doggy_quads(const FaceObs& obs, float amount, float t,
                            FaceSpriteQuad* out, int max_out);

// Bake a face filter (warp + doggy sprites) into a texture from a tracked
// face. `obs` lives in the texture's w×h pixel space. The live mirror passes
// its live FaceObs; the take path passes a cached one — same pixels result.
uintptr_t face_filter_apply_obs(int filter_id, float amount, const FaceObs& obs,
                                float anim_t, uintptr_t tex,
                                int slot, int w, int h);

// Picker preview: render `filter_id` onto the embedded base face photo and
// return its texture for the camera brick's Filters tab. Landmarks on the base
// face are detected once (lazily, synchronously). Returns the unmodified base
// face for None (id 0) or when no face / no models are available.
uintptr_t face_filter_preview_texture(int filter_id, float amount);
void      face_filter_preview_dims(int* w, int* h);

// Playback/export: apply the clip's face filter to its decoded frame using
// the take's cached landmark pass (kicking the background build if missing).
// Returns tex unchanged until the cache is ready. Shared by the preview and
// export compositors so they cannot diverge.
// `sync_track`: when the landmark cache isn't current, true = run inference
// synchronously on this frame (export: deterministic), false = feed the async
// tracker and use its latest result (preview: real-time, ~1 frame behind).
uintptr_t face_filter_apply_take(const Clip& cl, double src_t,
                                 uintptr_t tex, int video_slot, int w, int h,
                                 bool sync_track = false);

// Doggy overlay: ears, nose, tongue drawn from landmarks. `to_screen` maps
// frame UV → screen px (the mirror's quad mapping, mirrored/rotated).
void face_filter_draw_doggy(ImDrawList* dl, const FaceObs& obs, float amount,
                            float t,
                            const std::function<ImVec2(float, float)>& to_screen);
