#include "studio_types.h"
#include "studio_shared.h"
#include "pipeline.h"
#include "panel_animation.h"
#include "app.h"
#include "audio.h"
#include "video.h"
#include "proxy.h"
#include "history.h"
#include "filepicker.h"
#include "transcribe.h"
#include "vision_download.h"
#include "beat_detect.h"
#include "waveform.h"
#include "theme.h"
#include "render.h"
#include "json.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <mutex>
#include <unordered_set>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace fs = std::filesystem;
extern ImFont* g_font_bold;

// Parse an SRT file into a list of Text clips.
std::vector<Clip> parse_srt(const std::string& path) {
    std::vector<Clip> out;
    std::ifstream f(path);
    if (!f) return out;

    auto parse_ts = [](const std::string& s) -> float {
        // HH:MM:SS,mmm
        int h=0, m=0, sec=0, ms=0;
        sscanf(s.c_str(), "%d:%d:%d,%d", &h, &m, &sec, &ms);
        return h*3600.f + m*60.f + sec + ms*0.001f;
    };

    std::string line;
    while (std::getline(f, line)) {
        // Strip carriage return for Windows-encoded SRTs
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Look for timecode line: HH:MM:SS,mmm --> HH:MM:SS,mmm
        float t0 = 0.f, t1 = 0.f;
        char buf0[32]={}, buf1[32]={};
        if (sscanf(line.c_str(), "%31[^-]-->%31s", buf0, buf1) == 2) {
            t0 = parse_ts(std::string(buf0));
            t1 = parse_ts(std::string(buf1));

            // Collect text lines until blank
            std::string text;
            while (std::getline(f, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) break;
                if (!text.empty()) text += ' ';
                text += line;
            }
            if (!text.empty()) {
                Clip c;
                c.clip_type = ClipType::Subtitle;
                c.source_id = path;
                c.start     = t0;
                c.end       = t1;
                c.text      = text;
                out.push_back(c);
            }
        }
    }
    return out;
}

// ── Subtitle grouping ─────────────────────────────────────────────────────────

// Build clips from a flat list of words using the requested grouping mode.
// pause_gap: override default gap threshold (0 = use mode default).
// max_words: force a group break after this many words regardless of gap (0 = no limit).
std::vector<Clip> group_words(
    const std::vector<Clip>& words,
    SubtitleMode mode, int custom_n,
    float pause_gap, int max_words)
{
    if (words.empty()) return {};
    std::vector<Clip> out;

    // Helper: append word to current group, flushing if max_words hit.
    auto flush_if_full = [&](Clip& cur, int& wcount) {
        if (max_words > 0 && wcount >= max_words) {
            out.push_back(cur);
            cur = Clip{};
            wcount = 0;
            return true;
        }
        return false;
    };

    switch (mode) {
    case SubtitleMode::Word: {
        // Extend each word to the next word's start to fill intra-sentence gaps,
        // but stop at the word's natural CTC end when a sentence-level pause follows.
        constexpr float kSentenceGap = 0.4f;
        std::vector<Clip> out = words;
        for (size_t i = 0; i + 1 < out.size(); ++i) {
            if (out[i + 1].start - out[i].end < kSentenceGap)
                out[i].end = out[i + 1].start;
        }
        return out;
    }

    case SubtitleMode::Phrase: {
        float thresh = (pause_gap > 0.f) ? pause_gap : 0.3f;
        Clip cur = words[0]; int wc = 1;
        for (size_t i = 1; i < words.size(); ++i) {
            float gap = words[i].start - words[i-1].end;
            if (gap > thresh || (max_words > 0 && wc >= max_words)) {
                out.push_back(cur); cur = words[i]; wc = 1;
            } else {
                cur.text += " " + words[i].text; cur.end = words[i].end; ++wc;
            }
        }
        out.push_back(cur);
        break;
    }

    case SubtitleMode::Line: {
        float thresh = (pause_gap > 0.f) ? pause_gap : 0.8f;
        Clip cur = words[0]; int wc = 1;
        for (size_t i = 1; i < words.size(); ++i) {
            float gap = words[i].start - words[i-1].end;
            if (gap > thresh || (max_words > 0 && wc >= max_words)) {
                out.push_back(cur); cur = words[i]; wc = 1;
            } else {
                cur.text += " " + words[i].text; cur.end = words[i].end; ++wc;
            }
        }
        out.push_back(cur);
        break;
    }

    case SubtitleMode::CustomN: {
        int n = (custom_n < 1) ? 1 : custom_n;
        if (max_words > 0 && max_words < n) n = max_words;
        for (size_t i = 0; i < words.size(); ) {
            Clip c = words[i++];
            for (int k = 1; k < n && i < words.size(); ++k, ++i) {
                c.text += " " + words[i].text; c.end = words[i].end;
            }
            out.push_back(c);
        }
        break;
    }

    case SubtitleMode::Karaoke: {
        float thresh = (pause_gap > 0.f) ? pause_gap : 0.8f;
        auto lines = group_words(words, SubtitleMode::Line, custom_n, thresh, max_words);
        for (auto& c : lines) c.karaoke = true;
        return lines;
    }

    case SubtitleMode::Segment:
        return group_words(words, SubtitleMode::Line, custom_n, pause_gap, max_words);
    }

    // Suppress unused-variable warning from the lambda (only used in old draft).
    (void)flush_if_full;
    return out;
}

