#include <cmath>
#include <filesystem>
#include "../src/paths.h"
#include <algorithm>
// metal_render_test.mm — numeric offscreen verification of the Metal scene
// compositor + FX runner (the iOS render path), run on a macOS host GPU.
//
// Drives the REAL app flow through the C ABI: pms_create(MTLDevice) →
// pms_command (new_project / add_track / add_clip / add_effect_brick /
// add_multifx_brick / set_clip_prop) → pms_submit_layer_frame with synthetic
// CVPixelBuffers → pms_render into an offscreen BGRA8 texture → readback →
// pixel assertions. fx_debug (pms_command) is dumped on every failure.
//
// Cases:
//   a. background only — legacy path renders the aurora, scene inactive
//   b. one video layer — scene path active, layer pixels dominate
//   c. REPRO: video + coupled pixelate brick (0..2), gradient layer at t=1.0
//      → pixels must DIFFER from the no-FX render (the user's FX-not-
//      rendering bug lands here)
//   d. time window: same scene at t=3.0 (outside the brick) → pixels EQUAL
//   e. two-FX order: uncoupled [pixelate→fisheye] vs [fisheye→pixelate] on a
//      rail track → outputs differ (warp composition is order-sensitive)
//   f. transitions: two adjacent clips with a Dissolve — mid-transition frame
//      is a blend (differs from both pure frames; both colors present)
//   g. text layer: half-transparent raster on a higher track composites over
//      the video layer
//   h. bus scope: uncoupled brick on a track between two video tracks alters
//      the lower layer's region but not the upper layer's pixels
//
// Shader lookup: the FX runner loads Shaders/msl/<name>.metal +
// params_manifest.json from the app bundle; headless we point it at a
// directory via metal_render_set_shader_dir. Set PMS_SHADER_DIR (default:
// $HOME/dev/pms-ios/Shaders/msl).
//
// Prints "metal render test: PASS" and exits 0; exits nonzero with the named
// failing case otherwise.
// CarbonCore's AIFF.h (pulled in by CoreVideo on macOS) has a legacy
// `struct Marker` that collides with the engine's (app.h). Shield it.
#define Marker PMSCarbonAIFFMarker
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#undef Marker
#include "../src/pms_engine.h"
#include "../src/metal_render.h"
#include "stb_image.h"
#include <unistd.h>
#include "../src/app.h"          // AppState/Track/Clip — case f builds a scene directly
#include "json.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <functional>

using json = nlohmann::json;

static const int W = 360, H = 640;      // 9:16 canvas
static pms_engine*        g_e      = nullptr;
static id<MTLDevice>      g_dev    = nil;
static id<MTLTexture>     g_target = nil;
static id<MTLCommandQueue> g_rq    = nil;

// ── Failure plumbing ─────────────────────────────────────────────────────────

static std::string fx_debug_str() {
    if (!g_e) return "{}";
    char* r = pms_command(g_e, "{\"id\":\"d\",\"method\":\"fx_debug\",\"params\":{}}");
    std::string s = r ? r : "{}";
    pms_free(r);
    return s;
}

static void fail(const std::string& case_name, const std::string& msg) {
    fprintf(stderr, "metal render test: FAIL [%s] %s\n", case_name.c_str(), msg.c_str());
    fprintf(stderr, "fx_debug: %s\n", fx_debug_str().c_str());
    exit(1);
}

static void check(bool ok, const std::string& case_name, const std::string& msg) {
    if (!ok) fail(case_name, msg);
}

// Send one lever; assert success; return the result payload.
static json cmd(const std::string& case_name, const std::string& method,
                const json& params = json::object()) {
    json req = {{"id", "t"}, {"method", method}, {"params", params}};
    char* r = pms_command(g_e, req.dump().c_str());
    std::string out = r ? r : "";
    pms_free(r);
    json reply = json::parse(out, nullptr, false);
    if (reply.is_discarded()) fail(case_name, method + ": unparsable reply: " + out);
    if (reply.contains("error"))
        fail(case_name, method + " errored: " + reply["error"].dump());
    return reply.value("result", json::object());
}

static json fx_debug(const std::string& case_name) {
    json j = json::parse(fx_debug_str(), nullptr, false);
    if (j.is_discarded()) fail(case_name, "fx_debug: unparsable reply");
    return j.value("result", json::object());   // unwrap the {id,result} envelope
}

// ── Synthetic CVPixelBuffers ─────────────────────────────────────────────────
// IOSurface-backed 32BGRA so CVMetalTextureCache maps them zero-copy, exactly
// like the AVFoundation buffers the app submits.

