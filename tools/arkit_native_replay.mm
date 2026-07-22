// arkit_native_replay.mm — QA harness for the native tier-1 ARKit path
// (docs/ARKIT_NATIVE_PLAN.md). Feeds synthetic 3D fixtures (canonical mesh +
// scripted blink/gaze/yaw) through pms_submit_arkit_face_3d and the real
// render, writes PNGs for eyes-on review, and asserts the structural
// properties the 2D bridge kept failing: pigment inside the projected face,
// eyeshadow riding the lid on blink, iris following gaze.
//
// Usage: arkit-native-replay <arkit_face_canonical.obj> <out_prefix>
//        [filter_id=13] [amount=1.0]
// Env:   PMS_NATIVE_ATLAS=checker.png  — force the checker atlas (Phase 1).
//        PMS_ASSET_ROOT / PMS_SHADER_DIR as usual.
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define Marker PMSCarbonAIFFMarker
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#undef Marker

#include "../src/pms_engine.h"
#include "../src/metal_render.h"
#include "stb_image_write.h"
#include "json.hpp"

using json = nlohmann::json;

static int W = 720, H = 1280;   // fixture mode adopts the capture viewport

static pms_engine*         g_e = nullptr;
static id<MTLDevice>       g_dev = nil;
static id<MTLTexture>      g_target = nil;
static id<MTLCommandQueue> g_rq = nil;

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

static void write_png(const std::string& path, const Img& im) {
    std::vector<uint8_t> rgb((size_t)W * H * 3);
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        rgb[i * 3 + 0] = im.px[i * 4 + 2];
        rgb[i * 3 + 1] = im.px[i * 4 + 1];
        rgb[i * 3 + 2] = im.px[i * 4 + 0];
    }
    if (!stbi_write_png(path.c_str(), W, H, 3, rgb.data(), W * 3))
        fail("write " + path);
    printf("wrote %s\n", path.c_str());
}

// Flat skin-tone camera frame (pigment shows on it like on skin).
static uint8_t g_skin[3] = {205, 170, 150};   // r,g,b — override with --skin
static CVPixelBufferRef skin_frame() {
    NSDictionary* attrs = @{ (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
                             (id)kCVPixelBufferMetalCompatibilityKey: @YES };
    CVPixelBufferRef pb = NULL;
    CVPixelBufferCreate(kCFAllocatorDefault, W, H, kCVPixelFormatType_32BGRA,
                        (__bridge CFDictionaryRef)attrs, &pb);
    CVPixelBufferLockBaseAddress(pb, 0);
    uint8_t* dst = (uint8_t*)CVPixelBufferGetBaseAddress(pb);
    size_t bpr = CVPixelBufferGetBytesPerRow(pb);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t* q = dst + (size_t)y * bpr + (size_t)x * 4;
            // Subtle deterministic skin texture: value noise + a soft radial
            // falloff. A flat fill hides exactly what art QA needs to see —
            // whether pigment preserves skin detail (realism) or plasters
            // over it (decal). Mean stays g_skin.
            uint32_t h32 = (uint32_t)(x * 374761393u + y * 668265263u);
            h32 = (h32 ^ (h32 >> 13)) * 1274126177u;
            int n = (int)((h32 >> 8) & 0xff) - 128;
            float dx = (float)x / W - 0.5f, dy = (float)y / H - 0.5f;
            int grad = (int)(-28.0f * (dx * dx + dy * dy));
            int delta = n / 16 + grad;
            // QA: fake occluder — a dark diagonal bar across the frame so
            // the skin-gate must suppress makeup where it crosses the face.
            if (getenv("PMS_FAKE_OCCLUDER")) {
                float dbar = fabsf((float)x / W - (float)y / H);
                if (dbar < 0.045) delta = -110;
            }
            for (int c = 0; c < 3; ++c) {
                int v = g_skin[2 - c] + delta;
                q[c] = (uint8_t)std::min(255, std::max(0, v));
            }
            q[3] = 255;
        }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    return pb;
}

