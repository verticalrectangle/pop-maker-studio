// HTDemucs stem separation — C++ ONNX Runtime inference.
// The MrCitron ONNX export handles STFT/iSTFT internally.
// Interface: input[1,2,N] stereo → output[1,4,2,N] four stems stereo.
// Stem order: drums=0 bass=1 other=2 vocals=3
#include "demucs.h"
#include <onnxruntime_cxx_api.h>
#include <filesystem>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <functional>
#include <cstdlib>
#include <cstdio>

namespace fs = std::filesystem;

static const char* kModelFile  = "htdemucs.onnx";
static const char* kModelUrl   =
    "https://huggingface.co/MrCitron/demucs-v4-onnx/resolve/main/htdemucs.onnx";

static constexpr int kRate     = 44100;
static constexpr int kNumStem  = 4;
static constexpr int kVocals   = 3;   // drums=0 bass=1 other=2 vocals=3
// Segment length and overlap for overlap-add on long files.
static constexpr int kSegLen   = 343980;   // 7.8 s — HTDemucs fixed segment
static constexpr int kOverlap  = 17199;    // 0.39 s fade zone each side (5%)
static constexpr int kStride   = kSegLen - 2 * kOverlap;   // 309582

// ── Paths ─────────────────────────────────────────────────────────────────────

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

// ── Audio I/O ─────────────────────────────────────────────────────────────────

// Returns interleaved L R float32 at kRate. Sets n to per-channel count.
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

    // Deinterleave: L[0..N-1] then R[0..N-1] (model wants non-interleaved)
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
    try { OrtCUDAProviderOptions cuda{}; opts.AppendExecutionProvider_CUDA(cuda); }
    catch (...) {}

    Ort::Session session(env, demucs_model_path().c_str(), opts);

    // Validate model interface at runtime
    Ort::AllocatorWithDefaultOptions alloc;
    if (session.GetInputCount() < 1 || session.GetOutputCount() < 1)
        return "Unexpected model: no inputs or outputs";

    auto in0  = session.GetInputNameAllocated(0, alloc);
    auto out0 = session.GetOutputNameAllocated(0, alloc);
    std::string in_name(in0.get()), out_name(out0.get());

    // ── Overlap-add accumulators ───────────────────────────────────────────────
    std::vector<float> voc_L(N, 0.f), voc_R(N, 0.f);
    std::vector<float> ins_L(N, 0.f), ins_R(N, 0.f);
    std::vector<float> wsum  (N, 0.f);

    int n_segs = (N <= kSegLen) ? 1
               : (int)std::ceil((double)(N - kSegLen) / kStride) + 1;

    for (int seg = 0; seg < n_segs; seg++) {
        int seg_start = seg * kStride;
        int seg_end   = std::min(seg_start + kSegLen, N);
        int seg_n     = seg_end - seg_start;

        prog(0.35f + (float)seg / n_segs * 0.55f,
             "Separating stems (segment " + std::to_string(seg + 1) +
             "/" + std::to_string(n_segs) + ")…");

        // Build model input [1, 2, seg_len] — zero-padded if needed.
        // Layout: first kSegLen floats = L, next kSegLen = R.
        std::vector<float> mix(2 * kSegLen, 0.f);
        for (int i = 0; i < seg_n; i++) {
            mix[i]          = L[seg_start + i];
            mix[kSegLen + i] = R[seg_start + i];
        }

        Ort::MemoryInfo mem =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<int64_t> in_shape = {1, 2, (int64_t)kSegLen};

        std::vector<Ort::Value> inputs;
        inputs.push_back(Ort::Value::CreateTensor<float>(
            mem, mix.data(), mix.size(), in_shape.data(), 3));

        const char* in_names[]  = {in_name.c_str()};
        const char* out_names[] = {out_name.c_str()};
        auto outs = session.Run(
            Ort::RunOptions{nullptr},
            in_names, inputs.data(), 1,
            out_names, 1);

        // Output: [1, 4, 2, kSegLen]  stems × channels × samples
        auto out_shape = outs[0].GetTensorTypeAndShapeInfo().GetShape();
        if (out_shape.size() < 4)
            return "Unexpected model output rank " + std::to_string(out_shape.size());

        int n_out_stems = (int)out_shape[1];
        int n_out_ch    = (int)out_shape[2];
        int n_out_time  = (int)out_shape[3];
        const float* pd = outs[0].GetTensorData<float>();

        if (n_out_stems < kNumStem || n_out_ch < 2)
            return "Unexpected model output shape: stems=" + std::to_string(n_out_stems);

        // Overlap-add with linear cross-fade at segment boundaries.
        // Weight: ramp in for leading overlap, ramp out for trailing overlap.
        int lim = std::min(seg_n, n_out_time);
        for (int i = 0; i < lim; i++) {
            int   out_i = seg_start + i;
            float w;
            if      (i < kOverlap      && seg > 0)        w = (float)i / kOverlap;
            else if (i >= lim-kOverlap && seg < n_segs-1) w = (float)(lim - 1 - i) / kOverlap;
            else                                           w = 1.f;

            for (int s = 0; s < kNumStem; s++) {
                float sL = pd[(size_t)s * n_out_ch * n_out_time + 0 * n_out_time + i];
                float sR = pd[(size_t)s * n_out_ch * n_out_time + 1 * n_out_time + i];
                if (s == kVocals) {
                    voc_L[out_i] += w * sL;
                    voc_R[out_i] += w * sR;
                } else {
                    ins_L[out_i] += w * sL;
                    ins_R[out_i] += w * sR;
                }
            }
            wsum[out_i] += w;
        }
    }

    // Normalise by accumulated weight
    for (int i = 0; i < N; i++) {
        if (wsum[i] > 1e-6f) {
            voc_L[i] /= wsum[i]; voc_R[i] /= wsum[i];
            ins_L[i] /= wsum[i]; ins_R[i] /= wsum[i];
        }
    }

    // ── Write output WAVs ─────────────────────────────────────────────────────
    prog(0.93f, "Writing output files…");

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
