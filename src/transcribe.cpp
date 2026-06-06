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

// Faster model used by find_and_add_clip search (tiny.en ≈ 10× faster than large).
// Falls back to large if tiny isn't present.
fs::path whisper_search_model_path() {
    fs::path tiny = fs::path(app_models_dir()) / "ggml-tiny.en.bin";
    if (fs::exists(tiny)) return tiny;
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

        // Pass whisper's segment boundaries so forced_align uses exact segment
        // audio windows for best timing accuracy.
        std::vector<std::pair<float,float>> sb;
        sb.reserve(segs_arr.size());
        for (auto& s : segs_arr)
            sb.push_back({s.value("start", 0.f), s.value("end", 0.f)});

        auto we_out = forced_align(pcm, we_in, proxy_fps, sb);
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

// ── Chunked search ────────────────────────────────────────────────────────────

static whisper_context* load_whisper_ctx() {
    fs::path mp = whisper_search_model_path();
    if (!fs::exists(mp)) return nullptr;
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = true;
    return whisper_init_from_file_with_params(mp.string().c_str(), cparams);
}

// Score a word list against query words (intersection / query_size).
static float score_words(const nlohmann::json& words,
                         const std::vector<std::string>& query_words) {
    if (query_words.empty()) return 0.f;
    std::set<std::string> qset(query_words.begin(), query_words.end());
    int hits = 0;
    int window = (int)query_words.size() * 3;
    // Slide a window over the words list looking for the best match
    for (int i = 0; i < (int)words.size(); ++i) {
        std::set<std::string> seen;
        for (int j = i; j < std::min((int)words.size(), i + window); ++j) {
            std::string w = words[j].value("word", "");
            // lowercase
            for (auto& c : w) c = (char)std::tolower((unsigned char)c);
            if (qset.count(w)) seen.insert(w);
        }
        hits = std::max(hits, (int)seen.size());
    }
    return (float)hits / (float)query_words.size();
}

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
    wp.language         = "en";
    wp.n_threads        = 4;
    wp.token_timestamps = true;
    wp.thold_pt         = 0.01f;
    wp.thold_ptsum      = 0.01f;
    wp.print_progress   = false;
    wp.print_realtime   = false;
    wp.print_timestamps = false;
    wp.print_special    = false;

    MediaFileInfo info = video_probe_file(path);
    if (!info.error.empty()) {
        whisper_free(ctx);
        set_search_status(false, 0.f, 0.f, info.error);
        res.error = info.error;
        return res;
    }
    float total_dur = (float)info.duration;

    // Derive output path for the accumulated transcript
    fs::path src(path);
    fs::path outdir = src.parent_path() / src.stem();
    fs::create_directories(outdir);
    std::string words_json_path = (outdir / (src.stem().string() + "_words.json")).string();

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
        if (do_separate) {
            set_search_status(true, window_start, total_dur,
                "Separating vocals " + fmt_time(window_start) + " – " + fmt_time(win_end) + "…");
            std::string sep_err = separate_run(path, tmp_vocals, tmp_inst,
                [&](float /*p*/, const std::string& msg) {
                    set_search_status(true, window_start, total_dur,
                        fmt_time(window_start) + ": " + msg);
                },
                window_start, dur);
            if (sep_err.empty() && fs::exists(tmp_vocals))
                pcm = decode_16k(tmp_vocals);
        }
        if (pcm.empty())
            pcm = decode_16k(path, window_start, dur);  // fallback: raw mix
        if (pcm.empty()) { window_start += step_sec; continue; }

        if (whisper_full(ctx, wp, pcm.data(), (int)pcm.size()) != 0) {
            window_start += step_sec; continue;
        }

        nlohmann::json words_arr = nlohmann::json::array();
        nlohmann::json segs_arr  = nlohmann::json::array();
        extract_words_segments(ctx, words_arr, segs_arr);

        for (auto& w : words_arr) {
            w["start"] = w.value("start", 0.f) + window_start;
            w["end"]   = w.value("end",   0.f) + window_start;
        }

        // Accumulate non-duplicate words (past the high-water mark)
        for (const auto& w : words_arr) {
            float ws = w.value("start", 0.f);
            if (ws >= last_saved_end)
                all_words.push_back(w);
        }
        if (!words_arr.empty()) {
            float new_end = words_arr.back().value("end", 0.f);
            // Advance high-water mark to the non-overlap boundary so the next
            // window's overlap region doesn't re-add the same words.
            last_saved_end = std::max(last_saved_end, window_start + step_sec);
            (void)new_end;
        }

        float score = score_words(words_arr, query_words);
        if (score >= 0.5f && !res.found) {
            int qwin = (int)query_words.size() * 3;
            std::set<std::string> qset(query_words.begin(), query_words.end());
            int best_hits = 0, best_i = 0;
            for (int i = 0; i < (int)words_arr.size(); ++i) {
                std::set<std::string> seen;
                for (int j = i; j < std::min((int)words_arr.size(), i + qwin); ++j) {
                    std::string w = words_arr[j].value("word", "");
                    for (auto& c : w) c = (char)std::tolower((unsigned char)c);
                    if (qset.count(w)) seen.insert(w);
                }
                if ((int)seen.size() > best_hits) { best_hits = (int)seen.size(); best_i = i; }
            }
            int end_i = std::min((int)words_arr.size() - 1, best_i + qwin - 1);
            res.found = true;
            res.start = words_arr[best_i].value("start", 0.f);
            res.end   = words_arr[end_i].value("end",   0.f);
            std::string ex;
            for (int k = best_i; k <= end_i && k < (int)words_arr.size(); ++k)
                ex += words_arr[k].value("word", "") + " ";
            res.excerpt      = ex;
            collecting_buffer = true;
            buffer_end       = res.end + buffer_sec;

            set_search_status(true, res.start, total_dur,
                "Found at " + fmt_time(res.start) + " — collecting buffer...");
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
    if (!res.found) res.error = "query not found in transcript";
    {
        std::lock_guard<std::mutex> lk(g_search_mu);
        g_search_status.running     = false;
        g_search_status.found       = res.found;
        g_search_status.start       = res.start;
        g_search_status.end         = res.end;
        g_search_status.excerpt     = res.excerpt;
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
