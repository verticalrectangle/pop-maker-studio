// metal_render.mm — Metal RenderSurface backend (Phase 3, iOS).
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#include "metal_render.h"
#include <vector>
#include <mutex>

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
// Live-FX applied inline in the blit (first increment: one hand-written effect
// proves the Swift-lever → engine → Metal-FX → preview path; the transpiled
// shader library for all 109 rides a real multi-pass chain afterward).
struct FxUni { int type; float amount; };
fragment float4 quad_f(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]],
                       constant FxUni& fx [[buffer(0)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    if (fx.type == 1) {                          // chromatic_aberration
        float2 off = (in.uv - 0.5) * (fx.amount * 0.03);
        float r = tex.sample(s, in.uv + off).r;
        float g = tex.sample(s, in.uv).g;
        float b = tex.sample(s, in.uv - off).b;
        return float4(r, g, b, 1.0);
    }
    return float4(tex.sample(s, in.uv).rgb, 1.0);
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
static int                        g_fx_type   = 0;    // live-FX applied in the blit (0 = none)
static float                      g_fx_amount = 0.0f;

// Set the render-time FX applied to the current frame (from pms_render, off the
// engine's live_fx state). type 0 = none; 1 = chromatic_aberration.
void metal_render_set_live_fx(int type, float amount) {
    g_fx_type = type; g_fx_amount = amount;
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

    CVMetalTextureCacheCreate(kCFAllocatorDefault, NULL, g_dev, NULL, &g_texcache);
}

// Release any zero-copy CVMetalTexture mapping (call under g_content_mu).
static void release_cvtex() {
    if (g_cvtex) { CFRelease(g_cvtex); g_cvtex = NULL; }
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

    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor new];
    rp.colorAttachments[0].texture     = target;
    rp.colorAttachments[0].loadAction  = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor  = MTLClearColorMake(0.02, 0.02, 0.03, 1.0);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLCommandBuffer>        cb  = [g_queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];

    // (1) aurora background / letterbox
    [enc setRenderPipelineState:g_bg_pso];
    struct { float t; float rx, ry; } bgu = { (float)t, (float)w, (float)h };
    [enc setFragmentBytes:&bgu length:sizeof(bgu) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

    // (2) content frame, aspect-fit over the background
    {
        std::lock_guard<std::mutex> lk(g_content_mu);
        if (g_content && g_quad_pso && g_cw > 0 && g_ch > 0) {
            float ta = (float)w / (float)h;
            float ca = (float)g_cw / (float)g_ch;
            float sx = ca > ta ? 1.0f : ca / ta;   // half-extents in NDC (aspect-fit)
            float sy = ca > ta ? ta / ca : 1.0f;
            [enc setRenderPipelineState:g_quad_pso];
            float half[2] = { sx, sy };
            [enc setVertexBytes:half length:sizeof(half) atIndex:0];
            struct { int type; float amount; } fxu = { g_fx_type, g_fx_amount };
            [enc setFragmentBytes:&fxu length:sizeof(fxu) atIndex:0];
            [enc setFragmentTexture:g_content atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
    }

    [enc endEncoding];
    [cb commit];
    return 0;
}
