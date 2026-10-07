// arkit_makeup.mm — the ARKit TrueDepth makeup renderer (iOS, Metal).
//
// Everything renders on ARKit's own 1220-vertex face mesh, drawn with ARKit's
// own camera matrices from the same ARFrame as the pixels, so alignment,
// blinks and expressions are carried by the surface itself. Per frame:
//   1. prep   (half res, mesh)   linear camera color × skin mask, premultiplied;
//                                second target: the lip core (mean lip color)
//   2. blur   (half res, 2× 1-D) mask-normalized bilateral → the local skin
//                                color; its 1×1 mip = the face-mean skin color
//   2b. lipev (half res, mesh)   lip evidence: redness over the local skin
//   3a. lidcrop (256², fullscreen) roll-normalized face crop → worker thread:
//                                MediaPipe's eyelid moves ARKit's lash line
//   3. face   (full res, mesh)   skin finish, pigment layers, lips, analytic
//                                3D liner, gloss/highlight
//   4. lashes (full res)         depth-tested strands, premultiplied over
//
// Pigment model (linear light): each translucent color in a look is authored
// as "how it reads on the look's reference skin" (sampled from the reference
// photo), so a layer is the per-channel transmittance T = lin(color) /
// lin(reference) applied Beer–Lambert style, c *= T^(coverage · amount). The
// camera's own lighting, pores and shading survive because pigment only
// filters the light already there, and every skin tone keeps its own depth.
// Liner ink uses the same ratio over the local skin color. Lipstick is opaque:
// the product color, re-lit by the lips' own shading; where the lips are is
// read from the camera image inside the mesh's mouth region (lip tissue is
// redder than skin — the canonical lip loops are not every wearer's lips).
// Gloss and highlighter add light (GGX specular from ARKit's primary light).
//
// A look is data — models/face/arkit/<id>.json + the two mask atlases it names
// (tools/gen_arkit_makeup.py) — documented in pms-ios docs/ARKIT_NATIVE_PLAN.md.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include "arkit_makeup.h"
#include "arkit_face.h"
#include "paths.h"        // app_models_dir()
#include "stb_image.h"    // mask PNG decode (impl compiled in video.cpp)
#include "json.hpp"
#include <onnxruntime_cxx_api.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using json = nlohmann::json;

// ── Shaders ──────────────────────────────────────────────────────────────────
NSString* const kMakeupSrc = @R"(
#include <metal_stdlib>
using namespace metal;

static float3 to_lin(float3 c) {
    c = saturate(c);
    return select(pow((c + 0.055) / 1.055, float3(2.4)), c / 12.92, c <= 0.04045);
}
static float3 to_srgb(float3 c) {
    c = saturate(c);
    return select(1.055 * pow(c, float3(1.0 / 2.4)) - 0.055, c * 12.92, c <= 0.0031308);
}
static float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
static float3 tint(float3 t, float k) { return pow(t, float3(k)); }
static float redness(float3 c) { return (c.r - c.g) / max(c.r + c.g, 1e-4); }
// Lip tissue is redder than the skin around it, whatever the light.
static float lip_evidence(float3 c, float skin_red) {
    return smoothstep(0.04, 0.11, redness(c) - skin_red);
}
// Local skin color at screen point sp (mask-normalized blur; the face mean
// where no skin is near) and the face-mean skin color (the blur's 1×1 mip).
static float3 local_skin(texture2d<float> blur, float2 sp, float mean_level, float3 fallback,
                         thread float3& mean_skin, thread float& support) {
    constexpr sampler ls(filter::linear, address::clamp_to_edge);
    constexpr sampler mean_s(filter::linear, mip_filter::nearest, address::clamp_to_edge);
    float4 bl = blur.sample(ls, sp);
    float4 m4 = blur.sample(mean_s, float2(0.5), level(mean_level));
    mean_skin = m4.a > 1e-6 ? m4.rgb / m4.a : fallback;
    support = smoothstep(0.02, 0.25, bl.a);
    return mix(mean_skin, bl.rgb / max(bl.a, 1e-4), support);
}

// GGX specular (normalized, Schlick Fresnel at F0 = 0.04) for unit vectors.
static float ggx_spec(float3 N, float3 V, float3 L, float rough) {
    float3 H = normalize(L + V);
    float ndl = saturate(dot(N, L)), ndh = saturate(dot(N, H)), vdh = saturate(dot(V, H));
    float a2 = rough * rough * rough * rough;
    float dd = ndh * ndh * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159265 * dd * dd);
    float F = 0.04 + 0.96 * pow(1.0 - vdh, 5.0);
    return D * F * 0.25 * ndl;
}

// ── face mesh ──
struct MeshUni { float4x4 mvp; float4x4 mv; float2 inv_target; float2 pad; };
struct MeshOut {
    float4 pos [[position]];
    float2 uv;
    float3 vpos;   // view space (m)
    float3 vnrm;   // view space
    float3 mpos;   // face-anchor space (m)
};
vertex MeshOut mk_mesh_v(uint vid [[vertex_id]],
                         device const packed_float3* pos [[buffer(0)]],
                         device const packed_float3* nrm [[buffer(1)]],
                         device const float2* uv [[buffer(2)]],
                         constant MeshUni& u [[buffer(3)]]) {
    float4 p = float4(float3(pos[vid]), 1.0);
    MeshOut o;
    o.pos  = u.mvp * p;
    o.uv   = uv[vid];
    o.vpos = (u.mv * p).xyz;
    o.vnrm = (u.mv * float4(float3(nrm[vid]), 0.0)).xyz;
    o.mpos = p.xyz;
    return o;
}

// 1. prep: premultiplied (linear rgb · w, w), w = skin mask × facing; the lip
// target accumulates the lip core (deep inside the mouth loops — lips on
// every face), whose 1×1 mip is the wearer's mean lip color.
struct PrepOut { float4 skin [[color(0)]]; float4 lip [[color(1)]]; };
fragment PrepOut mk_prep_f(MeshOut in [[stage_in]],
                           constant MeshUni& u [[buffer(0)]],
                           texture2d<float> src [[texture(0)]],
                           texture2d<float> mask_a [[texture(1)]],
                           texture2d<float> mask_b [[texture(2)]]) {
    constexpr sampler ls(filter::linear, mip_filter::linear, address::clamp_to_edge);
    float face = smoothstep(0.08, 0.32, dot(normalize(in.vnrm), normalize(-in.vpos)));
    float3 c = to_lin(src.sample(ls, in.pos.xy * u.inv_target).rgb);
    float w = mask_a.sample(ls, in.uv).r * face;
    float d_mm = (mask_b.sample(ls, in.uv).r - 0.5) * 16.0;
    float wl = (1.0 - smoothstep(-6.0, -4.5, d_mm)) * face;
    PrepOut o;
    o.skin = float4(c * w, w);
    o.lip = float4(c * wl, wl);
    return o;
}

