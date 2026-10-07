// arkit_native_replay.mm — renders ARKit makeup looks through the real engine.
//
// Fixture mode: replays a capture recorded on the phone (triple-tap in the
// record screen → Documents/arkit_capture_<ts>/: frames.jsonl + fNNNN.jpg).
// Every recorded ARFrame's own camera image is submitted with its own mesh,
// matrices, blendshapes and light estimate — exactly what the app submits —
// so the PNGs show the look on the wearer's real face, in its real light.
//
//   arkit-native-replay <capture_dir> <out_dir> <look_id> [amount=1]
//                       [--every N=15] [--frames i,j,...] [--raw]
//
//   Writes <out_dir>/fNNNN.png for every Nth frame plus the hardest frames
//   (max blink, max jaw-open, max smile, max head yaw); --raw also writes the
//   untouched frame as fNNNN_raw.png for before/after review.
//
// Synthetic mode: the canonical ARKit head over flat skin — neutral, blink,
// yaw — with assertions (CI-able, no capture needed):
//
//   arkit-native-replay synth <out_dir> <look_id> [amount=1]
//
//   PMS_CANONICAL_OBJ overrides tools/arkit_face_canonical.obj. Env as usual:
//   PMS_ASSET_ROOT (pms-ios Engine/EngineAssets), PMS_SHADER_DIR.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#define Marker PMSCarbonAIFFMarker
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#undef Marker

#include "../src/pms_engine.h"
#include "../src/metal_render.h"
#include "stb_image.h"
#include "stb_image_write.h"
#include "json.hpp"

using json = nlohmann::json;

static int W = 1080, H = 1440;
static pms_engine*         g_e = nullptr;
static id<MTLDevice>       g_dev = nil;
static id<MTLTexture>      g_target = nil;
static id<MTLCommandQueue> g_rq = nil;

// MediaPipe-order blendshape slots (pms-ios ARKitBlendshapes.swift).
enum { kBlinkL = 9, kBlinkR = 10, kJawOpen = 25, kSmileL = 44, kSmileR = 45 };

static void fail(const std::string& m) {
    fprintf(stderr, "arkit native replay: FAIL %s\n", m.c_str());
    exit(1);
}

static json cmd(const std::string& method, const json& params = json::object()) {
    json req = {{"id", "t"}, {"method", method}, {"params", params}};
    char* r = pms_command(g_e, req.dump().c_str());
    std::string out = r ? r : "";
    pms_free(r);
    json reply = json::parse(out, nullptr, false);
    if (reply.is_discarded()) fail(method + ": bad reply " + out);
    if (reply.contains("error")) fail(method + ": " + reply["error"].dump());
    return reply.value("result", json::object());
}

static void set_look(const std::string& look, double amount) {
    cmd("set_live_fx", {{"fx", json::array({{{"fx_type", "face_fx"},
                                              {"face_look", look},
                                              {"params", {{"face_amount", amount}}}}})}});
}

static std::string face_status() {
    json d = cmd("fx_debug");
    auto stk = d.value("stack", json::array());
    return stk.empty() ? std::string() : stk[0].value("status", std::string());
}

static void make_target() {
    MTLTextureDescriptor* td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
        width:(NSUInteger)W height:(NSUInteger)H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeManaged;
    g_target = [g_dev newTextureWithDescriptor:td];
}

struct Img { std::vector<uint8_t> px; };   // BGRA, W×H

static Img render() {
    if (pms_render(g_e, (__bridge void*)g_target, W, H) != 0) fail("render rc");
    pms_render_wait(g_e);
    id<MTLCommandBuffer> cb = [g_rq commandBuffer];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl synchronizeResource:g_target];
    [bl endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    Img im;
    im.px.resize((size_t)W * H * 4);
    [g_target getBytes:im.px.data() bytesPerRow:(NSUInteger)W * 4
            fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)W, (NSUInteger)H) mipmapLevel:0];
    return im;
}

