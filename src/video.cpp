#include "video.h"

// ── stb_image — JPEG decode for preview frames ────────────────────────────────
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <GL/gl.h>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <array>
#include <vector>

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
    static std::vector<uint8_t> s_row;
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

// Output is 4-channel RGBA. Keying in chroma space separates colour from luminance
// so the key distance is independent of scene brightness. Spill suppression blends
// the dominant key channel toward its neighbours on semi-transparent edge pixels.
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
    for (int i = 0; i < n; ++i) {
        float r = rgb[i*3+0] * (1.f/255.f);
        float g = rgb[i*3+1] * (1.f/255.f);
        float b = rgb[i*3+2] * (1.f/255.f);

        float lum  = 0.299f*r + 0.587f*g + 0.114f*b;
        float cp_r = r - lum, cp_g = g - lum, cp_b = b - lum;

        float dist = sqrtf((cp_r-ck_r)*(cp_r-ck_r) +
                           (cp_g-ck_g)*(cp_g-ck_g) +
                           (cp_b-ck_b)*(cp_b-ck_b));

        // smoothstep: 0 = fully keyed out, 1 = fully opaque
        float t     = (dist - threshold) / soft;
        float alpha = t <= 0.f ? 0.f : t >= 1.f ? 1.f : t*t*(3.f - 2.f*t);

        // Spill suppression: on semi-transparent pixels blend the dominant key
        // channel toward the average of the other two channels
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

    static std::vector<uint8_t> tmp;
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

// Datamosh — motion-detection driven P-frame simulation on JPEG proxy frames.
//
// Real datamosh corrupts interframe codec references so P-frame motion vectors
// apply to the wrong source frame, causing moving areas to smear while static
// areas stay clean. We replicate that using SAD (sum of absolute differences)
// per block between current frame and ghost:
//
//   HIGH SAD (motion detected) → keep ghost block (smear trail, like a stuck P-frame)
//   LOW  SAD (static area)     → pass current frame through normally
//
// The ghost buffer then self-feeds on the mashed output so corruption compounds
// frame over frame — even on a still image it keeps generating motion.
// Ghost initialises from the first frame so there's no black-frame flash.
static void cpu_apply_datamosh(uint8_t* px, uint8_t* ghost, int w, int h,
                                float intensity, float decay, int bs,
                                float bleedback, float t_in_clip, float clip_duration) {
    if (!ghost || w <= 0 || h <= 0) return;

    int nbx = (w + bs - 1) / bs;
    int nby = (h + bs - 1) / bs;

    // Fixed motion threshold — catches real inter-frame motion, ignores static areas.
    // Intensity no longer controls what moshs; it controls HOW DEEP the mosh goes.
    const float threshold = 8.f;

    // Intensity bends the effective decay: low intensity → ghost tracks current quickly
    // (short trails, mild mosh); high intensity → ghost persists long (deep chaos).
    // Floor at 0.05 so the ghost always bleeds in some real content — prevents runaway
    // feedback where total divergence locks every block into white-noise substitution.
    float effective_decay = decay * (1.f - intensity * 0.75f);
    effective_decay = fmaxf(0.05f, effective_decay);

    // Bleedback: organic block-level healing via ghost convergence.
    // Two levers driven by the same ramp:
    //   1. effective_decay ramps toward 1.0 — ghost learns the real frame faster
    //   2. upper SAD threshold ramps DOWN — diverged blocks that were skipped
    //      (ghost too smeared to use) now get replaced by real content instead,
    //      so blocks heal one by one rather than crossfading as a flat layer.
    float bleedback_amount = 0.f;
    if (bleedback > 0.01f && clip_duration > 0.01f && t_in_clip > 0.f) {
        float ramp_start = clip_duration * (1.f - fminf(1.f, bleedback) * 0.6f);
        if (t_in_clip > ramp_start) {
            float ramp_t = (t_in_clip - ramp_start) / (clip_duration - ramp_start);
            bleedback_amount = fminf(1.f, ramp_t) * bleedback;
        }
    }
    if (bleedback_amount > 0.01f)
        effective_decay = effective_decay + (1.f - effective_decay) * bleedback_amount;

    // Upper threshold for Pass 1: ramps 180→15 so progressively more smeared
    // blocks get healed by the real frame instead of substituting ghost pixels.
    float upper_thresh = 180.f - (180.f - 15.f) * bleedback_amount;

    // ── Pass 1: per-block SAD → mosh or pass ─────────────────────────────────
    for (int by = 0; by < nby; ++by) {
        for (int bx = 0; bx < nbx; ++bx) {
            uint32_t sad = 0;
            int count = 0;
            for (int py = 0; py < bs; ++py) {
                int y = by * bs + py; if (y >= h) break;
                for (int px_ = 0; px_ < bs; ++px_) {
                    int x = bx * bs + px_; if (x >= w) break;
                    size_t i = (size_t)y * w * 3 + x * 3;
                    sad += (uint32_t)abs((int)px[i+0] - (int)ghost[i+0]);
                    sad += (uint32_t)abs((int)px[i+1] - (int)ghost[i+1]);
                    sad += (uint32_t)abs((int)px[i+2] - (int)ghost[i+2]);
                    count++;
                }
            }
            float mean_sad = count > 0 ? (float)sad / (float)(count * 3) : 0.f;

            if (mean_sad < threshold)     continue;  // static — let current frame through
            if (mean_sad > upper_thresh)  continue;  // diverged — heal with real content

            // Motion block — substitute ghost (stuck P-frame reference).
            // Displacement simulates wrong motion vector; chroma twist scales with
            // both motion strength and intensity for the colour-zone separation look.
            float motion_norm = fminf(1.f, mean_sad / 128.f);  // 0..1
            int disp_x = (int)(motion_norm * (float)nbx * 0.2f);
            int disp_y = (int)(motion_norm * (float)nby * 0.12f);
            int sbx = ((bx + disp_x) % nbx + nbx) % nbx;
            int sby = ((by + disp_y) % nby + nby) % nby;
            // Twist driven by both motion and intensity — more intensity = wider split
            int twist = (int)((motion_norm * 0.4f + intensity * 0.6f) * (float)bs * 0.55f);

            for (int py = 0; py < bs; ++py) {
                int dy  = by  * bs + py; if (dy >= h) break;
                int gsy = sby * bs + py; if (gsy >= h) gsy = h - 1;
                for (int px_ = 0; px_ < bs; ++px_) {
                    int dx  = bx  * bs + px_; if (dx >= w) break;
                    int gsx = sbx * bs + px_; if (gsx >= w) gsx = w - 1;
                    // R ahead in scan direction, B behind — colour-zone separation
                    int gsr = cx(gsx + twist,     w);
                    int gsb = cx(gsx - twist / 2, w);
                    px[(size_t)dy * w * 3 + dx * 3 + 0] = ghost[(size_t)gsy * w * 3 + gsr * 3 + 0];
                    px[(size_t)dy * w * 3 + dx * 3 + 1] = ghost[(size_t)gsy * w * 3 + gsx * 3 + 1];
                    px[(size_t)dy * w * 3 + dx * 3 + 2] = ghost[(size_t)gsy * w * 3 + gsb * 3 + 2];
                }
            }
        }
    }

    // ── Pass 2: update ghost ──────────────────────────────────────────────────
    float keep = 1.f - effective_decay;
    int n = w * h;
    for (int i = 0; i < n; ++i) {
        ghost[i*3+0] = cu8((int)(ghost[i*3+0] * keep + px[i*3+0] * effective_decay + 0.5f));
        ghost[i*3+1] = cu8((int)(ghost[i*3+1] * keep + px[i*3+1] * effective_decay + 0.5f));
        ghost[i*3+2] = cu8((int)(ghost[i*3+2] * keep + px[i*3+2] * effective_decay + 0.5f));
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
    static std::vector<uint8_t> s_row;
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

    static std::vector<uint8_t> tmp;
    tmp.resize((size_t)w * h * 3);
    for (int pass = 0; pass < 3; ++pass) {
        int r = sizes[pass] / 2;
        box_blur_h(px, tmp.data(), w, h, r);
        box_blur_v(tmp.data(), px,  w, h, r);
    }
}


// ── Preview state ─────────────────────────────────────────────────────────────

struct PreviewState {
    FILE*     mjpeg_file     = nullptr;
    ProxyInfo proxy          = {};
    int       last_frame_idx = -1;
    GLuint    tex      = 0;
    int       tex_w    = 0;
    int       tex_h    = 0;
    bool      tex_rgba = false;
    bool      is_proxy = false;
    bool      is_open  = false;
    VideoInfo info = {};
    PixelFX   pixel_fx;
    bool      pixel_fx_dirty = false;
    // Datamosh ghost buffer
    std::vector<uint8_t> ghost_buf;        // RGB, ghost_w * ghost_h * 3
    int                  ghost_w         = 0;
    int                  ghost_h         = 0;
    float                ghost_clip_start = -999.f; // tracks which clip owns the ghost
    int                  ghost_frame_idx  = -1;     // proxy frame the ghost currently represents
    int                  ghost_seed_idx   = -1;     // proxy frame of clip start (anchor)
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
};

static PreviewState g_pv[MAX_VIDEO_TRACKS];

// Separate texture for hover-preview thumbnails (track 0's proxy).
static struct ThumbState {
    GLuint tex            = 0;
    int    tex_w          = 0;
    int    tex_h          = 0;
    int    last_frame_idx = -1;
} g_th;

// Upload a JPEG buffer into a GL texture slot, optionally applying CPU pixel FX.
// tex_rgba: in/out — tracks whether the current GL texture is RGBA or RGB.
// bg_mask: pre-loaded alpha channel (w*h bytes, 0=transparent 255=opaque) or nullptr.
static void upload_jpeg(GLuint* tex, int* tex_w, int* tex_h, bool* tex_rgba,
                        const uint8_t* buf, size_t sz,
                        const PixelFX* pfx = nullptr,
                        uint8_t* ghost = nullptr, int ghost_w = 0, int ghost_h = 0,
                        const uint8_t* bg_mask = nullptr, int bg_mask_w = 0, int bg_mask_h = 0,
                        float bg_softness = 0.f) {
    int w, h, ch;
    uint8_t* pixels = stbi_load_from_memory(buf, (int)sz, &w, &h, &ch, 3);
    if (!pixels) return;

    bool do_corr_bleed = pfx && pfx->glitch_on &&
                         pfx->glitch_corruption >= 0.01f &&
                         pfx->glitch_corruption_bleed > 0.01f;
    bool bg_active = (bg_mask != nullptr && bg_mask_w > 0 && bg_mask_h > 0);
    bool want_rgba = (pfx && pfx->chroma_key_on) || do_corr_bleed || bg_active;

    static std::vector<uint8_t> s_corr_alpha;  // w*h alpha mask from corruption bleed

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
                s_corr_alpha.resize((size_t)w * h);
                cpu_apply_corruption(pixels, w, h, pfx->glitch_corruption, pfx->time,
                                     s_corr_alpha.data(), pfx->glitch_corruption_bleed);
            } else {
                cpu_apply_corruption(pixels, w, h, pfx->glitch_corruption, pfx->time);
            }
        }
        if (pfx->vhs_on && (pfx->vhs_noise >= 0.01f || pfx->vhs_bleed >= 0.1f || pfx->vhs_tracking >= 0.01f))
            cpu_apply_vhs(pixels, w, h, pfx->vhs_noise, pfx->vhs_bleed, pfx->vhs_tracking, pfx->time);
        if (pfx->datamosh_on && ghost && ghost_w == w && ghost_h == h)
            cpu_apply_datamosh(pixels, ghost, w, h,
                               pfx->datamosh_intensity, pfx->datamosh_decay,
                               pfx->datamosh_block_size,
                               pfx->datamosh_bleedback,
                               pfx->datamosh_t_in_clip,
                               pfx->datamosh_clip_duration);
    }

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

    if (want_rgba) {
        static std::vector<uint8_t> s_rgba;
        s_rgba.resize((size_t)w * h * 4);
        int n = w * h;

        bool ck = pfx && pfx->chroma_key_on;
        if (ck) {
            // Chroma key → RGBA; then multiply by corruption bleed alpha if both active
            cpu_apply_chroma_key(pixels, s_rgba.data(), w, h,
                                 pfx->chroma_key_r, pfx->chroma_key_g, pfx->chroma_key_b,
                                 pfx->chroma_key_threshold, pfx->chroma_key_softness);
            if (do_corr_bleed) {
                for (int i = 0; i < n; ++i)
                    s_rgba[i*4+3] = (uint8_t)((int)s_rgba[i*4+3] * (int)s_corr_alpha[i] / 255);
            }
        } else if (bg_active && bg_mask_w == w && bg_mask_h == h) {
            // BG remove: apply rembg alpha channel to source pixels
            for (int i = 0; i < n; ++i) {
                s_rgba[i*4+0] = pixels[i*3+0];
                s_rgba[i*4+1] = pixels[i*3+1];
                s_rgba[i*4+2] = pixels[i*3+2];
                float a = bg_mask[i] / 255.f;
                if (bg_softness > 0.01f) a = powf(a, 1.f + bg_softness * 3.f);
                s_rgba[i*4+3] = (uint8_t)(a * 255.f + 0.5f);
            }
        } else {
            // Corruption bleed only — copy RGB from processed pixels, alpha from mask
            for (int i = 0; i < n; ++i) {
                s_rgba[i*4+0] = pixels[i*3+0];
                s_rgba[i*4+1] = pixels[i*3+1];
                s_rgba[i*4+2] = pixels[i*3+2];
                s_rgba[i*4+3] = s_corr_alpha[i];
            }
        }

        if (size_changed || format_changed) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, s_rgba.data());
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                            GL_RGBA, GL_UNSIGNED_BYTE, s_rgba.data());
        }
    } else {
        if (size_changed || format_changed) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0,
                         GL_RGB, GL_UNSIGNED_BYTE, pixels);
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                            GL_RGB, GL_UNSIGNED_BYTE, pixels);
        }
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(pixels);

    *tex_w = w; *tex_h = h;
    if (tex_rgba) *tex_rgba = want_rgba;
}

