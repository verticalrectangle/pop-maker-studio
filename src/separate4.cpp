// 4-stem source separation — htdemucs via ONNX Runtime.
//
// Model: htdemucs.onnx (demucs 4.1.0 `htdemucs` checkpoint, signature
// 955717e8; export: tools/export_htdemucs_onnx.py). The ONNX graph is the
// HTDemucs core only: spectrogram in/out, real-valued
// (complex-as-channels), STFT/iSTFT live in C++ with FFTW.
//
// Pipeline: ffmpeg decode (44.1 kHz stereo float) → `python -m demucs`
// input normalisation (Separator.separate_tensor: mono-mix mean/std) →
// segment (343980-sample training-length chunks, 25% overlap, triangle
// weights — exactly demucs.apply.apply_model(shifts=0, split=True,
// overlap=0.25)) → per segment: _spec (reflect-pad 1536, STFT n_fft=4096
// hop=1024, periodic Hann, normalized=True, center=True reflect, drop
// Nyquist + guard frames) → ONNX (mix + spec → zout CaC spectrogram + xt
// time branch) → _ispec (pad guards/Nyquist, iSTFT, strip pad) + xt →
// weighted overlap-add → denormalise (*std+mean) → four float WAVs
// (drums/bass/other/vocals).
#include "separate4.h"

#include "platform.h"
#include "paths.h"

#include <onnxruntime_cxx_api.h>
#if __has_include(<dnnl_provider_options.h>)
#  include <dnnl_provider_options.h>
#  define PMS_HAVE_DNNL 1
#endif
#if PMS_HAS_FFTW
#include <fftw3.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using cx = std::complex<float>;

static constexpr int kRate = 44100;
static constexpr int kNFFT = 4096;
static constexpr int kHop = kNFFT / 4;          // 1024
static constexpr int kBins = kNFFT / 2;         // 2048 (Nyquist dropped)
static constexpr int kSpecPad = kHop / 2 * 3;   // 1536 (_spec reflect pad)
static constexpr int kTrainLen = 343980;        // segment=39/5 s * 44.1 kHz
static constexpr int kFrames = 336;             // spectrogram frames/segment
static constexpr int kSources = 4;
static constexpr int kChannels = 2;
static constexpr float kOverlap = 0.25f;
static constexpr int kStride = (int)((1.f - kOverlap) * kTrainLen);  // 257985

static const char* kStemNames[4] = {"drums", "bass", "other", "vocals"};

static std::string model_path() { return (fs::path(app_models_dir()) / "htdemucs.onnx").string(); }

bool separate4_available() { return fs::exists(model_path()); }

// ── Audio I/O (house style: separate.cpp) ────────────────────────────────────

static std::vector<float> read_stereo(const std::string& p, int& n) {
    std::string file_arg = "file:" + p;
    std::string rate_val = std::to_string(kRate);
    std::vector<const char*> argv = {"ffmpeg", "-hide_banner", "-loglevel", "error",
                                     "-i", file_arg.c_str(), "-vn", "-ar", rate_val.c_str(),
                                     "-ac", "2", "-f", "f32le", "pipe:1", nullptr};
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
    std::vector<float> buf;
    float tmp[4096]; size_t r;
    while ((r = fread(tmp, sizeof(float), 4096, fp)) > 0)
        buf.insert(buf.end(), tmp, tmp + r);
    fclose(fp);
    waitpid(pid, nullptr, 0);
    n = (int)(buf.size() / 2);
    return buf;
}