// column-major helpers
static void ident(float* m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1.f; }
static void mul(const float* a, const float* b, float* o) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float acc = 0;
            for (int k = 0; k < 4; ++k) acc += a[k * 4 + r] * b[c * 4 + k];
            o[c * 4 + r] = acc;
        }
}
static void xform(const float* m, const float* v, float w, float* o) {
    for (int r = 0; r < 4; ++r)
        o[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * w;
}

// Upper-arc verts per eye (x-ordered outer->inner; see arkit_map_smoke.cpp).
static const int kArcR[10] = {1100, 1099, 1098, 1097, 1096, 1095, 1094, 1093, 1092, 1091};
static const int kArcL[10] = {1079, 1078, 1077, 1076, 1075, 1074, 1073, 1072, 1071, 1070};

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <arkit_face_canonical.obj> <out_prefix> "
                        "[filter_id=13] [amount=1.0]\n", argv[0]);
        return 2;
    }
    const std::string obj_path = argv[1], prefix = argv[2];
    const bool fixture_mode = obj_path.size() > 6 &&
        obj_path.substr(obj_path.size() - 6) == ".jsonl";
    const double filter_id = argc > 3 ? atof(argv[3]) : 13.0;
    const double amount    = argc > 4 ? atof(argv[4]) : 1.0;
    if (argc > 5) {   // --skin support: "r,g,b" as the 5th arg
        int r2, g2, b2;
        if (sscanf(argv[5], "%d,%d,%d", &r2, &g2, &b2) == 3) {
            g_skin[0] = (uint8_t)r2; g_skin[1] = (uint8_t)g2;
            g_skin[2] = (uint8_t)b2;
        }
    }

    // canonical mesh (mm) -> meters (synthetic mode only)
    static float verts[1220][3];
    if (!fixture_mode) {
        int nv = 0;
        FILE* f = fopen(obj_path.c_str(), "r");
        if (!f) fail("open " + obj_path);
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (line[0] == 'v' && line[1] == ' ' && nv < 1220) {
                float x, y, z;
                sscanf(line + 2, "%f %f %f", &x, &y, &z);
                verts[nv][0] = x * 0.001f; verts[nv][1] = y * 0.001f;
                verts[nv][2] = z * 0.001f;
                ++nv;
            }
        }
        fclose(f);
        if (nv != 1220) fail("verts");
    }

    @autoreleasepool {
        const char* sd = getenv("PMS_SHADER_DIR");
        std::string shader_dir = sd ? sd
            : std::string(getenv("HOME")) + "/dev/pms-ios/Shaders/msl";
        metal_render_set_shader_dir(shader_dir.c_str());
        g_dev = MTLCreateSystemDefaultDevice();
        if (!g_dev) fail("no metal device");
        const char* ar = getenv("PMS_ASSET_ROOT");
        std::string assets = ar ? ar
            : std::string(getenv("HOME")) + "/dev/pms-ios/Engine/EngineAssets";
        g_e = pms_create((__bridge void*)g_dev, assets.c_str(),
                         "/tmp/pms-arkit-replay");
        if (!g_e) fail("pms_create");
        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeManaged;
        g_target = [g_dev newTextureWithDescriptor:td];
        g_rq = [g_dev newCommandQueue];

        if (fixture_mode) {
            // Real-face replay: render recorded per-frame geometry/matrices
            // (docs/ARKIT_NATIVE_PLAN.md Phase 0) and write PNGs every 45
            // frames for eyes-on art review on the wearer's true
            // proportions. Assertions don't apply — this is a review tool.
            // Neutral skin bundle when QA-forcing an atlas: without these
            // overrides the run inherits the filter's own skin params (Goth's
            // 0.62 smooth / 0.28 desat washed out every look in review).
            json params = {{"face_filter", filter_id},
                           {"face_amount", amount}};
            if (getenv("PMS_NATIVE_ATLAS")) {
                params["smooth"] = 0.40; params["brighten"] = 0.15;
                params["warmth"] = 0.0; params["desat"] = 0.0;
                params["chrome"] = 0.0; params["scanlines"] = 0.0;
                params["skin_tint"] = 0.0; params["eye_pop"] = 0.0;
            }
            cmd("set_live_fx",
                {{"fx", json::array({ {{"fx_type", "face_fx"},
                   {"params", params}} })}});
            FILE* jf = fopen(obj_path.c_str(), "r");
            if (!jf) fail("open " + obj_path);
            // Pre-scan: find the interesting frames — max blink, extreme gaze,
            // extreme yaw — so art review always includes the hard cases even
            // when they fall between the every-45 dumps.
            int blink_frame = -1, gaze_frame = -1, yaw_frame = -1;
            {
                float best_blink = 0.3f, best_gaze = 0.3f, best_yaw = 0.f;
                std::string pl;
                int pc, pi = 0;
                while ((pc = fgetc(jf)) != EOF) {
                    if (pc != '\n') { pl.push_back((char)pc); continue; }
                    if (!pl.empty()) {
                        json pr = json::parse(pl, nullptr, false);
                        pl.clear();
                        if (!pr.is_discarded() && pr.contains("blend")) {
                            auto& b = pr["blend"];
                            auto bs = [&](int k) { return k < (int)b.size() ? (float)b[k].get<double>() : 0.f; };
                            float blink = std::max(bs(9), bs(10));
                            float gaze = std::max(std::max(bs(15), bs(16)),
                                                  std::max(bs(17), bs(18)));
                            float yawr = 0.f;
                            if (pr.contains("model")) {
                                auto& m = pr["model"];
                                // column 2 x-component ~ face normal yaw
                                yawr = std::fabs((float)m[8].get<double>());
                            }
                            if (blink > best_blink) { best_blink = blink; blink_frame = pi; }
                            if (gaze > best_gaze) { best_gaze = gaze; gaze_frame = pi; }
                            if (yawr > best_yaw) { best_yaw = yawr; yaw_frame = pi; }
                        }
                    }
                    ++pi;
                }
                rewind(jf);
            }
            std::string linebuf;
            int ch2, fidx = 0, written = 0;
            CVPixelBufferRef fpb = nullptr;
            double t2 = 1.0;
            while ((ch2 = fgetc(jf)) != EOF) {
                if (ch2 != '\n') { linebuf.push_back((char)ch2); continue; }
                if (linebuf.empty()) continue;
                json rec = json::parse(linebuf, nullptr, false);
                linebuf.clear();
                if (rec.is_discarded()) continue;
                if (!fpb) {
                    int vw = rec.value("w", 1080), vh = rec.value("h", 1440);
                    W = vw; H = vh;   // full-res: half-res hides liner/lash adhesion artifacts
                    [g_target release];
                    MTLTextureDescriptor* td2 = [MTLTextureDescriptor
                        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                        width:W height:H mipmapped:NO];
                    td2.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                    td2.storageMode = MTLStorageModeManaged;
                    g_target = [g_dev newTextureWithDescriptor:td2];
                    fpb = skin_frame();
                }
                static float fv[1220 * 3];
                auto& jv = rec["verts"];
                for (int k = 0; k < 1220 * 3 && k < (int)jv.size(); ++k)
                    fv[k] = (float)jv[k].get<double>();
                float fm[16], fvw[16], fpj[16], fel[16], fer[16];
                auto getm = [&](const char* key, float* out) {
                    auto& a2 = rec[key];
                    for (int k = 0; k < 16; ++k)
                        out[k] = (float)a2[k].get<double>();
                };
                getm("model", fm); getm("view", fvw); getm("proj", fpj);
                getm("eye_l", fel); getm("eye_r", fer);
                static float fb[52];
                auto& jb = rec["blend"];
                for (int k = 0; k < 52 && k < (int)jb.size(); ++k)
                    fb[k] = (float)jb[k].get<double>();
                pms_submit_camera_frame(g_e, fpb, 0, t2);
                pms_submit_arkit_face_3d(g_e, fv, fm, fvw, fpj, fel, fer,
                                         fb, 1, rec.value("w", 1080),
                                         rec.value("h", 1440));
                int rc2 = pms_render(g_e, (__bridge void*)g_target, W, H);
                if (rc2 != 0) fail("render rc (fixture)");
                pms_render_wait(g_e);
                bool dump = (fidx % 45 == 0);
                const char* tag = nullptr;
                if (fidx == blink_frame) { dump = true; tag = "blink"; }
                else if (fidx == gaze_frame) { dump = true; tag = "gaze"; }
                else if (fidx == yaw_frame) { dump = true; tag = "yaw"; }
                if (dump) {
                    Img im2 = read_target();
                    char nm[64];
                    if (tag) snprintf(nm, sizeof nm, "_%s_f%03d.png", tag, fidx);
                    else snprintf(nm, sizeof nm, "_f%03d.png", fidx);
                    write_png(prefix + nm, im2);
                    ++written;
                }
                ++fidx; t2 += 0.017;
            }
            fclose(jf);
            CVPixelBufferRelease(fpb);
            pms_destroy(g_e);
            printf("arkit native replay (fixture): %d frames, %d PNGs\n",
                   fidx, written);
            return 0;
        }

        CVPixelBufferRef pb = skin_frame();

        // camera: at origin looking -z; face 40cm out.
        float model[16], view[16], proj[16];
        ident(view);
        ident(model); model[14] = -0.40f;
        memset(proj, 0, sizeof proj);
        const float fovy = 42.f * (float)M_PI / 180.f;
        const float fy = 1.f / tanf(fovy * 0.5f);
        const float aspect = (float)W / (float)H, zn = 0.01f, zf = 10.f;
        proj[0] = fy / aspect; proj[5] = fy;
        proj[10] = zf / (zn - zf); proj[11] = -1.f;
        proj[14] = zn * zf / (zn - zf);

        // eyeball anchors: centers behind the hole corner midpoints.
        auto eye_mat = [&](int c0, int c1, float yaw_rad, float* out) {
            ident(out);
            out[12] = 0.5f * (verts[c0][0] + verts[c1][0]);
            out[13] = 0.5f * (verts[c0][1] + verts[c1][1]) + 0.001f;
            out[14] = 0.5f * (verts[c0][2] + verts[c1][2]) - 0.0122f;
            // gaze = -Z column; neutral gaze toward camera (+z model).
            float cs = cosf(yaw_rad), sn = sinf(yaw_rad);
            out[0] = cs;  out[2] = sn;           // x axis
            out[8] = -sn; out[10] = -cs;         // z axis (-z = gaze)
            out[5] = 1.f;
        };
        float blend[52] = {0};

        struct Frame { const char* tag; float blink_mm; float gaze_yaw;
                       float head_yaw; };
        const Frame frames[] = {
            {"neutral", 0.f, 0.f, 0.f},
            {"blink",   7.f, 0.f, 0.f},
            {"gaze",    0.f, 25.f * (float)M_PI / 180.f, 0.f},
            {"yaw",     0.f, 0.f, 18.f * (float)M_PI / 180.f},
        };

        cmd("set_live_fx",
            {{"fx", json::array({ {{"fx_type", "face_fx"},
               {"params", {{"face_filter", filter_id},
                           {"face_amount", amount},
                           {"iris_tint", 0.8},
                           {"iris_r", 0.25}, {"iris_g", 0.85},
                           {"iris_b", 0.55}}}} })}});

        std::vector<Img> outs;
        double t0 = 1.0;
        for (const Frame& fr : frames) {
            static float v2[1220][3];
            memcpy(v2, verts, sizeof v2);
            if (fr.blink_mm > 0.f) {
                for (int e = 0; e < 2; ++e) {
                    const int* arc = e == 0 ? kArcR : kArcL;
                    for (int k = 0; k < 10; ++k)
                        v2[arc[k]][1] -= fr.blink_mm * 0.001f
                            * sinf((float)M_PI * (float)(k + 1) / 11.f);
                }
            }
            float m2[16];
            memcpy(m2, model, sizeof m2);
            if (fr.head_yaw != 0.f) {
                float rot[16]; ident(rot);
                float cs = cosf(fr.head_yaw), sn = sinf(fr.head_yaw);
                rot[0] = cs; rot[2] = -sn; rot[8] = sn; rot[10] = cs;
                float tmp[16]; mul(model, rot, tmp);
                memcpy(m2, tmp, sizeof m2);
            }
            float el[16], er[16];
            eye_mat(1069, 1080, fr.gaze_yaw, el);   // person's left eye
            eye_mat(1101, 1090, fr.gaze_yaw, er);   // person's right eye

            pms_submit_camera_frame(g_e, pb, 0, t0);
            pms_submit_arkit_face_3d(g_e, &v2[0][0], m2, view, proj,
                                     el, er, blend, 1, W, H);
            int rc = pms_render(g_e, (__bridge void*)g_target, W, H);
            if (rc != 0) fail("render rc");
            pms_render_wait(g_e);
            outs.push_back(read_target());
            write_png(prefix + "_" + fr.tag + ".png", outs.back());
            t0 += 0.033;
        }

        // plain frame (no fx) for diffs
        cmd("set_live_fx", {{"fx", json::array()}});
        pms_submit_camera_frame(g_e, pb, 0, t0);
        pms_render(g_e, (__bridge void*)g_target, W, H);
        pms_render_wait(g_e);
        Img plain = read_target();
        write_png(prefix + "_plain.png", plain);

        // ── assertions ──
        int fails = 0;
        auto expect = [&](bool ok, const char* what) {
            if (!ok) { fprintf(stderr, "  FAIL: %s\n", what); ++fails; }
        };
        auto diff_rows = [&](const Img& a, const Img& b, int y0, int y1) {
            long n = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < W; ++x) {
                    const uint8_t* pa = a.px.data() + ((size_t)y * W + x) * 4;
                    const uint8_t* pc = b.px.data() + ((size_t)y * W + x) * 4;
                    int d = 0;
                    for (int c = 0; c < 3; ++c)
                        d = std::max(d, abs((int)pa[c] - (int)pc[c]));
                    if (d > 12) ++n;
                }
            return n;
        };
        // pigment appears, and only in the face's vertical band
        long mid = diff_rows(outs[0], plain, H / 4, 3 * H / 4);
        long top = diff_rows(outs[0], plain, 0, H / 8);
        expect(mid > 2000, "pigment renders on the face");
        expect(top == 0, "no pigment above the face");
        // blink moves pigment near the eyes but not the mouth
        long eye_band = diff_rows(outs[1], outs[0], (int)(H * 0.32),
                                  (int)(H * 0.47));
        long mouth_band = diff_rows(outs[1], outs[0], (int)(H * 0.60),
                                    (int)(H * 0.75));
        expect(eye_band > 300, "blink changes the eye region");
        expect(mouth_band < 200, "blink leaves the mouth alone");
        // gaze moves iris pixels
        long gaze_eye = diff_rows(outs[2], outs[0], (int)(H * 0.32),
                                  (int)(H * 0.47));
        expect(gaze_eye > 100, "gaze moves the iris");
        // yaw keeps pigment attached (something still renders)
        long yaw_mid = diff_rows(outs[3], plain, H / 4, 3 * H / 4);
        expect(yaw_mid > 2000, "pigment survives head yaw");

        CVPixelBufferRelease(pb);
        pms_destroy(g_e);
        if (fails) { fprintf(stderr, "arkit native replay: FAIL (%d)\n", fails); return 1; }
        printf("arkit native replay: PASS\n");
    }
    return 0;
}
