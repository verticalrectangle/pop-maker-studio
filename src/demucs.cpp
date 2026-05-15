// HTDemucs stem separation — pure C++ ONNX Runtime inference.
// Pipeline: ffmpeg decode → Cooley-Tukey STFT → ONNX (HTDemucs) → iSTFT → ffmpeg encode.
// No Python required. Model auto-downloaded on first use.
#include "demucs.h"
#include <onnxruntime_cxx_api.h>
#include <filesystem>
#include <cmath>
#include <complex>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;
using cx = std::complex<float>;

// ── Constants ─────────────────────────────────────────────────────────────────

static const char* kModelFile = "htdemucs.onnx";
static const char* kModelUrl  =
    "https://huggingface.co/MrCitron/demucs-v4-onnx/resolve/main/htdemucs.onnx";

static constexpr int   kNFFT     = 4096;
static constexpr int   kHop      = 1024;
static constexpr int   kFreqBins = kNFFT / 2 + 1;   // 2049
static constexpr int   kRate     = 44100;
static constexpr int   kNumStem  = 4;
static constexpr int   kVocals   = 3;   // drums=0 bass=1 other=2 vocals=3
// Segment length and overlap for overlap-add processing.
// 10 s with 1 s fade zone on each side gives a 8 s effective stride.
static constexpr int   kSegLen   = 441000;
static constexpr int   kOverlap  = 44100;
static constexpr int   kStride   = kSegLen - 2 * kOverlap;   // 352800

// ── Model path ────────────────────────────────────────────────────────────────

std::string demucs_model_path() {
    const char* h = getenv("HOME");
    if (!h) return {};
    return std::string(h) + "/.cache/pop-maker-studio/demucs/" + kModelFile;
}
bool demucs_model_exists() { return fs::exists(demucs_model_path()); }

// ── Download ──────────────────────────────────────────────────────────────────

std::string demucs_download(
    std::function<void(float, const std::string&)> on_progress)
{
    if (demucs_model_exists()) return {};

    fs::path mp = demucs_model_path();
    std::error_code ec;
    fs::create_directories(mp.parent_path(), ec);

    if (on_progress) on_progress(0.01f, "Downloading HTDemucs model (~289 MB)…");

    std::string tmp = mp.string() + ".part";
    std::string cmd = "curl -fsSL -o \"" + tmp + "\" \"" + kModelUrl + "\"";
    if (system(cmd.c_str()) != 0 || !fs::exists(tmp)) {  // NOLINT
        fs::remove(tmp, ec);
        return "Failed to download HTDemucs model";
    }
    fs::rename(tmp, mp, ec);
    if (ec) return "Failed to install model: " + ec.message();

    if (on_progress) on_progress(1.f, "Model ready");
    return {};
}

// ── Hann window (periodic) ────────────────────────────────────────────────────

static std::vector<float> make_hann() {
    std::vector<float> w(kNFFT);
    for (int i = 0; i < kNFFT; i++)
        w[i] = 0.5f * (1.f - cosf(2.f * float(M_PI) * i / kNFFT));
    return w;
}

// ── Cooley-Tukey radix-2 DIT FFT ─────────────────────────────────────────────

static void fft(std::vector<cx>& a, bool inv) {
    int n = (int)a.size();
    // Bit-reversal permutation
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    // Butterfly stages
    for (int len = 2; len <= n; len <<= 1) {
        float ang  = 2.f * float(M_PI) / len * (inv ? 1.f : -1.f);
        cx    wlen = {cosf(ang), sinf(ang)};
        for (int i = 0; i < n; i += len) {
            cx w = {1.f, 0.f};
            for (int j = 0; j < len / 2; j++) {
                cx u = a[i + j], v = a[i + j + len/2] * w;
                a[i + j]          = u + v;
                a[i + j + len/2]  = u - v;
                w *= wlen;
            }
        }
    }
    if (inv)
        for (auto& x : a) x /= float(n);
}

// ── STFT (mono) ───────────────────────────────────────────────────────────────
// Center-padded STFT matching PyTorch torch.stft(center=True).
// Outputs re / im as [kFreqBins × T] in frequency-major (row = freq, col = time).
// Returns T (frame count).