// Load the flat word list from words_json_path into AppState::words_cache.
// ── Beat detection subprocess ─────────────────────────────────────────────────

// ── Beat detect completion queue ──────────────────────────────────────────────
// Background threads post results here; main thread drains each frame.
static std::mutex              s_beat_queue_mtx;
static std::vector<BeatResult> s_beat_queue;
// Tracks which source_ids are currently being analysed (prevents duplicate jobs).
static std::mutex              s_beat_pending_mtx;
static std::unordered_set<std::string> s_beat_pending;

void run_beat_detect(AppState& state) {
    if (state.beats_running) return;
    std::string src = state.vocals_path.empty() ? state.audio_path : state.vocals_path;
    if (src.empty() || !fs::exists(src)) return;
    state.beats_running = true;
    std::thread([&state, src]() {
        BeatResult r = beat_detect(src);
        state.beats_running = false;
        if (r.ok) {
            state.beat_bpm = r.bpm;
            state.beats    = std::move(r.beats);
        }
    }).detach();
}

// ── Per-clip beat analysis ────────────────────────────────────────────────────

void kick_clip_beat_detect(const std::string& src) {
    {
        std::lock_guard<std::mutex> lk(s_beat_pending_mtx);
        if (s_beat_pending.count(src)) return; // already running
        s_beat_pending.insert(src);
    }
    std::thread([src]() {
        BeatResult r = beat_detect(src);
        {
            std::lock_guard<std::mutex> lk(s_beat_queue_mtx);
            s_beat_queue.push_back(std::move(r));
        }
        {
            std::lock_guard<std::mutex> lk(s_beat_pending_mtx);
            s_beat_pending.erase(src);
        }
    }).detach();
}

// Poll per-frame: drain completion queue and auto-trigger analysis.
void poll_clip_beat_analysis(AppState& state) {
    // Drain completed results — match by source_id across all clips
    std::vector<BeatResult> done;
    {
        std::lock_guard<std::mutex> lk(s_beat_queue_mtx);
        done.swap(s_beat_queue);
    }
    for (auto& r : done) {
        for (auto& track : state.tracks) {
            for (auto& cl : track.clips) {
                if (cl.source_id != r.source_id) continue;
                cl.beats_analyzing = false;
                if (r.ok) { cl.beat_bpm = r.bpm; cl.beats = r.beats; }
            }
        }
    }

    // Auto-trigger for any Audio/Video clip that has no beats and isn't pending
    for (auto& track : state.tracks) {
        for (auto& cl : track.clips) {
            if (cl.clip_type != ClipType::Video && cl.clip_type != ClipType::Audio) continue;
            if (cl.source_id.empty() || !cl.beats.empty() || cl.beats_analyzing) continue;
            cl.beats_analyzing = true;
            kick_clip_beat_detect(cl.source_id);
        }
    }
}

// ── Envelope extraction subprocess ───────────────────────────────────────────

void load_envelope_cache(AppState& state) {
    state.amplitude_envelope.clear();
    state.envelope_fps = 0.f;
    if (state.envelope_json_path.empty() || !fs::exists(state.envelope_json_path)) return;
    std::ifstream f(state.envelope_json_path);
    if (!f) return;
    try {
        auto j = nlohmann::json::parse(f);
        state.envelope_fps = j.value("fps", 0.f);
        for (auto& v : j["rms"])
            state.amplitude_envelope.push_back(v.get<float>());
    } catch (...) {}
}