// 2. blur: mask-normalized separable bilateral. Non-skin texels (eyes, brows,
// lips) carry w = 0 and never bleed in; the luminance range term keeps real
// shading edges (nose, jaw) while flattening blotches.
struct FsOut { float4 pos [[position]]; float2 uv; };
vertex FsOut mk_fs_v(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    FsOut o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv  = float2(p.x, 1.0 - p.y);
    return o;
}
struct BlurUni { float2 step; float range; float pad; };
fragment float4 mk_blur_f(FsOut in [[stage_in]], constant BlurUni& u [[buffer(0)]],
                          texture2d<float> t [[texture(0)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    float4 c0 = t.sample(s, in.uv);
    float l0 = c0.a > 1e-3 ? luma(c0.rgb / c0.a) : -1.0;
    float4 acc = 0.0;
    float wsum = 0.0;
    for (int i = -7; i <= 7; ++i) {
        float4 ci = t.sample(s, in.uv + u.step * float(i));
        float w = exp(-float(i * i) * (1.0 / 18.0));          // σ = 3 taps
        if (l0 >= 0.0 && ci.a > 1e-3) {
            float d = (luma(ci.rgb / ci.a) - l0) / (u.range * (l0 + 0.02));
            w *= exp(-0.5 * d * d);
        }
        acc += ci * w;
        wsum += w;
    }
    return acc / max(wsum, 1e-6);
}

// 2b. lip evidence (half res, mesh): how lip-like each point of the mouth
// region is — redness over the local skin of a ~6 px averaged color, so
// camera chroma subsampling and JPEG blocks never reach the edge. The face
// pass samples it bilinearly (smooth edges) and dilates it for the overline.
struct EvUni { float2 inv_src; float mean_level; float pad; };
fragment float4 mk_lipev_f(MeshOut in [[stage_in]],
                           constant MeshUni& mu [[buffer(0)]],
                           constant EvUni& u [[buffer(1)]],
                           texture2d<float> src [[texture(0)]],
                           texture2d<float> blur [[texture(1)]],
                           texture2d<float> mask_b [[texture(2)]]) {
    constexpr sampler ls(filter::linear, address::clamp_to_edge);
    constexpr sampler ms(filter::linear, mip_filter::linear, address::clamp_to_edge);
    float d_mm = (mask_b.sample(ms, in.uv).r - 0.5) * 16.0;
    if (d_mm > 3.0) return float4(0.0);
    float2 sp = in.pos.xy * mu.inv_target;
    float3 acc = 0.0;
    for (int j = 0; j < 9; ++j) {
        float2 o = float2(float(j % 3) - 1.0, float(j / 3) - 1.0) * 2.0;   // full-res px
        acc += to_lin(src.sample(ls, sp + o * u.inv_src).rgb);
    }
    acc *= 1.0 / 9.0;
    float3 mean_skin;
    float support;
    float3 lo = local_skin(blur, sp, u.mean_level, acc, mean_skin, support);
    return float4(lip_evidence(acc, redness(lo)));
}

// 3a. eyelid crop (256², fullscreen): the face, roll-normalized, as
// MediaPipe's landmark model takes it. On a worker thread the model's upper
// eyelid contour corrects ARKit's rim, which rides onto the lid when the lids
// are lowered and misses the real lid shape (see LidWorker).
struct CropUni { float2 c; float size; float roll; float2 inv_src; float2 pad; };
fragment float4 mk_crop_f(FsOut in [[stage_in]], constant CropUni& u [[buffer(0)]],
                          texture2d<float> src [[texture(0)]]) {
    constexpr sampler ls(filter::linear, address::clamp_to_zero);
    float2 l = (in.pos.xy - 0.5) / 256.0 * u.size - 0.5 * u.size;
    float cr = cos(u.roll), sr = sin(u.roll);
    float2 f = u.c + float2(l.x * cr - l.y * sr, l.x * sr + l.y * cr);
    return float4(src.sample(ls, f * u.inv_src).rgb, 1.0);
}

// 3. face composite.
struct FaceUni {
    float2 inv_target; float amount; float overlay;
    float4 skin;        // smooth, even, lift, mean mip level
    float4 blush;       // rgb transmittance, a amount
    float4 shadow;
    float4 freckle;
    float4 misc;        // x inner-corner light, y reference skin luma (linear)
    float4 lip_cream;   // rgb lipstick (linear, as on the reference face), a cover
    float4 lip_shape;   // x overline mm, y luminance match, z gloss, w gloss roughness
    float4 hl;          // x highlight amount, y roughness, z sheen
    float4 ink;         // rgb liner ratio over the local skin, a amount
    float4 light_dir;   // xyz view-space direction to the light, w gain
    float4 light_col;   // rgb, luma 1
    float4 stroke[32];  // per eye (0: person's right, x < 0): liner centerline from
                        // the inner corner — 10 lash-line + 6 wing points, w = width (m)
    float4 corner_n[2]; // per eye: outer-corner surface normal
};

// Liner coverage at anchor-space point p: signed distance to one
// variable-width stroke. The lash-line part is measured in 3D on the lid; the
// wing leaves the eye's curvature, so it is measured in the outer corner's
// tangent plane. `px_m` is the fragment's footprint in meters (antialiasing).
static float liner_cover(constant FaceUni& u, float3 p, float px_m) {
    int e = p.x < 0.0 ? 0 : 1;
    float3 n = u.corner_n[e].xyz;
    float sd = 1e9;
    for (int k = 0; k < 15; ++k) {
        float4 a = u.stroke[e * 16 + k], b = u.stroke[e * 16 + k + 1];
        float3 pa = p - a.xyz, ab = b.xyz - a.xyz;
        if (k >= 9) { pa -= n * dot(pa, n); ab -= n * dot(ab, n); }
        float t = saturate(dot(pa, ab) / max(dot(ab, ab), 1e-12));
        sd = min(sd, length(pa - ab * t) - 0.5 * mix(a.w, b.w, t));
    }
    return 1.0 - smoothstep(-px_m, px_m, sd);
}

fragment float4 mk_face_f(MeshOut in [[stage_in]],
                          constant FaceUni& u [[buffer(0)]],
                          texture2d<float> src [[texture(0)]],
                          texture2d<float> blur [[texture(1)]],
                          texture2d<float> mask_a [[texture(2)]],
                          texture2d<float> mask_b [[texture(3)]],
                          texture2d<float> lipm [[texture(4)]],
                          texture2d<float> lipev [[texture(5)]]) {
    constexpr sampler ls(filter::linear, address::clamp_to_edge);
    constexpr sampler ms(filter::linear, mip_filter::linear, address::clamp_to_edge,
                         max_anisotropy(8));
    constexpr sampler mean_s(filter::linear, mip_filter::nearest, address::clamp_to_edge);
    float2 sp = in.pos.xy * u.inv_target;
    float3 c0 = to_lin(src.sample(ls, sp).rgb);
    float3 c = c0;
    float4 A = mask_a.sample(ms, in.uv);   // r skin, g blush, b shadow
    float4 B = mask_b.sample(ms, in.uv);   // r lip SDF, g freckles, b gloss/highlight, a inner light
    float3 N = normalize(in.vnrm), V = normalize(-in.vpos);
    float ndv = saturate(dot(N, V));
    float face = smoothstep(0.08, 0.32, ndv);
    float amt = u.amount, amt1 = min(amt, 1.0);

    float3 mean_skin;
    float support;
    float3 lo = local_skin(blur, sp, u.skin.w, c0, mean_skin, support);

    float px_m = max(0.7 * length(fwidth(in.mpos)), 2e-5);
    float ink = liner_cover(u, in.mpos, px_m);

    if (u.overlay > 0.5) {               // alignment QA: UV checker + liner
        float2 cell = floor(in.uv * 48.0);
        float chk = fmod(cell.x + cell.y, 2.0);
        float3 dbg = mix(float3(0.85, 0.15, 0.55), float3(0.15, 0.85, 0.35), chk);
        c = mix(c, dbg * (0.25 + luma(c0)), 0.45);
        c = mix(c, float3(1.0, 0.9, 0.0), ink);
        return float4(to_srgb(c), 1.0);
    }

    // 1. skin: porcelain finish on skin texels only (smooth, even, lift).
    float ws = A.r * face * amt1;
    c = mix(c, lo, u.skin.x * ws * support);
    c = mix(c, luma(c) * mean_skin / max(luma(mean_skin), 1e-4), u.skin.y * ws);
    c *= 1.0 + u.skin.z * ws;

    // 2. pigments: Beer–Lambert transmittance over the skin.
    float k = amt * face;
    c *= tint(u.blush.rgb,   A.g * u.blush.a   * k);
    c *= tint(u.freckle.rgb, B.g * u.freckle.a * k);
    c *= tint(u.shadow.rgb,  A.b * u.shadow.a  * k);
    c *= 1.0 + B.a * u.misc.x * k;                                  // inner-corner light

    // 3. lips: the mesh bounds a generous mouth region (canonical border +
    // 3 mm); the camera decides where the lips are, grown by the overline,
    // and the deep lip core always counts (low-contrast lips still get
    // color). The cream is the product color re-lit by the lips' own
    // shading relative to their mean, so volume, creases and real
    // highlights survive; on overlined skin it is flat product color.
    float d_mm = (B.r - 0.5) * 16.0;
    float prior = (1.0 - smoothstep(0.5, 3.0, d_mm)) * face;
    float lip = 0.0, lip_tex = 1.0;
    if (prior > 0.002) {
        float ev = lipev.sample(ls, sp).r;
        float r_px = u.lip_shape.x * 1e-3 / px_m;      // overline radius, px
        float grown = 0.0;                               // dilation by the overline
        for (int j = 0; j < 8; ++j) {
            float ang = float(j) * 0.7853982;
            float2 o = float2(cos(ang), sin(ang)) * r_px;
            grown = max(grown, lipev.sample(ls, sp + o * u.inv_target).r);
        }
        float core = 1.0 - smoothstep(-6.0, -4.5, d_mm);
        lip = prior * max(max(ev, grown), core);

        float4 lm = lipm.sample(mean_s, float2(0.5), level(u.skin.w));
        float lip_mean = lm.a > 1e-6 ? luma(lm.rgb / lm.a) : 0.8 * luma(lo);
        float adapt = sqrt(clamp(luma(mean_skin) / max(u.misc.y, 1e-4), 0.5, 1.6));
        float3 P = u.lip_cream.rgb * adapt;
        float lp = max(luma(P), 1e-4);
        float target = mix(lip_mean, lp, u.lip_shape.y);
        float shade = mix(1.0, clamp(luma(c) / max(lip_mean, 1e-4), 0.35, 2.5), ev);
        c = mix(c, P * (target / lp) * shade, saturate(lip * u.lip_cream.a * amt));
        lip_tex = shade;
    }

    // 4. liner: opaque ink over the local skin (stays visible near grazing).
    float ink_face = smoothstep(0.02, 0.12, ndv);
    c = mix(c, u.ink.rgb * lo, saturate(ink * u.ink.a * amt1 * ink_face));

    // 5. gloss + highlighter: GGX specular. Shine on a selfie mostly mirrors
    // what faces the face — the screen, a ring light, the room in front — so
    // a soft frontal key above the camera joins ARKit's primary light. The
    // highlighter never touches the mouth region; on the lips the gloss is
    // broken up by their own texture (ridges catch it, creases don't)
    // instead of a smooth blob on ARKit's coarse normals.
    float on_lip = saturate(lip * u.lip_cream.a);
    float off_lip = smoothstep(0.0, 1.5, d_mm);
    float3 L = normalize(u.light_dir.xyz);
    const float3 L_front = normalize(float3(0.0, 0.35, 1.0));
    float E = luma(lo) / 0.45;                       // irradiance vs. a 0.45-albedo skin
    float hl_spec = ggx_spec(N, V, L, u.hl.y) * u.light_dir.w + 0.7 * ggx_spec(N, V, L_front, u.hl.y);
    c += u.light_col.rgb * hl_spec * E * B.b * u.hl.x * off_lip * amt * face;
    c += lo * u.hl.z * B.b * off_lip * amt * face;                  // highlighter sheen
    if (on_lip > 0.0) {
        float rough = u.lip_shape.w;
        float lip_spec = ggx_spec(N, V, L, rough) * u.light_dir.w + 0.7 * ggx_spec(N, V, L_front, rough);
        float ridges = pow(clamp(lip_tex, 0.5, 1.8), 2.0);
        c += u.light_col.rgb * lip_spec * E * ridges * B.b * u.lip_shape.z * on_lip * amt * face;
    }

    return float4(to_srgb(c), 1.0);
}

// 4. lashes: each strand segment is a screen-aligned quad at least 1 px wide;
// coverage = true width in px, so sub-pixel strands darken by exactly their
// area (no aliasing, no stair-stepped fringe).
struct LashVtx { packed_float3 p0; packed_float3 p1; float w0; float w1; float side; float end; float shade; float pad; };
struct LashUni { float4x4 mvp; float2 half_target; float px_per_m; float alpha; float4 color; };
struct LashOut { float4 pos [[position]]; float side_px; float core_px; float cover; float shade; };
vertex LashOut mk_lash_v(uint vid [[vertex_id]],
                         device const LashVtx* vb [[buffer(0)]],
                         constant LashUni& u [[buffer(1)]]) {
    LashVtx L = vb[vid];
    float4 a = u.mvp * float4(float3(L.p0), 1.0);
    float4 b = u.mvp * float4(float3(L.p1), 1.0);
    float2 sa = a.xy / a.w * u.half_target, sb = b.xy / b.w * u.half_target;
    float2 dir = sb - sa;
    float len = length(dir);
    dir = len > 1e-5 ? dir / len : float2(1.0, 0.0);
    float2 nrm = float2(-dir.y, dir.x);
    float4 c = L.end > 0.5 ? b : a;
    float wpx = (L.end > 0.5 ? L.w1 : L.w0) * u.px_per_m / max(c.w, 1e-4);
    float core = max(wpx * 0.5, 0.5);
    float half_draw = core + 1.0;
    c.xy += nrm * (L.side * half_draw) / u.half_target * c.w;
    LashOut o;
    o.pos = c;
    o.side_px = L.side * half_draw;
    o.core_px = core;
    o.cover = min(wpx, 1.0);
    o.shade = L.shade;
    return o;
}
fragment float4 mk_lash_f(LashOut in [[stage_in]], constant LashUni& u [[buffer(0)]]) {
    float a = saturate(in.core_px + 0.5 - abs(in.side_px)) * in.cover * u.alpha;
    return float4(u.color.rgb * in.shade * a, a);
}
)";

// ── Small math ───────────────────────────────────────────────────────────────
struct V3 { float x = 0, y = 0, z = 0; };
inline V3 v3(float x, float y, float z) { V3 r; r.x = x; r.y = y; r.z = z; return r; }
inline V3 operator+(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline V3 operator-(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline V3 operator*(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
inline float length(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 normalize(V3 a) { float l = length(a); return l > 1e-12f ? a * (1.f / l) : a; }
inline float mixf(float a, float b, float t) { return a + (b - a) * t; }
inline float smooth01(float t) { t = std::clamp(t, 0.f, 1.f); return t * t * (3.f - 2.f * t); }

// Column-major 4x4 (simd_float4x4 memory layout).
void mat4_mul(const float* a, const float* b, float* out) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float acc = 0.f;
            for (int k = 0; k < 4; ++k) acc += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = acc;
        }
}
V3 mat4_dir(const float* m, V3 v) {     // upper 3x3 × v
    return v3(m[0] * v.x + m[4] * v.y + m[8] * v.z,
              m[1] * v.x + m[5] * v.y + m[9] * v.z,
              m[2] * v.x + m[6] * v.y + m[10] * v.z);
}

float srgb_to_lin(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
bool parse_hex(const std::string& s, float out[3]) {   // "#rrggbb" → sRGB 0..1
    if (s.size() != 7 || s[0] != '#') return false;
    for (int i = 0; i < 3; ++i) {
        char* end = nullptr;
        std::string byte = s.substr(1 + i * 2, 2);
        long v = std::strtol(byte.c_str(), &end, 16);
        if (!end || *end) return false;
        out[i] = (float)v / 255.f;
    }
    return true;
}

// Approximate blackbody color for an ambient color temperature (Kelvin), in
// linear RGB normalized to luma 1 (Tanner Helland's fit).
void kelvin_rgb(float kelvin, float out[3]) {
    float t = std::clamp(kelvin, 1500.f, 15000.f) / 100.f;
    float r = t <= 66.f ? 255.f : 329.698727446f * std::pow(t - 60.f, -0.1332047592f);
    float g = t <= 66.f ? 99.4708025861f * std::log(t) - 161.1195681661f
                        : 288.1221695283f * std::pow(t - 60.f, -0.0755148492f);
    float b = t >= 66.f ? 255.f
            : (t <= 19.f ? 0.f : 138.5177312231f * std::log(t - 10.f) - 305.0447927307f);
    float c[3] = {r, g, b};
    for (int i = 0; i < 3; ++i) out[i] = srgb_to_lin(std::clamp(c[i], 0.f, 255.f) / 255.f);
    float l = 0.2126f * out[0] + 0.7152f * out[1] + 0.0722f * out[2];
    for (int i = 0; i < 3; ++i) out[i] /= std::max(l, 1e-4f);
}

// ── ARKit topology (constant across faces; see tools/gen_arkit_makeup.py) ──
// Person's RIGHT eye has x < 0 in anchor space. Polylines run outer → inner.
const int kUpperR[12] = {1101, 1100, 1099, 1098, 1097, 1096, 1095, 1094, 1093, 1092, 1091, 1090};
const int kUpperL[12] = {1069, 1070, 1071, 1072, 1073, 1074, 1075, 1076, 1077, 1078, 1079, 1080};
const int kLowerR[14] = {1101, 1102, 1103, 1104, 1105, 1106, 1107, 1108,
                         1085, 1086, 1087, 1088, 1089, 1090};
const int kLowerL[14] = {1069, 1068, 1067, 1066, 1065, 1064, 1063, 1062,
                         1061, 1084, 1083, 1082, 1081, 1080};
// MediaPipe-order blendshape slots (ARKitBlendshapes.swift): eyeBlinkLeft = 9,
// eyeBlinkRight = 10 — anatomical, so the person's right eye blinks with 10.
const int kBlinkR = 10, kBlinkL = 9;

// ── Look data ────────────────────────────────────────────────────────────────
struct LashRow {
    int   count = 0, clumps = 0;
    float len_inner_mm = 6.f, len_outer_mm = 10.f, root_mm = 0.1f;
    float lift_deg = 15.f, curl_deg = 50.f, flare = 0.f, wisp = 0.f, clump = 0.f;
    float offset_mm = 0.f, t0 = 0.f, t1 = 1.f, blink_close = 0.f;
};
struct Look {
    id<MTLTexture> mask_a = nil, mask_b = nil;
    float smooth = 0.f, smooth_mm = 2.f, even = 0.f, lift = 0.f;
    float blush[4] = {1, 1, 1, 0}, shadow[4] = {1, 1, 1, 0}, freckle[4] = {1, 1, 1, 0};
    float inner_light = 0.f, ref_luma = 0.5f;
    float lip_cream[4] = {1, 1, 1, 0};   // linear product color, cover
    float overline_mm = 0.f, lip_match = 0.85f, gloss = 0.f, gloss_rough = 0.3f;
    float hl_amt = 0.f, hl_rough = 0.4f, hl_sheen = 0.f;
    float ink[4] = {0, 0, 0, 0};
    float liner_in_mm = 0.3f, liner_out_mm = 1.f, wing_mm = 0.f, wing_lift_deg = 15.f;
    float liner_offset_mm = 0.f;
    float lash_srgb[3] = {0.05f, 0.04f, 0.04f};
    float lash_amt = 0.f;
    LashRow upper, lower;
};

float jf(const json& j, const char* k, float def) {
    auto it = j.find(k);
    return (it != j.end() && it->is_number()) ? it->get<float>() : def;
}

// Layer color → transmittance ratio over the reference skin, plus amount.
void ratio_layer(const json& j, const char* key, const float ref_lin[3], float out[4]) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_object()) return;
    float srgb[3];
    if (!parse_hex(it->value("color", std::string()), srgb)) return;
    for (int i = 0; i < 3; ++i)
        out[i] = std::clamp(srgb_to_lin(srgb[i]) / std::max(ref_lin[i], 1e-4f), 1e-3f, 4.f);
    out[3] = jf(*it, "amount", 1.f);
}

LashRow parse_row(const json& j) {
    LashRow r;
    r.count = j.value("count", 0);
    r.clumps = j.value("clumps", 0);
    r.len_inner_mm = jf(j, "len_inner_mm", r.len_inner_mm);
    r.len_outer_mm = jf(j, "len_outer_mm", r.len_outer_mm);
    r.root_mm = jf(j, "root_mm", r.root_mm);
    r.lift_deg = jf(j, "lift_deg", r.lift_deg);
    r.curl_deg = jf(j, "curl_deg", r.curl_deg);
    r.flare = jf(j, "flare", r.flare);
    r.wisp = jf(j, "wisp", r.wisp);
    r.clump = jf(j, "clump", r.clump);
    r.offset_mm = jf(j, "offset_mm", r.offset_mm);
    r.t0 = jf(j, "t0", r.t0);
    r.t1 = jf(j, "t1", r.t1);
    r.blink_close = jf(j, "blink_close", r.blink_close);
    return r;
}

id<MTLTexture> load_mask(id<MTLDevice> dev, id<MTLCommandQueue> q, const std::string& path) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px) { NSLog(@"[arkit_makeup] mask missing: %s", path.c_str()); return nil; }
    MTLTextureDescriptor* td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:(NSUInteger)w height:(NSUInteger)h mipmapped:YES];
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
    [tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0
             withBytes:px bytesPerRow:(NSUInteger)w * 4];
    stbi_image_free(px);
    // The baker pads every mask past its UV islands, so mips never pull in
    // the empty atlas background.
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl generateMipmapsForTexture:tex];
    [bl endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return tex;
}

