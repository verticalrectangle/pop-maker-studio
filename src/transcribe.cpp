#include "transcribe.h"
#include "separate.h"
#include "paths.h"
#include "forced_align.h"
#include "video.h"
#include <whisper.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <vector>
#include <string>
#include <unordered_set>
#include <cctype>
#include "json.hpp"
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>


namespace fs = std::filesystem;

static std::thread       g_thread;
static std::atomic<bool> g_running{false};
static std::atomic<bool> g_cancel{false};

static std::mutex      g_search_mu;
static SearchStatus    g_search_status;
static std::thread     g_search_thread;

// ── Whisper ggml model path ───────────────────────────────────────────────────

fs::path whisper_model_path() {
    return fs::path(app_models_dir()) / "ggml-large-v3-turbo-q5_0.bin";
}

// find_and_add_clip search uses the SAME model as the full pipeline.
// Tiny.en used to be the search model for speed, but it hallucinated [Music]/♪♪
// on quiet (low-amplitude) vocals and forced a tiny-vs-large quality fork
// between the search cache and the canonical cache.  MDX-Net per-chunk
// dominates latency anyway, so the large-v3-turbo cost delta is small.
fs::path whisper_search_model_path() {
    return whisper_model_path();
}

bool whisper_model_exists() { return fs::exists(whisper_model_path()); }

// ── Audio helpers ─────────────────────────────────────────────────────────────

// Decode audio to 16 kHz mono float32 via ffmpeg pipe — same pattern as separate.cpp.
// clip_in / clip_dur: when clip_dur > 0, only decode [clip_in, clip_in+clip_dur] seconds.
static std::vector<float> decode_16k(const std::string& path,
                                      float clip_in = 0.f, float clip_dur = 0.f) {
    std::string file_arg = "file:" + path;
    std::string ss_val   = std::to_string(clip_in);
    std::string t_val    = std::to_string(clip_dur);

    std::vector<const char*> argv = {"ffmpeg", "-hide_banner", "-loglevel", "error"};
    if (clip_in  > 0.f) { argv.push_back("-ss"); argv.push_back(ss_val.c_str()); }
    argv.push_back("-i"); argv.push_back(file_arg.c_str());
    if (clip_dur > 0.f) { argv.push_back("-t");  argv.push_back(t_val.c_str()); }
    argv.insert(argv.end(), {"-vn", "-ar", "16000", "-ac", "1", "-f", "f32le", "pipe:1", nullptr});

    int pipefd[2];
    if (pipe(pipefd) != 0) return {};
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
    if (!fp) { close(pipefd[0]); waitpid(pid, nullptr, 0); return {}; }

    std::vector<float> pcm;
    float buf[4096];
    size_t r;
    while ((r = fread(buf, sizeof(float), 4096, fp)) > 0)
        pcm.insert(pcm.end(), buf, buf + r);
    fclose(fp);
    waitpid(pid, nullptr, 0);
    return pcm;
}

// ── Adaptive VAD gate ─────────────────────────────────────────────────────────
//
// Zero out non-speech samples in-place so Whisper's `no_speech_thold` actually
// trips on dead regions (MDX residuals, room tone, between-sentence gaps).
// The threshold is derived from the signal itself — 30th-percentile frame RMS
// estimates the noise floor, threshold sits ~+12 dB above it.  Works uniformly
// on separated vocals stems, clean podcast tracks, and noisy captures.  Music-
// heavy mixes have a high, uniform RMS distribution: the uniform-signal escape
// hatch detects that and skips gating, so the worst case is a no-op.
static void vad_gate_inplace(std::vector<float>& pcm) {
    constexpr int kWin = 480;   // 30 ms @ 16 kHz
    constexpr int kHop = 160;   // 10 ms
    constexpr int kMinSpeechFrames = 25;   // 250 ms
    constexpr int kPadFrames       = 20;   // 200 ms padding around speech

    if ((int)pcm.size() < kWin + kHop) return;
    int n_frames = ((int)pcm.size() - kWin) / kHop;
    if (n_frames < 50) return;  // <500 ms — too short to estimate a noise floor

    std::vector<float> rms(n_frames);
    float max_rms = 0.f;
    for (int i = 0; i < n_frames; ++i) {
        double sumsq = 0;
        const float* p = pcm.data() + i * kHop;
        for (int j = 0; j < kWin; ++j) sumsq += (double)p[j] * p[j];
        rms[i] = (float)std::sqrt(sumsq / kWin);
        if (rms[i] > max_rms) max_rms = rms[i];
    }
    if (max_rms < 1e-5f) return;  // entire buffer is silent; nothing to do

    // Noise floor = 30th percentile of frame RMS.
    std::vector<float> sorted_rms = rms;
    size_t k = sorted_rms.size() * 3 / 10;
    std::nth_element(sorted_rms.begin(), sorted_rms.begin() + k, sorted_rms.end());
    float noise_floor = sorted_rms[k];

    // Escape hatch: if the noise floor is within ~12 dB of the peak, the signal
    // is too uniform to gate safely (e.g., raw music with vocals on top).  Skip.
    if (noise_floor / max_rms > 0.25f) return;

    // Gate at noise_floor × 4 (~+12 dB above noise), absolute floor for very
    // clean recordings where the percentile is essentially zero.
    float thresh = std::max(noise_floor * 4.0f, 1e-4f);

    std::vector<uint8_t> active(n_frames, 0);
    for (int i = 0; i < n_frames; ++i) active[i] = rms[i] > thresh ? 1 : 0;

    // Drop islands shorter than kMinSpeechFrames, pad survivors by kPadFrames.
    std::vector<uint8_t> keep(n_frames, 0);
    int i = 0;
    while (i < n_frames) {
        if (!active[i]) { ++i; continue; }
        int j = i;
        while (j < n_frames && active[j]) ++j;
        if (j - i >= kMinSpeechFrames) {
            int a = std::max(0, i - kPadFrames);
            int b = std::min(n_frames, j + kPadFrames);
            for (int m = a; m < b; ++m) keep[m] = 1;
        }
        i = j;
    }

    // Zero non-speech samples in place.
    for (int f = 0; f < n_frames; ++f) {
        if (keep[f]) continue;
        int s = f * kHop;
        int e = std::min((int)pcm.size(), s + kHop);
        for (int m = s; m < e; ++m) pcm[m] = 0.0f;
    }
}