static CVPixelBufferRef make_pb(int w, int h,
        const std::function<void(uint8_t* row, int x, int y, uint8_t* px)>& fill) {
    NSDictionary* attrs = @{ (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
                             (id)kCVPixelBufferMetalCompatibilityKey: @YES };
    CVPixelBufferRef pb = NULL;
    CVReturn r = CVPixelBufferCreate(kCFAllocatorDefault, w, h,
                                     kCVPixelFormatType_32BGRA,
                                     (__bridge CFDictionaryRef)attrs, &pb);
    if (r != kCVReturnSuccess || !pb) fail("setup", "CVPixelBufferCreate failed");
    CVPixelBufferLockBaseAddress(pb, 0);
    uint8_t* base = (uint8_t*)CVPixelBufferGetBaseAddress(pb);
    size_t   bpr  = CVPixelBufferGetBytesPerRow(pb);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = base + (size_t)y * bpr;
        for (int x = 0; x < w; ++x) fill(row, x, y, row + (size_t)x * 4);
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    return pb;
}

// Solid premultiplied BGRA.
static CVPixelBufferRef make_solid(int w, int h, uint8_t b, uint8_t g, uint8_t r, uint8_t a = 255) {
    return make_pb(w, h, [=](uint8_t*, int, int, uint8_t* px) {
        px[0] = b; px[1] = g; px[2] = r; px[3] = a;
    });
}

// Diagonal gradient (opaque): B ramps with x, G ramps with y, R with x+y —
// smooth in every direction so warps/quantizers visibly rearrange it.
static CVPixelBufferRef make_gradient(int w, int h) {
    return make_pb(w, h, [=](uint8_t*, int x, int y, uint8_t* px) {
        px[0] = (uint8_t)(x * 255 / (w - 1));
        px[1] = (uint8_t)(y * 255 / (h - 1));
        px[2] = (uint8_t)(((x + y) * 255) / (w + h - 2));
        px[3] = 255;
    });
}

// 20px checkerboard (opaque black/white): maximal contrast so pixelate(40)
// produces ~full-range deltas and a 50% wet/dry blend stays far above noise.
static CVPixelBufferRef make_checker(int w, int h) {
    return make_pb(w, h, [=](uint8_t*, int x, int y, uint8_t* px) {
        uint8_t v = (((x / 20) + (y / 20)) & 1) ? 255 : 0;
        px[0] = v; px[1] = v; px[2] = v; px[3] = 255;
    });
}

static void submit_layer(int track, int clip, CVPixelBufferRef pb, double host_time) {
    pms_submit_layer_frame(g_e, track, clip, (void*)pb, 0, host_time);
}
static void clear_layer(int track, int clip) {
    pms_submit_layer_frame(g_e, track, clip, NULL, 0, -1.0);
}

// ── Render + readback ────────────────────────────────────────────────────────

struct Img { std::vector<uint8_t> px; };   // W*H BGRA

static Img read_target() {
    id<MTLCommandBuffer> cb = [g_rq commandBuffer];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
#if TARGET_OS_OSX || defined(__MAC_OS_X_VERSION_MIN_REQUIRED)
    if (g_target.storageMode == MTLStorageModeManaged) [bl synchronizeResource:g_target];
#endif
    [bl endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    Img im; im.px.resize((size_t)W * H * 4);
    [g_target getBytes:im.px.data() bytesPerRow:(NSUInteger)W * 4
            fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    return im;
}

static Img render_frame(const std::string& case_name) {
    int rc = pms_render(g_e, (__bridge void*)g_target, W, H);
    if (rc != 0) fail(case_name, "pms_render rc=" + std::to_string(rc));
    pms_render_wait(g_e);
    return read_target();
}

// ── Pixel assertions ─────────────────────────────────────────────────────────

struct PxRect { int x0, y0, x1, y1; };       // half-open
static const PxRect kFull = {0, 0, W, H};

static const uint8_t* at(const Img& im, int x, int y) {
    return im.px.data() + ((size_t)y * W + x) * 4;
}

static int px_diff(const uint8_t* a, const uint8_t* b) {
    int d = 0;
    for (int c = 0; c < 3; ++c) d = std::max(d, std::abs((int)a[c] - (int)b[c]));
    return d;
}

// Fraction of pixels in `r` whose max BGR channel delta exceeds `tol`.
static double frac_diff(const Img& a, const Img& b, int tol, PxRect r = kFull) {
    long n = 0, diff = 0;
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) {
            ++n;
            if (px_diff(at(a, x, y), at(b, x, y)) > tol) ++diff;
        }
    return n ? (double)diff / (double)n : 0.0;
}

static int max_region_diff(const Img& a, const Img& b, PxRect r) {
    int d = 0;
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x)
            d = std::max(d, px_diff(at(a, x, y), at(b, x, y)));
    return d;
}

// Average BGR over a rect.
static void avg_bgr(const Img& im, PxRect r, double out[3]) {
    double s[3] = {0, 0, 0}; long n = 0;
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x, ++n)
            for (int c = 0; c < 3; ++c) s[c] += at(im, x, y)[c];
    for (int c = 0; c < 3; ++c) out[c] = n ? s[c] / n : 0.0;
}

static std::string bgr_str(const double c[3]) {
    char buf[64];
    snprintf(buf, sizeof buf, "(B=%.1f G=%.1f R=%.1f)", c[0], c[1], c[2]);
    return buf;
}
struct RegionStats { double mean_lum, std_lum; int max_chan, min_chan; };
static RegionStats region_stats(const Img& im, PxRect r) {
    double sum = 0.0, sum2 = 0.0; long n = 0;
    int maxc = 0, minc = 255;
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) {
            const uint8_t* p = at(im, x, y);
            int m = std::max((int)p[0], std::max((int)p[1], (int)p[2]));
            int n2 = std::min((int)p[0], std::min((int)p[1], (int)p[2]));
            maxc = std::max(maxc, m); minc = std::min(minc, n2);
            double lum = 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2];
            sum += lum; sum2 += lum * lum; ++n;
        }
    double mean = n ? sum / n : 0.0;
    double var = n ? sum2 / n - mean * mean : 0.0;
    RegionStats st; st.mean_lum = mean; st.std_lum = std::sqrt(std::max(0.0, var));
    st.max_chan = maxc; st.min_chan = minc; return st;
}

// ── Cases ────────────────────────────────────────────────────────────────────

// a. Background only: no project content, no layer frames — the legacy path
// must still render (aurora, non-uniform, not black), scene inactive.
static void case_background_only() {
    const std::string C = "a.background_only";
    Img im = render_frame(C);
    json dbg = fx_debug(C);
    check(dbg.contains("scene") && dbg["scene"].value("active", true) == false, C,
          "scene compositor ran with no layer frames (expected legacy path)");
    double top[3], bottom[3];
    avg_bgr(im, {0, 0, W, 40}, top);
    avg_bgr(im, {0, H - 40, W, H}, bottom);
    check(top[0] + top[1] + top[2] + bottom[0] + bottom[1] + bottom[2] > 3.0, C,
          "aurora rendered all-black: top=" + bgr_str(top) + " bottom=" + bgr_str(bottom));
    double d = 0;
    for (int c = 0; c < 3; ++c) d = std::max(d, std::abs(top[c] - bottom[c]));
    check(d > 1.0, C, "aurora is uniform (no vertical structure) — background pass suspect");
}

// Shared project for b/c/d: one track, one video clip 0..4.
static void build_video_project(const std::string& C) {
    cmd(C, "new_project", {{"force", true}});
    cmd(C, "add_track", {{"name", "V"}, {"position", 0}});
    cmd(C, "add_clip", {{"track", 0}, {"type", "video"}, {"start", 0.0}, {"end", 4.0},
                        {"text", "/tmp/pms_mrt_fake.mp4"}});
}