std::unique_ptr<Look> load_look(id<MTLDevice> dev, id<MTLCommandQueue> q, const std::string& id) {
    const std::string dir = app_models_dir() + "/face/arkit/";
    std::ifstream in(dir + id + ".json");
    if (!in) { NSLog(@"[arkit_makeup] look missing: %s", id.c_str()); return nullptr; }
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        NSLog(@"[arkit_makeup] look %s: invalid JSON", id.c_str());
        return nullptr;
    }
    auto L = std::make_unique<Look>();
    auto masks = j.value("masks", json::array());
    if (masks.size() != 2) { NSLog(@"[arkit_makeup] look %s: needs 2 masks", id.c_str()); return nullptr; }
    L->mask_a = load_mask(dev, q, dir + masks[0].get<std::string>());
    L->mask_b = load_mask(dev, q, dir + masks[1].get<std::string>());
    if (!L->mask_a || !L->mask_b) return nullptr;

    float ref[3] = {0.87f, 0.72f, 0.68f};
    float ref_lin[3];
    parse_hex(j.value("reference_skin", std::string("#deb7ae")), ref);
    for (int i = 0; i < 3; ++i) ref_lin[i] = srgb_to_lin(ref[i]);
    L->ref_luma = 0.2126f * ref_lin[0] + 0.7152f * ref_lin[1] + 0.0722f * ref_lin[2];

    const json skin = j.value("skin", json::object());
    L->smooth = jf(skin, "smooth", 0.f);
    L->smooth_mm = jf(skin, "smooth_mm", 2.f);
    L->even = jf(skin, "even", 0.f);
    L->lift = jf(skin, "lift", 0.f);
    ratio_layer(j, "blush", ref_lin, L->blush);
    ratio_layer(j, "shadow", ref_lin, L->shadow);
    ratio_layer(j, "freckles", ref_lin, L->freckle);
    L->inner_light = jf(j, "inner_light", 0.f);

    // Lipstick is opaque: its color is absolute (as it reads on the
    // reference face), not a ratio over the wearer's lips.
    const json lips = j.value("lips", json::object());
    float lip_srgb[3];
    if (parse_hex(lips.value("color", std::string()), lip_srgb)) {
        for (int i = 0; i < 3; ++i) L->lip_cream[i] = srgb_to_lin(lip_srgb[i]);
        L->lip_cream[3] = jf(lips, "cover", 0.f);
    }
    L->overline_mm = jf(lips, "overline_mm", 0.f);
    L->lip_match = jf(lips, "match", 0.85f);
    L->gloss = jf(lips, "gloss", 0.f);
    L->gloss_rough = jf(lips, "roughness", 0.3f);

    const json hl = j.value("highlight", json::object());
    L->hl_amt = jf(hl, "amount", 0.f);
    L->hl_rough = jf(hl, "roughness", 0.4f);
    L->hl_sheen = jf(hl, "sheen", 0.f);

    const json liner = j.value("liner", json::object());
    ratio_layer(j, "liner", ref_lin, L->ink);
    L->liner_in_mm = jf(liner, "inner_mm", 0.3f);
    L->liner_out_mm = jf(liner, "outer_mm", 1.f);
    L->wing_mm = jf(liner, "wing_mm", 0.f);
    L->wing_lift_deg = jf(liner, "wing_lift_deg", 15.f);
    L->liner_offset_mm = jf(liner, "offset_mm", 0.f);

    const json lashes = j.value("lashes", json::object());
    parse_hex(lashes.value("color", std::string("#0e0a0b")), L->lash_srgb);
    L->lash_amt = jf(lashes, "amount", 0.f);
    L->upper = parse_row(lashes.value("upper", json::object()));
    L->lower = parse_row(lashes.value("lower", json::object()));
    return L;
}

