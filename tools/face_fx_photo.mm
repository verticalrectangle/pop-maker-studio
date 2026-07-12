// face_fx_photo.mm — apply a face_fx look to a still photo and write PNGs
// for eyes-on makeup QA (alignment, eye makeup, glasses interaction).
// Drives the same live path as the app: face_track_enable → camera side-feed
// → worker inference → face_fx render on macOS Metal.
//
// Usage: face-fx-photo <in.png|jpg> <out_prefix> [filter_id=13] [amount=1.0]
// Writes <out_prefix>_plain.png, <out_prefix>_look.png,
//        <out_prefix>_landmarks.png (tracked MP landmarks over the photo),
//        <out_prefix>_obs.json (the raw 478 landmark positions).
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

#define Marker PMSCarbonAIFFMarker
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#undef Marker

#include "../src/pms_engine.h"
#include "../src/metal_render.h"
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "json.hpp"

using json = nlohmann::json;

static int W = 0, H = 0;   // render target = photo dims (capped)

static pms_engine*         g_e      = nullptr;
static id<MTLDevice>       g_dev    = nil;
static id<MTLTexture>      g_target = nil;
static id<MTLCommandQueue> g_rq     = nil;

static void fail(const std::string& msg) {
    fprintf(stderr, "face fx photo: FAIL %s\n", msg.c_str());
    exit(1);
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
    if (g_target.storageMode == MTLStorageModeManaged) [bl synchronizeResource:g_target];
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

static void write_png(const std::string& path, const Img& im) {
    // BGRA → RGB
    std::vector<uint8_t> rgb((size_t)W * H * 3);
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        rgb[i * 3 + 0] = im.px[i * 4 + 2];
        rgb[i * 3 + 1] = im.px[i * 4 + 1];
        rgb[i * 3 + 2] = im.px[i * 4 + 0];
    }
    if (!stbi_write_png(path.c_str(), W, H, 3, rgb.data(), W * 3))
        fail("cannot write " + path);
    printf("wrote %s\n", path.c_str());
}

static void dot(Img& im, float fx, float fy, uint8_t r, uint8_t g, uint8_t b) {
    int cx = (int)std::lround(fx), cy = (int)std::lround(fy);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            int x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= W || y >= H) continue;
            uint8_t* q = im.px.data() + ((size_t)y * W + x) * 4;
            q[0] = b; q[1] = g; q[2] = r;
        }
}

