// metal_render.mm — Metal RenderSurface backend (Phase 3, iOS).
// On macOS (host test build) CoreVideo drags in CarbonCore's AIFF.h whose
// legacy `struct Marker` collides with the engine's Marker (app.h). Rename
// the Carbon one for the framework includes only — we never touch it.
#define Marker PMSCarbonAIFFMarker
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#undef Marker
#include "metal_render.h"
#include "app.h"          // AppState/Track/Clip — the scene compositor walks these
#include "face_filters.h" // BeautyLook + face_filter_build_plan_look (face_fx)
#include "face_track.h"   // live FaceObs for the record-mode makeup passes
#include "paths.h"        // app_models_dir() — makeup PNGs live in models/face
#include "stb_image.h"    // makeup texture load (impl compiled in video.cpp)
#include "generated/face_uv_mesh.h"   // canonical UVs + 898-tri topology
#include "json.hpp"
#include <vector>
#include <mutex>
#include <map>
#include <unordered_map>
#include <string>
#include <cstring>

// Generated FX registry tables (same ones ipc_server uses): FXType→shader-id
// names for the transpiled MSL runner, plus the param-setter used to validate
// which fx_<id>_<param> Clip fields belong to a given effect id.
#include "generated/fx_type_list.h"
static const char* k_gen_fx_names[] = {
#include "generated/fx_gen_names.h"
};
#include "generated/fx_clip_set_dispatch.h"

// ── Inline MSL ────────────────────────────────────────────────────────────────
// Two pipelines:
//   (1) the background — a full-screen aurora over near-black (the letterbox /
//       "nothing loaded" state), and
//   (2) a textured quad blit — the "over" operator every real layer flows
//       through: video frames, backgrounds, text, and FX outputs all become a
//       texture composited aspect-fit into the canvas. This is the compositor
//       keystone; Phase 4 (AVFoundation) feeds it decoded frames.
static NSString* const kSrc = @R"(
#include <metal_stdlib>
using namespace metal;

struct VOut { float4 pos [[position]]; float2 uv; };

// (1) background aurora — one oversized triangle
vertex VOut bg_v(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);   // (0,0)(2,0)(0,2)
    VOut o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv  = float2(p.x, 1.0 - p.y);
    return o;
}
struct BgUni { float t; float2 res; };
fragment float4 bg_f(VOut in [[stage_in]], constant BgUni& u [[buffer(0)]]) {
    float2 uv = in.uv;
    float3 base = mix(float3(0.02, 0.02, 0.03), float3(0.05, 0.04, 0.07), uv.y);
    float3 lav = float3(0.71, 0.66, 1.0);
    float b1 = 0.14 * exp(-pow((uv.y - 0.35 + 0.10 * sin(u.t * 0.6 + uv.x * 3.1)) * 4.0, 2.0));
    float b2 = 0.10 * exp(-pow((uv.y - 0.65 + 0.08 * sin(u.t * 0.4 - uv.x * 2.3)) * 5.0, 2.0));
    return float4(base + lav * (b1 + b2), 1.0);
}

// (2) textured quad — aspect-fit rect passed as half-extents in NDC
struct QuadUni { float2 he; };    // half-extents sx, sy in [0,1] ('half' is a reserved MSL type)
vertex VOut quad_v(uint vid [[vertex_id]], constant QuadUni& u [[buffer(0)]]) {
    float2 c[4] = { float2(0,0), float2(1,0), float2(0,1), float2(1,1) };  // tri-strip
    float2 uv = c[vid];
    VOut o;
    o.pos = float4((uv * 2.0 - 1.0) * u.he, 0.0, 1.0);
    o.uv  = float2(uv.x, 1.0 - uv.y);   // flip to top-left origin
    return o;
}
fragment float4 quad_f(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    return float4(tex.sample(s, in.uv).rgb, 1.0);
}