// b. One video layer → scene active, layer pixels dominate the center.
static void case_one_layer(CVPixelBufferRef green) {
    const std::string C = "b.one_video_layer";
    build_video_project(C);
    submit_layer(0, 0, green, 1.0);
    Img im = render_frame(C);
    json dbg = fx_debug(C);
    check(dbg["scene"].value("active", false), C, "scene.active is false with a layer submitted");
    check(!dbg["scene"]["layers_drawn"].empty(), C,
          "no layers drawn; scene=" + dbg["scene"].dump());
    double c[3]; avg_bgr(im, {W/2 - 20, H/2 - 20, W/2 + 20, H/2 + 20}, c);
    check(std::abs(c[0] - 20) < 6 && std::abs(c[1] - 200) < 6 && std::abs(c[2] - 20) < 6, C,
          "center is not the submitted layer color: " + bgr_str(c) + " expected (B=20 G=200 R=20)");
}

// c. REPRO: coupled pixelate brick over the clip; gradient frame at t=1.0
// (inside the brick's 0..2 window) → output must differ from the no-FX render.
static Img g_nofx_gradient;   // baseline shared with case d
static void case_glass_fx(CVPixelBufferRef gradient) {
    const std::string C = "c.glass_pixelate";
    build_video_project(C);
    submit_layer(0, 0, gradient, 1.0);
    g_nofx_gradient = render_frame(C);   // no-FX baseline, same frame/time

    json r = cmd(C, "add_effect_brick",
                 {{"track", 0}, {"fx_type", "pixelate"}, {"start", 0.0}, {"end", 2.0},
                  {"params", {{"size", 40.0}}}});
    check(r.value("coupled", false), C, "pixelate brick did not auto-couple: " + r.dump());

    // The engine playhead is only coarsely reconciled in the app (5 Hz preview)
    // and FROZEN during export — park it OUTSIDE the brick's window so the test
    // fails if FX windows read state.playhead instead of the frame clock (the
    // exact FX-don't-render bug: frames at t=1.0 must still get the brick).
    cmd(C, "seek", {{"time", 3.0}});

    Img fx = render_frame(C);
    json dbg = fx_debug(C);
    check(dbg["scene"].value("glass_applied", 0) >= 1, C,
          "glass chain reported 0 applied passes; scene=" + dbg["scene"].dump());
    double d = frac_diff(fx, g_nofx_gradient, 8);
    check(d > 0.01, C,
          "pixelate(size 40) left the frame unchanged (diff frac=" + std::to_string(d) +
          ") — FX are not rendering; scene=" + dbg["scene"].dump());
    // Numeric correctness: pixelate output is CONSTANT within each 40px block
    // (the 360x640 layer maps 1:1 onto the canvas) and steps across blocks.
    int in_block  = px_diff(at(fx, 10, 10), at(fx, 30, 30));
    int x_block   = px_diff(at(fx, 10, 10), at(fx, 50, 10));
    check(in_block <= 3, C, "pixelate block is not constant (in-block diff=" +
          std::to_string(in_block) + ") — params/UV path wrong");
    check(x_block >= 10, C, "pixelate blocks do not step across the boundary (diff=" +
          std::to_string(x_block) + ") — u_size not reaching the shader");
}

// d. Same scene at t=3.0 — outside the brick's 0..2 window → equals no-FX.
static void case_fx_window(CVPixelBufferRef gradient) {
    const std::string C = "d.fx_time_window";
    // Converse of case c: park the playhead INSIDE the brick window while the
    // frame clock is outside — a playhead-driven window would wrongly apply.
    cmd(C, "seek", {{"time", 1.0}});
    submit_layer(0, 0, gradient, 3.0);   // moves the scene clock past the brick
    Img im = render_frame(C);
    json dbg = fx_debug(C);
    check(dbg["scene"].value("glass_applied", 0) == 0, C,
          "brick applied outside its window; scene=" + dbg["scene"].dump());
    int md = max_region_diff(im, g_nofx_gradient, kFull);
    check(md <= 2, C, "frame at t=3.0 differs from the no-FX render (max diff=" +
          std::to_string(md) + ") — FX window leaking; scene=" + dbg["scene"].dump());
}

// e. Two-FX order on an uncoupled rail brick: pixelate→fisheye vs
// fisheye→pixelate are different warp compositions.
static Img render_rail_chain(const std::string& C, CVPixelBufferRef gradient,
                             const json& effects) {
    cmd(C, "new_project", {{"force", true}});
    cmd(C, "add_track", {{"name", "RAIL"}, {"position", 0}});
    cmd(C, "add_track", {{"name", "V"}, {"position", 1}});
    cmd(C, "add_clip", {{"track", 1}, {"type", "video"}, {"start", 0.0}, {"end", 4.0},
                        {"text", "/tmp/pms_mrt_fake.mp4"}});
    json r = cmd(C, "add_multifx_brick",
                 {{"track", 0}, {"start", 0.0}, {"end", 4.0}, {"effects", effects}});
    check(!r.value("coupled", true), C,
          "rail brick unexpectedly coupled (no host on its track): " + r.dump());
    submit_layer(1, 0, gradient, 1.0);
    Img im = render_frame(C);
    json dbg = fx_debug(C);
    check(dbg["scene"].value("bus_applied", 0) == 2, C,
          "expected 2 bus passes applied, scene=" + dbg["scene"].dump());
    clear_layer(1, 0);
    return im;
}