// ── Internal: decode one proxy frame by index into a caller-supplied RGB buffer ─

static bool read_proxy_pixels(PreviewState& pv, int idx, std::vector<uint8_t>& out_rgb,
                               int& out_w, int& out_h) {
    int max_idx = (int)pv.proxy.offsets.size() - 1;
    idx = std::max(0, std::min(idx, max_idx));

    uint64_t offset = pv.proxy.offsets[(size_t)idx];
    size_t frame_sz = 0;
    if ((size_t)idx + 1 < pv.proxy.offsets.size()) {
        frame_sz = (size_t)(pv.proxy.offsets[(size_t)idx + 1] - offset);
    } else {
        fseeko(pv.mjpeg_file, (off_t)offset, SEEK_SET);
        long cur = ftell(pv.mjpeg_file); fseeko(pv.mjpeg_file, 0, SEEK_END);
        long end = ftell(pv.mjpeg_file);
        frame_sz = (end > cur) ? (size_t)(end - cur) : 0;
    }
    if (frame_sz == 0) return false;

    std::vector<uint8_t> buf(frame_sz);
    fseeko(pv.mjpeg_file, (off_t)offset, SEEK_SET);
    if (fread(buf.data(), 1, frame_sz, pv.mjpeg_file) == 0) return false;

    int w, h, ch;
    uint8_t* pixels = stbi_load_from_memory(buf.data(), (int)frame_sz, &w, &h, &ch, 3);
    if (!pixels) return false;
    out_w = w; out_h = h;
    out_rgb.assign(pixels, pixels + (size_t)w * h * 3);
    stbi_image_free(pixels);
    return true;
}

