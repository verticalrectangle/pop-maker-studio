// RVC voice conversion — pure C++ ONNX Runtime inference.
// Pipeline: ffmpeg decode → YIN F0 → HuBERT ONNX → repeat-interleave
//           → VITS ONNX → ffmpeg encode.
// Python is only ever used for one-time model export (vc_export.py).
#include "vc_onnx.h"
#include "paths.h"
#include <onnxruntime_cxx_api.h>
#include <filesystem>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <functional>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;

static constexpr int kHubRate  = 16000;   // HuBERT expects 16 kHz
static constexpr int kOutRate  = 44100;   // app standard
static constexpr int kYINWin   = 1024;    // ~64 ms at 16 kHz
static constexpr int kYINHop   = 160;     // ~10 ms at 16 kHz → ~100 fps

// ── Paths ─────────────────────────────────────────────────────────────────────

std::string hubert_onnx_path() {
    return app_models_dir() + "/hubert.onnx";
}
bool hubert_onnx_exists() { return fs::exists(hubert_onnx_path()); }

// ── Audio I/O ─────────────────────────────────────────────────────────────────

static std::vector<float> read_mono(const std::string& p, int rate) {
    std::string cmd = "ffmpeg -hide_banner -loglevel error"
                      " -i \"" + p + "\""
                      " -vn -ar " + std::to_string(rate) +
                      " -ac 1 -f f32le pipe:1 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return {};
    std::vector<float> buf;
    float tmp[4096]; size_t n;
    while ((n = fread(tmp, sizeof(float), 4096, fp)) > 0)
        buf.insert(buf.end(), tmp, tmp + n);
    pclose(fp);
    return buf;
}

// Write mono float32 at in_rate, resampling to kOutRate via ffmpeg.
static bool write_wav(const std::string& p, const std::vector<float>& d, int in_rate) {
    std::string cmd = "ffmpeg -hide_banner -loglevel error -y"
                      " -f f32le -ar " + std::to_string(in_rate) + " -ac 1 -i pipe:0"
                      " -ar " + std::to_string(kOutRate) +
                      " -ac 1 \"" + p + "\" 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "w");
    if (!fp) return false;
    fwrite(d.data(), sizeof(float), d.size(), fp);
    return pclose(fp) == 0;
}

// ── YIN F0 detector (from silvertune) ────────────────────────────────────────
// Returns F0 in Hz per frame (0.0 = unvoiced).

static std::vector<float> yin_f0(
    const std::vector<float>& x, int sr, int hop = kYINHop, int win = kYINWin)
{
    int N        = (int)x.size();
    int half     = win / 2;
    int min_tau  = std::max(1, sr / 1100);
    int max_tau  = std::min(half - 1, sr / 50);
    int n_frames = std::max(1, (N - win) / hop + 1);

    std::vector<float> f0(n_frames, 0.f), d(half);

    for (int i = 0; i < n_frames; i++) {
        int start = i * hop;

        // Difference function
        for (int tau = 0; tau < half; tau++) {
            float sum = 0.f;
            for (int j = 0; j < half; j++) {
                float a = (start + j        < N) ? x[start + j]        : 0.f;
                float b = (start + j + tau  < N) ? x[start + j + tau]  : 0.f;
                sum += (a - b) * (a - b);
            }
            d[tau] = sum;
        }

        // Cumulative mean normalised difference
        d[0] = 1.f;
        float run = 0.f;
        for (int tau = 1; tau < half; tau++) {
            run += d[tau];
            d[tau] = (run > 1e-10f) ? d[tau] * tau / run : 1.f;
        }

        // Absolute threshold + parabolic interpolation
        for (int tau = std::max(min_tau, 2); tau < max_tau; tau++) {
            if (d[tau] < 0.15f && d[tau] <= d[tau - 1]) {
                float denom = d[tau-1] - 2.f*d[tau] + (tau+1 < half ? d[tau+1] : d[tau]);
                float adj   = (std::abs(denom) > 1e-10f && tau+1 < half)
                            ? 0.5f * (d[tau-1] - d[tau+1]) / denom : 0.f;
                adj = std::max(-0.5f, std::min(0.5f, adj));
                f0[i] = (float)sr / (tau + adj);
                break;
            }
        }
    }
    return f0;
}

// Linear interpolation to resize an F0 array.
static std::vector<float> interp_f0(const std::vector<float>& f0, int target) {
    if ((int)f0.size() == target) return f0;
    std::vector<float> out(target);
    float scale = (float)((int)f0.size() - 1) / std::max(target - 1, 1);
    for (int i = 0; i < target; i++) {
        float pos = i * scale;
        int   j   = (int)pos;
        float t   = pos - j;
        int   k   = std::min(j + 1, (int)f0.size() - 1);
        out[i]    = f0[j] * (1.f - t) + f0[k] * t;
    }
    return out;
}

// ── ONNX session helper ───────────────────────────────────────────────────────

static Ort::SessionOptions make_opts() {
    Ort::SessionOptions o;
    o.SetIntraOpNumThreads(4);
    o.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    try { OrtCUDAProviderOptions cuda{}; o.AppendExecutionProvider_CUDA(cuda); }
    catch (...) {}
    return o;
}

// ── Main conversion ───────────────────────────────────────────────────────────

