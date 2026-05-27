#include "transcribe.h"
#include "separate.h"
#include "paths.h"
#include "forced_align.h"
#include <whisper.h>
#include <thread>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include "json.hpp"

namespace fs = std::filesystem;

static std::thread       g_thread;
static std::atomic<bool> g_running{false};
static std::atomic<bool> g_cancel{false};

// ── Whisper ggml model path ───────────────────────────────────────────────────

fs::path whisper_model_path() {
    return fs::path(app_models_dir()) / "ggml-large-v3-turbo-q5_0.bin";
}

bool whisper_model_exists() { return fs::exists(whisper_model_path()); }

// ── Audio helpers ─────────────────────────────────────────────────────────────

// Decode audio to 16 kHz mono float32 via ffmpeg.
// clip_in / clip_dur: when clip_dur > 0, only decode [clip_in, clip_in+clip_dur] seconds.
static std::vector<float> decode_16k(const std::string& path,
                                      float clip_in = 0.f, float clip_dur = 0.f) {
    std::string seek_args;
    if (clip_in  > 0.f) seek_args += " -ss " + std::to_string(clip_in);
    std::string dur_args;
    if (clip_dur > 0.f) dur_args  += " -t "  + std::to_string(clip_dur);

    std::string cmd = "ffmpeg -hide_banner -loglevel error" + seek_args +
                      " -i \"" + path + "\"" + dur_args +
                      " -vn -ar 16000 -ac 1 -f f32le pipe:1 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return {};
    std::vector<float> s;
    float buf[4096];
    size_t n;
    while ((n = fread(buf, sizeof(float), 4096, fp)) > 0)
        s.insert(s.end(), buf, buf + n);
    pclose(fp);
    return s;
}

// ── Stem separation ───────────────────────────────────────────────────────────

// Calls the C++ MDX-Net (Kim_Vocal_2) vocal separation. Returns false and sets status on error.
// No ffmpeg fallback — if the model is not ready, the user must download the model.
static bool separate_channels(
    const std::string& in,
    const std::string& outdir,
    PipelineStatus&    status,
    float clip_in  = 0.f,
    float clip_dur = 0.f)
{
    std::string voc  = outdir + "/vocals.wav";
    std::string inst = outdir + "/instrumental.wav";

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
    fs::path audio(audio_path);
    fs::path outdir = audio.parent_path() / audio.stem();
    fs::create_directories(outdir);

    std::string stem       = audio.stem().string();
    out_words_json         = (outdir / (stem + "_words.json")).string();
    out_vocals_wav         = (outdir / "vocals.wav").string();
    std::string segs_json  = (outdir / (stem + "_segments.json")).string();

    // ── Separation ────────────────────────────────────────────────────────────
    if (mode == PipelineMode::Both || mode == PipelineMode::SeparateOnly) {
        status.stage    = PipelineStage::Extract;
        status.progress = 0.02f;
        status.message  = "Separating vocals (MDX-Net)…";
        if (!separate_channels(audio_path, outdir.string(), status, clip_in, clip_dur)) {
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

    // ── Init whisper ──────────────────────────────────────────────────────────
    status.progress = 0.28f;
    status.message  = "Loading whisper large-v3-turbo…";

    // DTW-based word timestamps live in context params (experimental).
    whisper_context_params cparams     = whisper_context_default_params();
    cparams.use_gpu                    = true;
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
    wp.language         = "en";
    wp.n_threads        = 4;
    wp.token_timestamps = true;
    wp.thold_pt         = 0.01f;
    wp.thold_ptsum      = 0.01f;
    wp.print_progress   = false;
    wp.print_realtime   = false;
    wp.print_timestamps = false;
    wp.print_special    = false;

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

        auto we_out = forced_align(pcm, we_in, proxy_fps);
        if (we_out.size() == we_in.size()) {
            for (size_t i = 0; i < words_arr.size(); ++i) {
                words_arr[i]["start"] = we_out[i].start;
                words_arr[i]["end"]   = we_out[i].end;
            }
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

    status.stage    = PipelineStage::Done;
    status.progress = 1.0f;
    status.message  = std::to_string(words_arr.size()) + " words transcribed";
    g_running.store(false);
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
