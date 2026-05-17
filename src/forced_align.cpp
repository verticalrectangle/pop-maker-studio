#include "forced_align.h"
#include "paths.h"
#include "json.hpp"
#include <onnxruntime_cxx_api.h>
#include <fstream>
#include <filesystem>
#include <cmath>
#include <algorithm>
#include <unordered_map>

namespace fs = std::filesystem;

static constexpr float  NEG_INF     = -1e30f;
static constexpr double CHUNK_DUR   = 30.0;    // seconds per alignment window
static constexpr double SAMPLE_RATE = 16000.0;

// ── Vocab ─────────────────────────────────────────────────────────────────────

struct AlignVocab {
    int blank   = 0;   // <pad> — CTC blank token
    int wordsep = 0;   // |    — word boundary token
    std::unordered_map<char, int> c2i;  // lowercase char → vocab index
    bool ok = false;
};

static AlignVocab load_vocab(const std::string& model_path) {
    AlignVocab v;
    fs::path vp = fs::path(model_path).parent_path() / "wav2vec2_vocab.json";
    std::ifstream f(vp.string());
    if (!f) return v;
    try {
        auto j = nlohmann::json::parse(f);
        for (auto& [key, val] : j.items()) {
            int idx = val.get<int>();
            if (key == "<pad>" || key == "[PAD]") v.blank = idx;
            else if (key == "|")  { v.wordsep = idx; v.c2i[' '] = idx; }
            else if (key == "'")    v.c2i['\''] = idx;
            else if (key.size() == 1 && std::isalpha((unsigned char)key[0]))
                v.c2i[(char)std::tolower((unsigned char)key[0])] = idx;
        }
        v.ok = !v.c2i.empty();
    } catch (...) {}
    return v;
}

// ── Log-softmax ───────────────────────────────────────────────────────────────

static void log_softmax_row(float* row, int n) {
    float mx = *std::max_element(row, row + n);
    float s = 0.f;
    for (int i = 0; i < n; ++i) { row[i] -= mx; s += std::exp(row[i]); }
    float ls = std::log(s);
    for (int i = 0; i < n; ++i) row[i] -= ls;
}

// ── Viterbi CTC forced alignment ─────────────────────────────────────────────
// Returns char_times[L]: the first frame index at which each target[i] is "active"
// in the best Viterbi path.

static std::vector<int> ctc_align(
    const float* lp, int T, int V,
    const std::vector<int>& target, int blank)
{
    int L = (int)target.size();
    if (L == 0 || T < L) return {};

    // Blank-padded expanded target: [B, t0, B, t1, ..., tL, B], length = 2L+1
    std::vector<int> exp(2*L+1);
    for (int i = 0; i <= L; ++i) {
        exp[2*i] = blank;
        if (i < L) exp[2*i+1] = target[i];
    }
    int S = 2*L+1;

    // Two-row DP + full back-pointer matrix for backtracking
    std::vector<float> dp(S, NEG_INF), ndp(S, NEG_INF);
    // back[t*S+s]: signed offset to predecessor state (0, -1, or -2)
    std::vector<int8_t> back((size_t)T * S, 0);

    // Initialise t = 0
    dp[0] = lp[exp[0]];
    if (S > 1) dp[1] = lp[exp[1]];

    for (int t = 1; t < T; ++t) {
        const float* row = lp + (size_t)t * V;
        for (int s = 0; s < S; ++s) {
            float best = dp[s]; int8_t from = 0;
            if (s >= 1 && dp[s-1] > best) { best = dp[s-1]; from = -1; }
            // Skip-blank transition: only when target char differs from two positions back
            if (s >= 2 && exp[s] != blank && exp[s] != exp[s-2] && dp[s-2] > best)
                { best = dp[s-2]; from = -2; }
            ndp[s]  = (best > NEG_INF) ? best + row[exp[s]] : NEG_INF;
            back[(size_t)t * S + s] = from;
        }
        std::swap(dp, ndp);
    }

    // Backtrack from the best terminal state (last char or last blank)
    int s = (S >= 2 && dp[S-2] > dp[S-1]) ? S-2 : S-1;
    std::vector<int> path(T);
    path[T-1] = s;
    for (int t = T-1; t > 0; --t) {
        s += (int)back[(size_t)t * S + s];
        path[t-1] = s;
    }

    // First frame at which each non-blank state 2i+1 appears on the best path
    std::vector<int>  times(L, T-1);
    std::vector<bool> found(L, false);
    for (int t = 0; t < T; ++t) {
        int st = path[t];
        if (st % 2 == 1) {
            int ci = st / 2;
            if (ci < L && !found[ci]) { times[ci] = t; found[ci] = true; }
        }
    }
    return times;
}

// ── Public API ────────────────────────────────────────────────────────────────