void run_envelope_extract(AppState& state) {
    if (state.envelope_running) return;
    std::string src = state.vocals_path.empty() ? state.audio_path : state.vocals_path;
    if (src.empty() || !fs::exists(src)) return;

    fs::path out = fs::path(src).parent_path() / "envelope.json";
    state.envelope_json_path = out.string();
    state.envelope_running   = true;

    std::string outpath = out.string();

    std::thread([&state, src, outpath]() {
        const float env_fps = 24.f;

        // Probe sample rate
        std::string probe = "ffprobe -v error -select_streams a:0"
                            " -show_entries stream=sample_rate"
                            " -of default=noprint_wrappers=1:nokey=1"
                            " \"" + src + "\" 2>/dev/null";
        int sr = 44100;
        {
            FILE* p = popen(probe.c_str(), "r");
            if (p) {
                char buf[32] = {};
                if (fgets(buf, sizeof(buf), p)) sr = std::max(1, atoi(buf));
                pclose(p);
            }
        }

        int samples_per_frame = std::max(1, (int)(sr / env_fps));

        // Decode mono f32le PCM via pipe
        std::string cmd = "ffmpeg -hide_banner -loglevel error"
                          " -i \"" + src + "\""
                          " -vn -ac 1 -f f32le pipe:1 2>/dev/null";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            state.envelope_running = false;
            return;
        }

        std::vector<float> rms_vals;
        float sample = 0.f;
        double sum_sq = 0.0;
        int n = 0;
        while (fread(&sample, sizeof(float), 1, pipe) == 1) {
            sum_sq += (double)sample * sample;
            if (++n >= samples_per_frame) {
                rms_vals.push_back((float)std::sqrt(sum_sq / n));
                sum_sq = 0.0;
                n = 0;
            }
        }
        if (n > 0) rms_vals.push_back((float)std::sqrt(sum_sq / n));
        pclose(pipe);

        // Normalize to [0, 1]
        float peak = 0.f;
        for (float v : rms_vals) peak = std::max(peak, v);
        if (peak > 0.f)
            for (float& v : rms_vals) v /= peak;

        // Write JSON
        nlohmann::json j;
        j["fps"] = env_fps;
        j["rms"] = rms_vals;
        std::ofstream f(outpath);
        f << j.dump();

        state.envelope_running = false;
        load_envelope_cache(state);
    }).detach();
}

// ─────────────────────────────────────────────────────────────────────────────

void load_words_cache(AppState& state) {
    state.words_cache.clear();
    if (state.words_json_path.empty() || !fs::exists(state.words_json_path)) return;
    std::ifstream f(state.words_json_path);
    if (!f) return;
    try {
        auto j = nlohmann::json::parse(f);
        for (auto& w : j) {
            if (!w.contains("word") || !w.contains("start") || !w.contains("end")) continue;
            WordEntry e;
            e.text  = w["word"].get<std::string>();
            e.start = w["start"].get<float>();
            e.end   = w["end"].get<float>();
            state.words_cache.push_back(std::move(e));
        }
    } catch (...) {}
}

// Split a string on ASCII whitespace into tokens.
static std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
        if (i >= s.size()) break;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t' && s[j] != '\n' && s[j] != '\r') ++j;
        out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

void apply_lyrics_edits(AppState& state, Clip& c) {
    if (state.lyrics_edits.empty() || c.words.empty()) return;
    auto tokens = split_words(c.text);
    if (tokens.size() != c.words.size()) return;  // mismatched grouping — skip
    bool changed = false;
    for (int i = 0; i < (int)c.words.size(); ++i) {
        int frame = (int)(c.words[i].start * (float)state.fps);
        auto it = state.lyrics_edits.find(frame);
        if (it != state.lyrics_edits.end() && it->second != tokens[i]) {
            tokens[i]       = it->second;
            c.words[i].text = it->second;
            changed = true;
        }
    }
    if (changed) {
        c.text.clear();
        for (int i = 0; i < (int)tokens.size(); ++i) {
            if (i) c.text += ' ';
            c.text += tokens[i];
        }
    }
}

// Load word JSON and apply current grouping mode.
// generate_typography is defined in panel_animation.cpp, declared in panel_animation.h

