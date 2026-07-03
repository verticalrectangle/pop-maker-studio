#include "video.h"
#include "fx_shader.h"

// ── stb_image — JPEG + PNG decode ────────────────────────────────────────────
#define STB_IMAGE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop

// ── stb_image_write — JPEG encode for datamosh preview ───────────────────────
#include "stb_image_write.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/display.h>
#include <libswscale/swscale.h>
}

#include <turbojpeg.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <deque>

#include "gl_compat.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <array>
#include <unordered_map>
#include <vector>
#include <filesystem>

#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <cerrno>

namespace fs = std::filesystem;


// ── CPU pixel FX helpers ──────────────────────────────────────────────────────

static inline uint32_t uhash(uint32_t x) {
    x ^= x >> 17; x *= 0xbf324c81u;
    x ^= x >> 11; x *= 0x68e31da4u;
    x ^= x >> 14; return x;
}
static inline int   cx(int v, int w) { return v < 0 ? 0 : v >= w ? w-1 : v; }
static inline uint8_t cu8(int v)     { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void cpu_apply_glitch(uint8_t* px, int w, int h,
                             float chroma_px, float jitter, float time) {
    int chroma = (int)(chroma_px + 0.5f);
    uint32_t t1 = (uint32_t)(time * 12.f);
    uint32_t t2 = (uint32_t)(time * 8.f);
    thread_local std::vector<uint8_t> s_row;
    s_row.resize((size_t)w * 3);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = px + (size_t)y * w * 3;
        memcpy(s_row.data(), row, (size_t)w * 3);
        int jshift = 0;
        if (jitter > 0.01f) {
            float rnd = (float)(uhash((uint32_t)y ^ (t1 * 7u)) & 0xFFFF) / 65535.f;
            if (rnd > 1.f - jitter * 0.4f) {
                float rnd2 = (float)(uhash((uint32_t)y ^ (t2 * 3u)) & 0xFFFF) / 65535.f;
                jshift = (int)((rnd2 - 0.5f) * jitter * 0.12f * (float)w);
            }
        }
        for (int x = 0; x < w; ++x) {
            int gx_ = cx(x + jshift, w);
            row[x*3+0] = s_row[cx(gx_ + chroma, w)*3+0];
            row[x*3+1] = s_row[gx_*3+1];
            row[x*3+2] = s_row[cx(gx_ - chroma, w)*3+2];
        }
    }
}

// Output is 4-channel RGBA. Three-pass approach:
//   1. Per-pixel chroma distance → raw alpha (float buffer)
//   2. Separable box blur on alpha → smooth feathered matte
//   3. Composite RGB with blurred alpha + spill suppression across full feather zone
static void cpu_apply_chroma_key(
    const uint8_t* rgb, uint8_t* rgba, int w, int h,
    float kr, float kg, float kb, float threshold, float softness)
{
    float lum_k = 0.299f*kr + 0.587f*kg + 0.114f*kb;
    float ck_r  = kr - lum_k;
    float ck_g  = kg - lum_k;
    float ck_b  = kb - lum_k;
    float soft  = fmaxf(0.001f, softness);
    bool  green_key = (kg > kr && kg > kb);
    bool  blue_key  = (kb > kr && kb > kg);

    int n = w * h;

    // Pass 1: raw alpha per pixel
    thread_local std::vector<float> s_alpha;
    s_alpha.resize(n);
    for (int i = 0; i < n; ++i) {
        float r = rgb[i*3+0] * (1.f/255.f);
        float g = rgb[i*3+1] * (1.f/255.f);
        float b = rgb[i*3+2] * (1.f/255.f);
        float lum  = 0.299f*r + 0.587f*g + 0.114f*b;
        float cp_r = r - lum, cp_g = g - lum, cp_b = b - lum;
        float dist = sqrtf((cp_r-ck_r)*(cp_r-ck_r) +
                           (cp_g-ck_g)*(cp_g-ck_g) +
                           (cp_b-ck_b)*(cp_b-ck_b));
        float t = (dist - threshold) / soft;
        s_alpha[i] = t <= 0.f ? 0.f : t >= 1.f ? 1.f : t*t*(3.f - 2.f*t);
    }

    // Pass 2: separable box blur on alpha — radius scales with softness
    int radius = (int)(softness * w * 0.04f + 1.5f);
    if (radius > 12) radius = 12;
    thread_local std::vector<float> s_tmp;
    s_tmp.resize(n);
    // horizontal
    for (int y = 0; y < h; ++y) {
        float sum = 0.f;
        int   cnt = 0;
        float* row = s_alpha.data() + y * w;
        float* dst = s_tmp.data()   + y * w;
        for (int x = -radius; x < w; ++x) {
            if (x + radius < w) { sum += row[x + radius]; ++cnt; }
            if (x - radius > 0) { sum -= row[x - radius - 1]; --cnt; }
            if (x >= 0) dst[x] = sum / cnt;
        }
    }
    // vertical
    for (int x = 0; x < w; ++x) {
        float sum = 0.f;
        int   cnt = 0;
        for (int y = -radius; y < h; ++y) {
            if (y + radius < h) { sum += s_tmp[(y + radius) * w + x]; ++cnt; }
            if (y - radius > 0) { sum -= s_tmp[(y - radius - 1) * w + x]; --cnt; }
            if (y >= 0) s_alpha[y * w + x] = sum / cnt;
        }
    }

    // Pass 3: write RGBA with blurred alpha + spill suppression across feather zone
    for (int i = 0; i < n; ++i) {
        float r = rgb[i*3+0] * (1.f/255.f);
        float g = rgb[i*3+1] * (1.f/255.f);
        float b = rgb[i*3+2] * (1.f/255.f);
        float alpha = s_alpha[i];

        float out_r = r, out_g = g, out_b = b;
        if (alpha < 1.f) {
            float spill = 1.f - alpha;
            if (green_key) {
                float avg = (out_r + out_b) * 0.5f;
                out_g = out_g + spill * (avg - out_g);
            } else if (blue_key) {
                float avg = (out_r + out_g) * 0.5f;
                out_b = out_b + spill * (avg - out_b);
            }
        }

        rgba[i*4+0] = cu8((int)(out_r * 255.f + 0.5f));
        rgba[i*4+1] = cu8((int)(out_g * 255.f + 0.5f));
        rgba[i*4+2] = cu8((int)(out_b * 255.f + 0.5f));
        rgba[i*4+3] = cu8((int)(alpha  * 255.f + 0.5f));
    }
}

// alpha_out: optional w*h byte array — when provided, corrupted regions are written
// with alpha < 255 so the layer beneath bleeds through in proportion to `bleed`.
// Noise bands punch hardest (full sev), displacement bands half as much, DC blocks fully.
static void cpu_apply_corruption(uint8_t* px, int w, int h, float intensity, float time,
                                  uint8_t* alpha_out = nullptr, float bleed = 0.f) {
    if (intensity < 0.01f) return;
    uint32_t fseed = (uint32_t)(time * 10.f);

    if (alpha_out && bleed > 0.f)
        memset(alpha_out, 0xFF, (size_t)w * h);

    thread_local std::vector<uint8_t> tmp;
    tmp.assign(px, px + (size_t)w * h * 3);

    // ── Pass 1: restart-interval bands ───────────────────────────────────────────
    const int INTERVAL_H = 48;
    int n_iv = (h + INTERVAL_H - 1) / INTERVAL_H;

    for (int iv = 0; iv < n_iv; ++iv) {
        uint32_t ivh = uhash((uint32_t)iv * 7331u ^ fseed * 104729u);
        float rv     = (float)(ivh & 0xFFFF) / 65536.f;
        if (rv >= intensity * 0.9f) continue;

        float sev = 1.f - rv / (intensity * 0.9f);
        uint32_t sh = uhash(ivh ^ 0x13579BDFu);

        int y0 = iv * INTERVAL_H;
        int y1 = y0 + INTERVAL_H; if (y1 > h) y1 = h;

        bool noise_band = sev > 0.55f && (sh & 1u) == 0u;

        if (noise_band) {
            uint32_t ns = uhash(ivh ^ 0xDEADBEEFu);
            // Noise bands lose all structure → maximum bleed-through
            uint8_t band_a = (uint8_t)(255.f * fmaxf(0.f, 1.f - bleed * sev));
            for (int y = y0; y < y1; ++y) {
                uint8_t* row = px + (size_t)y * w * 3;
                for (int x = 0; x < w; ++x) {
                    uint32_t nh = uhash((uint32_t)(x + y * w) ^ ns);
                    uint8_t lum = (uint8_t)(nh & 0xFF);
                    int cr = (int)lum + (int)((int8_t)((nh >> 8)  & 0xFF)) / 4;
                    int cg = (int)lum + (int)((int8_t)((nh >> 16) & 0xFF)) / 4;
                    int cb = (int)lum + (int)((int8_t)((nh >> 24) & 0xFF)) / 4;
                    row[x*3+0] = cu8(cr);
                    row[x*3+1] = cu8(cg);
                    row[x*3+2] = cu8(cb);
                    if (alpha_out && bleed > 0.f)
                        alpha_out[(size_t)y * w + x] = band_a;
                }
            }
        } else {
            // Displacement bands still carry image data → partial bleed
            uint8_t band_a = (uint8_t)(255.f * fmaxf(0.f, 1.f - bleed * sev * 0.5f));
            int src_row = (int)(((float)((sh >> 1) & 0xFFFF) / 65536.f) * (float)h);
            int chroma  = (int)(((float)((sh >> 17) & 0x7FFFu) / 32767.f - 0.5f)
                                * intensity * 60.f);
            for (int y = y0; y < y1; ++y) {
                uint8_t* row = px + (size_t)y * w * 3;
                int sy = ((src_row + (y - y0)) % h + h) % h;
                for (int x = 0; x < w; ++x) {
                    row[x*3+0] = tmp[(size_t)sy * w * 3 + cx(x + chroma,     w) * 3 + 0];
                    row[x*3+1] = tmp[(size_t)sy * w * 3 + cx(x,              w) * 3 + 1];
                    row[x*3+2] = tmp[(size_t)sy * w * 3 + cx(x - chroma / 2, w) * 3 + 2];
                    if (alpha_out && bleed > 0.f)
                        alpha_out[(size_t)y * w + x] = band_a;
                }
            }
        }
    }

    // ── Pass 2: scattered 8×8 DC-only blocks ─────────────────────────────────────
    // DC-only blocks are flat wrong color — best candidates for full bleed-through
    const int BS = 8;
    int nbx = (w + BS - 1) / BS;
    int nby = (h + BS - 1) / BS;
    uint8_t dc_a = (uint8_t)(255.f * fmaxf(0.f, 1.f - bleed));
    for (int by = 0; by < nby; ++by) {
        for (int bx = 0; bx < nbx; ++bx) {
            uint32_t bh = uhash(uhash((uint32_t)bx * 1619u ^ (uint32_t)by * 31337u)
                                ^ (fseed ^ 0x55667788u));
            if ((float)(bh & 0xFFFF) / 65536.f >= intensity * 0.35f) continue;
            int sbx = (int)((float)((bh >> 16) & 0xFFFF) / 65536.f * (float)nbx);
            int sby = (int)(uhash(bh ^ 0xABCDu) % (uint32_t)nby);
            int sx  = cx(sbx * BS, w), sy = cx(sby * BS, h);
            uint8_t qr = tmp[(size_t)sy * w * 3 + sx * 3 + 0] & 0xC0;
            uint8_t qg = tmp[(size_t)sy * w * 3 + sx * 3 + 1] & 0xC0;
            uint8_t qb = tmp[(size_t)sy * w * 3 + sx * 3 + 2] & 0xC0;
            qr |= qr >> 2; qr |= qr >> 4;
            qg |= qg >> 2; qg |= qg >> 4;
            qb |= qb >> 2; qb |= qb >> 4;
            for (int py = 0; py < BS; ++py) {
                int dy = by * BS + py; if (dy >= h) break;
                for (int px_ = 0; px_ < BS; ++px_) {
                    int dx = bx * BS + px_; if (dx >= w) break;
                    px[(size_t)dy * w * 3 + dx * 3 + 0] = qr;
                    px[(size_t)dy * w * 3 + dx * 3 + 1] = qg;
                    px[(size_t)dy * w * 3 + dx * 3 + 2] = qb;
                    if (alpha_out && bleed > 0.f)
                        alpha_out[(size_t)dy * w + dx] = dc_a;
                }
            }
        }
    }
}

// Datamosh on a decoded RGB buffer (w*h*3, in place). Block-displacement +
// chroma bleed — the recognizable "moshy" P-frame-bloom smear. JPEG-bitstream
// corruption (the old approach) was unreliable: the proxy MJPEG's restart
// markers heal mild corruption, and aggressive corruption just makes the
// decoder return null (silent no-op). This is deterministic per frame (seed
// from time → it animates), always visible, and shared by preview + export.
static inline uint32_t dm_hash(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ b * 0x85EBCA77u;
    h ^= h >> 15; h *= 0xC2B2AE3Du; h ^= h >> 13;
    return h;
}
static void datamosh_rgb(uint8_t* rgb, int w, int h, float intensity, float time_sec) {
    if (!rgb || intensity <= 0.f || w <= 0 || h <= 0) return;
    if (intensity > 1.f) intensity = 1.f;
    static thread_local std::vector<uint8_t> src;
    src.assign(rgb, rgb + (size_t)w * h * 3);

    const int   B     = 16;                          // macroblock size (px)
    const float reach = 6.f + intensity * 70.f;      // max smear length
    const int   TAPS  = 5;                            // samples along each trail
    const uint32_t frame = (uint32_t)(time_sec * 24.f);
    auto at = [&](int x, int y, int c) -> uint8_t {
        if (x < 0) x = 0; else if (x >= w) x = w - 1;
        if (y < 0) y = 0; else if (y >= h) y = h - 1;
        return src[((size_t)y * w + x) * 3 + c];
    };
    for (int by = 0; by < h; by += B) {
        for (int bx = 0; bx < w; bx += B) {
            int bxi = bx / B, byi = by / B;
            uint32_t hb = dm_hash((uint32_t)bxi | ((uint32_t)byi << 16), frame);

            // Coherent pseudo-motion flow: a low-frequency field (neighbouring
            // blocks share a direction, so it reads as a moving region rather than
            // noise) plus a per-block jitter. This is what fakes the "P-frame
            // motion applied to the wrong content" smear without a real codec.
            float fa = sinf(bxi * 0.20f + time_sec * 1.3f) + cosf(byi * 0.17f - time_sec * 0.9f);
            float fb = cosf(bxi * 0.15f - time_sec * 1.1f) + sinf(byi * 0.23f + time_sec * 0.7f);
            float vx = fa * 0.6f + (((int)((hb >> 8)  & 0xFF) - 128) / 128.f) * 0.8f;
            float vy = fb * 0.6f + (((int)((hb >> 16) & 0xFF) - 128) / 128.f) * 0.8f;
            float vl = sqrtf(vx*vx + vy*vy) + 1e-4f; vx /= vl; vy /= vl;
            float mag = reach * (0.4f + 0.6f * ((hb & 0xFF) / 255.f));

            // Patchy block selection (scaled by intensity); a rare few "lose" the
            // macroblock entirely → frozen, channel-swapped flat region.
            float pick = (float)((hb >> 24) & 0xFF) / 255.f;
            bool smear = pick < intensity * 0.9f + 0.05f;
            bool lost  = pick > 0.97f - intensity * 0.10f;
            if (!smear && !lost) continue;            // clean blocks pass through

            for (int y = by; y < by + B && y < h; ++y)
                for (int x = bx; x < bx + B && x < w; ++x) {
                    uint8_t* d = rgb + ((size_t)y * w + x) * 3;
                    if (lost) {                        // decoder gave up here
                        int sx = x - (int)(vx * mag), sy = y - (int)(vy * mag);
                        d[0] = at(sx, sy, 1); d[1] = at(sx, sy, 2); d[2] = at(sx, sy, 0);
                        continue;
                    }
                    // Directional smear: average taps back along the flow → a
                    // trail. Chroma lags luma (G half-shift, B static) → the
                    // signature cyan/magenta datamosh bleed along the streak.
                    int rs = 0, gs = 0;
                    for (int k = 0; k < TAPS; ++k) {
                        float f = (float)k / (TAPS - 1);
                        rs += at(x - (int)(vx * mag * f),        y - (int)(vy * mag * f),        0);
                        gs += at(x - (int)(vx * mag * f * 0.5f), y - (int)(vy * mag * f * 0.5f), 1);
                    }
                    d[0] = (uint8_t)(rs / TAPS);
                    d[1] = (uint8_t)(gs / TAPS);
                    d[2] = at(x, y, 2);
                }
        }
    }
}

