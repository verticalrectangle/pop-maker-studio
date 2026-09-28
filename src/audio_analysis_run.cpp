// audio_analysis_run.cpp — analysis v2 (docs/AUDIO_ANALYSIS.md).
//
// Blocking full analysis; run on a worker thread. progress(0..1, stage).
//
// Pipeline (all times are source-file seconds):
//   1. Decode the mix to 44.1 kHz mono; also decode each stem.
//   2. Stems: with_separation ? separate_stems4() : injected stems_dir (a real
//      feature — reuse of precomputed stems) : no-stems fallback. Stems are
//      cached as <stem>_analysis.json next to the audio when writable.
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
//      windows from a Whisper coarse pass (order-constrained fuzzy match of
//      each line onto the decoded word stream; even splits only for lines
//      that match nothing), with the low-confidence clamp
//      t0 = max(t0, t1 − (0.07·chars + 0.05)).
#include "audio_analysis.h"

#include "audio_dsp.h"
#include "audio_whisper_coarse.h"
#include "paths.h"
#include "separate4.h"

#include <algorithm>
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

// Cache path: <audio stem>_analysis.json next to the audio, or the project
// cache dir when that is not writable.
std::string cache_json_path(const std::string& audio_path) {
    fs::path a(audio_path);
    fs::path beside = a.parent_path() / (a.stem().string() + "_analysis.json");
    std::error_code ec;
    // Writable if the parent exists and we can create the file.
    std::ofstream t(beside, std::ios::app);
    if (t) return beside.string();
    return cache_path(audio_path, "_analysis.json");
}

float percentile_vec(std::vector<float> v, float q) {
    return aadsp::percentile_sorted(std::move(v), q);
}

// Timing grid: onset frames at 10 ms hop, frame f ↔ t = f * hop / sr.
inline float onset_frame_time(int f) { return (float)f * kHop / kSR; }

