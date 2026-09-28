// audio_analysis_run.cpp — analysis v2 (docs/AUDIO_ANALYSIS.md).
//
// Blocking full analysis; run on a worker thread. progress(0..1, stage).
//
// Pipeline (all times are source-file seconds):
//   1. Decode the mix to 44.1 kHz mono; also decode each stem.
//   2. Stems: with_separation ? separate_stems4() : injected stems_dir (a real
//      feature — reuse of precomputed stems) : no-stems fallback. Callers
//      cache results (audio_analysis_cache_file, media cache dir).
//   3. Beats/bpm: librosa-faithful beat tracking on the MIX (never the vocal
//      stem), trim off (the DP tail select keeps the final frame; clipping to
//      the file then matches the reference).
//   4. Downbeat phase: bass onset at beats + chroma novelty (spec §downbeats).
//      Downbeats = beats at that phase. Without a bass stem the phase falls
//      back to the instrumental (original − vocals).
//   5. Hits: band flux on drums stem (kick 30–150, snare 180–4k, hat 7–16k),
//      onset strength on bass/other/vocal stems; greedy peak picking with the
//      spec's q/wait per kind; strength = peak / p90(peaks), clipped.
//      Fallback (no drums stem): band flux on the instrumental.
//   6. Envelopes: RMS per 60 fps frame (frame_length 2048), /p98, clip.
//   7. Spectrum: mel power, 32 bands 30 Hz–16 kHz, n_fft 4096, hop = sr/fps,
//      center=False, dB rel max, (dB+70)/70 → 0..99.
//   8. Words: wav2vec2 CTC forced alignment of opt.lyrics inside per-line
//      windows. Lines carrying {text,t0,t1} coarse windows (source seconds)
//      align exactly like the reference (per-window inference, letter
//      targets, merge_tokens, low-conf rule t0 = max(t0, t1−(0.07·n+0.05)));
//      plain-text lines use the Whisper coarse pass for windows (same
//      trellis + rule, no blend).
#include "audio_analysis.h"

#include "audio_dsp.h"
#include "audio_whisper_coarse.h"
#include "paths.h"
#include "separate4.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>

#include <onnxruntime_cxx_api.h>
#include "json.hpp"

namespace fs = std::filesystem;

namespace {

constexpr int kSR = 44100;
constexpr int kHop = 441;  // 10 ms onset resolution
constexpr int kOutSR = 44100;

void report(const std::function<void(float, const char*)>& p, float f, const char* s) {
    if (p) p(f, s);
}

// Decode with stems_dir/file lookup: <dir>/{drums,bass,other,vocals}.wav.
bool stems_in_dir(const std::string& dir, std::array<std::string, 4>& paths) {
    if (dir.empty() || !fs::exists(dir)) return false;
    static const char* kNames[4] = {"drums", "bass", "other", "vocals"};
    for (int i = 0; i < 4; i++) {
        fs::path p = fs::path(dir) / (std::string(kNames[i]) + ".wav");
        if (!fs::exists(p)) return false;
        paths[i] = p.string();
    }
    return true;
}

float percentile_vec(std::vector<float> v, float q) {
    return aadsp::percentile_sorted(std::move(v), q);
}

// Timing grid: onset frames at 10 ms hop, frame f ↔ t = f * hop / sr.
inline double onset_frame_time(int f) { return (double)f * kHop / kSR; }

// Peak-pick an onset envelope per the spec table; write hits with p90 strengths.
// norm_q: quantile for norm = env / p97; delta = 0.5 * quantile(norm, delta_q).
void pick_hits(const std::vector<float>& env, float delta_q, float wait_s,
               std::vector<AudioHit>& out, double t_off = 0.0) {
    out.clear();
    int n = (int)env.size();
    if (n == 0) return;
    float p97 = aadsp::percentile(env.data(), n, 0.97f);
    float den = p97 + 1e-9f;
    std::vector<float> norm((size_t)n);
    for (int i = 0; i < n; i++) norm[i] = env[i] / den;
    float delta = aadsp::percentile(norm.data(), n, delta_q) * 0.5f;
    int wait = (int)(wait_s * 100);
    auto peaks = aadsp::peak_pick(norm.data(), n, 3, 3, 12, 12, delta, wait);
    if (peaks.empty()) return;
    std::vector<float> heights;
    heights.reserve(peaks.size());
    for (int p : peaks) heights.push_back(norm[p]);
    float ref = percentile_vec(heights, 0.90f) + 1e-9f;
    for (int p : peaks) {
        float s = norm[p] / ref;
        if (s > 1.f) s = 1.f;
        out.push_back({onset_frame_time(p) + t_off, s});
    }
}

// Vocab for the wav2vec2 CTC model (shared-models copy next to the .onnx).
struct CtcVocab {
    int blank = 0;
    std::vector<std::string> id2tok;  // size V; "" for specials we skip
    bool ok = false;
};
CtcVocab load_ctc_vocab(const std::string& model_path) {
    CtcVocab v;
    fs::path mp(model_path);
    fs::path vp = mp.parent_path() / (mp.stem().string() == "wav2vec2_ctc_float"
                                           ? "wav2vec2_vocab_float.json"
                                           : "wav2vec2_vocab.json");
    std::ifstream f(vp.string());
    if (!f) return v;
    try {
        nlohmann::json j = nlohmann::json::parse(f);
        int vmax = 0;
        for (auto& kv : j.items()) vmax = std::max(vmax, kv.value().get<int>());
        v.id2tok.assign((size_t)vmax + 1, "");
        for (auto& kv : j.items()) {
            const std::string& kk = kv.key();
            int id = kv.value().get<int>();
            if (kk == "<pad>" || kk == "-") v.blank = id;
            else if (kk == "|" || kk == "'") v.id2tok[id] = kk;
            else if (kk.size() == 1 && std::isalpha((unsigned char)kk[0]))
                v.id2tok[id] = std::string(1, (char)std::toupper((unsigned char)kk[0]));
            else v.id2tok[id] = "";  // <s>, </s>, <unk>: unmappable
        }
        v.ok = true;
    } catch (...) {}
    return v;
}

std::string norm_word(const std::string& w) {
    std::string o;
    for (char c : w) {
        if (std::isalpha((unsigned char)c)) o += (char)std::toupper((unsigned char)c);
        else if (c == '\'') o += '\'';
    }
    return o;
}


}  // namespace