static bool write_wav_float(const std::string& path, const float* l, const float* r, int n) {
    // Minimal 44.1 kHz stereo float32 WAV writer (no ffmpeg round-trip).
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) return false;
    uint32_t data_bytes = (uint32_t)n * 2 * 4;
    uint32_t chunk_size = 36 + data_bytes;
    uint8_t hdr[44] = {};
    memcpy(hdr + 0, "RIFF", 4);
    memcpy(hdr + 4, &chunk_size, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    uint32_t fmt_len = 16; memcpy(hdr + 16, &fmt_len, 4);
    uint16_t fmt = 3, ch = 2; memcpy(hdr + 20, &fmt, 2); memcpy(hdr + 22, &ch, 2);
    uint32_t rate = kRate; memcpy(hdr + 24, &rate, 4);
    uint32_t br = kRate * 2 * 4; memcpy(hdr + 28, &br, 4);
    uint16_t ba = 8; memcpy(hdr + 32, &ba, 2);
    uint16_t bps = 32; memcpy(hdr + 34, &bps, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &data_bytes, 4);
    bool ok = fwrite(hdr, 1, 44, fp) == 44;
    for (int i = 0; ok && i < n; i++) {
        ok = fwrite(l + i, 4, 1, fp) == 1 && fwrite(r + i, 4, 1, fp) == 1;
    }
    fclose(fp);
    return ok;
}

// ── STFT / iSTFT (matches demucs _spec/_ispec via torch.stft normalized=True,
// center=True, reflect pad_mode) ─────────────────────────────────────────────
// torch.stft(normalized=True) = unnormalized FFT scaled by 1/sqrt(n_fft);
// torch.istft inverts with the ortho-norm IFFT. FFTW's r2c/c2r are
// unnormalized both ways: r2c = sqrt(n) * ortho-FFT, c2r = sqrt(n) *
// ortho-IFFT (FFTW Backward has no 1/n; torch backward-norm IFFT does, so
// FFTW-c2r = n * torch-irfft-backward = 64 * ortho-irfft for n = 4096).
// Hence both directions scale by 1/sqrt(4096) = 1/64. Round-trip verified
// sample-for-sample against torch.stft/torch.istft.
static constexpr float kFwdScale = 1.f / 64.f;
static constexpr float kInvScale = 1.f / 64.f;

static std::vector<float> make_hann(int n) {
    std::vector<float> w(n);
    for (int i = 0; i < n; i++)
        w[i] = 0.5f * (1.f - cosf(2.f * float(M_PI) * i / n));
    return w;
}

// Reflect-pad index map (torch F.pad reflect: edge NOT repeated —
// pad -1 maps to src 1, pad -2 to src 2, ...).
static inline int reflect_idx(int i, int n) {
    if (n <= 1) return 0;
    for (;;) {
        if (i < 0) i = -i;
        else if (i >= n) i = 2 * (n - 1) - i;
        else return i;
    }
}

// _spec for one channel: reflect-pad time by kSpecPad on the left and
// (le*hop - len) + kSpecPad on the right, STFT (center=True reflect pad
// n_fft/2 each side inside torch.stft), drop Nyquist bin, drop guard
// frames 0/1 and F+2/F+3. Output: [kFrames][kBins] complex.
static void spec_mono(const float* x, int len, const std::vector<float>& win,
                      fftwf_plan plan, fftwf_complex* out_c, float* in_f,
                      std::vector<std::vector<cx>>& spec) {
    int le = (len + kHop - 1) / kHop;
    int total = le * kHop + 2 * kSpecPad;  // reflect-padded length (347136)
    // Full STFT frame count of the padded signal (center=True).
    int nfull = total / kHop + 1;  // 340
    spec.assign(kFrames, std::vector<cx>(kBins));
    // Stream frames t=0..nfull-1; keep t in [2, 2+le).
    std::vector<float> pad_sig(total);
    for (int i = 0; i < total; i++)
        pad_sig[i] = x[reflect_idx(i - kSpecPad, len)];
    int center = kNFFT / 2;
    for (int t = 0; t < nfull; t++) {
        int start = t * kHop - center;
        for (int i = 0; i < kNFFT; i++) {
            int idx = start + i;
            float v = (idx >= 0 && idx < total) ? pad_sig[idx]
                                                : pad_sig[reflect_idx(idx, total)];
            in_f[i] = v * win[i];
        }
        fftwf_execute(plan);
        if (t < 2 || t >= 2 + le) continue;
        for (int f = 0; f < kBins; f++)
            spec[t - 2][f] = cx(out_c[f][0] * kFwdScale, out_c[f][1] * kFwdScale);
    }
}

