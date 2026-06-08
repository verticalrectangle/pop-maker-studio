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

static constexpr double SAMPLE_RATE  = 16000.0;
static constexpr int    WAV2VEC2_MIN = 400;    // minimum samples wav2vec2 accepts
static constexpr float  SEG_GAP      = 0.40f;  // fallback gap for segment splitting

// ── Vocab ─────────────────────────────────────────────────────────────────────

struct AlignVocab {
    int blank   = 0;
    int wordsep = 0;
    std::unordered_map<char, int> c2i;
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

// ── stay-advance trellis (torchaudio forced alignment) ───────────────────────
//
// Based on:
//   https://pytorch.org/tutorials/intermediate/forced_alignment_with_torchaudio_tutorial.html
//
// State j = "number of tokens consumed so far" (0..L).
// At each frame: stay at j (emit blank) or advance to j+1 (emit tokens[j]).
// Unlike the standard CTC Viterbi there is no blank-padded expanded sequence
// and no skip-blank transition — the trellis is (T+1) × (L+1).

struct PathPoint {
    int   token_idx;  // 0-based index into target[]
    int   time_idx;   // 0-based frame within the segment's emission matrix
    float score;      // probability (not log-prob) at this frame
};

// A merged character span: one token, spanning [start, end) frames.
struct CharSeg {
    int   start;   // first frame (inclusive)
    int   end;     // last frame (exclusive)
    float score;   // mean probability over frames
};

// Build the (T+1) × (L+1) trellis in log-prob space.
static std::vector<float> get_trellis(
    const float* lp, int T, int V, int blank,
    const std::vector<int>& tokens)
{
    int L = (int)tokens.size();
    std::vector<float> tr((size_t)(T+1) * (L+1), -1e30f);
    auto cell = [&](int t, int j) -> float& { return tr[(size_t)t*(L+1)+j]; };

    cell(0, 0) = 0.f;

    // Column 0: cumulative blank score (no tokens consumed yet)
    float cum = 0.f;
    for (int t = 0; t < T; ++t) {
        cum += lp[(size_t)t * V + blank];
        cell(t+1, 0) = cum;
    }
    // Force consuming all L tokens: last L rows of column 0 = +inf.
    // This makes backtrack always find t_start near the end of the segment.
    // BUG: this propagates into columns 1..L and collapses all tokens into the
    // last L frames, producing severely wrong timestamps.  Disabled.
    // for (int t = std::max(0, T + 1 - L); t <= T; ++t)
    //     cell(t, 0) = 1e30f;

    // Forward DP
    for (int t = 0; t < T; ++t) {
        float bs = lp[(size_t)t * V + blank];   // blank score at frame t
        for (int j = 1; j <= L; ++j) {
            float stay    = cell(t, j)   + bs;
            float advance = cell(t, j-1) + lp[(size_t)t * V + tokens[j-1]];
            cell(t+1, j) = std::max(stay, advance);
        }
    }
    return tr;
}

// Backtrack through the trellis.  Returns empty on failure.
static std::vector<PathPoint> backtrack(
    const std::vector<float>& tr, int T, int L,
    const float* lp, int V, int blank,
    const std::vector<int>& tokens)
{
    auto cell = [&](int t, int j) { return tr[(size_t)t*(L+1)+j]; };

    // Find the frame where all tokens are optimally consumed
    int   t_start = 0;
    float best    = cell(0, L);
    for (int t = 1; t <= T; ++t)
        if (cell(t, L) > best) { best = cell(t, L); t_start = t; }

    std::vector<PathPoint> path;
    path.reserve(t_start);
    int j = L;

    for (int t = t_start; t > 0; --t) {
        float stayed  = cell(t-1, j)   + lp[(size_t)(t-1)*V + blank];
        float changed = cell(t-1, j-1) + lp[(size_t)(t-1)*V + tokens[j-1]];
        bool  adv     = changed > stayed;
        float prob    = std::exp(lp[(size_t)(t-1)*V + (adv ? tokens[j-1] : blank)]);
        path.push_back({j-1, t-1, prob});
        if (adv) { if (--j == 0) break; }
    }

    if (j != 0) return {};           // failed to consume all tokens
    std::reverse(path.begin(), path.end());
    return path;
}

// Merge consecutive same-token path entries → one CharSeg per token (0..L-1).
// Because the path is monotonically non-decreasing in token_idx, result[i]
// always corresponds to tokens[i].
static std::vector<CharSeg> merge_repeats(const std::vector<PathPoint>& path, int L)
{
    std::vector<CharSeg> segs(L, {0, 0, 0.f});
    if (path.empty()) return segs;

    int n = (int)path.size(), i1 = 0;
    while (i1 < n) {
        int  tok = path[i1].token_idx;
        int  i2  = i1;
        while (i2 < n && path[i2].token_idx == tok) ++i2;
        float sum = 0.f;
        for (int k = i1; k < i2; ++k) sum += path[k].score;
        segs[tok] = {path[i1].time_idx, path[i2-1].time_idx + 1, sum / (i2 - i1)};
        i1 = i2;
    }
    return segs;
}

// ── Public API ────────────────────────────────────────────────────────────────
//
// For each whisper segment [t1, t2]:
//   1. Slice audio to exactly [t1, t2] (no extra padding).
//   2. Run wav2vec2 ONNX on that slice → T × V log-probs.
//   3. Build the character target for words in this segment (letters + "|").
//   4. get_trellis → backtrack → merge_repeats → one CharSeg per target char.
//   5. Map char frame indices to absolute time: t_abs = t1 + frame * ratio
//      where ratio = (t2 - t1) / T.
//   6. word.start = min char start, word.end = max char end.
//   7. Words with no vocabulary-mapped chars get NaN — filled by linear
//      interpolation between the nearest aligned neighbours afterward.

std::vector<WordEntry> forced_align(
    const std::vector<float>&               audio16k,
    const std::vector<WordEntry>&           whisper_words,
    double                                  proxy_fps,
    const std::vector<std::pair<float,float>>& seg_bounds)
{
    if (audio16k.empty() || whisper_words.empty()) return {};

    std::string mp = wav2vec2_ctc_path();
    if (!fs::exists(mp)) return {};

    AlignVocab vocab = load_vocab(mp);
    if (!vocab.ok) return {};

    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "forced_align");
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(2);
    Ort::Session sess(env, mp.c_str(), opts);
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    bool need_mask = (sess.GetInputCount() >= 2);