// ── GPU state ────────────────────────────────────────────────────────────────
constexpr int kRing = 3;   // per-frame buffers in flight
constexpr int kMaxLashVerts = 2 * (160 + 60) * 6 * 6;

struct MeshUniC { float mvp[16]; float mv[16]; float inv_target[2]; float pad[2]; };
struct BlurUniC { float step[2]; float range; float pad; };
struct EvUniC { float inv_src[2]; float mean_level; float pad; };
struct FaceUniC {
    float inv_target[2]; float amount; float overlay;
    float skin[4], blush[4], shadow[4], freckle[4], misc[4];
    float lip_cream[4], lip_shape[4], hl[4], ink[4];
    float light_dir[4], light_col[4];
    float stroke[32][4];
    float corner_n[2][4];
};
struct LashVtx { float p0[3]; float p1[3]; float w0, w1, side, end, shade, pad; };
struct LashUniC { float mvp[16]; float half_target[2]; float px_per_m; float alpha; float color[4]; };
struct CropUniC { float c[2]; float size; float roll; float inv_src[2]; float pad[2]; };
static_assert(sizeof(LashVtx) == 48, "LashVtx must match the MSL packed layout");
constexpr int kCrop = 256;   // MediaPipe landmark input
constexpr int kCols = 16;    // eyelid rays per upper rim

struct Ctx {
    bool tried = false, ok = false;
    id<MTLDevice> dev = nil;
    id<MTLCommandQueue> upload_q = nil;
    id<MTLRenderPipelineState> prep = nil, blur = nil, lipev = nil, face = nil, lash = nil;
    id<MTLRenderPipelineState> crop = nil;
    id<MTLDepthStencilState> dss_face = nil, dss_lash = nil;
    id<MTLBuffer> uv = nil, idx = nil;
    id<MTLBuffer> pos[kRing] = {}, nrm[kRing] = {}, lashv[kRing] = {};
    id<MTLTexture> crop_tex = nil;     // eyelid crop render target
    id<MTLBuffer> crop_buf = nil;      // its pixels for the eyelid worker (one job in flight)
    float lid_off[2][2][kCols] = {};   // [eye][upper, lower] smoothed offsets, mm toward the opening
    double lid_t = -1.0;               // last frame time they were updated
    double lid_job_t = -1.0;           // last frame time a crop went to the worker
    int ring = 0;
    int tw = 0, th = 0;                                   // full-res target size
    id<MTLTexture> half0 = nil, half1 = nil, half2 = nil;  // prep, blur tmp, blur out (mips)
    id<MTLTexture> half_lip = nil;                         // prep lip core (mips → mean lip)
    id<MTLTexture> half_ev = nil;                          // lip evidence
    id<MTLTexture> depth = nil;
    std::map<std::string, std::unique_ptr<Look>> looks;   // nullptr = known missing
};
Ctx g;

id<MTLRenderPipelineState> make_pso(id<MTLLibrary> lib, NSString* vfn, NSString* ffn,
                                    MTLPixelFormat color, bool depth, bool premul,
                                    int targets = 1) {
    MTLRenderPipelineDescriptor* rd = [MTLRenderPipelineDescriptor new];
    rd.vertexFunction = [lib newFunctionWithName:vfn];
    rd.fragmentFunction = [lib newFunctionWithName:ffn];
    for (int i = 0; i < targets; ++i) rd.colorAttachments[i].pixelFormat = color;
    if (depth) rd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    if (premul) {
        auto* ca = rd.colorAttachments[0];
        ca.blendingEnabled = YES;
        ca.rgbBlendOperation = ca.alphaBlendOperation = MTLBlendOperationAdd;
        ca.sourceRGBBlendFactor = ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
        ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    }
    NSError* err = nil;
    id<MTLRenderPipelineState> pso = (rd.vertexFunction && rd.fragmentFunction)
        ? [g.dev newRenderPipelineStateWithDescriptor:rd error:&err] : nil;
    if (!pso) NSLog(@"[arkit_makeup] %@ pso: %@", ffn, err);
    return pso;
}

bool init_ctx(id<MTLDevice> dev) {
    if (g.tried) return g.ok && g.dev == dev;
    g.tried = true;
    g.dev = dev;
    NSError* err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:kMakeupSrc options:nil error:&err];
    if (!lib) { NSLog(@"[arkit_makeup] library: %@", err); return false; }
    g.prep = make_pso(lib, @"mk_mesh_v", @"mk_prep_f", MTLPixelFormatRGBA16Float, false, false, 2);
    g.blur = make_pso(lib, @"mk_fs_v", @"mk_blur_f", MTLPixelFormatRGBA16Float, false, false);
    g.lipev = make_pso(lib, @"mk_mesh_v", @"mk_lipev_f", MTLPixelFormatR16Float, false, false);
    g.face = make_pso(lib, @"mk_mesh_v", @"mk_face_f", MTLPixelFormatBGRA8Unorm, true, false);
    g.lash = make_pso(lib, @"mk_lash_v", @"mk_lash_f", MTLPixelFormatBGRA8Unorm, true, true);
    MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = MTLCompareFunctionLess;      // folded lids self-overlap
    d.depthWriteEnabled = YES;
    g.dss_face = [dev newDepthStencilStateWithDescriptor:d];
    d.depthCompareFunction = MTLCompareFunctionLessEqual; // strands occluded by the face
    d.depthWriteEnabled = NO;
    g.dss_lash = [dev newDepthStencilStateWithDescriptor:d];
    g.uv = [dev newBufferWithBytes:&k_arkit_uv[0][0] length:sizeof(k_arkit_uv)
                           options:MTLResourceStorageModeShared];
    g.idx = [dev newBufferWithBytes:&k_arkit_tris[0][0] length:sizeof(k_arkit_tris)
                            options:MTLResourceStorageModeShared];
    for (int i = 0; i < kRing; ++i) {
        g.pos[i] = [dev newBufferWithLength:ARKIT_NPTS * 12 options:MTLResourceStorageModeShared];
        g.nrm[i] = [dev newBufferWithLength:ARKIT_NPTS * 12 options:MTLResourceStorageModeShared];
        g.lashv[i] = [dev newBufferWithLength:sizeof(LashVtx) * kMaxLashVerts
                                      options:MTLResourceStorageModeShared];
    }
    g.crop = make_pso(lib, @"mk_fs_v", @"mk_crop_f", MTLPixelFormatRGBA8Unorm, false, false);
    MTLTextureDescriptor* cd = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:kCrop height:kCrop
                                 mipmapped:NO];
    cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    cd.storageMode = MTLStorageModePrivate;
    g.crop_tex = [dev newTextureWithDescriptor:cd];
    g.crop_buf = [dev newBufferWithLength:(NSUInteger)kCrop * kCrop * 4
                                  options:MTLResourceStorageModeShared];
    g.upload_q = [dev newCommandQueue];
    g.ok = g.prep && g.blur && g.lipev && g.face && g.lash && g.crop && g.crop_tex &&
           g.crop_buf && g.dss_face && g.dss_lash;
    return g.ok;
}

void ensure_targets(int w, int h) {
    if (g.tw == w && g.th == h && g.half0) return;
    g.tw = w; g.th = h;
    const int hw = std::max(1, w / 2), hh = std::max(1, h / 2);
    auto rt = [&](int tw, int th, MTLPixelFormat fmt, bool mips) {
        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:fmt width:(NSUInteger)tw height:(NSUInteger)th
                                     mipmapped:mips];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        return [g.dev newTextureWithDescriptor:td];
    };
    g.half0 = rt(hw, hh, MTLPixelFormatRGBA16Float, false);
    g.half1 = rt(hw, hh, MTLPixelFormatRGBA16Float, false);
    g.half2 = rt(hw, hh, MTLPixelFormatRGBA16Float, true);
    g.half_lip = rt(hw, hh, MTLPixelFormatRGBA16Float, true);
    g.half_ev = rt(hw, hh, MTLPixelFormatR16Float, false);
    g.depth = rt(w, h, MTLPixelFormatDepth32Float, false);
}

const Look* get_look(const std::string& id) {
    if (id.empty()) return nullptr;
    auto it = g.looks.find(id);
    if (it == g.looks.end())
        it = g.looks.emplace(id, load_look(g.dev, g.upload_q, id)).first;
    return it->second.get();
}