// _ispec for one source-channel: pad freq to 2049 (Nyquist zero) and time
// guards (2,2 zero frames), iSTFT with center=True (n_fft/2 reflect pad
// each side, windowed OLA normalised by window-square envelope),
// strip the kSpecPad samples each side. Input [kFrames][kBins];
// output kTrainLen samples appended at out0.
static void ispec_mono(const std::vector<std::vector<cx>>& spec, const std::vector<float>& win,
                       fftwf_plan iplan, float* out_f, fftwf_complex* in_c,
                       std::vector<float>& out) {
    int T = kFrames + 4;  // 340 with guards
    int buflen = (T - 1) * kHop + kNFFT;
    std::vector<float> buf(buflen, 0.f), env(buflen, 0.f);
    // Materialise guarded frames.
    for (int t = 0; t < T; t++) {
        for (int f = 0; f <= kNFFT / 2; f++) {
            float re = 0.f, im = 0.f;
            if (f < kBins && t >= 2 && t < 2 + kFrames) {
                re = spec[t - 2][f].real();
                im = spec[t - 2][f].imag();
            }
            in_c[f][0] = re;
            in_c[f][1] = im;
        }
        fftwf_execute(iplan);
        int start = t * kHop;
        for (int i = 0; i < kNFFT; i++) {
            buf[start + i] += out_f[i] * win[i] * kInvScale;
            env[start + i] += win[i] * win[i];
        }
    }
    // torch.istft(center=True,length=le): OLA each frame at t*hop over the
    // n_fft/2 reflect-padded signal, divide by the window-square envelope,
    // trim n_fft/2 each side (giving `le` samples); demucs then strips the
    // _spec kSpecPad samples each side, leaving exactly kTrainLen samples.
    int center = kNFFT / 2 + kSpecPad;
    for (int i = 0; i < kTrainLen; i++) {
        float e = env[i + center];
        out.push_back(e > 1e-8f ? buf[i + center] / e : 0.f);
    }
}

// ── Main separation ─────────────────────────────────────────────────────────

