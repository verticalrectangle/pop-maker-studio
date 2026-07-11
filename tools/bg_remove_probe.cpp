// bg_remove_probe — standalone harness for brick-only background removal.
//
// No app, no GL, no Wayland: decodes the cached bg_masks.mjpeg and the matching
// proxy.mjpeg (both are MJPEG, so stb_image reads them frame-by-frame off SOI
// markers), replays the RemoveBackground shader math on CPU (alpha = orig.a*mask,
// plus the bounding box + edge-softness gamma), and writes composited cutout PNGs
// plus numeric stats. This isolates the exact cutout pipeline the Glass BodyFX
// brick runs, so we can verify it deterministically instead of fighting the live
// preview/export.
//
// It catches: mask-load failure, mask↔frame index desync (the export ghost), the
// box y-orientation, the softness curve, and degenerate (all-white / all-black)
// masks — i.e. every failure mode this feature has hit.
//
//   bg_remove_probe <bg_masks_dir> [proxy.mjpeg] [out_dir] [box=l,r,t,b] [soft=s]
//
// With a proxy: writes <out>/cut_<frame>.png — the real frame with alpha = the
//               cutout, composited over a checkerboard so transparency is visible.
// Without:      writes <out>/mask_<frame>.png — the raw mask — and stats only.
//
// Mirrors body_fx.cpp: get_mask_index (the .idx seek table) + body_fx_mask_texture
// (the per-frame MJPEG decode) + k_frag_RemoveBackground (the shader).

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <sys/stat.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// ── tiny file helpers ─────────────────────────────────────────────────────────
static std::string read_text(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return "";
    char b[64];
    size_t n = fread(b, 1, sizeof(b) - 1, f);
    fclose(f);
    b[n] = 0;
    return b;
}

// Scan an MJPEG for JPEG SOI markers (FF D8 FF) — the same byte pattern the app's
// get_mask_index builder keys on, so proxy frames and a missing .idx both work.
static std::vector<uint64_t> scan_soi(const std::string& path) {
    std::vector<uint64_t> offs;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return offs;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> buf((size_t)sz);
    if (fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return offs; }
    fclose(f);
    for (long i = 0; i + 2 < sz; ++i)
        if (buf[i] == 0xFF && buf[i + 1] == 0xD8 && buf[i + 2] == 0xFF)
            offs.push_back((uint64_t)i);
    return offs;
}

struct Index {
    std::vector<uint64_t> offsets;
    int start_frame = 0;
};

// Mirror of body_fx.cpp:get_mask_index — prefer bg_masks.idx, else scan the MJPEG.
static bool load_index(const std::string& dir, Index& ix) {
    std::string idxp = dir + "/bg_masks.idx";
    FILE* f = fopen(idxp.c_str(), "rb");
    if (f) {
        uint32_t cnt = 0;
        if (fread(&cnt, sizeof(cnt), 1, f) == 1 && cnt > 0) {
            ix.offsets.resize(cnt);
            if (fread(ix.offsets.data(), sizeof(uint64_t), cnt, f) != cnt)
                ix.offsets.clear();
        }
        fclose(f);
    }
    if (ix.offsets.empty())
        ix.offsets = scan_soi(dir + "/bg_masks.mjpeg");
    ix.start_frame = atoi(read_text(dir + "/start_frame.txt").c_str());
    return !ix.offsets.empty();
}

// Decode frame `local` from an MJPEG given its SOI offsets. want_ch: 1=mask, 3=rgb.
static unsigned char* decode_frame(const std::string& mjpeg, const std::vector<uint64_t>& offs,
                                   int local, int want_ch, int& w, int& h) {
    if (local < 0 || local >= (int)offs.size()) return nullptr;
    FILE* f = fopen(mjpeg.c_str(), "rb");
    if (!f) return nullptr;
    long start = (long)offs[(size_t)local];
    long end;
    if (local + 1 < (int)offs.size()) {
        end = (long)offs[(size_t)local + 1];
    } else {
        fseek(f, 0, SEEK_END);
        end = ftell(f);
    }
    size_t sz = (size_t)(end - start);
    std::vector<unsigned char> jb(sz);
    fseek(f, start, SEEK_SET);
    if (fread(jb.data(), 1, sz, f) != sz) { fclose(f); return nullptr; }
    fclose(f);
    int n;
    return stbi_load_from_memory(jb.data(), (int)sz, &w, &h, &n, want_ch);
}