static void cpu_apply_vhs(uint8_t* px, int w, int h,
                          float noise, float bleed_px, float tracking, float time) {
    int bleed   = (int)(bleed_px + 0.5f);
    int bleed_b = (int)(bleed_px * 0.4f + 0.5f);
    float track_y_f = fmodf(time * 0.17f + 0.3f, 1.f);
    int   track_y   = (int)(track_y_f * (float)h);
    int   track_bw  = (int)(0.04f * (float)h) + 1;
    uint32_t t_nx = (uint32_t)(fmodf(time * 37.3f, 1.f) * 65535.f);
    uint32_t t_ny = (uint32_t)(fmodf(time * 19.7f, 1.f) * 65535.f);
    uint32_t t_tr = (uint32_t)(time * 7.f);
    thread_local std::vector<uint8_t> s_row;
    s_row.resize((size_t)w * 3);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = px + (size_t)y * w * 3;
        memcpy(s_row.data(), row, (size_t)w * 3);
        int tshift = 0;
        if (tracking > 0.01f) {
            int dist = abs(y - track_y);
            if (dist < track_bw) {
                float band = 1.f - (float)dist / (float)track_bw;
                float rnd  = (float)(uhash(t_tr * 7u) & 0xFFFF) / 65535.f - 0.5f;
                tshift = (int)(band * tracking * 0.06f * rnd * (float)w);
            }
        }
        float scan = 1.f - 0.06f * fabsf(sinf((float)y * 3.14159f));
        for (int x = 0; x < w; ++x) {
            int sx = cx(x + tshift, w);
            int r  = s_row[cx(sx + bleed,   w)*3+0];
            int g  = s_row[sx*3+1];
            int b  = s_row[cx(sx - bleed_b, w)*3+2];
            if (noise > 0.01f) {
                uint32_t h_ = uhash((uint32_t)(x + y * w) ^ (t_nx * 0x100u + t_ny));
                int grain = (int)(((float)(h_ & 0xFF) / 255.f - 0.5f) * noise * 63.75f);
                r += grain; g += grain; b += grain;
            }
            row[x*3+0] = cu8((int)((float)r * scan));
            row[x*3+1] = cu8((int)((float)g * scan));
            row[x*3+2] = cu8((int)((float)b * scan));
        }
    }
}

// ── Color grade (single float pass) ──────────────────────────────────────────

static void cpu_apply_grade(uint8_t* px, int w, int h,
                            float brightness, float contrast,
                            float saturation, float hue_deg) {
    bool do_hue = fabsf(hue_deg) > 0.1f;
    float hm[9] = {1,0,0, 0,1,0, 0,0,1};
    if (do_hue) {
        float rad = hue_deg * 3.14159265f / 180.f;
        float c = cosf(rad), s = sinf(rad);
        float sq3 = 0.57735026919f;
        float ic = (1.f - c) / 3.f;
        hm[0] = c+ic;      hm[1] = ic+sq3*s;  hm[2] = ic-sq3*s;
        hm[3] = ic-sq3*s;  hm[4] = c+ic;      hm[5] = ic+sq3*s;
        hm[6] = ic+sq3*s;  hm[7] = ic-sq3*s;  hm[8] = c+ic;
    }
    int n = w * h;
    for (int i = 0; i < n; ++i) {
        float r = px[i*3+0] * (1.f/255.f);
        float g = px[i*3+1] * (1.f/255.f);
        float b = px[i*3+2] * (1.f/255.f);
        // Brightness (additive) → Contrast (around grey) → Saturation → Hue
        r += brightness; g += brightness; b += brightness;
        r = (r - 0.5f) * contrast + 0.5f;
        g = (g - 0.5f) * contrast + 0.5f;
        b = (b - 0.5f) * contrast + 0.5f;
        float lum = r*0.299f + g*0.587f + b*0.114f;
        r = lum + saturation * (r - lum);
        g = lum + saturation * (g - lum);
        b = lum + saturation * (b - lum);
        if (do_hue) {
            float nr = hm[0]*r + hm[1]*g + hm[2]*b;
            float ng = hm[3]*r + hm[4]*g + hm[5]*b;
            float nb = hm[6]*r + hm[7]*g + hm[8]*b;
            r = nr; g = ng; b = nb;
        }
        px[i*3+0] = cu8((int)(r * 255.f + 0.5f));
        px[i*3+1] = cu8((int)(g * 255.f + 0.5f));
        px[i*3+2] = cu8((int)(b * 255.f + 0.5f));
    }
}

// ── Blur — 3-pass box blur approximating Gaussian in O(w·h) ──────────────────

static void box_blur_h(const uint8_t* src, uint8_t* dst, int w, int h, int r) {
    int sz = 2 * r + 1;
    for (int y = 0; y < h; ++y) {
        const uint8_t* sr = src + (size_t)y * w * 3;
        uint8_t*       dr = dst + (size_t)y * w * 3;
        int ar = 0, ag = 0, ab = 0;
        for (int i = -r; i <= r; ++i) {
            int xi = i < 0 ? 0 : i >= w ? w-1 : i;
            ar += sr[xi*3]; ag += sr[xi*3+1]; ab += sr[xi*3+2];
        }
        for (int x = 0; x < w; ++x) {
            dr[x*3] = (uint8_t)(ar / sz); dr[x*3+1] = (uint8_t)(ag / sz); dr[x*3+2] = (uint8_t)(ab / sz);
            int xl = x - r;     if (xl < 0)  xl = 0;
            int xr = x + r + 1; if (xr >= w) xr = w-1;
            ar += sr[xr*3] - sr[xl*3];
            ag += sr[xr*3+1] - sr[xl*3+1];
            ab += sr[xr*3+2] - sr[xl*3+2];
        }
    }
}

static void box_blur_v(const uint8_t* src, uint8_t* dst, int w, int h, int r) {
    int sz = 2 * r + 1;
    for (int x = 0; x < w; ++x) {
        int ar = 0, ag = 0, ab = 0;
        for (int i = -r; i <= r; ++i) {
            int yi = i < 0 ? 0 : i >= h ? h-1 : i;
            ar += src[(size_t)yi*w*3 + x*3];
            ag += src[(size_t)yi*w*3 + x*3+1];
            ab += src[(size_t)yi*w*3 + x*3+2];
        }
        for (int y = 0; y < h; ++y) {
            dst[(size_t)y*w*3 + x*3]   = (uint8_t)(ar / sz);
            dst[(size_t)y*w*3 + x*3+1] = (uint8_t)(ag / sz);
            dst[(size_t)y*w*3 + x*3+2] = (uint8_t)(ab / sz);
            int yt = y - r;     if (yt < 0)  yt = 0;
            int yb = y + r + 1; if (yb >= h) yb = h-1;
            ar += src[(size_t)yb*w*3 + x*3]   - src[(size_t)yt*w*3 + x*3];
            ag += src[(size_t)yb*w*3 + x*3+1] - src[(size_t)yt*w*3 + x*3+1];
            ab += src[(size_t)yb*w*3 + x*3+2] - src[(size_t)yt*w*3 + x*3+2];
        }
    }
}

static void cpu_apply_blur(uint8_t* px, int w, int h, float sigma) {
    // 3 box-blur passes converge to a Gaussian — O(18·w·h) regardless of sigma
    float wf = sqrtf(12.f * sigma * sigma / 3.f + 1.f);
    int wl = (int)wf; if (wl % 2 == 0) wl--;
    int wu = wl + 2;
    int m = (int)roundf((12.f * sigma * sigma
                         - 3.f*(float)(wl*wl) - 12.f*(float)wl - 9.f)
                        / (-4.f*(float)wl - 4.f));
    int sizes[3];
    for (int i = 0; i < 3; ++i) sizes[i] = (i < m) ? wu : wl;

    thread_local std::vector<uint8_t> tmp;
    tmp.resize((size_t)w * h * 3);
    for (int pass = 0; pass < 3; ++pass) {
        int r = sizes[pass] / 2;
        box_blur_h(px, tmp.data(), w, h, r);
        box_blur_v(tmp.data(), px,  w, h, r);
    }
}


// ── Preview state ─────────────────────────────────────────────────────────────

// ── Per-slot decoded-frame ring ──────────────────────────────────────────────
//
// Each slot keeps a small ring of CPU-decoded frames. video_prefetch_frames()
// fills the ring with current + N future frames on worker threads; the main
// thread's video_get_texture() then uploads the matching cached frame with
// zero JPEG decode work on the critical path.
//
// fx_stamp captures the identity of decode-affecting FX params. When FX
// params change, fx_stamp bumps and the ring's entries become stale (they
// stay in memory but get skipped by ring_find()). Pure-time changes do NOT
// bump fx_stamp unless a time-driven FX is on, so playback under static
// FX keeps cache hits.
//
// 8-frame ring (~135 ms at 60 fps) gives the canvas pre-walk room to warm
// both the active clip and a 1-second-boundary neighbor without thrashing
// each other's entries.
static constexpr int RING_FRAMES = 8;

struct DecodedFrame {
    int      frame_idx = -1;       // -1 = empty, -2 = reserved (in-flight)
    uint64_t fx_stamp  = 0;
    int      w = 0, h = 0;
    bool     rgba = false;
    std::vector<uint8_t> jpeg_buf;   // raw JPEG bytes (per-frame so workers don't share)
    std::vector<uint8_t> rgb;        // decoded RGB pixels (always populated)
    std::vector<uint8_t> rgba_buf;   // composited RGBA (populated when rgba=true)
    std::vector<uint8_t> corr_alpha; // corruption-bleed alpha mask scratch
};

struct PreviewState {
    PreviewSource source = PreviewSource::None;

    // ── MJPEG proxy state (source == Proxy) ───────────────────────────────
    FILE*     mjpeg_file     = nullptr;
    ProxyInfo proxy          = {};

    // ── Native libav decode state (source == Native) ──────────────────────
    // Populated by video_open_native(). dec_ctx is HW-attached when
    // hw_dev_ctx is non-null; sws scales decoded frames into (preview_w,
    // preview_h) RGB so they slot straight into DecodedFrame.rgb. The
    // last_decoded_pts lets sequential play / forward scrub skip the
    // av_seek_frame + avcodec_flush_buffers that would otherwise restart
    // the decoder from a keyframe on every frame.
    AVFormatContext* fmt_ctx          = nullptr;
    AVCodecContext*  dec_ctx          = nullptr;
    AVBufferRef*     hw_dev_ctx       = nullptr;
    AVPixelFormat    hw_pix_fmt       = AV_PIX_FMT_NONE;
    SwsContext*      sws              = nullptr;
    int              stream_idx       = -1;
    AVRational       stream_tb        = {0, 1};
    double           last_decoded_pts = -1.0;
    int              preview_w        = 0;
    int              preview_h        = 0;

    // ── Common ────────────────────────────────────────────────────────────
    int       last_frame_idx = -1;
    GLuint    tex      = 0;
    int       tex_w    = 0;
    int       tex_h    = 0;
    bool      tex_rgba = false;
    bool      is_open  = false;
    VideoInfo info = {};
    PixelFX   pixel_fx;
    bool      pixel_fx_dirty = false;
    uint64_t  fx_stamp       = 1;   // bumped on decode-affecting FX change

    // ── Animated GIF (source == Still, gif == true) ───────────────────────
    // Decoded to full-res RGBA frames via stb_image — lossless and alpha-safe,
    // no lossy mp4 conform / MJPEG proxy. The preview uploads the frame at the
    // playhead into `tex`; export keeps decoding the original via libav.
    bool                 gif          = false;
    std::vector<uint8_t> gif_px;            // every frame, contiguous RGBA
    std::vector<float>   gif_end;           // cumulative end time (s) per frame
    float                gif_total    = 0.f;
    int                  gif_w = 0, gif_h = 0, gif_n = 0;
    int                  gif_uploaded = -1; // frame index currently on `tex`
    // BG remove mask MJPEG (streamed, one grayscale-alpha JPEG per frame)
    FILE*                bg_mjpeg_file        = nullptr;
    std::vector<uint64_t> bg_mjpeg_offsets;            // SOI byte offsets
    int                  bg_mjpeg_start_frame = 0;     // proxy frame_idx of mask frame 0
    long                 bg_mjpeg_scanned_sz  = 0;     // how many bytes we've scanned so far
    std::string          bg_mjpeg_dir;                 // last seen mask_dir
    // Decoded alpha for the current frame
    int                  bg_mask_frame   = -1;
    int                  bg_mask_w       = 0;
    int                  bg_mask_h       = 0;
    std::vector<uint8_t> bg_mask_alpha;                // w*h alpha values
    std::vector<uint8_t> mask_jpeg_buf;                // raw JPEG bytes for bg mask (sync path)

    // Serializes access to mjpeg_file / fmt_ctx / dec_ctx / bg_mjpeg_file
    // and the bg_mask_* streaming state. Workers hold this only for the
    // disk read + decode; CPU FX runs unlocked so frames N, N+1, N+2 across
    // different slots decode in parallel.
    std::mutex file_mu;

    DecodedFrame ring[RING_FRAMES];
    int          ring_head = 0;   // next eviction target (round-robin)
};

static PreviewState g_pv[MAX_VIDEO_SLOTS];

// Separate texture for hover-preview thumbnails (track 0's proxy).
static struct ThumbState {
    GLuint tex            = 0;
    int    tex_w          = 0;
    int    tex_h          = 0;
    int    last_frame_idx = -1;
} g_th;

// ── libjpeg-turbo decode helpers ─────────────────────────────────────────────
//
// One tjhandle per thread (decoder state is not thread-safe).
static thread_local tjhandle s_tj_dec = nullptr;
static tjhandle tj_dec() {
    if (!s_tj_dec) s_tj_dec = tjInitDecompress();
    return s_tj_dec;
}

// Decode JPEG buffer into `out` (resized to w*h*channels). Returns true on success.
// channels = 3 → TJPF_RGB, 1 → TJPF_GRAY.
static bool tj_decode(const uint8_t* buf, size_t sz, int channels,
                      std::vector<uint8_t>& out, int& w, int& h) {
    tjhandle tj = tj_dec();
    int subsamp = 0, colorspace = 0;
    if (tjDecompressHeader3(tj, buf, (unsigned long)sz, &w, &h, &subsamp, &colorspace) != 0)
        return false;
    int pf = (channels == 1) ? TJPF_GRAY : TJPF_RGB;
    out.resize((size_t)w * h * channels);
    if (tjDecompress2(tj, buf, (unsigned long)sz, out.data(),
                      w, 0 /*pitch*/, h, pf, TJFLAG_FASTDCT) != 0)
        return false;
    return true;
}

// ── Tiny thread pool for parallel per-slot JPEG decode + cpu_fx ──────────────
//
// Workers stay alive for the process lifetime. Each prefetch round submits
// up to MAX_VIDEO_TRACKS tasks and waits for the batch to complete before
// the main thread does GL uploads.
namespace {
struct ThreadPool {
    std::vector<std::thread>          workers;
    std::deque<std::function<void()>> q;
    std::mutex                        mu;
    std::condition_variable           cv_task;
    std::condition_variable           cv_done;
    std::atomic<int>                  inflight{0};
    bool                              stop = false;

    void start(int n) {
        if (!workers.empty()) return;
        for (int i = 0; i < n; ++i) {
            workers.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lk(mu);
                        cv_task.wait(lk, [this]{ return stop || !q.empty(); });
                        if (stop && q.empty()) return;
                        task = std::move(q.front()); q.pop_front();
                    }
                    task();
                    if (--inflight == 0) {
                        std::lock_guard<std::mutex> lk(mu);
                        cv_done.notify_all();
                    }
                }
            });
        }
    }

    void submit(std::function<void()> f) {
        ++inflight;
        {
            std::lock_guard<std::mutex> lk(mu);
            q.emplace_back(std::move(f));
        }
        cv_task.notify_one();
    }

    void wait_idle() {
        std::unique_lock<std::mutex> lk(mu);
        cv_done.wait(lk, [this]{ return inflight.load() == 0; });
    }
};
}  // namespace
static ThreadPool& pool() {
    static ThreadPool p;
    if (p.workers.empty()) {
        int n = (int)std::thread::hardware_concurrency();
        if (n < 2) n = 2;
        if (n > MAX_VIDEO_TRACKS) n = MAX_VIDEO_TRACKS;
        p.start(n);
    }
    return p;
}

