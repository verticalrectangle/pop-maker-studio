// face_fx_smoke.mm — minimal reproduction of the record-mode face_fx crash.
// Exercises the full live path: face_track_enable → camera side-feed →
// worker inference → ensure_sessions → render face_fx on macOS Metal.
//
// Set PMS_ASSET_ROOT (default $HOME/dev/pms-ios/Engine/EngineAssets) and
// PMS_SHADER_DIR (default $HOME/dev/pms-ios/Shaders/msl).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

// CarbonCore's AIFF.h (pulled in by CoreVideo) has a legacy `struct Marker`
// that collides with the engine's struct Marker in app.h.
#define Marker PMSCarbonAIFFMarker
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#undef Marker

#include "../src/pms_engine.h"
#include "../src/metal_render.h"
#include "stb_image.h"
#include "json.hpp"

using json = nlohmann::json;

static const int W = 360, H = 640;

static pms_engine*        g_e      = nullptr;
static id<MTLDevice>      g_dev    = nil;
static id<MTLTexture>     g_target = nil;
static id<MTLCommandQueue> g_rq    = nil;

static void fail(const std::string& msg) {
    fprintf(stderr, "face fx smoke: FAIL %s\n", msg.c_str());
    exit(1);
}

static std::string fx_debug_str() {
    if (!g_e) return "{}";
    char* r = pms_command(g_e, "{\"id\":\"d\",\"method\":\"fx_debug\",\"params\":{}}");
    std::string s = r ? r : "{}";
    pms_free(r);
    return s;
}

static json cmd(const std::string& method, const json& params = json::object()) {
    json req = {{"id", "t"}, {"method", method}, {"params", params}};
    char* r = pms_command(g_e, req.dump().c_str());
    std::string out = r ? r : "";
    pms_free(r);
    json reply = json::parse(out, nullptr, false);
    if (reply.is_discarded()) fail(method + ": unparsable reply: " + out);
    if (reply.contains("error")) fail(method + " errored: " + reply["error"].dump());
    return reply.value("result", json::object());
}

struct Img { std::vector<uint8_t> px; };

static Img read_target() {
    id<MTLCommandBuffer> cb = [g_rq commandBuffer];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
#if TARGET_OS_OSX || defined(__MAC_OS_X_VERSION_MIN_REQUIRED)
    if (g_target.storageMode == MTLStorageModeManaged) [bl synchronizeResource:g_target];
#endif
    [bl endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    Img im;
    im.px.resize((size_t)W * H * 4);
    [g_target getBytes:im.px.data() bytesPerRow:(NSUInteger)W * 4
            fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    return im;
}

static Img render_frame(const std::string& tag) {
    int rc = pms_render(g_e, (__bridge void*)g_target, W, H);
    if (rc != 0) fail(tag + ": pms_render rc=" + std::to_string(rc));
    pms_render_wait(g_e);
    return read_target();
}

static int px_diff(const uint8_t* a, const uint8_t* b) {
    int d = 0;
    for (int c = 0; c < 3; ++c) d = std::max(d, std::abs((int)a[c] - (int)b[c]));
    return d;
}

static double frac_diff(const Img& a, const Img& b, int tol) {
    long n = 0, diff = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            ++n;
            const uint8_t* pa = a.px.data() + ((size_t)y * W + x) * 4;
            const uint8_t* pb = b.px.data() + ((size_t)y * W + x) * 4;
            if (px_diff(pa, pb) > tol) ++diff;
        }
    return n ? (double)diff / (double)n : 0.0;
}

static CVPixelBufferRef load_test_face_png(const std::string& path) {
    int iw = 0, ih = 0, n = 0;
    unsigned char* rgb = stbi_load(path.c_str(), &iw, &ih, &n, 3);
    if (!rgb) fail("cannot load " + path);
    NSDictionary* attrs = @{ (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
                             (id)kCVPixelBufferMetalCompatibilityKey: @YES };
    CVPixelBufferRef pb = NULL;
    CVReturn r = CVPixelBufferCreate(kCFAllocatorDefault, iw, ih, kCVPixelFormatType_32BGRA,
                                     (__bridge CFDictionaryRef)attrs, &pb);
    if (r != kCVReturnSuccess || !pb) fail("camera pixel buffer alloc failed");
    CVPixelBufferLockBaseAddress(pb, 0);
    uint8_t* dst = (uint8_t*)CVPixelBufferGetBaseAddress(pb);
    size_t bpr = CVPixelBufferGetBytesPerRow(pb);
    for (int y = 0; y < ih; ++y)
        for (int x = 0; x < iw; ++x) {
            const unsigned char* px = rgb + ((size_t)y * iw + x) * 3;
            uint8_t* q = dst + (size_t)y * bpr + (size_t)x * 4;
            q[0] = px[2]; q[1] = px[1]; q[2] = px[0]; q[3] = 255;
        }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    stbi_image_free(rgb);
    return pb;
}

int main() {
    @autoreleasepool {
        const char* sd = getenv("PMS_SHADER_DIR");
        std::string shader_dir = sd ? sd : std::string(getenv("HOME") ? getenv("HOME") : "")
                                              + "/dev/pms-ios/Shaders/msl";
        metal_render_set_shader_dir(shader_dir.c_str());

        g_dev = MTLCreateSystemDefaultDevice();
        if (!g_dev) { fprintf(stderr, "face fx smoke: FAIL no Metal device\n"); return 2; }

        const char* asset_root = getenv("PMS_ASSET_ROOT");
        if (!asset_root) asset_root = "";
        g_e = pms_create((__bridge void*)g_dev, asset_root, "/tmp/pms-face-fx-smoke");
        if (!g_e) fail("pms_create failed");

        json dbg = cmd("fx_debug");
        if (dbg.value("manifest_count", 0) < 50)
            fail("FX manifest not loaded from '" + shader_dir + "'");

        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeManaged;
        g_target = [g_dev newTextureWithDescriptor:td];
        g_rq = [g_dev newCommandQueue];

        json r = cmd("face_track_enable", {{"on", true}});
        if (!r.value("models_present", false)) {
            printf("face fx smoke: SKIP (face models missing)\n");
            return 0;
        }

        CVPixelBufferRef pb = load_test_face_png("assets/test_face.png");

        bool locked = false;
        for (int k = 0; k < 100 && !locked; ++k) {
            pms_submit_camera_frame(g_e, pb, 0, 0.1 + k * 0.03);
            usleep(50 * 1000);
            locked = cmd("face_debug").value("valid", false);
        }
        if (!locked) fail("face worker never produced a valid observation");

        cmd("set_live_fx", {{"fx", json::array()}});
        pms_submit_camera_frame(g_e, pb, 0, 0.1);
        Img plain = render_frame("plain");

        cmd("set_live_fx",
            {{"fx", json::array({ {{"fx_type", "face_fx"},
                                   {"params", {{"face_filter", 14.0},
                                               {"face_amount", 1.0}}}} })}});
        pms_submit_camera_frame(g_e, pb, 0, 0.2);
        Img out = render_frame("face_fx");

        double diff = frac_diff(out, plain, 6);
        if (diff <= 0.01) fail("face_fx changed no pixels (diff=" + std::to_string(diff) + ")");

        cmd("set_live_fx", {{"fx", json::array()}});
        cmd("face_track_enable", {{"on", false}});
        pms_submit_camera_frame(g_e, NULL, 0, 0);
        CVPixelBufferRelease(pb);
        pms_destroy(g_e);

        printf("face fx smoke: PASS (diff=%.3f)\n", diff);
    }
    return 0;
}