// Removes all Lyrics clips with matching source_id from ALL tracks, then
// places fresh grouped clips on the "Lyrics" track.
void apply_subtitle_mode(AppState& state) {
    if (state.words_json_path.empty()) return;
    const std::string src = state.audio_path;

    // Compute timeline offset: whisper timestamps are relative to the audio file
    // start, but the clip may sit at a non-zero timeline position or have an in_point.
    // offset = clip.start - clip.in_point
    float tl_offset = 0.f;
    for (auto& t : state.tracks) {
        bool found = false;
        for (auto& c : t.clips) {
            if ((c.clip_type == ClipType::Audio || c.clip_type == ClipType::Video) &&
                c.source_id == src) {
                tl_offset = c.start - c.in_point;
                found = true;
                break;
            }
        }
        if (found) break;
    }

    // Remove all Lyrics clips from this source across all tracks
    if (!src.empty()) {
        for (auto& t : state.tracks) {
            t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(),
                [&](const Clip& c){ return c.clip_type == ClipType::Lyrics && c.source_id == src; }),
                t.clips.end());
        }
    }

    auto stamp = [&](Clip& c){
        c.clip_type = ClipType::Lyrics;
        c.source_id = src;
        c.sub_pos   = 1;          // center
    };

    // For Segment mode, read _segments.json instead
    if (state.subtitle_mode == SubtitleMode::Segment &&
        !state.segments_json_path.empty() &&
        fs::exists(state.segments_json_path)) {
        std::ifstream f(state.segments_json_path);
        if (!f) return;
        try {
            auto j = nlohmann::json::parse(f);
            Track* lyrics = nullptr;
            for (auto& t : state.tracks)
                if (t.name == "Lyrics") { lyrics = &t; break; }
            if (!lyrics) {
                state.tracks.insert(state.tracks.begin(), Track{});
                lyrics = &state.tracks.front();
                lyrics->name = "Lyrics";
            }
            lyrics->managed = true;
            lyrics->clips.clear();
            float latency = audio_latency();
            for (auto& seg : j) {
                Clip c;
                c.text  = seg["text"].get<std::string>();
                c.start = snap_to_frame(seg["start"].get<float>() + tl_offset - latency, state.fps);
                c.end   = snap_end_to_frame(seg["end"].get<float>() + tl_offset - latency, state.fps);
                if (c.end < 0.f) continue;  // skip clips shifted before timeline start
                stamp(c);
                lyrics->clips.push_back(c);
            }
            // Extend project duration so all lyrics are reachable on the timeline.
            for (auto& c : lyrics->clips)
                if (c.end > state.duration) state.duration = c.end;
        } catch (...) {}
        return;
    }

    // Word-level JSON → group
    std::ifstream f(state.words_json_path);
    if (!f) return;
    try {
        auto j = nlohmann::json::parse(f);
        float latency = audio_latency();
        std::vector<Clip> raw;
        for (auto& w : j) {
            Clip c;
            c.text  = w["word"].get<std::string>();
            c.start = snap_to_frame(w["start"].get<float>() + tl_offset - latency, state.fps);
            c.end   = snap_end_to_frame(w["end"].get<float>() + tl_offset - latency, state.fps);
            raw.push_back(c);
        }
        auto grouped = group_words(raw, state.subtitle_mode, state.subtitle_n);
        for (auto& c : grouped) stamp(c);

        // Drop clips that land entirely before timeline start (can happen when
        // in_point > 0 causes a negative tl_offset shifting early words off-screen).
        grouped.erase(std::remove_if(grouped.begin(), grouped.end(),
            [](const Clip& c){ return c.end < 0.f; }), grouped.end());

        // Populate per-clip word lists from the raw word entries
        std::vector<WordEntry> all_words;
        for (auto& w : j) {
            WordEntry we;
            we.text  = w["word"].get<std::string>();
            we.start = w["start"].get<float>() + tl_offset - latency;
            we.end   = w["end"].get<float>()   + tl_offset - latency;
            all_words.push_back(we);
        }
        for (auto& c : grouped) {
            c.words.clear();
            for (auto& we : all_words)
                if (we.start >= c.start - 0.001f && we.end <= c.end + 0.001f)
                    c.words.push_back(we);
            apply_lyrics_edits(state, c);
        }

        Track* lyrics = nullptr;
        for (auto& t : state.tracks)
            if (t.name == "Lyrics") { lyrics = &t; break; }
        if (!lyrics) {
            state.tracks.insert(state.tracks.begin(), Track{});
            lyrics = &state.tracks.front();
            lyrics->name = "Lyrics";
        }
        lyrics->managed = true;
        lyrics->clips = grouped;

        // Extend project duration so all lyrics are reachable on the timeline.
        for (auto& c : lyrics->clips)
            if (c.end > state.duration) state.duration = c.end;
    } catch (...) {}
}

