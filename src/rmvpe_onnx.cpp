// rmvpe_onnx.cpp — RMVPE pitch tracking: log-mel front end (FFTW) + ONNX
// salience model + local-average-cents decode. Mirrors RVC's rmvpe.py:
//   MelSpectrogram(128 mels, sr 16000, win/n_fft 1024, hop 160, fmin 30,
//                  fmax 8000, slaney filterbank+norm, log(clamp(., 1e-5)))
//   mel frames padded to a multiple of 32 for the U-Net, cropped after.
//   decode: argmax + 9-bin weighted average of cents, threshold 0.03.
#include "rmvpe_onnx.h"
#include "paths.h"
#include <onnxruntime_cxx_api.h>
#include <fftw3.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

namespace fs = std::filesystem;

static constexpr int kSR    = 16000;
static constexpr int kNFFT  = 1024;
static constexpr int kHop   = 160;
static constexpr int kMels  = 128;
static constexpr int kBins  = kNFFT / 2 + 1;   // 513
static constexpr float kFMin = 30.f;
static constexpr float kFMax = 8000.f;

std::string rmvpe_onnx_path() { return app_models_dir() + "/rmvpe.onnx"; }
bool        rmvpe_onnx_exists() { return fs::exists(rmvpe_onnx_path()); }

// ── Slaney mel scale (librosa htk=False) ─────────────────────────────────────

static float hz_to_mel(float hz) {
    constexpr float f_sp = 200.f / 3.f;
    if (hz < 1000.f) return hz / f_sp;
    const float logstep = std::log(6.4f) / 27.f;
    return 15.f + std::log(hz / 1000.f) / logstep;
}
static float mel_to_hz(float mel) {
    constexpr float f_sp = 200.f / 3.f;
    if (mel < 15.f) return mel * f_sp;
    const float logstep = std::log(6.4f) / 27.f;
    return 1000.f * std::exp(logstep * (mel - 15.f));
}

// Mel filterbank [kMels][kBins], slaney-normalised triangles.
static std::vector<float> mel_filterbank() {
    std::vector<float> fb((size_t)kMels * kBins, 0.f);
    float mel_lo = hz_to_mel(kFMin), mel_hi = hz_to_mel(kFMax);
    std::vector<float> pts(kMels + 2);
    for (int i = 0; i < kMels + 2; i++)
        pts[(size_t)i] = mel_to_hz(mel_lo + (mel_hi - mel_lo) * i / (kMels + 1));
    for (int m = 0; m < kMels; m++) {
        float f0 = pts[(size_t)m], f1 = pts[(size_t)m+1], f2 = pts[(size_t)m+2];
        float norm = 2.f / (f2 - f0);
        for (int k = 0; k < kBins; k++) {
            float fk = (float)k * kSR / kNFFT;
            float up = (f1 > f0) ? (fk - f0) / (f1 - f0) : 0.f;
            float dn = (f2 > f1) ? (f2 - fk) / (f2 - f1) : 0.f;
            float w  = std::fmin(up, dn);
            if (w > 0.f) fb[(size_t)m * kBins + k] = w * norm;
        }
    }
    return fb;
}

// ── F0 estimation ─────────────────────────────────────────────────────────────

