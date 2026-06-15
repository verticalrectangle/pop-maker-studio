#include "av_sync.h"
#include <complex>
#include <vector>
#include <cmath>
#include <algorithm>

namespace {

using cf = std::complex<float>;

// In-place iterative radix-2 Cooley-Tukey FFT. n must be a power of two.
// inv=false → forward, inv=true → inverse (NOT normalized; caller divides by n).
void fft(std::vector<cf>& a, bool inv) {
    const size_t n = a.size();
    // Bit-reversal permutation.
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        float ang = (inv ? 2.f : -2.f) * 3.14159265358979323846f / (float)len;
        cf wlen(cosf(ang), sinf(ang));
        for (size_t i = 0; i < n; i += len) {
            cf w(1.f, 0.f);
            for (size_t k = 0; k < len / 2; ++k) {
                cf u = a[i + k];
                cf v = a[i + k + len / 2] * w;
                a[i + k]             = u + v;
                a[i + k + len / 2]   = u - v;
                w *= wlen;
            }
        }
    }
}

size_t next_pow2(size_t v) {
    size_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

// Mean-subtract a window into out[0..n) (zero-padded beyond the source length).
void load_centered(std::vector<cf>& out, const float* s, size_t ns, size_t n) {
    double mean = 0.0;
    for (size_t i = 0; i < ns; ++i) mean += s[i];
    if (ns) mean /= (double)ns;
    out.assign(n, cf(0.f, 0.f));
    for (size_t i = 0; i < ns && i < n; ++i)
        out[i] = cf((float)((double)s[i] - mean), 0.f);
}

} // namespace

float av_estimate_offset_seconds(const float* a, size_t na,
                                 const float* b, size_t nb,
                                 int sample_rate, float max_lag_s) {
    if (!a || !b || na < 8 || nb < 8 || sample_rate <= 0) return 0.f;

    // Pad to a power of two ≥ na+nb so the circular correlation behaves like a
    // linear one across the lag window we care about.
    const size_t n = next_pow2(na + nb);

    std::vector<cf> A, B;
    load_centered(A, a, na, n);
    load_centered(B, b, nb, n);
    fft(A, false);
    fft(B, false);

    // Cross-power spectrum with phase transform: R = A·conj(B) / |A·conj(B)|.
    std::vector<cf> R(n);
    for (size_t k = 0; k < n; ++k) {
        cf x = A[k] * std::conj(B[k]);
        float m = std::abs(x);
        R[k] = (m > 1e-9f) ? x / m : cf(0.f, 0.f);
    }
    fft(R, true);  // inverse → correlation (unnormalized; magnitude is all we need)

    // Correlation r[tau]: tau in [0,n). tau > n/2 maps to the negative lag tau-n.
    // r[tau] peaks when b shifted by tau aligns with a → b lags a by tau samples.
    const long max_lag = std::min((long)(max_lag_s * (float)sample_rate),
                                  (long)(n / 2 - 1));
    long best = 0;
    float best_mag = -1.f;
    for (long tau = -max_lag; tau <= max_lag; ++tau) {
        size_t idx = (tau >= 0) ? (size_t)tau : (size_t)(tau + (long)n);
        float mag = std::abs(R[idx]);
        if (mag > best_mag) { best_mag = mag; best = tau; }
    }

    // Parabolic interpolation around the integer peak for sub-sample accuracy.
    auto at = [&](long tau) -> float {
        size_t idx = (tau >= 0) ? (size_t)tau : (size_t)(tau + (long)n);
        return std::abs(R[idx]);
    };
    float refine = 0.f;
    if (best > -max_lag && best < max_lag) {
        float ym1 = at(best - 1), y0 = at(best), yp1 = at(best + 1);
        float denom = (ym1 - 2.f * y0 + yp1);
        if (std::fabs(denom) > 1e-9f)
            refine = 0.5f * (ym1 - yp1) / denom;   // ∈ (-0.5, 0.5)
    }
    // R = A·conj(B) peaks at tau = -(b's delay vs a); negate so a positive
    // result means "b lags a" (the documented convention).
    return -((float)best + refine) / (float)sample_rate;
}