// Core: separate interleaved stereo (N samples @ kRate) into out_dir stems.
// span_t0/audio_path are only used for error messages.
static bool separate_span_core(std::vector<float> interleaved, int N,
                               double span_t0, const std::string& audio_path,
                               const std::string& out_dir,
                               std::array<std::string, 4>& out_paths,
                               const std::function<void(float)>& progress, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };
    if (!separate4_available())
        return fail("Model not found: " + model_path() + "\nPlace the models/ folder next to the binary.");
    (void)span_t0;
    (void)audio_path;

    if (progress) progress(0.05f);
    if (N == 0 || (int)interleaved.size() < 2 * N)
        return fail("Failed to decode audio: " + audio_path);
    std::vector<float> L(N), R(N);
    for (int i = 0; i < N; i++) { L[i] = interleaved[2 * i]; R[i] = interleaved[2 * i + 1]; }
    interleaved.clear(); interleaved.shrink_to_fit();
    // `python -m demucs` input normalisation (Separator.separate_tensor):
    // mean/std of the mono downmix, applied to both channels, undone on
    // the stems after overlap-add (out = out * std + mean). Matches what
    // users get from the CLI; the parity script's raw apply_model() call
    // skips it, so it compares through the same Separator path.
    double m1 = 0.0, m2 = 0.0;
    for (int i = 0; i < N; i++) {
        double v = 0.5 * ((double)L[i] + (double)R[i]);
        m1 += v; m2 += v * v;
    }
    m1 /= N;
    double var = m2 / N - m1 * m1;
    double cli_mean = m1, cli_std = sqrt(var > 0.0 ? var : 0.0) + 1e-8;
    for (int i = 0; i < N; i++) {
        L[i] = (float)(((double)L[i] - cli_mean) / cli_std);
        R[i] = (float)(((double)R[i] - cli_mean) / cli_std);
    }

    // ── Segmenting (demucs apply_model split/overlap) ───────────────────────
    // Triangle weights 1..seg/2+1, seg-seg/2..1 normalised to peak 1
    // (transition_power=1 → plain linear crossfade). Chunks at stride
    // (1-overlap)*segment. A short tail chunk is TensorChunk.padded to the
    // training length by CENTERING (zeros past the song edges) and the model
    // output is center_trimmed back to the chunk length before the weighted
    // overlap-add; out /= sum_weight.
    int seg = kTrainLen, stride = kStride;
    std::vector<int> offsets;
    for (int off = 0; off < N; off += stride) offsets.push_back(off);
    if (offsets.empty()) offsets.push_back(0);
    int nseg = (int)offsets.size();
    int half = seg / 2;  // 171990
    // Exact demucs triangle: cat(arange(1, seg//2+1), arange(seg-seg//2, 0, -1))
    // normalised by its peak (transition_power=1 → plain linear crossfade).
    auto tri_weight = [&](int i) -> float {
        float w = (i < half) ? (float)(i + 1) : (float)(seg - i);
        return w / (float)half;
    };
    auto win = make_hann(kNFFT);
    float* fft_in = fftwf_alloc_real(kNFFT);
    fftwf_complex* fft_out = fftwf_alloc_complex(kNFFT / 2 + 1);
    fftwf_complex* iff_in = fftwf_alloc_complex(kNFFT / 2 + 1);
    float* iff_out = fftwf_alloc_real(kNFFT);
    fftwf_plan fwd = fftwf_plan_dft_r2c_1d(kNFFT, fft_in, fft_out, FFTW_ESTIMATE);
    fftwf_plan inv = fftwf_plan_dft_c2r_1d(kNFFT, iff_in, iff_out, FFTW_ESTIMATE);

    // ── ONNX session (house style: separate.cpp) ────────────────────────────
    if (progress) progress(0.08f);
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "htdemucs");
    Ort::SessionOptions opts;
    unsigned hw = std::thread::hardware_concurrency();
    opts.SetIntraOpNumThreads(hw >= 4 ? (int)hw - 2 : 4);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    try { OrtCUDAProviderOptions cuda{}; opts.AppendExecutionProvider_CUDA(cuda); }
    catch (...) {}
#ifdef PMS_HAVE_DNNL
    try { OrtDnnlProviderOptions dnnl{}; dnnl.use_arena = 1;
          opts.AppendExecutionProvider_Dnnl(dnnl); }
    catch (...) {}