// Peak-pick an onset envelope per the spec table; write hits with p90 strengths.
// norm_q: quantile for norm = env / p97; delta = 0.5 * quantile(norm, delta_q).
void pick_hits(const std::vector<float>& env, float delta_q, float wait_s,
               std::vector<AudioHit>& out) {
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
        out.push_back({onset_frame_time(p), s});
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
    fs::path vp = fs::path(model_path).parent_path() / "wav2vec2_vocab.json";
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
            if (kk == "<pad>") v.blank = id;
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

    // ── 1. Decode the mix ────────────────────────────────────────────────────
    report(progress, 0.02f, "Decoding mix…");
    std::vector<float> mix;
    {
        std::string derr;
        if (!aadsp::decode_mono(audio_path, kOutSR, mix, &derr)) {
            if (err) *err = derr;
            return false;
        }
    }
    out.duration = (double)mix.size() / kOutSR;

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
            if (aadsp::decode_mono(stem_paths[i], kOutSR, stems[i], &derr)) have[i] = true;
        }
        have4 = have[0] && have[1] && have[2] && have[3];
        for (int i = 0; i < 4; i++) out.stems[i] = have[i] ? stem_paths[i] : std::string();
    } else if (opt.separate_stems && separate4_available()) {
        std::string derr;
        fs::path od = fs::path(cache_path(audio_path, "_stems4"));
        std::error_code ec;
        fs::create_directories(od, ec);
        std::array<std::string, 4> op;
        if (separate_stems4(audio_path, od.string(), op, nullptr, &derr)) {
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
        for (int f : bt.frames) {
            float t = onset_frame_time(f);
            if (t >= -0.05f && t < (float)out.duration) out.beats.push_back(t);
            else if (t >= (float)out.duration && t < (float)out.duration + 0.011f)
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
        std::vector<int> bf;
        for (float b : out.beats) bf.push_back((int)(b * kSR / kHop));
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
        pick_hits(kick_env, 0.90f, 0.12f, out.hits[(int)HitKind::Kick]);
        pick_hits(snare_env, 0.90f, 0.12f, out.hits[(int)HitKind::Snare]);
        pick_hits(hat_env, 0.80f, 0.07f, out.hits[(int)HitKind::Hat]);
        auto stem_onsets = [&](const std::vector<float>& s) {
            return aadsp::onset_strength_mel(s.data(), (int)s.size(), kSR, kHop);
        };
        std::vector<float> bsrc = have[1] ? stems[1] : (have_vocals ? instrumental : mix);
        std::vector<float> osrc = have[2] ? stems[2] : (have_vocals ? instrumental : mix);
        std::vector<float> vsrc = have_vocals ? stems[3] : mix;
        pick_hits(stem_onsets(bsrc), 0.85f, 0.10f, out.hits[(int)HitKind::Bass]);
        pick_hits(stem_onsets(osrc), 0.85f, 0.08f, out.hits[(int)HitKind::Other]);
        pick_hits(stem_onsets(vsrc), 0.85f, 0.08f, out.hits[(int)HitKind::Vocal]);
    }

    // ── 6. Envelopes at 60 fps ───────────────────────────────────────────────
    report(progress, 0.70f, "Building envelopes…");
    {
        out.fps = 60;
        int hop = kSR / out.fps;
        int n_frames = (int)(out.duration * out.fps);
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
        int n_frames = (int)(out.duration * out.fps);
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
    out.lines = opt.lyrics;
    if (!opt.lyrics.empty()) {
        report(progress, 0.86f, "Aligning lyrics…");
        // Vocal audio for the aligner: vocals stem when present, else the mix.
        const std::vector<float>* vptr = have_vocals ? &stems[3] : &mix;
        // Resample to 16 kHz mono (libswresample path via decode helper would
        // re-decode; do it directly from the decoded mix-rate buffer).
        std::vector<float> v16;
        {
            // Rational resample 44100 → 16000 via linear interpolation
            // (the aligner normalises per-window; interpolation error is
            // negligible for CTC emissions).
            size_t n16 = (size_t)((double)vptr->size() * 16000 / kOutSR);
            v16.resize(n16);
            for (size_t i = 0; i < n16; i++) {
                double p = (double)i * kOutSR / 16000;
                size_t i0 = (size_t)p;
                double f = p - i0;
                float a = (i0 < vptr->size()) ? (*vptr)[i0] : 0.f;
                float b = (i0 + 1 < vptr->size()) ? (*vptr)[i0 + 1] : 0.f;
                v16[i] = (float)(a + (b - a) * f);
            }
        }
        std::string model_path = wav2vec2_ctc_path();
        CtcVocab vocab = load_ctc_vocab(model_path);
        bool aligned = false;
        std::string align_err;
        if (!vocab.ok) align_err = "vocab load failed";
        else if (!fs::exists(model_path)) align_err = "model missing: " + model_path;
        else if (v16.empty()) align_err = "empty vocal pcm";
        // Full-track CTC inference first (its emission is sliced per line
        // below). `outs` owns the tensor memory; it must outlive the trellis.
        std::vector<Ort::Value> outs;
        int T = 0, V = 0;
        double sec_per_frame = 0.0;
        const float* logits = nullptr;
        if (align_err.empty()) {
            try {
                Ort::Env oenv(ORT_LOGGING_LEVEL_WARNING, "audio_analysis_align");
                Ort::SessionOptions sopts;
                sopts.SetIntraOpNumThreads(2);
                Ort::Session sess(oenv, model_path.c_str(), sopts);
                Ort::MemoryInfo mem =
                    Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
                // Full-track inference (35 s clip ≈ 4 s on CPU).
                std::vector<float> chunk = v16;
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
                bool need_mask = sess.GetInputCount() >= 2;
                std::vector<float> mask;
                const char* in_names[] = {"input_values", "attention_mask"};
                if (need_mask) {
                    mask.assign(chunk.size(), 1.f);
                    ins.push_back(Ort::Value::CreateTensor<float>(
                        mem, mask.data(), mask.size(), shp.data(), 2));
                }
                const char* out_names[] = {"logits"};
                outs = sess.Run(Ort::RunOptions{nullptr}, in_names,
                                ins.data(), need_mask ? 2u : 1u, out_names, 1);
                auto sh = outs[0].GetTensorTypeAndShapeInfo().GetShape();
                T = (int)sh[1];
                V = (int)sh[2];
                logits = outs[0].GetTensorData<float>();
                sec_per_frame = out.duration / T;
            } catch (const std::exception& e) {
                align_err = e.what();
            } catch (...) {
                align_err = "unknown align exception";
            }
        }
        // Coarse per-line windows from the Whisper pass (order-constrained
        // fuzzy match of each line onto the decoded word stream). The old
        // greedy-CTC windows drifted whole lines by seconds on sung material.
        std::vector<std::pair<float, float>> wins;
        if (align_err.empty()) {
            auto wprog = [&](float p, const char* s) {
                report(progress, 0.86f + p * 0.04f, s);
            };
            std::string werr;
            if (!whisper_coarse_windows(v16, opt.lyrics, wins, wprog, &werr) ||
                wins.size() != opt.lyrics.size()) {
                if (werr.empty()) werr = "coarse pass failed";
                fprintf(stderr, "[align] coarse fallback (%s): even splits\n",
                        werr.c_str());
                wins.clear();
                for (size_t i = 0; i < opt.lyrics.size(); i++)
                    wins.push_back({(float)(out.duration * i / opt.lyrics.size()),
                                    (float)(out.duration * (i + 1) / opt.lyrics.size())});
            }
        }
        // Token targets per line.
        std::vector<std::vector<std::string>> ltoks;
        for (auto& ln : opt.lyrics) {
            std::vector<std::string> ws;
            std::string cur;
            for (char c : ln) {
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
                // Per-line forced alignment (torchaudio stay-advance trellis
                // over the sliced emission; windows are Whisper-derived).
                // Map vocab token → id for target building.
                std::vector<std::vector<int>> line_targets;
                for (auto& ws : ltoks) {
                    std::vector<int> tgt;
                    for (size_t wi = 0; wi < ws.size(); wi++) {
                        if (wi) {
                            // word separator id
                            for (size_t id = 0; id < vocab.id2tok.size(); id++)
                                if (vocab.id2tok[id] == "|") {
                                    tgt.push_back((int)id);
                                    break;
                                }
                        }
                        for (char c : norm_word(ws[wi])) {
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
                for (size_t li = 0; li < opt.lyrics.size(); li++) {
                    float w0 = wins[li].first, w1 = wins[li].second;
                    int f0 = std::max(0, (int)(w0 / sec_per_frame));
                    int f1 = std::min(T, (int)(w1 / sec_per_frame));
                    if (f1 <= f0) continue;
                    const auto& tgt = line_targets[li];
                    if (tgt.empty()) continue;
                    int Tw = f1 - f0, L = (int)tgt.size();
                    if (Tw < L) continue;
                    // Log-softmax rows.
                    std::vector<float> lp((size_t)Tw * V);
                    for (int t = 0; t < Tw; t++) {
                        const float* row = logits + (size_t)(f0 + t) * V;
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
                    // Backtrack.
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
                    };
                    std::vector<PP> path;
                    int j = L;
                    for (int t = t_start; t > 0; --t) {
                        float stayed = cell(t - 1, j) + lp[(size_t)(t - 1) * V + vocab.blank];
                        float changed =
                            cell(t - 1, j - 1) + lp[(size_t)(t - 1) * V + tgt[j - 1]];
                        bool adv = changed > stayed;
                        float pr = exp(lp[(size_t)(t - 1) * V + (adv ? tgt[j - 1] : vocab.blank)]);
                        path.push_back({j - 1, t - 1, pr});
                        if (adv && --j == 0) break;
                    }
                    if (j != 0) continue;
                    std::reverse(path.begin(), path.end());
                    // Merge repeats → char spans (frame indices, window-relative).
                    std::vector<int> cs((size_t)L, 0), ce((size_t)L, 0);
                    std::vector<float> csc((size_t)L, 0.f);
                    {
                        std::vector<int> cnt((size_t)L, 0);
                        std::vector<int> fmin((size_t)L, Tw), fmax((size_t)L, -1);
                        std::vector<double> psum((size_t)L, 0.0);
                        for (auto& p : path) {
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
                    // Words → times. Char index k walks the target.
                    int ci = 0;
                    for (size_t wi = 0; wi < ltoks[li].size(); wi++) {
                        std::string nw = norm_word(ltoks[li][wi]);
                        if (wi) ci++;  // separator
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
                        float t0 = w0 + (fmin)* (float)sec_per_frame;
                        float t1 = w0 + (fmax)* (float)sec_per_frame;
                        if (conf < 0.5f)
                            t0 = std::max(t0, t1 - (0.07f * (float)nw.size() + 0.05f));
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
                for (char c : opt.lyrics[li]) {
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
                    w.t0 = (float)(dur * (li + (double)wi / ws.size()) / opt.lyrics.size());
                    w.t1 = (float)(dur * (li + (double)(wi + 1) / ws.size()) / opt.lyrics.size());
                    w.conf = 0.f;
                    out.words.push_back(w);
                }
            }
        }
    }

    // ── Cache + publish ──────────────────────────────────────────────────────
    report(progress, 0.95f, "Writing cache…");
    {
        std::string err2;
        audio_analysis_save_json(out, cache_json_path(audio_path), &err2);
    }
    report(progress, 1.f, "Done");
    return true;
}