// ── Stem separation ───────────────────────────────────────────────────────────

// Calls the C++ MDX-Net (Kim_Vocal_2) vocal separation. Returns false and sets status on error.
// No ffmpeg fallback — if the model is not ready, the user must download the model.
static bool separate_channels(
    const std::string& in,
    const std::string& voc,
    const std::string& inst,
    PipelineStatus&    status,
    float clip_in  = 0.f,
    float clip_dur = 0.f)
{
    std::string err = separate_run(in, voc, inst, [&](float p, const std::string& msg) {
        status.progress = 0.05f + p * 0.15f;
        status.message  = msg;
    }, clip_in, clip_dur);

    if (!err.empty()) {
        status.stage = PipelineStage::Error;
        status.error = "Stem separation failed: " + err +
                       "\nPlace the models/ folder next to the binary.";
        return false;
    }
    return true;
}

// ── Word-timestamp extraction ─────────────────────────────────────────────────

// Group BPE tokens into words.  Whisper tokens starting with a space signal
// a new word boundary.  We use t_dtw (DTW-aligned) timestamps when available,
// falling back to the regular t0/t1 in centiseconds.
static void extract_words_segments(
    whisper_context* ctx,
    nlohmann::json& words_out,
    nlohmann::json& segs_out)
{
    whisper_token tok_eot = whisper_token_eot(ctx);
    whisper_token tok_beg = whisper_token_beg(ctx);

    int n_segs = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_segs; ++i) {
        // Segment-level entry
        const char* seg_raw = whisper_full_get_segment_text(ctx, i);
        std::string seg_text(seg_raw ? seg_raw : "");
        if (!seg_text.empty() && seg_text[0] == ' ') seg_text = seg_text.substr(1);
        if (!seg_text.empty()) {
            double t0 = whisper_full_get_segment_t0(ctx, i) / 100.0;
            double t1 = whisper_full_get_segment_t1(ctx, i) / 100.0;
            segs_out.push_back({{"text", seg_text}, {"start", t0}, {"end", t1}});
        }

        // Token → word grouping
        int n_tok = whisper_full_n_tokens(ctx, i);
        std::string cur_word;
        double w_t0 = 0.0, w_t1 = 0.0;

        auto emit = [&]() {
            std::string w = cur_word;
            size_t a = w.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) return;
            w = w.substr(a);
            if (!w.empty())
                words_out.push_back({{"word", w}, {"start", w_t0}, {"end", w_t1}});
        };

        for (int j = 0; j < n_tok; ++j) {
            whisper_token id = whisper_full_get_token_id(ctx, i, j);
            if (id >= tok_eot || id == tok_beg) continue;

            const char* raw = whisper_full_get_token_text(ctx, i, j);
            if (!raw) continue;
            std::string t(raw);

            whisper_token_data td = whisper_full_get_token_data(ctx, i, j);
            double t0_s = (td.t_dtw >= 0 ? td.t_dtw : td.t0) / 100.0;
            double t1_s = td.t1 / 100.0;

            bool new_word = !t.empty() && t[0] == ' ';

            if (new_word && !cur_word.empty()) {
                emit();
                cur_word.clear();
            }

            std::string stripped = t;
            if (!stripped.empty() && stripped[0] == ' ')
                stripped = stripped.substr(1);

            if (cur_word.empty()) {
                cur_word = stripped;
                w_t0 = t0_s;
                w_t1 = t1_s;
            } else {
                cur_word += stripped;
                w_t1 = t1_s;
            }
        }
        if (!cur_word.empty()) emit();
    }
}

// ── Transcript provenance sidecar ─────────────────────────────────────────────
// "<...>_words.json" → "<...>_words.src" (a one-word file: vocals | raw).
static std::string words_src_sidecar(const std::string& words_json) {
    if (words_json.size() > 5 &&
        words_json.compare(words_json.size() - 5, 5, ".json") == 0)
        return words_json.substr(0, words_json.size() - 5) + ".src";
    return words_json + ".src";
}

void transcript_write_source(const std::string& words_json, bool from_vocals) {
    std::ofstream f(words_src_sidecar(words_json));
    if (f) f << (from_vocals ? "vocals" : "raw");
}

