// audio_whisper_coarse.cpp — Whisper coarse pass for v2 word alignment.
//
// Runs whisper.cpp (large-v3-turbo, the transcription model already in
// models/) in-process on the 16 kHz vocal audio, then maps each lyric line
// (in order) onto the decoded word stream with an order-constrained fuzzy
// match. Replaces the greedy-CTC coarse windows, which drifted whole lines
// by seconds on sung material (the quantized wav2vec2 model hallucinates
// long runs of '|' and vowel tokens in sung regions, so fuzzy token matching
// anchored lines to the wrong span; line-5's window landed ~5.8 s late).
//
// Whisper token timestamps (t0/t1, no DTW needed — DTW is disabled under
// flash_attn anyway) anchor lines reliably: on the reference clip all 7
// lines decode with the right text at the right times, including the
// 24.3–30 s instrumental gap the greedy decode filled with garbage.
#include "audio_whisper_coarse.h"

#include "paths.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <whisper.h>

namespace fs = std::filesystem;

namespace {

// Progress bridge: whisper calls back with 0..100 per encode; map to 0..1.
struct ProgBridge {
    const std::function<void(float, const char*)>* fn;
};
static void whisper_prog_cb(struct whisper_context*, struct whisper_state*,
                            int progress, void* user_data) {
    auto* b = static_cast<ProgBridge*>(user_data);
    if (b && b->fn && *b->fn) (*b->fn)(progress / 100.f, "Whisper coarse pass…");
}

std::string norm_tok(const std::string& w) {
    std::string o;
    for (char c : w) {
        if (std::isalpha((unsigned char)c)) o += (char)std::tolower((unsigned char)c);
        else if (c == '\'') o += '\'';
    }
    return o;
}

// Split a lyric line into normed word tokens.
std::vector<std::string> split_line(const std::string& ln) {
    std::vector<std::string> ws;
    std::string cur;
    for (char c : ln) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) {
                std::string n = norm_tok(cur);
                if (!n.empty()) ws.push_back(n);
                cur.clear();
            }
        } else cur += c;
    }
    if (!cur.empty()) {
        std::string n = norm_tok(cur);
        if (!n.empty()) ws.push_back(n);
    }
    return ws;
}

struct DecWord {
    std::string text;  // normed
    float t0 = 0.f, t1 = 0.f;
    int seg = -1;  // whisper segment index (for boundary floors/ceilings)
};

// Word-level edit similarity: 1 − lev/maxlen over normed strings.
float word_sim(const std::string& a, const std::string& b) {
    if (a == b) return 1.f;
    size_t n = a.size(), m = b.size();
    if (n == 0 || m == 0) return 0.f;
    std::vector<int> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; j++) prev[j] = (int)j;
    for (size_t i = 1; i <= n; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= m; j++)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1,
                               prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        prev.swap(cur);
    }
    return 1.f - (float)prev[m] / (float)std::max(n, m);
}

}  // namespace