// One-euro filter on the anchor-space vertices: raw ARKit per-vertex noise
// shimmers at high-contrast liner/lip edges. Velocity-adaptive — sub-mm jitter
// smooths hard, real blinks pass unlagged. Head motion lives in the
// transform, so it is never smoothed.
void filter_vertices(float (*v)[3], double t) {
    static float flt[ARKIT_NPTS][3], dflt[ARKIT_NPTS][3];
    static bool init = false;
    static double last = 0.0;
    float dt = (init && t > last) ? (float)std::min(t - last, 0.1) : (1.f / 60.f);
    last = t;
    if (!init || dt >= 0.1f) {
        std::memcpy(flt, v, sizeof(flt));
        std::memset(dflt, 0, sizeof(dflt));
        init = true;
        return;
    }
    const float fs = 1.f / std::max(dt, 1e-3f);
    const float min_fc = 1.2f, beta = 240.f, d_fc = 8.f;
    const float ad = 1.f / (1.f + fs / (6.2832f * d_fc));
    bool bad = false;
    for (int i = 0; i < ARKIT_NPTS; ++i)
        for (int c = 0; c < 3; ++c) {
            float dx = (v[i][c] - flt[i][c]) * fs;
            dflt[i][c] += ad * (dx - dflt[i][c]);
            float fc = min_fc + beta * std::fabs(dflt[i][c]);
            float al = 1.f / (1.f + fs / (6.2832f * fc));
            flt[i][c] += al * (v[i][c] - flt[i][c]);
            bad |= !std::isfinite(flt[i][c]);
        }
    // A single NaN from ARKit would poison the static state forever.
    if (bad) { std::memcpy(flt, v, sizeof(flt)); std::memset(dflt, 0, sizeof(dflt)); return; }
    std::memcpy(v, flt, sizeof(flt));
}

// Area-weighted vertex normals in anchor space (outward: k_arkit_tris winds
// counter-clockwise seen from the front).
void vertex_normals(const float (*v)[3], float (*n)[3]) {
    std::memset(n, 0, sizeof(float) * 3 * ARKIT_NPTS);
    for (int t = 0; t < ARKIT_NTRI; ++t) {
        const unsigned short* tr = k_arkit_tris[t];
        V3 a = v3(v[tr[0]][0], v[tr[0]][1], v[tr[0]][2]);
        V3 b = v3(v[tr[1]][0], v[tr[1]][1], v[tr[1]][2]);
        V3 c = v3(v[tr[2]][0], v[tr[2]][1], v[tr[2]][2]);
        V3 fn = cross(b - a, c - a);
        for (int k = 0; k < 3; ++k) {
            n[tr[k]][0] += fn.x; n[tr[k]][1] += fn.y; n[tr[k]][2] += fn.z;
        }
    }
    for (int i = 0; i < ARKIT_NPTS; ++i) {
        V3 u = normalize(v3(n[i][0], n[i][1], n[i][2]));
        n[i][0] = u.x; n[i][1] = u.y; n[i][2] = u.z;
    }
}

// Arc-length parameterized polyline over mesh vertices (outer → inner).
struct Poly {
    std::vector<V3> p, n;
    std::vector<float> s;   // cumulative length, normalized to [0, 1]
    void build(const int* ids, int count, const float (*v)[3], const float (*nv)[3]) {
        p.resize(count); n.resize(count); s.resize(count);
        float acc = 0.f;
        for (int i = 0; i < count; ++i) {
            p[i] = v3(v[ids[i]][0], v[ids[i]][1], v[ids[i]][2]);
            n[i] = v3(nv[ids[i]][0], nv[ids[i]][1], nv[ids[i]][2]);
            if (i > 0) acc += length(p[i] - p[i - 1]);
            s[i] = acc;
        }
        for (float& x : s) x /= std::max(acc, 1e-9f);
    }
    // Position, normal and unit tangent (toward the inner corner) at t ∈ [0,1].
    void eval(float t, V3& pos, V3& nrm, V3& tan) const {
        t = std::clamp(t, 0.f, 1.f);
        int i = 0;
        while (i < (int)s.size() - 2 && s[i + 1] < t) ++i;
        float span = std::max(s[i + 1] - s[i], 1e-9f);
        float f = std::clamp((t - s[i]) / span, 0.f, 1.f);
        pos = p[i] + (p[i + 1] - p[i]) * f;
        nrm = normalize(n[i] + (n[i + 1] - n[i]) * f);
        tan = normalize(p[i + 1] - p[i]);
    }
};

uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
float rnd(uint32_t seed) { return (float)(hash_u32(seed) & 0xffffff) / 16777215.f; }

// ── Eyelid refinement (MediaPipe) ────────────────────────────────────────────
// ARKit's eye rims are the lash lines only with the eyes wide open. With the
// lids lowered (gaze down at the phone — the usual selfie pose) the upper rim
// rides ~2 mm onto the lid over the iris and sits inside the eye toward the
// outer corner (the real lid is flatter than ARKit's almond), the lower rim
// sits inside the eye opening, and ARKit's blink value stays low through all
// of it. Edge rules on the image cannot tell lashes from lid (eyes open,
// natural lashes stand above the margin; lowered, they hang over the eye), so
// the trained lid contours of MediaPipe's landmark model (bundled for the
// script API) decide. A frame whose GPU work completes while the worker is
// idle hands it a 256² roll-normalized face crop placed from ARKit's
// projected mesh (no detector); the worker intersects the model's lid
// contours with 16 rays per rim cast from that frame's ARKit rims and
// publishes the offsets (mm, + toward the eye opening). Later frames apply
// them relative to their own rims — head motion stays ARKit's — smoothed.
struct LidRay { float p[2], d[2], ppmm; };   // frame px, unit dir toward the opening, px per mm
struct LidJob {
    float cx, cy, size, roll;                 // crop placement (frame px, radians)
    float eye_c[2][2];                        // eye centers (frame px), to pair contours
    float facing[2];                          // cos(eye normal, direction to the camera)
    LidRay ray[2][2][kCols];                  // [eye][upper, lower][column]
    double t;
};
// Per eye and lid: offsets on a quadratic fit along the rim (mm), and their trust.
struct LidResult { float off[2][2][kCols] = {}; float conf[2][2] = {}; double t = -1.0; };
// MediaPipe 478-mesh lid contours, corner to corner: [mesh eye][upper, lower].
const int kMpLid[2][2][9] = {
    {{33, 246, 161, 160, 159, 158, 157, 173, 133}, {33, 7, 163, 144, 145, 153, 154, 155, 133}},
    {{263, 466, 388, 387, 386, 385, 384, 398, 362}, {263, 249, 390, 373, 374, 380, 381, 382, 362}}};

struct LidWorker {
    dispatch_queue_t q = nullptr;
    std::mutex mu;
    LidResult latest;                         // guarded by mu
    std::atomic<uint64_t> submitted{0}, done{0};
    std::unique_ptr<Ort::Session> sess;       // worker thread only
    bool tried = false;                       // worker thread only
    std::vector<float> input;                 // worker thread only
};
LidWorker lid;

Ort::Env& lid_env() {
    static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "pms-eyelid");
    return env;
}

// Offsets along one ARKit rim from one MediaPipe contour: ray intersections,
// then a least-squares quadratic in t. The real lid differs from ARKit's
// smoothly (shifted, tilted, flatter or more arched); the net's contour does
// not always — kinks and a turned eye's foreshortened outline fit badly and
// lose trust, as does an eye facing away from the camera.
void fit_lid(const float (*lp)[2], const LidRay* rays, float facing, float* off, float& conf) {
    float raw[kCols];
    bool ok[kCols];
    for (int k = 0; k < kCols; ++k) {
        const LidRay& R = rays[k];
        float best_s = 1e30f;
        for (int j = 0; j < 8; ++j) {   // solve p + d·s = a + (b − a)·u
            const float ax = lp[j][0], ay = lp[j][1];
            const float ex = lp[j + 1][0] - ax, ey = lp[j + 1][1] - ay;
            const float den = ex * R.d[1] - R.d[0] * ey;
            if (std::fabs(den) < 1e-6f) continue;
            const float wx = ax - R.p[0], wy = ay - R.p[1];
            const float s = (ex * wy - wx * ey) / den;
            const float u = (R.d[0] * wy - R.d[1] * wx) / den;
            if (u >= 0.f && u <= 1.f && std::fabs(s) < std::fabs(best_s)) best_s = s;
        }
        raw[k] = best_s / std::max(R.ppmm, 1e-3f);
        ok[k] = std::fabs(raw[k]) < 4.f;
    }
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, y0 = 0, y1 = 0, y2 = 0;
    float tlo = 1.f, thi = 0.f;
    int n = 0;
    for (int k = 1; k < kCols - 1; ++k) {   // corner rays graze the contour
        if (!ok[k]) continue;
        const double t = 0.05 + 0.9 * (k + 0.5) / kCols, t2 = t * t;
        s0 += 1; s1 += t; s2 += t2; s3 += t2 * t; s4 += t2 * t2;
        y0 += raw[k]; y1 += raw[k] * t; y2 += raw[k] * t2;
        tlo = std::min(tlo, (float)t);
        thi = std::max(thi, (float)t);
        ++n;
    }
    conf = 0.f;
    std::fill(off, off + kCols, 0.f);
    const double det = s0 * (s2 * s4 - s3 * s3) - s1 * (s1 * s4 - s3 * s2) + s2 * (s1 * s3 - s2 * s2);
    if (n < 6 || std::fabs(det) < 1e-12) return;
    const double a = (y0 * (s2 * s4 - s3 * s3) - s1 * (y1 * s4 - s3 * y2) + s2 * (y1 * s3 - s2 * y2)) / det;
    const double b = (s0 * (y1 * s4 - s3 * y2) - y0 * (s1 * s4 - s3 * s2) + s2 * (s1 * y2 - y1 * s2)) / det;
    const double c = (s0 * (s2 * y2 - y1 * s3) - s1 * (s1 * y2 - y1 * s2) + y0 * (s1 * s3 - s2 * s2)) / det;
    double sq = 0;
    for (int k = 0; k < kCols; ++k) {
        const double t = 0.05 + 0.9 * (k + 0.5) / kCols;
        if (ok[k] && k > 0 && k < kCols - 1) {
            const double r = raw[k] - (a + b * t + c * t * t);
            sq += r * r;
        }
        const double tc = std::clamp((float)t, tlo, thi);   // hold the ends
        off[k] = (float)(a + b * tc + c * tc * tc);
    }
    const float rms = (float)std::sqrt(sq / n);
    conf = std::clamp(1.f - (rms - 0.45f) / 0.55f, 0.f, 1.f) * smooth01((facing - 0.65f) / 0.10f);
}