std::string transcript_source(const std::string& words_json) {
    std::ifstream f(words_src_sidecar(words_json));
    if (!f) return "";   // legacy cache with no sidecar → unknown
    std::string s; std::getline(f, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

// Hallucination filters (defined below, near the search path that first used
// them). is_music_token: [Music]/♪♪/[Applause] surface tokens — never real
// content. is_hallucination_phrase: whisper's stock non-speech inventions
// ("thank you", "thanks for watching", …) — never real song lyrics.
static bool is_music_token(const std::string& w);
static bool is_hallucination_phrase(const std::string& seg_text);

// ── Main transcription worker ─────────────────────────────────────────────────

static void do_transcribe(
    const std::string& audio_path,
    PipelineStatus&    status,
    std::string&       out_words_json,
    std::string&       out_vocals_wav,
    PipelineMode       mode,
    double             proxy_fps,
    float              clip_in,
    float              clip_dur)
{
    // Derived artifacts live in the shared cache dir (~/.cache/pop-maker-studio),
    // not next to the user's media — keyed by the source path so readers agree.
    fs::path audio(audio_path);
    std::string stem       = audio.stem().string();
    out_words_json         = cache_path(audio_path, "_words.json");
    out_vocals_wav         = cache_path(audio_path, "_vocals.wav");
    std::string inst_wav   = cache_path(audio_path, "_instrumental.wav");
    std::string segs_json  = cache_path(audio_path, "_segments.json");

    // ── Separation ────────────────────────────────────────────────────────────
    if (mode == PipelineMode::Both || mode == PipelineMode::SeparateOnly) {
        status.stage    = PipelineStage::Extract;
        status.progress = 0.02f;
        status.message  = "Separating vocals (MDX-Net)…";
        if (!separate_channels(audio_path, out_vocals_wav, inst_wav, status, clip_in, clip_dur)) {
            g_running.store(false);
            return;
        }
        status.progress = 0.20f;
    }

    if (g_cancel.load()) {
        status.stage = PipelineStage::Idle;
        g_running.store(false);
        return;
    }

    if (mode == PipelineMode::SeparateOnly) {
        status.stage    = PipelineStage::Done;
        status.progress = 1.0f;
        status.message  = "Separation complete";
        g_running.store(false);
        return;
    }

    // ── Check ggml model exists ───────────────────────────────────────────────
    fs::path mp = whisper_model_path();
    if (!fs::exists(mp)) {
        status.stage = PipelineStage::Error;
        status.error = "Whisper model not found: " + mp.string() +
                       "\nPlace the models/ folder next to the binary.";
        g_running.store(false);
        return;
    }

    if (g_cancel.load()) {
        status.stage = PipelineStage::Idle;
        g_running.store(false);
        return;
    }

    // ── Decode audio to 16 kHz ────────────────────────────────────────────────
    status.stage    = PipelineStage::Transcribe;
    status.progress = 0.25f;
    status.message  = "Loading audio…";

    // Transcribe vocals if we separated, else original.
    // When using the separated vocals.wav, the clip window was already applied
    // during separation — no extra seek/duration args needed for decode.
    // When transcribing the original (TranscribeOnly), pass the clip window so
    // we only decode the brick's portion of the source.
    bool use_vocals = (mode == PipelineMode::Both && fs::exists(out_vocals_wav));
    std::string tx_src = use_vocals ? out_vocals_wav : audio_path;
    float tx_in  = use_vocals ? 0.f : clip_in;
    float tx_dur = use_vocals ? 0.f : clip_dur;
    std::vector<float> pcm = decode_16k(tx_src, tx_in, tx_dur);
    if (pcm.empty()) {
        status.stage = PipelineStage::Error;
        status.error = "Failed to decode audio";
        g_running.store(false);
        return;
    }

    // Adaptive VAD gate — silence non-speech regions so Whisper drops them
    // instead of hallucinating.  The gated PCM is also fed to forced_align:
    // long zero regions in inter-phrase silences make per-segment normalization
    // sharpen the contrast on real speech, which keeps CTC's vowel-sharing
    // pathology from collapsing adjacent words (e.g. "my pride") into each
    // other.  Gate edges occasionally fire a fake /t/ at sub-segment starts,
    // but the capitalization-driven segment splitter places those boundaries
    // at the real word onsets anyway, so the artifact aligns with the truth
    // instead of distorting it.
    //
    // BUT only on the raw mix. An MDX-Net vocal stem is already isolated —
    // non-vocal regions are quiet by construction — so the gate is redundant
    // AND destructive there: on quiet, reverb-heavy sung stems (dreampop, etc.)
    // its noise-floor estimate sits ABOVE the vocal level and zeros the actual
    // singing, leaving whisper to hallucinate "Thank you." over 30 s of
    // silence. Measured on "Just Like Honey": gated → ' Thank you.'; ungated →
    // the real lyrics. Whisper's own no_speech_thold + the output-side
    // hallucination filter handle dead stems without nuking live ones.
    if (!use_vocals) vad_gate_inplace(pcm);

    // ── Init whisper ──────────────────────────────────────────────────────────
    status.progress = 0.28f;
    status.message  = "Loading whisper large-v3-turbo…";

    // DTW-based word timestamps live in context params (experimental).
    whisper_context_params cparams     = whisper_context_default_params();
    cparams.use_gpu                    = !getenv("PMS_WHISPER_CPU");  // escape hatch for broken Vulkan
    cparams.dtw_token_timestamps       = true;
    cparams.dtw_aheads_preset          = WHISPER_AHEADS_LARGE_V3_TURBO;

    whisper_context* ctx = whisper_init_from_file_with_params(
        mp.string().c_str(), cparams);
    if (!ctx) {
        status.stage = PipelineStage::Error;
        status.error = "Failed to load whisper model";
        g_running.store(false);
        return;
    }

    // ── Transcribe ────────────────────────────────────────────────────────────
    status.progress = 0.35f;
    status.message  = "Transcribing (whisper large-v3-turbo + DTW alignment)…";

    whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    wp.language                   = "en";
    wp.n_threads                  = 4;
    wp.token_timestamps           = true;
    wp.thold_pt                   = 0.01f;
    wp.thold_ptsum                = 0.01f;
    wp.print_progress             = false;
    wp.print_realtime             = false;
    wp.print_timestamps           = false;
    wp.print_special              = false;
    // Condition each 30 s decode window on the previously-decoded text — this is
    // whisper's default and what a plain python `whisper` pass does. Turning it
    // off is what made us miss words: `n_max_text_ctx = 0` severs ALL past-text
    // carryover, so every window decodes in isolation, which drops real words and
    // degrades word/segment boundaries. Measured on a clean spoken stem: mc=0
    // loses ~4% of words vs the default (isolated to this one param — the logprob
    // threshold made no difference); on quiet/sung material whole trailing lines
    // vanish.
    //
    // mc=0 was originally a sledgehammer against a repetition loop (Ween
    // "Transdermal Celebration": a repetitive outro feeds its own decoded tokens
    // back and snowballs into a ~1-seg/sec loop that eats the rest of the song).
    // Modern whisper.cpp breaks that loop WITHOUT severing context: degenerate,
    // repetitive output trips `entropy_thold`, which forces temperature fallback,
    // and history conditioning is auto-disabled once the temperature climbs past
    // 0.5 (WHISPER_HISTORY_CONDITIONING_TEMP_CUTOFF) — so a loop starves its own
    // context while ordinary speech keeps it. We therefore leave n_max_text_ctx
    // at whisper's default (~224 after the internal n_text_ctx/2 clamp).
    // `no_context` only clears a carried-over *initial* prompt; we make a single
    // whisper_full call so it is a no-op here, kept for clarity.
    wp.no_context      = true;
    wp.no_speech_thold = 0.6f;
    wp.entropy_thold   = 2.4f;
    // Temperature fallback breaks intra-chunk repetition loops: high-entropy /
    // low-logprob output retries at T += 0.2 up to 1.0 (and, per the note above,
    // past-text conditioning switches off above T=0.5, cutting the loop's fuel).
    wp.temperature     = 0.0f;
    wp.temperature_inc = 0.2f;
    // Match python whisper's default (-1.0). A segment is deleted as "no speech"
    // only when no_speech_prob > no_speech_thold AND avg_logprob < logprob_thold;
    // the old -0.5 met that AND on ordinary low-confidence lines (sung vocals,
    // quiet or reverberant speech) and silently dropped them. -1.0 restores recall.
    wp.logprob_thold   = -1.0f;

    if (whisper_full(ctx, wp, pcm.data(), (int)pcm.size()) != 0) {
        whisper_free(ctx);
        status.stage = PipelineStage::Error;
        status.error = "Whisper inference failed";
        g_running.store(false);
        return;
    }

    // ── Build JSON ────────────────────────────────────────────────────────────
    status.stage    = PipelineStage::Align;
    status.progress = 0.90f;
    status.message  = "Building word timestamps…";

    nlohmann::json words_arr = nlohmann::json::array();
    nlohmann::json segs_arr  = nlohmann::json::array();
    extract_words_segments(ctx, words_arr, segs_arr);
    whisper_free(ctx);

    // ── Hallucination / non-speech cleanup ────────────────────────────────────
    // The main pipeline never filtered its output (only the search path did), so
    // whisper's stock inventions leaked straight into the canonical cache. Drop
    // music tokens ([Music]/♪♪) on any path — they're never real content — and
    // drop whole segments that are pure speech hallucinations ("Thank you",
    // "thanks for watching") on the vocal-stem path, where they're spurious by
    // construction. Words inside a dropped segment go with it.
    {
        std::vector<std::pair<float, float>> dropped;  // [start,end] removed segs
        nlohmann::json segs_keep = nlohmann::json::array();
        for (auto& s : segs_arr) {
            std::string txt = s.value("text", "");
            bool bad = is_music_token(txt) ||
                       (use_vocals && is_hallucination_phrase(txt));
            if (bad) dropped.push_back({s.value("start", 0.f), s.value("end", 0.f)});
            else     segs_keep.push_back(s);
        }
        segs_arr = std::move(segs_keep);

        nlohmann::json words_keep = nlohmann::json::array();
        for (auto& w : words_arr) {
            if (is_music_token(w.value("word", ""))) continue;
            float ws = w.value("start", 0.f);
            bool in_dropped = false;
            for (auto& d : dropped)
                if (ws >= d.first - 0.01f && ws < d.second + 0.01f) { in_dropped = true; break; }
            if (!in_dropped) words_keep.push_back(w);
        }
        words_arr = std::move(words_keep);
    }

    // ── CTC forced alignment ──────────────────────────────────────────────────
    if (!words_arr.empty() && !pcm.empty()) {
        status.progress = 0.92f;
        status.message  = "CTC forced alignment…";

        std::vector<WordEntry> we_in;
        we_in.reserve(words_arr.size());
        for (auto& w : words_arr) {
            WordEntry e;
            e.text  = w.value("word",  "");
            e.start = w.value("start", 0.f);
            e.end   = w.value("end",   0.f);
            we_in.push_back(e);
        }

        // Build forced-alignment windows: ONE per whisper segment. We used to
        // sub-split a multi-phrase segment at its capitalized words, to keep CTC
        // from aligning across a silent gap inside it. But that split at whisper's
        // word TIMES — and whisper collapses instrumental gaps (it jams a line
        // against the previous phrase, before the gap), so the split landed in the
        // wrong place and scrunched / mis-placed the words. forced_align now
        // excises sustained silence from each window's audio directly
        // (active_ranges) and aligns only the real vocals, so it handles the gaps
        // correctly on its own — the per-phrase split is no longer needed.
        std::vector<std::pair<float,float>> sb;
        for (auto& s : segs_arr) {
            float seg_start = s.value("start", 0.f);
            float seg_end   = s.value("end",   0.f);
            if (seg_end > seg_start) sb.push_back({seg_start, seg_end});
        }

        auto we_out = forced_align(pcm, we_in, proxy_fps, sb);
        if (we_out.size() == we_in.size()) {
            for (size_t i = 0; i < words_arr.size(); ++i) {
                words_arr[i]["start"] = we_out[i].start;
                words_arr[i]["end"]   = we_out[i].end;
            }
        }
    }

    // ── CTC onset compensation ────────────────────────────────────────────────
    // wav2vec2 peak CTC emission sits at the phoneme centre, not the onset,
    // so timestamps feel slightly late on fast syllables.  Shift earlier, then
    // re-snap to the nearest proxy frame so clip placement stays frame-accurate.
    // Clamp to 0 so the first word can't be pushed before the clip's audio start.
    {
        constexpr float kOnsetShift = -0.050f;   // ~50 ms; tune if needed
        auto snap_t = [&](float t) -> float {
            return (proxy_fps > 0.0)
                ? (float)(std::round((double)t * proxy_fps) / proxy_fps)
                : t;
        };
        for (auto& w : words_arr) {
            w["start"] = snap_t(std::max(0.f, w.value("start", 0.f) + kOnsetShift));
            w["end"]   = snap_t(std::max(0.f, w.value("end",   0.f) + kOnsetShift));
        }
    }

    // Whisper timestamps are 0-based relative to the start of the decoded audio.
    // When we clipped the source to [clip_in, clip_in+clip_dur], timestamp 0 in
    // the JSON represents source position clip_in.  Add clip_in back so that the
    // JSON holds source-relative timestamps — apply_subtitle_mode's tl_offset
    // math (tl_offset = clip.start - clip.in_point) then works unchanged.
    if (clip_in > 0.f) {
        for (auto& w : words_arr) {
            w["start"] = w.value("start", 0.f) + clip_in;
            w["end"]   = w.value("end",   0.f) + clip_in;
        }
        for (auto& s : segs_arr) {
            s["start"] = s.value("start", 0.f) + clip_in;
            s["end"]   = s.value("end",   0.f) + clip_in;
        }
    }

    { std::ofstream f(out_words_json); f << words_arr.dump(2); }
    { std::ofstream f(segs_json);      f << segs_arr.dump(2); }
    // Record what audio these words came from so a later lyrics request can tell
    // a vocal-stem transcript from a raw-mix (subtitles) one and re-run if needed.
    transcript_write_source(out_words_json, use_vocals);
    // Record the SOURCE-relative span this transcript covers. A transcript is
    // source-relative (clip_in was added back above), so trimming the brick never
    // invalidates it — trigger_pipeline reuses this to skip re-separation/whisper
    // when a later trim stays inside [span_lo, span_hi]. clip_dur<=0 == whole file.
    {
        float span_lo = clip_in;
        float span_hi = clip_dur > 0.f ? clip_in + clip_dur : 1e9f;
        std::ofstream f(cache_path(audio_path, "_words.span"));
        f << span_lo << " " << span_hi << "\n";
    }

    status.stage    = PipelineStage::Done;
    status.progress = 1.0f;
    status.message  = std::to_string(words_arr.size()) + " words transcribed";
    g_running.store(false);
}

// ── Chunked search ────────────────────────────────────────────────────────────

static whisper_context* load_whisper_ctx() {
    fs::path mp = whisper_search_model_path();
    if (!fs::exists(mp)) return nullptr;
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu              = !getenv("PMS_WHISPER_CPU");  // escape hatch for broken Vulkan
    cparams.dtw_token_timestamps = true;
    cparams.dtw_aheads_preset    = WHISPER_AHEADS_LARGE_V3_TURBO;
    return whisper_init_from_file_with_params(mp.string().c_str(), cparams);
}

// Whisper's known music/non-speech surface tokens.  When fed silence or
// instrumental-only audio (e.g. a dead MDX-Net stem or a raw mix during an
// instrumental break), whisper emits these as "transcribed" content.  We
// strip them at the word level before stitching/saving, so the cache stays
// clean even when we let whisper run on dubious chunks (raw-mix fallback,
// borderline gate trips, etc).
static bool is_music_token(const std::string& w) {
    if (w.empty()) return false;
    // Strip trailing punctuation and lowercase for matching.
    std::string s = w;
    while (!s.empty() && !std::isalnum((unsigned char)s.back()) &&
           (unsigned char)s.back() < 0x80) s.pop_back();
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);

    if (s == "[music]" || s == "(music)" || s == "music"
        || s == "[applause]" || s == "(applause)"
        || s == "[silence]" || s == "(silence)"
        || s == "[blank_audio]" || s == "[inaudible]") return true;
    // Music-note glyphs (♪ U+266A, ♫ U+266B). Whisper emits various run-lengths.
    if (s.empty()) return false;
    bool all_notes = true;
    for (size_t i = 0; i < s.size(); ) {
        // U+266A "♪" is bytes E2 99 AA; U+266B "♫" is E2 99 AB.
        if (i + 2 < s.size() &&
            (unsigned char)s[i] == 0xE2 && (unsigned char)s[i+1] == 0x99 &&
            ((unsigned char)s[i+2] == 0xAA || (unsigned char)s[i+2] == 0xAB)) {
            i += 3;
        } else { all_notes = false; break; }
    }
    return all_notes;
}

// Whisper's stock hallucinations on near-silent / non-speech audio: the YouTube
// outro boilerplate it was trained on ("thank you", "thanks for watching",
// "please subscribe", …). These are never real song lyrics, so we drop whole
// segments that match on the vocal-stem (lyrics) path. Matched on the full,
// normalized segment text (lowercased, punctuation/whitespace collapsed) so a
// real line that merely contains the word "you" is never touched.
static bool is_hallucination_phrase(const std::string& seg_text) {
    std::string s;
    s.reserve(seg_text.size());
    bool prev_space = false;
    for (unsigned char c : seg_text) {
        if (std::isalnum(c)) { s += (char)std::tolower(c); prev_space = false; }
        else if (c == ' ' || c == '\t' || std::ispunct(c)) {
            if (!prev_space && !s.empty()) { s += ' '; prev_space = true; }
        }
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.empty()) return false;

    static const std::unordered_set<std::string> kPhrases = {
        "thank you",
        "thank you very much",
        "thank you so much",
        "thank you all",
        "thanks for watching",
        "thank you for watching",
        "thanks for watching the video",
        "thank you for watching the video",
        "thanks for watching this video",
        "thanks for listening",
        "thank you for listening",
        "please subscribe",
        "like and subscribe",
        "please like and subscribe",
        "dont forget to subscribe",
        "see you next time",
        "see you in the next video",
        "see you next video",
    };
    return kPhrases.count(s) > 0;
}

// Score a word list against query words (intersection / query_size).
SearchStatus transcribe_search_status() {
    std::lock_guard<std::mutex> lk(g_search_mu);
    return g_search_status;
}

static void set_search_status(bool running, float current, float total, const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_search_mu);
    g_search_status.running     = running;
    g_search_status.current_sec = current;
    g_search_status.total_sec   = total;
    g_search_status.progress    = (total > 0.f) ? std::min(current / total, 1.f) : 0.f;
    g_search_status.message     = msg;
}

