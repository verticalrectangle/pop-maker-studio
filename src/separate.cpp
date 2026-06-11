// MDX-Net vocal separation — C++ ONNX Runtime inference.
// Model: Kim_Vocal_2.onnx  (UVR5 community, battle-tested)
// Pipeline: ffmpeg decode → STFT (FFTW3) → chunked ONNX → ISTFT → ffmpeg encode.
// Stems: vocals (model output) + instrumental (original − vocals).
#include "separate.h"
#include "paths.h"
#include <onnxruntime_cxx_api.h>
#include <fftw3.h>
#include <filesystem>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <functional>
#include <complex>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

namespace fs = std::filesystem;
using cx = std::complex<float>;

static constexpr int   kRate   = 44100;
static constexpr int   kNFFT   = 6144;         // n_fft
static constexpr int   kDimF   = 3072;         // n_fft / 2 — freq bins used by model
static constexpr int   kDimT   = 256;          // time frames per model chunk
static constexpr int   kHop    = 1024;         // STFT hop length
static constexpr float kComp   = 1.035f;       // Kim_Vocal_2 amplitude compensate
// Overlap between consecutive model chunks (in STFT frames).
static constexpr int   kOvlap  = 64;           // 25% of 256
static constexpr int   kStride = kDimT - kOvlap; // 192 frames

// ── Paths ─────────────────────────────────────────────────────────────────────

std::string separate_model_path() {
    return app_models_dir() + "/Kim_Vocal_2.onnx";
}
bool separate_model_exists() { return fs::exists(separate_model_path()); }

// ── Audio I/O ─────────────────────────────────────────────────────────────────

static std::vector<float> read_stereo(const std::string& p, int& n,
                                       float clip_in = 0.f, float clip_dur = 0.f) {
    std::string file_arg = "file:" + p;
    std::string ss_val   = std::to_string(clip_in);
    std::string t_val    = std::to_string(clip_dur);
    std::string rate_val = std::to_string(kRate);

    std::vector<const char*> argv = {"ffmpeg", "-hide_banner", "-loglevel", "error"};
    if (clip_in  > 0.f) { argv.push_back("-ss"); argv.push_back(ss_val.c_str()); }
    argv.push_back("-i"); argv.push_back(file_arg.c_str());
    if (clip_dur > 0.f) { argv.push_back("-t");  argv.push_back(t_val.c_str()); }
    argv.insert(argv.end(), {"-vn", "-ar", rate_val.c_str(), "-ac", "2", "-f", "f32le", "pipe:1", nullptr});

    int pipefd[2];
    if (pipe(pipefd) != 0) { n = 0; return {}; }
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
        execvp("ffmpeg", const_cast<char**>(argv.data()));
        _exit(127);
    }
    close(pipefd[1]);
    FILE* fp = fdopen(pipefd[0], "r");
    if (!fp) { close(pipefd[0]); waitpid(pid, nullptr, 0); n = 0; return {}; }

    std::vector<float> buf;
    float tmp[4096]; size_t r;
    while ((r = fread(tmp, sizeof(float), 4096, fp)) > 0)
        buf.insert(buf.end(), tmp, tmp + r);
    fclose(fp);
    waitpid(pid, nullptr, 0);
    n = (int)(buf.size() / 2);
    return buf;
}

static bool write_stereo(const std::string& p, const std::vector<float>& d, int n) {
    std::string cmd = "ffmpeg -hide_banner -loglevel error -y"
                      " -f f32le -ar " + std::to_string(kRate) +
                      " -ac 2 -i pipe:0"
                      " \"" + p + "\" 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "w");
    if (!fp) return false;
    fwrite(d.data(), sizeof(float), (size_t)n * 2, fp);
    return pclose(fp) == 0;
}

// ── STFT / ISTFT ──────────────────────────────────────────────────────────────

// Hann window, periodic (matches librosa default).
static std::vector<float> make_hann(int n) {
    std::vector<float> w(n);
    for (int i = 0; i < n; i++)
        w[i] = 0.5f * (1.f - cosf(2.f * float(M_PI) * i / n));
    return w;
}

// STFT of a mono signal.  Returns complex spectrum [T][kNFFT/2+1].
// Uses center-padding (n_fft/2 zeros on each side) matching librosa default.
static std::vector<std::vector<cx>>
stft_mono(const std::vector<float>& x, const std::vector<float>& win,
          fftwf_plan plan, fftwf_complex* out_c, float* in_f)
{
    int N   = (int)x.size();
    int pad = kNFFT / 2;
    int T   = (N + 2 * pad - kNFFT) / kHop + 1;

    std::vector<std::vector<cx>> spec(T, std::vector<cx>(kNFFT / 2 + 1));

    for (int t = 0; t < T; t++) {
        int start = t * kHop - pad;
        for (int i = 0; i < kNFFT; i++) {
            int idx = start + i;
            in_f[i] = (idx >= 0 && idx < N) ? x[idx] * win[i] : 0.f;
        }
        fftwf_execute(plan);
        for (int f = 0; f <= kNFFT / 2; f++)
            spec[t][f] = cx(out_c[f][0], out_c[f][1]);
    }
    return spec;
}