static int stft(const float* x, int N, const std::vector<float>& win,
                std::vector<float>& re, std::vector<float>& im)
{
    int pad = kNFFT / 2;
    // Reflect-pad: left pad from x[1..pad], right pad from x[N-2..N-1-pad]
    std::vector<float> px(N + 2 * pad, 0.f);
    for (int i = 0; i < N; i++) px[pad + i] = x[i];
    for (int i = 1; i <= pad && i < N; i++) {
        px[pad - i]          = x[i];
        px[pad + N - 1 + i]  = x[N - 1 - i];
    }

    int T = ((int)px.size() - kNFFT) / kHop + 1;
    re.assign((size_t)kFreqBins * T, 0.f);
    im.assign((size_t)kFreqBins * T, 0.f);

    std::vector<cx> frame(kNFFT);
    for (int t = 0; t < T; t++) {
        int s = t * kHop;
        for (int k = 0; k < kNFFT; k++) frame[k] = {px[s + k] * win[k], 0.f};
        fft(frame, false);
        for (int k = 0; k < kFreqBins; k++) {
            re[(size_t)k * T + t] = frame[k].real();
            im[(size_t)k * T + t] = frame[k].imag();
        }
    }
    return T;
}

// ── iSTFT (mono) ──────────────────────────────────────────────────────────────
// re/im: [kFreqBins × T] frequency-major.
// Returns depadded signal of length N.

static std::vector<float> istft(const float* re, const float* im,
                                int T, int N,
                                const std::vector<float>& win)
{
    int pad  = kNFFT / 2;
    int padN = N + 2 * pad;
    std::vector<float> out(padN, 0.f), nrm(padN, 0.f);

    std::vector<cx> frame(kNFFT);
    for (int t = 0; t < T; t++) {
        for (int k = 0; k < kFreqBins; k++)
            frame[k] = {re[(size_t)k * T + t], im[(size_t)k * T + t]};
        // Conjugate-symmetric mirror for real signal
        for (int k = 1; k < kNFFT / 2; k++)
            frame[kNFFT - k] = std::conj(frame[k]);
        frame[kNFFT / 2] = {frame[kNFFT / 2].real(), 0.f};

        fft(frame, true);   // IFFT normalises by kNFFT

        int s = t * kHop;
        for (int k = 0; k < kNFFT; k++) {
            out[s + k] += frame[k].real() * win[k];
            nrm[s + k] += win[k] * win[k];
        }
    }
    for (int i = 0; i < padN; i++)
        if (nrm[i] > 1e-8f) out[i] /= nrm[i];

    return {out.begin() + pad, out.begin() + pad + N};
}

// ── Audio I/O (via ffmpeg) ────────────────────────────────────────────────────

// Returns interleaved L R L R float32 at kRate Hz. Sets n to per-channel count.
static std::vector<float> read_stereo(const std::string& p, int& n) {
    std::string cmd = "ffmpeg -hide_banner -loglevel error"
                      " -i \"" + p + "\""
                      " -vn -ar " + std::to_string(kRate) +
                      " -ac 2 -f f32le pipe:1 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) { n = 0; return {}; }
    std::vector<float> buf;
    float tmp[4096]; size_t r;
    while ((r = fread(tmp, sizeof(float), 4096, fp)) > 0)
        buf.insert(buf.end(), tmp, tmp + r);
    pclose(fp);
    n = (int)(buf.size() / 2);
    return buf;
}

static bool write_stereo(const std::string& p, const std::vector<float>& d) {
    std::string cmd = "ffmpeg -hide_banner -loglevel error -y"
                      " -f f32le -ar " + std::to_string(kRate) +
                      " -ac 2 -i pipe:0"
                      " \"" + p + "\" 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "w");
    if (!fp) return false;
    fwrite(d.data(), sizeof(float), d.size(), fp);
    return pclose(fp) == 0;
}

// ── Main separation ───────────────────────────────────────────────────────────