// CPU-only: apply pixel FX to an existing RGB buffer (in-place) and
// (optionally) composite to RGBA in `rgba_out`. No GL. Safe to call on any
// thread provided rgba_out/corr_alpha are caller-owned (per-slot or thread_local).
// Used by both prepare_proxy_frame_cpu (after tj_decode) and prepare_native_frame_cpu
// (after sws_scale).
static void apply_pixel_fx_rgb(uint8_t* pixels, int w, int h,
                               const PixelFX* pfx,
                               const uint8_t* bg_mask, int bg_mask_w, int bg_mask_h,
                               float bg_softness,
                               std::vector<uint8_t>& rgba_out,
                               std::vector<uint8_t>& corr_alpha,
                               bool& want_rgba_out) {
    bool do_corr_bleed = pfx && pfx->glitch_on &&
                         pfx->glitch_corruption >= 0.01f &&
                         pfx->glitch_corruption_bleed > 0.01f;
    bool bg_active = (bg_mask != nullptr && bg_mask_w > 0 && bg_mask_h > 0);
    bool want_rgba = (pfx && pfx->chroma_key_on) || do_corr_bleed || bg_active;

    // Datamosh first, on the raw RGB — applies on BOTH the proxy and native
    // preview sources (this is the only shared post-decode hook).
    if (pfx && pfx->datamosh_on && pfx->datamosh_intensity > 0.01f)
        datamosh_rgb(pixels, w, h, pfx->datamosh_intensity, pfx->time);

    if (pfx) {
        bool need_grade = fabsf(pfx->brightness) > 0.005f || fabsf(pfx->contrast - 1.f) > 0.005f ||
                          fabsf(pfx->saturation - 1.f) > 0.005f || fabsf(pfx->hue_deg) > 0.1f;
        if (need_grade)
            cpu_apply_grade(pixels, w, h, pfx->brightness, pfx->contrast, pfx->saturation, pfx->hue_deg);
        if (pfx->blur_sigma > 0.1f)
            cpu_apply_blur(pixels, w, h, pfx->blur_sigma);
        if (pfx->glitch_on && (pfx->glitch_chroma >= 0.1f || pfx->glitch_jitter >= 0.01f))
            cpu_apply_glitch(pixels, w, h, pfx->glitch_chroma, pfx->glitch_jitter, pfx->time);
        if (pfx->glitch_on && pfx->glitch_corruption >= 0.01f) {
            if (do_corr_bleed) {
                corr_alpha.resize((size_t)w * h);
                cpu_apply_corruption(pixels, w, h, pfx->glitch_corruption, pfx->time,
                                     corr_alpha.data(), pfx->glitch_corruption_bleed);
            } else {
                cpu_apply_corruption(pixels, w, h, pfx->glitch_corruption, pfx->time);
            }
        }
        if (pfx->vhs_on && (pfx->vhs_noise >= 0.01f || pfx->vhs_bleed >= 0.1f || pfx->vhs_tracking >= 0.01f))
            cpu_apply_vhs(pixels, w, h, pfx->vhs_noise, pfx->vhs_bleed, pfx->vhs_tracking, pfx->time);
    }

    if (want_rgba) {
        rgba_out.resize((size_t)w * h * 4);
        int n = w * h;
        bool ck = pfx && pfx->chroma_key_on;
        if (ck) {
            cpu_apply_chroma_key(pixels, rgba_out.data(), w, h,
                                 pfx->chroma_key_r, pfx->chroma_key_g, pfx->chroma_key_b,
                                 pfx->chroma_key_threshold, pfx->chroma_key_softness);
            if (do_corr_bleed) {
                for (int i = 0; i < n; ++i)
                    rgba_out[i*4+3] = (uint8_t)((int)rgba_out[i*4+3] * (int)corr_alpha[i] / 255);
            }
        } else if (bg_active && bg_mask_w == w && bg_mask_h == h) {
            for (int i = 0; i < n; ++i) {
                rgba_out[i*4+0] = pixels[i*3+0];
                rgba_out[i*4+1] = pixels[i*3+1];
                rgba_out[i*4+2] = pixels[i*3+2];
                // Matte control (bg_remove_softness, -1..1): >0 trims the edge
                // tighter, <0 lifts the partial-mask edge pixels back in so the
                // cutout stops eating into the subject. Gamma on the mask alpha.
                float a = bg_mask[i] / 255.f;
                if (bg_softness > 0.001f)       a = powf(a, 1.f + bg_softness * 3.f);
                else if (bg_softness < -0.001f) a = powf(a, 1.f / (1.f - bg_softness * 3.f));
                rgba_out[i*4+3] = (uint8_t)(a * 255.f + 0.5f);
            }
        } else {
            for (int i = 0; i < n; ++i) {
                rgba_out[i*4+0] = pixels[i*3+0];
                rgba_out[i*4+1] = pixels[i*3+1];
                rgba_out[i*4+2] = pixels[i*3+2];
                rgba_out[i*4+3] = corr_alpha[i];
            }
        }
    }

    want_rgba_out = want_rgba;
}

// CPU-only: decode JPEG bytes → RGB into `rgb_out`, then apply pixel FX and
// (optionally) composite to RGBA in `rgba_out`. No GL. Safe to call on any
// thread provided rgb_out/rgba_out/corr_alpha are caller-owned (per-slot or
// thread_local). out_w/out_h are the decoded JPEG dimensions; want_rgba_out is
// whether the consumer should upload rgba_out (true) or rgb_out (false).
static bool process_jpeg_cpu(const uint8_t* buf, size_t sz,
                             const PixelFX* pfx,
                             const uint8_t* bg_mask, int bg_mask_w, int bg_mask_h,
                             float bg_softness,
                             std::vector<uint8_t>& rgb_out,
                             std::vector<uint8_t>& rgba_out,
                             std::vector<uint8_t>& corr_alpha,
                             int& out_w, int& out_h, bool& want_rgba_out) {
    int w = 0, h = 0;
    if (!tj_decode(buf, sz, 3, rgb_out, w, h)) return false;
    apply_pixel_fx_rgb(rgb_out.data(), w, h, pfx, bg_mask, bg_mask_w, bg_mask_h,
                       bg_softness, rgba_out, corr_alpha, want_rgba_out);
    out_w = w; out_h = h;
    return true;
}

// Main-thread GL upload of pre-decoded pixels into the given texture slot.
static void upload_pixels_gl(GLuint* tex, int* tex_w, int* tex_h, bool* tex_rgba,
                             const uint8_t* pixels, int w, int h, bool want_rgba) {
    if (*tex == 0) {
        glGenTextures(1, tex);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, *tex);
    }

    bool format_changed = tex_rgba && (*tex_rgba != want_rgba);
    bool size_changed   = (w != *tex_w || h != *tex_h);
    GLenum fmt = want_rgba ? GL_RGBA : GL_RGB;
    if (size_changed || format_changed) {
        glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, pixels);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, GL_UNSIGNED_BYTE, pixels);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    *tex_w = w; *tex_h = h;
    if (tex_rgba) *tex_rgba = want_rgba;
}

// Upload a JPEG buffer into a GL texture slot, optionally applying CPU pixel FX.
// Single-shot: used by still loaders / thumbnail. Main thread only.
static void upload_jpeg(GLuint* tex, int* tex_w, int* tex_h, bool* tex_rgba,
                        const uint8_t* buf, size_t sz,
                        const PixelFX* pfx = nullptr,
                        const uint8_t* bg_mask = nullptr, int bg_mask_w = 0, int bg_mask_h = 0,
                        float bg_softness = 0.f) {
    thread_local std::vector<uint8_t> tl_rgb, tl_rgba, tl_corr;
    int w = 0, h = 0; bool want_rgba = false;
    if (!process_jpeg_cpu(buf, sz, pfx, bg_mask, bg_mask_w, bg_mask_h, bg_softness,
                          tl_rgb, tl_rgba, tl_corr, w, h, want_rgba))
        return;
    const uint8_t* src = want_rgba ? tl_rgba.data() : tl_rgb.data();
    upload_pixels_gl(tex, tex_w, tex_h, tex_rgba, src, w, h, want_rgba);
}

// ── BG mask MJPEG helpers ─────────────────────────────────────────────────────

// Incrementally scan bg_masks.mjpeg for new SOI markers since last scan.
static void bg_mjpeg_scan(PreviewState& pv) {
    if (!pv.bg_mjpeg_file) return;
    fseeko(pv.bg_mjpeg_file, 0, SEEK_END);
    long file_sz = ftell(pv.bg_mjpeg_file);
    if (file_sz <= pv.bg_mjpeg_scanned_sz) return;

    // Start 2 bytes before last scanned position to catch markers spanning chunks.
    long scan_from = pv.bg_mjpeg_scanned_sz > 2 ? pv.bg_mjpeg_scanned_sz - 2 : 0;
    long to_scan   = file_sz - scan_from;
    fseeko(pv.bg_mjpeg_file, scan_from, SEEK_SET);

    std::vector<uint8_t> buf((size_t)to_scan);
    size_t got = fread(buf.data(), 1, (size_t)to_scan, pv.bg_mjpeg_file);
    for (size_t i = 0; i + 2 < got; ++i) {
        if (buf[i] == 0xFF && buf[i+1] == 0xD8 && buf[i+2] == 0xFF) {
            long abs = scan_from + (long)i;
            // Avoid duplicate if we re-scanned the last 2 bytes of a previous chunk.
            if (pv.bg_mjpeg_offsets.empty() || abs > (long)pv.bg_mjpeg_offsets.back())
                pv.bg_mjpeg_offsets.push_back((uint64_t)abs);
        }
    }
    pv.bg_mjpeg_scanned_sz = file_sz;
}

// Open (or reopen) the mask MJPEG for a given mask_dir; read start_frame.txt.
static void bg_mjpeg_open(PreviewState& pv, const std::string& mdir) {
    if (pv.bg_mjpeg_file) { fclose(pv.bg_mjpeg_file); pv.bg_mjpeg_file = nullptr; }
    pv.bg_mjpeg_offsets.clear();
    pv.bg_mjpeg_scanned_sz  = 0;
    pv.bg_mjpeg_start_frame = 0;
    pv.bg_mjpeg_dir         = mdir;
    pv.bg_mask_frame        = -1;
    pv.bg_mask_alpha.clear();
    pv.bg_mask_w = pv.bg_mask_h = 0;

    FILE* sf = fopen((mdir + "/start_frame.txt").c_str(), "r");
    if (sf) { fscanf(sf, "%d", &pv.bg_mjpeg_start_frame); fclose(sf); }

    pv.bg_mjpeg_file = fopen((mdir + "/bg_masks.mjpeg").c_str(), "rb");
    if (pv.bg_mjpeg_file) bg_mjpeg_scan(pv);
}

// ── Ring helpers ────────────────────────────────────────────────────────────

// Find a ring entry holding (frame_idx, current fx_stamp). Reserved slots
// (frame_idx = -2) are skipped — only fully-decoded entries match.
static DecodedFrame* ring_find(PreviewState& pv, int frame_idx) {
    for (auto& f : pv.ring)
        if (f.frame_idx == frame_idx && f.fx_stamp == pv.fx_stamp)
            return &f;
    return nullptr;
}

// Reserve a ring slot for decode. Round-robin eviction, but skip slots that
// are already reserved (-2) so a prefetch batch of N ≤ RING_FRAMES jobs
// doesn't stomp its own pending decodes.
static DecodedFrame& ring_alloc(PreviewState& pv) {
    for (int tries = 0; tries < RING_FRAMES; ++tries) {
        DecodedFrame& f = pv.ring[pv.ring_head];
        pv.ring_head = (pv.ring_head + 1) % RING_FRAMES;
        if (f.frame_idx != -2) { f.frame_idx = -2; return f; }
    }
    // All slots reserved (shouldn't happen). Stomp head anyway.
    DecodedFrame& f = pv.ring[pv.ring_head];
    pv.ring_head = (pv.ring_head + 1) % RING_FRAMES;
    f.frame_idx = -2;
    return f;
}

static void ring_invalidate(PreviewState& pv) {
    for (auto& f : pv.ring) f.frame_idx = -1;
    pv.ring_head = 0;
}

// True if any FX in `fx` reads pfx.time — i.e. the decoded output changes
// every frame even with identical params. Disables ring lookahead caching.
static bool pfx_is_time_driven(const PixelFX& fx) {
    bool glitch_active = fx.glitch_on &&
        (fx.glitch_chroma >= 0.1f || fx.glitch_jitter >= 0.01f ||
         fx.glitch_corruption >= 0.01f);
    bool vhs_active    = fx.vhs_on &&
        (fx.vhs_noise >= 0.01f || fx.vhs_bleed >= 0.1f || fx.vhs_tracking >= 0.01f);
    bool dmosh_active  = fx.datamosh_on && fx.datamosh_intensity > 0.01f;
    return glitch_active || vhs_active || dmosh_active;
}

// Equal except for `time`. Used to decide whether a pfx change should bump
// fx_stamp (and thereby invalidate the ring): time-only deltas under static
// FX should NOT invalidate.
static bool pfx_eq_modulo_time(const PixelFX& a, const PixelFX& b) {
    PixelFX a2 = a, b2 = b;
    a2.time = 0.f; b2.time = 0.f;
    return a2 == b2;
}

// ── Internal: prepare (CPU-only) one proxy frame into a ring slot ────────────
//
// Decodes pv.mjpeg_file[frame_idx] into `f` and applies CPU FX. The file read
// (and bg_mask state mutation) is serialized under pv.file_mu; the CPU decode
// + FX work is unlocked so the thread pool can run multiple frames of the
// same slot in parallel.
//
// On success, sets f.frame_idx = frame_idx (publishes the entry to readers).
// On failure, sets f.frame_idx = -1 (releases the reservation).
static void prepare_proxy_frame_cpu(PreviewState& pv, DecodedFrame& f, int frame_idx) {
    auto release_empty = [&]{ f.frame_idx = -1; };

    if (!pv.mjpeg_file || pv.proxy.offsets.empty()) { release_empty(); return; }
    if (frame_idx < 0 || (size_t)frame_idx >= pv.proxy.offsets.size())
        { release_empty(); return; }

    // ── Phase 1 (locked): file read + bg_mask state update ──────────────────
    PixelFX pfx;
    std::vector<uint8_t> bg_mask_snapshot;
    int  bg_mask_w_snap = 0, bg_mask_h_snap = 0;
    size_t got = 0;
    uint64_t stamp_at_decode = 0;
    {
        std::lock_guard<std::mutex> lk(pv.file_mu);

        pfx              = pv.pixel_fx;
        stamp_at_decode  = pv.fx_stamp;

        uint64_t offset = pv.proxy.offsets[(size_t)frame_idx];
        bool is_last    = ((size_t)frame_idx + 1 >= pv.proxy.offsets.size());
        size_t frame_sz = 0;
        if (!is_last) {
            frame_sz = (size_t)(pv.proxy.offsets[(size_t)frame_idx + 1] - offset);
        } else {
            fseeko(pv.mjpeg_file, 0, SEEK_END);
            long end = ftell(pv.mjpeg_file);
            frame_sz = (end > (long)offset) ? (size_t)((long)end - (long)offset) : 0;
        }
        if (frame_sz == 0) { release_empty(); return; }

        f.jpeg_buf.resize(frame_sz);
        fseeko(pv.mjpeg_file, (off_t)offset, SEEK_SET);
        got = fread(f.jpeg_buf.data(), 1, frame_sz, pv.mjpeg_file);
        if (got == 0) { release_empty(); return; }

        // DEAD CODE — mechanism A's preview alpha-bake (see the PixelFX bg_remove
        // fields in video.h). Nothing sets pfx.bg_remove_on anymore, so this whole
        // block is unreachable; the cutout is the RemoveBackground body-FX brick.
        // Kept inert rather than unthreaded from this hot decode path.
        if (pfx.bg_remove_on && !pfx.bg_remove_mask_dir.empty()) {
            if (pfx.bg_remove_mask_dir != pv.bg_mjpeg_dir)
                bg_mjpeg_open(pv, pfx.bg_remove_mask_dir);
            else
                bg_mjpeg_scan(pv);

            int mask_idx = frame_idx - pv.bg_mjpeg_start_frame;
            if (frame_idx != pv.bg_mask_frame) {
                pv.bg_mask_frame = frame_idx;
                pv.bg_mask_alpha.clear();
                pv.bg_mask_w = pv.bg_mask_h = 0;
                if (pv.bg_mjpeg_file && mask_idx >= 0 &&
                    mask_idx < (int)pv.bg_mjpeg_offsets.size()) {
                    uint64_t off    = pv.bg_mjpeg_offsets[(size_t)mask_idx];
                    bool     m_last = ((size_t)mask_idx + 1 >= pv.bg_mjpeg_offsets.size());
                    size_t   fsz    = 0;
                    if (!m_last) {
                        fsz = (size_t)(pv.bg_mjpeg_offsets[(size_t)mask_idx + 1] - off);
                    } else {
                        fseeko(pv.bg_mjpeg_file, 0, SEEK_END);
                        long end = ftell(pv.bg_mjpeg_file);
                        fsz = (end > (long)off) ? (size_t)(end - off) : 0;
                    }
                    if (fsz > 0) {
                        pv.mask_jpeg_buf.resize(fsz);
                        fseeko(pv.bg_mjpeg_file, (off_t)off, SEEK_SET);
                        size_t mgot = fread(pv.mask_jpeg_buf.data(), 1, fsz, pv.bg_mjpeg_file);
                        if (mgot > 0) {
                            int mw = 0, mh = 0;
                            if (tj_decode(pv.mask_jpeg_buf.data(), mgot, 1,
                                          pv.bg_mask_alpha, mw, mh)) {
                                pv.bg_mask_w = mw; pv.bg_mask_h = mh;
                                if (pfx.bg_remove_box_on && mw > 0 && mh > 0) {
                                    int xl = (int)(pfx.bg_remove_box_l * mw);
                                    int xr = (int)(pfx.bg_remove_box_r * mw);
                                    int yt = (int)(pfx.bg_remove_box_t * mh);
                                    int yb = (int)(pfx.bg_remove_box_b * mh);
                                    for (int y = 0; y < mh; ++y)
                                        for (int x = 0; x < mw; ++x)
                                            if (x < xl || x >= xr || y < yt || y >= yb)
                                                pv.bg_mask_alpha[(size_t)y * mw + x] = 0;
                                }
                            }
                        }
                    }
                }
            }
            // Snapshot for unlocked decode.
            bg_mask_w_snap = pv.bg_mask_w;
            bg_mask_h_snap = pv.bg_mask_h;
            bg_mask_snapshot = pv.bg_mask_alpha;
        } else if (!pfx.bg_remove_on) {
            if (pv.bg_mjpeg_file) { fclose(pv.bg_mjpeg_file); pv.bg_mjpeg_file = nullptr; }
            pv.bg_mjpeg_offsets.clear();
            pv.bg_mjpeg_dir.clear();
            pv.bg_mask_alpha.clear();
            pv.bg_mask_w = pv.bg_mask_h = 0;
        }
    }

    // Datamosh is applied to the DECODED RGB inside process_jpeg_cpu (the
    // proxy bitstream has restart markers that heal byte corruption).

    // ── Phase 2 (unlocked): JPEG decode + CPU FX ────────────────────────────
    const uint8_t* bg_ptr = (pfx.bg_remove_on && !bg_mask_snapshot.empty())
                            ? bg_mask_snapshot.data() : nullptr;
    int w = 0, h = 0; bool want_rgba = false;
    if (!process_jpeg_cpu(f.jpeg_buf.data(), got, &pfx,
                          bg_ptr, bg_mask_w_snap, bg_mask_h_snap,
                          pfx.bg_remove_softness,
                          f.rgb, f.rgba_buf, f.corr_alpha,
                          w, h, want_rgba))
        { release_empty(); return; }

    f.w         = w;
    f.h         = h;
    f.rgba      = want_rgba;
    f.fx_stamp  = stamp_at_decode;
    f.frame_idx = frame_idx;   // publish
}