// ISTFT: inverse of stft_mono.  Returns reconstructed signal of length N.
static std::vector<float>
istft_mono(const std::vector<std::vector<cx>>& spec, int N,
           const std::vector<float>& win,
           fftwf_plan iplan, float* out_f, fftwf_complex* in_c)
{
    int T   = (int)spec.size();
    int pad = kNFFT / 2;

    std::vector<float> buf(N + 2 * pad, 0.f);
    std::vector<float> wsq(N + 2 * pad, 0.f);

    for (int t = 0; t < T; t++) {
        // Fill complex input (n_fft/2+1 bins)
        for (int f = 0; f <= kNFFT / 2; f++) {
            in_c[f][0] = spec[t][f].real();
            in_c[f][1] = spec[t][f].imag();
        }
        fftwf_execute(iplan);

        int start = t * kHop;
        float norm = 1.f / kNFFT;
        for (int i = 0; i < kNFFT; i++) {
            buf[start + i] += out_f[i] * win[i] * norm;
            wsq[start + i] += win[i] * win[i];
        }
    }

    // Normalize overlap-add by window power, trim center padding.
    std::vector<float> result(N);
    for (int i = 0; i < N; i++) {
        float w = wsq[i + pad];
        result[i] = (w > 1e-8f) ? buf[i + pad] / w : 0.f;
    }
    return result;
}

// ── Main separation ───────────────────────────────────────────────────────────