static void write_png(const std::string& path, const uint8_t* bgra) {
    std::vector<uint8_t> rgb((size_t)W * H * 3);
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        rgb[i * 3 + 0] = bgra[i * 4 + 2];
        rgb[i * 3 + 1] = bgra[i * 4 + 1];
        rgb[i * 3 + 2] = bgra[i * 4 + 0];
    }
    if (!stbi_write_png(path.c_str(), W, H, 3, rgb.data(), W * 3)) fail("write " + path);
}

static CVPixelBufferRef make_frame() {
    NSDictionary* attrs = @{ (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
                             (id)kCVPixelBufferMetalCompatibilityKey: @YES };
    CVPixelBufferRef pb = NULL;
    CVPixelBufferCreate(kCFAllocatorDefault, (size_t)W, (size_t)H, kCVPixelFormatType_32BGRA,
                        (__bridge CFDictionaryRef)attrs, &pb);
    if (!pb) fail("pixel buffer alloc");
    return pb;
}

// RGB rows → the BGRA camera buffer (the app submits sRGB-encoded BGRA).
static void fill_rgb(CVPixelBufferRef pb, const uint8_t* rgb) {
    CVPixelBufferLockBaseAddress(pb, 0);
    uint8_t* dst = (uint8_t*)CVPixelBufferGetBaseAddress(pb);
    const size_t bpr = CVPixelBufferGetBytesPerRow(pb);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const uint8_t* s = rgb + ((size_t)y * W + x) * 3;
            uint8_t* q = dst + (size_t)y * bpr + (size_t)x * 4;
            q[0] = s[2]; q[1] = s[1]; q[2] = s[0]; q[3] = 255;
        }
    CVPixelBufferUnlockBaseAddress(pb, 0);
}

static std::vector<float> floats(const json& a) {
    std::vector<float> v;
    for (const auto& x : a) v.push_back(x.get<float>());
    return v;
}

static pms_arkit_light light_of(const json& rec) {
    pms_arkit_light l{};
    l.ambient_intensity = 1000.f;
    l.ambient_kelvin = 6500.f;
    if (!rec.contains("light")) return l;
    const json& j = rec["light"];
    l.ambient_intensity = j.value("ambient", 1000.f);
    l.ambient_kelvin = j.value("kelvin", 6500.f);
    if (j.contains("dir") && j.contains("sh")) {
        auto d = floats(j["dir"]);
        auto sh = floats(j["sh"]);
        for (int i = 0; i < 3 && i < (int)d.size(); ++i) l.primary_dir[i] = d[(size_t)i];
        for (int i = 0; i < 27 && i < (int)sh.size(); ++i) l.sh[i] = sh[(size_t)i];
        l.primary_intensity = j.value("intensity", 0.f);
        l.directional = 1;
    }
    return l;
}