bool audio_analysis_run(const std::string& audio_path, const AudioAnalysisOptions& opt,
                        AudioAnalysis& out, const std::function<void(float, const char*)>& progress,
                        std::string* err) {
    out = AudioAnalysis{};
    if (audio_path.empty() || !fs::exists(audio_path)) {
        if (err) *err = "audio file not found: " + audio_path;
        return false;
    }
    char absbuf[4096] = {};
    (void)absbuf;
    out.source = fs::absolute(audio_path).string();
    out.env_start = 0.0;

    // Optional analysis range [range_t0, range_t1) in source seconds: decode,
    // separate, analyse and normalise only that span; all event times stay in
    // source seconds (offset by the span start). Like the reference, which
    // analysed the 80–130 s segment of the song (SEG_START=80).
    double span_t0 = 0.0, span_t1 = -1.0;
    bool use_span = false;
    if (opt.has_range && opt.range_t1 > opt.range_t0 && opt.range_t0 >= 0.0) {
        span_t0 = opt.range_t0;
        span_t1 = opt.range_t1;
        use_span = true;
    }
    size_t span_s0 = 0;  // span start in samples @ kOutSR (mix rate)
    size_t span_n = 0;   // span length in samples @ kOutSR

    // ── 1. Decode the mix ────────────────────────────────────────────────────
    report(progress, 0.02f, "Decoding mix…");
    std::vector<float> mix;
    {
        std::string derr;
        if (use_span) {
            if (!aadsp::decode_mono_span(audio_path, kOutSR, span_t0, span_t1, mix, &derr)) {
                if (err) *err = derr;
                return false;
            }
            span_s0 = (size_t)std::llround(span_t0 * kOutSR);
            span_n = mix.size();
            out.env_start = span_t0;
        } else {
            if (!aadsp::decode_mono(audio_path, kOutSR, mix, &derr)) {
                if (err) *err = derr;
                return false;
            }
        }
    }
    double span_off = use_span ? (double)span_s0 / kOutSR : 0.0;  // source seconds of mix[0]
    out.duration = use_span ? span_off + (double)mix.size() / kOutSR
                            : (double)mix.size() / kOutSR;

    // ── 2. Stems ─────────────────────────────────────────────────────────────
    // Order: drums, bass, other, vocals.
    report(progress, 0.08f, "Separating stems…");
    std::array<std::string, 4> stem_paths;
    std::array<std::vector<float>, 4> stems;
    std::array<bool, 4> have = {false, false, false, false};
    bool have4 = false;
    if (stems_in_dir(opt.stems_dir, stem_paths)) {
        have4 = true;
        for (int i = 0; i < 4; i++) {
            std::string derr;
            if (use_span) {
                if (aadsp::decode_mono_span(stem_paths[i], kOutSR, span_t0, span_t1, stems[i],
                                            &derr))
                    have[i] = true;
            } else if (aadsp::decode_mono(stem_paths[i], kOutSR, stems[i], &derr)) {
                have[i] = true;
            }
        }
        have4 = have[0] && have[1] && have[2] && have[3];
        for (int i = 0; i < 4; i++) out.stems[i] = have[i] ? stem_paths[i] : std::string();
    } else if (opt.separate_stems && separate4_available()) {
        std::string derr;
        fs::path od = fs::path(use_span ? cache_path(audio_path + "\nspan", "_stems4")
                                        : cache_path(audio_path, "_stems4"));
        std::error_code ec;
        fs::create_directories(od, ec);
        std::array<std::string, 4> op;
        bool sep_ok = use_span
            ? separate_stems4_span(audio_path, span_t0, span_t1, od.string(), op, nullptr,
                                   &derr)
            : separate_stems4(audio_path, od.string(), op, nullptr, &derr);
        if (sep_ok) {
            for (int i = 0; i < 4; i++) {
                stem_paths[i] = op[i];
                out.stems[i] = op[i];
                std::string d2;
                if (aadsp::decode_mono(op[i], kOutSR, stems[i], &d2)) have[i] = true;
            }
            have4 = have[0] && have[1] && have[2] && have[3];
        } else if (err) {
            *err = derr;
        }
        if (!have4) {
            if (err && err->empty()) *err = "stem separation failed";
            // Fall through to the no-stems fallback below rather than failing:
            // beats/env/spectrum/words still work on the mix.
            if (err) err->clear();
        }
    }
    // Instrumental fallback = original − vocals (spec §stems). Needed when the
    // drums stem is missing (band flux runs on it) and for the downbeat bass
    // fallback. Requires the vocal stem; else fall back to the mix itself.
    std::vector<float> instrumental;
    bool have_vocals = have[3];
    if (!have4 && have_vocals) {
        instrumental.resize(mix.size());
        size_t nv = stems[3].size();
        for (size_t i = 0; i < mix.size(); i++)
            instrumental[i] = mix[i] - (i < nv ? stems[3][i] : 0.f);
    }

    // ── 3. Beats on the mix ──────────────────────────────────────────────────
    report(progress, 0.30f, "Tracking beats…");
    std::vector<int> beat_frames;
    {
        auto oenv = aadsp::onset_strength_mel(mix.data(), (int)mix.size(), kSR, kHop);
        auto bt = aadsp::beat_track(oenv.data(), (int)oenv.size(), kSR, kHop, 400.f, false);
        out.bpm = bt.bpm;
        // Span-relative bookkeeping: the DSP runs on the span slice, so frame
        // times are span-local; the user-visible duration/beats stay absolute.
        double span_len = (double)mix.size() / kOutSR;
        for (int f : bt.frames) {
            double tl = onset_frame_time(f);
            double t = tl + span_off;
            if (tl >= -0.05 && tl < span_len) out.beats.push_back(t);
            else if (tl >= span_len && tl < span_len + 0.011)
                out.beats.push_back(t);  // keep the closing-frame beat (trim=False)
        }
    }

    // ── 4. Downbeats ─────────────────────────────────────────────────────────
    report(progress, 0.45f, "Finding downbeats…");
    {
        const std::vector<float>* bass_ptr = have[1] ? &stems[1] : nullptr;
        const std::vector<float>* other_ptr = have[2] ? &stems[2] : nullptr;
        std::vector<float> bass_mix, other_mix;
        if (!bass_ptr) {
            // Fallback: instrumental (original − vocals) or the mix.
            bass_mix = have_vocals ? instrumental : mix;
            bass_ptr = &bass_mix;
        }
        if (!other_ptr) {
            other_mix = have_vocals ? instrumental : mix;
            other_ptr = &other_mix;
        }
        auto bo = aadsp::onset_strength_mel(bass_ptr->data(), (int)bass_ptr->size(), kSR, kHop);
        // Chroma of bass+other at hop 441, then median-sync to beat frames.
        std::vector<float> summed(std::max(bass_ptr->size(), other_ptr->size()), 0.f);
        {
            size_t nb = bass_ptr->size(), no = other_ptr->size();
            summed.assign(std::max(nb, no), 0.f);
            for (size_t i = 0; i < nb; i++) summed[i] += (*bass_ptr)[i];
            for (size_t i = 0; i < no; i++) summed[i] += (*other_ptr)[i];
        }
        float tuning = aadsp::estimate_tuning(summed.data(), (int)summed.size(), kSR);
        auto cfb = aadsp::make_chroma(kSR, 2048, tuning);
        auto win = aadsp::hann_periodic(2048);
        std::vector<float> mag;
        int nfr = 0;
        aadsp::stft_mag(summed.data(), (int)summed.size(), 2048, kHop, true, win.data(), mag,
                        nfr);
        std::vector<float> chroma;
        aadsp::chroma_from_power(cfb, mag.data(), nfr, chroma);
        // Beat-frame indices must be INTEGERS on the onset grid: beats sit
        // exactly on frames (0.61 s period = 61 hops), and (b - span_off) in
        // floating point can land one frame low (80.81 → 80 instead of 81),
        // shifting chroma sync + bass windows and flipping the phase.
        std::vector<int> bf;
        for (double b : out.beats) bf.push_back((int)std::llround((b - span_off) * kSR / kHop));
        int phase = 0;
        if (!bf.empty())
            phase = aadsp::downbeat_phase(bo.data(), (int)bo.size(), bf.data(),
                                          (int)bf.size(), chroma.data(), nfr);
        for (size_t i = 0; i < out.beats.size(); i++)
            if ((int)(i % 4) == phase) out.downbeats.push_back(out.beats[i]);
    }

    // ── 5. Hits ──────────────────────────────────────────────────────────────
    report(progress, 0.55f, "Picking drum hits…");
    {
        auto win = aadsp::hann_periodic(2048);
        // Drums source: drums stem, else instrumental, else mix.
        const std::vector<float>* dsrc = have[0] ? &stems[0]
                                         : (have_vocals ? &instrumental : &mix);
        std::vector<float> mag;
        int nfr = 0;
        aadsp::stft_mag(dsrc->data(), (int)dsrc->size(), 2048, kHop, true, win.data(), mag,
                        nfr);
        int bins = 2048 / 2 + 1;
        auto kick_env = aadsp::band_flux(mag.data(), bins, nfr, kSR, 2048, 30, 150);
        auto snare_env = aadsp::band_flux(mag.data(), bins, nfr, kSR, 2048, 180, 4000);
        auto hat_env = aadsp::band_flux(mag.data(), bins, nfr, kSR, 2048, 7000, 16000);
        pick_hits(kick_env, 0.90f, 0.12f, out.hits[(int)HitKind::Kick], span_off);
        pick_hits(snare_env, 0.90f, 0.12f, out.hits[(int)HitKind::Snare], span_off);
        pick_hits(hat_env, 0.80f, 0.07f, out.hits[(int)HitKind::Hat], span_off);
        auto stem_onsets = [&](const std::vector<float>& s) {
            return aadsp::onset_strength_mel(s.data(), (int)s.size(), kSR, kHop);
        };
        std::vector<float> bsrc = have[1] ? stems[1] : (have_vocals ? instrumental : mix);
        std::vector<float> osrc = have[2] ? stems[2] : (have_vocals ? instrumental : mix);
        std::vector<float> vsrc = have_vocals ? stems[3] : mix;
        pick_hits(stem_onsets(bsrc), 0.85f, 0.10f, out.hits[(int)HitKind::Bass], span_off);
        pick_hits(stem_onsets(osrc), 0.85f, 0.08f, out.hits[(int)HitKind::Other], span_off);
        pick_hits(stem_onsets(vsrc), 0.85f, 0.08f, out.hits[(int)HitKind::Vocal], span_off);
    }

    // ── 6. Envelopes at 60 fps ───────────────────────────────────────────────
    // Span-relative: frame f ↔ span_off + f/fps (env/spectrum stay span-local
    // like the reference clip analysis; beats/hits/words carry absolute times).
    report(progress, 0.70f, "Building envelopes…");
    {
        out.fps = 60;
        int hop = kSR / out.fps;
        int n_frames = (int)((double)mix.size() / kOutSR * out.fps);
        const std::vector<float>* srcs[5] = {&mix, &mix, &mix, &mix, &mix};
        if (have4) {
            srcs[1] = &stems[0];
            srcs[2] = &stems[1];
            srcs[3] = &stems[2];
            srcs[4] = &stems[3];
        } else if (have_vocals) {
            srcs[4] = &stems[3];
        }
        for (int k = 0; k < 5; k++) {
            auto rms =
                aadsp::frame_rms(srcs[k]->data(), (int)srcs[k]->size(), 2048, hop, n_frames);
            float p98 = aadsp::percentile(rms.data(), n_frames, 0.98f) + 1e-9f;
            out.env[k].resize((size_t)n_frames);
            for (int i = 0; i < n_frames; i++) {
                float v = rms[i] / p98;
                out.env[k][i] = v > 1.f ? 1.f : v;
            }
        }
    }

    // ── 7. Spectrum ──────────────────────────────────────────────────────────
    report(progress, 0.80f, "Building spectrum…");
    {
        out.spectrum_bands = 32;
        int hop = kSR / out.fps;
        int n_frames = (int)((double)mix.size() / kOutSR * out.fps);
        const int n_fft = 4096;
        auto win = aadsp::hann_periodic(n_fft);
        std::vector<float> mag;
        int nfr = 0;
        aadsp::stft_mag(mix.data(), (int)mix.size(), n_fft, hop, false, win.data(), mag,
                        nfr);
        auto mfb = aadsp::make_mel(kSR, n_fft, 32, 30.f, 16000.f);
        std::vector<float> m;
        aadsp::mel_power(mfb, mag.data(), nfr, m);
        aadsp::power_to_db_inplace(m);
        // librosa.power_to_db(M, ref=np.max): 0 dB at the loudest bin of the span.
        const float peak = m.empty() ? 0.f : *std::max_element(m.begin(), m.end());
        for (float& v : m) v -= peak;
        out.spectrum.assign((size_t)n_frames * 32, 0);
        for (int t = 0; t < n_frames; t++) {
            for (int b = 0; b < 32; b++) {
                float db = (t < nfr) ? m[(size_t)b * nfr + t] : -70.f;
                float v = (db + 70.f) / 70.f;
                if (v < 0) v = 0;
                if (v > 1) v = 1;
                out.spectrum[(size_t)t * 32 + b] = (uint8_t)lroundf(v * 99);
            }
        }
    }

    // ── 8. Words ─────────────────────────────────────────────────────────────
    // Reference-faithful alignment (pipeline/audio.py align_words): per-line
    // windows, wav2vec2 inference per window, trellis over the line's LETTERS
    // (no "|" separators — the reference target has none), merge_tokens
    // semantics, word spans from first/last char, conf = mean span score,
    // low-conf rule t0 = max(t0, t1 − (0.07·n_letters + 0.05)) with t1 kept.
    // Windows come from the line itself ({text,t0,t1}, source seconds) or
    // from the Whisper coarse pass for plain-text lines. out.lines stays
    // plain text.
    for (size_t li = 0; li < opt.lyrics.size(); li++) out.lines.push_back(opt.lyrics[li].text);
    if (!opt.lyrics.empty()) {
        report(progress, 0.86f, "Aligning lyrics…");
        // Vocal audio at 16 kHz, decoded DIRECTLY via libswresample (an
        // earlier 44.1k-then-interp path aliased HF stem content into the
        // vocal band and destroyed first-word emissions).
        std::vector<float> v16;
        std::string align_err;
        bool aligned = false;
        // v16_off: source seconds of v16[0]. Span stem files cover the span
        // (span-relative); whole-file sources are sliced to the span here.
        double v16_off = use_span ? span_off : 0.0;
        {
            const char* vsrc = nullptr;
            if (have_vocals && !out.stems[3].empty()) vsrc = out.stems[3].c_str();
            std::string derr;
            bool ok16;
            if (use_span && vsrc) {
                ok16 = aadsp::decode_mono(vsrc, 16000, v16, &derr);
            } else if (use_span) {
                ok16 = aadsp::decode_mono_span(audio_path, 16000, span_t0, span_t1, v16,
                                               &derr);
            } else {
                ok16 = aadsp::decode_mono(vsrc ? vsrc : audio_path, 16000, v16, &derr);
            }
            if (!ok16) align_err = derr;
        }
        std::string model_path = wav2vec2_ctc_path();
        CtcVocab vocab = load_ctc_vocab(model_path);
        if (!vocab.ok) align_err = "vocab load failed";
        else if (!fs::exists(model_path)) align_err = "model missing: " + model_path;
        else if (v16.empty()) align_err = "empty vocal pcm";
        // Windows in source seconds: given ones verbatim (clamped to the
        // track); the rest from the Whisper coarse pass (even splits only on
        // total failure of the pass).
        std::vector<std::pair<double, double>> wins(opt.lyrics.size(), {0.0, 0.0});
        std::vector<char> win_given(opt.lyrics.size(), 0);
        double span_end = use_span ? span_off + (double)mix.size() / kOutSR
                                     : out.duration;
        for (size_t li = 0; li < opt.lyrics.size(); li++) {
            if (opt.lyrics[li].has_window && opt.lyrics[li].w1 > opt.lyrics[li].w0) {
                double w0 = opt.lyrics[li].w0, w1 = opt.lyrics[li].w1;
                if (w0 < span_off) w0 = span_off;
                if (w1 > span_end) w1 = span_end;
                if (w1 > w0) {
                    wins[li] = {w0, w1};
                    win_given[li] = 1;
                }
            }
        }
        bool need_whisper = false;
        for (char g : win_given)
            if (!g) {
                need_whisper = true;
                break;
            }
        if (align_err.empty() && need_whisper) {
            std::vector<std::string> texts;
            texts.reserve(opt.lyrics.size());
            for (auto& l : opt.lyrics) texts.push_back(l.text);
            auto wprog = [&](float p, const char* s) {
                report(progress, 0.86f + p * 0.02f, s);
            };
            std::vector<std::pair<float, float>> ww;
            std::string werr;
            // word_times_out=nullptr: the reference low-conf rule needs no blend.
            if (!whisper_coarse_windows(v16, texts, ww, nullptr, wprog, &werr) ||
                ww.size() != opt.lyrics.size()) {
                if (werr.empty()) werr = "coarse pass failed";
                fprintf(stderr, "[align] coarse fallback (%s): even splits\n",
                        werr.c_str());
                ww.clear();
                for (size_t i = 0; i < opt.lyrics.size(); i++)
                    ww.push_back({(float)(out.duration * i / opt.lyrics.size()),
                                  (float)(out.duration * (i + 1) / opt.lyrics.size())});
            }
            for (size_t i = 0; i < opt.lyrics.size(); i++)
                if (!win_given[i]) wins[i] = {ww[i].first, ww[i].second};
        }
        // Token targets per line: LETTERS ONLY (the reference has no separators).
        std::vector<std::vector<std::string>> ltoks;
        for (auto& ln : opt.lyrics) {
            std::vector<std::string> ws;
            std::string cur;
            for (char c : ln.text) {
                if (c == ' ' || c == '\t') {
                    if (!cur.empty()) {
                        ws.push_back(cur);
                        cur.clear();
                    }
                } else cur += c;
            }
            if (!cur.empty()) ws.push_back(cur);
            ltoks.push_back(ws);
        }
        if (align_err.empty()) {
            try {
                Ort::Env oenv(ORT_LOGGING_LEVEL_WARNING, "audio_analysis_align");
                Ort::SessionOptions sopts;
                sopts.SetIntraOpNumThreads(2);
                Ort::Session sess(oenv, model_path.c_str(), sopts);
                Ort::MemoryInfo mem =
                    Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
                bool need_mask = sess.GetInputCount() >= 2;
                const char* in_names[] = {"input_values", "attention_mask"};
                const char* out_names[] = {"logits"};
                // Map vocab token → id for target building.
                std::vector<std::vector<int>> line_targets;
                for (auto& ws : ltoks) {
                    std::vector<int> tgt;
                    for (auto& w : ws) {
                        for (char c : norm_word(w)) {
                            std::string s(1, c);
                            for (size_t id = 0; id < vocab.id2tok.size(); id++)
                                if (vocab.id2tok[id] == s) {
                                    tgt.push_back((int)id);
                                    break;
                                }
                        }
                    }
                    line_targets.push_back(tgt);
                }
                size_t nl = opt.lyrics.size();
                for (size_t li = 0; li < nl; li++) {
                    report(progress, 0.88f + 0.07f * (float)li / (float)nl,
                           "Aligning lyrics…");
                    double w0 = wins[li].first, w1 = wins[li].second;
                    if (w1 <= w0) continue;
                    const auto& tgt = line_targets[li];
                    if (tgt.empty()) continue;
                    int L = (int)tgt.size();
                    int s0 = std::max(0, (int)((w0 - v16_off) * 16000.0));
                    int s1 = std::min((int)v16.size(), (int)((w1 - v16_off) * 16000.0));
                    if (s1 <= s0) continue;
                    // Per-window zero-mean unit-variance chunk (the reference
                    // runs inference over the window slice only).
                    std::vector<float> chunk(v16.begin() + s0, v16.begin() + s1);
                    {
                        double mean = 0;
                        for (float x : chunk) mean += x;
                        mean /= chunk.size();
                        double var = 0;
                        for (float x : chunk) {
                            double d = x - mean;
                            var += d * d;
                        }
                        float istd = (float)(1.0 / (sqrt(var / chunk.size()) + 1e-7));
                        for (float& x : chunk) x = (float)((x - mean) * istd);
                    }
                    std::vector<int64_t> shp = {1, (int64_t)chunk.size()};
                    std::vector<Ort::Value> ins;
                    ins.push_back(Ort::Value::CreateTensor<float>(
                        mem, chunk.data(), chunk.size(), shp.data(), 2));
                    std::vector<float> mask;
                    if (need_mask) {
                        mask.assign(chunk.size(), 1.f);
                        ins.push_back(Ort::Value::CreateTensor<float>(
                            mem, mask.data(), mask.size(), shp.data(), 2));
                    }
                    std::vector<Ort::Value> wouts =
                        sess.Run(Ort::RunOptions{nullptr}, in_names,
                                 ins.data(), need_mask ? 2u : 1u, out_names, 1);
                    auto sh = wouts[0].GetTensorTypeAndShapeInfo().GetShape();
                    int Tw = (int)sh[1], V = (int)sh[2];
                    if (Tw < L || V <= 0) continue;
                    const float* wlog = wouts[0].GetTensorData<float>();
                    // Reference: sec_per_frame = window_sec / emission frames.
                    double spf = (w1 - w0) / Tw;
                    // Log-softmax rows.
                    std::vector<float> lp((size_t)Tw * V);
                    for (int t = 0; t < Tw; t++) {
                        const float* row = wlog + (size_t)t * V;
                        float mx = row[0];
                        for (int i = 1; i < V; i++) mx = std::max(mx, row[i]);
                        double s = 0;
                        for (int i = 0; i < V; i++) {
                            lp[(size_t)t * V + i] = row[i] - mx;
                            s += exp(lp[(size_t)t * V + i]);
                        }
                        float ls = (float)log(s);
                        for (int i = 0; i < V; i++) lp[(size_t)t * V + i] -= ls;
                    }
                    // Trellis (T+1)x(L+1).
                    const float NEGV = -1e30f;
                    std::vector<float> tr((size_t)(Tw + 1) * (L + 1), NEGV);
                    auto cell = [&](int t, int j) -> float& {
                        return tr[(size_t)t * (L + 1) + j];
                    };
                    cell(0, 0) = 0.f;
                    {
                        float cum = 0.f;
                        for (int t = 0; t < Tw; t++) {
                            cum += lp[(size_t)t * V + vocab.blank];
                            cell(t + 1, 0) = cum;
                        }
                    }
                    for (int t = 0; t < Tw; t++) {
                        float bs = lp[(size_t)t * V + vocab.blank];
                        for (int j = 1; j <= L; j++) {
                            float stay = cell(t, j) + bs;
                            float adv = cell(t, j - 1) + lp[(size_t)t * V + tgt[j - 1]];
                            cell(t + 1, j) = std::max(stay, adv);
                        }
                    }
                    // Backtrack from the argmax end (best cell(t, L)).
                    // Path entries record EMIT vs blank-stay: merge_tokens
                    // drops blanks, so only adv frames form spans and conf.
                    int t_start = 0;
                    float best = cell(0, L);
                    for (int t = 1; t <= Tw; t++)
                        if (cell(t, L) > best) {
                            best = cell(t, L);
                            t_start = t;
                        }
                    struct PP {
                        int tok, fr;
                        float p;
                        bool adv;
                    };
                    std::vector<PP> path;
                    int j = L;
                    for (int t = t_start; t > 0; --t) {
                        float stayed = cell(t - 1, j) + lp[(size_t)(t - 1) * V + vocab.blank];
                        float changed =
                            cell(t - 1, j - 1) + lp[(size_t)(t - 1) * V + tgt[j - 1]];
                        bool adv = changed > stayed;
                        float pr = exp(lp[(size_t)(t - 1) * V + (adv ? tgt[j - 1] : vocab.blank)]);
                        path.push_back({j - 1, t - 1, pr, adv});
                        if (adv && --j == 0) break;
                    }
                    if (j != 0) continue;
                    std::reverse(path.begin(), path.end());
                    // Merge → per-token spans from EMITTED frames only.
                    std::vector<int> cs((size_t)L, 0), ce((size_t)L, 0);
                    std::vector<float> csc((size_t)L, 0.f);
                    {
                        std::vector<int> cnt((size_t)L, 0);
                        std::vector<int> fmin((size_t)L, Tw), fmax((size_t)L, -1);
                        std::vector<double> psum((size_t)L, 0.0);
                        for (auto& p : path) {
                            if (!p.adv) continue;
                            int tk = p.tok;
                            if (tk < 0 || tk >= L) continue;
                            cnt[tk]++;
                            fmin[tk] = std::min(fmin[tk], p.fr);
                            fmax[tk] = std::max(fmax[tk], p.fr);
                            psum[tk] += p.p;
                        }
                        for (int c = 0; c < L; c++) {
                            cs[c] = fmin[c];
                            ce[c] = fmax[c] + 1;
                            csc[c] = cnt[c] ? (float)(psum[c] / cnt[c]) : 0.f;
                        }
                    }
                    // Words → times. Char index walks the letter target
                    // (no separators). Reference low-conf rule: end stays,
                    // start pulls in. No whisper blend.
                    int ci = 0;
                    for (size_t wi = 0; wi < ltoks[li].size(); wi++) {
                        std::string nw = norm_word(ltoks[li][wi]);
                        int c0 = ci, c1 = ci + (int)nw.size();
                        ci = c1;
                        if (c0 >= c1 || c1 > L) continue;
                        int fmin = cs[c0], fmax = ce[c0];
                        double ps = csc[c0];
                        int cn = 1;
                        for (int c = c0 + 1; c < c1; c++) {
                            fmin = std::min(fmin, cs[c]);
                            fmax = std::max(fmax, ce[c]);
                            ps += csc[c];
                            cn++;
                        }
                        float conf = (float)(ps / cn);
                        double t0 = w0 + fmin * spf;
                        double t1 = w0 + fmax * spf;
                        if (conf < 0.5f)
                            t0 = std::max(t0, t1 - (0.07 * (double)nw.size() + 0.05));
                        AnalysisWord w;
                        w.w = ltoks[li][wi];
                        w.line = (int)li;
                        w.i = (int)wi;
                        w.t0 = t0;
                        w.t1 = t1;
                        w.conf = conf;
                        out.words.push_back(w);
                    }
                }
                aligned = !out.words.empty();
                if (!aligned) align_err = "trellis produced no words";
            } catch (const std::exception& e) {
                align_err = e.what();
                aligned = false;
            } catch (...) {
                align_err = "unknown align exception";
                aligned = false;
            }
        }
        if (!aligned) {
            // Even-split fallback keeps the schema valid when the model is
            // missing or alignment fails outright.
            out.words.clear();
            for (size_t li = 0; li < opt.lyrics.size(); li++) {
                std::string cur;
                std::vector<std::string> ws;
                for (char c : opt.lyrics[li].text) {
                    if (c == ' ' || c == '\t') {
                        if (!cur.empty()) {
                            ws.push_back(cur);
                            cur.clear();
                        }
                    } else cur += c;
                }
                if (!cur.empty()) ws.push_back(cur);
                double dur = out.duration;
                for (size_t wi = 0; wi < ws.size(); wi++) {
                    AnalysisWord w;
                    w.w = ws[wi];
                    w.line = (int)li;
                    w.i = (int)wi;
                    w.t0 = dur * (li + (double)wi / ws.size()) / opt.lyrics.size();
                    w.t1 = dur * (li + (double)(wi + 1) / ws.size()) / opt.lyrics.size();
                    w.conf = 0.f;
                    out.words.push_back(w);
                }
            }
        }
    }
    report(progress, 1.f, "Done");
    return true;
}