std::string demucs_separate(
    const std::string& input_path,
    const std::string& vocals_out,
    const std::string& instrumental_out,
    std::function<void(float, const std::string&)> on_progress)
{
    auto prog = [&](float p, const std::string& m) {
        if (on_progress) on_progress(p, m);
    };

    // ── Download model if needed ──────────────────────────────────────────────
    if (!demucs_model_exists()) {
        std::string err = demucs_download([&](float p, const std::string& m) {
            prog(p * 0.30f, m);
        });
        if (!err.empty()) return err;
    }

    // ── Load audio ────────────────────────────────────────────────────────────
    prog(0.30f, "Loading audio…");
    int N = 0;
    auto interleaved = read_stereo(input_path, N);
    if (N == 0) return "Failed to decode audio";

    // Deinterleave
    std::vector<float> L(N), R(N);
    for (int i = 0; i < N; i++) { L[i] = interleaved[2*i]; R[i] = interleaved[2*i+1]; }
    interleaved.clear();
    interleaved.shrink_to_fit();

    // ── ONNX session ──────────────────────────────────────────────────────────
    prog(0.32f, "Loading HTDemucs model…");

    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "demucs");
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(4);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    // Try CUDA; silently fall back to CPU if unavailable.
    try { OrtCUDAProviderOptions cuda{}; opts.AppendExecutionProvider_CUDA(cuda); }
    catch (...) {}

    Ort::Session session(env, demucs_model_path().c_str(), opts);

    // Verify input/output names at runtime so errors are actionable.
    Ort::AllocatorWithDefaultOptions alloc;
    auto in0 = session.GetInputNameAllocated(0, alloc);
    auto in1 = session.GetInputNameAllocated(1, alloc);
    auto ou0 = session.GetOutputNameAllocated(0, alloc);
    auto ou1 = session.GetOutputNameAllocated(1, alloc);
    if (strcmp(in0.get(), "mix") != 0 || strcmp(in1.get(), "x") != 0)
        return std::string("Unexpected model inputs: '") + in0.get() +
               "', '" + in1.get() + "'. Expected 'mix', 'x'.";
    if (strcmp(ou0.get(), "out_x") != 0 || strcmp(ou1.get(), "out_xt") != 0)
        return std::string("Unexpected model outputs: '") + ou0.get() +
               "', '" + ou1.get() + "'. Expected 'out_x', 'out_xt'.";

    // ── Overlap-add output accumulators ───────────────────────────────────────
    // vocals = stem 3; instrumental = stems 0+1+2
    std::vector<float> voc_L(N, 0.f), voc_R(N, 0.f);
    std::vector<float> ins_L(N, 0.f), ins_R(N, 0.f);
    std::vector<float> wsum  (N, 0.f);   // accumulated window weights

    auto hann = make_hann();

    int n_segs = (N <= kSegLen) ? 1 : (int)std::ceil((double)(N - kSegLen) / kStride) + 1;

    for (int seg = 0; seg < n_segs; seg++) {
        int seg_start = seg * kStride;
        int seg_end   = std::min(seg_start + kSegLen, N);
        int seg_n     = seg_end - seg_start;        // actual samples in this segment

        prog(0.35f + (float)seg / n_segs * 0.50f,
             "Separating stems (segment " + std::to_string(seg + 1) +
             "/" + std::to_string(n_segs) + ")…");

        // ── Build segment buffers (zero-padded to kSegLen) ────────────────────
        std::vector<float> segL(kSegLen, 0.f), segR(kSegLen, 0.f);
        for (int i = 0; i < seg_n; i++) {
            segL[i] = L[seg_start + i];
            segR[i] = R[seg_start + i];
        }

        // ── STFT ──────────────────────────────────────────────────────────────
        std::vector<float> Lre, Lim, Rre, Rim;
        int T = stft(segL.data(), kSegLen, hann, Lre, Lim);
        stft(segR.data(), kSegLen, hann, Rre, Rim);

        size_t spec_sz = (size_t)kFreqBins * T;

        // x: [1, 4, kFreqBins, T] — channels: Lre Lim Rre Rim
        std::vector<float> x_data(4 * spec_sz);
        std::copy(Lre.begin(), Lre.end(), x_data.begin()              );
        std::copy(Lim.begin(), Lim.end(), x_data.begin() +     spec_sz);
        std::copy(Rre.begin(), Rre.end(), x_data.begin() + 2 * spec_sz);
        std::copy(Rim.begin(), Rim.end(), x_data.begin() + 3 * spec_sz);
        Lre.clear(); Lim.clear(); Rre.clear(); Rim.clear();

        // mix: [1, 2, kSegLen] — L then R
        std::vector<float> mix(2 * kSegLen);
        std::copy(segL.begin(), segL.end(), mix.begin()          );
        std::copy(segR.begin(), segR.end(), mix.begin() + kSegLen);

        // ── ONNX inference ────────────────────────────────────────────────────
        Ort::MemoryInfo mem =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        std::vector<int64_t> mix_sh = {1, 2, (int64_t)kSegLen};
        std::vector<int64_t> x_sh   = {1, 4, (int64_t)kFreqBins, (int64_t)T};

        std::vector<Ort::Value> inputs;
        inputs.push_back(Ort::Value::CreateTensor<float>(
            mem, mix.data(), mix.size(), mix_sh.data(), 3));
        inputs.push_back(Ort::Value::CreateTensor<float>(
            mem, x_data.data(), x_data.size(), x_sh.data(), 4));

        const char* in_names[]  = {"mix",   "x"    };
        const char* out_names[] = {"out_x", "out_xt"};
        auto outs = session.Run(
            Ort::RunOptions{nullptr},
            in_names, inputs.data(), 2,
            out_names, 2);

        // out_x:  [1, 4 stems, 4 channels, kFreqBins, T]   freq domain
        // out_xt: [1, 4 stems, 2 channels, kSegLen]         time residual
        const float* px  = outs[0].GetTensorData<float>();
        const float* pxt = outs[1].GetTensorData<float>();

        // ── Reconstruct stems + overlap-add ───────────────────────────────────
        for (int s = 0; s < kNumStem; s++) {
            size_t freq_off = (size_t)s * 4 * spec_sz;
            auto sL = istft(px + freq_off,             px + freq_off +     spec_sz,
                            T, kSegLen, hann);
            auto sR = istft(px + freq_off + 2*spec_sz, px + freq_off + 3*spec_sz,
                            T, kSegLen, hann);

            const float* tL = pxt + (size_t)s * 2 * kSegLen;
            const float* tR = tL + kSegLen;

            // Compute fade weight for this segment position:
            //   ramp in  over [0, kOverlap), constant in middle, ramp out over [-kOverlap, 0)
            // Weights from adjacent segments sum to 1 in the overlap zone.
            for (int i = 0; i < seg_n; i++) {
                int out_i = seg_start + i;
                float w;
                if      (i < kOverlap)           w = (float)i        / kOverlap;
                else if (i >= seg_n - kOverlap)   w = (float)(seg_n - 1 - i) / kOverlap;
                else                             w = 1.f;
                // Clamp for first/last segment where neighbour doesn't exist
                if (seg == 0         && i < kOverlap)         w = 1.f;
                if (seg == n_segs-1  && i >= seg_n - kOverlap) w = 1.f;

                float sLi = sL[i] + tL[i];
                float sRi = sR[i] + tR[i];

                if (s == kVocals) {
                    voc_L[out_i] += w * sLi;
                    voc_R[out_i] += w * sRi;
                } else {
                    ins_L[out_i] += w * sLi;
                    ins_R[out_i] += w * sRi;
                }
                if (s == 0) wsum[out_i] += w;   // accumulate once per position
            }
        }
    }

    // Normalise by accumulated weight (near 1.0 everywhere after OLA)
    for (int i = 0; i < N; i++) {
        if (wsum[i] > 1e-6f) {
            voc_L[i] /= wsum[i]; voc_R[i] /= wsum[i];
            ins_L[i] /= wsum[i]; ins_R[i] /= wsum[i];
        }
    }

    // ── Write output WAVs ─────────────────────────────────────────────────────
    prog(0.92f, "Writing output files…");

    std::vector<float> voc_pcm(2 * N), ins_pcm(2 * N);
    for (int i = 0; i < N; i++) {
        voc_pcm[2*i]   = voc_L[i]; voc_pcm[2*i+1]   = voc_R[i];
        ins_pcm[2*i]   = ins_L[i]; ins_pcm[2*i+1]   = ins_R[i];
    }

    if (!write_stereo(vocals_out,       voc_pcm)) return "Failed to write vocals WAV";
    if (!write_stereo(instrumental_out, ins_pcm)) return "Failed to write instrumental WAV";

    prog(1.f, "Separation complete");
    return {};
}