// (2b) matte composite — same aspect-fit quad, but the person matte (R8, from
// the platform segmenter) becomes the source alpha: the person stays from the
// content frame, the background shows whatever is already in the target (the
// engine's background render). Matte and content are the same camera frame, so
// one set of normalized UVs samples both. Blending: srcAlpha/1-srcAlpha.
fragment float4 matte_f(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]],
                        texture2d<float> matte [[texture(1)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float a = matte.sample(s, in.uv).r;
    return float4(tex.sample(s, in.uv).rgb, a);
}

// (3) fullscreen pass for the FX chain — emits v_uv [[user(locn0)]] to match the
// transpiled fragment ABI (fx_<name>(in, Params [[buffer(0)]], u_tex [[texture(0)]],
// u_texSmplr [[sampler(0)]])). One oversized triangle.
struct FSOut { float4 pos [[position]]; float2 v_uv [[user(locn0)]]; };
vertex FSOut fs_v(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    FSOut o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); o.v_uv = float2(p.x, 1.0 - p.y); return o;
}
// wet/dry blend — the automatic second pass that makes `amount` work per effect.
struct BlendUni { float amt; };
fragment float4 blend_f(FSOut in [[stage_in]], texture2d<float> pre [[texture(0)]],
                        texture2d<float> post [[texture(1)]], constant BlendUni& u [[buffer(0)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    return mix(pre.sample(s, in.v_uv), post.sample(s, in.v_uv), u.amt);
}

// (4) scene layer quad — the desktop scene_add_layer port. Pixel-space
// transform (center / half-extents / rotation), UV window (crop + flips baked
// into u0..v1 by the CPU), buffer-orientation quarter turns, opacity.
// Premultiplied-over blending set on the PSO. All-floats uniform (no float2)
// so the CPU byte layout matches trivially.
struct LayerUni {
    float cx;  float cy;  float hw;  float hh;   // pixel-space center + half extents
    float cs;  float sn;                          // cos/sin of rotation
    float cw;  float ch;                          // canvas size in pixels
    float u0;  float v0;  float u1;  float v1;    // UV window (top-left origin)
    float opacity;
    float quarters;                               // buffer rotation, quarter turns
    float pad0; float pad1;
};
vertex VOut layer_v(uint vid [[vertex_id]], constant LayerUni& u [[buffer(0)]]) {
    float2 c[4] = { float2(0,0), float2(1,0), float2(0,1), float2(1,1) };  // tri-strip
    float2 q = c[vid];                              // (0,0)=top-left in pixel space
    float2 corner = (q - 0.5) * 2.0 * float2(u.hw, u.hh);
    float2 p = float2(u.cx + corner.x * u.cs - corner.y * u.sn,
                      u.cy + corner.x * u.sn + corner.y * u.cs);
    VOut o;
    o.pos = float4(p.x / u.cw * 2.0 - 1.0, 1.0 - p.y / u.ch * 2.0, 0.0, 1.0);
    float2 uv = float2(u.u0 + q.x * (u.u1 - u.u0), u.v0 + q.y * (u.v1 - u.v0));
    int k = int(u.quarters) & 3;                    // upright→buffer orientation
    if      (k == 1) uv = float2(uv.y, 1.0 - uv.x);
    else if (k == 2) uv = float2(1.0 - uv.x, 1.0 - uv.y);
    else if (k == 3) uv = float2(1.0 - uv.y, uv.x);
    o.uv = uv;
    return o;
}
// Premultiplied source (CoreGraphics text rasters are premultiplied; opaque
// video has a=1 so rgb*opacity / a=opacity — the correct over in both cases).
fragment float4 layer_f(VOut in [[stage_in]], constant LayerUni& u [[buffer(0)]],
                        texture2d<float> tex [[texture(0)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    return tex.sample(s, in.uv) * u.opacity;
}
// Layer + person-matte cutout (RemoveBackground body brick): the matte is the
// same camera frame as the content, sampled with the same UVs.
fragment float4 layer_matte_f(VOut in [[stage_in]], constant LayerUni& u [[buffer(0)]],
                              texture2d<float> tex [[texture(0)]],
                              texture2d<float> matte [[texture(1)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    return tex.sample(s, in.uv) * (u.opacity * matte.sample(s, in.uv).r);
}
// Solid full-canvas quad (DipWhite transition veil) — premultiplied color.
struct SolidUni { float r; float g; float b; float a; };
fragment float4 solid_f(FSOut in [[stage_in]], constant SolidUni& u [[buffer(0)]]) {
    return float4(u.r * u.a, u.g * u.a, u.b * u.a, u.a);
}
)";

// ── Body FX MSL ──────────────────────────────────────────────────────────────
// Hand-written matte-consuming passes (live-FX entries with fx_type "body_fx").
// Ports of the desktop GLSL bodies in body_fx.cpp (k_frag_NeonOutline /
// k_frag_DepthBlur / k_frag_GlitchBody) onto the FX-chain fragment ABI: the
// shared fullscreen vertex (fs_v) + content at texture(0), plus the person
// matte at texture(1). Compiled at runtime like the transpiled registry.
// Param names/defaults mirror body_fx.cpp's BodyFXInfo table (see
// body_param()); wet/dry `amount` rides the chain's automatic blend pass.
static NSString* const kBodySrc = @R"(
#include <metal_stdlib>
using namespace metal;

struct FSOut { float4 pos [[position]]; float2 v_uv [[user(locn0)]]; };
// Plain floats (no float2) — byte layout matches the CPU-side BodyUni exactly.
struct BodyUni { float p0; float p1; float p2; float p3; float time; float tw; float th; };

float body_hash(float2 p) { return fract(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
float3 body_hue2rgb(float h) {
    h = fract(h);
    return clamp(abs(fract(h + float3(0.0, 2.0/3.0, 1.0/3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
}

// "Neon Outline" — glow along the matte edge (screen-space gradient of the
// matte, hue-cycling color, additive over the body). p0 = Glow Width, p1 = Hue.
fragment float4 body_neon_outline(FSOut in [[stage_in]], constant BodyUni& u [[buffer(0)]],
                                  texture2d<float> src [[texture(0)]],
                                  texture2d<float> matte [[texture(1)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float4 orig = src.sample(s, in.v_uv);
    float  m    = matte.sample(s, in.v_uv).r;
    float2 px   = float2(1.0 / u.tw, 1.0 / u.th);
    float  w    = u.p0 + 1.0;
    float  e    = 0.0;
    for (float dx = -w; dx <= w; dx += 1.0)
        for (float dy = -w; dy <= w; dy += 1.0)
            e = max(e, abs(m - matte.sample(s, in.v_uv + float2(dx, dy) * px).r));
    float  edge = smoothstep(0.05, 0.5, e);
    float3 glow = body_hue2rgb(fract(u.p1 + u.time * 0.2));
    float3 body = orig.rgb + glow * edge * 2.0;
    return float4(mix(orig.rgb, body, m), orig.a);   // body-only, bg untouched (desktop parity)
}

// "Depth Blur" — sharp person (matte≈1), box-blurred background (matte≈0).
// p0 = Blur Radius (px). 9x9 single-pass box (desktop parity).
fragment float4 body_depth_blur(FSOut in [[stage_in]], constant BodyUni& u [[buffer(0)]],
                                texture2d<float> src [[texture(0)]],
                                texture2d<float> matte [[texture(1)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float4 orig = src.sample(s, in.v_uv);
    float  m    = matte.sample(s, in.v_uv).r;
    float  rx   = u.p0 / u.tw, ry = u.p0 / u.th;
    float4 blurred = float4(0.0);
    for (int i = -4; i <= 4; ++i)
        for (int j = -4; j <= 4; ++j)
            blurred += src.sample(s, clamp(in.v_uv + float2(float(i) * rx, float(j) * ry),
                                           0.0, 1.0));
    blurred /= 81.0;
    return float4(mix(blurred.rgb, orig.rgb, m), orig.a);
}

// "Glitch Body" — time-animated horizontal slice displacement + RGB split
// inside the person only (matte≈1). p0 = Chroma (px), p1 = Jitter.
fragment float4 body_glitch(FSOut in [[stage_in]], constant BodyUni& u [[buffer(0)]],
                            texture2d<float> src [[texture(0)]],
                            texture2d<float> matte [[texture(1)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float4 orig  = src.sample(s, in.v_uv);
    float  m     = matte.sample(s, in.v_uv).r;
    float  chroma = u.p0 / u.tw;
    float  y_id  = floor(in.v_uv.y * u.th);
    float  rnd   = body_hash(float2(y_id, floor(u.time * 12.0)));
    float  jshift = (rnd > 1.0 - u.p1 * 0.4)
                  ? (body_hash(float2(y_id, u.time * 8.0)) - 0.5) * u.p1 * 0.12 : 0.0;
    float r = src.sample(s, clamp(float2(in.v_uv.x + jshift + chroma, in.v_uv.y), 0.0, 1.0)).r;
    float g = src.sample(s, clamp(float2(in.v_uv.x + jshift,          in.v_uv.y), 0.0, 1.0)).g;
    float b = src.sample(s, clamp(float2(in.v_uv.x + jshift - chroma, in.v_uv.y), 0.0, 1.0)).b;
    return float4(mix(orig.rgb, float3(r, g, b), m), orig.a);
}
)";

// ── Chroma feedback FX MSL ───────────────────────────────────────────────────
// Hand-written ports of the desktop chroma feedback family (k_chroma_melt_frag /
// k_chroma_echo_frag / k_chroma_frame_frag in fx_shader.cpp — keep in sync).
// These are stateful: melt/echo read the previous frame's own output from a
// persistent per-chain feedback texture; frame reads a per-chain ring of past
// snapshots (2D array, 8 layers). The state lives CPU-side in g_chroma_fb,
// keyed by (chain, stack index, fx_type) — see the chroma branch of
// run_fx_stack. Uniform layout is plain floats (CPU mirror: ChromaUniCPU).
static NSString* const kChromaSrc = @R"(
#include <metal_stdlib>
using namespace metal;

struct FSOut { float4 pos [[position]]; float2 v_uv [[user(locn0)]]; };
struct ChromaUni { float key_r; float key_g; float key_b; float threshold;
                   float persist; float head; float ntaps; float falloff;
                   float tw; float th; float matte_key; };

// Foreground term: person matte when matte_key is set (selfie mode — the
// subject stays crisp, the BACKGROUND trails), else colour-distance from the
// key (classic green-screen mode).
static float chroma_fg(float2 uv, float3 kc_avg, constant ChromaUni& u,
                       texture2d<float> matte, bool soft) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    if (u.matte_key > 0.5) return matte.sample(s, uv).r;
    float3 key   = float3(u.key_r, u.key_g, u.key_b);
    float  lum_k = dot(key,    float3(0.299, 0.587, 0.114));
    float  lum_p = dot(kc_avg, float3(0.299, 0.587, 0.114));
    float  dist  = length((kc_avg - lum_p) - (key - lum_k));
    return soft ? smoothstep(u.threshold, u.threshold + 0.12, dist)
                : step(u.threshold, dist);
}

// Chroma melt — keyed background keeps the prior frame, drifting sideways →
// smear trails. Soft matte on a box-averaged colour (codec-noise tolerant).
fragment float4 chroma_melt_f(FSOut in [[stage_in]], constant ChromaUni& u [[buffer(0)]],
                              texture2d<float> src [[texture(0)]],
                              texture2d<float> fb  [[texture(1)]],
                              texture2d<float> matte [[texture(2)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float3 cur = src.sample(s, in.v_uv).rgb;
    float2 tx  = float2(1.0 / u.tw, 1.0 / u.th);
    float3 kc = (cur
              + src.sample(s, in.v_uv + float2( 2.0*tx.x, 0.0)).rgb
              + src.sample(s, in.v_uv + float2(-2.0*tx.x, 0.0)).rgb
              + src.sample(s, in.v_uv + float2(0.0,  2.0*tx.y)).rgb
              + src.sample(s, in.v_uv + float2(0.0, -2.0*tx.y)).rgb) * 0.2;
    float  fg    = chroma_fg(in.v_uv, kc, u, matte, true);
    float3 prev  = fb.sample(s, in.v_uv + float2(1.5*tx.x, 0.0)).rgb;
    float3 trail = mix(cur, prev, u.persist);
    return float4(mix(trail, cur, fg), 1.0);
}

// Chroma echo — crisp sibling: HARD matte + no drift, keyed pixels stack the
// subject's past frames as distinct fading ghosts.
fragment float4 chroma_echo_f(FSOut in [[stage_in]], constant ChromaUni& u [[buffer(0)]],
                              texture2d<float> src [[texture(0)]],
                              texture2d<float> fb  [[texture(1)]],
                              texture2d<float> matte [[texture(2)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float3 cur = src.sample(s, in.v_uv).rgb;
    float2 tx  = float2(1.0 / u.tw, 1.0 / u.th);
    float3 kc = (cur
              + src.sample(s, in.v_uv + float2( 2.0*tx.x, 0.0)).rgb
              + src.sample(s, in.v_uv + float2(-2.0*tx.x, 0.0)).rgb
              + src.sample(s, in.v_uv + float2(0.0,  2.0*tx.y)).rgb
              + src.sample(s, in.v_uv + float2(0.0, -2.0*tx.y)).rgb) * 0.2;
    float  fg    = chroma_fg(in.v_uv, kc, u, matte, false);
    float3 prev  = fb.sample(s, in.v_uv).rgb;
    float3 echo  = mix(cur, prev, u.persist);
    return float4(mix(echo, cur, fg), 1.0);
}

// Chroma frame — discrete multi-tap delay from the snapshot ring. head/ntaps
// arrive as floats (plain-float uniform); RING must match kChromaRingLayers.
// In matte mode the ring holds PRE-MASKED snapshots (alpha = subject matte,
// written by chroma_snapshot_f) so each tap composites only the past subject.
fragment float4 chroma_frame_f(FSOut in [[stage_in]], constant ChromaUni& u [[buffer(0)]],
                               texture2d<float>       src  [[texture(0)]],
                               texture2d_array<float> ring [[texture(1)]],
                               texture2d<float>       matte [[texture(2)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    const int RING = 8;
    float3 key   = float3(u.key_r, u.key_g, u.key_b);
    float  lum_k = dot(key, float3(0.299, 0.587, 0.114));
    float3 live  = src.sample(s, in.v_uv).rgb;
    float  live_fg = chroma_fg(in.v_uv, live, u, matte, false);
    int head  = int(u.head);
    int ntaps = int(u.ntaps);
    float3 echo = live;
    for (int i = RING - 1; i >= 0; --i) {
        if (i >= ntaps) continue;
        int layer = ((head - i) % RING + RING) % RING;
        float4 tap = ring.sample(s, in.v_uv, layer);
        float  m   = (u.matte_key > 0.5)
                   ? tap.a
                   : step(u.threshold,
                          length((tap.rgb - dot(tap.rgb, float3(0.299,0.587,0.114))) - (key - lum_k)));
        float  a   = m * pow(u.falloff, float(i));
        echo = mix(echo, tap.rgb, a);
    }
    return float4(mix(echo, live, live_fg), 1.0);
}

// Matte-masked ring snapshot: rgb = frame, a = subject matte (matte mode only;
// colour mode snapshots via a plain blit, alpha unused).
fragment float4 chroma_snapshot_f(FSOut in [[stage_in]],
                                  texture2d<float> src   [[texture(0)]],
                                  texture2d<float> matte [[texture(1)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    return float4(src.sample(s, in.v_uv).rgb, matte.sample(s, in.v_uv).r);
}
)";

// ── Face FX MSL (makeup looks) ───────────────────────────────────────────────
// Hand-written ports of the desktop face passes in fx_shader.cpp
// (k_face_beauty_fs / k_face_mk_vs+fs / k_face_warp_fs — keep in sync).
// Geometry arrives fully assembled from face_filter_build_plan_look
// (face_filters.cpp), so these shaders are as dumb as their GL twins.
// All uniforms are plain float arrays — byte layout matches the CPU mirrors.
static NSString* const kFaceSrc = @R"(
#include <metal_stdlib>
using namespace metal;

struct FSOut { float4 pos [[position]]; float2 v_uv [[user(locn0)]]; };

// ── warp: up to 12 local bumps (radial scale + shift, gaussian falloff) ─────
struct FaceWarpUni { float n; float aspect; float pad0; float pad1;
                     float ba[48]; float bb[48]; };
fragment float4 face_warp_f(FSOut in [[stage_in]], constant FaceWarpUni& u [[buffer(0)]],
                            texture2d<float> src [[texture(0)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 uv = in.v_uv;
    int n = int(u.n);
    for (int i = 0; i < 12; ++i) {
        if (i >= n) break;
        float2 c = float2(u.ba[i*4+0], u.ba[i*4+1]);
        float  r = max(u.ba[i*4+2], 1e-4);
        float2 d = in.v_uv - c;
        d.x *= u.aspect;
        float g = exp(-dot(d, d) / (r * r * 0.45));
        uv -= (float2(u.bb[i*4+0], u.bb[i*4+1]) + (in.v_uv - c) * u.ba[i*4+3]) * g;
    }
    return src.sample(s, clamp(uv, float2(0.001), float2(0.999)));
}

// ── beauty: skin mask + smoothing + brighten/warmth + procedural makeup ─────
struct FaceBeautyUni {
    float dim[2]; float up[2];
    float face[4]; float eyes[4]; float feat[4]; float amt[4]; float makeup[4];
    float cheeks[4]; float blushc[4]; float lipc[4]; float eyeglow[4];
    float cyber[4]; float tintc[4]; float mouthax[4];
    float lippoly[24];
    float nose[4]; float lash[4]; float chin[4]; float eyeout[4];
    float lidL[14]; float lidR[14];
    float brow_r; float pad0; float blink[2];
};
static float f_lum(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
static float f_seg(float2 p, float2 a, float2 b) {
    float2 e = b - a;
    float t = clamp(dot(p - a, e) / max(dot(e, e), 1e-4), 0.0, 1.0);
    return length(p - (a + e * t));
}
fragment float4 face_beauty_f(FSOut in [[stage_in]], constant FaceBeautyUni& u [[buffer(0)]],
                              texture2d<float> src [[texture(0)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 dim = float2(u.dim[0], u.dim[1]);
    float2 p   = in.v_uv * dim;
    float4 c0  = src.sample(s, in.v_uv);
    float3 col = c0.rgb;
    float2 upv = float2(u.up[0], u.up[1]);
    float2 rightv = float2(-upv.y, upv.x);
    float2 d = p - float2(u.face[0], u.face[1]);
    float a = dot(d, rightv) / max(u.face[2], 1.0);
    float b = dot(d, upv)    / max(u.face[3], 1.0);
    float mask = 1.0 - smoothstep(0.72, 1.05, length(float2(a, b)));
    mask *= 1.0 - smoothstep(0.84, 1.0, -b);
    float er = max(u.feat[0], 1.0);
    float2 eyeL = float2(u.eyes[0], u.eyes[1]), eyeR = float2(u.eyes[2], u.eyes[3]);
    float holeL = 1.0 - smoothstep(er * 0.7, er * 1.25, distance(p, eyeL));
    float holeR = 1.0 - smoothstep(er * 0.7, er * 1.25, distance(p, eyeR));
    float2 browL = eyeL + upv * er * 1.2, browR = eyeR + upv * er * 1.2;
    float br = max(u.brow_r, 1.0);
    float holeBL = 1.0 - smoothstep(br * 0.6, br * 1.1, distance(p, browL));
    float holeBR = 1.0 - smoothstep(br * 0.6, br * 1.1, distance(p, browR));
    float mr = max(u.feat[3], 1.0);
    float2 mouth = float2(u.feat[1], u.feat[2]);
    float holeM = 1.0 - smoothstep(mr * 0.75, mr * 1.3, distance(p, mouth));
    mask *= (1.0 - holeL) * (1.0 - holeR) * (1.0 - holeM)
          * (1.0 - holeBL * 0.85) * (1.0 - holeBR * 0.85);

    if (u.amt[0] > 0.001 && mask > 0.003) {           // bilateral-lite smooth
        float rad = max(u.face[2], u.face[3]) * 0.045;
        const float2 taps[12] = {
            float2(-0.326,-0.406), float2(-0.840,-0.074), float2(-0.696, 0.457),
            float2(-0.203, 0.621), float2( 0.962,-0.195), float2( 0.473,-0.480),
            float2( 0.519, 0.767), float2( 0.185,-0.893), float2( 0.507, 0.064),
            float2( 0.896, 0.412), float2(-0.322,-0.933), float2(-0.792,-0.598)};
        float l0 = f_lum(col);
        float3 acc = col; float wsum = 1.0;
        for (int i = 0; i < 12; ++i) {
            float3 c = src.sample(s, in.v_uv + taps[i] * rad / dim).rgb;
            float wl = exp(-pow((f_lum(c) - l0) * 9.0, 2.0));
            acc += c * wl; wsum += wl;
        }
        col = mix(col, acc / wsum, u.amt[0] * mask);
    }
    if (u.amt[1] > 0.001) {                            // soft-light brighten
        float3 lift = col + (float3(1.0) - col) * col * 0.9;
        col = mix(col, lift, u.amt[1] * mask);
    }
    if (u.amt[2] > 0.001)
        col += float3(0.035, 0.012, -0.02) * (u.amt[2] * mask);
    if (u.amt[3] > 0.001) {                            // eye pop
        float eL = 1.0 - smoothstep(er * 0.35, er * 0.95, distance(p, eyeL));
        float eR = 1.0 - smoothstep(er * 0.35, er * 0.95, distance(p, eyeR));
        col *= 1.0 + u.amt[3] * 0.30 * max(eL, eR);
    }
    if (u.chin[2] > 0.001) {                           // double-chin crease erase
        float below = smoothstep(0.70, 0.95, -b) * (1.0 - smoothstep(1.30, 1.55, -b));
        float reg = below * (1.0 - smoothstep(er * 1.3, er * 2.3,
                                              distance(p, float2(u.chin[0], u.chin[1]))));
        if (reg > 0.003) {
            float2 px2 = 1.0 / dim;
            float rad = er * 0.55;
            float3 acc = float3(0.0);
            for (int i = 0; i < 12; ++i) {
                float ang = float(i) * 0.5236;
                float rr2 = (0.35 + 0.65 * fract(float(i) * 0.618)) * rad;
                acc += src.sample(s, in.v_uv + float2(cos(ang), sin(ang)) * rr2 * px2).rgb;
            }
            col = mix(col, acc * (1.0 / 12.0), reg * u.chin[2] * 0.8);
        }
    }
    if (u.makeup[3] > 0.001) {                         // under-jaw contour shadow
        float elen = length(float2(a, b));
        float band = smoothstep(0.94, 1.03, elen) * (1.0 - smoothstep(1.03, 1.18, elen));
        float below = smoothstep(0.45, 0.85, -b);
        float2 chin_px = float2(u.face[0], u.face[1]) - upv * u.face[3] * 0.92;
        float near_chin = 1.0 - smoothstep(u.face[2] * 0.55, u.face[2] * 0.95,
                                           distance(p, chin_px));
        float sh = u.makeup[3] * band * below * near_chin;
        col *= 1.0 - sh * float3(0.20, 0.22, 0.24);
    }
    // Skin-chroma gate (YCbCr window) — keeps makeup off mics/hands/props.
    float m_cb = 0.5 - 0.168736 * col.r - 0.331264 * col.g + 0.5 * col.b;
    float m_cr = 0.5 + 0.5 * col.r - 0.418688 * col.g - 0.081312 * col.b;
    float skin_chroma = smoothstep(0.27, 0.31, m_cb) * (1.0 - smoothstep(0.45, 0.50, m_cb))
                      * smoothstep(0.50, 0.54, m_cr) * (1.0 - smoothstep(0.66, 0.70, m_cr));
    if (u.makeup[0] > 0.001) {                         // blush discs
        float br2 = max(u.face[2], u.face[3]) * 0.30;
        float cL = 1.0 - smoothstep(br2 * 0.3, br2, distance(p, float2(u.cheeks[0], u.cheeks[1])));
        float cR = 1.0 - smoothstep(br2 * 0.3, br2, distance(p, float2(u.cheeks[2], u.cheeks[3])));
        float bm = max(cL, cR) * u.makeup[0] * mask * skin_chroma;
        col = mix(col, col * (0.75 + 0.5 * float3(u.blushc[0], u.blushc[1], u.blushc[2])), bm * 0.55);
    }
    if (u.nose[2] + u.nose[3] > 0.001) {               // e-girl nose blush + freckles
        float2 nb = float2(dot(p - float2(u.nose[0], u.nose[1]), rightv),
                           dot(p - float2(u.nose[0], u.nose[1]), upv)) / max(er, 1.0);
        float band = 1.0 - smoothstep(0.55, 1.0, length(nb * float2(0.50, 1.55)));
        if (u.nose[2] > 0.001) {
            float m_cb2 = 0.5 - 0.168736 * col.r - 0.331264 * col.g + 0.5 * col.b;
            float m_cr2 = 0.5 + 0.5 * col.r - 0.418688 * col.g - 0.081312 * col.b;
            float sk2 = smoothstep(0.27, 0.31, m_cb2) * (1.0 - smoothstep(0.45, 0.50, m_cb2))
                      * smoothstep(0.50, 0.54, m_cr2) * (1.0 - smoothstep(0.66, 0.70, m_cr2));
            col = mix(col, col * (0.75 + 0.5 * float3(u.blushc[0], u.blushc[1], u.blushc[2])),
                      band * u.nose[2] * mask * sk2 * 0.50);
        }
        if (u.nose[3] > 0.001) {
            float2 cell = floor(nb * 6.0);
            float2 fr2  = fract(nb * 6.0);
            float h1 = fract(sin(dot(cell, float2(127.1, 311.7))) * 43758.5453);
            float2 jit = fract(sin(float2(dot(cell, float2(269.5, 183.3)),
                                          dot(cell, float2(419.2, 371.9)))) * 43758.5453);
            float fd = length(fr2 - 0.30 - jit * 0.40);
            float rsz  = 0.11 + 0.10 * fract(h1 * 7.31);
            float dotm = (1.0 - smoothstep(rsz * 0.4, rsz + 0.06, fd)) * step(0.45, h1);
            float op   = 0.35 + 0.30 * fract(h1 * 13.7);
            col = mix(col, col * float3(0.74, 0.55, 0.45),
                      dotm * band * u.nose[3] * mask * op);
        }
    }
    if (u.makeup[1] > 0.001) {                         // lip tint (polygon mask)
        int crossings = 0;
        float dmin = 1e9;
        for (int i = 0; i < 12; ++i) {
            float2 a2 = float2(u.lippoly[i*2], u.lippoly[i*2+1]);
            int j = (i == 11) ? 0 : i + 1;
            float2 b2 = float2(u.lippoly[j*2], u.lippoly[j*2+1]);
            if ((a2.y > p.y) != (b2.y > p.y)) {
                float xin = a2.x + (p.y - a2.y) * (b2.x - a2.x) / (b2.y - a2.y);
                if (p.x < xin) crossings++;
            }
            dmin = min(dmin, f_seg(p, a2, b2));
        }
        float inside  = float((crossings & 1) == 1);
        float feather = max(u.mouthax[1] * 0.30, 1.5);
        float lm2 = inside * smoothstep(0.0, feather * 0.8, dmin);
        float2 md = p - mouth;
        float la = dot(md, rightv) / max(u.mouthax[0], 1.0);
        float lb = dot(md, upv)    / max(u.mouthax[1], 1.0);
        float grad = 1.0 - 0.55 * u.makeup[2] * smoothstep(0.25, 1.0, length(float2(la, lb)));
        float deep  = inside * smoothstep(feather, feather * 2.2, dmin);
        float lippy = max(smoothstep(0.03, 0.12, col.r - col.g), deep);
        float t = u.makeup[1] * lm2 * grad * lippy;
        float3 lip_target = float3(u.lipc[0], u.lipc[1], u.lipc[2]) * (0.30 + 1.05 * f_lum(col));
        col = mix(col, clamp(lip_target, 0.0, 1.0), t * 0.85);
    }
    if (u.cyber[0] + u.cyber[1] + u.cyber[2] + u.cyber[3] > 0.001) {   // cyber layer
        float lm3 = f_lum(col);
        float3 tintc = float3(u.tintc[0], u.tintc[1], u.tintc[2]);
        col = mix(col, float3(lm3), u.cyber[1] * mask);
        col = mix(col, tintc * (0.15 + 1.1 * lm3), u.cyber[0] * mask);
        if (u.cyber[2] > 0.001) {
            float3 crm = clamp((col - 0.5) * 2.2 + 0.5, 0.0, 1.0);
            crm += smoothstep(0.75, 0.98, lm3) * 0.25;
            col = mix(col, crm, u.cyber[2] * mask);
        }
        if (u.cyber[3] > 0.001) {
            float sl = 0.5 + 0.5 * sin(p.y * 1.4);
            col *= 1.0 - u.cyber[3] * mask * 0.35 * smoothstep(0.55, 0.95, sl);
            col += tintc * u.cyber[3] * mask * 0.06 * (1.0 - sl);
        }
    }
    if (u.lash[0] + u.lash[2] > 0.001) {               // lashes + liner + wing
        float3 ink = float3(0.04, 0.03, 0.04);
        for (int side = 0; side < 2; ++side) {
            float bfade = 1.0 - 0.9 * smoothstep(0.25, 0.55,
                                    side == 0 ? u.blink[0] : u.blink[1]);
            float2 outc = side == 0 ? float2(u.eyeout[0], u.eyeout[1])
                                    : float2(u.eyeout[2], u.eyeout[3]);
            float dmin2 = 1e9; float tbest = 0.0;
            for (int i = 0; i < 6; ++i) {
                float2 a3 = side == 0 ? float2(u.lidL[i*2],     u.lidL[i*2+1])
                                      : float2(u.lidR[i*2],     u.lidR[i*2+1]);
                float2 b3 = side == 0 ? float2(u.lidL[(i+1)*2], u.lidL[(i+1)*2+1])
                                      : float2(u.lidR[(i+1)*2], u.lidR[(i+1)*2+1]);
                float2 e3 = b3 - a3;
                float ts = clamp(dot(p - a3, e3) / max(dot(e3, e3), 1e-4), 0.0, 1.0);
                float dd = length(p - (a3 + e3 * ts));
                if (dd < dmin2) { dmin2 = dd; tbest = (float(i) + ts) / 6.0; }
            }
            float above = dot(p - outc, upv);
            float taper = 1.0 - tbest * 0.6;
            if (u.lash[2] > 0.001) {
                float lt = er * 0.055 * taper;
                float line = 1.0 - smoothstep(lt * 0.5, lt, dmin2);
                col = mix(col, ink, line * u.lash[2] * 0.95 * bfade);
            }
            if (u.lash[0] > 0.001) {
                float bt = er * 0.16 * taper;
                float band = (1.0 - smoothstep(bt * 0.35, bt, dmin2))
                           * smoothstep(-er * 0.05, er * 0.10, above);
                col = mix(col, col * 0.30, band * u.lash[0] * 0.75 * bfade);
            }
            if (u.lash[1] > 0.001) {
                float2 inc = side == 0 ? float2(u.lidL[12], u.lidL[13])
                                       : float2(u.lidR[12], u.lidR[13]);
                float2 outdir = normalize(outc - inc);
                float2 w1 = outc + (outdir * 0.80 + upv * 0.35) * er * u.lash[1];
                float wd = f_seg(p, outc, w1);
                float along = clamp(dot(p - outc, w1 - outc) /
                                    max(dot(w1 - outc, w1 - outc), 1e-4), 0.0, 1.0);
                float wt = mix(er * 0.10, er * 0.015, along);
                float wing = 1.0 - smoothstep(wt * 0.4, wt, wd);
                col = mix(col, ink, wing * max(u.lash[0], u.lash[2]) * 0.92 * bfade);
            }
        }
    }
    if (u.eyeglow[3] > 0.001) {                        // eye glow halo
        float gL = 1.0 - smoothstep(er * 0.3, er * 1.6, distance(p, eyeL));
        float gR = 1.0 - smoothstep(er * 0.3, er * 1.6, distance(p, eyeR));
        col += float3(u.eyeglow[0], u.eyeglow[1], u.eyeglow[2])
             * (u.eyeglow[3] * 0.55 * max(gL, gR));
    }
    return float4(clamp(col, 0.0, 1.0), c0.a);
}

// ── UV-mapped makeup mesh (tracked 478-pt mesh × authored makeup PNG) ────────
struct FaceMkUni { float dim[2]; float opacity; float adapt;
                   float eyes[4]; float blink[2]; float eye_r; float pad0; };
struct MkVOut { float4 pos [[position]]; float2 mkuv; float2 srcuv; };
vertex MkVOut face_mk_v(uint vid [[vertex_id]],
                        device const float4* vtx [[buffer(0)]],
                        constant FaceMkUni& u [[buffer(1)]]) {
    float4 v = vtx[vid];                 // px.xy, uv.xy
    MkVOut o;
    o.mkuv  = v.zw;
    o.srcuv = v.xy / float2(u.dim[0], u.dim[1]);
    // Framebuffer Y-down vs NDC Y-up: flip so texture-space px land upright.
    o.pos = float4(o.srcuv.x * 2.0 - 1.0, 1.0 - o.srcuv.y * 2.0, 0.0, 1.0);
    return o;
}
fragment float4 face_mk_f(MkVOut in [[stage_in]], constant FaceMkUni& u [[buffer(0)]],
                          texture2d<float> mk  [[texture(0)]],
                          texture2d<float> srct [[texture(1)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float4 mkc  = mk.sample(s, in.mkuv);
    float3 base = srct.sample(s, in.srcuv).rgb;
    float2 ppx = in.srcuv * float2(u.dim[0], u.dim[1]);
    float er2 = max(u.eye_r, 1.0);
    float nearL = 1.0 - smoothstep(er2 * 0.8, er2 * 2.2, distance(ppx, float2(u.eyes[0], u.eyes[1])));
    float nearR = 1.0 - smoothstep(er2 * 0.8, er2 * 2.2, distance(ppx, float2(u.eyes[2], u.eyes[3])));
    float bfade = 1.0 - 0.9 * max(nearL * smoothstep(0.25, 0.55, u.blink[0]),
                                  nearR * smoothstep(0.25, 0.55, u.blink[1]));
    float mka = mkc.a * bfade;
    float bl  = f_lum(base);
    float3 tint = base / max(bl, 0.04);
    float3 lit  = mkc.rgb
                * mix(float3(1.0), clamp(tint, 0.55, 1.6), u.adapt * 0.65)
                * mix(1.0, clamp(bl * 1.9, 0.20, 1.45), u.adapt * 0.85);
    return float4(mix(base, clamp(lit, 0.0, 1.0), mka * u.opacity), 1.0);
}
)";

static id<MTLDevice>              g_dev     = nil;
static id<MTLCommandQueue>        g_queue   = nil;
static id<MTLRenderPipelineState> g_bg_pso  = nil;   // background aurora
static id<MTLRenderPipelineState> g_quad_pso = nil;  // textured blit
static id<MTLTexture>             g_content = nil;    // current frame (BGRA8)
static int                        g_cw = 0, g_ch = 0;
static std::mutex                 g_content_mu;
static CVMetalTextureCacheRef     g_texcache = NULL;  // zero-copy CVPixelBuffer→MTLTexture
static CVMetalTextureRef          g_cvtex    = NULL;  // keeps the mapped frame alive
// Person matte (background replacement) — latest R8 CVPixelBuffer from the
// platform segmenter, mapped zero-copy like the content frame. All guarded by
// g_content_mu alongside the content frame they composite with.
static id<MTLRenderPipelineState> g_matte_pso   = nil;   // matte composite blit
static CVPixelBufferRef           g_matte_pb    = NULL;  // retained latest matte
static CVMetalTextureRef          g_matte_cvtex = NULL;  // keeps the mapping alive
static id<MTLTexture>             g_matte_tex   = nil;
static double                     g_matte_time  = 0.0;

// ── Layer store (scene compositor inputs) ────────────────────────────────────
// One frame per engine (track, clip): retained CVPixelBuffer mapped zero-copy
// through the shared texture cache. Guarded by g_content_mu (submissions come
// from Swift feeder threads; the render snapshots under the same lock). Stale
// keys are harmless — they just never match an active clip.
struct LayerFrame {
    CVPixelBufferRef  pb    = NULL;
    CVMetalTextureRef cvtex = NULL;
    id<MTLTexture>    tex   = nil;
    int               w = 0, h = 0;
    int               rot = 0;        // quarter turns to upright
    double            t   = 0.0;      // host/timeline time of the frame
};
static std::map<std::pair<int,int>, LayerFrame> g_layers;

// Scene clock: the timeline time of the LATEST submitted frame (host_time >= 0;
// static layers like text rasters pass -1 = "no clock"). This — not
// state.playhead — drives layer activity and FX windows: preview follows the
// AVPlayer clock exactly (state.playhead is only 5 Hz-reconciled), and export
// stamps each output frame's time (state.playhead is FROZEN during export —
// using it froze every FX window at the export start instant).
static double g_scene_clock = 0.0;
static bool   g_scene_clock_valid = false;

// Scene compositor pipelines + targets.
static id<MTLRenderPipelineState> g_layer_pso       = nil;  // transform quad, premul over
static id<MTLRenderPipelineState> g_layer_matte_pso = nil;  // + person-matte cutout
static id<MTLRenderPipelineState> g_solid_pso       = nil;  // DipWhite veil
static id<MTLTexture>             g_scene[2]        = { nil, nil };
static int                        g_scene_w = 0, g_scene_h = 0;
// Scene diagnostics of the last rendered frame (fx_debug), guarded by g_stack_mu.
static nlohmann::json             g_scene_debug;
// ── FX runner ────────────────────────────────────────────────────────────────
// Loads the transpiled MSL registry (Shaders/msl/<name>.metal + params_manifest
// .json, bundled by the app) and runs the live-FX stack as a ping-pong chain over
// g_content before the aspect-fit blit. Ports the desktop fx_apply model: one
// single-pass fragment per effect + an automatic wet/dry blend for `amount`.
using fxjson = nlohmann::json;

struct ParamField  { std::string name; int count; size_t offset; };
struct ManifestEntry { std::vector<ParamField> params; size_t params_size = 0; std::string entry; };
static std::unordered_map<std::string, ManifestEntry> g_manifest;
static bool g_manifest_loaded = false;

struct LiveFx { std::string fx_type; float amount = 1.0f; float start = -1e30f, end = 1e30f;
                std::map<std::string, float> params;
                std::string body_fx_type;           // fx_type == "body_fx" only
                int         body_pass = -1;         // index into the body pass table, -1 unknown
                std::string face_makeup_tex; };     // fx_type == "face_fx": UV makeup PNG name
static std::vector<LiveFx> g_stack;
static std::mutex          g_stack_mu;
static double              g_content_time = 0.0;   // timeline time of the current frame (for FX windowing)
// Per-entry pass status of the last rendered frame, index-aligned with g_stack
// (guarded by g_stack_mu; surfaced through metal_render_fx_debug).
static std::vector<std::string> g_pass_status;

// ── Body FX (matte-consuming hand-written passes) ────────────────────────────
// Selected by BodyFXInfo name (body_fx.cpp table). "Body Glitch" accepted as an
// alias of the desktop's "Glitch Body".
enum { kBodyNeonOutline = 0, kBodyDepthBlur = 1, kBodyGlitch = 2, kBodyPassCount = 3 };
static const char* const kBodyEntry[kBodyPassCount] =
    { "body_neon_outline", "body_depth_blur", "body_glitch" };
static id<MTLRenderPipelineState> g_body_pso[kBodyPassCount] = { nil, nil, nil };
static bool g_body_tried = false;

static int body_pass_index(const std::string& name) {
    if (name == "Neon Outline") return kBodyNeonOutline;
    if (name == "Depth Blur")   return kBodyDepthBlur;
    if (name == "Glitch Body" || name == "Body Glitch") return kBodyGlitch;
    return -1;
}

// Byte-for-byte mirror of the MSL BodyUni (plain floats, no alignment traps).
struct BodyUniCPU { float p0, p1, p2, p3, time, tw, th; };

// Named param lookup with the desktop BodyFXInfo label first, then friendlier
// aliases, then the positional slot name; falls back to the table default.
static float body_param(const LiveFx& fx, std::initializer_list<const char*> names, float def) {
    for (const char* n : names) {
        auto it = fx.params.find(n);
        if (it != fx.params.end()) return it->second;
    }
    return def;
}

// Defaults per body_fx.cpp's BodyFXInfo table:
//   Neon Outline: {"Glow Width", 1..4, 2}, {"Hue", 0..1, 0.8}
//   Depth Blur:   {"Blur Radius (px)", 0..20, 8}
//   Glitch Body:  {"Chroma (px)", 0..20, 8}, {"Jitter", 0..1, 0.3}
static BodyUniCPU body_uniforms(const LiveFx& fx, int w, int h, double t) {
    BodyUniCPU u = { 0, 0, 0, 0, (float)t, (float)w, (float)h };
    switch (fx.body_pass) {
        case kBodyNeonOutline:
            u.p0 = body_param(fx, { "Glow Width", "glow_width", "width", "p0" }, 2.0f);
            u.p1 = body_param(fx, { "Hue", "hue", "p1" }, 0.8f);
            break;
        case kBodyDepthBlur:
            u.p0 = body_param(fx, { "Blur Radius (px)", "blur_radius_px", "blur_radius", "radius", "p0" }, 8.0f);
            break;
        case kBodyGlitch:
            u.p0 = body_param(fx, { "Chroma (px)", "chroma_px", "chroma", "p0" }, 8.0f);
            u.p1 = body_param(fx, { "Jitter", "jitter", "p1" }, 0.3f);
            break;
    }
    return u;
}

// ── Chroma feedback FX (stateful hand-written passes) ────────────────────────
// The desktop chroma family (fx_shader.cpp) keeps per-slot GPU state; here the
// state is keyed by (chain id, stack index, fx_type) so a melt on layer A and a
// melt on layer B never share trails. chain ids: per-layer glass chains use
// (track<<12)|clip, the per-track bus uses -(track+16), the single-content
// (camera) path uses -2. Entries untouched for kChromaGCFrames frames are
// dropped (clip deleted / stack changed) so textures don't leak.
enum { kChromaMelt = 0, kChromaEcho = 1, kChromaFrame = 2, kChromaPassCount = 3,
       kChromaSnapshot = 3, kChromaPsoCount = 4 };   // snapshot = internal helper pass
enum { kChromaRingLayers = 8, kChromaGCFrames = 600 };
static const char* const kChromaEntry[kChromaPsoCount] =
    { "chroma_melt_f", "chroma_echo_f", "chroma_frame_f", "chroma_snapshot_f" };
static id<MTLRenderPipelineState> g_chroma_pso[kChromaPsoCount] = { nil, nil, nil, nil };
static bool g_chroma_tried = false;

static int chroma_pass_index(const std::string& fx_type) {
    if (fx_type == "chroma_melt")  return kChromaMelt;
    if (fx_type == "chroma_echo")  return kChromaEcho;
    if (fx_type == "chroma_frame") return kChromaFrame;
    return -1;
}

// Byte-for-byte mirror of the MSL ChromaUni (plain floats).
struct ChromaUniCPU { float key_r, key_g, key_b, threshold,
                            persist, head, ntaps, falloff, tw, th, matte_key; };

struct ChromaFbState {
    id<MTLTexture> feedback = nil;      // melt/echo: previous pass output
    id<MTLTexture> ring     = nil;      // frame: 2D array of past snapshots
    int      head = -1, count = 0;      // ring cursor
    double   last_t = -1e9;             // last snapshot time (frame)
    int      w = 0, h = 0;
    bool     primed = false;            // feedback holds a valid prior frame
    uint64_t last_used = 0;             // frame serial, for GC
};
static std::map<std::tuple<int, int, std::string>, ChromaFbState> g_chroma_fb;
static uint64_t g_frame_serial = 0;

static void chroma_fb_gc() {
    for (auto it = g_chroma_fb.begin(); it != g_chroma_fb.end();)
        if (g_frame_serial - it->second.last_used > kChromaGCFrames)
            it = g_chroma_fb.erase(it);
        else ++it;
}

static float chroma_param(const LiveFx& fx, const char* name, float def) {
    auto it = fx.params.find(name);
    return it != fx.params.end() ? it->second : def;
}

// Defaults mirror the Clip field initializers in app.h. `matte_key` is a
// live-stack-only param (record mode): 1 = the person matte is the key.
static ChromaUniCPU chroma_uniforms(const LiveFx& fx, int pass, int w, int h) {
    ChromaUniCPU u = { 0, 1, 0, 0.30f, 0, 0, 0, 0, (float)w, (float)h,
                       chroma_param(fx, "matte_key", 0.f) };
    switch (pass) {
        case kChromaMelt:
            u.key_r     = chroma_param(fx, "chroma_melt_r", 0.f);
            u.key_g     = chroma_param(fx, "chroma_melt_g", 1.f);
            u.key_b     = chroma_param(fx, "chroma_melt_b", 0.f);
            u.threshold = chroma_param(fx, "chroma_melt_threshold", 0.30f);
            u.persist   = chroma_param(fx, "chroma_melt_persist",   0.88f);
            break;
        case kChromaEcho:
            u.key_r     = chroma_param(fx, "chroma_echo_r", 0.f);
            u.key_g     = chroma_param(fx, "chroma_echo_g", 1.f);
            u.key_b     = chroma_param(fx, "chroma_echo_b", 0.f);
            u.threshold = chroma_param(fx, "chroma_echo_threshold", 0.30f);
            u.persist   = chroma_param(fx, "chroma_echo_persist",   0.92f);
            break;
        case kChromaFrame:
            u.key_r     = chroma_param(fx, "chroma_frame_r", 0.f);
            u.key_g     = chroma_param(fx, "chroma_frame_g", 1.f);
            u.key_b     = chroma_param(fx, "chroma_frame_b", 0.f);
            u.threshold = chroma_param(fx, "chroma_frame_threshold", 0.30f);
            u.ntaps     = chroma_param(fx, "chroma_frame_taps",    4.f);
            u.falloff   = chroma_param(fx, "chroma_frame_falloff", 0.75f);
            break;
    }
    return u;
}

// ── Face FX (makeup looks — stateless passes over live FaceObs) ─────────────
enum { kFaceWarp = 0, kFaceBeauty = 1, kFaceMesh = 2, kFacePsoCount = 3 };
static id<MTLRenderPipelineState> g_face_pso[kFacePsoCount] = { nil, nil, nil };
static bool g_face_tried = false;

// CPU mirrors of the MSL uniform structs (plain floats — layouts match).
struct FaceWarpUniCPU { float n, aspect, pad0, pad1; float ba[48]; float bb[48]; };
struct FaceBeautyUniCPU {
    float dim[2]; float up[2];
    float face[4]; float eyes[4]; float feat[4]; float amt[4]; float makeup[4];
    float cheeks[4]; float blushc[4]; float lipc[4]; float eyeglow[4];
    float cyber[4]; float tintc[4]; float mouthax[4];
    float lippoly[24];
    float nose[4]; float lash[4]; float chin[4]; float eyeout[4];
    float lidL[14]; float lidR[14];
    float brow_r; float pad0; float blink[2];
};
struct FaceMkUniCPU { float dim[2]; float opacity, adapt;
                      float eyes[4]; float blink[2]; float eye_r, pad0; };

// Makeup PNGs (models/face/, canonical-UV space) → RGBA8 textures, by name.
static std::map<std::string, id<MTLTexture>> g_face_mk_tex;

static id<MTLTexture> face_makeup_texture(const char* name) {
    auto it = g_face_mk_tex.find(name);
    if (it != g_face_mk_tex.end()) return it->second;
    std::string path = app_models_dir() + "/face/" + name;
    int w = 0, h = 0, n = 0;
    unsigned char* rgba = stbi_load(path.c_str(), &w, &h, &n, 4);
    id<MTLTexture> tex = nil;
    if (rgba && w > 0 && h > 0) {
        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        tex = [g_dev newTextureWithDescriptor:td];
        [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0
                 withBytes:rgba bytesPerRow:(NSUInteger)w * 4];
    } else {
        NSLog(@"[face_fx] makeup texture missing: %s", path.c_str());
    }
    if (rgba) stbi_image_free(rgba);
    g_face_mk_tex[name] = tex;             // negative-cache misses too
    return tex;
}

static id<MTLRenderPipelineState> get_face_pso(int which);   // after g_fs_v

// Fill the beauty uniform from the plan's FaceBeautyParams (1:1 field map —
// the GL path feeds the same values through glUniform calls).
static FaceBeautyUniCPU face_beauty_uniforms(const FaceBeautyParams& p, int w, int h) {
    FaceBeautyUniCPU u = {};
    u.dim[0] = (float)w; u.dim[1] = (float)h;
    u.up[0] = p.upx; u.up[1] = p.upy;
    u.face[0] = p.face_cx; u.face[1] = p.face_cy; u.face[2] = p.face_rx; u.face[3] = p.face_ry;
    u.eyes[0] = p.eyeL_x; u.eyes[1] = p.eyeL_y; u.eyes[2] = p.eyeR_x; u.eyes[3] = p.eyeR_y;
    u.feat[0] = p.eye_r; u.feat[1] = p.mouth_x; u.feat[2] = p.mouth_y; u.feat[3] = p.mouth_r;
    u.amt[0] = p.smooth; u.amt[1] = p.brighten; u.amt[2] = p.warmth; u.amt[3] = p.eye_pop;
    u.makeup[0] = p.blush; u.makeup[1] = p.lip_tint; u.makeup[2] = p.lip_grad; u.makeup[3] = p.jaw_shade;
    u.cheeks[0] = p.cheekL_x; u.cheeks[1] = p.cheekL_y; u.cheeks[2] = p.cheekR_x; u.cheeks[3] = p.cheekR_y;
    for (int i = 0; i < 3; ++i) {
        u.blushc[i] = p.blush_col[i]; u.lipc[i] = p.lip_col[i];
        u.eyeglow[i] = p.eye_glow_col[i]; u.tintc[i] = p.tint_col[i];
    }
    u.eyeglow[3] = p.eye_glow;
    u.cyber[0] = p.skin_tint; u.cyber[1] = p.desat; u.cyber[2] = p.chrome; u.cyber[3] = p.scanlines;
    u.mouthax[0] = p.mouth_sw; u.mouthax[1] = p.mouth_sh;
    for (int i = 0; i < 12; ++i) {
        u.lippoly[i*2]   = p.lip_poly[i][0];
        u.lippoly[i*2+1] = p.lip_poly[i][1];
    }
    u.nose[0] = p.nose_x; u.nose[1] = p.nose_y; u.nose[2] = p.nose_blush; u.nose[3] = p.freckles;
    u.lash[0] = p.lash; u.lash[1] = p.lash_wing; u.lash[2] = p.liner;
    u.chin[0] = p.chin_x; u.chin[1] = p.chin_y; u.chin[2] = p.chin_smooth;
    u.eyeout[0] = p.eyeoutL_x; u.eyeout[1] = p.eyeoutL_y;
    u.eyeout[2] = p.eyeoutR_x; u.eyeout[3] = p.eyeoutR_y;
    for (int i = 0; i < 7; ++i) {
        u.lidL[i*2] = p.lidL[i][0]; u.lidL[i*2+1] = p.lidL[i][1];
        u.lidR[i*2] = p.lidR[i][0]; u.lidR[i*2+1] = p.lidR[i][1];
    }
    u.brow_r = p.brow_r;
    u.blink[0] = p.blink_l; u.blink[1] = p.blink_r;
    return u;
}

// A BeautyLook from a face_fx live entry: optional preset base (face_filter)
// overlaid with per-field params — the Makeup Studio's custom looks arrive
// exactly this way (no enum growth per look).
static BeautyLook face_look_from(const LiveFx& fx) {
    BeautyLook L{};
    auto it = fx.params.find("face_filter");
    if (it != fx.params.end() && (int)it->second > 0)
        beauty_look_for((int)it->second, L);
    auto ov = [&](const char* k, float& dst) {
        auto p = fx.params.find(k);
        if (p != fx.params.end()) dst = p->second;
    };
    ov("smooth", L.smooth); ov("brighten", L.brighten); ov("warmth", L.warmth);
    ov("eye_pop", L.eye_pop); ov("blush", L.blush); ov("lip", L.lip);
    ov("eyes", L.eyes); ov("cheek", L.cheek); ov("vline", L.vline);
    ov("nose", L.nose); ov("lips_plump", L.lips_plump);
    ov("lip_grad", L.lip_grad); ov("nose_blush", L.nose_blush);
    ov("freckles", L.freckles); ov("jaw_shade", L.jaw_shade);
    ov("chin_smooth", L.chin_smooth);
    ov("lash", L.lash); ov("liner", L.liner); ov("lash_wing", L.lash_wing);
    ov("blush_r", L.blush_col[0]); ov("blush_g", L.blush_col[1]); ov("blush_b", L.blush_col[2]);
    ov("lip_r", L.lip_col[0]); ov("lip_g", L.lip_col[1]); ov("lip_b", L.lip_col[2]);
    ov("blush_raise", L.blush_raise); ov("makeup_adapt", L.makeup_adapt);
    ov("eye_glow", L.eye_glow);
    ov("glow_r", L.eye_glow_col[0]); ov("glow_g", L.eye_glow_col[1]); ov("glow_b", L.eye_glow_col[2]);
    ov("skin_tint", L.skin_tint);
    ov("tint_r", L.tint_col[0]); ov("tint_g", L.tint_col[1]); ov("tint_b", L.tint_col[2]);
    ov("desat", L.desat); ov("chrome", L.chrome); ov("scanlines", L.scanlines);
    if (!fx.face_makeup_tex.empty()) L.makeup_tex = fx.face_makeup_tex.c_str();
    return L;
}

struct FxProgram { id<MTLRenderPipelineState> pso = nil; const ManifestEntry* m = nullptr; bool tried = false; };
static std::unordered_map<std::string, FxProgram> g_fx_progs;

static id<MTLRenderPipelineState> g_fx_blend_pso = nil;   // wet/dry
static id<MTLFunction>            g_fs_v          = nil;   // shared fullscreen vertex
static id<MTLSamplerState>        g_fx_sampler    = nil;
static std::string                g_shader_dir;            // optional override (headless/test)

// Ping-pong texture pool keyed by size — FX chains run at the source's size
// (per-layer chains at layer size, bus chains at canvas size), so one global
// pair is not enough. Bounded by the number of distinct sizes in a project.
// Safe across chains in one frame: every chain's result is consumed by a pass
// encoded immediately after it, and command-buffer passes execute in order.
static std::map<std::pair<int,int>, std::array<id<MTLTexture>, 2>> g_pp_pool;
static std::array<id<MTLTexture>, 2>& pp_get(int w, int h) {
    auto& pair = g_pp_pool[{w, h}];
    if (!pair[0]) {
        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        pair[0] = [g_dev newTextureWithDescriptor:td];
        pair[1] = [g_dev newTextureWithDescriptor:td];
    }
    return pair;
}

// Lazily compile the three hand-written body passes (shared library, one PSO
// each). Deferred until the first body_fx entry so plain stacks pay nothing.
static id<MTLRenderPipelineState> get_body_pso(int pass) {
    if (!g_body_tried) {
        g_body_tried = true;
        if (g_dev && g_fs_v) {
            NSError* err = nil;
            id<MTLLibrary> lib = [g_dev newLibraryWithSource:kBodySrc options:nil error:&err];
            if (!lib) NSLog(@"[body_fx] library: %@", err);
            for (int k = 0; lib && k < kBodyPassCount; ++k) {
                id<MTLFunction> frag = [lib newFunctionWithName:
                    [NSString stringWithUTF8String:kBodyEntry[k]]];
                if (!frag) { NSLog(@"[body_fx] %s: function not found", kBodyEntry[k]); continue; }
                MTLRenderPipelineDescriptor* rd = [MTLRenderPipelineDescriptor new];
                rd.vertexFunction = g_fs_v; rd.fragmentFunction = frag;
                rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                g_body_pso[k] = [g_dev newRenderPipelineStateWithDescriptor:rd error:&err];
                if (!g_body_pso[k]) NSLog(@"[body_fx] %s pso: %@", kBodyEntry[k], err);
            }
        }
    }
    return (pass >= 0 && pass < kBodyPassCount) ? g_body_pso[pass] : nil;
}

// Lazily compile the face passes (warp / beauty fullscreen + the UV mesh
// pipeline with its own vertex function).
static id<MTLRenderPipelineState> get_face_pso(int which) {
    if (!g_face_tried) {
        g_face_tried = true;
        if (g_dev && g_fs_v) {
            NSError* err = nil;
            id<MTLLibrary> lib = [g_dev newLibraryWithSource:kFaceSrc options:nil error:&err];
            if (!lib) NSLog(@"[face_fx] library: %@", err);
            if (lib) {
                auto make = [&](int slot, NSString* frag, id<MTLFunction> vfn) {
                    id<MTLFunction> f = [lib newFunctionWithName:frag];
                    if (!f) { NSLog(@"[face_fx] %@ not found", frag); return; }
                    MTLRenderPipelineDescriptor* rd = [MTLRenderPipelineDescriptor new];
                    rd.vertexFunction = vfn; rd.fragmentFunction = f;
                    rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                    g_face_pso[slot] = [g_dev newRenderPipelineStateWithDescriptor:rd error:&err];
                    if (!g_face_pso[slot]) NSLog(@"[face_fx] %@ pso: %@", frag, err);
                };
                make(kFaceWarp,   @"face_warp_f",   g_fs_v);
                make(kFaceBeauty, @"face_beauty_f", g_fs_v);
                id<MTLFunction> mv = [lib newFunctionWithName:@"face_mk_v"];
                if (mv) make(kFaceMesh, @"face_mk_f", mv);
            }
        }
    }
    return (which >= 0 && which < kFacePsoCount) ? g_face_pso[which] : nil;
}

// Lazily compile the three chroma passes (same pattern as the body passes).
static id<MTLRenderPipelineState> get_chroma_pso(int pass) {
    if (!g_chroma_tried) {
        g_chroma_tried = true;
        if (g_dev && g_fs_v) {
            NSError* err = nil;
            id<MTLLibrary> lib = [g_dev newLibraryWithSource:kChromaSrc options:nil error:&err];
            if (!lib) NSLog(@"[chroma_fx] library: %@", err);
            for (int k = 0; lib && k < kChromaPsoCount; ++k) {
                id<MTLFunction> frag = [lib newFunctionWithName:
                    [NSString stringWithUTF8String:kChromaEntry[k]]];
                if (!frag) { NSLog(@"[chroma_fx] %s: function not found", kChromaEntry[k]); continue; }
                MTLRenderPipelineDescriptor* rd = [MTLRenderPipelineDescriptor new];
                rd.vertexFunction = g_fs_v; rd.fragmentFunction = frag;
                rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                g_chroma_pso[k] = [g_dev newRenderPipelineStateWithDescriptor:rd error:&err];
                if (!g_chroma_pso[k]) NSLog(@"[chroma_fx] %s pso: %@", kChromaEntry[k], err);
            }
        }
    }
    return (pass >= 0 && pass < kChromaPsoCount) ? g_chroma_pso[pass] : nil;
}

static NSString* shader_path(const char* name, const char* ext) {
    if (!g_shader_dir.empty())
        return [NSString stringWithFormat:@"%s/%s.%s", g_shader_dir.c_str(), name, ext];
    // Resolve directly from the bundle resource URL — pathForResource:inDirectory:
    // doesn't reliably find files inside a folder-reference (blue folder).
    NSFileManager* fm = [NSFileManager defaultManager];
    NSString* file = [NSString stringWithFormat:@"%s.%s", name, ext];
    NSURL* res = [[NSBundle mainBundle] resourceURL];
    NSString* inMsl = [[[res URLByAppendingPathComponent:@"msl"] URLByAppendingPathComponent:file] path];
    if ([fm fileExistsAtPath:inMsl]) return inMsl;
    NSString* atRoot = [[res URLByAppendingPathComponent:file] path];
    if ([fm fileExistsAtPath:atRoot]) return atRoot;
    return [[NSBundle mainBundle] pathForResource:[NSString stringWithUTF8String:name]
                                           ofType:[NSString stringWithUTF8String:ext] inDirectory:@"msl"];
}

static void load_manifest() {
    if (g_manifest_loaded) return;
    g_manifest_loaded = true;
    NSString* p = shader_path("params_manifest", "json");
    NSData* d = p ? [NSData dataWithContentsOfFile:p] : nil;
    if (!d) { NSLog(@"[fx] params_manifest.json not found"); return; }
    try {
        fxjson m = fxjson::parse(std::string((const char*)d.bytes, d.length));
        auto add = [&](const std::string& name, const fxjson& entry) {
            if (name.empty()) return;
            ManifestEntry e;
            e.params_size = entry.value("params_size", (size_t)0);
            e.entry = entry.value("entry", std::string("fx_") + name);
            for (const auto& pf : entry.value("params", fxjson::array()))
                e.params.push_back({ pf.value("name", std::string()), pf.value("count", 0),
                                     (size_t)pf.value("offset", 0) });
            g_manifest[name] = std::move(e);
        };
        if (m.is_array())                                    // [{shader,params,...}, ...]
            for (const auto& entry : m) add(entry.value("shader", std::string()), entry);
        else                                                 // {name: {params,...}, ...}
            for (auto it = m.begin(); it != m.end(); ++it) add(it.key(), it.value());
        NSLog(@"[fx] manifest loaded: %zu effects", g_manifest.size());
    } catch (const std::exception& ex) { NSLog(@"[fx] manifest parse failed: %s", ex.what()); }
}

static FxProgram* get_fx_program(const std::string& fx_type) {
    auto it = g_fx_progs.find(fx_type);
    if (it != g_fx_progs.end()) return it->second.pso ? &it->second : nullptr;
    FxProgram fp; fp.tried = true;
    auto mit = g_manifest.find(fx_type);
    if (mit != g_manifest.end() && g_fs_v && g_dev) {
        fp.m = &mit->second;
        NSString* path = shader_path(fx_type.c_str(), "metal");
        NSString* src = path ? [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] : nil;
        if (src) {
            NSError* err = nil;
            id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&err];
            id<MTLFunction> frag = lib ? [lib newFunctionWithName:[NSString stringWithUTF8String:mit->second.entry.c_str()]] : nil;
            if (frag) {
                MTLRenderPipelineDescriptor* rd = [MTLRenderPipelineDescriptor new];
                rd.vertexFunction = g_fs_v; rd.fragmentFunction = frag;
                rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                fp.pso = [g_dev newRenderPipelineStateWithDescriptor:rd error:&err];
                if (!fp.pso) NSLog(@"[fx] %s pso: %@", fx_type.c_str(), err);
            } else NSLog(@"[fx] %s compile: %@", fx_type.c_str(), err);
        }
    }
    g_fx_progs[fx_type] = fp;
    return fp.pso ? &g_fx_progs[fx_type] : nullptr;
}

// Write the std140 params buffer per the manifest (offsets authoritative). Lifted
// uniforms are filled from render state; the rest from the brick's params by name.
static void fill_params(const ManifestEntry& m, const LiveFx& fx, int w, int h, double t,
                        std::vector<uint8_t>& buf) {
    buf.assign(m.params_size ? m.params_size : 16, 0);
    for (const auto& pf : m.params) {
        int n = pf.count <= 0 ? 1 : pf.count;
        for (int i = 0; i < n; ++i) {
            float v = 0.0f;
            if      (pf.name == "u_tex_w")    v = (float)w;
            else if (pf.name == "u_tex_h")    v = (float)h;
            else if (pf.name == "u_time")     v = (float)t;
            else if (pf.name == "u_strength") {          // brick knob, else the wet/dry amount
                auto p = fx.params.find("strength");
                if (p == fx.params.end()) p = fx.params.find("amount");
                v = (p != fx.params.end()) ? p->second : fx.amount;
            }
            else {
                std::string bare = pf.name.rfind("u_", 0) == 0 ? pf.name.substr(2) : pf.name;
                auto p = fx.params.find(bare);
                if (p == fx.params.end()) p = fx.params.find(pf.name);
                if (p != fx.params.end()) v = p->second;
            }
            size_t off = pf.offset + (size_t)i * 4;
            if (off + 4 <= buf.size()) memcpy(buf.data() + off, &v, 4);
        }
    }
}

// ── Unified FX-stack runner ──────────────────────────────────────────────────
// Runs an ordered LiveFx stack over `src` as a ping-pong chain on `cb`:
// generated-MSL effects (manifest PSOs), matte body passes (body_fx entries),
// and the automatic wet/dry blend per entry. Returns the last pass output
// (== src when nothing applied). Every entry gets a status string — never a
// silent skip. `window_t` gates entries to their [start,end) span; `matte` may
// be nil (body passes then report no_matte). RemoveBackground body entries set
// *matte_cutout (the cutout happens at composite time, not as a chain pass).
struct FxRunResult { int applied = 0; bool matte_cutout = false; bool pso_failed = false; };
static id<MTLTexture> run_fx_stack(id<MTLCommandBuffer> cb, id<MTLTexture> src,
                                   int sw, int sh, int chain_id,
                                   const std::vector<LiveFx>& stack,
                                   double anim_t, double window_t,
                                   id<MTLTexture> matte,
                                   std::vector<std::string>* status_out,
                                   FxRunResult* result) {
    if (status_out) status_out->assign(stack.size(), "");
    if (stack.empty() || !src || sw <= 0 || sh <= 0) return src;
    auto& ping = pp_get(sw, sh);
    if (!ping[0] || !ping[1]) {
        static bool s_logged = false;
        if (!s_logged) { s_logged = true; NSLog(@"[fx] ping-pong alloc failed (%dx%d)", sw, sh); }
        if (status_out) status_out->assign(stack.size(), "no_pingpong");
        return src;
    }
    id<MTLTexture> cur = src; int dst = 0;
    std::vector<uint8_t> pbuf;
    auto set_status = [&](size_t i, const char* s) { if (status_out) (*status_out)[i] = s; };
    for (size_t i = 0; i < stack.size(); ++i) {
        const LiveFx& fx = stack[i];
        if (!(fx.start <= window_t && window_t < fx.end)) { set_status(i, "out_of_window"); continue; }

        // ── face_fx: multi-pass makeup look (beauty → UV mesh → warp) ────────
        // Fully self-encoded (its passes don't fit the shared single-pass
        // encode below). Live camera only for now: obs comes from the face
        // worker; layer chains report no_face until the take-cache port.
        if (fx.fx_type == "face_fx") {
            if (!face_track_available()) { set_status(i, "face_models_missing"); continue; }
            FaceObs obs;
            if (!face_track_latest(obs) || !obs.valid) { set_status(i, "no_face"); continue; }
            id<MTLRenderPipelineState> warp_pso   = get_face_pso(kFaceWarp);
            id<MTLRenderPipelineState> beauty_pso = get_face_pso(kFaceBeauty);
            id<MTLRenderPipelineState> mesh_pso   = get_face_pso(kFaceMesh);
            if (!warp_pso || !beauty_pso || !mesh_pso) {
                set_status(i, "pso_failed"); if (result) result->pso_failed = true; continue;
            }
            BeautyLook look = face_look_from(fx);
            float famt = 1.f;
            { auto p = fx.params.find("face_amount"); if (p != fx.params.end()) famt = p->second; }
            FaceRenderPlan plan;
            if (!face_filter_build_plan_look(look, famt, obs, sw, sh, plan) || !plan.valid) {
                set_status(i, "face_plan_empty"); continue;
            }
            // 1. beauty (skin + procedural makeup) — fullscreen.
            if (plan.has_beauty) {
                MTLRenderPassDescriptor* rp1 = [MTLRenderPassDescriptor new];
                rp1.colorAttachments[0].texture     = ping[dst];
                rp1.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
                rp1.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> e1 = [cb renderCommandEncoderWithDescriptor:rp1];
                [e1 setRenderPipelineState:beauty_pso];
                FaceBeautyUniCPU bu = face_beauty_uniforms(plan.beauty, sw, sh);
                [e1 setFragmentBytes:&bu length:sizeof(bu) atIndex:0];
                [e1 setFragmentTexture:cur atIndex:0];
                [e1 drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [e1 endEncoding];
                cur = ping[dst]; dst ^= 1;
            }
            // 2. UV-mapped makeup texture over the tracked mesh (pre-warp so
            //    pigment deforms with the skin). Base copy + mesh on top.
            id<MTLTexture> mk = plan.makeup_tex ? face_makeup_texture(plan.makeup_tex) : nil;
            if (mk) {
                id<MTLTexture> target = ping[dst];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl copyFromTexture:cur sourceSlice:0 sourceLevel:0
                       sourceOrigin:MTLOriginMake(0, 0, 0)
                         sourceSize:MTLSizeMake(sw, sh, 1)
                          toTexture:target destinationSlice:0
                   destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
                [bl endEncoding];
                // Vertex buffer: landmark px + canonical UV; index buffer with
                // fold-culled triangles (majority-winding rule, GL parity).
                static float vtx[FACE_UV_NPTS * 4];
                for (int k = 0; k < FACE_UV_NPTS; ++k) {
                    vtx[k*4+0] = plan.mesh_pts[k][0];
                    vtx[k*4+1] = plan.mesh_pts[k][1];
                    vtx[k*4+2] = k_face_uv[k][0];
                    vtx[k*4+3] = k_face_uv[k][1];
                }
                static unsigned short live_tris[FACE_UV_NTRI * 3];
                static float sa[FACE_UV_NTRI];
                int n_live = 0, n_pos = 0;
                for (int t2 = 0; t2 < FACE_UV_NTRI; ++t2) {
                    const unsigned short* tr = k_face_tris[t2];
                    float ax = plan.mesh_pts[tr[1]][0] - plan.mesh_pts[tr[0]][0];
                    float ay = plan.mesh_pts[tr[1]][1] - plan.mesh_pts[tr[0]][1];
                    float bx2 = plan.mesh_pts[tr[2]][0] - plan.mesh_pts[tr[0]][0];
                    float by2 = plan.mesh_pts[tr[2]][1] - plan.mesh_pts[tr[0]][1];
                    sa[t2] = ax * by2 - ay * bx2;
                    if (sa[t2] > 0.f) ++n_pos;
                }
                bool front_pos = n_pos * 2 >= FACE_UV_NTRI;
                for (int t2 = 0; t2 < FACE_UV_NTRI; ++t2) {
                    if ((sa[t2] > 0.f) != front_pos) continue;
                    live_tris[n_live*3+0] = k_face_tris[t2][0];
                    live_tris[n_live*3+1] = k_face_tris[t2][1];
                    live_tris[n_live*3+2] = k_face_tris[t2][2];
                    ++n_live;
                }
                if (n_live > 0) {
                    id<MTLBuffer> vb = [g_dev newBufferWithBytes:vtx length:sizeof(vtx)
                                                         options:MTLResourceStorageModeShared];
                    id<MTLBuffer> ib = [g_dev newBufferWithBytes:live_tris
                                                          length:(NSUInteger)n_live * 3 * sizeof(unsigned short)
                                                         options:MTLResourceStorageModeShared];
                    FaceMkUniCPU mu = {};
                    mu.dim[0] = (float)sw; mu.dim[1] = (float)sh;
                    mu.opacity = plan.makeup_opacity; mu.adapt = plan.makeup_adapt;
                    mu.eyes[0] = plan.beauty.eyeL_x; mu.eyes[1] = plan.beauty.eyeL_y;
                    mu.eyes[2] = plan.beauty.eyeR_x; mu.eyes[3] = plan.beauty.eyeR_y;
                    mu.blink[0] = plan.beauty.blink_l; mu.blink[1] = plan.beauty.blink_r;
                    mu.eye_r = plan.beauty.eye_r;
                    MTLRenderPassDescriptor* rp2 = [MTLRenderPassDescriptor new];
                    rp2.colorAttachments[0].texture     = target;
                    rp2.colorAttachments[0].loadAction  = MTLLoadActionLoad;
                    rp2.colorAttachments[0].storeAction = MTLStoreActionStore;
                    id<MTLRenderCommandEncoder> e2 = [cb renderCommandEncoderWithDescriptor:rp2];
                    [e2 setRenderPipelineState:mesh_pso];
                    [e2 setVertexBuffer:vb offset:0 atIndex:0];
                    [e2 setVertexBytes:&mu length:sizeof(mu) atIndex:1];
                    [e2 setFragmentBytes:&mu length:sizeof(mu) atIndex:0];
                    [e2 setFragmentTexture:mk atIndex:0];
                    [e2 setFragmentTexture:cur atIndex:1];
                    [e2 drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                   indexCount:(NSUInteger)n_live * 3
                                    indexType:MTLIndexTypeUInt16
                                  indexBuffer:ib indexBufferOffset:0];
                    [e2 endEncoding];
                }
                cur = target; dst ^= 1;
            }
            // 3. shape warp — fullscreen, last so it deforms skin + pigment.
            if (plan.n_bumps > 0) {
                FaceWarpUniCPU wu = {};
                wu.n = (float)std::min(plan.n_bumps, 12);
                wu.aspect = (float)sw / (float)sh;
                for (int k = 0; k < plan.n_bumps && k < 12; ++k) {
                    wu.ba[k*4+0] = plan.bumps[k].cx;  wu.ba[k*4+1] = plan.bumps[k].cy;
                    wu.ba[k*4+2] = plan.bumps[k].radius; wu.ba[k*4+3] = plan.bumps[k].scale;
                    wu.bb[k*4+0] = plan.bumps[k].dx;  wu.bb[k*4+1] = plan.bumps[k].dy;
                }
                MTLRenderPassDescriptor* rp3 = [MTLRenderPassDescriptor new];
                rp3.colorAttachments[0].texture     = ping[dst];
                rp3.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
                rp3.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> e3 = [cb renderCommandEncoderWithDescriptor:rp3];
                [e3 setRenderPipelineState:warp_pso];
                [e3 setFragmentBytes:&wu length:sizeof(wu) atIndex:0];
                [e3 setFragmentTexture:cur atIndex:0];
                [e3 drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [e3 endEncoding];
                cur = ping[dst]; dst ^= 1;
            }
            set_status(i, "applied");
            if (result) result->applied++;
            continue;
        }

        id<MTLRenderPipelineState> pso = nil;
        const ManifestEntry* mf = nullptr;
        ChromaFbState* cst = nullptr;                 // chroma feedback entry
        int chroma_pass = chroma_pass_index(fx.fx_type);
        bool is_body = (fx.fx_type == "body_fx");
        if (!is_body && chroma_pass >= 0) {
            pso = get_chroma_pso(chroma_pass);
            if (!pso) { set_status(i, "pso_failed"); if (result) result->pso_failed = true; continue; }
            // matte_key = 1 keys on the person matte (selfie mode) — requires one.
            if (chroma_param(fx, "matte_key", 0.f) > 0.5f && !matte) {
                set_status(i, "no_matte"); continue;
            }
            cst = &g_chroma_fb[{chain_id, (int)i, fx.fx_type}];
            cst->last_used = g_frame_serial;
            if (cst->w != sw || cst->h != sh) {       // (re)allocate state textures
                *cst = ChromaFbState{};
                cst->last_used = g_frame_serial;
                cst->w = sw; cst->h = sh;
                MTLTextureDescriptor* td = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                    width:sw height:sh mipmapped:NO];
                td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
                if (chroma_pass == kChromaFrame) {
                    td.textureType = MTLTextureType2DArray;
                    td.arrayLength = kChromaRingLayers;
                    cst->ring = [g_dev newTextureWithDescriptor:td];
                } else {
                    cst->feedback = [g_dev newTextureWithDescriptor:td];
                }
            }
            if ((chroma_pass == kChromaFrame && !cst->ring) ||
                (chroma_pass != kChromaFrame && !cst->feedback)) {
                set_status(i, "pso_failed"); if (result) result->pso_failed = true; continue;
            }
            if (chroma_pass == kChromaFrame) {
                // Snapshot cadence + scrub reset mirror the desktop FrameRing.
                float spacing = fmaxf(0.01f, chroma_param(fx, "chroma_frame_spacing", 0.12f));
                if (window_t < cst->last_t - 0.001 || window_t - cst->last_t > spacing * 8.0) {
                    cst->head = -1; cst->count = 0; cst->last_t = -1e9;
                }
                if (cst->head < 0 || window_t - cst->last_t >= spacing) {
                    cst->head = (cst->head + 1) % kChromaRingLayers;
                    if (cst->count < kChromaRingLayers) cst->count++;
                    cst->last_t = window_t;
                    bool matte_mode = chroma_param(fx, "matte_key", 0.f) > 0.5f && matte;
                    id<MTLRenderPipelineState> snap =
                        matte_mode ? get_chroma_pso(kChromaSnapshot) : nil;
                    if (snap) {
                        // Matte mode: pre-masked snapshot (rgb = frame,
                        // a = subject matte) so taps composite only the subject.
                        MTLRenderPassDescriptor* rs = [MTLRenderPassDescriptor new];
                        rs.colorAttachments[0].texture     = cst->ring;
                        rs.colorAttachments[0].slice       = cst->head;
                        rs.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
                        rs.colorAttachments[0].storeAction = MTLStoreActionStore;
                        id<MTLRenderCommandEncoder> es = [cb renderCommandEncoderWithDescriptor:rs];
                        [es setRenderPipelineState:snap];
                        [es setFragmentTexture:cur atIndex:0];
                        [es setFragmentTexture:matte atIndex:1];
                        [es drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                        [es endEncoding];
                    } else {
                        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                        [bl copyFromTexture:cur sourceSlice:0 sourceLevel:0
                               sourceOrigin:MTLOriginMake(0, 0, 0)
                                 sourceSize:MTLSizeMake(sw, sh, 1)
                                  toTexture:cst->ring destinationSlice:cst->head
                           destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
                        [bl endEncoding];
                    }
                }
            }
        } else if (is_body) {
            if (fx.body_fx_type == "Remove Background") {
                // The cutout is the person-matte composite itself.
                if (matte) { if (result) result->matte_cutout = true; set_status(i, "matte_cutout"); }
                else       set_status(i, "no_matte");
                continue;
            }
            if (fx.body_pass < 0) { set_status(i, "unknown_body_fx"); continue; }
            if (!matte)           { set_status(i, "no_matte");        continue; }
            pso = get_body_pso(fx.body_pass);
            if (!pso) { set_status(i, "pso_failed"); if (result) result->pso_failed = true; continue; }
        } else {
            FxProgram* p = get_fx_program(fx.fx_type);
            if (!p) {
                bool known = g_manifest.count(fx.fx_type) != 0;
                set_status(i, known ? "pso_failed" : "unknown_fx");
                if (known && result) result->pso_failed = true;
                continue;
            }
            pso = p->pso; mf = p->m;
        }

        MTLRenderPassDescriptor* rpF = [MTLRenderPassDescriptor new];
        rpF.colorAttachments[0].texture     = ping[dst];
        rpF.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
        rpF.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rpF];
        [e setRenderPipelineState:pso];
        if (cst) {
            ChromaUniCPU u = chroma_uniforms(fx, chroma_pass, sw, sh);
            if (chroma_pass == kChromaFrame) {
                u.head  = (float)cst->head;
                u.ntaps = fminf(u.ntaps, (float)cst->count);
                [e setFragmentTexture:cst->ring atIndex:1];
            } else {
                // First frame: the feedback slot is garbage — read the live
                // frame instead (trail == cur, i.e. no ghost yet).
                [e setFragmentTexture:(cst->primed ? cst->feedback : cur) atIndex:1];
            }
            // texture(2) is statically referenced by the shader — bind cur as
            // a harmless stand-in when no matte exists (colour-key mode).
            [e setFragmentTexture:(matte ? matte : cur) atIndex:2];
            [e setFragmentBytes:&u length:sizeof(u) atIndex:0];
            [e setFragmentTexture:cur atIndex:0];
        } else if (is_body) {
            BodyUniCPU u = body_uniforms(fx, sw, sh, anim_t);
            [e setFragmentBytes:&u length:sizeof(u) atIndex:0];
            [e setFragmentTexture:cur atIndex:0];
            [e setFragmentTexture:matte atIndex:1];
        } else {
            fill_params(*mf, fx, sw, sh, anim_t, pbuf);
            if (!pbuf.empty()) [e setFragmentBytes:pbuf.data() length:pbuf.size() atIndex:0];
            [e setFragmentTexture:cur atIndex:0];
            [e setFragmentSamplerState:g_fx_sampler atIndex:0];
        }
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
        id<MTLTexture> post = ping[dst]; dst ^= 1;
        if (cst && chroma_pass != kChromaFrame) {
            // Persist this pass's output as next frame's feedback (the desktop
            // reads last frame's slot output the same way).
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
            [bl copyFromTexture:post sourceSlice:0 sourceLevel:0
                   sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(sw, sh, 1)
                      toTexture:cst->feedback destinationSlice:0
               destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
            [bl endEncoding];
            cst->primed = true;
        }
        if (fx.amount < 0.999f && g_fx_blend_pso) {           // wet/dry blend
            MTLRenderPassDescriptor* rpB = [MTLRenderPassDescriptor new];
            rpB.colorAttachments[0].texture     = ping[dst];
            rpB.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
            rpB.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> eb = [cb renderCommandEncoderWithDescriptor:rpB];
            [eb setRenderPipelineState:g_fx_blend_pso];
            struct { float amt; } bu = { fx.amount };
            [eb setFragmentBytes:&bu length:sizeof(bu) atIndex:0];
            [eb setFragmentTexture:cur atIndex:0];
            [eb setFragmentTexture:post atIndex:1];
            [eb setFragmentSamplerState:g_fx_sampler atIndex:0];
            [eb drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [eb endEncoding];
            cur = ping[dst]; dst ^= 1;
        } else cur = post;
        set_status(i, "applied");
        if (result) result->applied++;
    }
    return cur;
}

// Parse the ordered FX stack pushed by set_live_fx (JSON array of {fx_type,params}).
// Pushed every frame from pms_render; re-parses only when the payload changes.
void metal_render_set_live_fx_stack(const char* json_utf8) {
    static std::string s_last;
    std::string incoming = json_utf8 ? json_utf8 : "";
    if (incoming == s_last) return;
    s_last = incoming;
    std::vector<LiveFx> next;
    if (json_utf8 && *json_utf8) {
        try {
            fxjson arr = fxjson::parse(json_utf8);
            for (const auto& e : arr) {
                LiveFx fx; fx.fx_type = e.value("fx_type", std::string());
                if (fx.fx_type.empty()) continue;
                fx.start = e.value("start", -1e30f);        // brick span (default = always on)
                fx.end   = e.value("end",    1e30f);
                if (fx.fx_type == "body_fx") {                 // matte-consuming pass
                    fx.body_fx_type = e.value("body_fx_type", std::string());
                    fx.body_pass    = body_pass_index(fx.body_fx_type);
                }
                if (fx.fx_type == "face_fx")                   // makeup look
                    fx.face_makeup_tex = e.value("face_makeup_tex", std::string());
                if (e.contains("params") && e["params"].is_object())
                    for (auto it = e["params"].begin(); it != e["params"].end(); ++it)
                        if (it.value().is_number()) fx.params[it.key()] = it.value().get<float>();
                auto a = fx.params.find("amount");
                if (a != fx.params.end()) fx.amount = a->second;
                next.push_back(std::move(fx));
            }
        } catch (...) { NSLog(@"[fx] stack parse failed"); }
    }
    std::lock_guard<std::mutex> lk(g_stack_mu);
    g_stack = std::move(next);
}

void metal_render_set_shader_dir(const char* dir) { g_shader_dir = dir ? dir : ""; }

// Timeline time of the current content frame — FX apply only within [start,end].
void metal_render_set_content_time(double t) { g_content_time = t; }

// Block until the GPU has finished all committed frames — for offline export
// (render a frame, wait, read it back, encode). g_queue is in-order, so an empty
// command buffer committed + waited flushes the render chain.
void metal_render_wait(void) {
    if (!g_queue) return;
    id<MTLCommandBuffer> cb = [g_queue commandBuffer];
    [cb commit]; [cb waitUntilCompleted];
}

// Diagnostic snapshot of the FX runner (for the `fx_debug` IPC command).
const char* metal_render_fx_debug() {
    static std::string s;
    fxjson j;
    j["manifest_count"] = (int)g_manifest.size();
    j["has_content"]    = (g_content != nil);
    { std::lock_guard<std::mutex> lk(g_content_mu);
      j["has_matte"]     = (g_matte_tex != nil);
      j["matte_pso_ok"]  = (g_matte_pso != nil);
      if (g_matte_tex) j["matte_time"] = g_matte_time; }
    fxjson stk = fxjson::array();
    { std::lock_guard<std::mutex> lk(g_stack_mu);
      for (size_t i = 0; i < g_stack.size(); ++i) {
          const auto& f = g_stack[i];
          fxjson e;
          e["fx_type"] = f.fx_type;
          e["amount"]  = f.amount;
          if (f.fx_type == "body_fx") {
              e["body_fx_type"] = f.body_fx_type;
              e["known"]        = (f.body_pass >= 0);        // false = skipped, never ran
              e["pso_ok"]       = (get_body_pso(f.body_pass) != nil);
          } else {
              e["in_manifest"] = (g_manifest.find(f.fx_type) != g_manifest.end());
              e["pso_ok"]      = (get_fx_program(f.fx_type) != nullptr);
          }
          // Pass status of the last rendered frame (index-aligned; may lag one
          // stack update): applied / out_of_window / no_matte / unknown_body_fx
          // / unknown_fx / pso_failed / no_content.
          if (i < g_pass_status.size()) e["status"] = g_pass_status[i];
          stk.push_back(e);
      } }
    j["stack"] = stk;
    { std::lock_guard<std::mutex> lk(g_content_mu);
      j["layer_frames"] = (int)g_layers.size(); }
    // Scene-compositor diagnostics of the last rendered frame (active=false
    // when the legacy single-content path ran instead).
    { std::lock_guard<std::mutex> lk(g_stack_mu);
      j["scene"] = g_scene_debug.is_null() ? fxjson{{"active", false}} : g_scene_debug; }
    s = j.dump();
    return s.c_str();
}

void metal_render_init(void* mtl_device) {
    if (g_dev) return;
    g_dev = (__bridge id<MTLDevice>)mtl_device;
    if (!g_dev) return;
    g_queue = [g_dev newCommandQueue];

    NSError* err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:kSrc options:nil error:&err];
    if (!lib) { NSLog(@"[metal_render] library: %@", err); return; }

    MTLRenderPipelineDescriptor* bg = [MTLRenderPipelineDescriptor new];
    bg.vertexFunction   = [lib newFunctionWithName:@"bg_v"];
    bg.fragmentFunction = [lib newFunctionWithName:@"bg_f"];
    bg.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_bg_pso = [g_dev newRenderPipelineStateWithDescriptor:bg error:&err];
    if (!g_bg_pso) NSLog(@"[metal_render] bg pipeline: %@", err);

    MTLRenderPipelineDescriptor* q = [MTLRenderPipelineDescriptor new];
    q.vertexFunction   = [lib newFunctionWithName:@"quad_v"];
    q.fragmentFunction = [lib newFunctionWithName:@"quad_f"];
    q.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_quad_pso = [g_dev newRenderPipelineStateWithDescriptor:q error:&err];
    if (!g_quad_pso) NSLog(@"[metal_render] quad pipeline: %@", err);

    // Matte composite blit — alpha blending so the person (matte=1) keeps the
    // content pixel and the background (matte=0) keeps the engine background
    // already rendered into the target.
    MTLRenderPipelineDescriptor* mq = [MTLRenderPipelineDescriptor new];
    mq.vertexFunction   = [lib newFunctionWithName:@"quad_v"];
    mq.fragmentFunction = [lib newFunctionWithName:@"matte_f"];
    mq.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    mq.colorAttachments[0].blendingEnabled             = YES;
    mq.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorSourceAlpha;
    mq.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
    mq.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorOne;
    mq.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    g_matte_pso = [g_dev newRenderPipelineStateWithDescriptor:mq error:&err];
    if (!g_matte_pso) NSLog(@"[metal_render] matte pipeline: %@", err);

    // FX runner: shared fullscreen vertex, wet/dry blend PSO, sampler, manifest.
    g_fs_v = [lib newFunctionWithName:@"fs_v"];

    // Scene layer pipelines — premultiplied over (src=1, dst=1-srcAlpha).
    auto premul = [](MTLRenderPipelineDescriptor* d) {
        d.colorAttachments[0].pixelFormat                = MTLPixelFormatBGRA8Unorm;
        d.colorAttachments[0].blendingEnabled            = YES;
        d.colorAttachments[0].sourceRGBBlendFactor       = MTLBlendFactorOne;
        d.colorAttachments[0].destinationRGBBlendFactor  = MTLBlendFactorOneMinusSourceAlpha;
        d.colorAttachments[0].sourceAlphaBlendFactor     = MTLBlendFactorOne;
        d.colorAttachments[0].destinationAlphaBlendFactor= MTLBlendFactorOneMinusSourceAlpha;
    };
    MTLRenderPipelineDescriptor* ld = [MTLRenderPipelineDescriptor new];
    ld.vertexFunction   = [lib newFunctionWithName:@"layer_v"];
    ld.fragmentFunction = [lib newFunctionWithName:@"layer_f"];
    premul(ld);
    g_layer_pso = [g_dev newRenderPipelineStateWithDescriptor:ld error:&err];
    if (!g_layer_pso) NSLog(@"[metal_render] layer pipeline: %@", err);

    MTLRenderPipelineDescriptor* lm = [MTLRenderPipelineDescriptor new];
    lm.vertexFunction   = [lib newFunctionWithName:@"layer_v"];
    lm.fragmentFunction = [lib newFunctionWithName:@"layer_matte_f"];
    premul(lm);
    g_layer_matte_pso = [g_dev newRenderPipelineStateWithDescriptor:lm error:&err];
    if (!g_layer_matte_pso) NSLog(@"[metal_render] layer-matte pipeline: %@", err);

    MTLRenderPipelineDescriptor* sq = [MTLRenderPipelineDescriptor new];
    sq.vertexFunction   = g_fs_v;
    sq.fragmentFunction = [lib newFunctionWithName:@"solid_f"];
    premul(sq);
    g_solid_pso = [g_dev newRenderPipelineStateWithDescriptor:sq error:&err];
    if (!g_solid_pso) NSLog(@"[metal_render] solid pipeline: %@", err);
    MTLRenderPipelineDescriptor* bl = [MTLRenderPipelineDescriptor new];
    bl.vertexFunction   = g_fs_v;
    bl.fragmentFunction = [lib newFunctionWithName:@"blend_f"];
    bl.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_fx_blend_pso = [g_dev newRenderPipelineStateWithDescriptor:bl error:&err];
    if (!g_fx_blend_pso) NSLog(@"[metal_render] blend pipeline: %@", err);

    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterLinear; sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge; sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    g_fx_sampler = [g_dev newSamplerStateWithDescriptor:sd];

    load_manifest();

    CVMetalTextureCacheCreate(kCFAllocatorDefault, NULL, g_dev, NULL, &g_texcache);
}

// Release any zero-copy CVMetalTexture mapping (call under g_content_mu).
static void release_cvtex() {
    if (g_cvtex) { CFRelease(g_cvtex); g_cvtex = NULL; }
}

// Release the stored matte (call under g_content_mu).
static void release_matte() {
    if (g_matte_cvtex) { CFRelease(g_matte_cvtex); g_matte_cvtex = NULL; }
    if (g_matte_pb)    { CVPixelBufferRelease(g_matte_pb); g_matte_pb = NULL; }
    g_matte_tex = nil;
}

// Release one stored layer frame's CoreVideo refs (call under g_content_mu).
static void release_layer(LayerFrame& lf) {
    if (lf.cvtex) { CFRelease(lf.cvtex); lf.cvtex = NULL; }
    if (lf.pb)    { CVPixelBufferRelease(lf.pb); lf.pb = NULL; }
    lf.tex = nil;
}

// Store one visual layer's frame keyed by (track, clip) — BGRA CVPixelBuffer
// mapped zero-copy like the content frame. Retains the buffer (releases the
// previous frame at that key); NULL clears the key. Thread-safe.
void metal_render_submit_layer(int track, int clip, void* cv_pixel_buffer_bgra,
                               int rotation_quarter_turns, double host_time_seconds) {
    std::pair<int,int> key{track, clip};
    if (!cv_pixel_buffer_bgra) {
        std::lock_guard<std::mutex> lk(g_content_mu);
        auto it = g_layers.find(key);
        if (it != g_layers.end()) { release_layer(it->second); g_layers.erase(it); }
        return;
    }
    if (!g_texcache) return;
    CVPixelBufferRef pb = (CVPixelBufferRef)cv_pixel_buffer_bgra;
    int w = (int)CVPixelBufferGetWidth(pb);
    int h = (int)CVPixelBufferGetHeight(pb);
    CVMetalTextureRef cvt = NULL;
    CVReturn r = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, g_texcache, pb, NULL,
        MTLPixelFormatBGRA8Unorm, w, h, 0, &cvt);
    if (r != kCVReturnSuccess || !cvt) {
        if (cvt) CFRelease(cvt);
        NSLog(@"[metal_render] layer (%d,%d) map failed (%d)", track, clip, (int)r);
        return;
    }
    std::lock_guard<std::mutex> lk(g_content_mu);
    LayerFrame& lf = g_layers[key];
    release_layer(lf);
    CVPixelBufferRetain(pb);
    lf.pb    = pb;
    lf.cvtex = cvt;
    lf.tex   = CVMetalTextureGetTexture(cvt);
    lf.w = w; lf.h = h;
    lf.rot = ((rotation_quarter_turns % 4) + 4) % 4;
    lf.t   = host_time_seconds;
    if (host_time_seconds >= 0.0) {           // latest submission wins (seeks go backward too)
        g_scene_clock = host_time_seconds;
        g_scene_clock_valid = true;
    }
}

// Camera → face-tracker side-feed: stride-2 BGRA→RGB copy to the worker,
// gated by face_feed_enabled() so plain recording pays nothing.
void metal_render_face_feed(void* cv_pixel_buffer_bgra) {
    if (!cv_pixel_buffer_bgra || !face_feed_enabled()) return;
    CVPixelBufferRef pb = (CVPixelBufferRef)cv_pixel_buffer_bgra;
    if (CVPixelBufferGetPixelFormatType(pb) != kCVPixelFormatType_32BGRA) return;
    if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
    const uint8_t* base = (const uint8_t*)CVPixelBufferGetBaseAddress(pb);
    const size_t   bpr  = CVPixelBufferGetBytesPerRow(pb);
    const int fw = (int)CVPixelBufferGetWidth(pb);
    const int fh = (int)CVPixelBufferGetHeight(pb);
    const int step = (fw > 720 || fh > 720) ? 2 : 1;   // 720x1280 → 360x640
    const int dw = fw / step, dh = fh / step;
    static thread_local std::vector<uint8_t> rgb;
    rgb.resize((size_t)dw * dh * 3);
    for (int y = 0; y < dh; ++y) {
        const uint8_t* row = base + (size_t)y * step * bpr;
        uint8_t* dst = rgb.data() + (size_t)y * dw * 3;
        for (int x = 0; x < dw; ++x) {
            const uint8_t* px = row + (size_t)x * step * 4;   // B G R A
            dst[x*3+0] = px[2]; dst[x*3+1] = px[1]; dst[x*3+2] = px[0];
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    face_track_submit(rgb.data(), dw, dh);
}

// Drop every stored layer frame + invalidate the scene clock (record mode:
// the camera path must not be shadowed by stale timeline frames).
void metal_render_clear_layers(void) {
    std::lock_guard<std::mutex> lk(g_content_mu);
    for (auto& kv : g_layers) release_layer(kv.second);
    g_layers.clear();
    g_scene_clock_valid = false;
}

// Store the latest person matte — an R8 (OneComponent8) CVPixelBufferRef from
// the platform segmenter. Retains the buffer (releases the previous); NULL
// clears. Mapped zero-copy through the same texture cache as the content frame.
void metal_render_submit_matte(void* cv_pixel_buffer_r8, double t) {
    if (!cv_pixel_buffer_r8) {
        std::lock_guard<std::mutex> lk(g_content_mu);
        release_matte();
        return;
    }
    if (!g_texcache) return;
    CVPixelBufferRef pb = (CVPixelBufferRef)cv_pixel_buffer_r8;
    int w = (int)CVPixelBufferGetWidth(pb);
    int h = (int)CVPixelBufferGetHeight(pb);
    CVMetalTextureRef cvt = NULL;
    CVReturn r = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, g_texcache, pb, NULL,
        MTLPixelFormatR8Unorm, w, h, 0, &cvt);
    if (r != kCVReturnSuccess || !cvt) {
        if (cvt) CFRelease(cvt);
        NSLog(@"[metal_render] matte map failed (%d)", (int)r);
        return;
    }
    std::lock_guard<std::mutex> lk(g_content_mu);
    release_matte();
    CVPixelBufferRetain(pb);
    g_matte_pb    = pb;
    g_matte_cvtex = cvt;
    g_matte_tex   = CVMetalTextureGetTexture(cvt);
    g_matte_time  = t;
}

void metal_render_set_content_bgra(const void* bgra, int w, int h) {
    std::lock_guard<std::mutex> lk(g_content_mu);
    release_cvtex();
    if (!g_dev || !bgra || w <= 0 || h <= 0) { g_content = nil; g_cw = g_ch = 0; return; }
    if (!g_content || g_cw != w || g_ch != h || g_content.pixelFormat != MTLPixelFormatBGRA8Unorm) {
        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        g_content = [g_dev newTextureWithDescriptor:td];
        g_cw = w; g_ch = h;
    }
    [g_content replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0
                   withBytes:bgra bytesPerRow:(NSUInteger)w * 4];
}

void metal_render_submit_pixelbuffer(void* cv_pixel_buffer) {
    if (!cv_pixel_buffer) {          // clear back to the aurora
        std::lock_guard<std::mutex> lk(g_content_mu);
        release_cvtex(); g_content = nil; g_cw = g_ch = 0; return;
    }
    if (!g_texcache) return;
    CVPixelBufferRef pb = (CVPixelBufferRef)cv_pixel_buffer;
    int w = (int)CVPixelBufferGetWidth(pb);
    int h = (int)CVPixelBufferGetHeight(pb);

    // Zero-copy: map the IOSurface-backed pixel buffer straight to an MTLTexture
    // (no CPU copy/upload) — the fix for 1080p/4K choppiness.
    CVMetalTextureRef cvt = NULL;
    CVReturn r = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, g_texcache, pb, NULL,
        MTLPixelFormatBGRA8Unorm, w, h, 0, &cvt);
    if (r != kCVReturnSuccess || !cvt) { if (cvt) CFRelease(cvt); return; }

    std::lock_guard<std::mutex> lk(g_content_mu);
    release_cvtex();
    g_cvtex   = cvt;                                   // retain until next frame / render
    g_content = CVMetalTextureGetTexture(cvt);
    g_cw = w; g_ch = h;
    CVMetalTextureCacheFlush(g_texcache, 0);
}

// ── Scene FX stacks from AppState ────────────────────────────────────────────
// The scene compositor derives its FX directly from the timeline (the desktop
// collect_glass_* / collect_*_for_track model), replacing the set_live_fx flat
// adapter. Generated-MSL effects resolve by shader id; the desktop's
// hand-written GLSL FX (grade/glitch/vhs/…) have no MSL port yet — they still
// produce entries so fx_debug reports them as unknown_fx, never a silent skip.

static std::string gen_fx_id(FXType t) {
    for (int i = 0; i < k_gen_fx_count; ++i)
        if (k_gen_fx_types[i] == t) return k_gen_fx_names[i];
    return std::string();
}

// Hand-written FX ids (fx_type_str parity, video kinds only) — used so the
// runner can name what it cannot run yet in fx_debug.
static const char* named_fx_id(FXType t) {
    switch (t) {
        case FXType::Grade:       return "grade";
        case FXType::Blur:        return "blur";
        case FXType::Vignette:    return "vignette";
        case FXType::Glitch:      return "glitch";
        case FXType::ZoomPunch:   return "zoom_punch";
        case FXType::LUT:         return "lut";
        case FXType::LightLeak:   return "light_leak";
        case FXType::VHS:         return "vhs";
        case FXType::Datamosh:    return "datamosh";
        case FXType::ChromaKey:   return "chroma_key";
        case FXType::ChromaMelt:  return "chroma_melt";
        case FXType::ChromaEcho:  return "chroma_echo";
        case FXType::ChromaFrame: return "chroma_frame";
        case FXType::KenBurns:    return "ken_burns";
        default:                  return "unknown";
    }
}

// One Effect brick / MultiFX sub-effect → one LiveFx entry. Params are read
// through eval_prop at the playhead so keyframed bricks animate (desktop
// accumulation parity); names are the manifest's bare param names.
static void scene_push_fx(std::vector<LiveFx>& out, const Clip& cl, float t) {
    LiveFx fx;
    std::string gid = gen_fx_id(cl.fx_type);
    if (!gid.empty()) {
        fx.fx_type = gid;
        static Clip s_probe;   // validates field↔effect ownership, like ipc_server
        const std::string prefix = "fx_" + gid + "_";
        for (int i = 0; i < kClipKfFieldCount; ++i) {
            const char* n = kClipKfFields[i].name;
            if (strncmp(n, prefix.c_str(), prefix.size()) != 0) continue;
            std::string pname = n + prefix.size();
            if (!fx_clip_set_param(s_probe, gid, pname, 0.f)) continue;
            fx.params[pname] = cl.eval_prop(n, t);
        }
    } else {
        // Legacy hand-wired FX: emit the same param names effect_params_to_json
        // uses (ipc_server.cpp), which are also the manifest/uniform names the
        // Metal ports of these passes expect. Fields read directly — legacy FX
        // params are not keyframable.
        fx.fx_type = named_fx_id(cl.fx_type);
        switch (cl.fx_type) {
            case FXType::Grade:
                fx.params["brightness"] = cl.fx_brightness;
                fx.params["contrast"]   = cl.fx_contrast;
                fx.params["saturation"] = cl.fx_saturation;
                fx.params["hue"]        = cl.fx_hue;
                break;
            case FXType::Blur:     fx.params["blur"]     = cl.fx_blur;     break;
            case FXType::Vignette: fx.params["vignette"] = cl.fx_vignette; break;
            case FXType::Glitch:
                fx.params["glitch_chroma"]           = cl.fx_glitch_chroma;
                fx.params["glitch_jitter"]           = cl.fx_glitch_jitter;
                fx.params["glitch_corruption"]       = cl.fx_glitch_corruption;
                fx.params["glitch_corruption_bleed"] = cl.fx_glitch_corruption_bleed;
                break;
            case FXType::VHS:
                fx.params["vhs_noise"]    = cl.fx_vhs_noise;
                fx.params["vhs_bleed"]    = cl.fx_vhs_bleed;
                fx.params["vhs_tracking"] = cl.fx_vhs_tracking;
                break;
            case FXType::LightLeak:
                fx.params["leak_intensity"] = cl.fx_leak_intensity;
                fx.params["leak_speed"]     = cl.fx_leak_speed;
                break;
            case FXType::Datamosh:
                fx.params["datamosh_intensity"] = cl.fx_datamosh_intensity;
                fx.params["datamosh_spread"]    = cl.fx_datamosh_spread;
                break;
            case FXType::ChromaKey:
                fx.params["chroma_key_r"]         = cl.fx_chroma_key_r;
                fx.params["chroma_key_g"]         = cl.fx_chroma_key_g;
                fx.params["chroma_key_b"]         = cl.fx_chroma_key_b;
                fx.params["chroma_key_threshold"] = cl.fx_chroma_key_threshold;
                fx.params["chroma_key_softness"]  = cl.fx_chroma_key_softness;
                break;
            case FXType::ChromaMelt:
                fx.params["chroma_melt_r"]         = cl.fx_chroma_melt_r;
                fx.params["chroma_melt_g"]         = cl.fx_chroma_melt_g;
                fx.params["chroma_melt_b"]         = cl.fx_chroma_melt_b;
                fx.params["chroma_melt_threshold"] = cl.fx_chroma_melt_threshold;
                fx.params["chroma_melt_persist"]   = cl.fx_chroma_melt_persist;
                break;
            case FXType::ChromaEcho:
                fx.params["chroma_echo_r"]         = cl.fx_chroma_echo_r;
                fx.params["chroma_echo_g"]         = cl.fx_chroma_echo_g;
                fx.params["chroma_echo_b"]         = cl.fx_chroma_echo_b;
                fx.params["chroma_echo_threshold"] = cl.fx_chroma_echo_threshold;
                fx.params["chroma_echo_persist"]   = cl.fx_chroma_echo_persist;
                break;
            case FXType::ChromaFrame:
                fx.params["chroma_frame_r"]         = cl.fx_chroma_frame_r;
                fx.params["chroma_frame_g"]         = cl.fx_chroma_frame_g;
                fx.params["chroma_frame_b"]         = cl.fx_chroma_frame_b;
                fx.params["chroma_frame_threshold"] = cl.fx_chroma_frame_threshold;
                fx.params["chroma_frame_taps"]      = cl.fx_chroma_frame_taps;
                fx.params["chroma_frame_spacing"]   = cl.fx_chroma_frame_spacing;
                fx.params["chroma_frame_falloff"]   = cl.fx_chroma_frame_falloff;
                break;
            default: break;
        }
    }
    auto a = fx.params.find("amount");
    if (a != fx.params.end()) fx.amount = a->second;
    out.push_back(std::move(fx));
}

// One BodyFX brick / sub-effect → one body_fx entry (positional p0..p3 params,
// the exact fallback names body_uniforms resolves).
static void scene_push_body(std::vector<LiveFx>& out, const Clip& cl, float t) {
    LiveFx fx;
    fx.fx_type = "body_fx";
    const BodyFXInfo* info = body_fx_find_info(cl.body_fx_type);
    fx.body_fx_type = info ? info->name : "None";
    fx.body_pass    = body_pass_index(fx.body_fx_type);
    fx.params["p0"] = cl.body_fx_params[0];
    fx.params["p1"] = cl.body_fx_params[1];
    fx.params["p2"] = cl.body_fx_params[2];
    fx.params["p3"] = cl.body_fx_params[3];
    fx.amount = cl.eval_prop("body_fx_amount", t);
    out.push_back(std::move(fx));
}

// Glass stack for a video layer on track ti at playhead t: the host clip's own
// grade fields, coupled Effect/MultiFX bricks (fx_clip_is_glass), then the
// matte body passes (standalone BodyFX bricks — desktop applies all active
// ones per-clip — and BodyFX sub-effects of glass MultiFX bricks).
static std::vector<LiveFx> scene_glass_stack(const AppState& st, int ti, float t) {
    std::vector<LiveFx> out, body;
    if (ti < 0 || ti >= (int)st.tracks.size()) return out;
    const auto& clips = st.tracks[ti].clips;
    for (const Clip& cl : clips) {   // host clip grade (collect_glass_effects tail)
        if (cl.clip_type != ClipType::Video && cl.clip_type != ClipType::Background) continue;
        if (t < cl.start || t >= cl.end) continue;
        if (cl.grade_brightness != 0.f || cl.grade_contrast != 1.f ||
            cl.grade_saturation != 1.f || cl.grade_hue      != 0.f) {
            LiveFx fx; fx.fx_type = "grade";
            fx.params["brightness"] = cl.grade_brightness;
            fx.params["contrast"]   = cl.grade_contrast;
            fx.params["saturation"] = cl.grade_saturation;
            fx.params["hue"]        = cl.grade_hue;
            out.push_back(std::move(fx));
        }
        break;
    }
    for (const Clip& cl : clips) {
        if (t < cl.start || t >= cl.end) continue;
        if (cl.clip_type == ClipType::Effect) {
            if (fx_brick_is_audio_kind(cl)) continue;
            if (!fx_clip_is_glass(st, ti, cl)) continue;
            scene_push_fx(out, cl, t);
        } else if (cl.clip_type == ClipType::MultiFX) {
            if (!fx_clip_is_glass(st, ti, cl)) continue;
            float rel = t - cl.start, dur = cl.end - cl.start;
            for (const Clip& se : cl.fx_chain) {
                float se_end = (se.rel_end <= 0.f) ? dur : se.rel_end;
                if (rel < se.rel_start || rel >= se_end) continue;
                if (se.clip_type == ClipType::BodyFX) scene_push_body(body, se, t);
                else                                  scene_push_fx(out, se, t);
            }
        } else if (cl.clip_type == ClipType::BodyFX) {
            scene_push_body(body, cl, t);
        }
    }
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// Bus stack for track ti: uncoupled Effect/MultiFX bricks — the desktop's
// scene_apply_fx-per-track semantics (they filter the composite of everything
// below + this track), gated by their span.
static std::vector<LiveFx> scene_bus_stack(const AppState& st, int ti, float t) {
    std::vector<LiveFx> out;
    if (ti < 0 || ti >= (int)st.tracks.size()) return out;
    for (const Clip& cl : st.tracks[ti].clips) {
        if (t < cl.start || t >= cl.end) continue;
        if (cl.clip_type == ClipType::Effect) {
            if (fx_brick_is_audio_kind(cl)) continue;
            if (fx_clip_is_glass(st, ti, cl)) continue;
            scene_push_fx(out, cl, t);
        } else if (cl.clip_type == ClipType::MultiFX) {
            if (fx_clip_is_glass(st, ti, cl)) continue;
            float rel = t - cl.start, dur = cl.end - cl.start;
            for (const Clip& se : cl.fx_chain) {
                float se_end = (se.rel_end <= 0.f) ? dur : se.rel_end;
                if (rel < se.rel_start || rel >= se_end) continue;
                if (se.clip_type == ClipType::BodyFX) scene_push_body(out, se, t);
                else                                  scene_push_fx(out, se, t);
            }
        }
    }
    return out;
}

static const char* scene_clip_type_str(ClipType t) {
    switch (t) {
        case ClipType::Video:       return "video";
        case ClipType::VideoRecord: return "video_record";
        case ClipType::Background:  return "background";
        case ClipType::Text:        return "text";
        case ClipType::Lyrics:      return "lyrics";
        case ClipType::Subtitle:    return "subtitle";
        default:                    return "other";
    }
}

// CPU mirror of the MSL LayerUni (plain floats, no alignment traps).
struct LayerUniCPU {
    float cx, cy, hw, hh, cs, sn, cw, ch;
    float u0, v0, u1, v1, opacity, quarters, pad0, pad1;
};

// ── Scene compositor — the desktop canvas.cpp Pass-1 track loop ──────────────
// Tracks walk LAST→0 (bottom lane = deepest). Per track: background clip,
// video clip (+transitions) with per-layer glass FX + body passes, text
// layers, then uncoupled bricks as a bus over the accumulated scene.
static int render_scene(id<MTLCommandBuffer> cb, id<MTLTexture> target, int w, int h,
                        double wall_t, const AppState& st,
                        const std::map<std::pair<int,int>, LayerFrame>& layers,
                        id<MTLTexture> matte) {
    // FX windows + layer activity + keyframes run on the FRAME clock (latest
    // submitted frame time), not state.playhead — see g_scene_clock.
    const float t = g_scene_clock_valid ? (float)g_scene_clock : st.playhead;
    int rc = 0;
    fxjson drawn = fxjson::array(), skipped = fxjson::array(), notes = fxjson::array();
    int glass_n = 0, bus_n = 0, trans_n = 0;

    // Scene accumulation targets (ping-pair so bus FX can rewrite the scene).
    if (!g_scene[0] || g_scene_w != w || g_scene_h != h) {
        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        g_scene[0] = [g_dev newTextureWithDescriptor:td];
        g_scene[1] = [g_dev newTextureWithDescriptor:td];
        g_scene_w = w; g_scene_h = h;
    }
    if (!g_scene[0] || !g_scene[1] || !g_layer_pso) {
        static bool s_logged = false;
        if (!s_logged) { s_logged = true; NSLog(@"[scene] targets/layer pso unavailable — aurora only"); }
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor new];
        rp.colorAttachments[0].texture     = target;
        rp.colorAttachments[0].loadAction  = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor  = MTLClearColorMake(0.02, 0.02, 0.03, 1.0);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:g_bg_pso];
        struct { float t; float rx, ry; } bgu = { (float)wall_t, (float)w, (float)h };
        [enc setFragmentBytes:&bgu length:sizeof(bgu) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        std::lock_guard<std::mutex> lk(g_stack_mu);
        g_scene_debug = {{"active", true}, {"error", "no_scene_targets_or_layer_pso"}};
        return 3;
    }
    int cur = 0;

    // Pass 0: aurora background into the scene.
    {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor new];
        rp.colorAttachments[0].texture     = g_scene[cur];
        rp.colorAttachments[0].loadAction  = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor  = MTLClearColorMake(0.02, 0.02, 0.03, 1.0);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:g_bg_pso];
        struct { float t; float rx, ry; } bgu = { (float)wall_t, (float)w, (float)h };
        [enc setFragmentBytes:&bgu length:sizeof(bgu) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
    }

    // Record every non-applied FX status so nothing is silently skipped.
    auto note_statuses = [&](int ti, const std::vector<LiveFx>& stack,
                             const std::vector<std::string>& sts, const char* where) {
        for (size_t i = 0; i < stack.size() && i < sts.size(); ++i) {
            if (sts[i] == "applied" || sts[i] == "matte_cutout") continue;
            fxjson n = {{"track", ti}, {"where", where},
                        {"fx_type", stack[i].fx_type}, {"status", sts[i]}};
            if (!stack[i].body_fx_type.empty()) n["body_fx_type"] = stack[i].body_fx_type;
            notes.push_back(std::move(n));
        }
    };

    // Composite one layer frame into the scene with the clip's keyframed
    // transform, crop UV window, flips, aspect-fit, premultiplied over.
    auto draw_layer = [&](int ti, int ci, const Clip& cl, float at_time,
                          float alpha_mul, bool glass) {
        auto itl = layers.find({ti, ci});
        if (itl == layers.end() || !itl->second.tex) {
            skipped.push_back({{"track", ti}, {"clip", ci},
                               {"type", scene_clip_type_str(cl.clip_type)},
                               {"reason", "no_frame"}});
            return;
        }
        const LayerFrame& lf = itl->second;
        id<MTLTexture> tex = lf.tex;
        bool cutout = false;
        int applied_here = 0;
        if (glass) {
            std::vector<LiveFx> stack = scene_glass_stack(st, ti, t);
            if (!stack.empty()) {
                std::vector<std::string> sts; FxRunResult rr;
                tex = run_fx_stack(cb, tex, lf.w, lf.h, (ti << 12) | ci,
                                   stack, t, t, matte, &sts, &rr);
                glass_n += rr.applied; applied_here = rr.applied;
                cutout = rr.matte_cutout;
                if (rr.pso_failed) rc = 4;
                note_statuses(ti, stack, sts, "glass");
            }
        }
        float px    = cl.eval_prop("pos_x",    at_time);
        float py    = cl.eval_prop("pos_y",    at_time);
        float sx    = cl.eval_prop("scale_x",  at_time);
        float sy    = cl.eval_prop("scale_y",  at_time);
        float rot   = cl.eval_prop("rotation", at_time);
        float alpha = cl.eval_prop("opacity",  at_time) * alpha_mul;
        alpha = fmaxf(0.f, fminf(1.f, alpha));
        // Aspect-fit of the (rotated, cropped) source into the canvas.
        int effw = (lf.rot & 1) ? lf.h : lf.w;
        int effh = (lf.rot & 1) ? lf.w : lf.h;
        float fit_w = (float)w, fit_h = (float)h;
        if (effw > 0 && effh > 0) {
            float va = cl.cropped_aspect(effw, effh);
            float ca = (float)w / (float)h;
            if (va > ca) { fit_w = (float)w; fit_h = (float)w / va; }
            else         { fit_h = (float)h; fit_w = (float)h * va; }
        }
        // UV window (crop) + mirror/flip by swapping extents. Camera-record
        // layers render mirrored (front-cam self-view convention, desktop parity).
        float u0 = cl.crop_l, u1 = 1.f - cl.crop_r;
        float v0 = cl.crop_t, v1 = 1.f - cl.crop_b;
        bool mirror = (cl.clip_type == ClipType::VideoRecord);
        if (mirror)    { float tv = u0; u0 = u1; u1 = tv; }
        if (cl.flip_h) { float tv = u0; u0 = u1; u1 = tv; }
        if (cl.flip_v) { float tv = v0; v0 = v1; v1 = tv; }
        float rad = rot * 3.14159265f / 180.f;
        LayerUniCPU u = {
            px * (float)w, py * (float)h,
            fit_w * sx * 0.5f, fit_h * sy * 0.5f,
            cosf(rad), sinf(rad), (float)w, (float)h,
            u0, v0, u1, v1, alpha, (float)lf.rot, 0.f, 0.f };
        bool with_matte = cutout && matte;
        if (with_matte && !g_layer_matte_pso) {
            static bool s_logged = false;
            if (!s_logged) { s_logged = true; NSLog(@"[scene] layer-matte pso unavailable"); }
            notes.push_back({{"track", ti}, {"clip", ci}, {"status", "matte_pso_failed"}});
            rc = 3; with_matte = false;
        }
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor new];
        rp.colorAttachments[0].texture     = g_scene[cur];
        rp.colorAttachments[0].loadAction  = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:(with_matte ? g_layer_matte_pso : g_layer_pso)];
        [enc setVertexBytes:&u length:sizeof(u) atIndex:0];
        [enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
        [enc setFragmentTexture:tex atIndex:0];
        if (with_matte) [enc setFragmentTexture:matte atIndex:1];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [enc endEncoding];
        drawn.push_back({{"track", ti}, {"clip", ci},
                         {"type", scene_clip_type_str(cl.clip_type)},
                         {"glass_fx", applied_here}, {"alpha", alpha}});
    };

    // Full-canvas premultiplied solid (DipWhite veil).
    auto draw_solid = [&](float r, float g, float b, float a) {
        if (!g_solid_pso) {
            static bool s_logged = false;
            if (!s_logged) { s_logged = true; NSLog(@"[scene] solid pso unavailable"); }
            notes.push_back({{"status", "solid_pso_failed"}});
            rc = 3; return;
        }
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor new];
        rp.colorAttachments[0].texture     = g_scene[cur];
        rp.colorAttachments[0].loadAction  = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:g_solid_pso];
        struct { float r, g, b, a; } su = { r, g, b, a };
        [enc setFragmentBytes:&su length:sizeof(su) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
    };

    for (int ti = (int)st.tracks.size() - 1; ti >= 0; --ti) {
        const Track& track = st.tracks[ti];
        if (!track.visible) continue;

        // ── Background clip (first active) ─────────────────────────────────
        for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
            const Clip& cl = track.clips[ci];
            if (cl.clip_type != ClipType::Background) continue;
            if (t < cl.start || t >= cl.end) continue;
            if (layers.count({ti, ci}))
                draw_layer(ti, ci, cl, t, 1.f, /*glass=*/false);
            else   // procedural bg presets not ported — accepted v1 gap
                notes.push_back({{"track", ti}, {"clip", ci},
                                 {"status", "background_preset_gap"}});
            break;
        }

        // ── Video clip + transitions (canvas.cpp ~1880-1963) ───────────────
        {
            const Clip* active = nullptr; int active_ci = -1;
            for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
                const Clip& cl = track.clips[ci];
                if (clip_is_videolike_type(cl.clip_type) &&
                    t >= cl.start && t < cl.end)
                    { active = &cl; active_ci = ci; break; }
            }
            if (!active) {   // incoming clip inside the previous clip's post zone
                for (int ci = 1; ci < (int)track.clips.size(); ++ci) {
                    const Clip& prev = track.clips[ci - 1];
                    const Clip& cl   = track.clips[ci];
                    if (prev.clip_type != ClipType::Video || cl.clip_type != ClipType::Video) continue;
                    if (prev.transition_type == TransitionType::None || prev.transition_post <= 0.f) continue;
                    if (t >= cl.start && t < cl.start + prev.transition_post)
                        { active = &track.clips[ci]; active_ci = ci; break; }
                }
            }
            if (active) {
                bool in_trans_out = (active->transition_type != TransitionType::None &&
                                     active->transition_pre > 0.f &&
                                     t >= active->end - active->transition_pre);
                const Clip* next_cl = nullptr; int next_ci = -1;
                if (in_trans_out && active_ci + 1 < (int)track.clips.size()) {
                    const Clip& nc = track.clips[active_ci + 1];
                    if (nc.clip_type == ClipType::Video) { next_cl = &nc; next_ci = active_ci + 1; }
                }
                bool in_trans_in = false;
                const Clip* prev_cl = nullptr; int prev_ci = -1;
                if (!in_trans_out && active_ci > 0) {
                    const Clip& pc = track.clips[active_ci - 1];
                    if (pc.clip_type == ClipType::Video &&
                        pc.transition_type != TransitionType::None &&
                        pc.transition_post > 0.f &&
                        t < active->start + pc.transition_post) {
                        in_trans_in = true; prev_cl = &pc; prev_ci = active_ci - 1;
                    }
                }
                if (in_trans_out && next_cl) {
                    ++trans_n;
                    float pre = active->transition_pre, post = active->transition_post;
                    float cut = active->end;
                    float t_a = fmaxf(0.f, fminf(1.f, (t - (cut - pre)) / fmaxf(pre, 1e-5f)));
                    float t_b = fmaxf(0.f, fminf(1.f, (t - cut) / fmaxf(post, 1e-5f)));
                    if (active->transition_type == TransitionType::Dissolve) {
                        draw_layer(ti, active_ci, *active, t, 1.f - t_a, true);
                        draw_layer(ti, next_ci, *next_cl, t, t_b > 0.f ? t_b : t_a, true);
                    } else if (active->transition_type == TransitionType::FadeBlack) {
                        draw_layer(ti, active_ci, *active, t, 1.f - t_a, true);
                        draw_layer(ti, next_ci, *next_cl, t, t_b, true);
                    } else {   // DipWhite (and Shake fallback, desktop parity)
                        draw_layer(ti, active_ci, *active, t, 1.f - t_a, true);
                        float white_a = t_a * (1.f - t_b);
                        if (white_a > 0.01f) draw_solid(1.f, 1.f, 1.f, white_a);
                        draw_layer(ti, next_ci, *next_cl, t, t_b, true);
                    }
                } else if (in_trans_in && prev_cl) {
                    ++trans_n;
                    float tt = fmaxf(0.f, fminf(1.f,
                        (t - active->start) / fmaxf(prev_cl->transition_post, 1e-5f)));
                    if (prev_cl->transition_type == TransitionType::Dissolve) {
                        draw_layer(ti, prev_ci, *prev_cl,
                                   fminf(t, prev_cl->end - 1e-4f), 1.f - tt, true);
                        draw_layer(ti, active_ci, *active, t, tt, true);
                    } else if (prev_cl->transition_type == TransitionType::FadeBlack) {
                        draw_layer(ti, active_ci, *active, t, tt, true);
                    } else {   // DipWhite: veil fades out, clip B fades in
                        float white_a = 1.f - tt;
                        if (white_a > 0.01f) draw_solid(1.f, 1.f, 1.f, white_a);
                        draw_layer(ti, active_ci, *active, t, tt, true);
                    }
                } else {
                    draw_layer(ti, active_ci, *active, t, 1.f, true);
                }
            }
        }

        // ── Text layers (Swift-rasterized, submitted as layer frames) ───────
        for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
            const Clip& cl = track.clips[ci];
            if (cl.clip_type != ClipType::Text && cl.clip_type != ClipType::Lyrics &&
                cl.clip_type != ClipType::Subtitle) continue;
            if (t < cl.start || t >= cl.end) continue;
            draw_layer(ti, ci, cl, t, 1.f, /*glass=*/false);
        }

        // ── Bus FX: uncoupled bricks filter the accumulated scene ───────────
        {
            std::vector<LiveFx> bus = scene_bus_stack(st, ti, t);
            if (!bus.empty()) {
                std::vector<std::string> sts; FxRunResult rr;
                id<MTLTexture> out = run_fx_stack(cb, g_scene[cur], w, h, -(ti + 16),
                                                  bus, t, t, matte, &sts, &rr);
                note_statuses(ti, bus, sts, "bus");
                if (rr.pso_failed) rc = 4;
                if (rr.matte_cutout)
                    notes.push_back({{"track", ti}, {"where", "bus"},
                                     {"status", "remove_background_ignored_on_bus"}});
                bus_n += rr.applied;
                if (out != g_scene[cur] && g_quad_pso) {
                    // Copy the chain result back into the scene pair so later
                    // chains can reuse the ping-pong pool without aliasing.
                    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor new];
                    rp.colorAttachments[0].texture     = g_scene[cur ^ 1];
                    rp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
                    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
                    [enc setRenderPipelineState:g_quad_pso];
                    float half[2] = { 1.f, 1.f };
                    [enc setVertexBytes:half length:sizeof(half) atIndex:0];
                    [enc setFragmentTexture:out atIndex:0];
                    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                    [enc endEncoding];
                    cur ^= 1;
                }
            }
        }
    }

    // Final: blit the accumulated scene into the app's target texture.
    {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor new];
        rp.colorAttachments[0].texture     = target;
        rp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        if (g_quad_pso) {
            [enc setRenderPipelineState:g_quad_pso];
            float half[2] = { 1.f, 1.f };
            [enc setVertexBytes:half length:sizeof(half) atIndex:0];
            [enc setFragmentTexture:g_scene[cur] atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        } else rc = 3;
        [enc endEncoding];
    }

    {
        std::lock_guard<std::mutex> lk(g_stack_mu);
        g_scene_debug = {
            {"active", true},
            {"playhead", t},
            {"layers_submitted", (int)layers.size()},
            {"layers_drawn", std::move(drawn)},
            {"layers_skipped", std::move(skipped)},
            {"glass_applied", glass_n},
            {"bus_applied", bus_n},
            {"transitions_active", trans_n},
            {"has_matte", matte != nil},
            {"notes", std::move(notes)},
        };
    }
    return rc;
}

// ── Legacy single-content path (camera / pre-Stage-2 Swift) ──────────────────
// Aurora + one aspect-fit content frame with the set_live_fx stack; kept until
// the Swift layer feeder (Stage 2) submits real layer frames.
static int render_legacy(id<MTLCommandBuffer> cb, id<MTLTexture> target, int w, int h,
                         double t, id<MTLTexture> source, int sw, int sh,
                         id<MTLTexture> matte) {
    std::vector<LiveFx> stack;
    { std::lock_guard<std::mutex> lk(g_stack_mu); stack = g_stack; }
    std::vector<std::string> statuses;
    int rc0 = 0;
    // Background cutout only when the stack asks for it (Remove Background
    // body entry). A matte may now be present for other consumers (matte-keyed
    // chroma, body passes) — it must not remove the background by itself.
    bool want_cutout = false;
    if (source && !stack.empty()) {
        FxRunResult rr;
        source = run_fx_stack(cb, source, sw, sh, /*chain_id=*/-2,
                              stack, t, g_content_time,
                              matte, &statuses, &rr);
        if (rr.pso_failed) rc0 = 4;
        want_cutout = rr.matte_cutout;
    } else if (!stack.empty()) {
        statuses.assign(stack.size(), "no_content");
    }
    { std::lock_guard<std::mutex> lk(g_stack_mu); g_pass_status = std::move(statuses); }

    // Main pass: aurora background + aspect-fit blit of `source`.
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor new];
    rp.colorAttachments[0].texture     = target;
    rp.colorAttachments[0].loadAction  = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor  = MTLClearColorMake(0.02, 0.02, 0.03, 1.0);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:g_bg_pso];
    struct { float t; float rx, ry; } bgu = { (float)t, (float)w, (float)h };
    [enc setFragmentBytes:&bgu length:sizeof(bgu) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    int rc = rc0;
    if (source && g_quad_pso && sw > 0 && sh > 0) {
        float ta = (float)w / (float)h, ca = (float)sw / (float)sh;
        float sx = ca > ta ? 1.0f : ca / ta, sy = ca > ta ? ta / ca : 1.0f;
        float half[2] = { sx, sy };
        if (matte && want_cutout && !g_matte_pso) {
            // Matte requested but the composite pipeline failed to build — a
            // real error, not a silent skip. Blit plain and report it.
            static bool s_logged = false;
            if (!s_logged) { s_logged = true; NSLog(@"[metal_render] matte pso unavailable — compositing without matte"); }
            rc = 2;
        }
        if (matte && want_cutout && g_matte_pso) {
            // Background replacement: the engine background is already in the
            // target; blend the content over it with the person matte as alpha.
            [enc setRenderPipelineState:g_matte_pso];
            [enc setVertexBytes:half length:sizeof(half) atIndex:0];
            [enc setFragmentTexture:source atIndex:0];
            [enc setFragmentTexture:matte atIndex:1];
            [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        } else {
            [enc setRenderPipelineState:g_quad_pso];
            [enc setVertexBytes:half length:sizeof(half) atIndex:0];
            [enc setFragmentTexture:source atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
    }
    [enc endEncoding];
    return rc;
}

int metal_render_frame(void* mtl_texture, int w, int h, double t, const AppState* state) {
    if (!g_queue || !g_bg_pso || !mtl_texture) return 1;
    id<MTLTexture> target = (__bridge id<MTLTexture>)mtl_texture;
    ++g_frame_serial;                 // chroma feedback GC clock
    if ((g_frame_serial & 63) == 0) chroma_fb_gc();

    // Snapshot content + matte + layer frames under the lock. Every CoreVideo
    // mapping used this frame is retained until the command buffer completes
    // (same discipline as the matte), so a mid-flight resubmission can't free
    // an IOSurface the GPU is still sampling.
    id<MTLTexture> source = nil; int sw = 0, sh = 0;
    id<MTLTexture> matte  = nil;
    std::map<std::pair<int,int>, LayerFrame> layers;
    std::vector<CVMetalTextureRef> held;
    { std::lock_guard<std::mutex> lk(g_content_mu);
      if (g_content && g_cw > 0 && g_ch > 0) { source = g_content; sw = g_cw; sh = g_ch; }
      if (g_matte_tex && g_matte_cvtex) {
          matte = g_matte_tex;
          held.push_back((CVMetalTextureRef)CFRetain(g_matte_cvtex));
      }
      layers = g_layers;
      for (auto& kv : layers)
          if (kv.second.cvtex) held.push_back((CVMetalTextureRef)CFRetain(kv.second.cvtex));
    }

    id<MTLCommandBuffer> cb = [g_queue commandBuffer];
    int rc;
    if (state && !layers.empty()) {
        rc = render_scene(cb, target, w, h, t, *state, layers, matte);
    } else {
        rc = render_legacy(cb, target, w, h, t, source, sw, sh, matte);
        std::lock_guard<std::mutex> lk(g_stack_mu);
        g_scene_debug = {{"active", false}};
    }
    if (!held.empty()) {
        std::vector<CVMetalTextureRef> keep = std::move(held);
        [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
            for (CVMetalTextureRef r : keep) CFRelease(r);
        }];
    }
    [cb commit];
    return rc;
}