std::string audio_analysis_cache_file(const std::string& audio_path, const AudioAnalysisOptions& opt) {
    // Bump when the analysis output changes (r2: env_start for spans; r3: spectrum dB rel max).
    static const char* kSchemaRevision = "r3";
    std::error_code ec;
    long long mtime = 0;
    auto wt = fs::last_write_time(audio_path, ec);
    if (!ec) mtime = (long long)wt.time_since_epoch().count();
    ec.clear();
    uint64_t size = (uint64_t)fs::file_size(audio_path, ec);
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const std::string& s) {
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
        h ^= 0xff; h *= 1099511628211ull;  // field separator
    };
    for (const LyricLine& l : opt.lyrics) {
        mix(l.text);
        if (l.has_window) { mix(std::to_string(l.w0)); mix(std::to_string(l.w1)); }
    }
    if (opt.has_range) { mix("range"); mix(std::to_string(opt.range_t0)); mix(std::to_string(opt.range_t1)); }
    mix(opt.stems_dir);
    char key[160];
    snprintf(key, sizeof(key), "%s_%llu_%lld_%016llx_%d", kSchemaRevision, (unsigned long long)size, mtime,
             (unsigned long long)h, opt.separate_stems ? 1 : 0);
    return cache_path(fs::absolute(audio_path).string() + '\n' + key, "_analysis.json");
}