// ── Internal: seed ghost from a specific proxy frame index ───────────────────
// One JPEG decode + memcpy — used for cold init and seek re-seeding.

static void seed_ghost_from_frame(PreviewState& pv, int frame_idx) {
    if (pv.ghost_w <= 0 || pv.ghost_h <= 0) return;
    frame_idx = std::max(0, std::min(frame_idx, (int)pv.proxy.offsets.size() - 1));
    std::vector<uint8_t> px; int w = 0, h = 0;
    if (!read_proxy_pixels(pv, frame_idx, px, w, h)) return;
    if (w != pv.ghost_w || h != pv.ghost_h) return;
    memcpy(pv.ghost_buf.data(), px.data(), (size_t)w * h * 3);
    pv.ghost_seed_idx  = frame_idx;
    pv.ghost_frame_idx = frame_idx;
}

// ── Internal: seed ghost from clip anchor time ────────────────────────────────

static void seed_ghost_from_src_time(PreviewState& pv, float src_t) {
    if (pv.ghost_w <= 0 || pv.ghost_h <= 0) return;
    if (pv.proxy.offsets.empty() || !pv.mjpeg_file) return;
    int64_t num = pv.proxy.fps_num, den = pv.proxy.fps_den;
    int seed_idx = (num > 0 && den > 0)
        ? (int)((int64_t)((double)src_t * (double)num) / den)
        : (int)((double)src_t * pv.proxy.fps);
    seed_ghost_from_frame(pv, seed_idx);
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

// ── Internal: read one JPEG frame from a proxy at frame_idx ──────────────────

static uintptr_t decode_proxy_frame(PreviewState& pv, int frame_idx) {
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
    if (frame_sz == 0) return pv.tex ? (uintptr_t)pv.tex : 0;

    static std::vector<uint8_t> s_buf;
    s_buf.resize(frame_sz);
    fseeko(pv.mjpeg_file, (off_t)offset, SEEK_SET);
    size_t got = fread(s_buf.data(), 1, frame_sz, pv.mjpeg_file);
    if (got == 0) return pv.tex ? (uintptr_t)pv.tex : 0;

    if (pv.pixel_fx.datamosh_on) {
        // Size ghost on first decode (dimensions only known after first STB decode)
        if (pv.ghost_w == 0 && pv.tex_w > 0) {
            pv.ghost_w = pv.tex_w; pv.ghost_h = pv.tex_h;
            pv.ghost_buf.assign((size_t)pv.ghost_w * pv.ghost_h * 3, 0);
        }
        // Re-size if proxy dimensions changed
        if (pv.ghost_w != pv.tex_w || pv.ghost_h != pv.tex_h) {
            if (pv.tex_w > 0 && pv.tex_h > 0) {
                pv.ghost_w = pv.tex_w; pv.ghost_h = pv.tex_h;
                pv.ghost_buf.assign((size_t)pv.ghost_w * pv.ghost_h * 3, 0);
                pv.ghost_frame_idx = -1; pv.ghost_seed_idx = -1;
            }
        }

        if (pv.ghost_w > 0) {
            bool ghost_cold = pv.ghost_seed_idx < 0 ||
                              (pv.ghost_buf[0] == 0 && pv.ghost_buf[1] == 0 && pv.ghost_buf[2] == 0);
            if (ghost_cold) {
                seed_ghost_from_src_time(pv, pv.pixel_fx.datamosh_src_at_start);
            } else {
                int jump = frame_idx - pv.ghost_frame_idx;
                if (jump < 0) {
                    // Backward — unambiguously a scrub. Re-seed instantly.
                    seed_ghost_from_frame(pv, frame_idx - 1);
                } else if (jump > 1) {
                    // Forward skip of any size — fill sequentially. Bounded by real
                    // time: playhead can't advance faster than wall clock, so this
                    // never spins more than a handful of decodes per frame.
                    for (int f = pv.ghost_frame_idx + 1; f < frame_idx; ++f) {
                        std::vector<uint8_t> px; int fw = 0, fh = 0;
                        if (!read_proxy_pixels(pv, f, px, fw, fh)) break;
                        if (fw != pv.ghost_w || fh != pv.ghost_h) break;
                        cpu_apply_datamosh(px.data(), pv.ghost_buf.data(), fw, fh,
                                           pv.pixel_fx.datamosh_intensity,
                                           pv.pixel_fx.datamosh_decay,
                                           pv.pixel_fx.datamosh_block_size,
                                           pv.pixel_fx.datamosh_bleedback,
                                           pv.pixel_fx.datamosh_t_in_clip,
                                           pv.pixel_fx.datamosh_clip_duration);
                    }
                }
                // jump == 1 or jump == 0: falls through, upload_jpeg handles it.
            }
        }
    }

    uint8_t* ghost_ptr = (pv.pixel_fx.datamosh_on && pv.ghost_w > 0)
                         ? pv.ghost_buf.data() : nullptr;

    // Load bg_remove mask for this frame from the streaming mask MJPEG.
    if (pv.pixel_fx.bg_remove_on && !pv.pixel_fx.bg_remove_mask_dir.empty()) {
        const std::string& mdir = pv.pixel_fx.bg_remove_mask_dir;

        // (Re)open if mask dir changed.
        if (mdir != pv.bg_mjpeg_dir)
            bg_mjpeg_open(pv, mdir);
        else
            bg_mjpeg_scan(pv);  // pick up newly appended frames

        int mask_idx = frame_idx - pv.bg_mjpeg_start_frame;
        bool need    = (frame_idx != pv.bg_mask_frame);
        if (need) {
            pv.bg_mask_frame = frame_idx;
            pv.bg_mask_alpha.clear();
            pv.bg_mask_w = pv.bg_mask_h = 0;

            if (pv.bg_mjpeg_file && mask_idx >= 0 &&
                mask_idx < (int)pv.bg_mjpeg_offsets.size()) {
                uint64_t off     = pv.bg_mjpeg_offsets[(size_t)mask_idx];
                bool     is_last = ((size_t)mask_idx + 1 >= pv.bg_mjpeg_offsets.size());
                size_t   fsz     = 0;
                if (!is_last) {
                    fsz = (size_t)(pv.bg_mjpeg_offsets[(size_t)mask_idx + 1] - off);
                } else {
                    fseeko(pv.bg_mjpeg_file, 0, SEEK_END);
                    long end = ftell(pv.bg_mjpeg_file);
                    fsz = (end > (long)off) ? (size_t)(end - off) : 0;
                }
                if (fsz > 0) {
                    static std::vector<uint8_t> s_mbuf;
                    s_mbuf.resize(fsz);
                    fseeko(pv.bg_mjpeg_file, (off_t)off, SEEK_SET);
                    size_t got = fread(s_mbuf.data(), 1, fsz, pv.bg_mjpeg_file);
                    if (got > 0) {
                        int mw, mh, mc;
                        uint8_t* dec = stbi_load_from_memory(
                            s_mbuf.data(), (int)got, &mw, &mh, &mc, 1);
                        if (dec) {
                            pv.bg_mask_alpha.resize((size_t)mw * mh);
                            memcpy(pv.bg_mask_alpha.data(), dec, (size_t)mw * mh);
                            pv.bg_mask_w = mw; pv.bg_mask_h = mh;
                            stbi_image_free(dec);
                        }
                    }
                }
            }
        }
    } else if (!pv.pixel_fx.bg_remove_on) {
        if (pv.bg_mjpeg_file) { fclose(pv.bg_mjpeg_file); pv.bg_mjpeg_file = nullptr; }
        pv.bg_mjpeg_offsets.clear();
        pv.bg_mjpeg_dir.clear();
        pv.bg_mask_alpha.clear();
        pv.bg_mask_w = pv.bg_mask_h = 0;
    }

    const uint8_t* bg_ptr = (pv.pixel_fx.bg_remove_on && !pv.bg_mask_alpha.empty())
                            ? pv.bg_mask_alpha.data() : nullptr;

    upload_jpeg(&pv.tex, &pv.tex_w, &pv.tex_h, &pv.tex_rgba, s_buf.data(), got, &pv.pixel_fx,
                ghost_ptr, pv.ghost_w, pv.ghost_h,
                bg_ptr, pv.bg_mask_w, pv.bg_mask_h, pv.pixel_fx.bg_remove_softness);

    // Track which frame the ghost now represents after datamosh ran
    if (pv.pixel_fx.datamosh_on && pv.ghost_w > 0)
        pv.ghost_frame_idx = frame_idx;

    return pv.tex ? (uintptr_t)pv.tex : 0;
}

// ── Preview API ───────────────────────────────────────────────────────────────

static void close_slot(PreviewState& pv) {
    if (pv.mjpeg_file)    { fclose(pv.mjpeg_file);    pv.mjpeg_file    = nullptr; }
    if (pv.bg_mjpeg_file) { fclose(pv.bg_mjpeg_file); pv.bg_mjpeg_file = nullptr; }
    if (pv.tex)           { glDeleteTextures(1, &pv.tex); pv.tex = 0; }
    pv.tex_w = pv.tex_h = 0;
    pv.last_frame_idx = -1;
    pv.is_open = pv.is_proxy = false;
    pv.proxy = {}; pv.info = {};
    pv.bg_mjpeg_offsets.clear();
    pv.bg_mjpeg_dir.clear();
    pv.bg_mask_alpha.clear();
}

void video_open_still(int track_id, const std::string& jpeg_path) {
    if (track_id < 0 || track_id >= MAX_VIDEO_TRACKS) return;
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

    upload_jpeg(&g_pv[track_id].tex, &g_pv[track_id].tex_w, &g_pv[track_id].tex_h,
                &g_pv[track_id].tex_rgba, buf.data(), (size_t)sz);
    g_pv[track_id].is_open  = true;
    g_pv[track_id].is_proxy = false;
}

bool video_open_proxy(int track_id, const ProxyInfo& proxy) {
    if (track_id < 0 || track_id >= MAX_VIDEO_TRACKS) return false;
    close_slot(g_pv[track_id]);

    FILE* f = fopen(proxy.mjpeg_path.c_str(), "rb");
    if (!f) return false;

    PreviewState& pv = g_pv[track_id];
    pv.mjpeg_file      = f;
    pv.proxy           = proxy;
    pv.is_proxy        = true;
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
        for (int i = 0; i < MAX_VIDEO_TRACKS; ++i) close_slot(g_pv[i]);
        if (g_th.tex) { glDeleteTextures(1, &g_th.tex); g_th.tex = 0; }
        g_th.tex_w = g_th.tex_h = 0;
        g_th.last_frame_idx = -1;
    } else if (track_id >= 0 && track_id < MAX_VIDEO_TRACKS) {
        close_slot(g_pv[track_id]);
    }
}