#endif
    Ort::Session session(env, model_path().c_str(), opts);
    Ort::AllocatorWithDefaultOptions alloc;
    auto in0 = session.GetInputNameAllocated(0, alloc);
    auto in1 = session.GetInputNameAllocated(1, alloc);
    auto out0 = session.GetOutputNameAllocated(0, alloc);
    auto out1 = session.GetOutputNameAllocated(1, alloc);
    std::string mix_name(in0.get()), spec_name(in1.get());
    std::string zout_name(out0.get()), xt_name(out1.get());
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // ── Accumulators ────────────────────────────────────────────────────────
    // out[s][ch][i], wsum[i] — demucs: out /= sum_weight (broadcast).
    std::vector<std::vector<float>> acc(kSources * kChannels, std::vector<float>(N, 0.f));
    std::vector<float> wsum(N, 0.f);

    std::vector<int64_t> mix_shape = {1, kChannels, seg};
    std::vector<int64_t> spec_shape = {1, kChannels * 2, kBins, kFrames};

    std::vector<float> segL(seg), segR(seg);
    std::vector<float> mix_in(2 * seg);
    std::vector<float> spec_in(4 * kBins * kFrames);
    std::vector<std::vector<cx>> specL, specR;

    for (int s = 0; s < nseg; s++) {
        int off = offsets[s];
        int chunk_len = std::min(seg, N - off);
        if (progress) progress(0.08f + 0.80f * (float)s / (float)nseg);
        // TensorChunk.padded(target=seg): the chunk is CENTERED in a
        // zero training-length buffer (F.pad zeros), NOT edge-extended.
        int pad_left = (seg - chunk_len) / 2;
        for (int i = 0; i < seg; i++) {
            int j = off - pad_left + i;
            segL[i] = (j >= 0 && j < N) ? L[j] : 0.f;
            segR[i] = (j >= 0 && j < N) ? R[j] : 0.f;
        }
        // _spec both channels.
        spec_mono(segL.data(), seg, win, fwd, fft_out, fft_in, specL);
        spec_mono(segR.data(), seg, win, fwd, fft_out, fft_in, specR);
        // Complex-as-channels input [1,4,2048,336]: re0,im0,re1,im1
        // interleave per bin (matches _magnitude reshape order).
        for (int t = 0; t < kFrames; t++)
            for (int f = 0; f < kBins; f++) {
                size_t base = (size_t)t + (size_t)kFrames * f;
                spec_in[0 * kBins * kFrames + base] = specL[t][f].real();
                spec_in[1 * kBins * kFrames + base] = specL[t][f].imag();
                spec_in[2 * kBins * kFrames + base] = specR[t][f].real();
                spec_in[3 * kBins * kFrames + base] = specR[t][f].imag();
            }
        memcpy(mix_in.data(), segL.data(), (size_t)seg * 4);
        memcpy(mix_in.data() + seg, segR.data(), (size_t)seg * 4);

        std::vector<Ort::Value> inputs;
        inputs.push_back(Ort::Value::CreateTensor<float>(
            mem, mix_in.data(), mix_in.size(), mix_shape.data(), 3));
        inputs.push_back(Ort::Value::CreateTensor<float>(
            mem, spec_in.data(), spec_in.size(), spec_shape.data(), 4));
        const char* in_names[] = {mix_name.c_str(), spec_name.c_str()};
        const char* out_names[] = {zout_name.c_str(), xt_name.c_str()};
        auto outs = session.Run(Ort::RunOptions{nullptr}, in_names, inputs.data(), 2,
                                out_names, 2);
        // zout [1,4,2,2048,336,2] (re0,im0 / re1,im1 + real/imag pair),
        // xt [1,4,2,seg] time-branch waveform.
        const float* zout = outs[0].GetTensorData<float>();
        const float* xt = outs[1].GetTensorData<float>();

        // Per source/channel: _ispec(zout) + xt → weighted overlap-add.
        for (int src = 0; src < kSources; src++) {
            for (int ch = 0; ch < kChannels; ch++) {
                // Gather CaC spectrogram [kFrames][kBins].
                std::vector<std::vector<cx>> sp(kFrames, std::vector<cx>(kBins));
                for (int t = 0; t < kFrames; t++)
                    for (int f = 0; f < kBins; f++) {
                        // [0,s,c,f,t,2]: re = zout[((s*2+c)*2048+f)*336+t][0]
                        size_t base = ((((size_t)src * 2 + ch) * kBins + f) * kFrames + t) * 2;
                        sp[t][f] = cx(zout[base], zout[base + 1]);
                    }
                std::vector<float> wav;
                wav.reserve(seg);
                ispec_mono(sp, win, inv, iff_out, iff_in, wav);
                const float* xtp = xt + ((size_t)src * 2 + ch) * seg;
                // center_trim(seg output → chunk): drop delta/2 front samples
                // (demucs center_trim; extra sample goes to the back).
                int delta = seg - chunk_len;
                int trim = delta / 2;
                for (int i = 0; i < chunk_len; i++) {
                    float w = tri_weight(i);  // positional inside segment
                    float v = wav[trim + i] + xtp[trim + i];
                    acc[(size_t)src * 2 + ch][off + i] += w * v;
                }
            }
        }
        for (int i = 0; i < chunk_len; i++)
            wsum[off + i] += tri_weight(i);
    }

    fftwf_destroy_plan(fwd);
    fftwf_destroy_plan(inv);
    fftwf_free(fft_in); fftwf_free(fft_out);
    fftwf_free(iff_in); fftwf_free(iff_out);

    // Normalise by overlap-add weights (demucs: out /= sum_weight), then
    // undo the CLI input normalisation (Separator: out = out * std + mean).
    for (size_t k = 0; k < acc.size(); k++)
        for (int i = 0; i < N; i++)
            acc[k][i] = (wsum[i] > 1e-6f)
                ? (float)(acc[k][i] / wsum[i] * cli_std + cli_mean) : 0.f;

    // ── Write four float WAVs ───────────────────────────────────────────────
    if (progress) progress(0.95f);
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    for (int src = 0; src < kSources; src++) {
        std::string p = (fs::path(out_dir) / (kStemNames[src] + std::string(".wav"))).string();
        if (!write_wav_float(p, acc[(size_t)src * 2].data(), acc[(size_t)src * 2 + 1].data(), N))
            return fail("Failed to write " + p);
        out_paths[src] = p;
    }
    if (progress) progress(1.0f);
    return true;
}