static CVPixelBufferRef load_photo(const std::string& path) {
    int iw = 0, ih = 0, n = 0;
    unsigned char* rgb = stbi_load(path.c_str(), &iw, &ih, &n, 3);
    if (!rgb) fail("cannot load " + path);
    // Cap the longer side at 1280 (tracking + render speed); keep aspect.
    float s = 1280.0f / (float)std::max(iw, ih);
    if (s > 1.0f) s = 1.0f;
    W = (int)(iw * s) & ~1;
    H = (int)(ih * s) & ~1;
    NSDictionary* attrs = @{ (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
                             (id)kCVPixelBufferMetalCompatibilityKey: @YES };
    CVPixelBufferRef pb = NULL;
    CVReturn r = CVPixelBufferCreate(kCFAllocatorDefault, W, H, kCVPixelFormatType_32BGRA,
                                     (__bridge CFDictionaryRef)attrs, &pb);
    if (r != kCVReturnSuccess || !pb) fail("camera pixel buffer alloc failed");
    CVPixelBufferLockBaseAddress(pb, 0);
    uint8_t* dst = (uint8_t*)CVPixelBufferGetBaseAddress(pb);
    size_t bpr = CVPixelBufferGetBytesPerRow(pb);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int sx = (int)((x + 0.5f) / s), sy = (int)((y + 0.5f) / s);
            if (sx >= iw) sx = iw - 1;
            if (sy >= ih) sy = ih - 1;
            const unsigned char* px = rgb + ((size_t)sy * iw + sx) * 3;
            uint8_t* q = dst + (size_t)y * bpr + (size_t)x * 4;
            q[0] = px[2]; q[1] = px[1]; q[2] = px[0]; q[3] = 255;
        }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    stbi_image_free(rgb);
    return pb;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <in.png|jpg> <out_prefix> [filter_id=13] [amount=1.0]\n",
                argv[0]);
        return 2;
    }
    const std::string in_path = argv[1], prefix = argv[2];
    const double filter_id = argc > 3 ? atof(argv[3]) : 13.0;
    const double amount    = argc > 4 ? atof(argv[4]) : 1.0;

    @autoreleasepool {
        const char* sd = getenv("PMS_SHADER_DIR");
        std::string shader_dir = sd ? sd : std::string(getenv("HOME") ? getenv("HOME") : "")
                                              + "/dev/pms-ios/Shaders/msl";
        metal_render_set_shader_dir(shader_dir.c_str());

        g_dev = MTLCreateSystemDefaultDevice();
        if (!g_dev) { fprintf(stderr, "face fx photo: FAIL no Metal device\n"); return 2; }

        const char* asset_root = getenv("PMS_ASSET_ROOT");
        std::string assets = asset_root ? asset_root
            : std::string(getenv("HOME") ? getenv("HOME") : "") + "/dev/pms-ios/Engine/EngineAssets";
        g_e = pms_create((__bridge void*)g_dev, assets.c_str(), "/tmp/pms-face-fx-photo");
        if (!g_e) fail("pms_create failed");

        CVPixelBufferRef pb = load_photo(in_path);
        printf("photo %dx%d, filter=%d amount=%.2f\n", W, H, (int)filter_id, amount);

        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeManaged;
        g_target = [g_dev newTextureWithDescriptor:td];
        g_rq = [g_dev newCommandQueue];

        json r = cmd("face_track_enable", {{"on", true}});
        if (!r.value("models_present", false)) fail("face models missing");

        bool locked = false;
        for (int k = 0; k < 100 && !locked; ++k) {
            pms_submit_camera_frame(g_e, pb, 0, 0.1 + k * 0.03);
            usleep(50 * 1000);
            locked = cmd("face_debug").value("valid", false);
        }
        if (!locked) fail("face worker never produced a valid observation");

        json obs = cmd("face_debug", {{"pts", true}});
        std::string obs_path = prefix + "_obs.json";
        FILE* f = fopen(obs_path.c_str(), "w");
        if (!f) fail("cannot write " + obs_path);
        std::string dumped = obs.dump(1);
        fwrite(dumped.data(), 1, dumped.size(), f);
        fclose(f);
        printf("wrote %s (score=%.2f)\n", obs_path.c_str(), obs.value("score", 0.0));

        cmd("set_live_fx", {{"fx", json::array()}});
        pms_submit_camera_frame(g_e, pb, 0, 10.0);
        Img plain = render_frame("plain");
        write_png(prefix + "_plain.png", plain);

        // Landmark overlay: eye complexes green, irises cyan, rest red.
        if (obs.contains("pts")) {
            Img lm = plain;
            auto& pts = obs["pts"];
            for (int i = 0; i < (int)pts.size(); ++i) {
                float x = pts[i][0], y = pts[i][1];
                bool iris = i >= 468;
                static const int eye_idx[] = {33,7,163,144,145,153,154,155,133,246,161,160,
                    159,158,157,173,263,249,390,373,374,380,381,382,362,466,388,387,386,385,
                    384,398,230,450,25,110,24,23,22,26,112,255,339,254,253,252,256,341};
                bool eye = false;
                for (int e : eye_idx) if (e == i) { eye = true; break; }
                if (iris)      dot(lm, x, y, 0, 255, 255);
                else if (eye)  dot(lm, x, y, 0, 255, 0);
                else           dot(lm, x, y, 255, 40, 40);
            }
            write_png(prefix + "_landmarks.png", lm);
        }

        cmd("set_live_fx",
            {{"fx", json::array({ {{"fx_type", "face_fx"},
                                   {"params", {{"face_filter", filter_id},
                                               {"face_amount", amount}}}} })}});
        pms_submit_camera_frame(g_e, pb, 0, 10.1);
        Img out = render_frame("look");
        write_png(prefix + "_look.png", out);

        cmd("set_live_fx", {{"fx", json::array()}});
        cmd("face_track_enable", {{"on", false}});
        pms_submit_camera_frame(g_e, NULL, 0, 0);
        CVPixelBufferRelease(pb);
        pms_destroy(g_e);
        printf("face fx photo: DONE\n");
    }
    return 0;
}