    auto snap = [&](float t) -> float {
        return (proxy_fps > 0.0) ? (float)(std::round((double)t * proxy_fps) / proxy_fps) : t;
    };

    int    N         = (int)whisper_words.size();
    double total_dur = (double)audio16k.size() / SAMPLE_RATE;
    (void)total_dur;

    // ── Build segment list ────────────────────────────────────────────────────
    // Each segment: word range [w0, w1] (inclusive) and audio window [t0, t1].

    struct Seg { int w0, w1; float t0, t1; };
    std::vector<Seg> segs;

    if (!seg_bounds.empty()) {
        // Use whisper's actual segment boundaries.
        // Walk word list in order; assign each word to the segment whose window
        // contains its start time.
        int wi = 0;
        for (auto& [st, en] : seg_bounds) {
            if (wi >= N) break;
            int w0 = wi;
            // Advance past words that start before this segment's end.
            // Use a small epsilon to handle floating-point boundary alignment.
            while (wi < N && whisper_words[wi].start < en - 0.001f) ++wi;
            int w1 = wi - 1;
            if (w0 <= w1)
                segs.push_back({w0, w1, st, en});
        }
        // Any words not covered by a segment (rare timing edge case)
        if (wi < N) {
            float t0 = whisper_words[wi].start;
            float t1 = std::max(whisper_words[N-1].end, t0 + 0.1f);
            segs.push_back({wi, N-1, t0, t1});
        }
    } else {
        // Fallback: reconstruct segments from inter-word pauses.
        int sw = 0;
        for (int i = 1; i < N; ++i) {
            if (whisper_words[i].start - whisper_words[i-1].end > SEG_GAP) {
                segs.push_back({sw, i-1,
                    whisper_words[sw].start,
                    whisper_words[i-1].end});
                sw = i;
            }
        }
        segs.push_back({sw, N-1,
            whisper_words[sw].start,
            whisper_words[N-1].end});
    }

    // ── Per-segment alignment ─────────────────────────────────────────────────

    std::vector<WordEntry> result(whisper_words);
    std::vector<bool>      aligned(N, false);