// Worker thread: one RGBA crop → per-ray offsets, published as `lid.latest`.
void lid_run(const uint8_t* rgba, const LidJob& job) {
    LidResult res;
    res.t = job.t;
    if (!lid.tried) {
        lid.tried = true;
        try {
            Ort::SessionOptions o;
            o.SetIntraOpNumThreads(2);   // the live app keeps its cores
            o.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            lid.sess = std::make_unique<Ort::Session>(
                lid_env(), (app_models_dir() + "/face/face_landmarks_v2.onnx").c_str(), o);
        } catch (const std::exception& e) {
            NSLog(@"[arkit_makeup] eyelid model unavailable: %s", e.what());
        }
    }
    if (lid.sess) {
        lid.input.resize((size_t)kCrop * kCrop * 3);
        for (size_t i = 0; i < (size_t)kCrop * kCrop; ++i)
            for (int c = 0; c < 3; ++c) lid.input[i * 3 + (size_t)c] = (float)rgba[i * 4 + (size_t)c] / 255.f;
        int64_t shape[4] = {1, kCrop, kCrop, 3};
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value in = Ort::Value::CreateTensor<float>(mem, lid.input.data(), lid.input.size(),
                                                        shape, 4);
        const char* in_names[] = {"input_12"};
        const char* out_names[] = {"Identity", "Identity_1"};   // 478 × xyz (crop px), face flag
        try {
            auto out = lid.sess->Run(Ort::RunOptions{nullptr}, in_names, &in, 1, out_names, 2);
            const float* mesh = out[0].GetTensorData<float>();
            const float flag = out[1].GetTensorData<float>()[0];
            if (1.f / (1.f + std::exp(-flag)) > 0.5f) {
                const float cr = std::cos(job.roll), sr = std::sin(job.roll);
                float lp[2][2][9][2];   // [mesh eye][upper, lower] contours in frame px
                for (int m = 0; m < 2; ++m)
                    for (int l = 0; l < 2; ++l)
                        for (int j = 0; j < 9; ++j) {
                            const float* q = mesh + kMpLid[m][l][j] * 3;
                            const float lx = q[0] / kCrop * job.size - 0.5f * job.size;
                            const float ly = q[1] / kCrop * job.size - 0.5f * job.size;
                            lp[m][l][j][0] = job.cx + lx * cr - ly * sr;
                            lp[m][l][j][1] = job.cy + lx * sr + ly * cr;
                        }
                for (int e = 0; e < 2; ++e) {
                    // Pair contours with ARKit's eyes by image position (mirror-proof).
                    int m = 0;
                    float best_d = 1e30f;
                    for (int mm = 0; mm < 2; ++mm) {
                        float cx = 0.f, cy = 0.f;
                        for (int j = 0; j < 9; ++j) { cx += lp[mm][0][j][0]; cy += lp[mm][0][j][1]; }
                        const float dx = cx / 9.f - job.eye_c[e][0], dy = cy / 9.f - job.eye_c[e][1];
                        if (dx * dx + dy * dy < best_d) { best_d = dx * dx + dy * dy; m = mm; }
                    }
                    for (int l = 0; l < 2; ++l)
                        fit_lid(lp[m][l], job.ray[e][l], job.facing[e], res.off[e][l], res.conf[e][l]);
                }
            }
        } catch (const std::exception& e) {
            NSLog(@"[arkit_makeup] eyelid inference: %s", e.what());
        }
    }
    { std::lock_guard<std::mutex> lk(lid.mu); lid.latest = res; }
    lid.done.fetch_add(1);
}

// Smoothed eyelid offset (mm toward the opening) at upper-rim parameter t.
float lid_offset_at(const float* off, float t) {
    const float k = std::clamp((t - 0.05f) / 0.9f * (float)kCols - 0.5f, 0.f, (float)(kCols - 1));
    const int i0 = (int)k, i1 = std::min(i0 + 1, kCols - 1);
    return mixf(off[i0], off[i1], k - (float)i0);
}

// Lid frame at rim parameter t: position, surface normal, tangent (toward the
// inner corner) and the returned U — along the lid, away from the eye
// opening. U is oriented by the opposite rim at the same t, plus a fixed bias
// (up for the upper lid, down for the lower) where the rims meet at the
// corners: the eye center sits on the rims of a nearly closed eye, and
// orienting by it flipped U along the rim (lashes and liner jumped sides).
V3 lid_frame(const Poly& rim, const Poly& other, float t, bool upper, V3& pos, V3& n, V3& tan) {
    rim.eval(t, pos, n, tan);
    V3 op, on, ot;
    other.eval(t, op, on, ot);
    const V3 U = normalize(cross(n, tan));
    const V3 ref = (pos - op) + v3(0.f, upper ? 1e-3f : -1e-3f, 0.f);
    return dot(U, ref) < 0.f ? U * -1.f : U;
}

// Strands for one lash row of one eye. Each strand leaves the lash line along
// the lid's surface normal tilted `lift` toward the lid, curls further toward
// the lid along its length, flares outward at the outer corner, and converges
// on its clump's tip (wispy clusters). Lid motion rotates the local frame, so
// blinks carry the fringe with the lid. `lid_off`: the eyelid offsets that
// move the roots onto the real lash line; `width_k` thins the whole row
// (lower lashes disappear as the eye closes).
void build_row(std::vector<LashVtx>& out, const Poly& rim, const Poly& other, bool upper,
               const LashRow& R, float blink, uint32_t seed, const float* lid_off, float width_k) {
    if (R.count <= 0 || width_k < 0.02f) return;
    constexpr int kSeg = 6;
    struct Strand { V3 pts[kSeg + 1]; float w0; int clump; };
    std::vector<Strand> strands((size_t)R.count);
    const float d2r = 3.14159265f / 180.f;
    for (int i = 0; i < R.count; ++i) {
        const uint32_t sd = seed * 977u + (uint32_t)i * 7919u;
        float u0 = ((float)i + 0.5f + (rnd(sd) - 0.5f) * 0.7f) / (float)R.count;
        float t = mixf(R.t0, R.t1, std::clamp(u0, 0.f, 1.f));
        V3 root, n, tan;
        const V3 U = lid_frame(rim, other, t, upper, root, n, tan);   // along the lid, away from the opening
        if (R.blink_close > 0.f && blink > 0.f) {            // ARKit's lids never fully meet
            V3 op, on, ot;
            other.eval(t, op, on, ot);
            root = root + (op - root) * (blink * R.blink_close);
        }
        if (lid_off) root = root - U * (lid_offset_at(lid_off, t) * 1e-3f);
        root = root + U * (R.offset_mm * 1e-3f) + n * 3e-4f; // 0.3 mm stand-off: no z-fight
        float clump_n = R.clumps > 0 ? (float)R.clumps : (float)R.count;
        int ci = std::min((int)(u0 * clump_n), (int)clump_n - 1);
        float wisp = 1.f + R.wisp * (rnd(seed * 31u + (uint32_t)ci * 104729u) * 2.f - 1.f);
        float len = mixf(R.len_outer_mm, R.len_inner_mm, smooth01(t)) * 1e-3f
                  * wisp * (0.9f + 0.2f * rnd(sd + 1u));
        float lift = (R.lift_deg + (rnd(sd + 2u) - 0.5f) * 8.f) * d2r;
        float curl = R.curl_deg * d2r;
        float flare = R.flare * (1.f - t) * (1.f - t) * 0.9f;    // radians toward the temple
        V3 outward = tan * -1.f;
        Strand& S = strands[(size_t)i];
        S.pts[0] = root;
        S.clump = ci;
        S.w0 = R.root_mm * 1e-3f * (0.8f + 0.4f * rnd(sd + 3u)) * width_k;
        for (int j = 1; j <= kSeg; ++j) {
            float s = ((float)j - 0.5f) / (float)kSeg;
            float ang = lift + curl * std::pow(s, 1.4f);
            V3 d = normalize(n * std::cos(ang) + U * std::sin(ang));
            d = normalize(d + outward * std::tan(flare));
            S.pts[j] = S.pts[j - 1] + d * (len / (float)kSeg);
        }
    }
    // Wispy clusters: pull each strand's tip toward its clump's mean tip.
    if (R.clumps > 0 && R.clump > 0.f) {
        std::vector<V3> tip((size_t)R.clumps, v3(0, 0, 0));
        std::vector<int> cnt((size_t)R.clumps, 0);
        for (const Strand& S : strands) { tip[(size_t)S.clump] = tip[(size_t)S.clump] + S.pts[kSeg]; cnt[(size_t)S.clump]++; }
        for (int c = 0; c < R.clumps; ++c) if (cnt[(size_t)c]) tip[(size_t)c] = tip[(size_t)c] * (1.f / (float)cnt[(size_t)c]);
        for (Strand& S : strands) {
            V3 pull = (tip[(size_t)S.clump] - S.pts[kSeg]) * R.clump;
            for (int j = 1; j <= kSeg; ++j) {
                float s = (float)j / (float)kSeg;
                S.pts[j] = S.pts[j] + pull * (s * s);
            }
        }
    }
    for (const Strand& S : strands) {
        for (int j = 0; j < kSeg; ++j) {
            float s0 = (float)j / kSeg, s1 = (float)(j + 1) / kSeg;
            float w0 = S.w0 * (1.f - 0.88f * std::pow(s0, 0.8f));
            float w1 = S.w0 * (1.f - 0.88f * std::pow(s1, 0.8f));
            const float corner[6][2] = {{0, -1}, {0, 1}, {1, -1}, {1, -1}, {0, 1}, {1, 1}};
            for (const auto& cv : corner) {
                if (out.size() >= (size_t)kMaxLashVerts) return;
                LashVtx v{};
                v.p0[0] = S.pts[j].x;     v.p0[1] = S.pts[j].y;     v.p0[2] = S.pts[j].z;
                v.p1[0] = S.pts[j + 1].x; v.p1[1] = S.pts[j + 1].y; v.p1[2] = S.pts[j + 1].z;
                v.w0 = w0; v.w1 = w1;
                v.end = cv[0]; v.side = cv[1];
                v.shade = 1.f + 0.6f * (cv[0] > 0.5f ? s1 : s0);   // tips thin to lighter
                out.push_back(v);
            }
        }
    }
}

}  // namespace