// Load segments JSON and populate a "Subtitles" track with ClipType::Subtitle clips.
void apply_subtitle_pipeline(AppState& state) {
    if (state.segments_json_path.empty()) return;
    std::ifstream f(state.segments_json_path);
    if (!f) return;
    const std::string src = state.audio_path;

    float tl_offset = 0.f;
    for (auto& t : state.tracks) {
        bool found = false;
        for (auto& c : t.clips) {
            if ((c.clip_type == ClipType::Audio || c.clip_type == ClipType::Video) &&
                c.source_id == src) {
                tl_offset = c.start - c.in_point;
                found = true;
                break;
            }
        }
        if (found) break;
    }

    // Remove existing Subtitle clips from this source across all tracks
    if (!src.empty()) {
        for (auto& t : state.tracks) {
            t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(),
                [&](const Clip& c){ return c.clip_type == ClipType::Subtitle && c.source_id == src; }),
                t.clips.end());
        }
    }
    try {
        auto j = nlohmann::json::parse(f);
        Track* tr = nullptr;
        for (auto& t : state.tracks)
            if (t.name == "Subtitles") { tr = &t; break; }
        if (!tr) {
            state.tracks.insert(state.tracks.begin(), Track{});
            tr = &state.tracks.front();
            tr->name = "Subtitles";
        }
        tr->clips.clear();
        float latency = audio_latency();
        for (auto& seg : j) {
            Clip c;
            c.clip_type = ClipType::Subtitle;
            c.source_id = src;
            c.sub_pos   = 3;      // custom Y — slightly below center
            c.sub_pos_y = 0.62f;
            c.text  = seg["text"].get<std::string>();
            c.start = snap_to_frame(seg["start"].get<float>() + tl_offset - latency, state.fps);
            c.end   = snap_end_to_frame(seg["end"].get<float>() + tl_offset - latency, state.fps);
            if (c.end < 0.f) continue;  // skip clips shifted before timeline start
            tr->clips.push_back(c);
        }
        // Extend project duration so all subtitles are reachable on the timeline.
        for (auto& c : tr->clips)
            if (c.end > state.duration) state.duration = c.end;
    } catch (...) {}
}

// Save all four SRT variants to the output directory.
void save_all_srts(AppState& state) {
    if (state.words_json_path.empty()) return;

    std::ifstream f(state.words_json_path);
    if (!f) return;
    std::vector<Clip> raw;
    try {
        auto j = nlohmann::json::parse(f);
        for (auto& w : j) {
            Clip c;
            c.text  = w["word"].get<std::string>();
            c.start = w["start"].get<float>();
            c.end   = w["end"].get<float>();
            raw.push_back(c);
        }
    } catch (...) { return; }

    fs::path base = fs::path(state.words_json_path).parent_path() /
                    fs::path(state.audio_path).stem();

    auto write_srt = [](const std::vector<Clip>& clips, const std::string& path) {
        auto ts = [](float s) -> std::string {
            int h=int(s/3600), m=int(s/60)%60, sc=int(s)%60, ms=int(fmodf(s,1.f)*1000);
            char buf[20]; snprintf(buf,sizeof(buf),"%02d:%02d:%02d,%03d",h,m,sc,ms);
            return buf;
        };
        std::ofstream o(path);
        int idx = 1;
        for (auto& c : clips)
            o << idx++ << "\n" << ts(c.start) << " --> " << ts(c.end) << "\n"
              << c.text << "\n\n";
    };

    struct { SubtitleMode mode; const char* suffix; } variants[] = {
        {SubtitleMode::Word,    "_word"},
        {SubtitleMode::Phrase,  "_phrase"},
        {SubtitleMode::Line,    "_line"},
        {SubtitleMode::CustomN, "_custom"},
    };
    for (auto& v : variants) {
        auto clips = group_words(raw, v.mode, state.subtitle_n);
        write_srt(clips, base.string() + v.suffix + ".srt");
    }

    // Segment SRT from segments JSON
    if (!state.segments_json_path.empty() && fs::exists(state.segments_json_path)) {
        std::ifstream sf(state.segments_json_path);
        try {
            auto j = nlohmann::json::parse(sf);
            std::vector<Clip> segs;
            for (auto& seg : j) {
                Clip c;
                c.text  = seg["text"].get<std::string>();
                c.start = seg["start"].get<float>();
                c.end   = seg["end"].get<float>();
                segs.push_back(c);
            }
            write_srt(segs, base.string() + "_segment.srt");
        } catch (...) {}
    }

    // Set the primary SRT path to the current mode's file
    const char* suffix = "_word";
    if (state.subtitle_mode == SubtitleMode::Phrase)   suffix = "_phrase";
    if (state.subtitle_mode == SubtitleMode::Line)     suffix = "_line";
    if (state.subtitle_mode == SubtitleMode::Segment)  suffix = "_segment";
    if (state.subtitle_mode == SubtitleMode::CustomN)  suffix = "_custom";
    state.out_srt = base.string() + std::string(suffix) + ".srt";
}

