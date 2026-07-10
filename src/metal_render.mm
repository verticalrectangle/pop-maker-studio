// metal_render.mm — Metal RenderSurface backend (Phase 3, iOS).
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#include "metal_render.h"
#include "app.h"          // AppState/Track/Clip — the scene compositor walks these
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
                int         body_pass = -1; };      // index into the body pass table, -1 unknown
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
                                   int sw, int sh,
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

        id<MTLRenderPipelineState> pso = nil;
        const ManifestEntry* mf = nullptr;
        bool is_body = (fx.fx_type == "body_fx");
        if (is_body) {
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
        if (is_body) {
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
        fx.fx_type = named_fx_id(cl.fx_type);
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
    const float t = st.playhead;   // FX windows + animation run on timeline time
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
                tex = run_fx_stack(cb, tex, lf.w, lf.h, stack, t, t, matte, &sts, &rr);
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
                id<MTLTexture> out = run_fx_stack(cb, g_scene[cur], w, h, bus, t, t,
                                                  matte, &sts, &rr);
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
    if (source && !stack.empty()) {
        FxRunResult rr;
        source = run_fx_stack(cb, source, sw, sh, stack, t, g_content_time,
                              matte, &statuses, &rr);
        if (rr.pso_failed) rc0 = 4;
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
        if (matte && !g_matte_pso) {
            // Matte requested but the composite pipeline failed to build — a
            // real error, not a silent skip. Blit plain and report it.
            static bool s_logged = false;
            if (!s_logged) { s_logged = true; NSLog(@"[metal_render] matte pso unavailable — compositing without matte"); }
            rc = 2;
        }
        if (matte && g_matte_pso) {
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