bool      video_is_open(int track_id) {
    if (track_id < 0 || track_id >= MAX_VIDEO_TRACKS) return false;
    return g_pv[track_id].is_open;
}
VideoInfo video_info(int track_id) {
    if (track_id < 0 || track_id >= MAX_VIDEO_TRACKS) return {};
    return g_pv[track_id].info;
}

void video_set_pixel_fx(int track_id, const PixelFX& fx) {
    if (track_id < 0 || track_id >= MAX_VIDEO_TRACKS) return;
    auto& pv = g_pv[track_id];
    // Reset ghost when the datamosh clip changes (moved, replaced, or turned off/on).
    // This ensures mosh always starts fresh from the clip's start frame.
    if (fx.datamosh_clip_start != pv.ghost_clip_start) {
        pv.ghost_clip_start = fx.datamosh_clip_start;
        if (!pv.ghost_buf.empty())
            std::fill(pv.ghost_buf.begin(), pv.ghost_buf.end(), 0);
        pv.ghost_frame_idx = -1;
        pv.ghost_seed_idx  = -1;
    }
    // Animated effects (glitch/VHS) always re-decode since time advances each frame.
    // Static effects (grade/blur) re-decode only when params change.
    if (!(pv.pixel_fx == fx)) {
        pv.pixel_fx = fx;
        pv.pixel_fx_dirty = true;
    }
}