// Main-thread GL upload from a ring entry.
static uintptr_t upload_ring_gl(PreviewState& pv, DecodedFrame& f) {
    const uint8_t* src = f.rgba ? f.rgba_buf.data() : f.rgb.data();
    upload_pixels_gl(&pv.tex, &pv.tex_w, &pv.tex_h, &pv.tex_rgba,
                     src, f.w, f.h, f.rgba);
    return pv.tex ? (uintptr_t)pv.tex : 0;
}

// Synchronous single-slot decode + upload (fallback when prefetch missed).
static uintptr_t decode_proxy_frame(PreviewState& pv, int frame_idx) {
    DecodedFrame& f = ring_alloc(pv);
    prepare_proxy_frame_cpu(pv, f, frame_idx);
    if (f.frame_idx != frame_idx) {
        // Decode failed — DO NOT fall back to pv.tex, that gives the caller the
        // previously-decoded frame which is almost certainly the wrong source
        // moment (and the user sees it as "the preview won't update past this
        // old image"). Invalidate the cached upload so the next call can't keep
        // serving the same stale frame either. Caller will see 0 and render
        // blank — much better than rendering content that doesn't match the
        // current clip configuration.
        pv.last_frame_idx = -1;
        static int s_warn_count = 0;
        if (s_warn_count++ < 8)
            fprintf(stderr, "[video] proxy decode failed at frame_idx=%d "
                    "(stale-tex fallback disabled; preview will go blank)\n",
                    frame_idx);
        return 0;
    }
    return upload_ring_gl(pv, f);
}

// ── Native libav decode path ─────────────────────────────────────────────────
//
// Used as the "instant load" tier: a clip is editable / previewable the moment
// it's added, before its MJPEG proxy finishes transcoding. libav's hwaccel auto
// gives us GPU decode on h264 / hevc / av1 when the platform supports it
// (VAAPI / NVDEC / VideoToolbox), with silent software fallback. Frames decode
// → CPU transfer (or just CPU decode in software mode) → sws_scale to the same
// preview resolution the proxy uses → write into the existing DecodedFrame ring
// so the rest of the pipeline (FX, prefetch, GL upload) is unchanged.
//
// Once the proxy is on disk, screen_studio swaps the slot to PreviewSource::Proxy
// because proxy decode is still 2-3× cheaper than native HW decode for
// steady-state scrubbing.

// HW format selection callback. The decoder picks the format that matches the
// HW context we attached; falling back to the first SW format if HW isn't
// usable means we still get a working picture, just slower.
static AVPixelFormat hw_get_format(AVCodecContext* ctx, const AVPixelFormat* fmts) {
    PreviewState* pv = (PreviewState*)ctx->opaque;
    if (pv && pv->hw_pix_fmt != AV_PIX_FMT_NONE) {
        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == pv->hw_pix_fmt) return *p;
    }
    return fmts[0];  // first SW fallback the decoder offered
}

// Walk the candidate HW backends in priority order and return the first one
// that initialises. Stores the matching pixel format on the slot so
// hw_get_format can recognise it later. Caller is responsible for unref'ing
// *dev_out via av_buffer_unref() in close_slot.
static bool try_attach_hw(PreviewState& pv, AVCodecContext* ctx,
                          AVBufferRef** dev_out) {
    static const AVHWDeviceType candidates[] = {
        AV_HWDEVICE_TYPE_VAAPI,         // Linux Intel/AMD/NVIDIA (Mesa)
        AV_HWDEVICE_TYPE_CUDA,          // NVIDIA proprietary
        AV_HWDEVICE_TYPE_VDPAU,         // Older NVIDIA
        AV_HWDEVICE_TYPE_VIDEOTOOLBOX,  // macOS
    };
    for (AVHWDeviceType type : candidates) {
        AVBufferRef* dev = nullptr;
        if (av_hwdevice_ctx_create(&dev, type, nullptr, nullptr, 0) < 0) continue;

        // Find the codec's HW config that matches this device type.
        AVPixelFormat hwfmt = AV_PIX_FMT_NONE;
        for (int i = 0; ; ++i) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(ctx->codec, i);
            if (!cfg) break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                cfg->device_type == type) {
                hwfmt = cfg->pix_fmt;
                break;
            }
        }
        if (hwfmt == AV_PIX_FMT_NONE) { av_buffer_unref(&dev); continue; }

        pv.hw_pix_fmt    = hwfmt;
        ctx->opaque      = &pv;
        ctx->hw_device_ctx = av_buffer_ref(dev);
        ctx->get_format    = hw_get_format;
        *dev_out = dev;
        return true;
    }
    return false;
}

static int max_frame_idx_for(const PreviewState& pv) {
    if (pv.source == PreviewSource::Proxy)
        return (int)pv.proxy.offsets.size();
    if (pv.source == PreviewSource::Native && pv.info.fps > 0.0)
        return (int)(pv.info.duration * pv.info.fps + 0.5);
    return 0;
}