bool whisper_coarse_windows(const std::vector<float>& audio16k,
                            const std::vector<std::string>& lyrics,
                            std::vector<std::pair<float, float>>& wins_out,
                            const std::function<void(float, const char*)>& progress,
                            std::string* err) {
    wins_out.clear();
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };
    if (audio16k.empty()) return fail("empty vocal pcm");
    if (lyrics.empty()) return fail("no lyrics");

    fs::path mp = fs::path(app_models_dir()) / "ggml-large-v3-turbo-q5_0.bin";
    if (!fs::exists(mp))
        return fail("Whisper model not found: " + mp.string());

    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = !getenv("PMS_WHISPER_CPU");
    // No DTW: flash_attn disables it anyway (probe showed t_dtw = −1 for all
    // tokens); plain token timestamps (t0/t1) are what we anchor on.
    whisper_context* ctx = whisper_init_from_file_with_params(mp.string().c_str(), cparams);
    if (!ctx) return fail("Failed to load whisper model");

    whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    wp.language = "en";
    wp.n_threads = 4;
    wp.token_timestamps = true;
    wp.thold_pt = 0.01f;
    wp.thold_ptsum = 0.01f;
    wp.print_progress = false;
    wp.print_realtime = false;
    wp.print_timestamps = false;
    wp.print_special = false;
    // Same recall-preserving settings as the transcription pipeline
    // (src/transcribe.cpp): keep past-text conditioning, temperature
    // fallback for repetition loops, logprob_thold −1.0 so quiet sung lines
    // aren't dropped as "no speech".
    wp.no_context = true;
    wp.no_speech_thold = 0.6f;
    wp.entropy_thold = 2.4f;
    wp.temperature = 0.0f;
    wp.temperature_inc = 0.2f;
    wp.logprob_thold = -1.0f;
    ProgBridge bridge{&progress};
    wp.progress_callback = whisper_prog_cb;
    wp.progress_callback_user_data = &bridge;

    int rc = whisper_full(ctx, wp, audio16k.data(), (int)audio16k.size());

    // Harvest words BEFORE freeing the context. deci.seg indexes into
    // seg_bounds (pushed in lockstep below), NOT the raw whisper segment id.
    std::vector<DecWord> dec;
    std::vector<std::pair<float, float>> seg_bounds;
    if (rc == 0) {
        whisper_token eot = whisper_token_eot(ctx);
        whisper_token beg = whisper_token_beg(ctx);
        int nsegs = whisper_full_n_segments(ctx);
        for (int i = 0; i < nsegs; i++) {
            double st0 = whisper_full_get_segment_t0(ctx, i) / 100.0;
            double st1 = whisper_full_get_segment_t1(ctx, i) / 100.0;
            if (st1 <= st0) continue;  // degenerate: skip entirely so deci.seg
                                       // stays a valid seg_bounds index
            seg_bounds.push_back({(float)st0, (float)st1});
            int sbi = (int)seg_bounds.size() - 1;
            int ntok = whisper_full_n_tokens(ctx, i);
            std::string cur;
            double w0 = 0.0, w1 = 0.0;
            auto emit = [&]() {
                std::string n = norm_tok(cur);
                if (!n.empty()) dec.push_back({n, (float)w0, (float)w1, sbi});
                cur.clear();
            };
            for (int j = 0; j < ntok; j++) {
                whisper_token id = whisper_full_get_token_id(ctx, i, j);
                if (id >= eot || id == beg) continue;
                const char* raw = whisper_full_get_token_text(ctx, i, j);
                if (!raw) continue;
                std::string t(raw);
                whisper_token_data td = whisper_full_get_token_data(ctx, i, j);
                double t0 = td.t0 / 100.0, t1 = td.t1 / 100.0;
                if (t1 <= 0.0) continue;  // no timestamp for this token
                bool new_word = !t.empty() && t[0] == ' ';
                if (new_word && !cur.empty()) emit();
                std::string stripped = (!t.empty() && t[0] == ' ') ? t.substr(1) : t;
                if (cur.empty()) {
                    cur = stripped;
                    w0 = t0;
                    w1 = t1;
                } else {
                    cur += stripped;
                    w1 = t1;
                }
            }
            if (!cur.empty()) emit();
        }
    }
    whisper_free(ctx);
    if (rc != 0) return fail("Whisper inference failed");
    if (dec.empty()) return fail("whisper decoded no words");

    double dur = (double)audio16k.size() / 16000.0;

    // ── Order-constrained line→word-span match ─────────────────────────────
    // The CTC needs windows that START at (or just before) the line's true
    // onset: a window starting late collapses the first words onto frame 0
    // (probe: line4's "Or/maybe/they" at 16.1–16.3 with conf ~0 when the
    // window started at 16.12 instead of 15.5 — the true onset is 15.72).
    // So windows are anchored to whisper's TOKEN TIMES: for each line in
    // order, find the decoded word most similar to the line's first word
    // (≥0.5) at/after the previous line's end (reach back 2 words), then
    // the decoded word most similar to the line's LAST word after that.
    // Window = [first-word start − 0.3, last-word end + 1.2], clamped and
    // never before the previous line's end. Two segment-boundary rules keep
    // lines from reaching across phrase boundaries (whisper merges short
    // lyric lines into one segment — seg2 holds lines 2+3):
    //   floor: a line's start never goes before its first word's segment
    //     start − 0.3 (line3's "would" @12.53 lives in seg2@10.22, so line3
    //     starts at 9.92, not 12.23 — which would cut Maybe/they/imagined);
    //   split: lines sharing one segment split it at the next line's
    //     first-word onset. Unmatched lines get even splits — never
    //     overlapping an anchor.
    size_t nd = dec.size(), nl = lyrics.size();
    std::vector<std::vector<std::string>> ltoks;
    for (auto& ln : lyrics) ltoks.push_back(split_line(ln));

    struct Span {
        bool ok = false;
        size_t a = 0, b = 0;
    };
    std::vector<Span> spans(nl);
    size_t pos = 0;
    for (size_t li = 0; li < nl; li++) {
        auto& lw = ltoks[li];
        if (lw.empty()) continue;
        size_t start_lo = (pos > 2) ? pos - 2 : 0;
        // First word: best match in a bounded lookahead (2× line length —
        // the true onset is near, not minutes away). Ties prefer the
        // EARLIEST index so a repeated word ("their" ×2 in seg2) anchors
        // the first occurrence, not the last.
        float best_s = 0.5f;
        size_t best_a = nd;
        size_t look = std::min(nd, start_lo + 2 * lw.size() + 6);
        for (size_t a = start_lo; a < look; a++) {
            float s = word_sim(lw.front(), dec[a].text);
            if (s > best_s + 1e-6f) {
                best_s = s;
                best_a = a;
            }
        }
        if (best_a == nd) continue;  // first word not found → even split
        // Last word: best match after the first IN THE SAME SEGMENT (a line
        // never spans a whisper segment boundary — the boundary marks a
        // real pause/gap), within line length + gap.
        float best_e = 0.5f;
        size_t best_b = nd;
        size_t elook = std::min(nd, best_a + lw.size() + 8);
        for (size_t b = best_a; b < elook; b++) {
            if (dec[b].seg != dec[best_a].seg) break;
            float s = word_sim(lw.back(), dec[b].text);
            if (s >= best_e) {
                best_e = s;
                best_b = b + 1;
            }
        }
        if (best_b == nd) {
            // Last word not found in-segment (whisper mis-heard it): end at
            // the segment's last word so the window still covers the phrase
            // without crossing the boundary.
            size_t e = best_a;
            while (e + 1 < nd && dec[e + 1].seg == dec[best_a].seg &&
                   e + 1 < best_a + lw.size() + 8)
                e++;
            best_b = e + 1;
        }
        spans[li] = {true, best_a, best_b};
        pos = best_b;
    }

    wins_out.resize(nl);
    float prev_end = 0.f;
    // Precompute split ends: lines sharing one segment split at the next
    // anchored line's first-word onset. Computed up front (not inline during
    // window assembly) so the earlier line's end is final before the later
    // line clamps its start to it.
    std::vector<float> split_end(nl, -1.f);
    for (size_t li = 0; li < nl; li++) {
        if (!spans[li].ok) continue;
        int sg = dec[spans[li].a].seg;
        for (size_t k = li + 1; k < nl; k++) {
            if (!spans[k].ok) continue;
            if (dec[spans[k].a].seg == sg)
                split_end[li] = dec[spans[k].a].t0 - 0.05f;
            break;
        }
    }
    // Window starts use SEGMENT starts (minus a small cushion): the segment
    // boundary is whisper's phrase onset and always sits before the line's
    // true onset (probe: seg1 starts 5.38, line1's They at 5.94; windows
    // [5.08,N] align They=5.94/1.00 for any end, while [6.57,N] collapses
    // They onto frame 0 with conf 0). The matched first word's time is NOT
    // used: whisper mis-hears sung onsets ("They" decoded at 5.62, 0.3 s
    // early of the true 5.94). A line sharing its segment with an earlier
    // line starts at that line's END (the split point). The prev_end clamp
    // in the assembly loop is REMOVED for matched lines: it pushed word-
    // anchored windows late (line1's 5.32 → 6.57 via line0's 6.52 end) and
    // caused exactly the collapse this scheme avoids; overlaps are harmless
    // (each line aligns only its own lyric text).
    std::vector<float> win_start(nl, 0.f);
    for (size_t li = 0; li < nl; li++) {
        if (!spans[li].ok) continue;
        int sg = dec[spans[li].a].seg;
        float s = (sg >= 0 && sg < (int)seg_bounds.size())
                      ? seg_bounds[sg].first - 0.30f
                      : dec[spans[li].a].t0 - 0.30f;
        if (sg >= 0) {
            for (size_t k = 0; k < li; k++) {
                if (!spans[k].ok) continue;
                if (dec[spans[k].a].seg == sg && split_end[k] > 0.f) {
                    s = split_end[k];
                    break;
                }
            }
        }
        if (s < 0.f) s = 0.f;
        win_start[li] = s;
    }
    for (size_t li = 0; li < nl; li++) {
        if (spans[li].ok) {
            // Window starts are precomputed segment starts (see win_start):
            // the matched first word is only a guess at the line start
            // (whisper mis-hears sung onsets), while the segment boundary
            // is the reliable phrase onset and the CTC finds real onsets
            // inside the window (probe: line1 wide 5.4–10.22 → They=5.94
            // vs tight 6.57–11.16 → They=6.57/0.00 collapsed onto frame 0).
            float s = win_start[li];
            float e = dec[spans[li].b - 1].t1 + 1.20f;
            if (split_end[li] > 0.f) e = std::min(e, split_end[li]);
            // No prev_end clamp: segment starts already order the lines and
            // the clamp pushed windows late (line1's 5.32 → 6.57), collapsing
            // first words onto frame 0. Overlaps are harmless.
            if (e < s + 0.5f) e = s + 0.5f;
            if (e > (float)dur) e = (float)dur;
            wins_out[li] = {s < 0.f ? 0.f : s, e};
            prev_end = e;
        } else {
            // Even split of the remaining span among this and following
            // unmatched lines (matched later lines keep their anchors).
            size_t rem = 0;
            for (size_t k = li; k < nl; k++)
                if (!spans[k].ok) rem++;
            float span_end = (float)dur;
            for (size_t k = li + 1; k < nl; k++)
                if (spans[k].ok) {
                    span_end = dec[spans[k].a].t0 - 0.25f;
                    break;
                }
            float w = (span_end - prev_end) / (float)std::max<size_t>(rem, 1);
            float s = prev_end, e = prev_end + w;
            if (e > (float)dur) e = (float)dur;
            wins_out[li] = {s < 0.f ? 0.f : s, e};
            prev_end = e;
        }
    }
    if (getenv("PMS_AA_DEBUG")) {
        for (size_t s = 0; s < seg_bounds.size(); s++)
            fprintf(stderr, "[wcoarse] seg%zu %.2f-%.2f\n", s,
                    seg_bounds[s].first, seg_bounds[s].second);
        for (size_t li = 0; li < nl; li++)
            fprintf(stderr, "[wcoarse] line%zu win=%.2f-%.2f %s (a=%zu b=%zu sg=%d wt=%.2f '%s')\n",
                    li, wins_out[li].first, wins_out[li].second,
                    spans[li].ok ? "anchored" : "even-split",
                    spans[li].a, spans[li].b,
                    spans[li].ok ? dec[spans[li].a].seg : -1,
                    spans[li].ok ? (double)dec[spans[li].a].t0 : -1.0,
                    spans[li].ok ? dec[spans[li].a].text.c_str() : "");
    }
    return true;
}