static int run_fixture(const std::string& dir, const std::string& out, const std::string& look,
                       double amount, int every, const std::set<int>& only, bool raw) {
    std::vector<json> recs;
    {
        std::ifstream in(dir + "/frames.jsonl");
        if (!in) fail("open " + dir + "/frames.jsonl");
        std::string line;
        while (std::getline(in, line)) {
            json r = json::parse(line, nullptr, false);
            if (!r.is_discarded() && r.contains("verts")) recs.push_back(std::move(r));
        }
    }
    if (recs.empty()) fail("no frames in " + dir);
    W = recs[0].value("w", 1080);
    H = recs[0].value("h", 1440);
    make_target();

    // The hardest frames always get a PNG, even between the every-N picks.
    auto bs = [](const json& r, int k) {
        const auto& b = r["blend"];
        return k < (int)b.size() ? b[(size_t)k].get<float>() : 0.f;
    };
    int best[4] = {0, 0, 0, 0};
    float score[4] = {-1.f, -1.f, -1.f, -1.f};
    for (int i = 0; i < (int)recs.size(); ++i) {
        const json& r = recs[(size_t)i];
        float s[4] = {std::max(bs(r, kBlinkL), bs(r, kBlinkR)), bs(r, kJawOpen),
                      std::max(bs(r, kSmileL), bs(r, kSmileR)),
                      std::fabs(r["model"][8].get<float>())};
        for (int k = 0; k < 4; ++k)
            if (s[k] > score[k]) { score[k] = s[k]; best[k] = i; }
    }
    std::set<int> picks = only;
    if (picks.empty()) {
        for (int i = 0; i < (int)recs.size(); i += std::max(every, 1)) picks.insert(i);
        for (int k : best) picks.insert(k);
    }

    set_look(look, amount);
    CVPixelBufferRef pb = make_frame();
    int written = 0;
    for (int i = 0; i < (int)recs.size(); ++i) {
        const json& r = recs[(size_t)i];
        if (!r.contains("img")) continue;
        int iw = 0, ih = 0, n = 0;
        std::string img = dir + "/" + r["img"].get<std::string>();
        uint8_t* rgb = stbi_load(img.c_str(), &iw, &ih, &n, 3);
        if (!rgb) continue;                       // encode skipped on the phone
        if (iw != W || ih != H) fail("frame size mismatch in " + img);
        fill_rgb(pb, rgb);
        auto v = floats(r["verts"]);
        auto m = floats(r["model"]), vw = floats(r["view"]), pj = floats(r["proj"]);
        auto bl = floats(r["blend"]);
        if (v.size() != 1220 * 3 || m.size() != 16 || vw.size() != 16 || pj.size() != 16)
            fail("bad record " + std::to_string(i));
        pms_arkit_light light = light_of(r);
        const double t = r.value("t", (double)i / 30.0);
        pms_submit_camera_frame(g_e, pb, 0, t);
        pms_submit_arkit_face_3d(g_e, v.data(), m.data(), vw.data(), pj.data(),
                                 bl.size() == 52 ? bl.data() : nullptr, &light, 1, W, H);
        Img im = render();       // every frame renders: the vertex filter runs in time
        if (picks.count(i)) {
            const std::string st = face_status();
            if (st != "applied") fail("frame " + std::to_string(i) + ": face_fx status " + st);
            char nm[64];
            snprintf(nm, sizeof nm, "/f%04d.png", i);
            write_png(out + nm, im.px.data());
            if (raw) {
                std::vector<uint8_t> src((size_t)W * H * 4);
                for (size_t p = 0; p < (size_t)W * H; ++p) {
                    src[p * 4 + 0] = rgb[p * 3 + 2];
                    src[p * 4 + 1] = rgb[p * 3 + 1];
                    src[p * 4 + 2] = rgb[p * 3 + 0];
                    src[p * 4 + 3] = 255;
                }
                snprintf(nm, sizeof nm, "/f%04d_raw.png", i);
                write_png(out + nm, src.data());
            }
            ++written;
        }
        stbi_image_free(rgb);
    }
    CVPixelBufferRelease(pb);
    printf("arkit native replay: %d frames, %d PNGs in %s (blink f%04d, jaw f%04d, smile f%04d, yaw f%04d)\n",
           (int)recs.size(), written, out.c_str(), best[0], best[1], best[2], best[3]);
    return 0;
}

// ── synthetic canonical head ─────────────────────────────────────────────────
static void ident(float* m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1.f; }
static void mul(const float* a, const float* b, float* o) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float acc = 0;
            for (int k = 0; k < 4; ++k) acc += a[k * 4 + r] * b[c * 4 + k];
            o[c * 4 + r] = acc;
        }
}