std::vector<WordEntry> forced_align(
    const std::vector<float>& audio16k,
    const std::vector<WordEntry>& whisper_words,
    double proxy_fps)
{
    if (audio16k.empty() || whisper_words.empty()) return {};

    std::string mp = wav2vec2_ctc_path();
    if (!fs::exists(mp)) return {};

    AlignVocab vocab = load_vocab(mp);
    if (!vocab.ok) return {};

    // One ONNX session reused across all chunks
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "forced_align");
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(2);
    Ort::Session sess(env, mp.c_str(), opts);
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // Detect whether the model expects an attention_mask input
    bool need_mask = (sess.GetInputCount() >= 2);

    auto snap = [&](float t) -> float {
        return (proxy_fps > 0.0) ? (float)(std::round((double)t * proxy_fps) / proxy_fps) : t;
    };

    // Start with Whisper timestamps as fallback
    std::vector<WordEntry> result(whisper_words);
    double total_dur = (double)audio16k.size() / SAMPLE_RATE;

    // Process in CHUNK_DUR-second windows to keep the Viterbi DP matrix small
    for (double ct0 = 0.0; ct0 < total_dur; ct0 += CHUNK_DUR) {
        double ct1 = std::min(ct0 + CHUNK_DUR, total_dur);

        // Words whose Whisper start time falls in this chunk
        std::vector<int> widxs;
        for (int i = 0; i < (int)whisper_words.size(); ++i)
            if ((double)whisper_words[i].start >= ct0 && (double)whisper_words[i].start < ct1)
                widxs.push_back(i);
        if (widxs.empty()) continue;

        // Extract and normalise chunk
        int s0 = (int)(ct0 * SAMPLE_RATE);
        int s1 = std::min((int)(ct1 * SAMPLE_RATE), (int)audio16k.size());
        if (s1 <= s0) continue;
        size_t CN = (size_t)(s1 - s0);
        std::vector<float> chunk(audio16k.begin() + s0, audio16k.begin() + s1);

        // Waveform normalisation expected by wav2vec2 feature extractor
        float mean = 0.f; for (float x : chunk) mean += x; mean /= (float)CN;
        float var  = 0.f; for (float x : chunk) var += (x-mean)*(x-mean); var /= (float)CN;
        float istd = 1.f / (std::sqrt(var) + 1e-7f);
        for (float& x : chunk) x = (x - mean) * istd;

        // Run ONNX inference
        std::vector<int64_t> in_shape = {1, (int64_t)CN};
        std::vector<Ort::Value> ins;
        ins.push_back(Ort::Value::CreateTensor<float>(
            mem, chunk.data(), CN, in_shape.data(), 2));

        std::vector<float> attn_mask;
        if (need_mask) {
            attn_mask.assign(CN, 1.0f);
            ins.push_back(Ort::Value::CreateTensor<float>(
                mem, attn_mask.data(), CN, in_shape.data(), 2));
        }

        const char* in_names2[] = {"input_values", "attention_mask"};
        const char* out_names[] = {"logits"};
        int n_in = need_mask ? 2 : 1;

        std::vector<Ort::Value> outs;
        try {
            outs = sess.Run(Ort::RunOptions{nullptr},
                in_names2, ins.data(), (size_t)n_in, out_names, 1);
        } catch (...) { continue; }

        auto sh = outs[0].GetTensorTypeAndShapeInfo().GetShape();
        int T = (int)sh[1], V = (int)sh[2];
        if (T == 0 || V == 0) continue;

        // Copy logits and apply log-softmax (ONNX output may be quantized / packed)
        const float* lp_raw = outs[0].GetTensorData<float>();
        std::vector<float> lp(lp_raw, lp_raw + (size_t)T * V);
        for (int t = 0; t < T; ++t) log_softmax_row(lp.data() + (size_t)t * V, V);

        // seconds per model output frame (computed from actual T vs chunk samples)
        double frame_dt = (double)CN / (SAMPLE_RATE * T);

        // Build character target for words in this chunk; record per-word char ranges
        std::vector<int> target;
        struct WRange { int ci0, ci1; };    // inclusive char indices in target
        std::vector<WRange> ranges;

        for (int k = 0; k < (int)widxs.size(); ++k) {
            if (k > 0) target.push_back(vocab.wordsep);
            int ci0 = (int)target.size();
            for (char c : whisper_words[widxs[k]].text) {
                char lc = (char)std::tolower((unsigned char)c);
                auto it = vocab.c2i.find(lc);
                if (it != vocab.c2i.end()) target.push_back(it->second);
            }
            int ci1 = (int)target.size() - 1;
            ranges.push_back({ci0, ci1});
        }
        if (target.empty()) continue;

        // Run CTC forced alignment (Viterbi)
        std::vector<int> times = ctc_align(lp.data(), T, V, target, vocab.blank);
        if (times.empty()) continue;

        // Map char frame times → word start/end seconds, then snap to proxy frames
        for (int k = 0; k < (int)widxs.size(); ++k) {
            auto& r = ranges[k];
            if (r.ci0 > r.ci1 || r.ci0 >= (int)times.size()) continue;

            float t0_abs = (float)(ct0 + times[r.ci0] * frame_dt);

            // End time = first frame of the next char (separator or past-end)
            int end_ci = r.ci1 + 1;
            float t1_abs;
            if (end_ci < (int)times.size())
                t1_abs = (float)(ct0 + times[end_ci] * frame_dt);
            else
                t1_abs = (float)(ct0 + (times[r.ci1] + 1) * frame_dt);

            result[widxs[k]].start = snap(t0_abs);
            result[widxs[k]].end   = snap(t1_abs);
        }
    }

    return result;
}