// ── Import a file onto the timeline (no pipeline) ─────────────────────────────

void import_file(AppState& state, const std::string& path) {
    fs::path fp(path);
    std::string ext = fp.extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);

    bool is_video = (ext==".mp4"||ext==".mov"||ext==".mkv"||ext==".avi"||ext==".webm");

    if (is_video) {
        state.video_path   = path;
        state.video_loaded = true;

        // Probe duration without touching the audio engine.
        // audio_load() calls audio_shutdown() internally which stops the device
        // and leaves it uninitialized if the video has no audio stream — killing
        // any music track that was already playing.  Use dedicated probe helpers
        // instead and just ensure the device is alive.
        {
            float vprobed = video_probe_duration(path);
            if (vprobed > 0.f) {
                state.duration = vprobed;
            } else {
                AudioMeta meta{};
                if (audio_probe(path, meta) && meta.duration_secs > 0.f)
                    state.duration = meta.duration_secs;
            }
        }
        audio_init();              // ensure device is alive (no-op if already running)
        audio_source_ensure(path); // decode video audio into per-clip buffer

        // Reuse an empty track when one exists; new track at top otherwise.
        int vti = find_empty_track(state);
        if (vti < 0) {
            state.tracks.insert(state.tracks.begin(), Track{});
            vti = 0;
            char vname[32];
            snprintf(vname, sizeof(vname), "Track %d", (int)state.tracks.size());
            state.tracks[vti].name = vname;
        }
        Track& vt = state.tracks[vti];
        Clip vc; vc.clip_type = ClipType::Video;
        vc.source_id = path;
        vc.start=0.f; vc.end=state.duration; vc.text=path;
        vt.clips.push_back(vc);
        state.tl_zoom_to_fit_end = vc.end;

        // Claim or reuse proxy slot for this clip instance.
        int slot = slot_for_video(state, clip_slot_key(path, 0.f), path);
        state.proxy_ready = false;

        // Start proxy generation (no-op if proxy already exists on disk).
        proxy_start(path);

        if (slot >= 0) {
            std::string still = proxy_still_path(path);
            video_open_still(slot, still);

            if (proxy_is_ready(path)) {
                ProxyInfo pi;
                if (proxy_load(path, pi)) {
                    video_open_proxy(slot, pi);
                    state.proxy_ready = true;
                }
            }
        }
    } else {
        state.audio_path = path;
        // Probe duration synchronously first (same approach as add_clip_to_track).
        // audio_load() is async — calling audio_duration() immediately after returns
        // 0 or a stale value from the previously loaded file, which would set the
        // clip's end to 4 s and break zoom-to-fit for long audio tracks.
        {
            AudioMeta meta{};
            if (audio_probe(path, meta) && meta.duration_secs > 0.f)
                state.duration = meta.duration_secs;
            else {
                // Fallback: start async load and wait for a duration reading.
                // audio_load probes the header quickly even though decode is async.
                audio_load(path);
                state.duration = audio_duration();
            }
        }
        audio_load(path);          // async — starts device and begins decode
        audio_source_ensure(path); // also load into per-clip buffer for clip playback

        // Add Audio track
        Track* at = nullptr;
        // Reuse a track that already holds this exact audio file
        for (auto& t : state.tracks)
            if (!t.clips.empty() && t.clips[0].clip_type == ClipType::Audio &&
                t.clips[0].source_id == path) { at=&t; break; }
        if (!at) {
            int ati = find_empty_track(state);  // reuse before creating
            if (ati < 0) {
                state.tracks.insert(state.tracks.begin(), Track{});
                ati = 0;
                char aname[32];
                snprintf(aname, sizeof(aname), "Track %d", (int)state.tracks.size());
                state.tracks[ati].name = aname;
            }
            at = &state.tracks[ati];
        }
        at->clips.clear();
        Clip ac; ac.clip_type = ClipType::Audio;
        ac.source_id = path;
        ac.start=0.f; ac.end=(state.duration > 0.f ? state.duration : 4.f); ac.text=path;
        at->clips.push_back(ac);
        state.tl_zoom_to_fit_end = ac.end;
    }

    // Pre-fill output paths in case user already has JSON from a previous run
    fs::path outdir = fp.parent_path() / fp.stem();
    std::string words_candidate = (outdir / (fp.stem().string() + "_words.json")).string();
    std::string segs_candidate  = (outdir / (fp.stem().string() + "_segments.json")).string();
    if (fs::exists(words_candidate)) {
        state.words_json_path    = words_candidate;
        state.segments_json_path = (outdir / (fp.stem().string() + "_segments.json")).string();
        state.vocals_path        = (outdir / "vocals.wav").string();
        state.out_wav            = state.vocals_path;
        load_words_cache(state);
        generate_typography(state);
    }
    {
        std::string src = (outdir / "vocals.wav").string();
        if (!fs::exists(src)) src = path;
        std::string ec = (fs::path(src).parent_path() / "envelope.json").string();
        if (fs::exists(ec)) { state.envelope_json_path = ec; load_envelope_cache(state); }
    }

    history_push(state, "Import \"" + fp.filename().string() + "\"");
}