static void case_fx_order(CVPixelBufferRef gradient) {
    const std::string C = "e.two_fx_order";
    json pixelate = {{"fx_type", "pixelate"}, {"params", {{"size", 40.0}}}};
    json fisheye  = {{"fx_type", "fisheye"}};
    Img ab = render_rail_chain(C, gradient, json::array({pixelate, fisheye}));
    Img ba = render_rail_chain(C, gradient, json::array({fisheye, pixelate}));
    double d = frac_diff(ab, ba, 8);
    check(d > 0.01, C, "pixelate→fisheye equals fisheye→pixelate (diff frac=" +
          std::to_string(d) + ") — chain order is not honored");
    // And both must actually differ from the raw gradient scene.
    double da = frac_diff(ab, g_nofx_gradient, 8);
    check(da > 0.01, C, "bus chain left the frame unchanged (diff frac=" +
          std::to_string(da) + ")");

    // Cross-track variant: one brick per rail track (lower rail applies first).
    // Exercises two scene copy-backs in a single frame (aliasing trap).
    auto rails = [&](const json& lower_fx, const json& upper_fx) {
        cmd(C, "new_project", {{"force", true}});
        cmd(C, "add_track", {{"name", "R0"}, {"position", 0}});
        cmd(C, "add_track", {{"name", "R1"}, {"position", 1}});
        cmd(C, "add_track", {{"name", "V"},  {"position", 2}});
        cmd(C, "add_clip", {{"track", 2}, {"type", "video"}, {"start", 0.0}, {"end", 4.0},
                            {"text", "/tmp/pms_mrt_fake.mp4"}});
        cmd(C, "add_effect_brick", {{"track", 1}, {"fx_type", lower_fx["fx_type"]},
                                    {"start", 0.0}, {"end", 4.0}, {"params", lower_fx["params"]}});
        cmd(C, "add_effect_brick", {{"track", 0}, {"fx_type", upper_fx["fx_type"]},
                                    {"start", 0.0}, {"end", 4.0}, {"params", upper_fx["params"]}});
        submit_layer(2, 0, gradient, 1.0);
        Img im = render_frame(C);
        json dbg2 = fx_debug(C);
        check(dbg2["scene"].value("bus_applied", 0) == 2, C,
              "cross-track: expected 2 bus passes; scene=" + dbg2["scene"].dump());
        clear_layer(2, 0);
        return im;
    };
    json pix = {{"fx_type", "pixelate"}, {"params", {{"size", 40.0}}}};
    json fis = {{"fx_type", "fisheye"}, {"params", json::object()}};
    Img ab2 = rails(pix, fis);   // pixelate (lower rail) then fisheye (upper rail)
    Img ba2 = rails(fis, pix);
    double d2 = frac_diff(ab2, ba2, 8);
    check(d2 > 0.01, C, "cross-track bus order not honored (diff frac=" + std::to_string(d2) + ")");
    // Track-stack order must equal chain order (desktop parity).
    double dsame = frac_diff(ab2, ab, 4);
    check(dsame < 0.02, C, "two rail tracks disagree with one chained brick (diff frac=" +
          std::to_string(dsame) + ") — scene copy-back suspect");
}

// f. Transition smoke: Dissolve mid-frame is a blend of both clips. No IPC
// lever sets transitions, so this case drives the real compositor directly
// with a hand-built AppState (same metal_render_frame the C ABI calls).
static void case_transition(CVPixelBufferRef red, CVPixelBufferRef blue) {
    const std::string C = "f.dissolve_transition";
    AppState st;
    st.tracks.emplace_back();
    Track& tr = st.tracks[0];
    tr.name = "V";
    Clip a;  a.clip_type = ClipType::Video; a.start = 0.f; a.end = 2.f;
    a.transition_type = TransitionType::Dissolve;
    a.transition_pre = 0.5f; a.transition_post = 0.5f;
    Clip b;  b.clip_type = ClipType::Video; b.start = 2.f; b.end = 4.f;
    tr.clips.push_back(a);
    tr.clips.push_back(b);

    auto render_at = [&](double t) {
        metal_render_submit_layer(0, 0, (void*)red,  0, t);
        metal_render_submit_layer(0, 1, (void*)blue, 0, t);
        int rc = metal_render_frame((__bridge void*)g_target, W, H, 0.0, &st);
        if (rc != 0) fail(C, "metal_render_frame rc=" + std::to_string(rc));
        metal_render_wait();
        return read_target();
    };
    Img pure_a = render_at(1.0);    // before the transition zone
    Img mid    = render_at(1.9);    // inside pre (1.5..2.0)
    Img pure_b = render_at(2.6);    // past post (2.0..2.5)
    metal_render_submit_layer(0, 0, NULL, 0, -1.0);
    metal_render_submit_layer(0, 1, NULL, 0, -1.0);

    PxRect c = {W/2 - 20, H/2 - 20, W/2 + 20, H/2 + 20};
    double ca[3], cm[3], cb2[3];
    avg_bgr(pure_a, c, ca); avg_bgr(mid, c, cm); avg_bgr(pure_b, c, cb2);
    check(ca[2] > 180 && ca[0] < 60, C, "pure A frame is not red: " + bgr_str(ca));
    check(cb2[0] > 180 && cb2[2] < 60, C, "pure B frame is not blue: " + bgr_str(cb2));
    check(max_region_diff(mid, pure_a, c) > 20 && max_region_diff(mid, pure_b, c) > 20, C,
          "mid-transition frame equals a pure frame: mid=" + bgr_str(cm));
    check(cm[2] > 20 && cm[0] > 20, C,
          "mid-transition is not a blend of both clips: " + bgr_str(cm));
}

// g. Text layer on a higher track composites over the video layer.
static void case_text_layer(CVPixelBufferRef video, CVPixelBufferRef text_raster) {
    const std::string C = "g.text_layer";
    cmd(C, "new_project", {{"force", true}});
    cmd(C, "add_track", {{"name", "T"}, {"position", 0}});
    cmd(C, "add_track", {{"name", "V"}, {"position", 1}});
    cmd(C, "add_clip", {{"track", 1}, {"type", "video"}, {"start", 0.0}, {"end", 4.0},
                        {"text", "/tmp/pms_mrt_fake.mp4"}});
    cmd(C, "add_clip", {{"track", 0}, {"type", "text"}, {"start", 0.0}, {"end", 4.0},
                        {"text", "hello"}});
    submit_layer(1, 0, video, 1.0);
    Img base = render_frame(C);
    submit_layer(0, 0, text_raster, -1.0);   // static raster: no scene clock
    Img over = render_frame(C);
    json dbg = fx_debug(C);
    clear_layer(0, 0);
    clear_layer(1, 0);

    PxRect c = {W/2 - 20, H/2 - 20, W/2 + 20, H/2 + 20};
    double cb[3], co[3];
    avg_bgr(base, c, cb); avg_bgr(over, c, co);
    // raster = premultiplied 50% red over dark-blue video: R rises, B halves.
    check(co[2] > cb[2] + 60, C, "text raster did not composite over the video: base=" +
          bgr_str(cb) + " over=" + bgr_str(co) + "; scene=" + dbg["scene"].dump());
    check(co[0] < cb[0] - 40, C, "text alpha not honored (background not attenuated): base=" +
          bgr_str(cb) + " over=" + bgr_str(co));
}