// Decode (or sequentially advance) to the given frame index, transfer to CPU
// if it landed in HW memory, sws_scale into f.rgb at preview resolution, then
// apply CPU pixel FX. Mirrors prepare_proxy_frame_cpu's "phase 1 locked, phase
// 2 unlocked" pattern. last_decoded_pts lets sequential play / forward scrub
// skip the av_seek_frame + avcodec_flush_buffers that would otherwise restart
// the decoder from the prior keyframe on every single frame — for a 5 s GOP
// at 30 fps that's the difference between decoding 1 frame vs. 150.
static void prepare_native_frame_cpu(PreviewState& pv, DecodedFrame& f, int frame_idx) {
    auto release_empty = [&]{ f.frame_idx = -1; };
    if (!pv.fmt_ctx || !pv.dec_ctx) { release_empty(); return; }
    if (pv.info.fps <= 0.0)         { release_empty(); return; }

    double target_t   = (double)frame_idx / pv.info.fps;
    double frame_dur  = 1.0 / pv.info.fps;

    PixelFX pfx;
    uint64_t stamp_at_decode = 0;
    int out_w = 0, out_h = 0;

    // ── Phase 1 (locked): demux + decode + sws_scale ────────────────────────
    {
        std::lock_guard<std::mutex> lk(pv.file_mu);
        pfx             = pv.pixel_fx;
        stamp_at_decode = pv.fx_stamp;

        // Sequential decode: skip seek+flush when target is the natural next
        // frame (or within ~8 frames forward — bigger jumps justify a seek).
        bool need_seek = (pv.last_decoded_pts < 0.0)
                      || (target_t <= pv.last_decoded_pts - frame_dur * 0.5)
                      || (target_t >  pv.last_decoded_pts + frame_dur * 8.0);
        if (need_seek) {
            int64_t ts = (int64_t)(target_t / av_q2d(pv.stream_tb));
            av_seek_frame(pv.fmt_ctx, pv.stream_idx, ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(pv.dec_ctx);
        }

        AVPacket* pkt    = av_packet_alloc();
        AVFrame*  frm    = av_frame_alloc();
        AVFrame*  sw_frm = av_frame_alloc();
        bool got = false;
        double got_pts = -1.0;

        // Decode forward to (or just past) target_t. AVSEEK_FLAG_BACKWARD lands
        // on a keyframe ≤ target, so we may chew through GOP frames first.
        while (!got && av_read_frame(pv.fmt_ctx, pkt) >= 0) {
            if (pkt->stream_index != pv.stream_idx) { av_packet_unref(pkt); continue; }
            avcodec_send_packet(pv.dec_ctx, pkt);
            av_packet_unref(pkt);
            while (!got && avcodec_receive_frame(pv.dec_ctx, frm) == 0) {
                double pts = (frm->pts == AV_NOPTS_VALUE)
                             ? got_pts + frame_dur
                             : frm->pts * av_q2d(pv.stream_tb);
                if (pts < target_t - frame_dur * 0.5) {
                    av_frame_unref(frm);
                    continue;  // before target — keep advancing
                }
                // At/past target — convert this frame and we're done.
                AVFrame* src = frm;
                if (pv.hw_dev_ctx && frm->format == pv.hw_pix_fmt) {
                    if (av_hwframe_transfer_data(sw_frm, frm, 0) >= 0)
                        src = sw_frm;
                    // If transfer fails (broken HW context), fall through and
                    // sws_scale on the HW format will fail cleanly below.
                }

                // (Re)build the scaler if input format or geometry changed.
                int sw = src->width, sh = src->height;
                AVPixelFormat sfmt = (AVPixelFormat)src->format;
                if (!pv.sws || pv.preview_w == 0) {
                    if (pv.sws) { sws_freeContext(pv.sws); pv.sws = nullptr; }
                    pv.preview_w = (sw > 1920) ? sw / 2 : (sw > 960 ? 960 : sw);
                    pv.preview_h = (int)((double)sh * pv.preview_w / (double)sw + 0.5);
                    if (pv.preview_h & 1) pv.preview_h--;  // even rows for swscaler
                    pv.sws = sws_getContext(sw, sh, sfmt,
                                            pv.preview_w, pv.preview_h, AV_PIX_FMT_RGB24,
                                            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                }
                if (pv.sws) {
                    f.rgb.resize((size_t)pv.preview_w * pv.preview_h * 3);
                    uint8_t* dst[1] = { f.rgb.data() };
                    int      lsz[1] = { pv.preview_w * 3 };
                    sws_scale(pv.sws, (const uint8_t* const*)src->data, src->linesize,
                              0, sh, dst, lsz);
                    out_w = pv.preview_w; out_h = pv.preview_h;
                    got_pts = pts;
                    got = true;
                }
                av_frame_unref(frm);
                av_frame_unref(sw_frm);
            }
        }

        av_packet_free(&pkt);
        av_frame_free(&frm);
        av_frame_free(&sw_frm);

        if (!got) { release_empty(); return; }
        pv.last_decoded_pts = got_pts;
    }

    // ── Phase 2 (unlocked): pixel FX + optional RGBA composite ──────────────
    bool want_rgba = false;
    // Native path doesn't currently wire bg_remove masks (those are keyed by
    // proxy frame index). FX without bg_remove still works fine.
    apply_pixel_fx_rgb(f.rgb.data(), out_w, out_h, &pfx,
                       nullptr, 0, 0, 0.f,
                       f.rgba_buf, f.corr_alpha, want_rgba);

    f.w         = out_w;
    f.h         = out_h;
    f.rgba      = want_rgba;
    f.fx_stamp  = stamp_at_decode;
    f.frame_idx = frame_idx;   // publish
}

// Synchronous single-slot native decode + upload (fallback when prefetch missed).
static uintptr_t decode_native_frame(PreviewState& pv, int frame_idx) {
    DecodedFrame& f = ring_alloc(pv);
    prepare_native_frame_cpu(pv, f, frame_idx);
    if (f.frame_idx != frame_idx) {
        // Same fix as decode_proxy_frame — don't serve a stale tex on failure,
        // which masks the actual problem and shows the user content from a
        // different source moment than the current clip points to.
        pv.last_frame_idx = -1;
        static int s_warn_count = 0;
        if (s_warn_count++ < 8)
            fprintf(stderr, "[video] native decode failed at frame_idx=%d "
                    "(stale-tex fallback disabled; preview will go blank)\n",
                    frame_idx);
        return 0;
    }
    return upload_ring_gl(pv, f);
}

// ── Preview API ───────────────────────────────────────────────────────────────

static void close_slot(PreviewState& pv) {
    if (pv.mjpeg_file)    { fclose(pv.mjpeg_file);    pv.mjpeg_file    = nullptr; }
    if (pv.bg_mjpeg_file) { fclose(pv.bg_mjpeg_file); pv.bg_mjpeg_file = nullptr; }
    if (pv.sws)           { sws_freeContext(pv.sws);  pv.sws           = nullptr; }
    if (pv.dec_ctx)       { avcodec_free_context(&pv.dec_ctx); }
    if (pv.hw_dev_ctx)    { av_buffer_unref(&pv.hw_dev_ctx); }
    if (pv.fmt_ctx)       { avformat_close_input(&pv.fmt_ctx); }
    if (pv.tex)           { glDeleteTextures(1, &pv.tex); pv.tex = 0; }
    pv.stream_idx       = -1;
    pv.stream_tb        = {0, 1};
    pv.last_decoded_pts = -1.0;
    pv.preview_w = pv.preview_h = 0;
    pv.hw_pix_fmt = AV_PIX_FMT_NONE;
    pv.tex_w = pv.tex_h = 0;
    pv.last_frame_idx = -1;
    pv.is_open = false;
    pv.source  = PreviewSource::None;
    pv.proxy = {}; pv.info = {};
    pv.bg_mjpeg_offsets.clear();
    pv.bg_mjpeg_dir.clear();
    pv.bg_mask_alpha.clear();
    pv.gif = false; pv.gif_total = 0.f; pv.gif_w = pv.gif_h = pv.gif_n = 0;
    pv.gif_uploaded = -1; pv.gif_px.clear(); pv.gif_end.clear();
    ring_invalidate(pv);
    pv.fx_stamp++;
}

void video_open_still(int track_id, const std::string& jpeg_path) {
    if (track_id < 0 || track_id >= MAX_VIDEO_SLOTS) return;
    close_slot(g_pv[track_id]);

    FILE* f = fopen(jpeg_path.c_str(), "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return; }

    std::vector<uint8_t> buf((size_t)sz);
    fread(buf.data(), 1, (size_t)sz, f);
    fclose(f);

    // Decode with stb_image for everything that isn't a baseline JPEG — PNG
    // (alpha preserved), BMP, static GIF, TGA… all at full quality. A format
    // stb can't read (HEIC/WEBP/TIFF) leaves the slot CLOSED so the caller can
    // fall back to a converted still proxy — we must NOT mark it Still with an
    // empty texture. JPEG keeps its own path below (orientation/colour handling).
    bool is_jpeg = sz >= 3 && buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF;
    if (!is_jpeg) {
        int w = 0, h = 0, ch = 0;
        uint8_t* px = stbi_load_from_memory(buf.data(), (int)sz, &w, &h, &ch, 4);
        if (!px) return;
        upload_pixels_gl(&g_pv[track_id].tex, &g_pv[track_id].tex_w, &g_pv[track_id].tex_h,
                        &g_pv[track_id].tex_rgba, px, w, h, true);
        stbi_image_free(px);
        g_pv[track_id].is_open     = true;
        g_pv[track_id].source      = PreviewSource::Still;
        g_pv[track_id].info.width  = g_pv[track_id].tex_w;
        g_pv[track_id].info.height = g_pv[track_id].tex_h;
        return;
    }

    upload_jpeg(&g_pv[track_id].tex, &g_pv[track_id].tex_w, &g_pv[track_id].tex_h,
                &g_pv[track_id].tex_rgba, buf.data(), (size_t)sz);
    g_pv[track_id].is_open       = true;
    g_pv[track_id].source        = PreviewSource::Still;
    // Expose dimensions via video_info() so canvas aspect-ratio fit works for stills.
    g_pv[track_id].info.width    = g_pv[track_id].tex_w;
    g_pv[track_id].info.height   = g_pv[track_id].tex_h;
}

// Open the source file with libav for direct decode — used as the "instant
// load" path before the MJPEG proxy finishes transcoding. Tries HW decode
// (VAAPI / NVDEC / VideoToolbox) first, falls back to software decode if no
// usable HW path is available. Returns false only when the file can't be
// opened at all (corrupt container, missing codec). Software decode of typical
// h264 1080p preview frames is ~15-30 ms per frame; HW is ~5-15 ms.
bool video_open_native(int track_id, const std::string& path) {
    if (track_id < 0 || track_id >= MAX_VIDEO_SLOTS) return false;
    if (path.empty()) return false;
    close_slot(g_pv[track_id]);
    PreviewState& pv = g_pv[track_id];

    if (avformat_open_input(&pv.fmt_ctx, path.c_str(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(pv.fmt_ctx, nullptr) < 0) {
        avformat_close_input(&pv.fmt_ctx); return false;
    }
    for (unsigned i = 0; i < pv.fmt_ctx->nb_streams; ++i) {
        if (pv.fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            pv.stream_idx = (int)i; break;
        }
    }
    if (pv.stream_idx < 0) { avformat_close_input(&pv.fmt_ctx); return false; }

    AVStream* st = pv.fmt_ctx->streams[pv.stream_idx];
    pv.stream_tb = st->time_base;
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) { avformat_close_input(&pv.fmt_ctx); pv.stream_idx = -1; return false; }
    pv.dec_ctx = avcodec_alloc_context3(codec);
    if (!pv.dec_ctx) { avformat_close_input(&pv.fmt_ctx); pv.stream_idx = -1; return false; }
    if (avcodec_parameters_to_context(pv.dec_ctx, st->codecpar) < 0) {
        avcodec_free_context(&pv.dec_ctx);
        avformat_close_input(&pv.fmt_ctx);
        pv.stream_idx = -1;
        return false;
    }

    // Try HW first; ignore failure — we'll just run in software mode.
    try_attach_hw(pv, pv.dec_ctx, &pv.hw_dev_ctx);

    // Mirror the proxy worker's threading budget: a couple of threads per slot
    // so multiple parallel slots don't all fight for the full core count.
    pv.dec_ctx->thread_count = 2;

    if (avcodec_open2(pv.dec_ctx, codec, nullptr) < 0) {
        avcodec_free_context(&pv.dec_ctx);
        if (pv.hw_dev_ctx) av_buffer_unref(&pv.hw_dev_ctx);
        avformat_close_input(&pv.fmt_ctx);
        pv.stream_idx = -1;
        return false;
    }

    pv.info.width    = pv.dec_ctx->width;
    pv.info.height   = pv.dec_ctx->height;
    pv.info.duration = (pv.fmt_ctx->duration != AV_NOPTS_VALUE && pv.fmt_ctx->duration > 0)
                       ? (double)pv.fmt_ctx->duration / AV_TIME_BASE : 0.0;
    pv.info.fps      = (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
                       ? av_q2d(st->avg_frame_rate) : 30.0;
    pv.source        = PreviewSource::Native;
    pv.is_open       = true;
    pv.last_frame_idx = -1;
    pv.last_decoded_pts = -1.0;

    // Decode frame 0 so the canvas has something to show immediately.
    (void)video_get_texture(track_id, 0.0);
    return true;
}

PreviewSource video_source(int track_id) {
    if (track_id < 0 || track_id >= MAX_VIDEO_SLOTS) return PreviewSource::None;
    return g_pv[track_id].source;
}

bool video_open_proxy(int track_id, const ProxyInfo& proxy) {
    if (track_id < 0 || track_id >= MAX_VIDEO_SLOTS) return false;
    close_slot(g_pv[track_id]);

    FILE* f = fopen(proxy.mjpeg_path.c_str(), "rb");
    if (!f) return false;

    PreviewState& pv = g_pv[track_id];
    pv.mjpeg_file      = f;
    pv.proxy           = proxy;
    pv.source          = PreviewSource::Proxy;
    pv.is_open         = true;
    pv.last_frame_idx  = -1;
    pv.info.width      = proxy.width;
    pv.info.height     = proxy.height;
    pv.info.fps        = proxy.fps;
    pv.info.duration   = proxy.fps > 0.0
                         ? (double)proxy.frame_count / proxy.fps : 0.0;

    // Show frame 0 immediately.
    uintptr_t tex = video_get_texture(track_id, 0.0);
    (void)tex;
    return true;
}

void video_close(int track_id) {
    if (track_id == -1) {
        for (int i = 0; i < MAX_VIDEO_SLOTS; ++i) close_slot(g_pv[i]);
        if (g_th.tex) { glDeleteTextures(1, &g_th.tex); g_th.tex = 0; }
        g_th.tex_w = g_th.tex_h = 0;
        g_th.last_frame_idx = -1;
    } else if (track_id >= 0 && track_id < MAX_VIDEO_SLOTS) {
        close_slot(g_pv[track_id]);
    }
}

bool      video_is_open(int track_id) {
    if (track_id < 0 || track_id >= MAX_VIDEO_SLOTS) return false;
    return g_pv[track_id].is_open;
}
VideoInfo video_info(int track_id) {
    if (track_id < 0 || track_id >= MAX_VIDEO_SLOTS) return {};
    return g_pv[track_id].info;
}

void video_set_pixel_fx(int track_id, const PixelFX& fx) {
    if (track_id < 0 || track_id >= MAX_VIDEO_SLOTS) return;
    auto& pv = g_pv[track_id];
    if (pv.pixel_fx == fx) return;

    // Bump fx_stamp (= invalidate ring cache) only when the change actually
    // affects decoded pixels. Pure pfx.time advances under static FX leave
    // the cache valid; time changes under time-driven FX invalidate.
    bool non_time_changed = !pfx_eq_modulo_time(pv.pixel_fx, fx);
    bool tdriven_now      = pfx_is_time_driven(fx);
    bool tdriven_prev     = pfx_is_time_driven(pv.pixel_fx);

    pv.pixel_fx       = fx;
    pv.pixel_fx_dirty = true;
    if (non_time_changed || tdriven_now || tdriven_prev) pv.fx_stamp++;
}

// Compute the frame index for a given playhead time on the given slot. Works
// for both Proxy (using the offsets table for clamp) and Native (using info.fps).
// Returns -1 if the slot is not open or not a frame-indexable source.
static int playhead_to_frame_idx(const PreviewState& pv, double playhead) {
    if (!pv.is_open) return -1;
    if (playhead < 0.0) playhead = 0.0;
    double dur = pv.info.duration;
    if (dur > 0.0 && playhead > dur) playhead = dur;

    if (pv.source == PreviewSource::Proxy) {
        if (!pv.mjpeg_file || pv.proxy.offsets.empty()) return -1;
        int64_t num = pv.proxy.fps_num;
        int64_t den = pv.proxy.fps_den;
        int frame_idx = (num > 0 && den > 0)
            ? (int)((int64_t)(playhead * (double)num) / den)
            : (int)(playhead * pv.proxy.fps);
        if (frame_idx >= (int)pv.proxy.offsets.size())
            frame_idx = (int)pv.proxy.offsets.size() - 1;
        if (frame_idx < 0) frame_idx = 0;
        return frame_idx;
    }

    if (pv.source == PreviewSource::Native) {
        if (pv.info.fps <= 0.0 || !pv.fmt_ctx || !pv.dec_ctx) return -1;
        int frame_idx = (int)(playhead * pv.info.fps);
        if (frame_idx < 0) frame_idx = 0;
        return frame_idx;
    }

    return -1;
}

// Public: the proxy frame index a playhead maps to on an open slot — the SAME
// mapping video_get_texture() uses, but with NO ffprobe fork (it reads the cached
// proxy rate), so it is safe to call from the render path. The bg-removal brick
// indexes its per-frame masks with this. Returns -1 if the slot isn't open.
int video_proxy_frame_idx(int track_id, double playhead) {
    if (track_id < 0 || track_id >= MAX_VIDEO_SLOTS) return -1;
    PreviewState& pv = g_pv[track_id];
    if (!pv.is_open) return -1;
    return playhead_to_frame_idx(pv, playhead);
}

// Forward decls — implementations live further down in the Native section.
static void      prepare_native_frame_cpu(PreviewState& pv, DecodedFrame& f, int frame_idx);
static uintptr_t decode_native_frame      (PreviewState& pv, int frame_idx);
static int       max_frame_idx_for        (const PreviewState& pv);

bool video_is_gif(int track_id) {
    if (track_id < 0 || track_id >= MAX_VIDEO_SLOTS) return false;
    return g_pv[track_id].is_open && g_pv[track_id].gif;
}

// Decode an animated GIF to full-res RGBA frames (lossless, alpha-safe) via
// stb_image and hold them in the slot. The preview uploads the frame at the
// playhead — no lossy mp4 conform / MJPEG proxy. Export keeps using libav.
bool video_open_gif(int track_id, const std::string& path) {
    if (track_id < 0 || track_id >= MAX_VIDEO_SLOTS) return false;
    PreviewState& pv = g_pv[track_id];
    close_slot(pv);

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    if (sz <= 0) { fclose(f); return false; }
    std::vector<uint8_t> buf((size_t)sz);
    size_t got = fread(buf.data(), 1, (size_t)sz, f); fclose(f);
    if (got != (size_t)sz) return false;

    int* delays = nullptr; int w = 0, h = 0, n = 0, comp = 0;
    uint8_t* px = stbi_load_gif_from_memory(buf.data(), (int)sz, &delays, &w, &h, &n, &comp, 4);
    if (!px || n < 1 || w <= 0 || h <= 0) { if (px) stbi_image_free(px); free(delays); return false; }

    size_t fsz = (size_t)w * h * 4;
    pv.gif_px.assign(px, px + fsz * (size_t)n);
    pv.gif_end.resize((size_t)n);
    float t = 0.f;
    for (int i = 0; i < n; ++i) {
        float d = (delays && delays[i] > 0) ? (float)delays[i] / 1000.f : 0.1f;  // GIF delay → seconds
        t += d; pv.gif_end[(size_t)i] = t;
    }
    free(delays);
    stbi_image_free(px);

    pv.gif       = true;
    pv.gif_w = w; pv.gif_h = h; pv.gif_n = n;
    pv.gif_total = (t > 0.f) ? t : 0.1f;
    pv.gif_uploaded = 0;
    upload_pixels_gl(&pv.tex, &pv.tex_w, &pv.tex_h, &pv.tex_rgba, pv.gif_px.data(), w, h, true);
    pv.is_open    = true;
    pv.source     = PreviewSource::Still;   // rendered like a still; the gif flag picks the frame
    pv.info.width = w; pv.info.height = h;
    return true;
}

uintptr_t video_get_texture(int track_id, double playhead) {
    if (track_id < 0 || track_id >= MAX_VIDEO_SLOTS) return 0;
    PreviewState& pv = g_pv[track_id];
    if (!pv.is_open) return 0;

    // Animated GIF: upload the frame at this playhead (full-res RGBA, looped).
    if (pv.gif && pv.gif_n > 0) {
        float tt = pv.gif_total > 0.f ? (float)fmod(playhead, (double)pv.gif_total) : 0.f;
        if (tt < 0.f) tt += pv.gif_total;
        int idx = 0;
        while (idx < pv.gif_n - 1 && tt >= pv.gif_end[(size_t)idx]) ++idx;
        if (idx != pv.gif_uploaded && pv.gif_w > 0) {
            size_t fsz = (size_t)pv.gif_w * pv.gif_h * 4;
            upload_pixels_gl(&pv.tex, &pv.tex_w, &pv.tex_h, &pv.tex_rgba,
                             pv.gif_px.data() + (size_t)idx * fsz, pv.gif_w, pv.gif_h, true);
            pv.gif_uploaded = idx;
        }
        return pv.tex ? (uintptr_t)pv.tex : 0;
    }

    if (pv.source == PreviewSource::Still || pv.source == PreviewSource::None)
        return pv.tex ? (uintptr_t)pv.tex : 0;

    int frame_idx = playhead_to_frame_idx(pv, playhead);
    if (frame_idx < 0) return 0;

    // Same frame, same FX, already on the GPU → no work.
    if (frame_idx == pv.last_frame_idx && pv.tex && !pv.pixel_fx_dirty)
        return (uintptr_t)pv.tex;

    // Ring cache hit (prefetched by an earlier video_prefetch_frames call).
    if (DecodedFrame* f = ring_find(pv, frame_idx)) {
        pv.pixel_fx_dirty = false;
        pv.last_frame_idx = frame_idx;
        return upload_ring_gl(pv, *f);
    }

    pv.pixel_fx_dirty = false;
    pv.last_frame_idx = frame_idx;
    return (pv.source == PreviewSource::Proxy)
        ? decode_proxy_frame (pv, frame_idx)
        : decode_native_frame(pv, frame_idx);
}

void video_prefetch_frames(const VideoPrefetchReq* reqs, int n) {
    if (!reqs || n <= 0) return;

    // Per-slot prefetch window: 1 frame for time-driven FX (every frame is
    // unique, no point caching), RING_FRAMES otherwise. The current frame
    // counts as one slot of the window.
    struct Job { PreviewState* pv; int frame_idx; DecodedFrame* target; };
    std::vector<Job> jobs;
    jobs.reserve((size_t)n * RING_FRAMES);

    for (int i = 0; i < n; ++i) {
        int t = reqs[i].track_id;
        if (t < 0 || t >= MAX_VIDEO_SLOTS) continue;
        PreviewState& pv = g_pv[t];
        if (!pv.is_open) continue;
        if (pv.source != PreviewSource::Proxy && pv.source != PreviewSource::Native) continue;

        int base_fidx = playhead_to_frame_idx(pv, reqs[i].playhead);
        if (base_fidx < 0) continue;

        int window = pfx_is_time_driven(pv.pixel_fx) ? 1 : RING_FRAMES;
        if (reqs[i].max_frames > 0 && reqs[i].max_frames < window)
            window = reqs[i].max_frames;
        int max_fidx = max_frame_idx_for(pv);
        for (int k = 0; k < window; ++k) {
            int fidx = base_fidx + k;
            if (max_fidx > 0 && fidx >= max_fidx) break;
            if (ring_find(pv, fidx)) continue;  // already cached
            jobs.push_back({&pv, fidx, nullptr});
        }
    }
    if (jobs.empty()) return;

    // Reserve ring slots on the main thread (ring_alloc is not thread-safe).
    for (auto& j : jobs) j.target = &ring_alloc(*j.pv);

    auto run_one = [](PreviewState* pv, DecodedFrame* tgt, int fidx) {
        if (pv->source == PreviewSource::Native)
            prepare_native_frame_cpu(*pv, *tgt, fidx);
        else
            prepare_proxy_frame_cpu(*pv, *tgt, fidx);
    };

    // 1 job: skip the pool entirely (no thread hand-off cost).
    if (jobs.size() == 1) {
        run_one(jobs[0].pv, jobs[0].target, jobs[0].frame_idx);
        return;
    }

    ThreadPool& tp = pool();
    for (auto& j : jobs) {
        PreviewState* pv = j.pv;
        DecodedFrame* tgt = j.target;
        int fidx = j.frame_idx;
        tp.submit([pv, tgt, fidx, run_one]{ run_one(pv, tgt, fidx); });
    }
    tp.wait_idle();
}

uintptr_t video_get_thumbnail(double t, int* out_w, int* out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    // Uses track 0's proxy for scrub bar hover preview. Native sources don't
    // serve thumbnails — they'd require a sync libav decode per hover frame
    // which is way too expensive; the proxy upgrade in screen_studio's per-frame
    // loop swaps in fast scrubbable thumbnails as soon as transcode finishes.
    PreviewState& pv = g_pv[0];
    if (!pv.is_open || pv.source != PreviewSource::Proxy) return 0;
    if (!pv.mjpeg_file || pv.proxy.offsets.empty()) return 0;

    double dur = pv.info.duration;
    if (t < 0.0) t = 0.0;
    if (dur > 0.0 && t > dur) t = dur;

    int64_t num = pv.proxy.fps_num;
    int64_t den = pv.proxy.fps_den;
    int frame_idx = (num > 0 && den > 0)
        ? (int)((int64_t)(t * (double)num) / den)
        : (int)(t * pv.proxy.fps);
    if (frame_idx >= (int)pv.proxy.offsets.size())
        frame_idx = (int)pv.proxy.offsets.size() - 1;
    if (frame_idx < 0) frame_idx = 0;

    if (frame_idx == g_th.last_frame_idx && g_th.tex) {
        if (out_w) *out_w = g_th.tex_w;
        if (out_h) *out_h = g_th.tex_h;
        return (uintptr_t)g_th.tex;
    }
    g_th.last_frame_idx = frame_idx;

    uint64_t offset = pv.proxy.offsets[(size_t)frame_idx];
    bool is_last = ((size_t)frame_idx + 1 >= pv.proxy.offsets.size());
    size_t frame_sz = 0;
    if (!is_last) {
        frame_sz = (size_t)(pv.proxy.offsets[(size_t)frame_idx + 1] - offset);
    } else {
        fseeko(pv.mjpeg_file, (off_t)offset, SEEK_SET);
        long cur = ftell(pv.mjpeg_file);
        fseeko(pv.mjpeg_file, 0, SEEK_END);
        long end = ftell(pv.mjpeg_file);
        frame_sz = (end > cur) ? (size_t)(end - cur) : 0;
    }
    if (frame_sz == 0) return g_th.tex ? (uintptr_t)g_th.tex : 0;

    static std::vector<uint8_t> s_th_buf;
    s_th_buf.resize(frame_sz);
    fseeko(pv.mjpeg_file, (off_t)offset, SEEK_SET);
    size_t got = fread(s_th_buf.data(), 1, frame_sz, pv.mjpeg_file);
    if (got == 0) return g_th.tex ? (uintptr_t)g_th.tex : 0;

    upload_jpeg(&g_th.tex, &g_th.tex_w, &g_th.tex_h, nullptr, s_th_buf.data(), got);

    if (out_w) *out_w = g_th.tex_w;
    if (out_h) *out_h = g_th.tex_h;
    return (uintptr_t)g_th.tex;
}

float video_probe_duration(const std::string& path) {
    AVFormatContext* fc = nullptr;
    const std::string& url0 = path;
    if (avformat_open_input(&fc, url0.c_str(), nullptr, nullptr) != 0) return 0.f;
    // find_stream_info is required for files where the container header doesn't
    // carry a reliable duration (e.g. some MP4/MKV variants).
    avformat_find_stream_info(fc, nullptr);
    float dur = (fc->duration != AV_NOPTS_VALUE)
        ? (float)fc->duration / (float)AV_TIME_BASE
        : 0.f;
    avformat_close_input(&fc);
    return dur;
}

MediaFileInfo video_probe_file(const std::string& path) {
    MediaFileInfo info;
    AVFormatContext* fc = nullptr;
    const std::string& url1 = path;
    if (avformat_open_input(&fc, url1.c_str(), nullptr, nullptr) != 0) {
        info.error = "cannot open file";
        return info;
    }
    if (avformat_find_stream_info(fc, nullptr) < 0) {
        avformat_close_input(&fc);
        info.error = "cannot read stream info";
        return info;
    }
    if (fc->duration != AV_NOPTS_VALUE)
        info.duration = (double)fc->duration / AV_TIME_BASE;

    for (unsigned i = 0; i < fc->nb_streams; ++i) {
        AVStream* st = fc->streams[i];
        AVCodecParameters* cp = st->codecpar;
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO && !info.has_video) {
            info.has_video   = true;
            info.width       = cp->width;
            info.height      = cp->height;
            if (st->avg_frame_rate.den > 0)
                info.fps = av_q2d(st->avg_frame_rate);
            const AVCodecDescriptor* desc = avcodec_descriptor_get(cp->codec_id);
            if (desc) info.video_codec = desc->name;
        } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO && !info.has_audio) {
            info.has_audio   = true;
            info.sample_rate = cp->sample_rate;
            info.channels    = cp->ch_layout.nb_channels;
            const AVCodecDescriptor* desc = avcodec_descriptor_get(cp->codec_id);
            if (desc) info.audio_codec = desc->name;
        }
    }
    avformat_close_input(&fc);
    return info;
}

// Run ffmpeg to completion (blocking). Returns the exit code, or -1 on spawn
// failure. argv must be NULL-terminated; argv[0] = "ffmpeg".
static int run_ffmpeg_blocking(const char** argv) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        (void)nice(10);
        execvp("ffmpeg", const_cast<char**>(argv));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

std::string video_extract_segment(const std::string& src,
                                  double start_sec, double end_sec,
                                  const std::string& dst,
                                  bool audio_only) {
    // ── Video segments: re-encode, don't stream-copy ─────────────────────────
    // A stream copy of a time slice that starts mid-GOP has no leading keyframe,
    // so the segment is undecodable (the referenced IDR lives outside the slice).
    // Re-encoding with an input seek decodes from the preceding keyframe and
    // emits a fresh IDR exactly at start_sec, so the segment always plays. H.264
    // can't live in a .webm/.ogg container, so we always mux to Matroska
    // (holds H.264 + copied opus/aac/vorbis audio); callers name the file .mkv.
    if (!audio_only && video_probe_file(src).has_video) {
        std::string srcarg = "file:" + src;
        char ss[32], tt[32];
        snprintf(ss, sizeof(ss), "%.3f", start_sec < 0.0 ? 0.0 : start_sec);
        double dur = end_sec - start_sec;
        std::vector<const char*> args = {
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
            "-ss", ss,
            "-i", srcarg.c_str(),
        };
        // end_sec is sometimes a sentinel (1e9) meaning "to EOF" — only bound
        // the duration when it's a real, finite span.
        if (dur > 0.0 && dur < 1e6) {
            snprintf(tt, sizeof(tt), "%.3f", dur);
            args.push_back("-t"); args.push_back(tt);
        }
        args.push_back("-c:v");     args.push_back("libx264");
        args.push_back("-crf");     args.push_back("18");
        args.push_back("-preset");  args.push_back("veryfast");
        args.push_back("-pix_fmt"); args.push_back("yuv420p");
        // Re-encode (not copy) the audio: with a fast input -ss seek, copying audio
        // keeps the source timestamps and anchors the muxer so the video starts at
        // pts=start_sec instead of 0. Re-encoding resets both streams to zero, so
        // the segment begins at pts 0 (in_point=0 = first frame, as callers assume).
        // AAC also muxes cleanly into the downstream conform .mp4 (opus does not).
        args.push_back("-c:a");     args.push_back("aac");
        args.push_back("-avoid_negative_ts"); args.push_back("make_zero");
        args.push_back("-f");       args.push_back("matroska");
        args.push_back(dst.c_str());
        args.push_back(nullptr);

        int rc = run_ffmpeg_blocking(args.data());
        if (rc != 0) {
            fs::remove(fs::path(dst));   // don't leave a half-written segment cached
            return "ffmpeg segment re-encode failed (exit " + std::to_string(rc) + ")";
        }
        return "";
    }

    AVFormatContext* in_ctx = nullptr;
    const std::string& url2 = src;
    if (avformat_open_input(&in_ctx, url2.c_str(), nullptr, nullptr) < 0)
        return "cannot open source file";
    if (avformat_find_stream_info(in_ctx, nullptr) < 0) {
        avformat_close_input(&in_ctx);
        return "cannot read source stream info";
    }

    // For audio-only sources (e.g. FLAC), the dst extension (.webm) may be
    // incompatible with the source codec. Pass the source format name explicitly
    // so ffmpeg picks a container that can hold the codec rather than guessing
    // from the dst extension.
    bool has_video_stream = false;
    for (unsigned i = 0; i < in_ctx->nb_streams; ++i) {
        if (in_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            has_video_stream = true;
            break;
        }
    }
    const char* force_fmt = nullptr;
    if (!has_video_stream && in_ctx->iformat)
        force_fmt = in_ctx->iformat->name;

    AVFormatContext* out_ctx = nullptr;
    if (avformat_alloc_output_context2(&out_ctx, nullptr, force_fmt, dst.c_str()) < 0) {
        avformat_close_input(&in_ctx);
        return "cannot create output context for: " + dst;
    }

    // Map streams: copy each stream header into the output
    std::vector<int> stream_map(in_ctx->nb_streams, -1);
    int out_stream_idx = 0;
    for (unsigned i = 0; i < in_ctx->nb_streams; ++i) {
        AVStream* in_st = in_ctx->streams[i];
        AVCodecParameters* cp = in_st->codecpar;
        if (cp->codec_type != AVMEDIA_TYPE_VIDEO &&
            cp->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
        if (audio_only && cp->codec_type == AVMEDIA_TYPE_VIDEO)
            continue;   // rip audio: drop the picture entirely
        AVStream* out_st = avformat_new_stream(out_ctx, nullptr);
        if (!out_st) {
            avformat_close_input(&in_ctx);
            avformat_free_context(out_ctx);
            return "cannot allocate output stream";
        }
        if (avcodec_parameters_copy(out_st->codecpar, cp) < 0) {
            avformat_close_input(&in_ctx);
            avformat_free_context(out_ctx);
            return "cannot copy codec parameters";
        }
        out_st->codecpar->codec_tag = 0;
        stream_map[i] = out_stream_idx++;
    }

    if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&out_ctx->pb, dst.c_str(), AVIO_FLAG_WRITE) < 0) {
            avformat_close_input(&in_ctx);
            avformat_free_context(out_ctx);
            return "cannot open output file: " + dst;
        }
    }

    if (avformat_write_header(out_ctx, nullptr) < 0) {
        avformat_close_input(&in_ctx);
        if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_ctx->pb);
        avformat_free_context(out_ctx);
        return "cannot write output header";
    }

    // Seek to just before start
    int64_t seek_ts = (int64_t)(start_sec * AV_TIME_BASE);
    av_seek_frame(in_ctx, -1, seek_ts, AVSEEK_FLAG_BACKWARD);

    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(in_ctx, pkt) >= 0) {
        int si = pkt->stream_index;
        if (si < 0 || si >= (int)in_ctx->nb_streams || stream_map[si] < 0) {
            av_packet_unref(pkt);
            continue;
        }
        AVStream* in_st  = in_ctx->streams[si];
        AVStream* out_st = out_ctx->streams[stream_map[si]];

        double pts_sec = (pkt->pts != AV_NOPTS_VALUE)
            ? pkt->pts * av_q2d(in_st->time_base)
            : start_sec;

        if (pts_sec < start_sec) { av_packet_unref(pkt); continue; }
        if (pts_sec > end_sec)   { av_packet_unref(pkt); break; }

        // Restamp relative to segment start
        int64_t offset = av_rescale_q(
            (int64_t)(start_sec * AV_TIME_BASE), AV_TIME_BASE_Q, in_st->time_base);
        if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= offset;
        if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= offset;
        if (pkt->duration > 0)
            pkt->duration = av_rescale_q(pkt->duration, in_st->time_base, out_st->time_base);
        pkt->pts      = av_rescale_q_rnd(pkt->pts, in_st->time_base, out_st->time_base,
                                         (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        pkt->dts      = av_rescale_q_rnd(pkt->dts, in_st->time_base, out_st->time_base,
                                         (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        pkt->stream_index = stream_map[si];
        av_interleaved_write_frame(out_ctx, pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    av_write_trailer(out_ctx);
    if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_ctx->pb);
    avformat_close_input(&in_ctx);
    avformat_free_context(out_ctx);
    return "";
}

// ── Browser thumbnail cache ───────────────────────────────────────────────────

uintptr_t video_load_thumb(const std::string& path, int* out_w, int* out_h) {
    struct Entry { GLuint tex; int w, h; };
    static std::unordered_map<std::string, Entry> s_cache;
    auto it = s_cache.find(path);
    if (it != s_cache.end()) {
        if (out_w) *out_w = it->second.w;
        if (out_h) *out_h = it->second.h;
        return it->second.tex;
    }

    if (!fs::exists(path)) return 0;

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return 0; }
    std::vector<uint8_t> buf((size_t)sz);
    fread(buf.data(), 1, (size_t)sz, f);
    fclose(f);

    int w, h, ch;
    uint8_t* px = stbi_load_from_memory(buf.data(), (int)sz, &w, &h, &ch, 3);
    if (!px) { s_cache[path] = {0, 0, 0}; return 0; }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(px);

    s_cache[path] = {tex, w, h};
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return tex;
}

// ── Export path — FFmpeg original file ───────────────────────────────────────
//
// Each slot is an independent decoder so that two clips in a cross-dissolve
// don't thrash each other's sequential-decode position.

static struct ExportState {
    AVFormatContext* fmt_ctx          = nullptr;
    AVCodecContext*  codec_ctx        = nullptr;
    SwsContext*      sws              = nullptr;
    int              stream_idx       = -1;
    int              rotation         = 0;   // degrees CW to apply to decoded frames (0/90/180/270)
    VideoInfo        info             = {};
    // Sequential decode state — avoids seek+flush on every frame when exporting.
    // Set to -1 when a seek is needed (first call, new file, backward jump, etc.).
    double           last_decoded_pts = -1.0;
    std::string      cur_path;   // path currently open in this slot (self-tracking)
    // ── Last-frame cache ──────────────────────────────────────────────────────
    // When multiple output frames map to the same source frame (slow-mo, stills,
    // 0.25× speed → 4 outputs per source frame) the second+ request is served
    // from this cached RGBA buffer instead of re-decoding. A clone is returned
    // each time so the existing "caller frees" contract is preserved; the clone
    // memcpy is ~2 ms at 1080p versus ~10–30 ms for a real decode.
    uint8_t*         cache_data       = nullptr;
    int              cache_w          = 0;
    int              cache_h          = 0;
    double           cache_pts        = -1.0;
} g_ex[MAX_VIDEO_TRACKS * 2];

bool video_open_export(int slot, const std::string& path) {
    if (slot < 0 || slot >= MAX_VIDEO_TRACKS * 2) return false;
    ExportState& ex = g_ex[slot];

    // If this slot already has the right file open, skip the expensive re-open.
    if (ex.cur_path == path && ex.fmt_ctx) return true;
    video_close_export(slot);

    const std::string& url3 = path;
    if (avformat_open_input(&ex.fmt_ctx, url3.c_str(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(ex.fmt_ctx, nullptr) < 0) {
        avformat_close_input(&ex.fmt_ctx); return false;
    }
    for (unsigned i = 0; i < ex.fmt_ctx->nb_streams; ++i) {
        if (ex.fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            ex.stream_idx = (int)i; break;
        }
    }
    if (ex.stream_idx < 0) { avformat_close_input(&ex.fmt_ctx); return false; }

    AVStream* st = ex.fmt_ctx->streams[ex.stream_idx];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    ex.codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ex.codec_ctx, st->codecpar);
    // Multi-threaded decode via SLICE threading only.
    //
    // FF_THREAD_FRAME (frame threading) parallelises across consecutive frames,
    // but its multi-frame output delay does NOT survive our manual
    // read-packet / drain-frame export loop: at certain GOP / B-frame
    // boundaries a frame is skipped and never recovered, so
    // video_decode_frame_at returns null and the caller freezes on the
    // EOF-hold — the symptom was every clip's tail stuck on one frame in the
    // exported MP4 (preview, which decodes differently, was fine). Reproduced
    // down to a standalone harness: frame-threading sticks mid-clip, slice-only
    // decodes the whole used range correctly. (2026-06-25.)
    //
    // FF_THREAD_SLICE parallelises *within* a frame — no reorder delay — so it
    // stays correct. thread_count=0 lets libavcodec pick based on CPU cores.
    ex.codec_ctx->thread_count = 0;
    ex.codec_ctx->thread_type  = FF_THREAD_SLICE;
    avcodec_open2(ex.codec_ctx, codec, nullptr);

    ex.info.width    = ex.codec_ctx->width;
    ex.info.height   = ex.codec_ctx->height;
    ex.info.duration = (double)ex.fmt_ctx->duration / AV_TIME_BASE;
    ex.info.fps      = av_q2d(st->avg_frame_rate);

    // Detect container rotation (phone portrait videos store raw as landscape + rotate tag).
    ex.rotation = 0;
#if LIBAVUTIL_VERSION_MAJOR >= 57
    for (int i = 0; i < st->codecpar->nb_coded_side_data; ++i) {
        const AVPacketSideData& sd = st->codecpar->coded_side_data[i];
#else
    for (int i = 0; i < st->nb_side_data; ++i) {
        const AVPacketSideData& sd = st->side_data[i];
#endif
        if (sd.type == AV_PKT_DATA_DISPLAYMATRIX && sd.size >= 9 * (int)sizeof(int32_t)) {
            double angle = -av_display_rotation_get((const int32_t*)sd.data);
            int rot = ((int)round(angle) % 360 + 360) % 360;
            if (rot == 90 || rot == 180 || rot == 270) { ex.rotation = rot; break; }
        }
    }
    if (ex.rotation == 0) {
        AVDictionaryEntry* e = av_dict_get(st->metadata, "rotate", nullptr, 0);
        if (e) {
            int rot = ((atoi(e->value) % 360) + 360) % 360;
            if (rot == 90 || rot == 180 || rot == 270) ex.rotation = rot;
        }
    }

    ex.sws = sws_getContext(
        ex.info.width, ex.info.height, ex.codec_ctx->pix_fmt,
        ex.info.width, ex.info.height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    ex.cur_path = path;
    return true;
}

void video_close_export(int slot) {
    if (slot < 0 || slot >= MAX_VIDEO_TRACKS * 2) return;
    ExportState& ex = g_ex[slot];
    if (ex.sws)       { sws_freeContext(ex.sws);       ex.sws       = nullptr; }
    if (ex.codec_ctx) { avcodec_free_context(&ex.codec_ctx); }
    if (ex.fmt_ctx)   { avformat_close_input(&ex.fmt_ctx); }
    if (ex.cache_data){ av_free(ex.cache_data); ex.cache_data = nullptr; }
    ex.cache_w = ex.cache_h = 0;
    ex.cache_pts        = -1.0;
    ex.stream_idx       = -1;
    ex.info             = {};
    ex.last_decoded_pts = -1.0;
    ex.cur_path.clear();
}

void video_close_export_all() {
    for (int i = 0; i < MAX_VIDEO_TRACKS * 2; ++i)
        video_close_export(i);
}

static VideoFrame* decode_and_rotate(ExportState& ex, AVFrame* frm) {
    VideoFrame* vf = new VideoFrame();
    vf->width  = ex.info.width;
    vf->height = ex.info.height;
    vf->data   = (uint8_t*)av_malloc((size_t)vf->width * vf->height * 4 + 64);
    AVStream* st = ex.fmt_ctx->streams[ex.stream_idx];
    vf->pts = frm->pts * av_q2d(st->time_base);
    uint8_t* dst[1] = { vf->data };
    int      lsz[1] = { vf->width * 4 };
    sws_scale(ex.sws,
        (const uint8_t* const*)frm->data, frm->linesize,
        0, frm->height, dst, lsz);
    if (ex.rotation != 0) {
        int ow = vf->width, oh = vf->height;
        int nw = (ex.rotation == 90 || ex.rotation == 270) ? oh : ow;
        int nh = (ex.rotation == 90 || ex.rotation == 270) ? ow : oh;
        uint8_t* rot = (uint8_t*)av_malloc((size_t)nw * nh * 4 + 64);
        if (rot) {
            for (int y = 0; y < oh; ++y) {
                for (int x = 0; x < ow; ++x) {
                    const uint8_t* s = vf->data + (y * ow + x) * 4;
                    int dx, dy;
                    if      (ex.rotation == 90)  { dx = oh-1-y; dy = x;      }
                    else if (ex.rotation == 270) { dx = y;      dy = ow-1-x; }
                    else                         { dx = ow-1-x; dy = oh-1-y; }
                    uint8_t* d = rot + (dy * nw + dx) * 4;
                    d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3];
                }
            }
            av_free(vf->data);
            vf->data   = rot;
            vf->width  = nw;
            vf->height = nh;
        }
    }
    return vf;
}

VideoFrame* video_decode_frame_at(int slot, double seconds) {
    if (slot < 0 || slot >= MAX_VIDEO_TRACKS * 2) return nullptr;
    ExportState& ex = g_ex[slot];
    if (!ex.fmt_ctx) return nullptr;

    AVStream* st = ex.fmt_ctx->streams[ex.stream_idx];
    double frame_dur = (ex.info.fps > 0.0) ? (1.0 / ex.info.fps) : (1.0 / 30.0);

    // Past-EOF requests freeze on the last frame. A bad retime can ask for
    // source time far beyond the file; unclamped, every such output frame
    // seeks to the tail keyframe and re-decodes to EOF — which wedges an
    // export. Clamped, the first request decodes the final frame and every
    // subsequent one is a cache hit.
    if (ex.info.duration > 0.0 && seconds > ex.info.duration - frame_dur)
        seconds = ex.info.duration - frame_dur;

    // Cache hit: same source frame as last call (slow-mo, stills, hold frames).
    if (ex.cache_data && ex.cache_pts >= 0.0 &&
        fabs(seconds - ex.cache_pts) < frame_dur * 0.5) {
        size_t bytes = (size_t)ex.cache_w * ex.cache_h * 4;
        VideoFrame* hit = new VideoFrame();
        hit->width  = ex.cache_w;
        hit->height = ex.cache_h;
        hit->pts    = ex.cache_pts;
        hit->data   = (uint8_t*)av_malloc(bytes);
        if (!hit->data) { delete hit; return nullptr; }
        memcpy(hit->data, ex.cache_data, bytes);
        return hit;
    }

    // Sequential decode optimisation: avoid seeking on every frame during export.
    // A seek + avcodec_flush_buffers forces the decoder to restart from a keyframe
    // and decode forward, which for H.264 with 2-5 s GOP means up to ~150 frames
    // decoded and thrown away per output frame — extremely slow.
    //
    // When the caller requests frames in order (the normal export case), we can
    // simply continue reading from where we stopped last frame.  We only seek when:
    //   (a) First call after open/clip-change  (last_decoded_pts < 0)
    //   (b) Backward jump or repeat             (seconds <= last_decoded_pts)
    //   (c) Forward gap bigger than ~8 frames   (clip cut with speed change etc.)
    //
    // Note: the decoder's internal B-frame buffer is preserved between calls when
    // we do not flush, so avcodec_receive_frame drains buffered frames first —
    // no frames are skipped on sequential access.
    bool need_seek = (ex.last_decoded_pts < 0.0)
                  || (seconds <= ex.last_decoded_pts - frame_dur * 0.5)
                  || (seconds >  ex.last_decoded_pts + frame_dur * 8.0);

    if (need_seek) {
        int64_t ts = (int64_t)(seconds * AV_TIME_BASE);
        av_seek_frame(ex.fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(ex.codec_ctx);
    }

    AVPacket*   pkt    = av_packet_alloc();
    AVFrame*    frm    = av_frame_alloc();
    VideoFrame* result = nullptr;

    // AVSEEK_FLAG_BACKWARD lands on the keyframe before the target.
    // Decode forward, keeping the last frame whose pts <= seconds.
    // Stop as soon as we decode a frame past the target (we already have the right one).
    //
    // Drain-before-send ordering is load-bearing: decoders with an internal
    // frame delay (dav1d/AV1 — the yt-dlp webm case) return EAGAIN from
    // avcodec_send_packet while output frames are pending. The old
    // send-then-drain loop ignored send_packet's return, so on a full decoder
    // the packet was silently dropped and its frame never emitted — every ~8th
    // frame on AV1 sources. The export then served the NEXT frame early
    // (forward jerk), and the following call saw a backward request and did a
    // full seek+flush re-decode (~40 packets) — a 4 Hz judder plus a big
    // slowdown, export-only (preview plays all-intra MJPEG proxies, which
    // have no decode delay). Draining the decoder dry before each read/send
    // makes EAGAIN impossible and keeps AV1 emission in display order.
    bool done = false;
    while (!done) {
        while (!done && avcodec_receive_frame(ex.codec_ctx, frm) == 0) {
            double pts = frm->pts * av_q2d(st->time_base);
            if (pts > seconds + frame_dur * 0.5) {
                // Overshot the target. Normally the previously kept frame is the
                // right one and we simply discard this one (the decoder's B-frame
                // queue still holds any remaining frames for the next sequential
                // call). But at a clip's FIRST output frame there is no previous
                // frame: `seconds` is ~0 (in_point 0) and the first decodable
                // frame of a chunked proxy / container with a non-zero start
                // offset can carry a pts a hair past the half-frame threshold.
                // Discarding it left `result` null while last_decoded_pts is still
                // < 0, so the hold-last-frame fallback below could not fire and the
                // frame rendered blank — black in export, white in preview: the
                // black flash at the start of every proxied clip after a cut.
                // Keep this earliest available frame instead of returning null.
                if (!result) result = decode_and_rotate(ex, frm);
                av_frame_unref(frm);
                done = true;
            } else {
                // This frame is at or before the target — keep it as best candidate.
                if (result) { av_free(result->data); delete result; }
                result = decode_and_rotate(ex, frm);
                av_frame_unref(frm);
                // If pts is within half a frame of target, we're accurate enough.
                if (pts >= seconds - frame_dur * 0.5) done = true;
            }
        }
        if (done) break;
        if (av_read_frame(ex.fmt_ctx, pkt) < 0) break;  // EOF
        if (pkt->stream_index != ex.stream_idx) { av_packet_unref(pkt); continue; }
        avcodec_send_packet(ex.codec_ctx, pkt);  // cannot EAGAIN: decoder just drained
        av_packet_unref(pkt);
    }

    if (result) {
        ex.last_decoded_pts = result->pts;
        // Refresh cache with this decoded frame so subsequent same-frame requests hit.
        size_t bytes = (size_t)result->width * result->height * 4;
        if (ex.cache_w != result->width || ex.cache_h != result->height) {
            if (ex.cache_data) { av_free(ex.cache_data); ex.cache_data = nullptr; }
            ex.cache_data = (uint8_t*)av_malloc(bytes);
            ex.cache_w = result->width;
            ex.cache_h = result->height;
        }
        if (ex.cache_data) memcpy(ex.cache_data, result->data, bytes);
        ex.cache_pts = result->pts;
    } else if (ex.last_decoded_pts >= 0.0) {
        // EOF or decode failure — hold the last successfully decoded frame rather
        // than returning null (which would produce a blank/black flash).
        // Re-decode at last_decoded_pts; this will seek and return the same frame.
        // Only do this once (don't recurse if the re-decode also fails).
        av_packet_free(&pkt);
        av_frame_free(&frm);
        double hold_pts = ex.last_decoded_pts;
        ex.last_decoded_pts = -1.0;  // force re-seek
        return video_decode_frame_at(slot, hold_pts);
    }

    av_packet_free(&pkt);
    av_frame_free(&frm);
    return result;
}

void video_free_frame(VideoFrame* f) {
    if (!f) return;
    av_free(f->data);
    delete f;
}

int video_export_width(int slot) {
    if (slot < 0 || slot >= MAX_VIDEO_TRACKS * 2) return 0;
    return g_ex[slot].info.width;
}

// Preview-frame dimensions for a playback slot (0,0 if not decoded yet). The
// aspect matches the clip's source. Uses the live GL texture size (set on every
// path — proxy and native), falling back to the probed source info.
void video_preview_dims(int slot, int* w, int* h) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (slot < 0 || slot >= MAX_VIDEO_SLOTS) return;
    PreviewState& pv = g_pv[slot];
    int tw = pv.tex_w, th = pv.tex_h;
    if (tw <= 0 || th <= 0) { tw = pv.info.width; th = pv.info.height; }
    if (w) *w = tw;
    if (h) *h = th;
}
int video_export_height(int slot) {
    if (slot < 0 || slot >= MAX_VIDEO_TRACKS * 2) return 0;
    return g_ex[slot].info.height;
}

void video_apply_datamosh(VideoFrame* vf, float intensity, float time_sec) {
    if (!vf || !vf->data || intensity <= 0.f) return;
    int W = vf->width, H = vf->height;
    // Mosh the RGB through the shared block-displacement core (identical look
    // to the preview), then write it back into the RGBA frame keeping alpha.
    static thread_local std::vector<uint8_t> rgb;
    rgb.resize((size_t)W * H * 3);
    for (int i = 0; i < W * H; ++i) {
        rgb[(size_t)i*3+0] = vf->data[(size_t)i*4+0];
        rgb[(size_t)i*3+1] = vf->data[(size_t)i*4+1];
        rgb[(size_t)i*3+2] = vf->data[(size_t)i*4+2];
    }
    datamosh_rgb(rgb.data(), W, H, intensity, time_sec);
    for (int i = 0; i < W * H; ++i) {
        vf->data[(size_t)i*4+0] = rgb[(size_t)i*3+0];
        vf->data[(size_t)i*4+1] = rgb[(size_t)i*3+1];
        vf->data[(size_t)i*4+2] = rgb[(size_t)i*3+2];
    }
}

// ── FX preview thumbnails ─────────────────────────────────────────────────────
// Source image: portrait_preview.h — 108×192 portrait RGB, embedded at build time
// from assets/imgs/portrait.jpg (center-cropped 9:16, Lanczos-scaled).
// Animated types (Glitch, VHS) regenerate every frame — 20,736 pixels, negligible.

#include "portrait_preview.h"
#include "fx_motion_webm.h"   // embedded FX-preview motion clip (CC BY 3.0 — see CREDITS)
#include "fx_face.h"          // embedded AI-woman face for face-centric effect previews
#include "paths.h"
#include <fstream>

static const int FXP_W = portrait_preview_w;   // 108
static const int FXP_H = portrait_preview_h;   // 192
static const int FXP_N = 8;  // number of legacy FXType enum values

struct FXPrev { GLuint tex = 0; bool animated = false; int src_ver = -1; };
static std::array<FXPrev, FXP_N> s_fxp;
static std::vector<uint8_t> s_fxp_src;      // base source image (RGB)

// Portrait source uploaded as GL texture for generated-effect previews
static GLuint s_fxp_portrait_gl = 0;
// Cache of output textures for generated effects, indexed by FXType int value
static std::unordered_map<int, GLuint> s_gen_fxp_cache;
static std::vector<uint8_t> s_fxp_src_ck;   // chroma key source (right half = green)

static void fxp_make_sources() {
    s_fxp_src.assign(portrait_preview_rgb, portrait_preview_rgb + portrait_preview_size);
    // Chroma key source: composite a green screen over the right 55% of the photo
    s_fxp_src_ck = s_fxp_src;
    for (int y = 0; y < FXP_H; ++y) {
        for (int x = (int)(FXP_W * 0.45f); x < FXP_W; ++x) {
            size_t i = ((size_t)y * FXP_W + x) * 3;
            s_fxp_src_ck[i+0] = 20; s_fxp_src_ck[i+1] = 200; s_fxp_src_ck[i+2] = 40;
        }
    }
}

static void fxp_upload(FXPrev& pv, const std::vector<uint8_t>& px) {
    if (!pv.tex) {
        glGenTextures(1, &pv.tex);
        glBindTexture(GL_TEXTURE_2D, pv.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, pv.tex);
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, FXP_W, FXP_H, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, px.data());
}

// The built-in portrait uploaded as a GL texture — the default preview source
// (and the source for generated-effect card thumbnails).
static void ensure_portrait_gl() {
    if (s_fxp_portrait_gl) return;
    if (s_fxp_src.empty()) fxp_make_sources();
    glGenTextures(1, &s_fxp_portrait_gl);
    glBindTexture(GL_TEXTURE_2D, s_fxp_portrait_gl);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, FXP_W, FXP_H, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, portrait_preview_rgb);
    glBindTexture(GL_TEXTURE_2D, 0);
}

uintptr_t video_default_preview_tex() {
    ensure_portrait_gl();
    return (uintptr_t)s_fxp_portrait_gl;
}

// Static portrait still (portrait.jpg, never touched by the motion clip) — the
// Ken Burns hover renders its pan/zoom on this so the move reads clearly.
static GLuint s_fxp_still_gl = 0;
static uintptr_t ensure_still_gl() {
    if (!s_fxp_still_gl) {
        glGenTextures(1, &s_fxp_still_gl);
        glBindTexture(GL_TEXTURE_2D, s_fxp_still_gl);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, FXP_W, FXP_H, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, portrait_preview_rgb);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    return (uintptr_t)s_fxp_still_gl;
}

// AI-woman face source — for face-centric effects (Skin Smooth, Glow Up) so the
// smoothing/glow reads on an actual face. (face.jpg — StyleGAN2, see CREDITS.)
static GLuint s_fxp_face_gl = 0;
static uintptr_t ensure_face_gl() {
    if (!s_fxp_face_gl) {
        glGenTextures(1, &s_fxp_face_gl);
        glBindTexture(GL_TEXTURE_2D, s_fxp_face_gl);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fx_face_w, fx_face_h, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, fx_face_rgb);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    return (uintptr_t)s_fxp_face_gl;
}

// ── FX-preview motion source ──────────────────────────────────────────────────
// The bundled clip decoded once into preview-sized RGB frames, then cycled by
// fxp_motion_advance() so the FX picker cards show motion instead of a frozen
// still. Generated-effect previews re-render every frame and sample the GL
// portrait texture, so re-uploading it when the frame flips is enough for them
// to animate. (CC BY 3.0 — Hackensack Meridian Health; see CREDITS.)
static std::vector<std::vector<uint8_t>> s_fxp_motion;   // RGB, FXP_W×FXP_H
static int  s_fxp_motion_cur   = -1;
static bool s_fxp_motion_tried = false;

static void fxp_load_motion() {
    s_fxp_motion_tried = true;
    // Materialise the embedded clip to a cache file, then decode it with a plain
    // file-based libav loop (simpler/safer than custom-AVIO memory IO).
    std::string path = media_cache_dir() + "/fx_preview_motion.webm";
    { std::ofstream f(path, std::ios::binary);
      f.write(reinterpret_cast<const char*>(fx_motion_webm),
              (std::streamsize)fx_motion_webm_size); }

    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) != 0) return;
    if (avformat_find_stream_info(fc, nullptr) < 0) { avformat_close_input(&fc); return; }

    int vs = -1;
    for (unsigned i = 0; i < fc->nb_streams; ++i)
        if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { vs = (int)i; break; }
    if (vs < 0) { avformat_close_input(&fc); return; }

    AVCodecParameters* cp = fc->streams[vs]->codecpar;
    const AVCodec* dec = avcodec_find_decoder(cp->codec_id);
    AVCodecContext* cc = dec ? avcodec_alloc_context3(dec) : nullptr;
    if (!cc) { avformat_close_input(&fc); return; }
    avcodec_parameters_to_context(cc, cp);
    if (avcodec_open2(cc, dec, nullptr) < 0) {
        avcodec_free_context(&cc); avformat_close_input(&fc); return;
    }

    SwsContext* sws = nullptr;
    AVFrame*  fr = av_frame_alloc();
    AVPacket* pk = av_packet_alloc();
    auto emit = [&]() {
        if (!sws)
            sws = sws_getContext(fr->width, fr->height, (AVPixelFormat)fr->format,
                                 FXP_W, FXP_H, AV_PIX_FMT_RGB24,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) return;
        std::vector<uint8_t> rgb((size_t)FXP_W * FXP_H * 3);
        uint8_t* d[1]  = { rgb.data() };
        int      ls[1] = { FXP_W * 3 };
        sws_scale(sws, fr->data, fr->linesize, 0, fr->height, d, ls);
        s_fxp_motion.push_back(std::move(rgb));
    };
    while (av_read_frame(fc, pk) >= 0) {
        if (pk->stream_index == vs && avcodec_send_packet(cc, pk) == 0)
            while (avcodec_receive_frame(cc, fr) == 0) emit();
        av_packet_unref(pk);
    }
    avcodec_send_packet(cc, nullptr);            // flush
    while (avcodec_receive_frame(cc, fr) == 0) emit();

    av_packet_free(&pk);
    av_frame_free(&fr);
    if (sws) sws_freeContext(sws);
    avcodec_free_context(&cc);
    avformat_close_input(&fc);
}

// Advance the shared preview source to the clip frame for `now` (seconds). Cheap
// when the frame hasn't changed; re-uploads the GL portrait when it flips.
void fxp_motion_advance(double now) {
    if (!s_fxp_motion_tried) fxp_load_motion();
    if (s_fxp_motion.empty()) return;
    const double kFps = 24.0;
    int n   = (int)s_fxp_motion.size();
    int idx = (int)((long long)(now * kFps) % n);
    if (idx < 0) idx += n;
    if (idx == s_fxp_motion_cur) return;
    s_fxp_motion_cur = idx;

    if (s_fxp_src.empty()) fxp_make_sources();   // ensure size/buffers exist
    s_fxp_src = s_fxp_motion[idx];
    // Rebuild the chroma-key source (green over the right 55%) from the new frame.
    s_fxp_src_ck = s_fxp_src;
    for (int y = 0; y < FXP_H; ++y)
        for (int x = (int)(FXP_W * 0.45f); x < FXP_W; ++x) {
            size_t i = ((size_t)y * FXP_W + x) * 3;
            s_fxp_src_ck[i+0] = 20; s_fxp_src_ck[i+1] = 200; s_fxp_src_ck[i+2] = 40;
        }
    if (s_fxp_portrait_gl) {
        glBindTexture(GL_TEXTURE_2D, s_fxp_portrait_gl);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, FXP_W, FXP_H,
                        GL_RGB, GL_UNSIGNED_BYTE, s_fxp_src.data());
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

// Apply one legacy CPU preview effect onto `src` (RGB, w*h*3) → `out` (RGB), at
// time t. Size-parametric so the same code drives both the small card thumbnail
// and the big hover popover. `synthetic` enables the canned framing (green
// screen for chroma-key, etc.) that only makes sense on the built-in portrait;
// on real footage those degrade to showing the source. Returns true if animated.
static bool fxp_cpu_effect(FXType ft, const std::vector<uint8_t>& src,
                           std::vector<uint8_t>& out, int w, int h,
                           float t, bool synthetic) {
    bool animated = false;
    switch (ft) {
        case FXType::Grade:
            out = src;
            cpu_apply_grade(out.data(), w, h, 0.06f, 1.45f, 1.7f, 0.f);
            break;
        case FXType::ChromaKey:
            if (synthetic && w == FXP_W && h == FXP_H) {
                // Key the canned green screen and checker the transparent pixels.
                out.resize((size_t)w * h * 3);
                std::vector<uint8_t> rgba((size_t)w * h * 4);
                cpu_apply_chroma_key(s_fxp_src_ck.data(), rgba.data(),
                                     w, h, 0.f, 1.f, 0.f, 0.28f, 0.12f);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        size_t i = (size_t)y * w + x;
                        if (rgba[i*4+3] < 200) {
                            bool chk = ((x / 2) + (y / 2)) % 2 == 0;
                            uint8_t v = chk ? 38 : 24;
                            out[i*3+0] = v; out[i*3+1] = v; out[i*3+2] = v;
                        } else {
                            out[i*3+0] = rgba[i*4+0];
                            out[i*3+1] = rgba[i*4+1];
                            out[i*3+2] = rgba[i*4+2];
                        }
                    }
            } else {
                out = src;   // real footage has no key colour — show it as-is
            }
            break;
        case FXType::Glitch:
            out = src;
            cpu_apply_glitch(out.data(), w, h, 12.f, 0.7f, t);
            cpu_apply_corruption(out.data(), w, h, 0.55f, t);
            animated = true;
            break;
        case FXType::ZoomPunch: {
            out.resize((size_t)w * h * 3);
            // Repeating beat-style punch so the preview reads as a pulse, not a
            // static crop: spike in then decay back, once per ~0.6s.
            float ph    = fmodf(t, 0.6f) / 0.6f;        // phase within a beat
            float env   = expf(-ph * 5.5f);             // sharp attack, smooth decay
            float scale = 1.f + 0.28f * env;
            float cx_f = w * 0.5f, cy_f = h * 0.5f;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    int sx = (int)((x - cx_f) / scale + cx_f);
                    int sy = (int)((y - cy_f) / scale + cy_f);
                    sx = sx < 0 ? 0 : sx >= w ? w-1 : sx;
                    sy = sy < 0 ? 0 : sy >= h ? h-1 : sy;
                    size_t di = ((size_t)y*w+x)*3, si = ((size_t)sy*w+sx)*3;
                    out[di+0] = src[si+0]; out[di+1] = src[si+1]; out[di+2] = src[si+2];
                }
            animated = true;
            break;
        }
        case FXType::LUT:
            out = src;
            cpu_apply_grade(out.data(), w, h, -0.04f, 1.25f, 0.75f, 18.f);
            break;
        case FXType::LightLeak:
            out = src;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    float dx = ((float)x - w * 0.78f) / (w * 0.35f);
                    float dy = ((float)y - h * 0.18f) / (h * 0.45f);
                    float flr = fmaxf(0.f, 1.f - (dx*dx + dy*dy)) * 0.82f;
                    size_t i = ((size_t)y*w+x)*3;
                    out[i+0] = cu8((int)(out[i+0] + 255.f * flr * 0.88f));
                    out[i+1] = cu8((int)(out[i+1] + 255.f * flr * 0.35f));
                    out[i+2] = cu8((int)(out[i+2] + 255.f * flr * 0.05f));
                }
            break;
        case FXType::VHS:
            out = src;
            cpu_apply_vhs(out.data(), w, h, 0.65f, 9.f, 0.55f, t);
            animated = true;
            break;
        case FXType::Datamosh:
            out = src;
            datamosh_rgb(out.data(), w, h, 0.7f, t);
            animated = true;
            break;
        default:
            out = src;
            break;
    }
    return animated;
}