// ── Kick ML pipeline on a specific clip ──────────────────────────────────────

void kick_pipeline(AppState& state, const std::string& path, PipelineMode mode) {
    if (transcribe_running()) return;

    state.audio_path = path;
    state.pipeline   = PipelineStatus{};

    fs::path audio(path);
    fs::path outdir = audio.parent_path() / audio.stem();
    std::string words_json = (outdir / (audio.stem().string() + "_words.json")).string();
    std::string vocals_out = (outdir / "vocals.wav").string();
    state.words_json_path    = words_json;
    state.segments_json_path = (outdir / (audio.stem().string() + "_segments.json")).string();
    state.vocals_path        = vocals_out;
    state.out_wav            = vocals_out;

    state.last_pipeline_mode = mode;

    // Get MJPEG proxy FPS so forced alignment can snap timestamps to frame boundaries
    double proxy_fps = 0.0;
    if (!state.video_path.empty()) {
        ProxyInfo pi;
        if (proxy_load(state.video_path, pi)) proxy_fps = pi.fps;
    }

    // Find the clip this pipeline was triggered on to get its in_point and
    // timeline duration.  We only process the portion of the source the clip
    // brick actually uses — separation and transcription are both scoped to
    // [in_point, in_point + (clip.end - clip.start)].
    float clip_in  = 0.f;
    float clip_dur = 0.f;
    for (auto& t : state.tracks) {
        for (auto& c : t.clips) {
            if ((c.clip_type == ClipType::Audio || c.clip_type == ClipType::Video) &&
                c.source_id == path) {
                clip_in  = c.in_point;
                clip_dur = c.end - c.start;
                break;
            }
        }
        if (clip_dur > 0.f) break;
    }

    transcribe_start(path, state.pipeline, state.words_json_path, state.vocals_path,
                     mode, proxy_fps, clip_in, clip_dur);
}

// ── Pipeline inline strip ─────────────────────────────────────────────────────

void draw_pipeline_strip(AppState& state, float w) {
    if (state.pipeline.stage == PipelineStage::Idle ||
        state.pipeline.stage == PipelineStage::Done ||
        state.pipeline.stage == PipelineStage::Error) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float  h = 32.f;

    dl->AddRectFilled(p, {p.x + w, p.y + h}, to_u32(Col::bg_soft));
    dl->AddLine({p.x, p.y + h}, {p.x + w, p.y + h}, to_u32(Col::line));

    // Progress fill
    dl->AddRectFilled(p, {p.x + w * state.pipeline.progress, p.y + h},
        IM_COL32(255,255,255,18));

    // Status dot
    float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 4.f);
    dl->AddCircleFilled({p.x + 14.f, p.y + h * 0.5f}, 4.f,
        ImGui::ColorConvertFloat4ToU32({1.f, 1.f, 1.f, pulse}));

    // Text — status + raw debug line
    std::string msg = state.pipeline.message.empty() ?
        "Processing…" : state.pipeline.message;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s  %d%%", msg.c_str(),
        (int)(state.pipeline.progress * 100.f));
    dl->AddText({p.x + 26.f, p.y + 3.f}, to_u32(Col::muted), buf);
    if (!state.pipeline.raw_line.empty()) {
        // truncate long lines for display
        std::string raw = state.pipeline.raw_line;
        if (raw.size() > 120) raw = raw.substr(0, 117) + "...";
        dl->AddText(ImGui::GetFont(), 10.f, {p.x + 26.f, p.y + 15.f},
            to_u32(Col::dim), raw.c_str());
    }

    // Cancel button (draw as text, handle click)
    const char* cancel_lbl = "Cancel";
    float cx = p.x + w - ImGui::CalcTextSize(cancel_lbl).x - 16.f;
    ImVec2 mp = ImGui::GetIO().MousePos;
    bool hov = mp.x >= cx && mp.y >= p.y && mp.y < p.y + h;
    dl->AddText({cx, p.y + 3.f},
        to_u32(hov ? Col::fg : Col::muted), cancel_lbl);
    if (hov && ImGui::IsMouseClicked(0)) transcribe_cancel();

    ImGui::Dummy({w, h});
}