// h. Bus scope: an uncoupled brick on the track BETWEEN two video tracks
// filters the accumulated scene below it (lower layer) but the upper layer,
// drawn afterwards, stays untouched.
static void case_bus_scope(CVPixelBufferRef gradient, CVPixelBufferRef yellow) {
    const std::string C = "h.bus_scope";
    auto build = [&](bool with_brick) {
        cmd(C, "new_project", {{"force", true}});
        cmd(C, "add_track", {{"name", "VU"},   {"position", 0}});
        cmd(C, "add_track", {{"name", "RAIL"}, {"position", 1}});
        cmd(C, "add_track", {{"name", "VL"},   {"position", 2}});
        cmd(C, "add_clip", {{"track", 0}, {"type", "video"}, {"start", 0.0}, {"end", 4.0},
                            {"text", "/tmp/pms_mrt_fake_u.mp4"}});
        cmd(C, "add_clip", {{"track", 2}, {"type", "video"}, {"start", 0.0}, {"end", 4.0},
                            {"text", "/tmp/pms_mrt_fake_l.mp4"}});
        // Upper layer: small solid patch, upper-right quadrant.
        cmd(C, "set_clip_prop", {{"track", 0}, {"clip", 0}, {"prop", "scale_x"}, {"value", 0.35}});
        cmd(C, "set_clip_prop", {{"track", 0}, {"clip", 0}, {"prop", "scale_y"}, {"value", 0.35}});
        cmd(C, "set_clip_prop", {{"track", 0}, {"clip", 0}, {"prop", "pos_x"},   {"value", 0.75}});
        cmd(C, "set_clip_prop", {{"track", 0}, {"clip", 0}, {"prop", "pos_y"},   {"value", 0.25}});
        if (with_brick) {
            json r = cmd(C, "add_effect_brick",
                         {{"track", 1}, {"fx_type", "posterize"}, {"start", 0.0}, {"end", 4.0},
                          {"params", {{"levels", 3.0}}}});
            check(!r.value("coupled", true), C, "rail brick unexpectedly coupled: " + r.dump());
        }
        submit_layer(0, 0, yellow, 1.0);
        submit_layer(2, 0, gradient, 1.0);
        Img im = render_frame(C);
        clear_layer(0, 0);
        clear_layer(2, 0);
        return im;
    };
    Img base = build(false);
    Img fx   = build(true);
    json dbg = fx_debug(C);
    check(dbg["scene"].value("bus_applied", 0) >= 1, C,
          "rail brick applied 0 bus passes; scene=" + dbg["scene"].dump());
    // Upper layer center (pos 0.75/0.25 of the canvas): must be identical.
    PxRect up = {(int)(0.75 * W) - 10, (int)(0.25 * H) - 10,
               (int)(0.75 * W) + 10, (int)(0.25 * H) + 10};
    int ud = max_region_diff(fx, base, up);
    check(ud <= 2, C, "bus FX on a middle track altered the UPPER layer (max diff=" +
          std::to_string(ud) + ") — bus scope broken; scene=" + dbg["scene"].dump());
    // Lower-left region (gradient only): must be posterized.
    PxRect low = {20, (int)(0.70 * H), (int)(0.40 * W), (int)(0.95 * H)};
    double ld = frac_diff(fx, base, 8, low);
    check(ld > 0.05, C, "bus FX did not alter the lower layer region (diff frac=" +
          std::to_string(ld) + "); scene=" + dbg["scene"].dump());
}

// i. Every generated FX in the manifest must resolve + apply as a glass brick
// (PSO compiles from the transpiled MSL, params buffer fills, pass encodes).
// A single shader failing to compile is exactly the "FX don't render" class.
static void case_all_manifest_fx(CVPixelBufferRef checker) {
    const std::string C = "i.all_manifest_fx";
    // The generated FX registry — the same table ipc_server/metal_render use.
    static const char* k_ids[] = {
#include "../src/generated/fx_gen_names.h"
    };
    std::vector<std::string> ids(k_ids, k_ids + sizeof(k_ids) / sizeof(k_ids[0]));
    std::vector<std::string> failed, unchanged;
    check(!ids.empty(), C, "generated FX registry is empty");
    build_video_project(C);
    submit_layer(0, 0, checker, 1.0);
    Img nofx = render_frame(C);
    clear_layer(0, 0);
    for (const std::string& id : ids) {
        if (id.empty()) continue;
        if (id == "ken_burns") continue;   // desktop CPU path (render.cpp), no fragment shader — accepted gap
        build_video_project(C + "." + id);
        cmd(C, "add_effect_brick",
            {{"track", 0}, {"fx_type", id}, {"start", 0.0}, {"end", 4.0}});
        submit_layer(0, 0, checker, 1.0);
        Img fx = render_frame(C + "." + id);
        json d = fx_debug(C);
        if (d["scene"].value("glass_applied", 0) < 1)
            failed.push_back(id + " → " + d["scene"].value("notes", json::array()).dump());
        else if (frac_diff(fx, nofx, 4) < 0.001)
            unchanged.push_back(id);   // informational: identity at defaults on this content
        clear_layer(0, 0);
    }
    if (!unchanged.empty()) {
        std::string s; for (auto& u : unchanged) s += u + " ";
        printf("  [i] note: identity-at-defaults fx (applied but no pixel change): %s\n", s.c_str());
    }
    if (!failed.empty()) {
        std::string s; for (auto& f : failed) s += "\n    " + f;
        fail(C, "generated FX did not apply:" + s);
    }
    printf("  [i] %zu generated FX applied (%zu identity at defaults)\n",
           ids.size(), unchanged.size());
}