uintptr_t video_get_texture(int track_id, double playhead) {
    if (track_id < 0 || track_id >= MAX_VIDEO_TRACKS) return 0;
    PreviewState& pv = g_pv[track_id];
    if (!pv.is_open) return 0;

    if (!pv.is_proxy)
        return pv.tex ? (uintptr_t)pv.tex : 0;

    if (!pv.mjpeg_file || pv.proxy.offsets.empty()) return 0;

    double dur = pv.info.duration;
    if (playhead < 0.0) playhead = 0.0;
    if (dur > 0.0 && playhead > dur) playhead = dur;

    int64_t num = pv.proxy.fps_num;
    int64_t den = pv.proxy.fps_den;
    int frame_idx = (num > 0 && den > 0)
        ? (int)((int64_t)(playhead * (double)num) / den)
        : (int)(playhead * pv.proxy.fps);
    if (frame_idx >= (int)pv.proxy.offsets.size())
        frame_idx = (int)pv.proxy.offsets.size() - 1;
    if (frame_idx < 0) frame_idx = 0;

    if (frame_idx == pv.last_frame_idx && pv.tex && !pv.pixel_fx_dirty)
        return (uintptr_t)pv.tex;

    pv.pixel_fx_dirty = false;
    pv.last_frame_idx = frame_idx;
    return decode_proxy_frame(pv, frame_idx);
}