    for (auto& seg : segs) {
        // Exact audio window — no extra padding
        int s0 = (int)(seg.t0 * SAMPLE_RATE);
        int s1 = std::min((int)(seg.t1 * SAMPLE_RATE), (int)audio16k.size());
        if (s1 <= s0) continue;

        std::vector<float> chunk(audio16k.begin() + s0, audio16k.begin() + s1);

        // Pad to wav2vec2 minimum (400 samples = 25 ms at 16 kHz)
        if ((int)chunk.size() < WAV2VEC2_MIN)
            chunk.resize(WAV2VEC2_MIN, 0.f);

        size_t CN = chunk.size();

        // Per-segment zero-mean unit-variance normalisation
        float mean = 0.f; for (float x : chunk) mean += x; mean /= (float)CN;
        float var  = 0.f; for (float x : chunk) var += (x-mean)*(x-mean); var /= (float)CN;
        float istd = 1.f / (std::sqrt(var) + 1e-7f);
        for (float& x : chunk) x = (x - mean) * istd;

        // ONNX inference
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
        const char* in_names[]  = {"input_values", "attention_mask"};
        const char* out_names[] = {"logits"};
        std::vector<Ort::Value> outs;
        try {
            outs = sess.Run(Ort::RunOptions{nullptr},
                in_names, ins.data(), need_mask ? 2u : 1u, out_names, 1);
        } catch (...) { continue; }

        auto sh = outs[0].GetTensorTypeAndShapeInfo().GetShape();
        int T = (int)sh[1], V = (int)sh[2];
        if (T == 0 || V == 0) continue;

        const float* raw = outs[0].GetTensorData<float>();
        std::vector<float> lp(raw, raw + (size_t)T * V);
        for (int t = 0; t < T; ++t) log_softmax_row(lp.data() + (size_t)t * V, V);

        // seconds per output frame — maps frame index → time offset from seg.t0
        double ratio = (seg.t1 - seg.t0) / T;

        // Build character target; record each word's char index range
        std::vector<int> target;
        struct WRange { int ci0, ci1; };   // inclusive char indices into target[]
        std::vector<WRange> wranges;

        for (int k = seg.w0; k <= seg.w1; ++k) {
            if (k > seg.w0) target.push_back(vocab.wordsep);
            int ci0 = (int)target.size();
            for (char c : whisper_words[k].text) {
                char lc = (char)std::tolower((unsigned char)c);
                auto it = vocab.c2i.find(lc);
                if (it != vocab.c2i.end()) target.push_back(it->second);
            }
            wranges.push_back({ci0, (int)target.size() - 1});
        }

        if (target.empty() || T < (int)target.size()) continue;

        // Trellis → path → CharSegs (one per target character, index-matched)
        auto tr    = get_trellis(lp.data(), T, V, vocab.blank, target);
        auto path  = backtrack(tr, T, (int)target.size(), lp.data(), V, vocab.blank, target);
        if (path.empty()) continue;
        auto csegs = merge_repeats(path, (int)target.size());

        // Extract word timestamps from character spans:
        //   word.start = min char start,  word.end = max char end
        //   word.score = mean char score
        for (int k = 0; k < (int)wranges.size(); ++k) {
            auto& r = wranges[k];
            if (r.ci0 > r.ci1) continue;   // no vocab-mapped chars → NaN, interpolate later

            int   fmin  = csegs[r.ci0].start;
            int   fmax  = csegs[r.ci0].end;
            float score = 0.f;
            for (int ci = r.ci0; ci <= r.ci1; ++ci) {
                fmin  = std::min(fmin,  csegs[ci].start);
                fmax  = std::max(fmax,  csegs[ci].end);
                score += csegs[ci].score;
            }
            score /= (float)(r.ci1 - r.ci0 + 1);

            float t0_abs = snap((float)(seg.t0 + fmin * ratio));
            float t1_abs = snap((float)(seg.t0 + fmax * ratio));

            result[seg.w0 + k].start = t0_abs;
            result[seg.w0 + k].end   = t1_abs;
            aligned[seg.w0 + k]      = true;

            (void)score;  // available for future score-gating if needed
        }
    }

    // ── NaN interpolation (method=nearest) ────────────────────────────────────
    // For each unaligned word, linearly interpolate its start/end between
    // the nearest aligned neighbours.  Falls back to the original Whisper
    // timestamp if there are no aligned neighbours on one or both sides.

    for (int i = 0; i < N; ++i) {
        if (aligned[i]) continue;

        // Find nearest aligned neighbours
        int left  = -1, right = -1;
        for (int j = i - 1; j >= 0; --j) { if (aligned[j]) { left  = j; break; } }
        for (int j = i + 1; j < N;  ++j) { if (aligned[j]) { right = j; break; } }

        float l_end   = (left  >= 0) ? result[left].end   : result[i].start; // Whisper fallback
        float r_start = (right >= 0) ? result[right].start : result[i].end;

        float gap = r_start - l_end;
        if (gap <= 0.f) {
            // No room — snap to the boundary
            result[i].start = snap(l_end);
            result[i].end   = snap(l_end);
            continue;
        }

        // Count mappable chars for proportional distribution within this run.
        // Collect the whole unaligned run so proportions are consistent.
        int run_end = i;
        while (run_end + 1 < N && !aligned[run_end + 1]) ++run_end;

        // Total mappable chars in run
        auto mapped = [&](int k) {
            int n = 0;
            for (char c : whisper_words[k].text) {
                char lc = (char)std::tolower((unsigned char)c);
                if (vocab.c2i.count(lc)) ++n;
            }
            return std::max(n, 1);   // at least 1 so unmappable words still get a slot
        };

        int total_chars = 0;
        for (int k = i; k <= run_end; ++k) total_chars += mapped(k);

        float t = l_end;
        for (int k = i; k <= run_end; ++k) {
            float dur = gap * (float)mapped(k) / (float)total_chars;
            result[k].start = snap(t);
            result[k].end   = snap(t + dur);
            t += dur;
        }

        i = run_end;   // skip rest of run (already handled)
    }

    return result;
}