// CPU port of k_frag_RemoveBackground: box clamp (full-frame when off) + softness gamma.
// box = {l, r, t, b} in [0,1] UV; v is GL bottom-up. softness in [-1, 1].
static float mask_math(float m, float u, float v, const float box[4], float softness) {
    if (u < box[0] || u > box[1] || v < box[2] || v > box[3]) return 0.0f;
    if (softness > 0.001f)       m = powf(m, 1.0f + softness * 3.0f);
    else if (softness < -0.001f) m = powf(m, 1.0f / (1.0f - softness * 3.0f));
    return m;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
            "usage: %s <bg_masks_dir> [proxy.mjpeg] [out_dir] [box=l,r,t,b] [soft=s]\n",
            argv[0]);
        return 2;
    }
    std::string dir   = argv[1];
    std::string proxy = (argc > 2 && strncmp(argv[2], "box=", 4) && strncmp(argv[2], "soft=", 5))
                            ? argv[2] : "";
    std::string out   = "/tmp/bg_probe";
    float box[4] = {0, 1, 0, 1};
    float softness = 0.f;
    for (int i = 2; i < argc; ++i) {
        if (!strncmp(argv[i], "box=", 4))
            sscanf(argv[i] + 4, "%f,%f,%f,%f", &box[0], &box[1], &box[2], &box[3]);
        else if (!strncmp(argv[i], "soft=", 5))
            softness = (float)atof(argv[i] + 5);
        else if (argv[i] != proxy && strchr(argv[i], '/'))
            out = argv[i];
    }

    Index ix;
    if (!load_index(dir, ix)) {
        fprintf(stderr, "FAIL: no masks under %s\n", dir.c_str());
        return 1;
    }
    std::string fps = read_text(dir + "/fps.txt");
    while (!fps.empty() && (fps.back() == '\n' || fps.back() == ' ')) fps.pop_back();
    printf("MASKS  frames=%d  start_frame=%d  fps.txt=%s\n",
           (int)ix.offsets.size(), ix.start_frame, fps.c_str());

    std::vector<uint64_t> poffs;
    if (!proxy.empty()) {
        poffs = scan_soi(proxy);
        printf("PROXY  frames=%d  %s\n", (int)poffs.size(), proxy.c_str());
    }
    printf("BOX    l=%.3f r=%.3f t=%.3f b=%.3f   SOFT %.3f\n", box[0], box[1], box[2], box[3], softness);
    mkdir(out.c_str(), 0755);

    int N = (int)ix.offsets.size();
    int samples[] = {0, N / 4, N / 2, (3 * N) / 4, N - 1};
    char path[600];
    double worst_pct = 100.0;

    for (int s : samples) {
        int mw, mh;
        unsigned char* mask = decode_frame(dir + "/bg_masks.mjpeg", ix.offsets, s, 1, mw, mh);
        if (!mask) { printf("  frame %3d: MASK DECODE FAIL\n", s); continue; }

        long kept = 0; double sum = 0;
        for (int i = 0; i < mw * mh; ++i) { if (mask[i] > 127) kept++; sum += mask[i]; }
        double pct = 100.0 * (double)kept / (mw * mh);
        double mean = sum / (mw * mh);
        if (pct < worst_pct) worst_pct = pct;

        if (!proxy.empty() && !poffs.empty()) {
            int pframe = ix.start_frame + s;   // proxy frame = mask local + start_frame
            int pw, ph;
            unsigned char* rgb = decode_frame(proxy, poffs, pframe, 3, pw, ph);
            if (rgb) {
                std::vector<unsigned char> img((size_t)pw * ph * 4);
                for (int y = 0; y < ph; ++y)
                    for (int x = 0; x < pw; ++x) {
                        int mx = (mw == pw) ? x : x * mw / pw;
                        int my = (mh == ph) ? y : y * mh / ph;
                        float m = mask[my * mw + mx] / 255.f;
                        float u = (x + 0.5f) / pw;
                        float v = 1.f - (y + 0.5f) / ph;          // GL bottom-up
                        float a = mask_math(m, u, v, box, softness);
                        // composite subject (alpha a) over a checkerboard so the
                        // cutout is visible at a glance
                        int cb = (((x >> 4) ^ (y >> 4)) & 1) ? 160 : 96;
                        size_t o = (size_t)(y * pw + x) * 4;
                        for (int c = 0; c < 3; ++c)
                            img[o + c] = (unsigned char)(rgb[(y * pw + x) * 3 + c] * a + cb * (1 - a));
                        img[o + 3] = 255;
                    }
                snprintf(path, sizeof(path), "%s/cut_%03d.png", out.c_str(), s);
                stbi_write_png(path, pw, ph, 4, img.data(), pw * 4);
                printf("  frame %3d: mask %dx%d  proxy %dx%d pframe=%d  kept=%.1f%% mean=%.0f -> %s\n",
                       s, mw, mh, pw, ph, pframe, pct, mean, path);
                stbi_image_free(rgb);
            } else {
                printf("  frame %3d: mask %dx%d kept=%.1f%% mean=%.0f  PROXY FRAME %d MISSING (proxy has %d)\n",
                       s, mw, mh, pct, mean, pframe, (int)poffs.size());
            }
        } else {
            snprintf(path, sizeof(path), "%s/mask_%03d.png", out.c_str(), s);
            stbi_write_png(path, mw, mh, 1, mask, mw);
            printf("  frame %3d: mask %dx%d  kept=%.1f%% mean=%.0f -> %s\n",
                   s, mw, mh, pct, mean, path);
        }
        stbi_image_free(mask);
    }

    printf("\nVERDICT: ");
    if (worst_pct < 1.0)        printf("mask ~ALL BLACK — everything cut (degenerate).\n");
    else if (worst_pct > 99.0)  printf("mask ~ALL WHITE — nothing cut (degenerate).\n");
    else                        printf("masks keep a %.1f%%+ subject region — real cutout.\n", worst_pct);
    return 0;
}