// j. Wet/dry: amount 0.5 must blend — differ from both the no-FX frame and
// the amount-1.0 frame (exercises the automatic blend pass).
static void case_wet_dry(CVPixelBufferRef checker) {
    const std::string C = "j.wet_dry_amount";
    build_video_project(C);
    submit_layer(0, 0, checker, 1.0);
    Img nofx = render_frame(C);
    cmd(C, "add_effect_brick",
        {{"track", 0}, {"fx_type", "pixelate"}, {"start", 0.0}, {"end", 4.0},
         {"params", {{"size", 40.0}, {"amount", 1.0}}}});
    Img full = render_frame(C);
    cmd(C, "set_clip_fx", {{"track", 0}, {"clip", 1}, {"fx_id", "pixelate"}, {"amount", 0.5}});
    Img half = render_frame(C);
    clear_layer(0, 0);
    double d_nofx = frac_diff(half, nofx, 30);
    double d_full = frac_diff(half, full, 30);
    check(d_nofx > 0.05, C, "amount 0.5 left the frame unchanged (diff frac vs no-FX=" +
          std::to_string(d_nofx) + ")");
    check(d_full > 0.05, C, "amount 0.5 equals amount 1.0 (diff frac=" +
          std::to_string(d_full) + ") — wet/dry blend not running");
}

// k. Legacy hand-wired FX (fx_shader.cpp family) — Metal ports transpiled from
// shaders/legacy/*.glsl. Each must resolve a PSO, report applied, and visibly
// change the frame at deliberately non-identity params.
static void case_legacy_fx(CVPixelBufferRef gradient, CVPixelBufferRef green,
                           CVPixelBufferRef checker) {
    const std::string C = "k.legacy_fx";
    struct LegacyCase { const char* id; json params; CVPixelBufferRef content; };
    const LegacyCase cases[] = {
        {"grade",      {{"brightness", 0.4}, {"contrast", 1.0}, {"saturation", 1.0}, {"hue", 0.0}}, gradient},
        // Checker, not gradient: a symmetric blur of a linear ramp is identity.
        {"blur",       {{"blur", 10.0}}, checker},
        {"vignette",   {{"vignette", 0.9}}, gradient},
        {"glitch",     {{"glitch_chroma", 15.0}, {"glitch_jitter", 0.8},
                        {"glitch_corruption", 0.6}, {"glitch_corruption_bleed", 0.0}}, gradient},
        {"vhs",        {{"vhs_noise", 0.8}, {"vhs_bleed", 15.0}, {"vhs_tracking", 0.8}}, gradient},
        {"light_leak", {{"leak_intensity", 1.0}, {"leak_speed", 1.0}}, gradient},
        {"datamosh",   {{"datamosh_intensity", 1.0}, {"datamosh_spread", 1.0}}, gradient},
        // Green content + green key → the layer keys out entirely.
        {"chroma_key", {{"chroma_key_r", 0.0}, {"chroma_key_g", 1.0}, {"chroma_key_b", 0.0},
                        {"chroma_key_threshold", 0.30}, {"chroma_key_softness", 0.15}}, green},
    };
    std::vector<std::string> failed, unchanged;
    for (const auto& lc : cases) {
        const std::string CC = C + "." + lc.id;
        build_video_project(CC);
        submit_layer(0, 0, lc.content, 1.0);
        Img base = render_frame(CC + ".base");
        cmd(CC, "add_effect_brick",
            {{"track", 0}, {"fx_type", lc.id}, {"start", 0.0}, {"end", 4.0},
             {"params", lc.params}});
        Img fx = render_frame(CC);
        json d = fx_debug(CC);
        if (d["scene"].value("glass_applied", 0) < 1)
            failed.push_back(std::string(lc.id) + " → " +
                             d["scene"].value("notes", json::array()).dump());
        else if (frac_diff(fx, base, 4) < 0.01)
            unchanged.push_back(lc.id);
        clear_layer(0, 0);
    }
    if (!failed.empty()) {
        std::string s; for (auto& f : failed) s += "\n    " + f;
        fail(C, "legacy FX did not apply:" + s);
    }
    if (!unchanged.empty()) {
        std::string s; for (auto& u : unchanged) s += u + " ";
        fail(C, "legacy FX applied but changed no pixels: " + s);
    }
    printf("  [k] %zu legacy FX applied with visible effect\n",
           sizeof(cases) / sizeof(cases[0]));
}

// l. Chroma feedback family — temporal state must survive across frames:
// stamp a non-keyed subject into the feedback/ring, then feed pure key-color
// content; the ghost of the subject must still be visible (a stateless pass
// would render the two key-color frames identically).
static void case_chroma_feedback(CVPixelBufferRef green, CVPixelBufferRef gradient) {
    const std::string C = "l.chroma_feedback";
    const char* fxs[] = {"chroma_melt", "chroma_echo", "chroma_frame"};
    for (const char* fx : fxs) {
        const std::string CC = C + "." + fx;
        build_video_project(CC);
        cmd(CC, "add_effect_brick",
            {{"track", 0}, {"fx_type", fx}, {"start", 0.0}, {"end", 8.0}});
        // Baseline: key-color content with virgin state → plain green out.
        submit_layer(0, 0, green, 0.10);
        Img base = render_frame(CC + ".base");
        json d = fx_debug(CC);
        check(d["scene"].value("glass_applied", 0) >= 1, CC,
              std::string("chroma pass did not apply; scene=") + d["scene"].dump());
        // Stamp the subject over several advancing-clock frames (chroma_frame
        // snapshots its ring on the scene clock at `spacing` cadence).
        submit_layer(0, 0, gradient, 0.30); (void)render_frame(CC + ".stamp1");
        submit_layer(0, 0, gradient, 0.50); (void)render_frame(CC + ".stamp2");
        submit_layer(0, 0, gradient, 0.70); (void)render_frame(CC + ".stamp3");
        // Back to pure key color: the ghost must persist.
        submit_layer(0, 0, green, 0.90);
        Img ghost = render_frame(CC + ".ghost");
        double diff = frac_diff(ghost, base, 8);
        check(diff > 0.02, CC,
              "no temporal ghost after content returned to the key color (diff frac=" +
              std::to_string(diff) + ") — feedback state not persisting");
        clear_layer(0, 0);
    }
    printf("  [l] chroma feedback family holds temporal state\n");
}