std::string separate_run(
    const std::string& input_path,
    const std::string& vocals_out,
    const std::string& instrumental_out,
    std::function<void(float, const std::string&)> on_progress,
    float clip_in,
    float clip_dur)
{
    auto prog = [&](float p, const std::string& m) {
        if (on_progress) on_progress(p, m);
    };

    if (!separate_model_exists()) {
        return "Model not found: " + separate_model_path() +
               "\nPlace the models/ folder next to the binary.";
    }

    // ── Load audio ────────────────────────────────────────────────────────────
    prog(0.20f, "Loading audio…");
    int N = 0;
    auto interleaved = read_stereo(input_path, N, clip_in, clip_dur);
    if (N == 0) return "Failed to decode audio";

    std::vector<float> L(N), R(N);
    for (int i = 0; i < N; i++) { L[i] = interleaved[2*i]; R[i] = interleaved[2*i+1]; }
    interleaved.clear(); interleaved.shrink_to_fit();

    // ── FFTW setup ────────────────────────────────────────────────────────────
    prog(0.22f, "Computing spectrograms…");

    auto win = make_hann(kNFFT);

    float*         fft_in  = fftwf_alloc_real(kNFFT);
    fftwf_complex* fft_out = fftwf_alloc_complex(kNFFT / 2 + 1);
    fftwf_complex* iff_in  = fftwf_alloc_complex(kNFFT / 2 + 1);
    float*         iff_out = fftwf_alloc_real(kNFFT);

    fftwf_plan fwd  = fftwf_plan_dft_r2c_1d(kNFFT, fft_in,  fft_out, FFTW_ESTIMATE);
    fftwf_plan inv  = fftwf_plan_dft_c2r_1d(kNFFT, iff_in,  iff_out, FFTW_ESTIMATE);

    // ── STFT of both channels ─────────────────────────────────────────────────
    auto specL = stft_mono(L, win, fwd, fft_out, fft_in);
    auto specR = stft_mono(R, win, fwd, fft_out, fft_in);

    int T_total = (int)specL.size();  // total STFT frames

    // ── ONNX session ──────────────────────────────────────────────────────────
    prog(0.28f, "Loading MDX model…");

    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "mdx");
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(4);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    try { OrtCUDAProviderOptions cuda{}; opts.AppendExecutionProvider_CUDA(cuda); }
    catch (...) {}

    Ort::Session session(env, separate_model_path().c_str(), opts);
    Ort::AllocatorWithDefaultOptions alloc;
    auto in0  = session.GetInputNameAllocated(0, alloc);
    auto out0 = session.GetOutputNameAllocated(0, alloc);
    std::string in_name(in0.get()), out_name(out0.get());

    // ── Chunked MDX inference ─────────────────────────────────────────────────
    // Accumulators for the vocal spectrogram.
    std::vector<std::vector<cx>> voc_L(T_total, std::vector<cx>(kDimF, 0.f));
    std::vector<std::vector<cx>> voc_R(T_total, std::vector<cx>(kDimF, 0.f));
    std::vector<float>           wsum(T_total, 0.f);

    int n_chunks = std::max(1, (T_total - kDimT + kStride - 1) / kStride + 1);
    if (T_total <= kDimT) n_chunks = 1;

    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<int64_t> in_shape = {1, 4, kDimF, kDimT};

    for (int chunk = 0; chunk < n_chunks; chunk++) {
        int t0 = chunk * kStride;
        int t1 = std::min(t0 + kDimT, T_total);
        int tN = t1 - t0;  // actual frames in this chunk

        prog(0.30f + (float)chunk / n_chunks * 0.60f,
             "Separating vocals (chunk " + std::to_string(chunk + 1) +
             "/" + std::to_string(n_chunks) + ")…");

        // Build model input [1, 4, kDimF, kDimT] — zero-padded if needed.
        std::vector<float> inp(4 * kDimF * kDimT, 0.f);
        for (int t = 0; t < tN; t++) {
            for (int f = 0; f < kDimF; f++) {
                cx sL = specL[t0 + t][f];
                cx sR = specR[t0 + t][f];
                size_t base = (size_t)t + (size_t)kDimT * f;
                inp[0 * kDimF * kDimT + base] = sL.real();   // L real
                inp[1 * kDimF * kDimT + base] = sL.imag();   // L imag
                inp[2 * kDimF * kDimT + base] = sR.real();   // R real
                inp[3 * kDimF * kDimT + base] = sR.imag();   // R imag
            }
        }

        std::vector<Ort::Value> inputs;
        inputs.push_back(Ort::Value::CreateTensor<float>(
            mem, inp.data(), inp.size(), in_shape.data(), 4));

        const char* in_names[]  = {in_name.c_str()};
        const char* out_names[] = {out_name.c_str()};
        auto outs = session.Run(Ort::RunOptions{nullptr},
                                in_names, inputs.data(), 1, out_names, 1);

        const float* pd = outs[0].GetTensorData<float>();

        // Crossfade weight: ramp in / ramp out at chunk boundaries.
        for (int t = 0; t < tN; t++) {
            float w;
            if      (t < kOvlap && chunk > 0)           w = (float)t / kOvlap;
            else if (t >= tN - kOvlap && chunk < n_chunks - 1) w = (float)(tN - 1 - t) / kOvlap;
            else                                          w = 1.f;

            for (int f = 0; f < kDimF; f++) {
                size_t base = (size_t)t + (size_t)kDimT * f;
                cx ol(pd[0 * kDimF * kDimT + base], pd[1 * kDimF * kDimT + base]);
                cx or_(pd[2 * kDimF * kDimT + base], pd[3 * kDimF * kDimT + base]);
                voc_L[t0 + t][f] += w * ol;
                voc_R[t0 + t][f] += w * or_;
            }
            wsum[t0 + t] += w;
        }
    }

    // Normalise overlap-add weights.
    for (int t = 0; t < T_total; t++) {
        float w = (wsum[t] > 1e-6f) ? 1.f / wsum[t] : 0.f;
        for (int f = 0; f < kDimF; f++) {
            voc_L[t][f] *= w * kComp;
            voc_R[t][f] *= w * kComp;
        }
    }

    // ── ISTFT ─────────────────────────────────────────────────────────────────
    prog(0.92f, "Reconstructing audio…");

    // Expand kDimF bins to full n_fft/2+1 (zero-pad the missing Nyquist bin).
    auto expandSpec = [&](const std::vector<std::vector<cx>>& src) {
        std::vector<std::vector<cx>> dst(T_total, std::vector<cx>(kNFFT / 2 + 1, 0.f));
        for (int t = 0; t < T_total; t++)
            for (int f = 0; f < kDimF; f++)
                dst[t][f] = src[t][f];
        return dst;
    };

    auto vocL_full = expandSpec(voc_L);
    auto vocR_full = expandSpec(voc_R);

    auto voc_l = istft_mono(vocL_full, N, win, inv, iff_out, iff_in);
    auto voc_r = istft_mono(vocR_full, N, win, inv, iff_out, iff_in);

    // Instrumental = original − vocals.
    std::vector<float> voc_pcm(2 * N), ins_pcm(2 * N);
    for (int i = 0; i < N; i++) {
        voc_pcm[2*i]   = voc_l[i];
        voc_pcm[2*i+1] = voc_r[i];
        ins_pcm[2*i]   = L[i] - voc_l[i];
        ins_pcm[2*i+1] = R[i] - voc_r[i];
    }

    fftwf_destroy_plan(fwd);
    fftwf_destroy_plan(inv);
    fftwf_free(fft_in); fftwf_free(fft_out);
    fftwf_free(iff_in); fftwf_free(iff_out);

    // ── Write outputs ─────────────────────────────────────────────────────────
    prog(0.95f, "Writing output files…");
    if (!write_stereo(vocals_out, voc_pcm, N))
        return "Failed to write vocals WAV";
    if (!write_stereo(instrumental_out, ins_pcm, N))
        return "Failed to write instrumental WAV";

    prog(1.f, "Separation complete");
    return {};
}