bool arkit_makeup_encode(id<MTLDevice> dev, id<MTLCommandBuffer> cb,
                         id<MTLTexture> src, id<MTLTexture> dst,
                         const std::string& look_id, float amount,
                         double time_s, const char** status) {
    *status = "no_face";
    static ARKitFace3D f;   // ~15 KB; render thread only
    if (!dev || !cb || !src || !dst || !arkit_face3d_take(&f)) return false;
    if (!init_ctx(dev)) { *status = "pso_failed"; return false; }
    const Look* L = get_look(look_id);
    if (!L) { *status = "look_missing"; return false; }
    const int W = (int)dst.width, H = (int)dst.height;
    ensure_targets(W, H);
    if (!g.half0 || !g.half1 || !g.half2 || !g.half_lip || !g.half_ev || !g.depth) {
        *status = "pso_failed";
        return false;
    }

    filter_vertices(f.verts, time_s);
    static float nrm[ARKIT_NPTS][3];
    vertex_normals(f.verts, nrm);
    const int r = g.ring;
    g.ring = (g.ring + 1) % kRing;
    std::memcpy(g.pos[r].contents, f.verts, sizeof(f.verts));
    std::memcpy(g.nrm[r].contents, nrm, sizeof(nrm));

    float mv[16], mvp[16];
    mat4_mul(f.view, f.model, mv);
    mat4_mul(f.proj, mv, mvp);
    const float depth_m = std::max(-mv[14], 0.05f);      // face anchor distance
    const float px_per_m1 = f.proj[5] * (float)H * 0.5f;  // px per meter at 1 m depth
    const float px_per_mm = px_per_m1 / depth_m * 1e-3f;
    const float amt = std::clamp(amount, 0.f, 2.f);

    // ── 1. prep (half res) ──
    MeshUniC mu{};
    std::memcpy(mu.mvp, mvp, sizeof(mvp));
    std::memcpy(mu.mv, mv, sizeof(mv));
    mu.inv_target[0] = 1.f / (float)g.half0.width;
    mu.inv_target[1] = 1.f / (float)g.half0.height;
    auto bind_mesh = [&](id<MTLRenderCommandEncoder> e) {
        [e setVertexBuffer:g.pos[r] offset:0 atIndex:0];
        [e setVertexBuffer:g.nrm[r] offset:0 atIndex:1];
        [e setVertexBuffer:g.uv offset:0 atIndex:2];
        [e setVertexBytes:&mu length:sizeof(mu) atIndex:3];
        [e setCullMode:MTLCullModeBack];
        [e setFrontFacingWinding:MTLWindingCounterClockwise];
    };
    auto draw_mesh = [&](id<MTLRenderCommandEncoder> e) {
        [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:ARKIT_NTRI * 3
                       indexType:MTLIndexTypeUInt16 indexBuffer:g.idx indexBufferOffset:0];
    };
    {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        id<MTLTexture> targets[2] = {g.half0, g.half_lip};
        for (int i = 0; i < 2; ++i) {
            rp.colorAttachments[i].texture = targets[i];
            rp.colorAttachments[i].loadAction = MTLLoadActionClear;
            rp.colorAttachments[i].clearColor = MTLClearColorMake(0, 0, 0, 0);
            rp.colorAttachments[i].storeAction = MTLStoreActionStore;
        }
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:g.prep];
        bind_mesh(e);
        [e setFragmentBytes:&mu length:sizeof(mu) atIndex:0];
        [e setFragmentTexture:src atIndex:0];
        [e setFragmentTexture:L->mask_a atIndex:1];
        [e setFragmentTexture:L->mask_b atIndex:2];
        draw_mesh(e);
        [e endEncoding];
    }
    // ── 2. blur (half res): σ = smooth_mm on the face, whatever the distance ──
    {
        const float sigma_half_px = L->smooth_mm * px_per_mm * 0.5f;
        const float tap = std::max(sigma_half_px / 3.f, 0.5f);   // kernel σ is 3 taps
        auto pass = [&](id<MTLTexture> in, id<MTLTexture> out, float sx, float sy) {
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = out;
            rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            [e setRenderPipelineState:g.blur];
            BlurUniC bu{};
            bu.step[0] = sx; bu.step[1] = sy;
            bu.range = 0.14f;
            [e setFragmentBytes:&bu length:sizeof(bu) atIndex:0];
            [e setFragmentTexture:in atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [e endEncoding];
        };
        pass(g.half0, g.half1, tap / (float)g.half0.width, 0.f);
        pass(g.half1, g.half2, 0.f, tap / (float)g.half0.height);
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl generateMipmapsForTexture:g.half2];            // 1×1 level = face-mean skin
        [bl generateMipmapsForTexture:g.half_lip];         // 1×1 level = mean lip color
        [bl copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake((NSUInteger)W, (NSUInteger)H, 1)
                  toTexture:dst destinationSlice:0 destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
        [bl endEncoding];
    }

    // ── 2b. lip evidence (half res) ──
    {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = g.half_ev;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:g.lipev];
        bind_mesh(e);
        EvUniC eu{};
        eu.inv_src[0] = 1.f / (float)W;
        eu.inv_src[1] = 1.f / (float)H;
        eu.mean_level = (float)(g.half2.mipmapLevelCount - 1);
        [e setFragmentBytes:&mu length:sizeof(mu) atIndex:0];
        [e setFragmentBytes:&eu length:sizeof(eu) atIndex:1];
        [e setFragmentTexture:src atIndex:0];
        [e setFragmentTexture:g.half2 atIndex:1];
        [e setFragmentTexture:L->mask_b atIndex:2];
        draw_mesh(e);
        [e endEncoding];
    }

    // ── 3. face composite (full res) ──
    Poly upR, upL, loR, loL;
    upR.build(kUpperR, 12, f.verts, nrm);
    upL.build(kUpperL, 12, f.verts, nrm);
    loR.build(kLowerR, 14, f.verts, nrm);
    loL.build(kLowerL, 14, f.verts, nrm);

    FaceUniC fu{};
    fu.inv_target[0] = 1.f / (float)W;
    fu.inv_target[1] = 1.f / (float)H;
    fu.amount = amt;
    fu.overlay = g_face_overlay ? 1.f : 0.f;
    fu.skin[0] = L->smooth; fu.skin[1] = L->even; fu.skin[2] = L->lift;
    fu.skin[3] = (float)(g.half2.mipmapLevelCount - 1);
    std::memcpy(fu.blush, L->blush, sizeof(fu.blush));
    std::memcpy(fu.shadow, L->shadow, sizeof(fu.shadow));
    std::memcpy(fu.freckle, L->freckle, sizeof(fu.freckle));
    fu.misc[0] = L->inner_light; fu.misc[1] = L->ref_luma;
    std::memcpy(fu.lip_cream, L->lip_cream, sizeof(fu.lip_cream));
    fu.lip_shape[0] = L->overline_mm; fu.lip_shape[1] = L->lip_match;
    fu.lip_shape[2] = L->gloss;       fu.lip_shape[3] = L->gloss_rough;
    fu.hl[0] = L->hl_amt; fu.hl[1] = L->hl_rough; fu.hl[2] = L->hl_sheen;
    std::memcpy(fu.ink, L->ink, sizeof(fu.ink));

    // Light: ARKit's primary direction is world space and points the way the
    // light travels; the shader wants the view-space direction TO the light.
    V3 to_light = v3(0.25f, 0.55f, 0.80f);   // fallback key light: above-front
    float gain = 0.8f;
    if (f.light.directional) {
        to_light = mat4_dir(f.view, v3(f.light.dir[0], f.light.dir[1], f.light.dir[2])) * -1.f;
        gain = std::clamp(f.light.intensity / 1000.f, 0.25f, 1.5f);
    }
    to_light = normalize(to_light);
    fu.light_dir[0] = to_light.x; fu.light_dir[1] = to_light.y; fu.light_dir[2] = to_light.z;
    fu.light_dir[3] = gain;
    kelvin_rgb(f.light.kelvin, fu.light_col);

    // Eye centers (rim means) orient "up the lid" for liner and lashes.
    const Poly* uppers[2] = {&upR, &upL};
    const Poly* lowers[2] = {&loR, &loL};
    V3 eye_c[2] = {v3(0, 0, 0), v3(0, 0, 0)};
    for (int e = 0; e < 2; ++e) {
        for (const V3& p : uppers[e]->p) eye_c[e] = eye_c[e] + p;
        for (const V3& p : lowers[e]->p) eye_c[e] = eye_c[e] + p;
        eye_c[e] = eye_c[e] * (1.f / (float)(uppers[e]->p.size() + lowers[e]->p.size()));
    }

    // ── Eyelid refinement: apply the latest MediaPipe result (see LidWorker) ──
    auto to_px = [&](V3 p, float& x, float& y) {
        const float cx = mvp[0] * p.x + mvp[4] * p.y + mvp[8] * p.z + mvp[12];
        const float cy = mvp[1] * p.x + mvp[5] * p.y + mvp[9] * p.z + mvp[13];
        const float iw = 1.f / std::max(mvp[3] * p.x + mvp[7] * p.y + mvp[11] * p.z + mvp[15], 1e-6f);
        x = (cx * iw * 0.5f + 0.5f) * (float)W;
        y = (0.5f - cy * iw * 0.5f) * (float)H;
    };
    const float blink_e[2] = {f.has_blend ? f.blend[kBlinkR] : 0.f,
                              f.has_blend ? f.blend[kBlinkL] : 0.f};
    if (!lid.q) lid.q = dispatch_queue_create("pms.arkit.eyelid", DISPATCH_QUEUE_SERIAL);
    static const bool lid_sync = getenv("PMS_ARKIT_SYNC") != nullptr;   // replay: deterministic
    if (lid_sync)
        for (int i = 0; i < 4000 && lid.done.load() != lid.submitted.load(); ++i)
            std::this_thread::sleep_for(std::chrono::microseconds(500));
    LidResult lr;
    { std::lock_guard<std::mutex> lk(lid.mu); lr = lid.latest; }
    const bool new_face = g.lid_t < 0.0 || time_s < g.lid_t || time_s - g.lid_t > 0.3;
    g.lid_t = time_s;
    const bool fresh = lr.t >= 0.0 && std::fabs(time_s - lr.t) < 0.35;
    // No blink fade: ARKit's blink stays low while the real lids are half
    // down (looking at the screen) — exactly when the correction matters.
    for (int e = 0; e < 2; ++e)
        for (int l = 0; l < 2; ++l) {
            const float w = fresh ? lr.conf[e][l] : 0.f;
            for (int k = 0; k < kCols; ++k) {
                const float tgt = std::clamp(lr.off[e][l][k], -3.f, 3.f) * w;
                // The net's contour jitters ±0.5 mm frame to frame on a still
                // face; real lid moves (gaze shifts, blinks) are bigger.
                float& cur = g.lid_off[e][l][k];
                const float delta = tgt - cur;
                const float alpha = std::clamp(0.12f + 0.4f * std::fabs(delta), 0.12f, 0.75f);
                cur = new_face ? tgt : cur + alpha * delta;
            }
        }

    // Liner: one centerline per eye. Along the lash line (ARKit's rim moved by
    // the eyelid offsets) from the inner corner to just short of the outer
    // corner (where the rim turns down into the lower lid), lifted half a
    // stroke onto the lid so the ink's lower edge sits on the lash line; then
    // a quadratic wing that leaves in the lash line's own direction and bends
    // to the look's wing angle — one stroke, so band and wing never cross.
    const float w_in = L->liner_in_mm * 1e-3f, w_out = L->liner_out_mm * 1e-3f;
    const float off = L->liner_offset_mm * 1e-3f, wing_len = L->wing_mm * 1e-3f;
    const float t_end = 0.12f;
    const float lift = L->wing_lift_deg * 3.14159265f / 180.f;
    for (int e = 0; e < 2; ++e) {
        const Poly& P = *uppers[e];
        float (*S)[4] = &fu.stroke[e * 16];
        V3 pos, n, tan;
        for (int i = 0; i < 10; ++i) {
            const float t = 1.f - (1.f - t_end) * (float)i / 9.f;    // inner → outer
            const V3 U = lid_frame(P, *lowers[e], t, true, pos, n, tan);
            const float w = mixf(w_out, w_in, smooth01(t))
                          * (1.f - 0.6f * smooth01((t - 0.8f) / 0.2f));
            const V3 c = pos + U * (0.5f * w + off - lid_offset_at(g.lid_off[e][0], t) * 1e-3f);
            S[i][0] = c.x; S[i][1] = c.y; S[i][2] = c.z; S[i][3] = w;
        }
        // `tan` is the lash line's direction at t_end (toward the inner corner).
        const V3 nO = P.n.front();
        const V3 axis = normalize(P.p.front() - P.p.back());        // inner → outer corner
        V3 lead = normalize(tan * -1.f + axis);
        lead = normalize(lead - nO * dot(lead, nO));
        V3 d = normalize(axis * std::cos(lift) + v3(0, 1, 0) * std::sin(lift));
        d = normalize(d - nO * dot(d, nO));
        const V3 A = v3(S[9][0], S[9][1], S[9][2]);
        const V3 C = A + lead * (0.35f * wing_len), T = A + d * wing_len;
        for (int j = 1; j <= 6; ++j) {
            const float s = (float)j / 6.f;
            const V3 q = A * ((1.f - s) * (1.f - s)) + C * (2.f * (1.f - s) * s) + T * (s * s);
            S[9 + j][0] = q.x; S[9 + j][1] = q.y; S[9 + j][2] = q.z;
            S[9 + j][3] = w_out * std::pow(1.f - s, 0.9f);
        }
        fu.corner_n[e][0] = nO.x; fu.corner_n[e][1] = nO.y; fu.corner_n[e][2] = nO.z;
    }

    // Lash strands (anchor space); roots ride the eyelid offsets. Lower lashes
    // fade out as the eye closes — judged by the real opening (ARKit's, closed
    // by both lid corrections), since ARKit's blink lags lowered lids; on a
    // closed eye they are tucked under the upper fringe, drawn they smudge.
    static std::vector<LashVtx> lv;
    lv.clear();
    if (L->lash_amt > 0.f) {
        float lower_k[2];
        for (int e = 0; e < 2; ++e) {
            V3 pu, nu, tu, pl, nl, tl;
            uppers[e]->eval(0.5f, pu, nu, tu);
            lowers[e]->eval(0.5f, pl, nl, tl);
            const float open_mm = length(pu - pl) * 1e3f - lid_offset_at(g.lid_off[e][0], 0.5f)
                                - lid_offset_at(g.lid_off[e][1], 0.5f);
            lower_k[e] = smooth01((open_mm - 2.f) / 3.f) * (1.f - smooth01((blink_e[e] - 0.5f) / 0.35f));
        }
        build_row(lv, upR, loR, true, L->upper, blink_e[0], 11u, g.lid_off[0][0], 1.f);
        build_row(lv, upL, loL, true, L->upper, blink_e[1], 12u, g.lid_off[1][0], 1.f);
        build_row(lv, loR, upR, false, L->lower, blink_e[0], 21u, g.lid_off[0][1], lower_k[0]);
        build_row(lv, loL, upL, false, L->lower, blink_e[1], 22u, g.lid_off[1][1], lower_k[1]);
        if (!lv.empty()) std::memcpy(g.lashv[r].contents, lv.data(), sizeof(LashVtx) * lv.size());
    }

    // ── 3a. eyelid crop → worker (one job in flight, ≤ 30 Hz: lids change
    // slowly; the smoothing absorbs the latency) ──
    if (lid.done.load() == lid.submitted.load() &&
        (lid_sync || g.lid_job_t < 0.0 || time_s < g.lid_job_t || time_s - g.lid_job_t >= 1.0 / 30.0)) {
        g.lid_job_t = time_s;
        LidJob job{};
        job.t = time_s;
        float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
        for (int i = 0; i < ARKIT_NPTS; ++i) {
            float x, y;
            to_px(v3(f.verts[i][0], f.verts[i][1], f.verts[i][2]), x, y);
            bx0 = std::min(bx0, x); bx1 = std::max(bx1, x);
            by0 = std::min(by0, y); by1 = std::max(by1, y);
        }
        // The landmark net was trained on detector-shaped crops: 1.6× a face
        // box sitting a little higher than the mesh (forehead included).
        job.cx = 0.5f * (bx0 + bx1);
        job.cy = 0.5f * (by0 + by1) - 0.05f * (by1 - by0);
        job.size = std::max(bx1 - bx0, by1 - by0) * 1.28f;
        float rx, ry, lx, ly;
        to_px(upR.p.front(), rx, ry);
        to_px(upL.p.front(), lx, ly);
        job.roll = std::atan2(ly - ry, lx - rx);
        for (int e = 0; e < 2; ++e) {
            to_px(eye_c[e], job.eye_c[e][0], job.eye_c[e][1]);
            // Facing: the rim's mean surface normal vs. the direction to the camera.
            V3 nsum = v3(0, 0, 0);
            for (const V3& nn : uppers[e]->n) nsum = nsum + nn;
            const V3 nv = normalize(mat4_dir(mv, nsum));
            const V3 pv = mat4_dir(mv, eye_c[e]) + v3(mv[12], mv[13], mv[14]);
            job.facing[e] = dot(nv, normalize(pv * -1.f));
            for (int l = 0; l < 2; ++l)   // rays from ARKit's rims toward the opening
                for (int k = 0; k < kCols; ++k) {
                    const float t = 0.05f + 0.9f * ((float)k + 0.5f) / (float)kCols;
                    V3 pos, n, tan;
                    const V3 U = lid_frame(l == 0 ? *uppers[e] : *lowers[e],
                                           l == 0 ? *lowers[e] : *uppers[e], t, l == 0, pos, n, tan);
                    float ax, ay, bx, by;
                    to_px(pos, ax, ay);
                    to_px(pos - U * 1e-3f, bx, by);
                    const float dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
                    LidRay& R = job.ray[e][l][k];
                    R.p[0] = ax; R.p[1] = ay;
                    R.d[0] = len > 1e-4f ? dx / len : 0.f;
                    R.d[1] = len > 1e-4f ? dy / len : 1.f;
                    R.ppmm = len;
                }
        }
        CropUniC cu{};
        cu.c[0] = job.cx; cu.c[1] = job.cy; cu.size = job.size; cu.roll = job.roll;
        cu.inv_src[0] = 1.f / (float)W; cu.inv_src[1] = 1.f / (float)H;
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = g.crop_tex;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> ce = [cb renderCommandEncoderWithDescriptor:rp];
        [ce setRenderPipelineState:g.crop];
        [ce setFragmentBytes:&cu length:sizeof(cu) atIndex:0];
        [ce setFragmentTexture:src atIndex:0];
        [ce drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [ce endEncoding];
        id<MTLBlitCommandEncoder> cbl = [cb blitCommandEncoder];
        [cbl copyFromTexture:g.crop_tex sourceSlice:0 sourceLevel:0
                sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(kCrop, kCrop, 1)
                    toBuffer:g.crop_buf destinationOffset:0
      destinationBytesPerRow:(NSUInteger)kCrop * 4
    destinationBytesPerImage:(NSUInteger)kCrop * kCrop * 4];
        [cbl endEncoding];
        lid.submitted.fetch_add(1);
        id<MTLBuffer> buf = g.crop_buf;
        [cb addCompletedHandler:^(id<MTLCommandBuffer> done_cb) {
            if (done_cb.status != MTLCommandBufferStatusCompleted) { lid.done.fetch_add(1); return; }
            dispatch_async(lid.q, ^{ lid_run((const uint8_t*)buf.contents, job); });
        }];
    }

    {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = dst;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.depthAttachment.texture = g.depth;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 1.0;
        rp.depthAttachment.storeAction = MTLStoreActionDontCare;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        mu.inv_target[0] = 1.f / (float)W;
        mu.inv_target[1] = 1.f / (float)H;
        [e setRenderPipelineState:g.face];
        [e setDepthStencilState:g.dss_face];
        bind_mesh(e);
        [e setFragmentBytes:&fu length:sizeof(fu) atIndex:0];
        [e setFragmentTexture:src atIndex:0];
        [e setFragmentTexture:g.half2 atIndex:1];
        [e setFragmentTexture:L->mask_a atIndex:2];
        [e setFragmentTexture:L->mask_b atIndex:3];
        [e setFragmentTexture:g.half_lip atIndex:4];
        [e setFragmentTexture:g.half_ev atIndex:5];
        draw_mesh(e);
        if (!lv.empty() && !g_face_overlay) {
            LashUniC lu{};
            std::memcpy(lu.mvp, mvp, sizeof(mvp));
            lu.half_target[0] = 0.5f * (float)W;
            lu.half_target[1] = 0.5f * (float)H;
            lu.px_per_m = px_per_m1;
            lu.alpha = L->lash_amt * std::min(amt, 1.f);
            lu.color[0] = L->lash_srgb[0]; lu.color[1] = L->lash_srgb[1];
            lu.color[2] = L->lash_srgb[2]; lu.color[3] = 1.f;
            [e setRenderPipelineState:g.lash];
            [e setDepthStencilState:g.dss_lash];
            [e setCullMode:MTLCullModeNone];
            [e setVertexBuffer:g.lashv[r] offset:0 atIndex:0];
            [e setVertexBytes:&lu length:sizeof(lu) atIndex:1];
            [e setFragmentBytes:&lu length:sizeof(lu) atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
                  vertexCount:(NSUInteger)lv.size()];
        }
        [e endEncoding];
    }
    *status = "applied";
    return true;
}