void draw_vision_download_strip(float w) {
    if (!vision_download_running()) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float  h = 32.f;

    dl->AddRectFilled(p, {p.x + w, p.y + h}, to_u32(Col::bg_soft));
    dl->AddLine({p.x, p.y + h}, {p.x + w, p.y + h}, to_u32(Col::line));

    float prog = vision_download_progress();
    dl->AddRectFilled(p, {p.x + w * prog, p.y + h}, IM_COL32(255,255,255,18));

    float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 4.f);
    dl->AddCircleFilled({p.x + 14.f, p.y + h * 0.5f}, 4.f,
        ImGui::ColorConvertFloat4ToU32({1.f, 1.f, 1.f, pulse}));

    std::string msg = vision_download_message();
    if (msg.empty()) msg = "Downloading vision model…";
    char buf[256];
    snprintf(buf, sizeof(buf), "Vision Model  •  %s  %d%%",
             msg.c_str(), (int)(prog * 100.f));
    dl->AddText({p.x + 26.f, p.y + 3.f}, to_u32(Col::muted), buf);

    std::string err = vision_download_error();
    if (!err.empty()) {
        std::string e = err.size() > 80 ? err.substr(0, 77) + "..." : err;
        dl->AddText(ImGui::GetFont(), 10.f, {p.x + 26.f, p.y + 15.f},
                    to_u32(Col::dim), e.c_str());
    }

    const char* cancel_lbl = "Cancel";
    float cx = p.x + w - ImGui::CalcTextSize(cancel_lbl).x - 16.f;
    ImVec2 mp = ImGui::GetIO().MousePos;
    bool hov = mp.x >= cx && mp.y >= p.y && mp.y < p.y + h;
    dl->AddText({cx, p.y + 3.f}, to_u32(hov ? Col::fg : Col::muted), cancel_lbl);
    if (hov && ImGui::IsMouseClicked(0)) vision_download_cancel();

    ImGui::Dummy({w, h});
}

// ── Find-and-add-clip search strip ────────────────────────────────────────────
//
// Agent-driven windowed transcript searches (find_and_add_clip /
// search_transcript) used to run invisibly — the human only saw a clip
// appear (or not) at the end.  This mirrors draw_pipeline_strip so a search
// in progress shows up in the same status bar with its current chunk range,
// progress, and a cancel button.  See feedback_agent_tools_visible_ui.
void draw_search_strip(float w) {
    if (!transcribe_search_running()) return;

    SearchStatus s = transcribe_search_status();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float  h = 32.f;

    dl->AddRectFilled(p, {p.x + w, p.y + h}, to_u32(Col::bg_soft));
    dl->AddLine({p.x, p.y + h}, {p.x + w, p.y + h}, to_u32(Col::line));

    dl->AddRectFilled(p, {p.x + w * s.progress, p.y + h},
        IM_COL32(255,255,255,18));

    float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 4.f);
    dl->AddCircleFilled({p.x + 14.f, p.y + h * 0.5f}, 4.f,
        ImGui::ColorConvertFloat4ToU32({1.f, 1.f, 1.f, pulse}));

    std::string msg = s.message.empty() ? "Searching transcript…" : s.message;
    char buf[256];
    snprintf(buf, sizeof(buf), "Search  •  %s  %d%%",
        msg.c_str(), (int)(s.progress * 100.f));
    dl->AddText({p.x + 26.f, p.y + 3.f}, to_u32(Col::muted), buf);

    if (s.total_sec > 0.f) {
        char range[64];
        snprintf(range, sizeof(range), "%.0fs / %.0fs scanned",
            s.current_sec, s.total_sec);
        dl->AddText(ImGui::GetFont(), 10.f, {p.x + 26.f, p.y + 15.f},
            to_u32(Col::dim), range);
    }

    const char* cancel_lbl = "Cancel";
    float cx = p.x + w - ImGui::CalcTextSize(cancel_lbl).x - 16.f;
    ImVec2 mp = ImGui::GetIO().MousePos;
    bool hov = mp.x >= cx && mp.y >= p.y && mp.y < p.y + h;
    dl->AddText({cx, p.y + 3.f},
        to_u32(hov ? Col::fg : Col::muted), cancel_lbl);
    if (hov && ImGui::IsMouseClicked(0)) transcribe_cancel();

    ImGui::Dummy({w, h});
}