// n. Matte-keyed chroma (record/selfie mode): with matte_key=1 the person
// matte is the key — the subject half stays live while the background half
// keeps temporal ghosts. Uses a left-half-white R8 matte.
static void case_chroma_matte_key(CVPixelBufferRef red, CVPixelBufferRef blue) {
    const std::string C = "n.chroma_matte_key";
    // R8 matte: left half = subject (1), right half = background (0).
    NSDictionary* attrs = @{ (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
                             (id)kCVPixelBufferMetalCompatibilityKey: @YES };
    CVPixelBufferRef matte = NULL;
    CVReturn r = CVPixelBufferCreate(kCFAllocatorDefault, W, H,
                                     kCVPixelFormatType_OneComponent8,
                                     (__bridge CFDictionaryRef)attrs, &matte);
    if (r != kCVReturnSuccess || !matte) fail(C, "matte CVPixelBufferCreate failed");
    CVPixelBufferLockBaseAddress(matte, 0);
    uint8_t* base = (uint8_t*)CVPixelBufferGetBaseAddress(matte);
    size_t   bpr  = CVPixelBufferGetBytesPerRow(matte);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            base[(size_t)y * bpr + x] = (x < W / 2) ? 255 : 0;
    CVPixelBufferUnlockBaseAddress(matte, 0);
    pms_submit_person_matte(g_e, matte, 0.1);

    // matte_key is a LIVE-stack-only param (record mode) — exercise the real
    // record path: set_live_fx + camera frames on the single-content path
    // (no layer frames stored → legacy renderer, exactly what RecordView hits).
    build_video_project(C);
    // Also covers clear_layer_frames: a stored layer frame would shadow the
    // camera path (the RecordView bug) — the command must drop it.
    submit_layer(0, 0, red, 0.05);
    cmd(C, "clear_layer_frames", json::object());
    cmd(C, "set_live_fx",
        {{"fx", json::array({ {{"fx_type", "chroma_echo"},
                               {"params", {{"matte_key", 1.0},
                                           {"chroma_echo_persist", 0.92}}}} })}});
    // Stamp red into the feedback, then switch to blue.
    pms_submit_camera_frame(g_e, red, 0, 0.10);  (void)render_frame(C + ".stamp");
    pms_submit_camera_frame(g_e, blue, 0, 0.30);
    Img out = render_frame(C + ".ghost");
    // Reference: the same blue frame with the stack cleared.
    cmd(C, "set_live_fx", {{"fx", json::array()}});
    Img plain = render_frame(C + ".plain");
    PxRect left  = {8, 8, W / 2 - 8, H - 8};
    PxRect right = {W / 2 + 8, 8, W - 8, H - 8};
    double dl = frac_diff(out, plain, 12, left);
    double dr = frac_diff(out, plain, 12, right);
    check(dl < 0.02, C, "subject half (matte=1) was ghosted (diff frac=" +
          std::to_string(dl) + ") — matte key not respected");
    check(dr > 0.05, C, "background half (matte=0) shows no ghost (diff frac=" +
          std::to_string(dr) + ") — matte-keyed feedback dead");
    pms_submit_camera_frame(g_e, NULL, 0, 0);
    pms_submit_person_matte(g_e, NULL, 0);
    CVPixelBufferRelease(matte);
    printf("  [n] matte-keyed chroma: subject crisp, background ghosts\n");
}

// o. face_fx end-to-end: the REAL record-mode makeup path — face models +
// tracker worker + camera side-feed + the Metal beauty/warp passes. Feeds the
// repo test portrait as camera frames, waits for the worker to lock on, then
// asserts a procedural makeup look (Barbie: blush/lip/lash + shape) visibly
// changes the frame. Skips (with a note) when models are absent.
static void case_face_fx(CVPixelBufferRef unused) {
    (void)unused;
    const std::string C = "o.face_fx";
    json fd = cmd(C, "face_track_enable", {{"on", true}});
    if (!fd.value("models_present", false)) {
        printf("  [o] face models missing — face_fx case SKIPPED\n");
        return;
    }
    int iw = 0, ih = 0, n = 0;
    unsigned char* rgb = stbi_load("assets/test_face.png", &iw, &ih, &n, 3);
    if (!rgb) fail(C, "cannot load assets/test_face.png");
    NSDictionary* attrs = @{ (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
                             (id)kCVPixelBufferMetalCompatibilityKey: @YES };
    CVPixelBufferRef pb = NULL;
    CVPixelBufferCreate(kCFAllocatorDefault, iw, ih, kCVPixelFormatType_32BGRA,
                        (__bridge CFDictionaryRef)attrs, &pb);
    if (!pb) fail(C, "camera pixel buffer alloc failed");
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

    build_video_project(C);
    clear_layer(0, 0);
    cmd(C, "set_live_fx", {{"fx", json::array()}});
    // Feed frames until the worker locks on (async — poll face_debug).
    bool locked = false;
    for (int k = 0; k < 100 && !locked; ++k) {
        pms_submit_camera_frame(g_e, pb, 0, 0.1);
        usleep(50 * 1000);
        locked = cmd(C, "face_debug", json::object()).value("valid", false);
    }
    check(locked, C, "face worker never produced a valid observation");
    Img plain = render_frame(C + ".plain");

    // UV-mesh material blend: dark pigment should keep skin detail, not flatten.
    const std::string goth_tex = "makeup_goth.png";
    bool goth_present = std::filesystem::exists(app_models_dir() + "/face/" + goth_tex);
    if (goth_present) {
        cmd(C, "set_live_fx",
            {{"fx", json::array({ {{"fx_type", "face_fx"},
                                   {"face_makeup_tex", goth_tex},
                                   {"params", {{"face_filter", 0.0},
                                               {"face_amount", 1.0}}}} })}});
        pms_submit_camera_frame(g_e, pb, 0, 0.2);
        Img goth = render_frame(C + ".goth");
        double gdiff = frac_diff(goth, plain, 6);
        check(gdiff > 0.01, C, "face_fx (makeup_goth) changed no pixels (diff=" +
              std::to_string(gdiff) + ")");
        PxRect eye = {0, 260, W, 300};
        RegionStats p_eye = region_stats(plain, eye);
        RegionStats g_eye = region_stats(goth, eye);
        check(g_eye.std_lum > p_eye.std_lum * 0.5, C,
              "dark pigment flattened source detail (goth std=" +
              std::to_string(g_eye.std_lum) + " vs plain=" + std::to_string(p_eye.std_lum) + ")");
        printf("  [o] face_fx/goth: diff=%.3f, detail preserved (std %.1f vs %.1f)\n",
               gdiff, g_eye.std_lum, p_eye.std_lum);
    }

    // Bright low-alpha gloss should add highlight, not paint white over the source.
    const std::string gloss_tex = "makeup_cherry_gloss.png";
    bool gloss_present = std::filesystem::exists(app_models_dir() + "/face/" + gloss_tex);
    if (gloss_present) {
        cmd(C, "set_live_fx",
            {{"fx", json::array({ {{"fx_type", "face_fx"},
                                   {"face_makeup_tex", gloss_tex},
                                   {"params", {{"face_filter", 0.0},
                                               {"face_amount", 1.0}}}} })}});
        pms_submit_camera_frame(g_e, pb, 0, 0.3);
        Img glossy = render_frame(C + ".gloss");
        double gldiff = frac_diff(glossy, plain, 6);
        check(gldiff > 0.01, C, "face_fx (makeup_cherry_gloss) changed no pixels (diff=" +
              std::to_string(gldiff) + ")");
        PxRect gloss = {0, 300, W, 400};
        RegionStats p_gloss = region_stats(plain, gloss);
        RegionStats a_gloss = region_stats(glossy, gloss);
        check(a_gloss.mean_lum < p_gloss.mean_lum + 12.0, C,
              "gloss over-brightened mean (cherry gloss mean=" +
              std::to_string(a_gloss.mean_lum) + " vs plain=" + std::to_string(p_gloss.mean_lum) + ")");
        check(a_gloss.max_chan < p_gloss.max_chan + 14 && a_gloss.max_chan < 252, C,
              "gloss clipped to white (cherry gloss max=" + std::to_string(a_gloss.max_chan) + ")");
        printf("  [o] face_fx/cherry_gloss: diff=%.3f, gloss non-clipped (max %d, mean %.1f)\n",
               gldiff, a_gloss.max_chan, a_gloss.mean_lum);
    }

    // Keep the original procedural smoke check.
    cmd(C, "set_live_fx",
        {{"fx", json::array({ {{"fx_type", "face_fx"},
                               {"params", {{"face_filter", 14.0},   // Barbie
                                           {"face_amount", 1.0}}}} })}});
    pms_submit_camera_frame(g_e, pb, 0, 0.4);
    Img out = render_frame(C + ".made_up");
    double diff = frac_diff(out, plain, 6);
    check(diff > 0.01, C, "face_fx (Barbie) changed no pixels (diff frac=" +
          std::to_string(diff) + ") — makeup passes dead");
    cmd(C, "set_live_fx", {{"fx", json::array()}});
    cmd(C, "face_track_enable", {{"on", false}});
    pms_submit_camera_frame(g_e, NULL, 0, 0);
    CVPixelBufferRelease(pb);
    printf("  [o] face_fx: tracker locked, makeup passes change pixels (%.3f)\n", diff);
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    @autoreleasepool {
        // FX shader registry: point the runner at the transpiled MSL dir
        // BEFORE pms_create (metal_render_init loads the manifest).
        const char* sd = getenv("PMS_SHADER_DIR");
        std::string shader_dir = sd ? sd : std::string(getenv("HOME") ? getenv("HOME") : "")
                                              + "/dev/pms-ios/Shaders/msl";
        metal_render_set_shader_dir(shader_dir.c_str());

        g_dev = MTLCreateSystemDefaultDevice();
        if (!g_dev) { fprintf(stderr, "metal render test: FAIL [setup] no Metal device\n"); return 2; }
        const char* asset_root = getenv("PMS_ASSET_ROOT");
        if (!asset_root) asset_root = "";
        g_e = pms_create((__bridge void*)g_dev, asset_root, "/tmp/pms-metal-render-test");
        if (!g_e) { fprintf(stderr, "metal render test: FAIL [setup] pms_create failed\n"); return 2; }

        json dbg = fx_debug("setup");
        if (dbg.value("manifest_count", 0) < 50)
            fail("setup", "FX manifest not loaded from '" + shader_dir +
                          "' (manifest_count=" + std::to_string(dbg.value("manifest_count", 0)) +
                          ") — set PMS_SHADER_DIR to Shaders/msl");

        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeManaged;
        g_target = [g_dev newTextureWithDescriptor:td];
        g_rq = [g_dev newCommandQueue];

        CVPixelBufferRef green    = make_solid(W, H, 20, 200, 20);
        CVPixelBufferRef gradient = make_gradient(W, H);
        CVPixelBufferRef red      = make_solid(W, H, 30, 30, 220);
        CVPixelBufferRef blue     = make_solid(W, H, 220, 40, 30);
        CVPixelBufferRef darkblue = make_solid(W, H, 180, 40, 20);
        CVPixelBufferRef yellow   = make_solid(W, H, 20, 210, 230);
        CVPixelBufferRef halfred  = make_solid(W, H, 0, 0, 128, 128);  // premultiplied 50% red
        CVPixelBufferRef checker  = make_checker(W, H);

        case_background_only();
        case_one_layer(green);
        case_glass_fx(gradient);
        case_fx_window(gradient);
        clear_layer(0, 0);
        case_fx_order(gradient);
        case_transition(red, blue);
        case_text_layer(darkblue, halfred);
        case_bus_scope(gradient, yellow);
        case_wet_dry(checker);
        case_all_manifest_fx(checker);
        case_legacy_fx(gradient, green, checker);
        case_chroma_feedback(green, gradient);
        case_chroma_matte_key(red, blue);
        case_face_fx(checker);

        CVPixelBufferRelease(green);    CVPixelBufferRelease(gradient);
        CVPixelBufferRelease(red);      CVPixelBufferRelease(blue);
        CVPixelBufferRelease(darkblue); CVPixelBufferRelease(yellow);
        CVPixelBufferRelease(halfred);  CVPixelBufferRelease(checker);

        pms_destroy(g_e);
        printf("metal render test: PASS\n");
    }
    return 0;
}
