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
static const int MAX_FACE_BUMPS = 12;

// Matches the Filters chips on the camera brick panel (order = id).
enum class FaceFilter {
    None = 0, Pretty, BigEyes, TinyFace, BigMouth, Alien, Doggy
};
const char* face_filter_name(int id);
int         face_filter_count();

// Build the warp set for a filter from a tracked face. Returns bump count.
int face_filter_bumps(int filter_id, float amount, const FaceObs& obs,
                      FaceWarpBump* out);

// Doggy overlay sprites (ears, nose, tongue) as frame-UV quads — feed them to
// face_sprites_apply (playback/export) or map through to_screen (mirror).
int face_filter_doggy_quads(const FaceObs& obs, float amount, float t,
                            FaceSpriteQuad* out, int max_out);

// Playback/export: apply the clip's face filter to its decoded frame using
// the take's cached landmark pass (kicking the background build if missing).
// Returns tex unchanged until the cache is ready. Shared by the preview and
// export compositors so they cannot diverge.
uintptr_t face_filter_apply_take(const Clip& cl, double src_t,
                                 uintptr_t tex, int video_slot, int w, int h);

// Doggy overlay: ears, nose, tongue drawn from landmarks. `to_screen` maps
// frame UV → screen px (the mirror's quad mapping, mirrored/rotated).
void face_filter_draw_doggy(ImDrawList* dl, const FaceObs& obs, float amount,
                            float t,
                            const std::function<ImVec2(float, float)>& to_screen);
