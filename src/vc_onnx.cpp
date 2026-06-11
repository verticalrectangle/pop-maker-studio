// RVC voice conversion — pure C++ ONNX Runtime inference.
// Pipeline: ffmpeg decode → RMVPE F0 → HuBERT ONNX
//           → repeat-interleave → VITS ONNX → ffmpeg encode.
#include "vc_onnx.h"
#include "rmvpe_onnx.h"
#include "faiss_ivf.h"
#include "paths.h"
#include <onnxruntime_cxx_api.h>
#include <filesystem>
#include <cmath>
#include <random>
#include <vector>
#include <string>
#include <algorithm>
#include <fstream>
#include <functional>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;

static constexpr int kHubRate  = 16000;   // HuBERT expects 16 kHz
static constexpr int kOutRate  = 44100;   // app standard

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
    bool f0_auto,
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

    // Normalise to 0.95 peak (parity with RVC preprocessing)
    {
        float peak = 0.f;
        for (float v : wav16) peak = std::fmax(peak, std::fabs(v));
        if (peak / 0.95f > 1.f) {
            float s = 0.95f / peak;
            for (auto& v : wav16) v *= s;
        }
    }

    // Reflect-pad 0.5 s each side so HuBERT/F0 context exists at the clip
    // edges; the matching span is trimmed from the synthesised output.
    constexpr int kPad16 = kHubRate / 2;
    {
        int n = (int)wav16.size();
        std::vector<float> p((size_t)n + 2 * kPad16);
        for (int i = 0; i < kPad16; i++)
            p[(size_t)i] = wav16[(size_t)std::min(kPad16 - i, n - 1)];
        std::copy(wav16.begin(), wav16.end(), p.begin() + kPad16);
        for (int i = 0; i < kPad16; i++)
            p[(size_t)(kPad16 + n + i)] = wav16[(size_t)std::max(0, n - 2 - i)];
        wav16 = std::move(p);
    }
    int N16 = (int)wav16.size();

    // ── ONNX sessions ─────────────────────────────────────────────────────────
    Ort::Env         env(ORT_LOGGING_LEVEL_WARNING, "vc_onnx");
    Ort::SessionOptions opts = make_opts();

    prog(0.10f, "Loading HuBERT…");
    Ort::Session hub_sess(env, hubert_onnx_path().c_str(), opts);

    prog(0.15f, "Loading voice model…");
    Ort::Session voc_sess(env, voice_onnx.c_str(), opts);

    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // ── Probe the voice model: signature, phone dim, rnd channels ────────────
    // Standard RVC ONNX signature is phone/phone_lengths/pitch/pitchf/ds/rnd;
    // we accept any subset of those names so third-party exports work natively.
    struct VocSig {
        bool has_lengths = false, has_rnd = false;
        int  phone_dim = 768, rnd_ch = 192;
    } sig;
    {
        Ort::AllocatorWithDefaultOptions alloc;
        size_t n_in = voc_sess.GetInputCount();
        for (size_t i = 0; i < n_in; i++) {
            std::string nm = voc_sess.GetInputNameAllocated(i, alloc).get();
            auto shape = voc_sess.GetInputTypeInfo(i)
                             .GetTensorTypeAndShapeInfo().GetShape();
            if (nm == "phone_lengths") sig.has_lengths = true;
            else if (nm == "rnd") {
                sig.has_rnd = true;
                if (shape.size() == 3 && shape[1] > 0) sig.rnd_ch = (int)shape[1];
            }
            else if (nm == "phone") {
                if (shape.size() == 3 && shape[2] > 0) sig.phone_dim = (int)shape[2];
            }
            else if (nm != "pitch" && nm != "pitchf" && nm != "ds")
                return "Voice model has unsupported input '" + nm + "'";
        }
    }

    // Run the voice model on tiny inputs; returns audio sample count (<0 = error).
    auto run_voice = [&](const std::vector<float>& phone, int T,
                         const std::vector<int64_t>& pitch,
                         const std::vector<float>& pitchf,
                         const std::vector<float>& rnd,
                         std::vector<float>* audio_out) -> int64_t {
        std::vector<int64_t> ph_shape  = {1, (int64_t)T, (int64_t)sig.phone_dim};
        std::vector<int64_t> t_shape   = {1, (int64_t)T};
        std::vector<int64_t> rnd_shape = {1, (int64_t)sig.rnd_ch, (int64_t)T};
        std::vector<int64_t> one_shape = {1};
        std::vector<int64_t> len_val   = {(int64_t)T};
        std::vector<int64_t> ds_val    = {0};

        std::vector<const char*> names;
        std::vector<Ort::Value>  vals;
        names.push_back("phone");
        vals.push_back(Ort::Value::CreateTensor<float>(
            mem, const_cast<float*>(phone.data()), phone.size(), ph_shape.data(), 3));
        if (sig.has_lengths) {
            names.push_back("phone_lengths");
            vals.push_back(Ort::Value::CreateTensor<int64_t>(
                mem, len_val.data(), 1, one_shape.data(), 1));
        }
        names.push_back("pitch");
        vals.push_back(Ort::Value::CreateTensor<int64_t>(
            mem, const_cast<int64_t*>(pitch.data()), pitch.size(), t_shape.data(), 2));
        names.push_back("pitchf");
        vals.push_back(Ort::Value::CreateTensor<float>(
            mem, const_cast<float*>(pitchf.data()), pitchf.size(), t_shape.data(), 2));
        names.push_back("ds");
        vals.push_back(Ort::Value::CreateTensor<int64_t>(
            mem, ds_val.data(), 1, one_shape.data(), 1));
        if (sig.has_rnd) {
            names.push_back("rnd");
            vals.push_back(Ort::Value::CreateTensor<float>(
                mem, const_cast<float*>(rnd.data()), rnd.size(), rnd_shape.data(), 3));
        }

        const char* out_names[] = {"audio"};
        try {
            auto out = voc_sess.Run(Ort::RunOptions{nullptr},
                                    names.data(), vals.data(), vals.size(),
                                    out_names, 1);
            auto oshape = out[0].GetTensorTypeAndShapeInfo().GetShape();
            int64_t M = oshape.back();
            if (audio_out) {
                const float* ad = out[0].GetTensorData<float>();
                audio_out->assign(ad, ad + M);
            }
            return M;
        } catch (const Ort::Exception&) {
            return -1;
        }
    };

    // ── Probe target sample rate ──────────────────────────────────────────────
    // RVC's frame rate is fixed at 100 fps, so sr = 100 × out_samples / frames.
    // Works for any export — no sidecar metadata needed.
    int tgt_sr;
    {
        constexpr int Tp = 8;
        std::vector<float>   p_phone((size_t)Tp * sig.phone_dim, 0.f);
        std::vector<int64_t> p_pitch((size_t)Tp, 1);
        std::vector<float>   p_pitchf((size_t)Tp, 0.f);
        std::vector<float>   p_rnd((size_t)Tp * sig.rnd_ch, 0.f);
        int64_t M = run_voice(p_phone, Tp, p_pitch, p_pitchf, p_rnd, nullptr);
        if (M <= 0) return "Voice model probe failed — incompatible ONNX graph";
        tgt_sr = (int)(M * 100 / Tp);
    }

    // ── HuBERT features (768-dim, or final_proj 256-dim for v1 models) ───────
    prog(0.20f, "Extracting phone features…");

    std::vector<int64_t> hub_shape = {1, (int64_t)N16};
    std::vector<Ort::Value> hub_in;
    hub_in.push_back(Ort::Value::CreateTensor<float>(
        mem, wav16.data(), wav16.size(), hub_shape.data(), 2));

    const char* hub_in_names[]  = {"audio"};
    const char* feat_name = (sig.phone_dim == 256) ? "features256" : "features";
    const char* hub_out_names[] = {feat_name};
    std::vector<Ort::Value> hub_out;
    try {
        hub_out = hub_sess.Run(
            Ort::RunOptions{nullptr},
            hub_in_names, hub_in.data(), 1,
            hub_out_names, 1);
    } catch (const Ort::Exception&) {
        return std::string("HuBERT model lacks output '") + feat_name +
               "' — re-export hubert.onnx from the ContentVec checkpoint";
    }

    // features: [1, T, 768]
    auto feat_info  = hub_out[0].GetTensorTypeAndShapeInfo();
    auto feat_shape = feat_info.GetShape();
    int  T_phone    = (int)feat_shape[1];
    int  D_phone    = (int)feat_shape[2];
    const float* fd = hub_out[0].GetTensorData<float>();
    std::vector<float> feats(fd, fd + (size_t)T_phone * D_phone);

    // ── Feature retrieval (faiss .index sibling, RVC index_rate) ─────────────
    // Pull each frame toward its nearest training-set features — tightens
    // timbre. Skipped when no <voice_stem>.index exists.
    {
        std::string ip = voice_onnx.substr(0, voice_onnx.rfind('.')) + ".index";
        if (fs::exists(ip)) {
            prog(0.45f, "Feature retrieval…");
            FaissIVF ivf = faiss_ivf_load(ip);
            if (!ivf.err.empty())
                return "Index load failed (" + ip + "): " + ivf.err;
            if (ivf.dim == D_phone) {
                constexpr float kIndexRate = 0.75f;
                faiss_ivf_blend(ivf, feats.data(), T_phone, kIndexRate);
            }
        }
    }

    // Repeat-interleave ×2: [1, T, D] → [1, 2T, D]
    int T2 = T_phone * 2;
    std::vector<float> phone((size_t)T2 * D_phone);
    for (int t = 0; t < T_phone; t++) {
        const float* src = feats.data() + (size_t)t * D_phone;
        float* d0 = phone.data() + (size_t)(2*t)   * D_phone;
        float* d1 = phone.data() + (size_t)(2*t+1) * D_phone;
        std::copy(src, src + D_phone, d0);
        std::copy(src, src + D_phone, d1);
    }

    // ── F0: RMVPE (neural pitch tracker) — required, no silent fallback ──────
    prog(0.55f, "Extracting pitch…");

    auto f0_raw = rmvpe_f0(wav16);
    if (f0_raw.empty())
        return "RMVPE pitch extraction failed (rmvpe.onnx missing or broken at "
               + rmvpe_onnx_path() + ")";
    auto f0 = interp_f0(f0_raw, T2);

    // ── Auto octave: trained-register mask from the sidecar ──────────────────
    // The exporter diffs emb_pitch against the pretrained base — embedding
    // rows only receive gradients for pitch bins seen in fine-tuning — and
    // stores the trained-bin mask. Pick the octave shift whose coverage of
    // the source's voiced bins is best (ties prefer 0). No synthesis needed.
    int auto_shift = 0;
    if (f0_auto) {
        bool mask[256] = {};
        bool have_mask = false;
        {
            std::string jp = voice_onnx.substr(0, voice_onnx.rfind('.')) + ".json";
            std::ifstream jf(jp);
            std::string js((std::istreambuf_iterator<char>(jf)), {});
            auto pos = js.find("\"register_mask\":\"");
            if (pos != std::string::npos) {
                pos += 17;
                auto end2 = js.find('"', pos);
                std::string hex = js.substr(pos, end2 - pos);
                if (hex.size() == 64) {
                    for (int i = 0; i < 64; i++) {
                        char c = hex[(size_t)i];
                        int nib = (c >= 'a') ? c - 'a' + 10 : c - '0';
                        for (int k = 0; k < 4; k++)
                            mask[i*4 + k] = (nib >> k) & 1;
                    }
                    have_mask = true;
                }
            }
        }
        if (have_mask) {
            // Erode the band edges (~3 st): pitch hugging the rim of the
            // trained register sounds strained even though it's "covered".
            bool core[256] = {};
            for (int b = 0; b < 256; b++) {
                bool ok = true;
                for (int k = -4; k <= 4 && ok; k++) {
                    int j = b + k;
                    ok = (j >= 0 && j < 256) ? mask[j] : false;
                }
                core[b] = ok;
            }
            std::memcpy(mask, core, sizeof(core));
            const float mel_min = 1127.f * std::log(1.f + 50.f   / 700.f);
            const float mel_max = 1127.f * std::log(1.f + 1100.f / 700.f);
            auto coverage = [&](int shift) {
                float mult = std::pow(2.f, shift / 12.f);
                int voiced = 0, in_mask = 0;
                for (int t = 0; t < T2; t++) {
                    float v = f0[(size_t)t];
                    if (v <= 0.f) continue;
                    voiced++;
                    float mel = 1127.f * std::log(1.f + v * mult / 700.f);
                    float b   = (mel - mel_min) * 254.f / (mel_max - mel_min) + 1.f;
                    int   bin = (int)std::lround(std::fmin(255.f, std::fmax(1.f, b)));
                    if (mask[bin]) in_mask++;
                }
                return voiced ? (float)in_mask / (float)voiced : 0.f;
            };
            float best = -1.f;
            for (int shift : {0, 12, -12}) {       // 0 first → wins ties
                float c = coverage(shift);
                if (c > best + 0.05f) { best = c; auto_shift = shift; }
            }
            char m[48];
            snprintf(m, sizeof(m), "Register: %+d st (coverage %.0f%%)",
                     auto_shift, best * 100.f);
            prog(0.58f, m);
        } else {
            prog(0.58f, "Register: auto unavailable (no mask)");
        }
    }

    int total_semis = f0_semitones + auto_shift;
    if (total_semis != 0) {
        float mult = std::pow(2.f, total_semis / 12.f);
        for (auto& v : f0) if (v > 0.f) v *= mult;
    }

    // ── Coarse mel pitch bins (RVC formula, caller-side) ─────────────────────
    // mel = 1127·ln(1 + f0/700); bins 1..255 spanning f0 50..1100 Hz.
    std::vector<int64_t> pitch_coarse((size_t)T2);
    {
        const float mel_min = 1127.f * std::log(1.f + 50.f   / 700.f);
        const float mel_max = 1127.f * std::log(1.f + 1100.f / 700.f);
        for (int t = 0; t < T2; t++) {
            float mel = 1127.f * std::log(1.f + f0[(size_t)t] / 700.f);
            float v   = (mel - mel_min) * 254.f / (mel_max - mel_min) + 1.f;
            pitch_coarse[(size_t)t] =
                (int64_t)std::lround(std::fmin(255.f, std::fmax(1.f, v)));
        }
    }

    // ── Sampling noise (seeded → reproducible output) ─────────────────────────
    // z = m_p + exp(logs_p)·rnd; 0.66666 is RVC's sampling temperature.
    std::vector<float> rnd((size_t)sig.rnd_ch * T2);
    {
        std::mt19937 rng(0x9e3779b9u);
        std::normal_distribution<float> gauss(0.f, 1.f);
        for (auto& v : rnd) v = gauss(rng) * 0.66666f;
    }

    // ── VITS synthesis ────────────────────────────────────────────────────────
    prog(0.65f, "Voice synthesis…");

    std::vector<float> audio_full;
    int64_t M = run_voice(phone, T2, pitch_coarse, f0, rnd, &audio_full);
    if (M <= 0) return "Voice synthesis failed";

    // ── Write output (trim the 0.5 s reflect padding) ─────────────────────────
    prog(0.92f, "Writing output…");
    int trim = tgt_sr / 2;
    int lo   = std::min((int64_t)trim, M);
    int hi   = (int)std::max((int64_t)lo, M - trim);
    std::vector<float> audio(audio_full.begin() + lo, audio_full.begin() + hi);
    if (!write_wav(output_wav, audio, tgt_sr))
        return "Failed to write output audio";

    prog(1.f, "Done");
    return {};
}