uintptr_t video_get_thumbnail(double t, int* out_w, int* out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    // Uses track 0's proxy for scrub bar hover preview
    PreviewState& pv = g_pv[0];
    if (!pv.is_open || !pv.is_proxy) return 0;
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
    if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) != 0) return 0.f;
    // find_stream_info is required for files where the container header doesn't
    // carry a reliable duration (e.g. some MP4/MKV variants).
    avformat_find_stream_info(fc, nullptr);
    float dur = (fc->duration != AV_NOPTS_VALUE)
        ? (float)fc->duration / (float)AV_TIME_BASE
        : 0.f;
    avformat_close_input(&fc);
    return dur;
}

// ── Export path — FFmpeg original file ───────────────────────────────────────

static struct ExportState {
    AVFormatContext* fmt_ctx    = nullptr;
    AVCodecContext*  codec_ctx  = nullptr;
    SwsContext*      sws        = nullptr;
    int              stream_idx = -1;
    VideoInfo        info       = {};
} g_ex;

bool video_open_export(const std::string& path) {
    video_close_export();

    if (avformat_open_input(&g_ex.fmt_ctx, path.c_str(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(g_ex.fmt_ctx, nullptr) < 0) {
        avformat_close_input(&g_ex.fmt_ctx); return false;
    }
    for (unsigned i = 0; i < g_ex.fmt_ctx->nb_streams; ++i) {
        if (g_ex.fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            g_ex.stream_idx = (int)i; break;
        }
    }
    if (g_ex.stream_idx < 0) { avformat_close_input(&g_ex.fmt_ctx); return false; }

    AVStream* st = g_ex.fmt_ctx->streams[g_ex.stream_idx];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    g_ex.codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(g_ex.codec_ctx, st->codecpar);
    avcodec_open2(g_ex.codec_ctx, codec, nullptr);

    g_ex.info.width    = g_ex.codec_ctx->width;
    g_ex.info.height   = g_ex.codec_ctx->height;
    g_ex.info.duration = (double)g_ex.fmt_ctx->duration / AV_TIME_BASE;
    g_ex.info.fps      = av_q2d(st->avg_frame_rate);

    g_ex.sws = sws_getContext(
        g_ex.info.width, g_ex.info.height, g_ex.codec_ctx->pix_fmt,
        g_ex.info.width, g_ex.info.height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    return true;
}

void video_close_export() {
    if (g_ex.sws)       { sws_freeContext(g_ex.sws);       g_ex.sws       = nullptr; }
    if (g_ex.codec_ctx) { avcodec_free_context(&g_ex.codec_ctx); }
    if (g_ex.fmt_ctx)   { avformat_close_input(&g_ex.fmt_ctx); }
    g_ex.stream_idx = -1;
    g_ex.info = {};
}

VideoFrame* video_decode_frame_at(double seconds) {
    if (!g_ex.fmt_ctx) return nullptr;

    int64_t ts = (int64_t)(seconds * AV_TIME_BASE);
    av_seek_frame(g_ex.fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(g_ex.codec_ctx);

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frm = av_frame_alloc();
    VideoFrame* result = nullptr;

    while (av_read_frame(g_ex.fmt_ctx, pkt) >= 0 && !result) {
        if (pkt->stream_index != g_ex.stream_idx) { av_packet_unref(pkt); continue; }
        avcodec_send_packet(g_ex.codec_ctx, pkt);
        av_packet_unref(pkt);
        while (avcodec_receive_frame(g_ex.codec_ctx, frm) == 0 && !result) {
            result = new VideoFrame();
            result->width  = g_ex.info.width;
            result->height = g_ex.info.height;
            result->data   = (uint8_t*)av_malloc(
                (size_t)result->width * result->height * 4 + 64);
            AVStream* st = g_ex.fmt_ctx->streams[g_ex.stream_idx];
            result->pts = frm->pts * av_q2d(st->time_base);
            uint8_t* dst[1] = { result->data };
            int      lsz[1] = { result->width * 4 };
            sws_scale(g_ex.sws,
                (const uint8_t* const*)frm->data, frm->linesize,
                0, frm->height, dst, lsz);
            av_frame_unref(frm);
        }
    }

    av_packet_free(&pkt);
    av_frame_free(&frm);
    return result;
}

// ── FX preview thumbnails ─────────────────────────────────────────────────────
// Source image: portrait_preview.h — 108×192 portrait RGB, embedded at build time
// from assets/imgs/portrait.jpg (center-cropped 9:16, Lanczos-scaled).
// Animated types (Glitch, VHS) regenerate every frame — 20,736 pixels, negligible.

#include "portrait_preview.h"

static const int FXP_W = portrait_preview_w;   // 108
static const int FXP_H = portrait_preview_h;   // 192
static const int FXP_N = 8;  // number of FXType enum values

struct FXPrev { GLuint tex = 0; bool animated = false; };
static std::array<FXPrev, FXP_N> s_fxp;
static std::vector<uint8_t> s_fxp_src;      // base source image (RGB)
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

uintptr_t video_fx_preview_texture(FXType ft, float t) {
    int idx = (int)ft;
    if (idx < 0 || idx >= FXP_N) return 0;

    if (s_fxp_src.empty()) fxp_make_sources();

    FXPrev& pv = s_fxp[idx];
    bool need = !pv.tex || pv.animated;
    if (!need) return (uintptr_t)pv.tex;

    std::vector<uint8_t> px;

    switch (ft) {
        case FXType::Adjustment: {
            px = s_fxp_src;
            cpu_apply_grade(px.data(), FXP_W, FXP_H, 0.06f, 1.45f, 1.7f, 0.f);
            break;
        }
        case FXType::ChromaKey: {
            // Show the key with right side keyed → dark checkerboard
            px.resize(FXP_W * FXP_H * 3);
            std::vector<uint8_t> rgba(FXP_W * FXP_H * 4);
            cpu_apply_chroma_key(s_fxp_src_ck.data(), rgba.data(),
                                 FXP_W, FXP_H, 0.f, 1.f, 0.f, 0.28f, 0.12f);
            for (int y = 0; y < FXP_H; ++y) {
                for (int x = 0; x < FXP_W; ++x) {
                    size_t i = ((size_t)y * FXP_W + x);
                    uint8_t a = rgba[i*4+3];
                    if (a < 200) {
                        // Checkerboard for transparent area
                        bool chk = ((x / 6) + (y / 6)) % 2 == 0;
                        px[i*3+0] = chk ? 80 : 50;
                        px[i*3+1] = chk ? 80 : 50;
                        px[i*3+2] = chk ? 80 : 50;
                    } else {
                        px[i*3+0] = rgba[i*4+0];
                        px[i*3+1] = rgba[i*4+1];
                        px[i*3+2] = rgba[i*4+2];
                    }
                }
            }
            break;
        }
        case FXType::Glitch: {
            px = s_fxp_src;
            cpu_apply_glitch(px.data(), FXP_W, FXP_H, 12.f, 0.7f, t);
            cpu_apply_corruption(px.data(), FXP_W, FXP_H, 0.55f, t);
            pv.animated = true;
            break;
        }
        case FXType::ZoomPunch: {
            // Static zoom-in crop — shows scale spike at peak
            px.resize(FXP_W * FXP_H * 3);
            float scale = 1.22f;
            float cx_f = FXP_W * 0.5f, cy_f = FXP_H * 0.5f;
            for (int y = 0; y < FXP_H; ++y) {
                for (int x = 0; x < FXP_W; ++x) {
                    int sx = (int)((x - cx_f) / scale + cx_f);
                    int sy = (int)((y - cy_f) / scale + cy_f);
                    sx = sx < 0 ? 0 : sx >= FXP_W ? FXP_W-1 : sx;
                    sy = sy < 0 ? 0 : sy >= FXP_H ? FXP_H-1 : sy;
                    size_t di = ((size_t)y*FXP_W+x)*3, si = ((size_t)sy*FXP_W+sx)*3;
                    px[di+0] = s_fxp_src[si+0];
                    px[di+1] = s_fxp_src[si+1];
                    px[di+2] = s_fxp_src[si+2];
                }
            }
            break;
        }
        case FXType::LUT: {
            // Simulate a teal-orange cinematic grade — warm shadows, cool highlights
            px = s_fxp_src;
            cpu_apply_grade(px.data(), FXP_W, FXP_H, -0.04f, 1.25f, 0.75f, 18.f);
            break;
        }
        case FXType::LightLeak: {
            // Source + warm additive flare blob upper-right
            px = s_fxp_src;
            for (int y = 0; y < FXP_H; ++y) {
                for (int x = 0; x < FXP_W; ++x) {
                    float dx = ((float)x - FXP_W * 0.78f) / (FXP_W * 0.35f);
                    float dy = ((float)y - FXP_H * 0.18f) / (FXP_H * 0.45f);
                    float r2  = dx*dx + dy*dy;
                    float flr = fmaxf(0.f, 1.f - r2) * 0.82f;
                    size_t i  = ((size_t)y*FXP_W+x)*3;
                    px[i+0] = cu8((int)(px[i+0] + 255.f * flr * 0.88f));
                    px[i+1] = cu8((int)(px[i+1] + 255.f * flr * 0.35f));
                    px[i+2] = cu8((int)(px[i+2] + 255.f * flr * 0.05f));
                }
            }
            break;
        }
        case FXType::VHS: {
            px = s_fxp_src;
            cpu_apply_vhs(px.data(), FXP_W, FXP_H, 0.65f, 9.f, 0.55f, t);
            pv.animated = true;
            break;
        }
        case FXType::Datamosh: {
            // Ghost = source shifted right; mosh against current source
            std::vector<uint8_t> ghost(FXP_W * FXP_H * 3);
            for (int y = 0; y < FXP_H; ++y) {
                for (int x = 0; x < FXP_W; ++x) {
                    int gx = (x + 10) % FXP_W;
                    size_t di = ((size_t)y*FXP_W+x)*3, si = ((size_t)y*FXP_W+gx)*3;
                    ghost[di+0] = s_fxp_src[si+0];
                    ghost[di+1] = s_fxp_src[si+1];
                    ghost[di+2] = s_fxp_src[si+2];
                }
            }
            px = s_fxp_src;
            cpu_apply_datamosh(px.data(), ghost.data(), FXP_W, FXP_H,
                               0.85f, 0.08f, 8, 0.f, 0.f, 0.f);
            break;
        }
        default: px = s_fxp_src; break;
    }

    fxp_upload(pv, px);
    return (uintptr_t)pv.tex;
}

// ── Adjustment preset preview textures ───────────────────────────────────────
// Keyed by unique_id in a small fixed-size cache. LRU eviction not needed —
// the preset list is static at runtime; we just keep one texture per preset.

static const int ADJ_PREV_MAX = 64;
struct AdjPrev { int id = -1; GLuint tex = 0; };
static std::array<AdjPrev, ADJ_PREV_MAX> s_adj_prev;
static int s_adj_prev_next = 0;

uintptr_t video_adj_preview_texture(int unique_id,
                                     float brightness, float contrast,
                                     float saturation, float hue,
                                     float blur, float vignette) {
    if (s_fxp_src.empty()) fxp_make_sources();

    // Find existing slot
    for (auto& ap : s_adj_prev) {
        if (ap.id == unique_id) return (uintptr_t)ap.tex;
    }

    // Claim next slot (ring)
    AdjPrev& ap = s_adj_prev[s_adj_prev_next % ADJ_PREV_MAX];
    s_adj_prev_next++;
    ap.id = unique_id;

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

    FXPrev tmp;
    fxp_upload(tmp, px);
    ap.tex = tmp.tex;
    return (uintptr_t)ap.tex;
}