// Ken Burns is a geometric zoom+pan (render.cpp drives it from the clip
// timeline), so the shader preview path can't show it. Fake a CPU zoom+pan over
// a ping-ponged clock so the card actually reads as a Ken Burns move.
static void kenburns_cpu(const unsigned char* src, std::vector<uint8_t>& out,
                         int w, int h, float t) {
    out.resize((size_t)w * h * 3);
    float p   = fabsf(fmodf(t, 8.f) / 4.f - 1.f);   // 0 → 1 → 0 over 8s
    float sc  = 1.f + 0.30f * p;                     // zoom 1.0 → 1.3
    float cxf = (0.40f + 0.20f * p) * w;             // pan right + down
    float cyf = (0.42f + 0.16f * p) * h;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int sx = (int)((x - w * 0.5f) / sc + cxf);
            int sy = (int)((y - h * 0.5f) / sc + cyf);
            sx = sx < 0 ? 0 : sx >= w ? w-1 : sx;
            sy = sy < 0 ? 0 : sy >= h ? h-1 : sy;
            size_t di = ((size_t)y*w+x)*3, si = ((size_t)sy*w+sx)*3;
            out[di+0] = src[si+0]; out[di+1] = src[si+1]; out[di+2] = src[si+2];
        }
}

uintptr_t video_fx_preview_texture(FXType ft, float t, bool live) {
    int idx = (int)ft;

    // Ken Burns: faked CPU zoom+pan on the still (the real transform happens at
    // render time, so the shader preview would otherwise show no movement).
    if (ft == FXType::KenBurns) {
        static GLuint kb_tex = 0;
        std::vector<uint8_t> out;
        kenburns_cpu(portrait_preview_rgb, out, FXP_W, FXP_H, t);
        if (!kb_tex) {
            glGenTextures(1, &kb_tex);
            glBindTexture(GL_TEXTURE_2D, kb_tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else glBindTexture(GL_TEXTURE_2D, kb_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, FXP_W, FXP_H, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, out.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        return (uintptr_t)kb_tex;
    }

    // Generated effects — re-render every frame with current t (same as the
    // animated legacy effects), then blit into a stable per-effect texture so
    // simultaneous picker cards don't alias the shared output slot.
    if (idx >= FXP_N) {
        ensure_portrait_gl();

        // Per-effect source: face for beauty effects (Skin Smooth, Glow Up),
        // else the moving clip (default).
        uintptr_t src = (uintptr_t)s_fxp_portrait_gl;
        if (ft == FXType::SkinSmooth || ft == FXType::GlowUp) src = ensure_face_gl();

        // Render into the shared slot with live t
        uintptr_t shared = fx_preview_gen_effect(ft, src, FXP_W, FXP_H, t);

        // Ensure a dedicated texture exists for this effect
        GLuint dedicated = s_gen_fxp_cache.count(idx) ? s_gen_fxp_cache[idx] : 0;
        if (!dedicated) {
            glGenTextures(1, &dedicated);
            glBindTexture(GL_TEXTURE_2D, dedicated);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, FXP_W, FXP_H, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindTexture(GL_TEXTURE_2D, 0);
            s_gen_fxp_cache[idx] = dedicated;
        }

        // Blit shared output → dedicated texture so each picker card is stable
        GLuint read_fbo = 0, draw_fbo = 0;
        glGenFramebuffers(1, &read_fbo);
        glGenFramebuffers(1, &draw_fbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, (GLuint)shared, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, dedicated, 0);
        // fx_apply preserves the source orientation, and the preview source
        // (s_fxp_portrait_gl) is uploaded top-down — same as the CPU-effect and
        // adjustment paths — so the output is already top-down. A straight copy
        // (no V-flip) keeps it upright when the card draws it with top-down UVs;
        // the earlier flip here left every generated preview upside-down.
        glBlitFramebuffer(0, 0, FXP_W, FXP_H, 0, 0, FXP_W, FXP_H,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &read_fbo);
        glDeleteFramebuffers(1, &draw_fbo);

        return (uintptr_t)dedicated;
    }

    if (idx < 0) return 0;

    if (s_fxp_src.empty()) fxp_make_sources();

    FXPrev& pv = s_fxp[idx];
    // Re-render when the texture's gone, the effect animates with t, or — while
    // this card is hovered (live) — the shared motion source advanced, so a static
    // filter still loops the moving footage instead of freezing on the frame it was
    // first drawn on. Non-hovered cards keep one cached representative frame.
    bool need = !pv.tex || pv.animated || (live && pv.src_ver != s_fxp_motion_cur);
    if (!need) return (uintptr_t)pv.tex;

    std::vector<uint8_t> px;
    pv.animated = fxp_cpu_effect(ft, s_fxp_src, px, FXP_W, FXP_H, t, true);
    fxp_upload(pv, px);
    pv.src_ver = s_fxp_motion_cur;
    return (uintptr_t)pv.tex;
}

// ── Big hover-preview (popover) ──────────────────────────────────────────────
// Renders an effect (or grade) onto an arbitrary source texture at w×h for the
// FX-card hover popover, so the preview is large and runs on the user's actual
// footage. src_tex==0 → the built-in portrait. One popover shows at a time, so a
// single resize/output buffer pair is reused.
static GLuint s_bigsrc_tex = 0, s_bigsrc_fbo = 0;
static GLuint s_bigout_tex = 0, s_bigout_fbo = 0;
static int    s_big_w = 0, s_big_h = 0;

static void big_ensure(int w, int h) {
    if (s_bigsrc_tex && s_big_w == w && s_big_h == h) return;
    if (s_bigsrc_tex) {
        glDeleteTextures(1, &s_bigsrc_tex);  glDeleteFramebuffers(1, &s_bigsrc_fbo);
        glDeleteTextures(1, &s_bigout_tex);  glDeleteFramebuffers(1, &s_bigout_fbo);
    }
    auto mk = [&](GLuint& tex, GLuint& fbo) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    };
    mk(s_bigsrc_tex, s_bigsrc_fbo);
    mk(s_bigout_tex, s_bigout_fbo);
    s_big_w = w; s_big_h = h;
}

// Resize `src_tex` into the w×h source buffer (s_bigsrc_tex/fbo) and read it
// back as RGB. The fx_blit reads src_tex and the glReadPixels forces a full
// GPU→CPU flush; doing both every frame while a clip is selected serialized the
// readback against the decoder's live per-frame upload into video_get_texture's
// preview texture — that contention (absent on the settled scene FBO) is why FX
// hover lagged with a clip selected but stayed smooth on the whole project.
// The hovered source frame is static, so cache the result by (src_tex, w, h)
// and only touch the GPU when it actually changes; callers get a private copy
// they may mutate in place (grade/effect). The same hit leaves s_bigsrc_tex
// holding the resized source for the generated-effect GPU path.
static uintptr_t s_bigsrc_key_tex = 0;
static int       s_bigsrc_key_w = 0, s_bigsrc_key_h = 0;
static int       s_bigsrc_key_ver = -2;
static std::vector<uint8_t> s_bigsrc_cache;

static bool big_source_prep(uintptr_t src_tex, int w, int h) {
    // The built-in motion portrait is updated IN PLACE by fxp_motion_advance
    // (same texture object, new pixels), so its pointer can't signal the change —
    // fold the motion frame counter into the key for it, so the big popover
    // re-reads and animates. Any other source (the live clip frame) is static
    // while hovering, so keep caching it by pointer alone to avoid the per-frame
    // GPU readback the note below warns about.
    int ver = (src_tex == (uintptr_t)s_fxp_portrait_gl) ? s_fxp_motion_cur : -1;
    if (src_tex == s_bigsrc_key_tex && w == s_bigsrc_key_w && h == s_bigsrc_key_h &&
        ver == s_bigsrc_key_ver && s_bigsrc_cache.size() == (size_t)w * h * 3)
        return false;  // s_bigsrc_tex + cache already hold this source
    fx_blit(src_tex, s_bigsrc_fbo, w, h);
    s_bigsrc_cache.resize((size_t)w * h * 3);
    glBindFramebuffer(GL_FRAMEBUFFER, s_bigsrc_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, s_bigsrc_cache.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_bigsrc_key_tex = src_tex; s_bigsrc_key_w = w; s_bigsrc_key_h = h;
    s_bigsrc_key_ver = ver;
    return true;
}

static void big_source_rgb(uintptr_t src_tex, int w, int h, std::vector<uint8_t>& rgb) {
    big_source_prep(src_tex, w, h);
    rgb = s_bigsrc_cache;  // cheap CPU copy; caller mutates in place
}

uintptr_t video_fx_preview_big(FXType ft, float t, uintptr_t src_tex, int w, int h) {
    bool synthetic = (src_tex == 0);
    if (synthetic) src_tex = video_default_preview_tex();
    if (w < 8 || h < 8) return 0;
    big_ensure(w, h);

    // Beauty effects preview on the AI-woman face; Ken Burns gets a faked CPU
    // zoom+pan (its real transform is render-time, invisible to the shader path).
    if (ft == FXType::SkinSmooth || ft == FXType::GlowUp) {
        src_tex = ensure_face_gl();
    } else if (ft == FXType::KenBurns) {
        std::vector<uint8_t> src, out;
        big_source_rgb(ensure_still_gl(), w, h, src);   // portrait.jpg, not the live source
        kenburns_cpu(src.data(), out, w, h, t);
        glBindTexture(GL_TEXTURE_2D, s_bigout_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, out.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        return (uintptr_t)s_bigout_tex;
    }

    int idx = (int)ft;
    if (idx >= FXP_N) {
        // Generated GPU effect: resize source then run the effect on it, and
        // copy out of the shared preview slot so card thumbnails can't clobber it.
        // big_source_prep blits into s_bigsrc_tex only when the source frame
        // changes (skips the per-frame read of the live clip texture); the
        // effect still re-runs each frame so time-driven motion animates.
        big_source_prep(src_tex, w, h);
        uintptr_t shared = fx_preview_gen_effect(ft, (uintptr_t)s_bigsrc_tex, w, h, t);
        fx_blit(shared, s_bigout_fbo, w, h);
        return (uintptr_t)s_bigout_tex;
    }

    std::vector<uint8_t> src, out;
    big_source_rgb(src_tex, w, h, src);
    fxp_cpu_effect(ft, src, out, w, h, t, synthetic);
    glBindTexture(GL_TEXTURE_2D, s_bigout_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, out.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return (uintptr_t)s_bigout_tex;
}

uintptr_t video_adj_preview_big(uintptr_t src_tex, int w, int h,
                                float brightness, float contrast,
                                float saturation, float hue,
                                float blur, float vignette) {
    if (src_tex == 0) src_tex = video_default_preview_tex();
    if (w < 8 || h < 8) return 0;
    big_ensure(w, h);
    std::vector<uint8_t> rgb;
    big_source_rgb(src_tex, w, h, rgb);
    // Mirrors video_adj_preview_texture's order/feel (grade → blur → vignette).
    if (contrast != 1.f || brightness != 0.f || saturation != 1.f || fabsf(hue) > 0.1f)
        cpu_apply_grade(rgb.data(), w, h, brightness, contrast, saturation, hue);
    if (blur > 0.1f)
        cpu_apply_blur(rgb.data(), w, h, blur * 2.f);
    if (vignette > 0.01f) {
        float cx_f = w * 0.5f, cy_f = h * 0.5f, rad = fmaxf(cx_f, cy_f);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float dx = (x - cx_f) / rad, dy = (y - cy_f) / rad;
                float d  = fminf(1.f, sqrtf(dx*dx + dy*dy));
                float vig = d * d * vignette;
                size_t i = ((size_t)y*w+x)*3;
                rgb[i+0] = cu8((int)(rgb[i+0] * (1.f - vig)));
                rgb[i+1] = cu8((int)(rgb[i+1] * (1.f - vig)));
                rgb[i+2] = cu8((int)(rgb[i+2] * (1.f - vig)));
            }
    }
    glBindTexture(GL_TEXTURE_2D, s_bigout_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return (uintptr_t)s_bigout_tex;
}

// ── Adjustment preset preview textures ───────────────────────────────────────
// Keyed by unique_id in a small fixed-size cache. LRU eviction not needed —
// the preset list is static at runtime; we just keep one texture per preset.

static const int ADJ_PREV_MAX = 64;
struct AdjPrev { int id = -1; GLuint tex = 0; int src_ver = -1; };
static std::array<AdjPrev, ADJ_PREV_MAX> s_adj_prev;
static int s_adj_prev_next = 0;

uintptr_t video_adj_preview_texture(int unique_id,
                                     float brightness, float contrast,
                                     float saturation, float hue,
                                     float blur, float vignette, bool live) {
    if (s_fxp_src.empty()) fxp_make_sources();

    // Find existing slot. Return its cached texture unless this card is hovered
    // (live) and the shared motion source advanced — then re-render into the slot
    // so the grade preview loops the moving footage instead of freezing forever.
    AdjPrev* slot = nullptr;
    for (auto& a : s_adj_prev)
        if (a.id == unique_id) { slot = &a; break; }
    if (slot && !(live && slot->src_ver != s_fxp_motion_cur))
        return (uintptr_t)slot->tex;
    if (!slot) {
        slot = &s_adj_prev[s_adj_prev_next % ADJ_PREV_MAX];
        s_adj_prev_next++;
        slot->id = unique_id;
    }
    slot->src_ver = s_fxp_motion_cur;
    AdjPrev& ap = *slot;

    std::vector<uint8_t> px = s_fxp_src;

    if (contrast != 1.f || brightness != 0.f || saturation != 1.f || fabsf(hue) > 0.1f)
        cpu_apply_grade(px.data(), FXP_W, FXP_H, brightness, contrast, saturation, hue);
    if (blur > 0.1f)
        cpu_apply_blur(px.data(), FXP_W, FXP_H, blur * 2.f);
    if (vignette > 0.01f) {
        float cx_f = FXP_W * 0.5f, cy_f = FXP_H * 0.5f;
        float rad   = fmaxf(cx_f, cy_f);
        for (int y = 0; y < FXP_H; ++y) {
            for (int x = 0; x < FXP_W; ++x) {
                float dx = (x - cx_f) / rad, dy = (y - cy_f) / rad;
                float d  = fminf(1.f, sqrtf(dx*dx + dy*dy));
                float vig = d * d * vignette;
                size_t i = ((size_t)y*FXP_W+x)*3;
                px[i+0] = cu8((int)(px[i+0] * (1.f - vig)));
                px[i+1] = cu8((int)(px[i+1] * (1.f - vig)));
                px[i+2] = cu8((int)(px[i+2] * (1.f - vig)));
            }
        }
    }

    FXPrev tmp; tmp.tex = ap.tex;   // reuse the slot's texture on re-render (no leak)
    fxp_upload(tmp, px);
    ap.tex = tmp.tex;
    return (uintptr_t)ap.tex;
}
