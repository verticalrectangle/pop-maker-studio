#pragma once
// loudness.h — ITU-R BS.1770-4 loudness measurement (LUFS) + sample peak.
//
// One instance per measured stream: the live master mix feeds g_master_meter
// (audio.cpp output callback), the export mix feeds its own instance so the
// export report measures exactly the samples that land in the file.
//
// process() is called from the audio/export thread with the FINAL interleaved
// stereo f32 buffer; the UI reads a consistent snapshot via snapshot(). All
// public entry points are internally locked (the lock is held for microseconds
// per callback — same pattern as g_clip_mutex).
//
// Measures:
//   momentary LUFS   — 400 ms window, updated every 100 ms hop
//   integrated LUFS  — gated per BS.1770 (-70 LUFS absolute, -10 LU relative)
//   sample peak dBFS — per channel, with a decaying bar and a 2 s clip hold
//
// "Peak" here is SAMPLE peak, not oversampled true peak — labeled accordingly
// in the UI (true peak needs 4x oversampling; not worth it for v1).

#include <cstddef>
#include <mutex>
#include <vector>

struct LoudnessSnapshot {
    float momentary_lufs  = -144.f;  // 400 ms window
    float integrated_lufs = -144.f;  // gated program loudness since reset
    float peak_l          = 0.f;     // linear |peak|, decaying display value
    float peak_r          = 0.f;
    bool  clipped         = false;   // any sample >= ~0 dBFS within the hold window
    double measured_secs  = 0.0;     // how much audio the integrated value covers
};

class LoudnessMeter {
public:
    void reset(float sample_rate = 44100.f);
    // Interleaved stereo frames from the FINAL mix. Audio-thread safe.
    void process(const float* lr, size_t frames);
    LoudnessSnapshot snapshot() const;

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;
        inline float run(float x) {
            double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return (float)y;
        }
    };

    mutable std::mutex mtx_;
    float  fs_ = 44100.f;
    Biquad shelf_[2], hp_[2];        // K-weighting: high shelf + RLB high-pass, per channel

    // 400 ms energy blocks on a 100 ms hop: momentary = sum of the last 4 hops;
    // integrated gating runs on the same block series (75% overlap per spec).
    size_t hop_len_ = 4410, hop_fill_ = 0;
    double hop_sum_ = 0.0;           // K-weighted mean-square accumulator (L+R)
    double hops_[4] = {0, 0, 0, 0};  // last four 100 ms hop energies
    int    hop_count_ = 0;
    std::vector<float> block_lufs_;  // per-400ms-block loudness for gating
    std::vector<double> block_ms_;   // matching mean-square energies

    float  momentary_ = -144.f;
    float  integrated_ = -144.f;
    float  peak_l_ = 0.f, peak_r_ = 0.f;
    double clip_hold_ = 0.0;         // seconds of audio since last clip
    bool   clipped_ = false;
    double total_frames_ = 0.0;

    void design_filters();
    void finish_hop();
};
