// metal_render.mm — Metal RenderSurface backend (Phase 3, iOS).
#import <Metal/Metal.h>
#include "metal_render.h"

// ── Inline MSL: full-screen triangle + a soft aurora over near-black. This is
// the "canvas is alive" state (matches the app's lavender accent) and, more
// importantly, proves the whole path: engine → command queue → pipeline →
// the app's drawable texture, per frame. The scene compositor replaces the
// fragment body; the plumbing here stays.
static NSString* const kSrc = @R"(
#include <metal_stdlib>
using namespace metal;

struct VOut { float4 pos [[position]]; float2 uv; };

vertex VOut vmain(uint vid [[vertex_id]]) {
    // one oversized triangle covering the viewport
    float2 p = float2((vid << 1) & 2, vid & 2);   // (0,0)(2,0)(0,2)
    VOut o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv  = float2(p.x, 1.0 - p.y);               // top-left origin
    return o;
}

struct FUni { float t; float2 res; };

fragment float4 fmain(VOut in [[stage_in]], constant FUni& u [[buffer(0)]]) {
    float2 uv = in.uv;
    // near-black base with a subtle vertical lift
    float3 base = mix(float3(0.02, 0.02, 0.03), float3(0.05, 0.04, 0.07), uv.y);
    // two slow lavender bands drifting with time — soft, low-alpha
    float3 lav = float3(0.71, 0.66, 1.0);
    float b1 = 0.14 * exp(-pow((uv.y - 0.35 + 0.10 * sin(u.t * 0.6 + uv.x * 3.1)) * 4.0, 2.0));
    float b2 = 0.10 * exp(-pow((uv.y - 0.65 + 0.08 * sin(u.t * 0.4 - uv.x * 2.3)) * 5.0, 2.0));
    float3 col = base + lav * (b1 + b2);
    return float4(col, 1.0);
}
)";

static id<MTLDevice>              g_dev   = nil;
static id<MTLCommandQueue>        g_queue = nil;
static id<MTLRenderPipelineState> g_pso   = nil;

void metal_render_init(void* mtl_device) {
    if (g_dev) return;
    g_dev = (__bridge id<MTLDevice>)mtl_device;
    if (!g_dev) return;
    g_queue = [g_dev newCommandQueue];

    NSError* err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:kSrc options:nil error:&err];
    if (!lib) { NSLog(@"[metal_render] library: %@", err); return; }

    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction   = [lib newFunctionWithName:@"vmain"];
    d.fragmentFunction = [lib newFunctionWithName:@"fmain"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;   // matches MTKView
    g_pso = [g_dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!g_pso) NSLog(@"[metal_render] pipeline: %@", err);
}

int metal_render_frame(void* mtl_texture, int w, int h, double t) {
    if (!g_queue || !g_pso || !mtl_texture) return 1;
    id<MTLTexture> target = (__bridge id<MTLTexture>)mtl_texture;

    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor new];
    rp.colorAttachments[0].texture     = target;
    rp.colorAttachments[0].loadAction  = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor  = MTLClearColorMake(0.02, 0.02, 0.03, 1.0);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLCommandBuffer>        cb  = [g_queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:g_pso];
    struct { float t; float rx, ry; } uni = { (float)t, (float)w, (float)h };
    [enc setFragmentBytes:&uni length:sizeof(uni) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    [cb commit];
    return 0;
}