std::vector<float> rmvpe_f0(const std::vector<float>& wav16k)
{
    if (!rmvpe_onnx_exists() || wav16k.empty()) return {};

    // Reflect-pad by n_fft/2 each side (torch.stft center=True equivalent)
    int N = (int)wav16k.size();
    int half = kNFFT / 2;
    std::vector<float> padded((size_t)N + kNFFT);
    for (int i = 0; i < half; i++)
        padded[(size_t)i] = wav16k[(size_t)std::min(half - i, N - 1)];
    std::memcpy(padded.data() + half, wav16k.data(), (size_t)N * sizeof(float));
    for (int i = 0; i < half; i++)
        padded[(size_t)(half + N + i)] =
            wav16k[(size_t)std::max(0, N - 2 - i)];

    int T = ((int)padded.size() - kNFFT) / kHop + 1;
    if (T <= 0) return {};

    // Periodic Hann window
    std::vector<float> win(kNFFT);
    for (int i = 0; i < kNFFT; i++)
        win[(size_t)i] = 0.5f * (1.f - std::cos(2.f * (float)M_PI * i / kNFFT));

    // STFT magnitudes → log-mel [128, T]
    static std::mutex fftw_mu;   // fftw plan creation is not thread-safe
    std::vector<float> frame(kNFFT);
    std::vector<fftwf_complex> spec_c(kBins);
    fftwf_plan plan;
    {
        std::lock_guard<std::mutex> lk(fftw_mu);
        plan = fftwf_plan_dft_r2c_1d(kNFFT, frame.data(), spec_c.data(),
                                     FFTW_ESTIMATE);
    }

    auto fb = mel_filterbank();
    std::vector<float> logmel((size_t)kMels * T);
    std::vector<float> mag(kBins);
    for (int t = 0; t < T; t++) {
        const float* src = padded.data() + (size_t)t * kHop;
        for (int i = 0; i < kNFFT; i++) frame[(size_t)i] = src[i] * win[(size_t)i];
        fftwf_execute(plan);
        for (int k = 0; k < kBins; k++)
            mag[(size_t)k] = std::sqrt(spec_c[(size_t)k][0] * spec_c[(size_t)k][0] +
                                       spec_c[(size_t)k][1] * spec_c[(size_t)k][1]);
        for (int m = 0; m < kMels; m++) {
            const float* w = fb.data() + (size_t)m * kBins;
            double acc = 0.0;
            for (int k = 0; k < kBins; k++) acc += (double)w[k] * mag[(size_t)k];
            logmel[(size_t)m * T + t] = std::log(std::fmax((float)acc, 1e-5f));
        }
    }
    {
        std::lock_guard<std::mutex> lk(fftw_mu);
        fftwf_destroy_plan(plan);
    }

    // Pad frame count to a multiple of 32
    int T_pad = 32 * ((T - 1) / 32 + 1);
    std::vector<float> mel_in((size_t)kMels * T_pad, std::log(1e-5f));
    for (int m = 0; m < kMels; m++)
        std::memcpy(mel_in.data() + (size_t)m * T_pad,
                    logmel.data() + (size_t)m * T, (size_t)T * sizeof(float));

    // ── ONNX inference ────────────────────────────────────────────────────────
    try {
        Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "rmvpe");
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(4);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        Ort::Session sess(env, rmvpe_onnx_path().c_str(), opts);

        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,
                                                         OrtMemTypeDefault);
        std::vector<int64_t> shape = {1, kMels, T_pad};
        Ort::Value in = Ort::Value::CreateTensor<float>(
            mem, mel_in.data(), mel_in.size(), shape.data(), 3);

        Ort::AllocatorWithDefaultOptions alloc;
        std::string in_name  = sess.GetInputNameAllocated(0, alloc).get();
        std::string out_name = sess.GetOutputNameAllocated(0, alloc).get();
        const char* ins[]  = {in_name.c_str()};
        const char* outs[] = {out_name.c_str()};
        auto out = sess.Run(Ort::RunOptions{nullptr}, ins, &in, 1, outs, 1);

        auto oshape = out[0].GetTensorTypeAndShapeInfo().GetShape(); // [1,Tp,360]
        const float* sal = out[0].GetTensorData<float>();
        int n_bins = (int)oshape[2];   // 360

        // ── Decode: argmax + 9-bin weighted cents average ─────────────────────
        std::vector<float> f0((size_t)T, 0.f);
        for (int t = 0; t < T; t++) {
            const float* row = sal + (size_t)t * n_bins;
            int   best = 0;
            float maxv = row[0];
            for (int b = 1; b < n_bins; b++)
                if (row[b] > maxv) { maxv = row[b]; best = b; }
            if (maxv <= 0.03f) continue;
            double ws = 0.0, cs = 0.0;
            for (int d = -4; d <= 4; d++) {
                int b = best + d;
                if (b < 0 || b >= n_bins) continue;
                float cents = 20.f * b + 1997.3794084376191f;
                ws += (double)row[b];
                cs += (double)row[b] * cents;
            }
            if (ws > 0.0)
                f0[(size_t)t] = 10.f * std::pow(2.f, (float)(cs / ws) / 1200.f);
        }
        return f0;
    } catch (...) {
        return {};
    }
}