std::string vc_onnx_convert(
    const std::string& source_wav,
    const std::string& voice_onnx,
    const std::string& output_wav,
    int f0_semitones,
    std::function<void(float, const std::string&)> on_progress)
{
    auto prog = [&](float p, const std::string& m) {
        if (on_progress) on_progress(p, m);
    };

    if (!fs::exists(voice_onnx))  return "Voice ONNX not found: " + voice_onnx;
    if (!hubert_onnx_exists())    return "HuBERT ONNX not found at " + hubert_onnx_path();

    // ── Load audio ────────────────────────────────────────────────────────────
    prog(0.05f, "Loading audio…");
    auto wav16 = read_mono(source_wav, kHubRate);   // for HuBERT + F0
    if (wav16.empty()) return "Failed to decode audio";
    int N16 = (int)wav16.size();

    // ── ONNX sessions ─────────────────────────────────────────────────────────
    Ort::Env         env(ORT_LOGGING_LEVEL_WARNING, "vc_onnx");
    Ort::SessionOptions opts = make_opts();

    prog(0.10f, "Loading HuBERT…");
    Ort::Session hub_sess(env, hubert_onnx_path().c_str(), opts);

    prog(0.15f, "Loading voice model…");
    Ort::Session voc_sess(env, voice_onnx.c_str(), opts);

    // Read target_sr from sidecar JSON written by vc_export.py
    int tgt_sr = 40000;
    {
        std::string jp = voice_onnx.substr(0, voice_onnx.rfind('.')) + ".json";
        std::ifstream jf(jp);
        if (jf) {
            std::string s((std::istreambuf_iterator<char>(jf)), {});
            auto find_int = [&](const char* key) {
                std::string search = std::string("\"") + key + "\"";
                auto pos = s.find(search);
                if (pos == std::string::npos) return -1;
                pos = s.find(':', pos);
                if (pos == std::string::npos) return -1;
                try { return std::stoi(s.substr(pos + 1)); } catch (...) { return -1; }
            };
            int v = find_int("target_sr");
            if (v > 0) tgt_sr = v;
        }
    }

    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // ── HuBERT features ───────────────────────────────────────────────────────
    prog(0.20f, "Extracting phone features…");

    std::vector<int64_t> hub_shape = {1, (int64_t)N16};
    std::vector<Ort::Value> hub_in;
    hub_in.push_back(Ort::Value::CreateTensor<float>(
        mem, wav16.data(), wav16.size(), hub_shape.data(), 2));

    const char* hub_in_names[]  = {"audio"};
    const char* hub_out_names[] = {"features"};
    auto hub_out = hub_sess.Run(
        Ort::RunOptions{nullptr},
        hub_in_names, hub_in.data(), 1,
        hub_out_names, 1);

    // features: [1, T, 768]
    auto feat_info  = hub_out[0].GetTensorTypeAndShapeInfo();
    auto feat_shape = feat_info.GetShape();
    int  T_phone    = (int)feat_shape[1];
    int  D_phone    = (int)feat_shape[2];
    const float* fd = hub_out[0].GetTensorData<float>();

    // Repeat-interleave ×2: [1, T, D] → [1, 2T, D]
    int T2 = T_phone * 2;
    std::vector<float> phone((size_t)T2 * D_phone);
    for (int t = 0; t < T_phone; t++) {
        const float* src = fd + (size_t)t * D_phone;
        float* d0 = phone.data() + (size_t)(2*t)   * D_phone;
        float* d1 = phone.data() + (size_t)(2*t+1) * D_phone;
        std::copy(src, src + D_phone, d0);
        std::copy(src, src + D_phone, d1);
    }

    // ── YIN F0 ────────────────────────────────────────────────────────────────
    prog(0.55f, "Extracting pitch…");

    auto f0_raw = yin_f0(wav16, kHubRate);
    if (f0_semitones != 0) {
        float mult = std::pow(2.f, f0_semitones / 12.f);
        for (auto& v : f0_raw) if (v > 0.f) v *= mult;
    }
    auto f0 = interp_f0(f0_raw, T2);

    // ── VITS synthesis ────────────────────────────────────────────────────────
    prog(0.65f, "Voice synthesis…");

    std::vector<int64_t> ph_shape  = {1, (int64_t)T2, (int64_t)D_phone};
    std::vector<int64_t> f0_shape  = {1, (int64_t)T2};
    std::vector<int64_t> sid_shape = {1};
    std::vector<int64_t> sid_val   = {0};

    std::vector<Ort::Value> voc_in;
    voc_in.push_back(Ort::Value::CreateTensor<float>(
        mem, phone.data(), phone.size(), ph_shape.data(), 3));
    voc_in.push_back(Ort::Value::CreateTensor<float>(
        mem, f0.data(), f0.size(), f0_shape.data(), 2));
    voc_in.push_back(Ort::Value::CreateTensor<int64_t>(
        mem, sid_val.data(), 1, sid_shape.data(), 1));

    const char* voc_in_names[]  = {"phone", "f0", "sid"};
    const char* voc_out_names[] = {"audio"};
    auto voc_out = voc_sess.Run(
        Ort::RunOptions{nullptr},
        voc_in_names, voc_in.data(), 3,
        voc_out_names, 1);

    auto audio_shape = voc_out[0].GetTensorTypeAndShapeInfo().GetShape();
    const float* ad  = voc_out[0].GetTensorData<float>();
    int M = (int)audio_shape[2];   // [1, 1, M]

    // ── Write output ──────────────────────────────────────────────────────────
    prog(0.92f, "Writing output…");
    std::vector<float> audio(ad, ad + M);
    if (!write_wav(output_wav, audio, tgt_sr))
        return "Failed to write output audio";

    prog(1.f, "Done");
    return {};
}
