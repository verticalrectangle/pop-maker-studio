// metal_render.mm — Metal RenderSurface backend (Phase 3, iOS).
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#include "metal_render.h"
#include "json.hpp"
#include <vector>
#include <mutex>
#include <map>
#include <unordered_map>
#include <string>
#include <cstring>

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
static id<MTLTexture>             g_ping[2]       = { nil, nil };
static int                        g_pw = 0, g_ph = 0;
static id<MTLSamplerState>        g_fx_sampler    = nil;
static std::string                g_shader_dir;            // optional override (headless/test)

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

static void ensure_ping(int w, int h) {
    if (g_ping[0] && g_pw == w && g_ph == h) return;
    MTLTextureDescriptor* td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    g_ping[0] = [g_dev newTextureWithDescriptor:td];
    g_ping[1] = [g_dev newTextureWithDescriptor:td];
    g_pw = w; g_ph = h;
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

int metal_render_frame(void* mtl_texture, int w, int h, double t) {
    if (!g_queue || !g_bg_pso || !mtl_texture) return 1;
    id<MTLTexture> target = (__bridge id<MTLTexture>)mtl_texture;

    // Capture the content texture (ARC + Metal command-buffer retention keep it
    // alive across the async passes even if a new frame is submitted meanwhile).
    // The matte is captured under the same lock so content+matte stay a pair;
    // its CVMetalTextureRef is retained until the command buffer completes.
    id<MTLTexture> source = nil; int sw = 0, sh = 0;
    id<MTLTexture> matte = nil; CVMetalTextureRef matte_cvtex = NULL;
    { std::lock_guard<std::mutex> lk(g_content_mu);
      if (g_content && g_cw > 0 && g_ch > 0) { source = g_content; sw = g_cw; sh = g_ch; }
      if (g_matte_tex && g_matte_cvtex) {
          matte = g_matte_tex;
          matte_cvtex = (CVMetalTextureRef)CFRetain(g_matte_cvtex);
      } }

    // Run the FX chain (ping-pong) → `source` becomes the last pass output.
    if (source) {
        std::vector<LiveFx> stack;
        { std::lock_guard<std::mutex> lk(g_stack_mu); stack = g_stack; }
        if (!stack.empty()) {
            ensure_ping(sw, sh);
            id<MTLCommandBuffer> fxcb = [g_queue commandBuffer];
            id<MTLTexture> cur = source; int dst = 0;
            std::vector<uint8_t> pbuf;
            for (auto& fx : stack) {
                if (!(fx.start <= g_content_time && g_content_time < fx.end)) continue;  // out of brick span
                FxProgram* p = get_fx_program(fx.fx_type);
                if (!p || !g_ping[dst]) continue;                 // unknown effect → skip
                MTLRenderPassDescriptor* rpF = [MTLRenderPassDescriptor new];
                rpF.colorAttachments[0].texture = g_ping[dst];
                rpF.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                rpF.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> e = [fxcb renderCommandEncoderWithDescriptor:rpF];
                [e setRenderPipelineState:p->pso];
                fill_params(*p->m, fx, sw, sh, t, pbuf);
                if (!pbuf.empty()) [e setFragmentBytes:pbuf.data() length:pbuf.size() atIndex:0];
                [e setFragmentTexture:cur atIndex:0];
                [e setFragmentSamplerState:g_fx_sampler atIndex:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [e endEncoding];
                id<MTLTexture> post = g_ping[dst]; dst ^= 1;
                if (fx.amount < 0.999f && g_fx_blend_pso && g_ping[dst]) {   // wet/dry blend
                    MTLRenderPassDescriptor* rpB = [MTLRenderPassDescriptor new];
                    rpB.colorAttachments[0].texture = g_ping[dst];
                    rpB.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                    rpB.colorAttachments[0].storeAction = MTLStoreActionStore;
                    id<MTLRenderCommandEncoder> eb = [fxcb renderCommandEncoderWithDescriptor:rpB];
                    [eb setRenderPipelineState:g_fx_blend_pso];
                    struct { float amt; } bu = { fx.amount };
                    [eb setFragmentBytes:&bu length:sizeof(bu) atIndex:0];
                    [eb setFragmentTexture:cur atIndex:0];
                    [eb setFragmentTexture:post atIndex:1];
                    [eb setFragmentSamplerState:g_fx_sampler atIndex:0];
                    [eb drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                    [eb endEncoding];
                    cur = g_ping[dst]; dst ^= 1;
                } else cur = post;
            }
            [fxcb commit];
            source = cur;
        }
    }

    // Main pass: aurora background + aspect-fit blit of `source`.
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor new];
    rp.colorAttachments[0].texture     = target;
    rp.colorAttachments[0].loadAction  = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor  = MTLClearColorMake(0.02, 0.02, 0.03, 1.0);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer>        cb  = [g_queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:g_bg_pso];
    struct { float t; float rx, ry; } bgu = { (float)t, (float)w, (float)h };
    [enc setFragmentBytes:&bgu length:sizeof(bgu) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    int rc = 0;
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
            // Matte and content are the same camera frame, so the identical
            // aspect-fit quad + normalized UVs sample both.
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
    if (matte_cvtex) {
        // Keep the CVMetalTexture mapping (and its IOSurface) alive until the
        // GPU has finished sampling it, even if a new matte replaces the global.
        CVMetalTextureRef held = matte_cvtex;
        [cb addCompletedHandler:^(id<MTLCommandBuffer>) { CFRelease(held); }];
    }
    [cb commit];
    return rc;
}