bool separate_stems4(const std::string& audio_path, const std::string& out_dir,
                     std::array<std::string, 4>& out_paths,
                     const std::function<void(float)>& progress, std::string* err) {
    int N = 0;
    auto interleaved = read_stereo(audio_path, N);
    if (N == 0) {
        if (err) *err = "Failed to decode audio: " + audio_path;
        return false;
    }
    return separate_span_core(std::move(interleaved), N, 0.0, audio_path, out_dir,
                              out_paths, progress, err);
}

bool separate_stems4_span(const std::string& audio_path, double t0, double t1,
                          const std::string& out_dir, std::array<std::string, 4>& out_paths,
                          const std::function<void(float)>& progress, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };
    if (!separate4_available())
        return fail("Model not found: " + model_path() + "\nPlace the models/ folder next to the binary.");
    if (!(t1 > t0)) return fail("empty span");
    // Sample-accurate span slice of the whole-file decode (same rationale as
    // decode_mono_span): no resampler/seek phase drift vs the full-file path.
    int Nfull = 0;
    auto full = read_stereo(audio_path, Nfull);
    if (Nfull == 0) return fail("Failed to decode audio: " + audio_path);
    size_t s0 = (size_t)std::llround(t0 * kRate);
    size_t s1 = (size_t)std::llround(t1 * kRate);
    if (s0 >= (size_t)Nfull) return fail("span starts past end of file");
    if (s1 > (size_t)Nfull) s1 = Nfull;
    if (s1 <= s0) return fail("empty span");
    std::vector<float> interleaved;
    interleaved.reserve((s1 - s0) * 2);
    for (size_t i = s0; i < s1; i++) {
        interleaved.push_back(full[2 * i]);
        interleaved.push_back(full[2 * i + 1]);
    }
    full.clear(); full.shrink_to_fit();
    return separate_span_core(std::move(interleaved), (int)(s1 - s0), t0, audio_path,
                              out_dir, out_paths, progress, err);
}

#else  // !PMS_HAS_FFTW — headless/iOS stub (platform.h)
#include <filesystem>
namespace fs = std::filesystem;
static std::string model_path() { return (fs::path(app_models_dir()) / "htdemucs.onnx").string(); }
bool separate4_available() { return fs::exists(model_path()); }
bool separate_stems4(const std::string&, const std::string&, std::array<std::string, 4>&,
                     const std::function<void(float)>&, std::string* err) {
    if (err) *err = "4-stem separation needs the desktop build (no FFTW/process-spawn headless)";
    return false;
}
bool separate_stems4_span(const std::string&, double, double, const std::string&,
                          std::array<std::string, 4>&, const std::function<void(float)>&,
                          std::string* err) {
    if (err) *err = "4-stem separation needs the desktop build (no FFTW/process-spawn headless)";
    return false;
}
#endif  // PMS_HAS_FFTW