static std::string fmt_time(float sec) {
    int h = (int)sec / 3600, m = ((int)sec % 3600) / 60, s = (int)sec % 60;
    char buf[16];
    if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else       snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

TranscribeSearchResult transcribe_search(
    const std::string&              path,
    const std::vector<std::string>& query_words,
    float                           buffer_sec)
{
    TranscribeSearchResult res;

    set_search_status(true, 0.f, 0.f, "Loading Whisper model...");

    whisper_context* ctx = load_whisper_ctx();
    if (!ctx) {
        set_search_status(false, 0.f, 0.f, "whisper model not found");
        res.error = "whisper model not found";
        return res;
    }

    whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    wp.language                   = "en";
    wp.n_threads                  = 4;
    wp.token_timestamps           = true;
    wp.thold_pt                   = 0.01f;
    wp.thold_ptsum                = 0.01f;
    wp.print_progress             = false;
    wp.print_realtime             = false;
    wp.print_timestamps           = false;
    wp.print_special              = false;
    // Keep past-text conditioning ON (whisper's default) so we don't drop words,
    // and use python-whisper's default logprob threshold (-1.0). See the
    // do_transcribe site above for why n_max_text_ctx = 0 / logprob -0.5 were
    // removed and how repetition loops are handled without them.
    wp.no_context      = true;
    wp.no_speech_thold = 0.6f;
    wp.entropy_thold   = 2.4f;
    wp.temperature     = 0.0f;
    wp.temperature_inc = 0.2f;
    wp.logprob_thold   = -1.0f;

    MediaFileInfo info = video_probe_file(path);
    if (!info.error.empty()) {
        whisper_free(ctx);
        set_search_status(false, 0.f, 0.f, info.error);
        res.error = info.error;
        return res;
    }
    float total_dur = (float)info.duration;

    // Derive output path for the accumulated transcript.
    // PROVENANCE: search writes to *_words_search.json, NOT *_words.json.
    // The full pipeline (do_transcribe) owns *_words.json — its output is
    // canonical (DTW + forced alignment + segment splits) and must not be
    // shadowed by the windowed search cache.  get_transcript / find_and_add_clip
    // prefer the canonical path and fall back to the search path.
    fs::path src(path);
    std::string words_json_path = cache_path(path, "_words_search.json");

    const float window_sec  = std::max(90.f, std::min(300.f, total_dur / 4.f));
    const float overlap_sec = std::min(30.f, window_sec * 0.15f);
    const float step_sec    = window_sec - overlap_sec;

    float window_start      = 0.f;
    bool  collecting_buffer = false;
    float buffer_end        = 0.f;

    // Separate vocals per chunk when the model is available — dramatically
    // improves Whisper accuracy on music (avoids dropped first lines, bad timings).
    const bool do_separate = separate_model_exists();
    const std::string tmp_vocals = "/tmp/pms_search_vocals.wav";
    const std::string tmp_inst   = "/tmp/pms_search_inst.wav";

    // Accumulate all words across windows; only keep words from each window's
    // non-overlapping region (word.start >= window_start && word.start < window_start + step_sec)
    // to avoid duplicates at overlap boundaries.
    nlohmann::json all_words = nlohmann::json::array();
    float last_saved_end = 0.f; // high-water mark: skip words we've already saved

    while (window_start < total_dur) {
        if (g_cancel.load()) break;

        float dur = std::min(window_sec, total_dur - window_start);
        float win_end = window_start + dur;

        set_search_status(true, window_start, total_dur,
            "Searching " + fmt_time(window_start) + " – " + fmt_time(win_end) +
            " / " + fmt_time(total_dur));

        std::vector<float> pcm;
        bool used_vocal_stem = false;
        if (do_separate) {
            const std::string range = fmt_time(window_start) + " – " + fmt_time(win_end);
            set_search_status(true, window_start, total_dur,
                "MDX-Net " + range + ": separating vocals…");
            std::string sep_err = separate_run(path, tmp_vocals, tmp_inst,
                [&](float /*p*/, const std::string& msg) {
                    set_search_status(true, window_start, total_dur,
                        "MDX-Net " + range + ": " + msg);
                },
                window_start, dur);
            if (sep_err.empty() && fs::exists(tmp_vocals)) {
                set_search_status(true, window_start, total_dur,
                    "MDX-Net " + range + ": decoding vocal stem…");
                pcm = decode_16k(tmp_vocals);
                if (!pcm.empty()) used_vocal_stem = true;
            }
        }
        if (pcm.empty())
            pcm = decode_16k(path, window_start, dur);  // separation unavailable
        if (pcm.empty()) { window_start += step_sec; continue; }

        // Skip whisper only on a GENUINELY SILENT vocal stem (an instrumental
        // window). The old RMS-floor presence gate rejected merely-quiet vocals
        // (dreampop / whispered / outro fades) as "dead" and fell back to the raw
        // mix — which just transcribes the music into [Music]/"Thank you"
        // hallucinations and never finds the actual sung line. A true-silence
        // check keeps quiet vocals; a silent stem is simply skipped (no raw-mix
        // fallback). Same lesson as the full pipeline: don't gate the stem.
        if (used_vocal_stem) {
            float peak = 0.f;
            for (float s : pcm) { float a = fabsf(s); if (a > peak) peak = a; }
            if (peak < 0.005f) { window_start += step_sec; continue; }
        }

        // Only gate the raw mix; an isolated vocal stem is already clean and the
        // gate would zero quiet singing (the bug we fixed in do_transcribe).
        if (!used_vocal_stem) vad_gate_inplace(pcm);

        set_search_status(true, window_start, total_dur,
            "Whisper " + fmt_time(window_start) + " – " + fmt_time(win_end) + "…");

        if (whisper_full(ctx, wp, pcm.data(), (int)pcm.size()) != 0) {
            window_start += step_sec; continue;
        }

        nlohmann::json words_arr = nlohmann::json::array();
        nlohmann::json segs_arr  = nlohmann::json::array();
        extract_words_segments(ctx, words_arr, segs_arr);

        // Post-filter music-token hallucinations ([Music], ♪♪, etc.).
        // Whisper-large emits these on instrumental audio (raw-mix fallback
        // chunks, or stems where MDX-Net leaked instrumental).  Stripping at
        // the word level keeps the cached transcript clean.
        {
            nlohmann::json filtered = nlohmann::json::array();
            for (auto& w : words_arr) {
                std::string wt = w.value("word", "");
                if (!is_music_token(wt)) filtered.push_back(w);
            }
            words_arr = std::move(filtered);
        }

        for (auto& w : words_arr) {
            w["start"] = w.value("start", 0.f) + window_start;
            w["end"]   = w.value("end",   0.f) + window_start;
        }

        // Stitch windows reliably:
        //  - Skip words near the trailing edge of this window (timing unreliable
        //    when whisper can't see what's next; the next window covers them).
        //  - Skip words near the leading edge that may be mid-word artifacts of
        //    the chunk cut (e.g., a phantom /t/ onset at window start).
        //  - Drop content duplicates against words already accumulated.
        const float win_end_abs       = window_start + dur;
        const float trail_skip_sec    = 1.0f;
        const float lead_skip_sec     = (window_start > 0.f) ? 0.3f : 0.f;
        const float dup_window_sec    = 0.6f;

        auto lower = [](std::string s) {
            for (auto& c : s) c = (char)std::tolower((unsigned char)c);
            return s;
        };

        for (const auto& w : words_arr) {
            float ws = w.value("start", 0.f);
            if (ws < window_start + lead_skip_sec) continue;
            if (ws > win_end_abs - trail_skip_sec) continue;

            std::string wt = lower(w.value("word", ""));
            bool is_dup = false;
            // Scan from the back — duplicates will be the most recent entries.
            for (int k = (int)all_words.size() - 1; k >= 0; --k) {
                float ks = all_words[k].value("start", 0.f);
                if (ws - ks > dup_window_sec) break;
                if (lower(all_words[k].value("word", "")) == wt) { is_dup = true; break; }
            }
            if (!is_dup) all_words.push_back(w);
        }
        (void)last_saved_end;

        // Match against the cross-chunk accumulator, not just this chunk's words.
        // Phrases that straddle a chunk boundary need words from both sides — the
        // previous per-chunk match clamped end_i to words_arr.size()-1 and pinned
        // res.end to the chunk's last word, which silently truncated the phrase.
        if (!res.found && !all_words.empty()) {
            auto lower = [](std::string s) {
                for (auto& c : s) c = (char)std::tolower((unsigned char)c);
                return s;
            };
            std::set<std::string> qset;
            for (const auto& q : query_words) qset.insert(lower(q));
            const int qsize = (int)query_words.size();
            const int qwin  = std::max(qsize * 3, qsize + 2);

            int best_hits = 0, best_i = 0, best_e = 0;
            for (int i = 0; i < (int)all_words.size(); ++i) {
                std::set<std::string> seen;
                int e = std::min((int)all_words.size(), i + qwin);
                for (int j = i; j < e; ++j) {
                    std::string w = lower(all_words[j].value("word", ""));
                    if (qset.count(w)) seen.insert(w);
                }
                if ((int)seen.size() > best_hits) {
                    best_hits = (int)seen.size();
                    best_i    = i;
                    best_e    = e - 1;
                }
            }

            const float coverage      = qsize > 0 ? (float)best_hits / (float)qsize : 0.f;
            const bool  match_at_tail = best_e >= (int)all_words.size() - 1;
            const bool  more_file     = (window_start + step_sec) < total_dur;
            // If the best span runs to the very last accumulated word and we
            // haven't matched all query tokens yet, the trailing word(s) are
            // probably in the next chunk. Keep scanning instead of committing
            // a truncated end timestamp.
            const bool  may_truncate  = match_at_tail && coverage < 1.0f && more_file;

            if (coverage >= 0.5f && !may_truncate) {
                // Tighten the span: shrink to the first/last words that actually
                // match a query token, so res.end is the real phrase end (not the
                // trailing filler from the qwin*3 search window).
                int narrow_s = best_i, narrow_e = best_e;
                for (int j = best_i; j <= best_e; ++j) {
                    if (qset.count(lower(all_words[j].value("word", "")))) { narrow_s = j; break; }
                }
                for (int j = best_e; j >= narrow_s; --j) {
                    if (qset.count(lower(all_words[j].value("word", "")))) { narrow_e = j; break; }
                }

                res.found   = true;
                res.start   = all_words[narrow_s].value("start", 0.f);
                res.end     = all_words[narrow_e].value("end",   0.f);
                std::string ex;
                for (int k = narrow_s; k <= narrow_e && k < (int)all_words.size(); ++k)
                    ex += all_words[k].value("word", "") + " ";
                res.excerpt = ex;

                collecting_buffer = true;
                buffer_end        = res.end + buffer_sec;

                set_search_status(true, res.start, total_dur,
                    "Found at " + fmt_time(res.start) + " — collecting buffer...");
            } else if (coverage >= 0.5f && may_truncate) {
                set_search_status(true, window_start, total_dur,
                    "Partial match at chunk tail — extending into next chunk...");
            }
        }

        if (collecting_buffer && window_start + dur >= buffer_end) break;

        window_start += step_sec;
    }

    // Save accumulated transcript to disk so get_transcript / find_video_moment can use it
    if (!all_words.empty()) {
        std::ofstream f(words_json_path);
        if (f) f << all_words.dump(2);
    }

    whisper_free(ctx);

    // On no-match, surface the top-N closest fuzzy windows so the caller
    // / human can disambiguate or re-query instead of getting a blank
    // failure.  Scans the accumulated transcript for the highest-coverage
    // disjoint windows above 0.5 coverage.
    std::vector<SearchCandidate> candidates;
    if (!res.found && !all_words.empty()) {
        auto lower = [](std::string s) {
            for (auto& c : s) c = (char)std::tolower((unsigned char)c);
            return s;
        };
        std::set<std::string> qset;
        for (const auto& q : query_words) qset.insert(lower(q));
        const int qsize = (int)query_words.size();
        const int qwin  = std::max(qsize * 3, qsize + 2);
        if (qsize > 0) {
            struct Hit { int i, e; float score; };
            std::vector<Hit> hits;
            for (int i = 0; i < (int)all_words.size(); ++i) {
                std::set<std::string> seen;
                int e = std::min((int)all_words.size(), i + qwin);
                for (int j = i; j < e; ++j) {
                    std::string w = lower(all_words[j].value("word", ""));
                    if (qset.count(w)) seen.insert(w);
                }
                float cov = (float)seen.size() / (float)qsize;
                if (cov >= 0.5f) hits.push_back({i, e - 1, cov});
            }
            std::sort(hits.begin(), hits.end(),
                [](const Hit& a, const Hit& b) { return a.score > b.score; });
            // Greedily keep up to 3 disjoint hits.
            for (const auto& h : hits) {
                if (candidates.size() >= 3) break;
                bool overlap = false;
                for (const auto& c : candidates) {
                    float hs = all_words[h.i].value("start", 0.f);
                    float he = all_words[h.e].value("end",   0.f);
                    if (!(he < c.start || hs > c.end)) { overlap = true; break; }
                }
                if (overlap) continue;
                SearchCandidate sc;
                sc.start = all_words[h.i].value("start", 0.f);
                sc.end   = all_words[h.e].value("end",   0.f);
                sc.score = h.score;
                std::string ex;
                for (int k = h.i; k <= h.e && k < (int)all_words.size(); ++k)
                    ex += all_words[k].value("word", "") + " ";
                sc.excerpt = ex;
                candidates.push_back(sc);
            }
        }
    }

    if (!res.found) {
        res.error = candidates.empty()
            ? "query not found in transcript"
            : "query not found exactly — see candidates for closest fuzzy hits";
    }
    {
        std::lock_guard<std::mutex> lk(g_search_mu);
        g_search_status.running     = false;
        g_search_status.found       = res.found;
        g_search_status.start       = res.start;
        g_search_status.end         = res.end;
        g_search_status.excerpt     = res.excerpt;
        g_search_status.candidates  = std::move(candidates);
        g_search_status.error       = res.error;
        g_search_status.progress    = 1.f;
        g_search_status.current_sec = res.found ? res.start : total_dur;
        g_search_status.total_sec   = total_dur;
        g_search_status.message     = res.found
                                    ? "Found at " + fmt_time(res.start)
                                    : "Not found";
    }
    return res;
}

// ── Public API ────────────────────────────────────────────────────────────────

void transcribe_start(
    const std::string& audio_path,
    PipelineStatus&    status,
    std::string&       out_words_json,
    std::string&       out_vocals_wav,
    PipelineMode       mode,
    double             proxy_fps,
    float              clip_in,
    float              clip_dur)
{
    if (g_running.load()) return;
    g_cancel.store(false);
    g_running.store(true);

    status.stage    = PipelineStage::Extract;
    status.progress = 0.01f;
    status.message  = "Starting pipeline…";

    g_thread = std::thread(do_transcribe,
        audio_path, std::ref(status),
        std::ref(out_words_json), std::ref(out_vocals_wav),
        mode, proxy_fps, clip_in, clip_dur);
    g_thread.detach();
}

void transcribe_cancel() { g_cancel.store(true); }
bool transcribe_running() { return g_running.load(); }

void transcribe_search_start(
    const std::string&              path,
    const std::vector<std::string>& query_words,
    float                           buffer_sec)
{
    {
        std::lock_guard<std::mutex> lk(g_search_mu);
        if (g_search_status.running) return;
        g_search_status = SearchStatus{};
        g_search_status.running = true;
        g_search_status.message = "Starting search...";
    }
    g_search_thread = std::thread([path, query_words, buffer_sec]() {
        transcribe_search(path, query_words, buffer_sec);
    });
    g_search_thread.detach();
}

bool transcribe_search_running() {
    std::lock_guard<std::mutex> lk(g_search_mu);
    return g_search_status.running;
}
