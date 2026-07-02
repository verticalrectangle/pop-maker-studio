#include "loudness.h"
#include <cmath>
#include <cstring>

// ── K-weighting filter design ─────────────────────────────────────────────────
// BS.1770 specifies coefficients at 48 kHz; the app runs at 44.1 kHz, so both
// stages are re-derived from their analog prototypes at the actual rate
// (standard practice — e.g. libebur128 does the same):
//   stage 1: high shelf  f0 = 1681.974 Hz, gain +3.99984 dB, Q = 0.7071752
//   stage 2: RLB high-pass f0 = 38.13547 Hz, Q = 0.5003270
// RBJ audio-EQ-cookbook forms.

static void shelf_coeffs(double fs, double& b0, double& b1, double& b2,
                         double& a1, double& a2) {
    const double f0 = 1681.9744509555319, G = 3.999843853973347, Q = 0.7071752369554196;
    double K  = std::tan(M_PI * f0 / fs);
    double Vh = std::pow(10.0, G / 20.0);
    double Vb = std::pow(Vh, 0.4996667741545416);
    double a0 = 1.0 + K / Q + K * K;
    b0 = (Vh + Vb * K / Q + K * K) / a0;
    b1 = 2.0 * (K * K - Vh) / a0;
    b2 = (Vh - Vb * K / Q + K * K) / a0;
    a1 = 2.0 * (K * K - 1.0) / a0;
    a2 = (1.0 - K / Q + K * K) / a0;
}

static void highpass_coeffs(double fs, double& b0, double& b1, double& b2,
                            double& a1, double& a2) {
    const double f0 = 38.13547087602444, Q = 0.5003270373238773;
    double K  = std::tan(M_PI * f0 / fs);
    double a0 = 1.0 + K / Q + K * K;
    b0 = 1.0 / a0;
    b1 = -2.0 / a0;
    b2 = 1.0 / a0;
    a1 = 2.0 * (K * K - 1.0) / a0;
    a2 = (1.0 - K / Q + K * K) / a0;
}

void LoudnessMeter::design_filters() {
    double b0, b1, b2, a1, a2;
    shelf_coeffs(fs_, b0, b1, b2, a1, a2);
    for (auto& f : shelf_) { f = {}; f.b0 = b0; f.b1 = b1; f.b2 = b2; f.a1 = a1; f.a2 = a2; }
    highpass_coeffs(fs_, b0, b1, b2, a1, a2);
    for (auto& f : hp_)    { f = {}; f.b0 = b0; f.b1 = b1; f.b2 = b2; f.a1 = a1; f.a2 = a2; }
}

void LoudnessMeter::reset(float sample_rate) {
    std::lock_guard<std::mutex> lk(mtx_);
    fs_       = sample_rate > 0.f ? sample_rate : 44100.f;
    hop_len_  = (size_t)(fs_ * 0.1f + 0.5f);
    hop_fill_ = 0;
    hop_sum_  = 0.0;
    hops_[0] = hops_[1] = hops_[2] = hops_[3] = 0.0;
    hop_count_ = 0;
    block_lufs_.clear();
    block_ms_.clear();
    momentary_ = integrated_ = -144.f;
    peak_l_ = peak_r_ = 0.f;
    clip_hold_ = 0.0;
    clipped_ = false;
    total_frames_ = 0.0;
    design_filters();
}

void LoudnessMeter::finish_hop() {
    // Shift in this hop's mean-square; a 400 ms block = the last 4 hops.
    hops_[0] = hops_[1]; hops_[1] = hops_[2]; hops_[2] = hops_[3];
    hops_[3] = hop_sum_ / (double)hop_len_;
    hop_sum_ = 0.0;
    ++hop_count_;
    if (hop_count_ < 4) return;

    double ms = (hops_[0] + hops_[1] + hops_[2] + hops_[3]) * 0.25;
    float lufs = ms > 1e-12 ? (float)(-0.691 + 10.0 * std::log10(ms)) : -144.f;
    momentary_ = lufs;

    // Gated integration per BS.1770: keep blocks above the -70 LUFS absolute
    // gate, then average the ones within 10 LU of that population's mean.
    if (lufs > -70.f) {
        block_lufs_.push_back(lufs);
        block_ms_.push_back(ms);
        double sum = 0.0;
        for (double m : block_ms_) sum += m;
        double mean_ms   = sum / (double)block_ms_.size();
        double rel_gate  = -0.691 + 10.0 * std::log10(mean_ms) - 10.0;
        double gsum = 0.0; int gn = 0;
        for (size_t i = 0; i < block_ms_.size(); ++i)
            if (block_lufs_[i] > rel_gate) { gsum += block_ms_[i]; ++gn; }
        if (gn > 0)
            integrated_ = (float)(-0.691 + 10.0 * std::log10(gsum / (double)gn));
    }
}

void LoudnessMeter::process(const float* lr, size_t frames) {
    if (!lr || frames == 0) return;
    std::lock_guard<std::mutex> lk(mtx_);
    const float decay = std::pow(0.3f, (float)frames / fs_);  // peak bar falls ~10 dB/s
    peak_l_ *= decay;
    peak_r_ *= decay;
    for (size_t i = 0; i < frames; ++i) {
        float l = lr[i * 2], r = lr[i * 2 + 1];
        float al = std::fabs(l), ar = std::fabs(r);
        if (al > peak_l_) peak_l_ = al;
        if (ar > peak_r_) peak_r_ = ar;
        if (al >= 0.999f || ar >= 0.999f) { clipped_ = true; clip_hold_ = 0.0; }
        float kl = hp_[0].run(shelf_[0].run(l));
        float kr = hp_[1].run(shelf_[1].run(r));
        hop_sum_ += (double)kl * kl + (double)kr * kr;
        if (++hop_fill_ >= hop_len_) { hop_fill_ = 0; finish_hop(); }
    }
    total_frames_ += (double)frames;
    clip_hold_ += (double)frames / fs_;
    if (clip_hold_ > 2.0) clipped_ = false;   // 2 s clip-light hold
}

LoudnessSnapshot LoudnessMeter::snapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    LoudnessSnapshot s;
    s.momentary_lufs  = momentary_;
    s.integrated_lufs = integrated_;
    s.peak_l          = peak_l_;
    s.peak_r          = peak_r_;
    s.clipped         = clipped_;
    s.measured_secs   = total_frames_ / (double)fs_;
    return s;
}