static int run_synth(const std::string& out, const std::string& look, double amount) {
    W = 720; H = 1280;
    make_target();
    const char* op = getenv("PMS_CANONICAL_OBJ");
    std::string obj = op ? op : "tools/arkit_face_canonical.obj";
    static float verts[1220][3];
    {
        FILE* f = fopen(obj.c_str(), "r");
        if (!f) fail("open " + obj + " (run from the engine repo root or set PMS_CANONICAL_OBJ)");
        char line[256];
        int nv = 0;
        while (fgets(line, sizeof line, f) && nv < 1220) {
            if (line[0] != 'v' || line[1] != ' ') continue;
            float x, y, z;
            sscanf(line + 2, "%f %f %f", &x, &y, &z);
            verts[nv][0] = x * 1e-3f; verts[nv][1] = y * 1e-3f; verts[nv][2] = z * 1e-3f;
            ++nv;
        }
        fclose(f);
        if (nv != 1220) fail("canonical obj vertex count");
    }
    // Flat skin with fine deterministic texture (pigment must keep it).
    std::vector<uint8_t> rgb((size_t)W * H * 3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint32_t h32 = (uint32_t)(x * 374761393u + y * 668265263u);
            h32 = (h32 ^ (h32 >> 13)) * 1274126177u;
            int n = (int)((h32 >> 8) & 0xff) / 16 - 8;
            const int base[3] = {214, 172, 156};
            for (int c = 0; c < 3; ++c)
                rgb[((size_t)y * W + x) * 3 + c] = (uint8_t)std::clamp(base[c] + n, 0, 255);
        }
    CVPixelBufferRef pb = make_frame();
    fill_rgb(pb, rgb.data());

    // Camera at the origin looking down -z; face 40 cm out.
    float model[16], view[16], proj[16];
    ident(view);
    ident(model); model[14] = -0.40f;
    memset(proj, 0, sizeof proj);
    const float fy = 1.f / tanf(42.f * (float)M_PI / 360.f);
    const float zn = 0.01f, zf = 10.f;
    proj[0] = fy * (float)H / (float)W; proj[5] = fy;
    proj[10] = zf / (zn - zf); proj[11] = -1.f; proj[14] = zn * zf / (zn - zf);
    pms_arkit_light light{};
    light.primary_dir[0] = -0.3f; light.primary_dir[1] = -0.6f; light.primary_dir[2] = -0.75f;
    light.primary_intensity = 1200.f; light.ambient_intensity = 1000.f;
    light.ambient_kelvin = 5500.f; light.directional = 1;
    float blend[52] = {};

    const int arcR[10] = {1100, 1099, 1098, 1097, 1096, 1095, 1094, 1093, 1092, 1091};
    const int arcL[10] = {1070, 1071, 1072, 1073, 1074, 1075, 1076, 1077, 1078, 1079};
    struct Frame { const char* tag; float blink_mm; float yaw; };
    const Frame frames[] = {{"neutral", 0.f, 0.f}, {"blink", 6.f, 0.f},
                            {"yaw", 0.f, 22.f * (float)M_PI / 180.f}};
    double t = 1.0;
    auto submit = [&](const float (*v)[3], const float* m, const float* b) {
        pms_submit_camera_frame(g_e, pb, 0, t);
        pms_submit_arkit_face_3d(g_e, &v[0][0], m, view, proj, b, &light, 1, W, H);
        t += 0.2;   // > the vertex filter's reset gap: each pose renders unfiltered
    };
    set_look(look, amount);
    std::vector<Img> outs;
    for (const Frame& fr : frames) {
        static float v2[1220][3];
        memcpy(v2, verts, sizeof v2);
        for (int e = 0; e < 2 && fr.blink_mm > 0.f; ++e)
            for (int k = 0; k < 10; ++k)
                v2[(e ? arcL : arcR)[k]][1] -=
                    fr.blink_mm * 1e-3f * sinf((float)M_PI * (float)(k + 1) / 11.f);
        float m2[16];
        memcpy(m2, model, sizeof m2);
        if (fr.yaw != 0.f) {
            float rot[16]; ident(rot);
            rot[0] = cosf(fr.yaw); rot[2] = -sinf(fr.yaw);
            rot[8] = sinf(fr.yaw); rot[10] = cosf(fr.yaw);
            mul(model, rot, m2);
        }
        blend[kBlinkL] = blend[kBlinkR] = fr.blink_mm > 0.f ? 0.9f : 0.f;
        submit(v2, m2, blend);
        outs.push_back(render());
        if (face_status() != "applied") fail(std::string(fr.tag) + ": status " + face_status());
        write_png(out + "/" + fr.tag + ".png", outs.back().px.data());
    }
    blend[kBlinkL] = blend[kBlinkR] = 0.f;
    cmd("face_overlay", {{"on", true}});
    submit(verts, model, blend);
    Img overlay = render();
    write_png(out + "/overlay.png", overlay.px.data());
    cmd("face_overlay", {{"on", false}});
    set_look("no_such_look", amount);
    submit(verts, model, blend);
    render();
    const std::string missing = face_status();
    cmd("set_live_fx", {{"fx", json::array()}});
    submit(verts, model, blend);
    Img plain = render();
    write_png(out + "/plain.png", plain.px.data());

    // ── invariants ──
    int fails = 0;
    auto expect = [&](bool ok, const std::string& what) {
        if (!ok) { fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++fails; }
    };
    auto changed = [&](const Img& a, const Img& b, int y0, int y1) {
        long n = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                const uint8_t* p = a.px.data() + ((size_t)y * W + x) * 4;
                const uint8_t* q = b.px.data() + ((size_t)y * W + x) * 4;
                int d = 0;
                for (int c = 0; c < 3; ++c) d = std::max(d, abs((int)p[c] - (int)q[c]));
                n += d > 10;
            }
        return n;
    };
    const long face = changed(outs[0], plain, H / 4, 3 * H / 4);
    expect(face > 4000, "makeup renders on the face (" + std::to_string(face) + " px)");
    expect(changed(outs[0], plain, 0, H / 10) == 0, "nothing renders above the face");
    expect(changed(outs[0], plain, H - H / 10, H) == 0, "nothing renders below the face");
    const long eyes = changed(outs[1], outs[0], (int)(H * 0.30), (int)(H * 0.46));
    const long mouth = changed(outs[1], outs[0], (int)(H * 0.60), (int)(H * 0.78));
    expect(eyes > 300, "blink moves the eye makeup (" + std::to_string(eyes) + " px)");
    expect(mouth < 150, "blink leaves the mouth alone (" + std::to_string(mouth) + " px)");
    expect(changed(outs[2], plain, H / 4, 3 * H / 4) > 4000, "makeup stays on under head yaw");
    expect(changed(overlay, outs[0], H / 4, 3 * H / 4) > 4000, "face_overlay renders the QA view");
    expect(missing == "look_missing", "unknown look reports look_missing (got " + missing + ")");
    CVPixelBufferRelease(pb);
    if (fails) { fprintf(stderr, "arkit native replay: FAIL (%d)\n", fails); return 1; }
    printf("arkit native replay (synth): PASS\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <capture_dir> <out_dir> <look_id> [amount] [--every N] [--frames i,j] [--raw]\n"
                "       %s synth <out_dir> <look_id> [amount]\n", argv[0], argv[0]);
        return 2;
    }
    const std::string src = argv[1], out = argv[2], look = argv[3];
    double amount = 1.0;
    int every = 15;
    bool raw = false;
    std::set<int> only;
    for (int i = 4; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--every" && i + 1 < argc) every = atoi(argv[++i]);
        else if (a == "--raw") raw = true;
        else if (a == "--frames" && i + 1 < argc) {
            std::string list = argv[++i];
            for (size_t p = 0; p < list.size();) {
                size_t q = list.find(',', p);
                only.insert(atoi(list.substr(p, q - p).c_str()));
                if (q == std::string::npos) break;
                p = q + 1;
            }
        } else amount = atof(a.c_str());
    }
    @autoreleasepool {
        const char* sd = getenv("PMS_SHADER_DIR");
        const std::string home = getenv("HOME") ? getenv("HOME") : "";
        metal_render_set_shader_dir(sd ? sd : (home + "/dev/pms-ios/Shaders/msl").c_str());
        g_dev = MTLCreateSystemDefaultDevice();
        if (!g_dev) fail("no Metal device");
        const char* ar = getenv("PMS_ASSET_ROOT");
        const std::string assets = ar ? ar : home + "/dev/pms-ios/Engine/EngineAssets";
        g_e = pms_create((__bridge void*)g_dev, assets.c_str(), "/tmp/pms-arkit-replay");
        if (!g_e) fail("pms_create");
        g_rq = [g_dev newCommandQueue];
        int rc = src == "synth" ? run_synth(out, look, amount)
                                : run_fixture(src, out, look, amount, every, only, raw);
        pms_destroy(g_e);
        return rc;
    }
}
