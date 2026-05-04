// screen_studio.cpp — the entire Pop Maker Studio workspace
#include "screens.h"
#include "theme.h"
#include "app.h"
#include "audio.h"
#include "video.h"
#include "transcribe.h"
#include "filepicker.h"
#include "globals.h"
#include "render.h"
#include "blender_export.h"
#include "history.h"
#include "proxy.h"
#include "waveform.h"
#include "project.h"
#include "fx_shader.h"
#include <imgui.h>
#include <imgui_internal.h>
#include "json.hpp"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

namespace fs = std::filesystem;

extern ImFont* g_font_bold;
extern ImFont* g_font_black;

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string fmt_time(float s) {
    int m  = (int)(s / 60);
    int sc = (int)s % 60;
    int cs = (int)((s - floorf(s)) * 100.f);
    char buf[20]; snprintf(buf, sizeof(buf), "%d:%02d.%02d", m, sc, cs);
    return buf;
}

static std::string fmt_time_short(float s) {
    int m  = (int)(s / 60);
    int sc = (int)s % 60;
    char buf[12]; snprintf(buf, sizeof(buf), "%d:%02d", m, sc);
    return buf;
}

// Re-anchor wall-clock playback whenever the playhead is moved.
// Delays the wall clock by audio_latency() so that the video frame and the
// audio sample at position t both become visible/audible at the same moment.
static double s_scrub_until = 0.0;

static void seek_to(AppState& state, float t) {
    t = roundf(t * 30.f) / 30.f;
    state.playhead = t;
    audio_seek(t);
    if (state.playing) {
        state.play_start_pos  = t;
        state.play_start_wall = std::chrono::steady_clock::now();
    } else {
        // Brief audio blip so user hears where the playhead landed.
        audio_play();
        s_scrub_until = ImGui::GetTime() + 0.08;
    }
}

static void toggle_play(AppState& state) {
    state.playing = !state.playing;
    if (state.playing) {
        state.play_start_pos  = state.playhead;
        state.play_start_wall = std::chrono::steady_clock::now();
        audio_seek(state.playhead);
        audio_play();
    } else {
        audio_pause();
    }
}

static float tl_fps(const AppState& state) {
    return (state.proxy_ready && video_info(0).fps > 0.0)
           ? (float)video_info(0).fps : (float)state.fps;
}

static int slot_for_video(AppState& state, const std::string& key, const std::string& src); // forward decl

// Composite slot key: source_path + '\x01' + clip start time.
// Two clips with the same source but different start times get independent decoders.
static std::string clip_slot_key(const std::string& src, float start) {
    char buf[32]; snprintf(buf, sizeof(buf), "\x01%.6f", start);
    return src + buf;
}
// Extract source path from composite key (or return key as-is for legacy callers).
static std::string source_from_key(const std::string& key) {
    auto pos = key.find('\x01');
    return pos == std::string::npos ? key : key.substr(0, pos);
}

// Track index the mouse is hovering over in the timeline — updated by draw_timeline
// each frame so the drop handler can target a specific lane.
static int s_tl_hover_track = -1;

// Drop flash — highlight the target track row for ~0.5 s after a file lands.
static float s_drop_flash_t     = 0.f;  // countdown in seconds
static int   s_drop_flash_track = -1;   // track index that received the drop (-1 = new track)


// Add a clip of the given type to an existing track, probing duration as needed.
static void add_clip_to_track(AppState& state, int ti, const std::string& path, ClipType ct) {
    if (ti < 0 || ti >= (int)state.tracks.size()) return;
    Track& tr = state.tracks[ti];

    Clip cl;
    cl.clip_type = ct;
    cl.start     = state.playhead;
    cl.text      = path;
    cl.source_id = path;

    if (ct == ClipType::Video) {
        float dur = video_probe_duration(path);
        if (dur <= 0.f) dur = 4.f;
        cl.end = cl.start + dur;
        int slot = slot_for_video(state, clip_slot_key(path, cl.start), path);
        proxy_start(path);
        if (slot >= 0) {
            video_open_still(slot, proxy_still_path(path));
            if (proxy_is_ready(path)) {
                ProxyInfo pi;
                if (proxy_load(path, pi)) video_open_proxy(slot, pi);
            }
        }
        state.video_loaded = true;
    } else if (ct == ClipType::Audio) {
        AudioMeta meta;
        float dur = audio_probe(path, meta) ? meta.duration_secs : 0.f;
        if (dur <= 0.f) dur = 4.f;
        cl.end = cl.start + dur;
        audio_source_ensure(path);
    } else {
        cl.end = cl.start + 2.f;  // blank text clip — 2 s default
    }

    tr.clips.push_back(cl);
    std::sort(tr.clips.begin(), tr.clips.end(),
              [](const Clip& a, const Clip& b){ return a.start < b.start; });

    // Select the newly added clip.
    state.selected_track = ti;
    for (int ci = 0; ci < (int)tr.clips.size(); ++ci)
        if (&tr.clips[ci] == &tr.clips.back() ||
            (fabsf(tr.clips[ci].start - cl.start) < 0.01f &&
             tr.clips[ci].clip_type == ct))
            { state.selected_clip = ci; break; }
    state.panel_tab = 0;
    history_push(state, std::string("Add ") +
                        (ct==ClipType::Video ? "video" :
                         ct==ClipType::Audio ? "audio" : "text") + " clip");
}

// Proxy slot keyed by composite key (source_path + clip start).
// Each clip instance gets its own decoder so same-source clips don't fight.
// Returns -1 when all slots full.
static int slot_for_video(AppState& state, const std::string& key, const std::string& /*src*/) {
    if (key.empty()) return -1;
    for (int i = 0; i < MAX_VIDEO_TRACKS; ++i)
        if (state.proxy_paths[i] == key) return i;
    for (int i = 0; i < MAX_VIDEO_TRACKS; ++i)
        if (state.proxy_paths[i].empty()) { state.proxy_paths[i] = key; return i; }
    return -1;
}

// Release slots whose clip no longer exists on any track.
static void gc_video_slots(AppState& state) {
    std::set<std::string> live;
    for (auto& tr : state.tracks)
        for (auto& cl : tr.clips)
            if (cl.clip_type == ClipType::Video && !cl.text.empty())
                live.insert(clip_slot_key(cl.text, cl.start));
    for (int i = 0; i < MAX_VIDEO_TRACKS; ++i) {
        if (!state.proxy_paths[i].empty() && !live.count(state.proxy_paths[i])) {
            video_close(i);
            state.proxy_paths[i].clear();
        }
    }
}

// Open stills/proxies for all video clips after project load.
static void reopen_video_slots(AppState& state) {
    for (auto& tr : state.tracks) {
        for (auto& cl : tr.clips) {
            if (cl.clip_type != ClipType::Video || cl.text.empty()) continue;
            std::string key = clip_slot_key(cl.text, cl.start);
            int slot = slot_for_video(state, key, cl.text);
            if (slot < 0) continue;
            proxy_start(cl.text);
            video_open_still(slot, proxy_still_path(cl.text));
            if (proxy_is_ready(cl.text)) {
                ProxyInfo pi;
                if (proxy_load(cl.text, pi)) {
                    video_open_proxy(slot, pi);
                    if (slot == 0) state.proxy_ready = true;
                }
            }
        }
    }
}

// Maximum timeline position covered by any clip across all tracks.
static float project_end(const AppState& state) {
    float end = 0.f;
    for (auto& tr : state.tracks)
        for (auto& cl : tr.clips)
            if (cl.end > end) end = cl.end;
    return fmaxf(end, 0.01f);
}

static bool is_audio_file(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    return ext==".wav"||ext==".mp3"||ext==".m4a"||
           ext==".flac"||ext==".mp4"||ext==".mov"||ext==".aac";
}

// Parse an SRT file into a list of Text clips.
static std::vector<Clip> parse_srt(const std::string& path) {
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
static std::vector<Clip> group_words(
    const std::vector<Clip>& words,
    SubtitleMode mode, int custom_n)
{
    if (words.empty()) return {};
    std::vector<Clip> out;

    switch (mode) {
    case SubtitleMode::Word:
        return words;

    case SubtitleMode::Phrase: {
        // Split when gap between words exceeds 0.3 s
        Clip cur = words[0];
        for (size_t i = 1; i < words.size(); ++i) {
            float gap = words[i].start - words[i-1].end;
            if (gap > 0.3f) {
                out.push_back(cur);
                cur = words[i];
            } else {
                cur.text += " " + words[i].text;
                cur.end   = words[i].end;
            }
        }
        out.push_back(cur);
        break;
    }

    case SubtitleMode::Line: {
        // Split when gap exceeds 0.8 s (breath / phrase boundary)
        Clip cur = words[0];
        for (size_t i = 1; i < words.size(); ++i) {
            float gap = words[i].start - words[i-1].end;
            if (gap > 0.8f) {
                out.push_back(cur);
                cur = words[i];
            } else {
                cur.text += " " + words[i].text;
                cur.end   = words[i].end;
            }
        }
        out.push_back(cur);
        break;
    }

    case SubtitleMode::CustomN: {
        int n = (custom_n < 1) ? 1 : custom_n;
        for (size_t i = 0; i < words.size(); ) {
            Clip c = words[i++];
            for (int k = 1; k < n && i < words.size(); ++k, ++i) {
                c.text += " " + words[i].text;
                c.end   = words[i].end;
            }
            out.push_back(c);
        }
        break;
    }

    case SubtitleMode::Karaoke: {
        // Same grouping as Line (split on breath gaps > 0.8 s) but each clip
        // carries karaoke=true so preview and render do per-word highlighting.
        auto lines = group_words(words, SubtitleMode::Line, custom_n);
        for (auto& c : lines) c.karaoke = true;
        return lines;
    }

    case SubtitleMode::Segment:
        // Handled separately via segments JSON — fall back to Line grouping
        // if segment data isn't available.
        return group_words(words, SubtitleMode::Line, custom_n);
    }

    return out;
}

// Load the flat word list from words_json_path into AppState::words_cache.
// ── Beat detection subprocess ─────────────────────────────────────────────────

static void load_beats_cache(AppState& state) {
    state.beats.clear();
    state.beat_bpm = 0.f;
    if (state.beats_json_path.empty() || !fs::exists(state.beats_json_path)) return;
    std::ifstream f(state.beats_json_path);
    if (!f) return;
    try {
        auto j = nlohmann::json::parse(f);
        state.beat_bpm = j.value("bpm", 0.f);
        for (auto& v : j["beats"])
            state.beats.push_back(v.get<float>());
    } catch (...) {}
}

static void run_beat_detect(AppState& state) {
    if (state.beats_running) return;
    std::string src = state.vocals_path.empty() ? state.audio_path : state.vocals_path;
    if (src.empty() || !fs::exists(src)) return;
    extern std::string g_beat_detect_script;
    if (g_beat_detect_script.empty()) return;

    fs::path out = fs::path(src).parent_path() / "beats.json";
    state.beats_json_path = out.string();
    state.beats_running   = true;

    std::string python  = state.python_path;
    std::string script  = g_beat_detect_script;
    std::string outpath = out.string();

    std::thread([&state, python, script, src, outpath]() {
        std::string cmd = "\"" + python + "\" \"" + script + "\" \"" + src + "\" \"" + outpath + "\" 2>&1";
        FILE* p = popen(cmd.c_str(), "r");
        char buf[256];
        while (p && fgets(buf, sizeof(buf), p)) {}
        if (p) pclose(p);
        state.beats_running = false;
        load_beats_cache(state);
    }).detach();
}

// ── Envelope extraction subprocess ───────────────────────────────────────────

static void load_envelope_cache(AppState& state) {
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

static void run_envelope_extract(AppState& state) {
    if (state.envelope_running) return;
    std::string src = state.vocals_path.empty() ? state.audio_path : state.vocals_path;
    if (src.empty() || !fs::exists(src)) return;
    extern std::string g_envelope_script;
    if (g_envelope_script.empty()) return;

    fs::path out = fs::path(src).parent_path() / "envelope.json";
    state.envelope_json_path = out.string();
    state.envelope_running   = true;

    std::string python  = state.python_path;
    std::string script  = g_envelope_script;
    std::string outpath = out.string();

    std::thread([&state, python, script, src, outpath]() {
        std::string cmd = "\"" + python + "\" \"" + script + "\" \"" + src + "\" \"" + outpath + "\" 2>&1";
        FILE* p = popen(cmd.c_str(), "r");
        char buf[256];
        while (p && fgets(buf, sizeof(buf), p)) {}
        if (p) pclose(p);
        state.envelope_running = false;
        load_envelope_cache(state);
    }).detach();
}

// ─────────────────────────────────────────────────────────────────────────────

static void load_words_cache(AppState& state) {
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

// Load word JSON and apply current grouping mode.
// Removes all Lyrics clips with matching source_id from ALL tracks, then
// places fresh grouped clips on the "Lyrics" track.
static void apply_subtitle_mode(AppState& state) {
    if (state.words_json_path.empty()) return;
    const std::string src = state.audio_path;

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
            lyrics->clips.clear();
            for (auto& seg : j) {
                Clip c;
                c.text  = seg["text"].get<std::string>();
                c.start = seg["start"].get<float>();
                c.end   = seg["end"].get<float>();
                stamp(c);
                lyrics->clips.push_back(c);
            }
        } catch (...) {}
        return;
    }

    // Word-level JSON → group
    std::ifstream f(state.words_json_path);
    if (!f) return;
    try {
        auto j = nlohmann::json::parse(f);
        std::vector<Clip> raw;
        for (auto& w : j) {
            Clip c;
            c.text  = w["word"].get<std::string>();
            c.start = w["start"].get<float>();
            c.end   = w["end"].get<float>();
            raw.push_back(c);
        }
        auto grouped = group_words(raw, state.subtitle_mode, state.subtitle_n);
        for (auto& c : grouped) stamp(c);

        // Populate per-clip word lists from the raw word entries
        std::vector<WordEntry> all_words;
        for (auto& w : j) {
            WordEntry we;
            we.text  = w["word"].get<std::string>();
            we.start = w["start"].get<float>();
            we.end   = w["end"].get<float>();
            all_words.push_back(we);
        }
        for (auto& c : grouped) {
            c.words.clear();
            for (auto& we : all_words)
                if (we.start >= c.start - 0.001f && we.end <= c.end + 0.001f)
                    c.words.push_back(we);
        }

        Track* lyrics = nullptr;
        for (auto& t : state.tracks)
            if (t.name == "Lyrics") { lyrics = &t; break; }
        if (!lyrics) {
            state.tracks.insert(state.tracks.begin(), Track{});
            lyrics = &state.tracks.front();
            lyrics->name = "Lyrics";
        }
        lyrics->clips = grouped;
    } catch (...) {}
}

// Load segments JSON and populate a "Subtitles" track with ClipType::Subtitle clips.
static void apply_subtitle_pipeline(AppState& state) {
    if (state.segments_json_path.empty()) return;
    std::ifstream f(state.segments_json_path);
    if (!f) return;
    const std::string src = state.audio_path;
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
        for (auto& seg : j) {
            Clip c;
            c.clip_type = ClipType::Subtitle;
            c.source_id = src;
            c.sub_pos   = 3;      // custom Y — slightly below center
            c.sub_pos_y = 0.62f;
            c.text  = seg["text"].get<std::string>();
            c.start = seg["start"].get<float>();
            c.end   = seg["end"].get<float>();
            tr->clips.push_back(c);
        }
    } catch (...) {}
}

// Save all four SRT variants to the output directory.
static void save_all_srts(AppState& state) {
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

static void import_file(AppState& state, const std::string& path) {
    fs::path fp(path);
    std::string ext = fp.extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);

    bool is_video = (ext==".mp4"||ext==".mov"||ext==".mkv"||ext==".avi"||ext==".webm");

    if (is_video) {
        state.video_path   = path;
        state.video_loaded = true;

        // audio_load probes duration from the container header synchronously
        // before spawning its background decode thread — use that as the
        // primary duration source since it works on all formats.
        audio_load(path);
        state.duration = audio_duration();
        // video_probe_duration is a faster path that works when the container
        // header carries duration; use it only as an override if it succeeds.
        float vprobed = video_probe_duration(path);
        if (vprobed > 0.f) state.duration = vprobed;

        state.tracks.insert(state.tracks.begin(), Track{});
        Track& vt = state.tracks.front();
        char vname[32];
        snprintf(vname, sizeof(vname), "Track %d", (int)state.tracks.size());
        vt.name = vname;
        Clip vc; vc.clip_type = ClipType::Video;
        vc.source_id = path;
        vc.start=0.f; vc.end=state.duration; vc.text=path;
        vt.clips.push_back(vc);

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
        audio_load(path);  // async — also probes container duration
        state.duration = audio_duration();
        if (state.duration <= 0.f) {
            AudioMeta meta{};
            if (audio_probe(path, meta) && meta.duration_secs > 0.f)
                state.duration = meta.duration_secs;
        }

        // Add Audio track
        Track* at = nullptr;
        // Reuse a track that already holds this exact audio file
        for (auto& t : state.tracks)
            if (!t.clips.empty() && t.clips[0].clip_type == ClipType::Audio &&
                t.clips[0].source_id == path) { at=&t; break; }
        if (!at) {
            state.tracks.insert(state.tracks.begin(), Track{});
            at = &state.tracks.front();
            char aname[32];
            snprintf(aname, sizeof(aname), "Track %d", (int)state.tracks.size());
            at->name = aname;
        }
        at->clips.clear();
        Clip ac; ac.clip_type = ClipType::Audio;
        ac.source_id = path;
        ac.start=0.f; ac.end=(state.duration > 0.f ? state.duration : 4.f); ac.text=path;
        at->clips.push_back(ac);
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
        apply_subtitle_mode(state);
    }
    {
        std::string src = (outdir / "vocals.wav").string();
        if (!fs::exists(src)) src = path;
        std::string bc = (fs::path(src).parent_path() / "beats.json").string();
        if (fs::exists(bc)) { state.beats_json_path = bc; load_beats_cache(state); }
        std::string ec = (fs::path(src).parent_path() / "envelope.json").string();
        if (fs::exists(ec)) { state.envelope_json_path = ec; load_envelope_cache(state); }
    }

    history_push(state, "Import \"" + fp.filename().string() + "\"");
}

// ── Kick ML pipeline on a specific clip ──────────────────────────────────────

static void kick_pipeline(AppState& state, const std::string& path, PipelineMode mode) {
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

    state.pipeline_produces_subtitles = (mode == PipelineMode::TranscribeOnly);

    if (mode == PipelineMode::Both) {
        bool has = false;
        for (auto& t : state.tracks) if (t.name=="Lyrics") { has=true; break; }
        if (!has) {
            Track ph; ph.name="Lyrics";
            state.tracks.insert(state.tracks.begin(), std::move(ph));
        }
    } else if (mode == PipelineMode::TranscribeOnly) {
        bool has = false;
        for (auto& t : state.tracks) if (t.name=="Subtitles") { has=true; break; }
        if (!has) {
            Track ph; ph.name="Subtitles";
            state.tracks.insert(state.tracks.begin(), std::move(ph));
        }
    }

    extern std::string g_pipeline_script;
    transcribe_start(path, state.python_path, g_pipeline_script,
                     state.pipeline, state.words_json_path, state.vocals_path, mode);
}

// ── Pipeline inline strip ─────────────────────────────────────────────────────

static void draw_pipeline_strip(AppState& state, float w) {
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

// ── Preview ───────────────────────────────────────────────────────────────────

static void draw_preview(AppState& state, ImVec2 p, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Black base
    dl->AddRectFilled(p, {p.x+w, p.y+h},
        state.video_loaded ? IM_COL32(0,0,0,255) : to_u32(Col::accent_dark), 2.f);

    // Empty state prompt
    if (state.tracks.empty()) {
        ImGui::PushFont(g_font_bold);
        ImGui::SetWindowFontScale(1.3f);
        const char* hint = "Drop a file to start";
        ImVec2 hsz = ImGui::CalcTextSize(hint);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.3f,
            {p.x + (w - hsz.x) * 0.5f, p.y + h * 0.5f - hsz.y},
            to_u32(Col::muted), hint);
        ImGui::SetWindowFontScale(1.f);
        ImGui::PopFont();

        const char* sub = "or  File → Import Audio";
        ImVec2 ssz = ImGui::CalcTextSize(sub);
        dl->AddText({p.x + (w - ssz.x) * 0.5f, p.y + h * 0.5f + 8.f},
            to_u32(Col::dim), sub);

        dl->AddRect(p, {p.x+w, p.y+h}, to_u32(Col::line), 2.f);
        return;
    }

    // Unified z-order pass: track index 0 = frontmost, so iterate high→low (background first).
    // Each track draws whichever clip type is active — video and text are interleaved correctly.
    static int   s_drag_ti   = -1, s_drag_ci   = -1;
    static bool  s_dragging  = false;
    ImVec2 mpos  = ImGui::GetIO().MousePos;
    bool   ldown  = ImGui::IsMouseDown(0);
    bool   lclick = ImGui::IsMouseClicked(0);

    float lookahead = ImGui::GetIO().DeltaTime;
    int text_rendered = 0;

    for (int ti = (int)state.tracks.size() - 1; ti >= 0; --ti) {
        auto& track = state.tracks[ti];
        if (!track.visible) continue;

        // ── Video clip ─────────────────────────────────────────────────────────
        {
            // Helper: draw a video clip at a given playhead time with alpha multiplier.
            auto draw_vid_clip = [&](const Clip* cl_ptr, float at_time, float alpha_mul) {
                if (!cl_ptr) return;
                int slot = slot_for_video(const_cast<AppState&>(state),
                               clip_slot_key(cl_ptr->text, cl_ptr->start), cl_ptr->text);
                // Map timeline time → source file time via clip's in_point and speed.
                float src_t = cl_ptr->in_point + (at_time - cl_ptr->start) * cl_ptr->speed;
                uintptr_t tex = (slot >= 0 && video_is_open(slot))
                    ? video_get_texture(slot, (double)(src_t + lookahead)) : 0;
                if (!tex) return;
                float px    = cl_ptr->eval_prop("pos_x",    at_time);
                float py    = cl_ptr->eval_prop("pos_y",    at_time);
                float sx    = cl_ptr->eval_prop("scale_x",  at_time);
                float sy    = cl_ptr->eval_prop("scale_y",  at_time);
                float rot   = cl_ptr->eval_prop("rotation", at_time);
                float alpha = cl_ptr->eval_prop("opacity",  at_time) * alpha_mul;
                VideoInfo vi = video_info(slot);
                float fit_w = w, fit_h = h;
                if (vi.width > 0 && vi.height > 0) {
                    float vid_asp = (float)vi.width / (float)vi.height;
                    float can_asp = w / h;
                    if (vid_asp > can_asp) { fit_w = w; fit_h = w / vid_asp; }
                    else                   { fit_h = h; fit_w = h * vid_asp; }
                }
                float cx = p.x + px * w, cy = p.y + py * h;
                float hw = fit_w * sx * 0.5f, hh = fit_h * sy * 0.5f;
                float rad = rot * 3.14159265f / 180.f;
                float cos_r = cosf(rad), sin_r = sinf(rad);
                auto rot_pt = [&](float ox, float oy) -> ImVec2 {
                    return { cx + ox*cos_r - oy*sin_r, cy + ox*sin_r + oy*cos_r };
                };
                EffectAccum vid_fx = collect_effects(state, at_time, ti);
                uintptr_t display_tex = tex;
                if (vid_fx.any_color || vid_fx.any_blur) {
                    display_tex = fx_apply(tex,
                        vid_fx.brightness, vid_fx.contrast,
                        vid_fx.saturation, vid_fx.hue,
                        vid_fx.blur);
                }
                ImU32 col = IM_COL32(255, 255, 255, (int)(std::fmaxf(0.f, std::fminf(1.f, alpha)) * 255.f));
                dl->AddImageQuad(ImTextureRef((ImTextureID)display_tex),
                    rot_pt(-hw,-hh), rot_pt(hw,-hh), rot_pt(hw,hh), rot_pt(-hw,hh),
                    {0,0}, {1,0}, {1,1}, {0,1}, col);
                if (vid_fx.any_vignette && vid_fx.vignette > 0.01f) {
                    float vx0=cx-hw, vy0=cy-hh, vx1=cx+hw, vy1=cy+hh;
                    ImU32 vig = IM_COL32(0,0,0,(int)(vid_fx.vignette*210.f));
                    float vw=vx1-vx0, vh=vy1-vy0;
                    dl->AddRectFilledMultiColor({vx0,vy0},{vx0+vw*0.4f,vy1},vig,IM_COL32(0,0,0,0),IM_COL32(0,0,0,0),vig);
                    dl->AddRectFilledMultiColor({vx0+vw*0.6f,vy0},{vx1,vy1},IM_COL32(0,0,0,0),vig,vig,IM_COL32(0,0,0,0));
                    dl->AddRectFilledMultiColor({vx0,vy0},{vx1,vy0+vh*0.3f},vig,vig,IM_COL32(0,0,0,0),IM_COL32(0,0,0,0));
                    dl->AddRectFilledMultiColor({vx0,vy0+vh*0.7f},{vx1,vy1},IM_COL32(0,0,0,0),IM_COL32(0,0,0,0),vig,vig);
                }
            };

            // Find the active video clip and check for transitions
            const Clip* active = nullptr;
            int active_ci = -1;
            for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
                auto& cl = track.clips[ci];
                if (cl.clip_type == ClipType::Video &&
                    state.playhead >= cl.start && state.playhead < cl.end)
                    { active = &cl; active_ci = ci; break; }
            }

            // Check if incoming clip has a transition that started before its start
            // Also show incoming clip B when playhead is inside its transition_post zone
            // (clip B hasn't started in [active] loop above, but needs to be visible)
            if (!active) {
                for (int ci = 1; ci < (int)track.clips.size(); ++ci) {
                    const Clip& prev = track.clips[ci - 1];
                    const Clip& cl   = track.clips[ci];
                    if (prev.clip_type != ClipType::Video || cl.clip_type != ClipType::Video) continue;
                    if (prev.transition_type == TransitionType::None || prev.transition_post <= 0.f) continue;
                    if (state.playhead >= cl.start && state.playhead < cl.start + prev.transition_post)
                        { active = &track.clips[ci]; active_ci = ci; break; }
                }
            }

            if (active) {
                bool in_trans_out = (active->transition_type != TransitionType::None &&
                                     active->transition_pre > 0.f &&
                                     state.playhead >= active->end - active->transition_pre);
                const Clip* next_cl = nullptr;
                if (in_trans_out && active_ci + 1 < (int)track.clips.size()) {
                    const Clip& nc = track.clips[active_ci + 1];
                    if (nc.clip_type == ClipType::Video) next_cl = &nc;
                }

                bool in_trans_in = false;
                const Clip* prev_cl = nullptr;
                if (!in_trans_out && active_ci > 0) {
                    const Clip& pc = track.clips[active_ci - 1];
                    if (pc.clip_type == ClipType::Video &&
                        pc.transition_type != TransitionType::None &&
                        pc.transition_post > 0.f &&
                        state.playhead < active->start + pc.transition_post) {
                        in_trans_in = true;
                        prev_cl = &pc;
                    }
                }

                if (in_trans_out && next_cl) {
                    // t_a: 0→1 over [end-pre .. end], t_b: 0→1 over [end .. end+post]
                    float pre = active->transition_pre, post = active->transition_post;
                    float cut = active->end;
                    float t_a = std::fmaxf(0.f, std::fminf(1.f, (state.playhead - (cut - pre)) / fmaxf(pre, 1e-5f)));
                    float t_b = std::fmaxf(0.f, std::fminf(1.f, (state.playhead - cut) / fmaxf(post, 1e-5f)));

                    if (active->transition_type == TransitionType::Dissolve) {
                        draw_vid_clip(active,  state.playhead, 1.f - t_a);
                        draw_vid_clip(next_cl, state.playhead, t_b > 0.f ? t_b : t_a); // blend once cut passes
                    } else if (active->transition_type == TransitionType::FadeBlack) {
                        draw_vid_clip(active,  state.playhead, 1.f - t_a);
                        draw_vid_clip(next_cl, state.playhead, t_b);
                    } else { // DipWhite
                        draw_vid_clip(active, state.playhead, 1.f - t_a);
                        float white_a = t_a * (1.f - t_b);
                        if (white_a > 0.01f)
                            dl->AddRectFilled(p, {p.x+w, p.y+h}, IM_COL32(255,255,255,(int)(white_a*255.f)));
                        draw_vid_clip(next_cl, state.playhead, t_b);
                    }
                } else if (in_trans_in && prev_cl) {
                    float t = std::fmaxf(0.f, std::fminf(1.f,
                        (state.playhead - active->start) / fmaxf(prev_cl->transition_post, 1e-5f)));
                    if (prev_cl->transition_type == TransitionType::Dissolve) {
                        // Draw clip A's last frame at 1-t so alpha_A + alpha_B = 1 at cut
                        draw_vid_clip(prev_cl, std::fminf(state.playhead, prev_cl->end - 1e-4f), 1.f - t);
                        draw_vid_clip(active,  state.playhead, t);
                    } else if (prev_cl->transition_type == TransitionType::FadeBlack) {
                        // Black at cut is intentional; clip B fades in from 0
                        draw_vid_clip(active, state.playhead, t);
                    } else { // DipWhite: white overlay fades out, clip B fades in
                        float white_a = 1.f - t;
                        if (white_a > 0.01f)
                            dl->AddRectFilled(p, {p.x+w, p.y+h}, IM_COL32(255,255,255,(int)(white_a*255.f)));
                        draw_vid_clip(active, state.playhead, t);
                    }
                } else {
                    draw_vid_clip(active, state.playhead, 1.f);
                }
            }
        }

        // ── Text / subtitle clip ───────────────────────────────────────────────
        {
            const Clip* active = nullptr;
            int active_ci = -1;
            for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
                auto ct = track.clips[ci].clip_type;
                if (ct != ClipType::Text && ct != ClipType::Lyrics && ct != ClipType::Subtitle)
                    continue;
                if (state.playhead >= track.clips[ci].start &&
                    state.playhead <  track.clips[ci].end)
                    { active = &track.clips[ci]; active_ci = ci; break; }
            }
            const Clip* show = active;
            int show_ci = active_ci;
            if (!show && state.selected_track == ti && state.selected_clip >= 0 &&
                state.selected_clip < (int)track.clips.size()) {
                auto ct = track.clips[state.selected_clip].clip_type;
                if (ct == ClipType::Text || ct == ClipType::Lyrics || ct == ClipType::Subtitle) {
                    show    = &track.clips[state.selected_clip];
                    show_ci = state.selected_clip;
                }
            }
            if (!show) { ++text_rendered; continue; }

            float slot_h = 40.f;
            float slot_y;
            if (show->sub_pos == 1)
                slot_y = p.y + h * 0.5f - slot_h * 0.5f;
            else if (show->sub_pos == 2)
                slot_y = p.y + 24.f + text_rendered * slot_h;
            else if (show->sub_pos == 3)
                slot_y = p.y + show->sub_pos_y * h - slot_h * 0.5f;
            else
                slot_y = p.y + h - 24.f - (text_rendered + 1) * slot_h;

            ImGui::PushFont(g_font_black);
            float fsz = ImGui::GetFontSize() * 1.8f;
            ImVec2 tsz = ImGui::CalcTextSize(show->text.c_str());
            float  tx  = p.x + (w - tsz.x) * 0.5f;

            // Kinetic typography: compute per-style animation offsets
            float local_t  = state.playhead - show->start;  // clip-relative time
            float clip_dur = show->end - show->start;
            float fade_in  = fminf(0.25f, clip_dur * 0.3f);
            float fade_out = fminf(0.25f, clip_dur * 0.2f);

            // Accumulated effect from Effect clips on tracks above this one
            EffectAccum text_fx = collect_effects(state, state.playhead, ti);

            float anim_dx    = 0.f;
            float anim_dy    = 0.f;
            float anim_alpha = 1.f;

            AnimStyle eff_style = (show->clip_style != AnimStyle::None)
                                  ? show->clip_style : state.style;

            if (active_ci >= 0) {  // only animate when actively playing
                switch (eff_style) {
                    case AnimStyle::Fade: {
                        if (local_t < fade_in)
                            anim_alpha = local_t / fade_in;
                        else if (local_t > clip_dur - fade_out)
                            anim_alpha = (clip_dur - local_t) / fade_out;
                        break;
                    }
                    case AnimStyle::Glitch: {
                        float decay = fmaxf(0.f, 1.f - local_t / 0.5f);
                        anim_dx = sinf(local_t * 97.f + sinf(local_t * 53.f) * 31.f) * 12.f * decay;
                        break;
                    }
                    case AnimStyle::Typewriter: {
                        if (local_t < fade_in) {
                            anim_alpha = local_t / fade_in;
                            anim_dy    = (fade_in - local_t) / fade_in * (-8.f);
                        }
                        break;
                    }
                    case AnimStyle::Bounce: {
                        float bounce_dur = fminf(0.6f, clip_dur);
                        if (local_t < bounce_dur) {
                            float p2 = local_t / bounce_dur;
                            anim_dy = sinf(p2 * 3.14159f) * (-60.f) * expf(-p2 * 4.f);
                        }
                        break;
                    }
                    case AnimStyle::Slide: {
                        if (local_t < fade_in)
                            anim_dx = (local_t / fade_in - 1.f) * w * 0.6f;
                        else if (local_t > clip_dur - fade_out)
                            anim_dx = ((local_t - (clip_dur - fade_out)) / fade_out) * w * 0.6f;
                        break;
                    }
                    case AnimStyle::Stack: {
                        if (local_t < fade_in)
                            anim_dy = (1.f - local_t / fade_in) * 80.f;
                        break;
                    }
                    default: break;
                }
            }

            // Apply effect clip overrides
            if (text_fx.any_text) {
                anim_alpha *= text_fx.opacity_mul;
                fsz        *= text_fx.scale_mul;
            }

            float tx_anim  = tx + anim_dx;
            float ty_anim  = slot_y + anim_dy;
            ImU32 shad_col = IM_COL32(0, 0, 0, (int)(180.f * anim_alpha));

            // Block style: filled box behind text
            if (eff_style == AnimStyle::Block && active_ci >= 0) {
                float pad_x = 8.f, pad_y = 4.f;
                dl->AddRectFilled(
                    {tx_anim - pad_x, ty_anim - pad_y},
                    {tx_anim + tsz.x + pad_x, ty_anim + tsz.y + pad_y},
                    to_u32(Col::fg), 2.f);
            }

            // Shadow pass
            dl->AddText(ImGui::GetFont(), fsz, {tx_anim+2.f, ty_anim+2.f}, shad_col, show->text.c_str());

            // Karaoke: per-word coloring when words_cache is available and clip is active.
            // Falls back to solid color for selected-but-not-playing previews.
            bool did_karaoke = false;
            if (active_ci >= 0 && show->karaoke && !state.words_cache.empty()) {
                std::vector<const WordEntry*> clip_words;
                for (auto& we : state.words_cache)
                    if (we.end > show->start && we.start < show->end)
                        clip_words.push_back(&we);

                if (!clip_words.empty()) {
                    did_karaoke = true;
                    float cur_x = tx_anim;
                    for (int wi = 0; wi < (int)clip_words.size(); ++wi) {
                        const WordEntry* we = clip_words[wi];
                        bool is_active = state.playhead >= we->start && state.playhead < we->end;

                        ImU32 wcol;
                        if (show->sub_color_override) {
                            float a = (is_active ? show->sub_color[3] : show->sub_color[3] * 0.45f) * anim_alpha;
                            wcol = IM_COL32((int)(show->sub_color[0]*255),
                                            (int)(show->sub_color[1]*255),
                                            (int)(show->sub_color[2]*255), (int)(a*255));
                        } else {
                            wcol = is_active ? IM_COL32(255,255,255,(int)(255*anim_alpha))
                                             : IM_COL32(255,255,255,(int)(100*anim_alpha));
                        }

                        std::string word_sp = we->text + (wi+1 < (int)clip_words.size() ? " " : "");
                        ImVec2 wsz = ImGui::CalcTextSize(word_sp.c_str());
                        dl->AddText(ImGui::GetFont(), fsz, {cur_x, ty_anim}, wcol, word_sp.c_str());
                        cur_x += wsz.x;
                    }
                }
            }

            if (!did_karaoke) {
                ImU32 tcol;
                if (show->sub_color_override) {
                    float a = ((active_ci >= 0) ? show->sub_color[3] : show->sub_color[3] * 0.5f) * anim_alpha;
                    tcol = IM_COL32((int)(show->sub_color[0]*255), (int)(show->sub_color[1]*255),
                                    (int)(show->sub_color[2]*255), (int)(a*255));
                } else if (eff_style == AnimStyle::Block && active_ci >= 0) {
                    tcol = to_u32(Col::bg);  // dark text on white block
                } else {
                    float a = (active_ci >= 0) ? anim_alpha : anim_alpha * 0.5f;
                    tcol = IM_COL32(255, 255, 255, (int)(255.f * a));
                }
                dl->AddText(ImGui::GetFont(), fsz, {tx_anim, ty_anim}, tcol, show->text.c_str());
            }

            ImGui::PopFont();

            if (show_ci >= 0 && slot_y >= p.y && slot_y + tsz.y <= p.y + h) {
                float pad = 8.f;
                bool in_handle = mpos.x >= tx_anim - pad && mpos.x <= tx_anim + tsz.x + pad &&
                                 mpos.y >= ty_anim - pad && mpos.y <= ty_anim + tsz.y + pad;
                bool is_this_drag = (s_drag_ti == ti && s_drag_ci == show_ci);

                if (in_handle || (is_this_drag && ldown))
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                if (in_handle && lclick) { s_drag_ti = ti; s_drag_ci = show_ci; }
                if (is_this_drag && ldown) {
                    s_dragging = true;
                    Clip& mc = state.tracks[ti].clips[show_ci];
                    float new_y = (mpos.y - p.y) / h;
                    mc.sub_pos   = 3;
                    mc.sub_pos_y = fmaxf(0.02f, fminf(0.98f, new_y));
                }
                if (in_handle && !ldown) {
                    float mid_x = tx_anim + tsz.x * 0.5f;
                    for (int d = -1; d <= 1; ++d)
                        dl->AddCircleFilled({mid_x + d*6.f, ty_anim - 6.f}, 2.f, to_u32(Col::muted));
                }
            }
            ++text_rendered;
        }
    }

    if (s_dragging && !ldown) {
        history_push(state, "Subtitle position Y");
        s_dragging = false; s_drag_ti = -1; s_drag_ci = -1;
    }

    // Border and chrome drawn last so they sit on top of all content
    dl->AddRect(p, {p.x+w, p.y+h}, to_u32(Col::line), 2.f);
    {
        const char* fmt_lbl = state.format == OutputFormat::Vertical   ? "9:16" :
                              state.format == OutputFormat::Horizontal  ? "16:9" : "1:1";
        dl->AddText({p.x+6.f, p.y+6.f}, to_u32(Col::dim), fmt_lbl);
    }
    float cm = 10.f; ImU32 cc = to_u32(Col::muted);
    dl->AddLine(p,             {p.x+cm, p.y},       cc);
    dl->AddLine(p,             {p.x, p.y+cm},       cc);
    dl->AddLine({p.x+w, p.y}, {p.x+w-cm, p.y},     cc);
    dl->AddLine({p.x+w, p.y}, {p.x+w, p.y+cm},     cc);
    dl->AddLine({p.x, p.y+h}, {p.x+cm, p.y+h},     cc);
    dl->AddLine({p.x, p.y+h}, {p.x, p.y+h-cm},     cc);
    dl->AddLine({p.x+w,p.y+h},{p.x+w-cm,p.y+h},    cc);
    dl->AddLine({p.x+w,p.y+h},{p.x+w,p.y+h-cm},    cc);
}

// ── Right panel: History tab ──────────────────────────────────────────────────

static void panel_history(AppState& state, float /*w*/) {
    ImGui::Dummy({0.f, 8.f});

    ImGui::BeginDisabled(!history_can_undo());
    if (ui_btn("Undo", false, true)) history_undo(state);
    ImGui::EndDisabled();
    ImGui::SameLine(0.f, 4.f);
    ImGui::BeginDisabled(!history_can_redo());
    if (ui_btn("Redo", false, true)) history_redo(state);
    ImGui::EndDisabled();

    ImGui::Dummy({0.f, 4.f}); ui_separator(); ImGui::Dummy({0.f, 4.f});

    const auto& entries = history_entries();
    int cur = history_pos();

    if (entries.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted("No history yet.");
        ImGui::PopStyleColor();
        return;
    }

    ImGui::BeginChild("##hist_list", {0.f, 0.f}, false);
    static int s_last_hist_pos = -1;
    for (int i = 0; i < (int)entries.size(); ++i) {
        bool is_cur  = (i == cur);
        bool is_redo = (i > cur);
        ImU32 col = is_redo ? to_u32(Col::dim)  :
                    is_cur  ? to_u32(Col::fg)    : to_u32(Col::muted);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        char lbl[128];
        snprintf(lbl, sizeof(lbl), "%s %s##hst%d",
            is_cur ? "\xe2\x96\xb6" : " ", entries[i].action.c_str(), i);
        if (ImGui::Selectable(lbl, is_cur)) history_jump(state, i);
        ImGui::PopStyleColor();
    }
    if (cur != s_last_hist_pos) {
        float frac = ((int)entries.size() > 1)
            ? (float)cur / (float)((int)entries.size() - 1) : 1.f;
        ImGui::SetScrollY(frac * ImGui::GetScrollMaxY());
        s_last_hist_pos = cur;
    }
    ImGui::EndChild();
}

// ── Right panel: Clip tab ─────────────────────────────────────────────────────

static char s_edit_buf[512] = {};
static bool s_edit_focus_next = false;

// ── Clip header helpers ───────────────────────────────────────────────────────

static ImU32 clip_type_badge_color(ClipType ct) {
    switch (ct) {
        case ClipType::Text:     return IM_COL32(80,140,220,255);
        case ClipType::Lyrics:   return IM_COL32(220,160,40,255);
        case ClipType::Subtitle: return IM_COL32(40,190,190,255);
        case ClipType::Video:    return IM_COL32(140,60,220,255);
        case ClipType::Audio:    return IM_COL32(50,180,100,255);
        default:                 return IM_COL32(120,80,220,255);
    }
}
static const char* clip_type_name(ClipType ct) {
    switch (ct) {
        case ClipType::Text:     return "TEXT";
        case ClipType::Lyrics:   return "LYRICS";
        case ClipType::Subtitle: return "SUBTITLE";
        case ClipType::Video:    return "VIDEO";
        case ClipType::Audio:    return "AUDIO";
        default:                 return "EFFECT";
    }
}

static void draw_clip_header(AppState& state, Clip& clip, Track& track, float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::Dummy({0.f, 8.f});

    // Type badge
    ImVec2 bp = ImGui::GetCursorScreenPos();
    const char* tname = clip_type_name(clip.clip_type);
    ImVec2 tsz = ImGui::CalcTextSize(tname);
    float bpad = 6.f, bh = tsz.y + 6.f;
    float bw = tsz.x + bpad * 2.f;
    ImU32 bcol = clip_type_badge_color(clip.clip_type);
    dl->AddRectFilled(bp, {bp.x+bw, bp.y+bh}, bcol, 3.f);
    dl->AddText({bp.x+bpad, bp.y+3.f}, IM_COL32(255,255,255,255), tname);

    // Track + clip index to the right of badge
    ImGui::SetCursorScreenPos({bp.x+bw+8.f, bp.y+3.f});
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    char tlbl[80];
    snprintf(tlbl, sizeof(tlbl), "%s  ·  clip %d of %d",
        track.name.c_str(), state.selected_clip+1, (int)track.clips.size());
    ImGui::TextUnformatted(tlbl);
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos({bp.x, bp.y + bh + 8.f});

    // Timing row — duration display only
    ImGui::Dummy({0.f, 4.f});
    char durbuf[48];
    snprintf(durbuf, sizeof(durbuf), "%.3fs  ·  start %.3fs", clip.end - clip.start, clip.start);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
    ImGui::TextUnformatted(durbuf);
    ImGui::PopStyleColor();

    // Nudge strip
    ImGui::Dummy({0.f, 4.f});
    struct NudgeBtn { float dt; const char* lbl; };
    static const NudgeBtn nudges[] = {{-1.f,"-1s"},{-0.1f,"-100ms"},{-0.01f,"-10ms"},
                                      {0.01f,"+10ms"},{0.1f,"+100ms"},{1.f,"+1s"}};
    bool nudged = false;
    for (auto& nb : nudges) {
        if (ui_btn(nb.lbl, false, true)) {
            clip.start += nb.dt; clip.end += nb.dt;
            if (clip.start < 0.f) { clip.end -= clip.start; clip.start = 0.f; }
            nudged = true;
        }
        ImGui::SameLine(0.f, 3.f);
    }
    ImGui::NewLine();
    if (nudged) history_push(state, "Nudge clip");

    // Action row
    ImGui::Dummy({0.f, 4.f});
    if (ui_btn("Split", false, true)) {
        float cut = state.playhead;
        if (cut > clip.start + 0.02f && cut < clip.end - 0.02f) {
            Clip right = clip; clip.end = cut; right.start = cut;
            track.clips.insert(track.clips.begin() + state.selected_clip + 1, right);
            history_push(state, "Split clip");
        }
    }
    ImGui::SameLine(0.f, 6.f);
    if (ui_btn("Duplicate", false, true)) {
        float len = clip.end - clip.start;
        Clip dup = clip; dup.start = clip.end; dup.end = clip.end + len;
        track.clips.insert(track.clips.begin() + state.selected_clip + 1, dup);
        history_push(state, "Duplicate clip");
    }
    ImGui::SameLine(0.f, 6.f);
    if (ui_btn("Delete", false, true)) {
        track.clips.erase(track.clips.begin() + state.selected_clip);
        state.selected_clip = -1;
        history_push(state, "Delete clip");
        return;
    }

    ImGui::Dummy({0.f, 8.f}); ui_separator();
}

// ── Word strip (deep lyrics word editor) ─────────────────────────────────────

static int  s_word_sel = -1;

static void draw_word_strip(AppState& state, Clip& clip, float w) {
    if (clip.words.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
        ImGui::TextWrapped("No word-level data — run ML Processing or apply grouping to populate.");
        ImGui::PopStyleColor();
        return;
    }

    float dur = clip.end - clip.start;
    if (dur <= 0.f) return;

    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2 origin   = ImGui::GetCursorScreenPos();
    const float sh  = 36.f;  // strip height
    const float gap = 2.f;   // pixel gap between word rects

    // Background
    dl->AddRectFilled(origin, {origin.x+w, origin.y+sh}, IM_COL32(15,15,25,255), 4.f);

    ImU32 hl_col = ImGui::ColorConvertFloat4ToU32(
        {clip.karaoke_highlight_color[0], clip.karaoke_highlight_color[1],
         clip.karaoke_highlight_color[2], clip.karaoke_highlight_color[3]});

    int n = (int)clip.words.size();

    // Word rects + invisible buttons
    for (int i = 0; i < n; ++i) {
        WordEntry& we = clip.words[i];
        float x0 = origin.x + (we.start - clip.start) / dur * w + gap;
        float x1 = origin.x + (we.end   - clip.start) / dur * w - gap;
        if (x1 <= x0 + 2.f) x1 = x0 + 4.f;

        bool active = (state.playhead >= we.start && state.playhead < we.end);
        bool sel    = (s_word_sel == i);

        ImU32 bg = active ? hl_col :
                   sel    ? IM_COL32(110,80,200,220) :
                            IM_COL32(55,55,75,220);
        dl->AddRectFilled({x0, origin.y+3.f}, {x1, origin.y+sh-3.f}, bg, 3.f);

        // Label
        float rw = x1 - x0;
        if (rw > 14.f) {
            std::string lbl = we.text;
            while (!lbl.empty() && ImGui::CalcTextSize(lbl.c_str()).x > rw - 4.f)
                lbl.pop_back();
            if (lbl.size() < we.text.size() && lbl.size() > 1)
                lbl.back() = '\xe2', lbl += "\x80\xa6";  // UTF-8 ellipsis
            dl->AddText({x0+3.f, origin.y+sh*0.5f-6.f},
                IM_COL32(255,255,255, sel||active ? 255 : 180), lbl.c_str());
        }

        // Click target
        ImGui::SetCursorScreenPos({x0, origin.y});
        char wid[32]; snprintf(wid, sizeof(wid), "##word%d", i);
        if (ImGui::InvisibleButton(wid, {x1-x0, sh})) {
            s_word_sel = (s_word_sel == i) ? -1 : i;
            if (s_word_sel == i) seek_to(state, we.start);
        }
    }

    // Boundary drag handles between words
    for (int i = 0; i < n - 1; ++i) {
        float bx = origin.x + (clip.words[i].end - clip.start) / dur * w;
        bool hov_or_drag = false;

        ImGui::SetCursorScreenPos({bx - 5.f, origin.y});
        char bid[32]; snprintf(bid, sizeof(bid), "##bound%d", i);
        ImGui::InvisibleButton(bid, {10.f, sh});

        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
            float dt = ImGui::GetIO().MouseDelta.x / w * dur;
            float new_t = clip.words[i].end + dt;
            float lo = clip.words[i].start + 0.02f;
            float hi = clip.words[i+1].end  - 0.02f;
            new_t = fmaxf(lo, fminf(hi, new_t));
            clip.words[i].end      = new_t;
            clip.words[i+1].start  = new_t;
            hov_or_drag = true;
        }
        hov_or_drag |= ImGui::IsItemHovered();

        // Draw handle line on top (foreground draw list to clear z-order issues)
        ImU32 hcol = IM_COL32(255,255,255, hov_or_drag ? 220 : 70);
        dl->AddLine({bx, origin.y+2.f}, {bx, origin.y+sh-2.f}, hcol, hov_or_drag ? 2.f : 1.f);
    }

    // Playhead cursor
    if (state.playhead >= clip.start && state.playhead < clip.end) {
        float px = origin.x + (state.playhead - clip.start) / dur * w;
        dl->AddLine({px, origin.y}, {px, origin.y+sh}, IM_COL32(255,80,80,220), 1.5f);
    }

    // Advance cursor past strip
    ImGui::SetCursorScreenPos({origin.x, origin.y + sh + 6.f});

    // ── Selected word controls ────────────────────────────────────────────────
    if (s_word_sel >= 0 && s_word_sel < n) {
        WordEntry& sel = clip.words[s_word_sel];
        ImGui::Dummy({0.f, 4.f});

        // Word text edit
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
        ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
        static char s_word_buf[128] = {};
        static int  s_word_buf_for  = -1;
        if (s_word_buf_for != s_word_sel) {
            strncpy(s_word_buf, sel.text.c_str(), sizeof(s_word_buf)-1);
            s_word_buf[sizeof(s_word_buf)-1] = '\0';
            s_word_buf_for = s_word_sel;
        }
        ImGui::SetNextItemWidth(w * 0.45f);
        if (ImGui::InputText("##wtext", s_word_buf, sizeof(s_word_buf),
                ImGuiInputTextFlags_EnterReturnsTrue)) {
            sel.text = s_word_buf;
            // Rebuild clip text
            std::string full;
            for (int i2 = 0; i2 < n; ++i2) {
                if (i2) full += ' ';
                full += clip.words[i2].text;
            }
            clip.text = full;
            history_push(state, "Edit word text");
        }
        if (ImGui::IsItemDeactivated()) {
            sel.text = s_word_buf;
            std::string full;
            for (int i2 = 0; i2 < n; ++i2) { if (i2) full+=' '; full+=clip.words[i2].text; }
            clip.text = full;
        }
        ImGui::PopStyleColor(2);

        // Start / End fields side by side
        ImGui::SameLine(0.f, 8.f);
        float fw = (w - ImGui::GetItemRectSize().x - 32.f) * 0.5f;
        ImGui::SetNextItemWidth(fw);
        float ws = sel.start - clip.start;
        if (ImGui::InputFloat("##ws", &ws, 0.001f, 0.01f, "%.3f")) {
            float abs_t = clip.start + fmaxf(0.f, ws);
            if (abs_t < sel.end - 0.02f) { sel.start = abs_t; if (s_word_sel > 0) clip.words[s_word_sel-1].end = abs_t; }
        }
        ImGui::SameLine(0.f, 4.f);
        ImGui::SetNextItemWidth(fw);
        float we2 = sel.end - clip.start;
        if (ImGui::InputFloat("##we", &we2, 0.001f, 0.01f, "%.3f")) {
            float abs_t = clip.start + we2;
            if (abs_t > sel.start + 0.02f) { sel.end = abs_t; if (s_word_sel < n-1) clip.words[s_word_sel+1].start = abs_t; }
        }

        // Nudge start row
        ImGui::Dummy({0.f, 3.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim); ImGui::TextUnformatted("Start:"); ImGui::PopStyleColor();
        ImGui::SameLine();
        struct WNudge { float dt; const char* l; };
        static const WNudge wn[] = {{-0.05f,"-50ms"},{-0.01f,"-10ms"},{0.01f,"+10ms"},{0.05f,"+50ms"}};
        for (auto& nb : wn) {
            if (ui_btn(nb.l, false, true)) {
                float nt = sel.start + nb.dt;
                if (nt >= clip.start && nt < sel.end - 0.02f) {
                    sel.start = nt;
                    if (s_word_sel > 0) clip.words[s_word_sel-1].end = nt;
                    history_push(state, "Nudge word start");
                }
            }
            ImGui::SameLine(0.f, 3.f);
        }
        ImGui::NewLine();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim); ImGui::TextUnformatted("End:  "); ImGui::PopStyleColor();
        ImGui::SameLine();
        for (auto& nb : wn) {
            if (ui_btn(nb.l, false, true)) {
                float nt = sel.end + nb.dt;
                if (nt > sel.start + 0.02f && nt <= clip.end) {
                    sel.end = nt;
                    if (s_word_sel < n-1) clip.words[s_word_sel+1].start = nt;
                    history_push(state, "Nudge word end");
                }
            }
            ImGui::SameLine(0.f, 3.f);
        }
        ImGui::NewLine();

        // Split / Merge / Delete
        ImGui::Dummy({0.f, 4.f});
        if (s_word_sel > 0 && ui_btn("Split here", false, true)) {
            // Split clip at this word's start, word goes to right clip
            float split_t = sel.start;
            if (split_t > clip.start + 0.02f && split_t < clip.end - 0.02f) {
                Clip right = clip;
                right.start = split_t;
                right.words.assign(clip.words.begin() + s_word_sel, clip.words.end());
                clip.words.erase(clip.words.begin() + s_word_sel, clip.words.end());
                clip.end = split_t;
                // Rebuild texts
                std::string lt, rt;
                for (auto& we3 : clip.words)  { if (!lt.empty()) lt+=' '; lt+=we3.text; }
                for (auto& we3 : right.words) { if (!rt.empty()) rt+=' '; rt+=we3.text; }
                clip.text = lt; right.text = rt;
                int ins = state.selected_clip + 1;
                state.tracks[state.selected_track].clips.insert(
                    state.tracks[state.selected_track].clips.begin() + ins, right);
                s_word_sel = -1;
                history_push(state, "Split clip at word");
            }
        }
        ImGui::SameLine(0.f, 6.f);

        // Merge with next clip on same track
        bool can_merge = false;
        int nc_idx = state.selected_clip + 1;
        Track& tr2 = state.tracks[state.selected_track];
        if (nc_idx < (int)tr2.clips.size() && tr2.clips[nc_idx].clip_type == clip.clip_type)
            can_merge = true;
        if (!can_merge) ImGui::BeginDisabled();
        if (ui_btn("Merge with next", false, true) && can_merge) {
            Clip& next = tr2.clips[nc_idx];
            clip.end = next.end;
            for (auto& we3 : next.words) clip.words.push_back(we3);
            if (!clip.text.empty() && !next.text.empty()) clip.text += ' ';
            clip.text += next.text;
            tr2.clips.erase(tr2.clips.begin() + nc_idx);
            history_push(state, "Merge clips");
        }
        if (!can_merge) ImGui::EndDisabled();
        ImGui::SameLine(0.f, 6.f);

        if (ui_btn("Del word", false, true)) {
            clip.words.erase(clip.words.begin() + s_word_sel);
            s_word_sel = -1;
            std::string full;
            for (auto& we3 : clip.words) { if (!full.empty()) full+=' '; full+=we3.text; }
            clip.text = full;
            history_push(state, "Delete word");
        }
    }
}

// ── Shared section helpers ────────────────────────────────────────────────────

static void section_position(AppState& state, Clip& clip, float w) {
    struct PosBtn { int v; const char* label; };
    PosBtn pbtns[] = {{0,"Bottom"},{1,"Center"},{2,"Top"},{3,"Custom Y"}};
    for (auto& pb : pbtns) {
        if (ui_btn(pb.label, clip.sub_pos == pb.v, true)) {
            clip.sub_pos = pb.v; history_push(state, "Position");
        }
        ImGui::SameLine(0.f, 4.f);
    }
    ImGui::NewLine();
    if (clip.sub_pos == 3) {
        ImGui::Dummy({0.f, 4.f});
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, Col::fg);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
        ImGui::SetNextItemWidth(w - 16.f);
        ImGui::SliderFloat("##sub_y", &clip.sub_pos_y, 0.f, 1.f, "Y  %.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Position Y");
        ImGui::PopStyleColor(2);
    }
    ImGui::Dummy({0.f, 4.f});
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted("Horizontal"); ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 2.f});
    struct HBtn { int v; const char* l; };
    HBtn hbtns[] = {{0,"Left"},{1,"Center"},{2,"Right"}};
    for (auto& hb : hbtns) {
        if (ui_btn(hb.l, clip.sub_anchor_h == hb.v, true)) {
            clip.sub_anchor_h = hb.v; history_push(state, "H anchor");
        }
        ImGui::SameLine(0.f, 4.f);
    }
    ImGui::NewLine();
}

static void section_color(AppState& state, Clip& clip, float w) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
    bool col_ov = clip.sub_color_override;
    if (ImGui::Checkbox("Override color##col_ov", &col_ov)) {
        clip.sub_color_override = col_ov; history_push(state, "Color override");
    }
    if (clip.sub_color_override) {
        ImGui::Dummy({0.f, 4.f});
        ImGui::SetNextItemWidth(w - 16.f);
        ImGui::ColorEdit4("##sub_col", clip.sub_color,
            ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar);
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Clip color");
    }
    ImGui::PopStyleColor();
}

static void section_fade(AppState& state, Clip& clip, float w) {
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, Col::fg);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
    float sw = w - 16.f;
    ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Fade in##fi",  &clip.fade_in,  0.f, 4.f, "%.2fs");
    if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Fade in");
    ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Fade out##fo", &clip.fade_out, 0.f, 4.f, "%.2fs");
    if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Fade out");
    ImGui::PopStyleColor(2);
    ImGui::Dummy({0.f, 2.f});
    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
    ImGui::TextWrapped("Opacity ramp — add manual opacity keyframes to override.");
    ImGui::PopStyleColor();
}

static void panel_clip(AppState& state, float w) {
    // ── Nothing selected ──────────────────────────────────────────────────────
    if (state.selected_track < 0 || state.selected_track >= (int)state.tracks.size()) {
        ImGui::Dummy({0.f, 32.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        auto centre = [&](const char* s) {
            ImGui::SetCursorPosX((w - ImGui::CalcTextSize(s).x) * 0.5f);
            ImGui::TextUnformatted(s);
        };
        centre("No track selected");
        ImGui::Dummy({0.f, 4.f});
        centre("Click a clip in the timeline to edit it");
        ImGui::PopStyleColor();
        return;
    }

    Track& track = state.tracks[state.selected_track];

    if (state.selected_clip < 0 || state.selected_clip >= (int)track.clips.size()) {
        ImGui::Dummy({0.f, 32.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::SetCursorPosX((w - ImGui::CalcTextSize("Click a clip to edit it").x) * 0.5f);
        ImGui::TextUnformatted("Click a clip to edit it");
        ImGui::PopStyleColor();
        return;
    }

    Clip& clip = track.clips[state.selected_clip];

    draw_clip_header(state, clip, track, w);
    // draw_clip_header may delete the clip (Delete button) — check again
    if (state.selected_clip < 0 || state.selected_clip >= (int)track.clips.size()) return;

    ImGui::Dummy({0.f, 4.f});

    // ── Keyframe slider helper (shared by transform + audio sections) ─────────
    float t_local = state.playhead - clip.start;
    int   sel_ti  = state.selected_track, sel_ci = state.selected_clip;

    auto kf_slider = [&](const char* prop, const char* label,
                          float* val_ptr, float vmin, float vmax, const char* fmt) -> bool
    {
        bool changed = false;
        PropTrack& pt = clip.ktracks[prop];
        bool has_kf   = (pt.find_nearest(t_local, 0.05f) >= 0);

        ImGui::PushStyleColor(ImGuiCol_Button,
            has_kf ? IM_COL32(255,200,60,200) : IM_COL32(80,80,80,180));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255,220,80,220));
        char kbid[64]; snprintf(kbid, sizeof(kbid), "\xe2\x97\x86##kf_%s", prop);
        if (ImGui::Button(kbid, {20.f, 0.f})) {
            if (has_kf) {
                pt.remove_at(t_local, 0.05f);
                if (pt.empty()) clip.ktracks.erase(prop);
                history_push(state, std::string("Remove KF ") + prop);
            } else {
                float cur = clip.eval_prop(prop, state.playhead);
                pt.set(t_local, cur);
                state.kf_sel_track = sel_ti; state.kf_sel_clip = sel_ci;
                state.kf_sel_prop  = prop;
                state.kf_sel_idx   = pt.find_nearest(t_local, 0.1f);
                history_push(state, std::string("Add KF ") + prop);
            }
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine(0.f, 4.f);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted(label); ImGui::PopStyleColor();
        ImGui::SameLine(0.f, 4.f);
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, has_kf ? IM_COL32(255,200,60,255) : to_u32(Col::fg));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
        float sw = w - 20.f - ImGui::CalcTextSize(label).x - 28.f;
        ImGui::SetNextItemWidth(fmaxf(40.f, sw));
        char sid[64]; snprintf(sid, sizeof(sid), "##kfs_%s", prop);
        if (ImGui::SliderFloat(sid, val_ptr, vmin, vmax, fmt)) {
            changed = true;
            if (has_kf) { int ki = pt.find_nearest(t_local, 0.05f); if (ki >= 0) pt.keys[ki].value = *val_ptr; }
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, std::string("Edit ") + prop);
        return changed;
    };

    // Interp bar for whichever keyframe is selected
    auto kf_interp_bar = [&]() {
        bool sel_this = (state.kf_sel_track == sel_ti && state.kf_sel_clip == sel_ci &&
                         !state.kf_sel_prop.empty() && state.kf_sel_idx >= 0);
        if (!sel_this) return;
        auto it2 = clip.ktracks.find(state.kf_sel_prop);
        if (it2 == clip.ktracks.end() || state.kf_sel_idx >= (int)it2->second.keys.size()) return;
        Keyframe& kf = it2->second.keys[state.kf_sel_idx];
        ImGui::Dummy({0.f, 4.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted("Interp:"); ImGui::PopStyleColor(); ImGui::SameLine(0.f, 6.f);
        struct IT { InterpType t; const char* n; };
        IT its[] = {{InterpType::Linear,"Lin"},{InterpType::EaseIn,"In"},
                    {InterpType::EaseOut,"Out"},{InterpType::EaseBoth,"Both"},{InterpType::Hold,"Hold"}};
        for (auto& it3 : its) {
            if (ui_btn(it3.n, kf.interp == it3.t, true)) { kf.interp = it3.t; history_push(state, "KF interp"); }
            ImGui::SameLine(0.f, 4.f);
        }
        ImGui::NewLine();
    };

    // ═══════════════════════════════════════════════════════════════════════════
    // TEXT CLIP
    // ═══════════════════════════════════════════════════════════════════════════
    if (clip.clip_type == ClipType::Text) {
        if (ImGui::CollapsingHeader("Content", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Dummy({0.f, 4.f});
            if (s_edit_focus_next) { ImGui::SetKeyboardFocusHere(); s_edit_focus_next = false; }
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
            ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
            ImGui::SetNextItemWidth(w - 16.f);
            if (ImGui::InputText("##clip_text", s_edit_buf, sizeof(s_edit_buf),
                    ImGuiInputTextFlags_EnterReturnsTrue))
                clip.text = s_edit_buf;
            if (ImGui::IsItemDeactivated()) {
                if (clip.text != s_edit_buf) history_push(state, "Edit text");
                clip.text = s_edit_buf;
            }
            ImGui::PopStyleColor(2);
            ImGui::Dummy({0.f, 4.f});
        }
        if (ImGui::CollapsingHeader("Position", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Dummy({0.f, 4.f}); section_position(state, clip, w); ImGui::Dummy({0.f, 4.f});
        }
        if (ImGui::CollapsingHeader("Color")) {
            ImGui::Dummy({0.f, 4.f}); section_color(state, clip, w); ImGui::Dummy({0.f, 4.f});
        }
        if (ImGui::CollapsingHeader("Fade")) {
            ImGui::Dummy({0.f, 4.f}); section_fade(state, clip, w); ImGui::Dummy({0.f, 4.f});
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // LYRICS CLIP
    // ═══════════════════════════════════════════════════════════════════════════
    else if (clip.clip_type == ClipType::Lyrics) {
        // Sync s_edit_buf to current clip text when selection changes
        static int s_last_lyrics_clip = -1;
        if (s_last_lyrics_clip != state.selected_clip) {
            strncpy(s_edit_buf, clip.text.c_str(), sizeof(s_edit_buf)-1);
            s_edit_buf[sizeof(s_edit_buf)-1] = '\0';
            s_last_lyrics_clip = state.selected_clip;
        }

        if (ImGui::CollapsingHeader("Content", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Dummy({0.f, 4.f});
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
            ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
            ImGui::SetNextItemWidth(w - 16.f);
            if (ImGui::InputText("##lyr_text", s_edit_buf, sizeof(s_edit_buf),
                    ImGuiInputTextFlags_EnterReturnsTrue))
                clip.text = s_edit_buf;
            if (ImGui::IsItemDeactivated()) {
                if (clip.text != s_edit_buf) history_push(state, "Edit lyrics text");
                clip.text = s_edit_buf;
            }
            ImGui::PopStyleColor(2);
            ImGui::Dummy({0.f, 4.f});
        }

        if (ImGui::CollapsingHeader("Words", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Dummy({0.f, 4.f}); draw_word_strip(state, clip, w - 8.f); ImGui::Dummy({0.f, 4.f});
        }

        if (ImGui::CollapsingHeader("Karaoke")) {
            ImGui::Dummy({0.f, 4.f});
            bool kar = clip.karaoke;
            if (ImGui::Checkbox("Enable karaoke highlight##kar", &kar)) {
                clip.karaoke = kar; history_push(state, "Karaoke toggle");
            }
            if (clip.karaoke) {
                ImGui::Dummy({0.f, 6.f});
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted("Base color"); ImGui::PopStyleColor();
                ImGui::SetNextItemWidth(w - 16.f);
                ImGui::ColorEdit4("##lyr_base_col", clip.sub_color,
                    ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar);
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Base color");
                ImGui::Dummy({0.f, 4.f});
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted("Highlight color"); ImGui::PopStyleColor();
                ImGui::SetNextItemWidth(w - 16.f);
                ImGui::ColorEdit4("##lyr_hl_col", clip.karaoke_highlight_color,
                    ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar);
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Highlight color");
            }
            ImGui::Dummy({0.f, 4.f});
        }

        if (ImGui::CollapsingHeader("Position")) {
            ImGui::Dummy({0.f, 4.f}); section_position(state, clip, w); ImGui::Dummy({0.f, 4.f});
        }
        if (ImGui::CollapsingHeader("Color")) {
            ImGui::Dummy({0.f, 4.f}); section_color(state, clip, w); ImGui::Dummy({0.f, 4.f});
        }
        if (ImGui::CollapsingHeader("Fade")) {
            ImGui::Dummy({0.f, 4.f}); section_fade(state, clip, w); ImGui::Dummy({0.f, 4.f});
        }

        if (ImGui::CollapsingHeader("Grouping")) {
            ImGui::Dummy({0.f, 4.f});
            if (!state.words_json_path.empty() && fs::exists(state.words_json_path)) {
                struct ModeBtn { SubtitleMode m; const char* label; const char* tip; };
                static const ModeBtn modes[] = {
                    {SubtitleMode::Word,    "Word",    "One clip per word"},
                    {SubtitleMode::Phrase,  "Phrase",  "Group by short pauses (>0.3s)"},
                    {SubtitleMode::Line,    "Line",    "Group by breath gaps (>0.8s)"},
                    {SubtitleMode::Segment, "Segment", "WhisperX sentence boundaries"},
                    {SubtitleMode::CustomN, "Custom",  "N words per clip"},
                };
                for (auto& mb : modes) {
                    bool sel2 = state.subtitle_mode == mb.m;
                    if (ui_btn(mb.label, sel2, true)) state.subtitle_mode = mb.m;
                    if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::TextUnformatted(mb.tip); ImGui::EndTooltip(); }
                    ImGui::SameLine(0.f, 4.f);
                }
                ImGui::NewLine();
                if (state.subtitle_mode == SubtitleMode::CustomN) {
                    ImGui::Dummy({0.f, 4.f});
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
                    ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
                    ImGui::SetNextItemWidth(80.f);
                    int n = state.subtitle_n;
                    if (ImGui::InputInt("words/clip##cn", &n))
                        state.subtitle_n = (n < 1) ? 1 : (n > 20) ? 20 : n;
                    ImGui::PopStyleColor(2);
                }
                ImGui::Dummy({0.f, 6.f});
                if (ui_btn("Apply grouping", true, true)) {
                    apply_subtitle_mode(state);
                    const char* mn = state.subtitle_mode == SubtitleMode::Word ? "Word" :
                        state.subtitle_mode == SubtitleMode::Phrase  ? "Phrase"  :
                        state.subtitle_mode == SubtitleMode::Line    ? "Line"    :
                        state.subtitle_mode == SubtitleMode::Segment ? "Segment" : "Custom";
                    history_push(state, std::string("Grouping — ") + mn);
                }
                // Propagate to multi-selection
                int sel_count = 0;
                for (auto& [st2, sc2] : state.clip_selection) {
                    if (st2 == sel_ti && sc2 == sel_ci) continue;
                    if (st2 < (int)state.tracks.size() && sc2 < (int)state.tracks[st2].clips.size() &&
                        state.tracks[st2].clips[sc2].clip_type == ClipType::Lyrics) ++sel_count;
                }
                if (sel_count > 0) {
                    ImGui::SameLine(0.f, 6.f);
                    char slbl[48]; snprintf(slbl, sizeof(slbl), "Apply to %d selected##lyr", sel_count);
                    if (ui_btn(slbl, false, true)) {
                        for (auto& [st2, sc2] : state.clip_selection) {
                            if (st2 == sel_ti && sc2 == sel_ci) continue;
                            if (st2 >= (int)state.tracks.size() || sc2 >= (int)state.tracks[st2].clips.size()) continue;
                            Clip& tgt = state.tracks[st2].clips[sc2];
                            if (tgt.clip_type != ClipType::Lyrics) continue;
                            tgt.karaoke = clip.karaoke; tgt.sub_pos = clip.sub_pos;
                            tgt.sub_pos_y = clip.sub_pos_y;
                            tgt.sub_color_override = clip.sub_color_override;
                            memcpy(tgt.sub_color, clip.sub_color, sizeof(clip.sub_color));
                            memcpy(tgt.karaoke_highlight_color, clip.karaoke_highlight_color,
                                   sizeof(clip.karaoke_highlight_color));
                        }
                        history_push(state, "Apply style to selected");
                    }
                }
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                ImGui::TextWrapped("Run ML Processing on an audio clip to generate word JSON, then grouping controls appear here.");
                ImGui::PopStyleColor();
            }
            ImGui::Dummy({0.f, 4.f});
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // SUBTITLE CLIP
    // ═══════════════════════════════════════════════════════════════════════════
    else if (clip.clip_type == ClipType::Subtitle) {
        // Build cross-track list sorted by start time
        struct SRTEntry { int ti, ci; };
        std::vector<SRTEntry> entries;
        for (int ti2 = 0; ti2 < (int)state.tracks.size(); ++ti2)
            for (int ci2 = 0; ci2 < (int)state.tracks[ti2].clips.size(); ++ci2) {
                const Clip& c2 = state.tracks[ti2].clips[ci2];
                if (c2.clip_type == ClipType::Subtitle &&
                    (clip.source_id.empty() || c2.source_id == clip.source_id))
                    entries.push_back({ti2, ci2});
            }
        std::sort(entries.begin(), entries.end(), [&](const SRTEntry& a, const SRTEntry& b){
            return state.tracks[a.ti].clips[a.ci].start < state.tracks[b.ti].clips[b.ci].start;
        });
        int n_total = (int)entries.size();

        // Find / Replace bar
        static char s_find[128] = {}, s_replace[128] = {};
        if (ImGui::CollapsingHeader("Find & Replace")) {
            ImGui::Dummy({0.f, 4.f});
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
            ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
            ImGui::SetNextItemWidth(w - 16.f);
            ImGui::InputText("Find##fr_find", s_find, sizeof(s_find));
            ImGui::SetNextItemWidth(w - 16.f);
            ImGui::InputText("Replace##fr_rep", s_replace, sizeof(s_replace));
            ImGui::PopStyleColor(2);
            ImGui::Dummy({0.f, 4.f});
            if (ui_btn("Replace all##frbtn", false, true) && s_find[0]) {
                std::string from(s_find), to(s_replace);
                int count = 0;
                for (auto& e : entries) {
                    Clip& sc2 = state.tracks[e.ti].clips[e.ci];
                    std::string& txt = sc2.text;
                    std::string out; size_t pos = 0, found;
                    while ((found = txt.find(from, pos)) != std::string::npos) {
                        out += txt.substr(pos, found - pos); out += to;
                        pos = found + from.size(); ++count;
                    }
                    out += txt.substr(pos);
                    if (count) txt = out;
                }
                if (count) history_push(state, "Find & Replace");
            }
            ImGui::Dummy({0.f, 4.f});
        }

        // Bulk timing shift
        if (ImGui::CollapsingHeader("Bulk Shift")) {
            ImGui::Dummy({0.f, 4.f});
            static float s_shift_amt = 0.f;
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
            ImGui::SetNextItemWidth(w * 0.55f);
            ImGui::InputFloat("seconds##shift", &s_shift_amt, 0.01f, 0.1f, "%.3f");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 4.f});
            if (ui_btn("Shift all##shiftbtn", false, true)) {
                for (auto& e : entries) {
                    state.tracks[e.ti].clips[e.ci].start += s_shift_amt;
                    state.tracks[e.ti].clips[e.ci].end   += s_shift_amt;
                    if (state.tracks[e.ti].clips[e.ci].start < 0.f) {
                        state.tracks[e.ti].clips[e.ci].end -= state.tracks[e.ti].clips[e.ci].start;
                        state.tracks[e.ti].clips[e.ci].start = 0.f;
                    }
                }
                history_push(state, "Bulk shift subtitles");
            }
            ImGui::Dummy({0.f, 4.f});
        }

        if (ImGui::CollapsingHeader("Subtitles", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Dummy({0.f, 4.f});
            char info[64];
            if (!clip.source_id.empty()) {
                std::string fn = fs::path(clip.source_id).filename().string();
                snprintf(info, sizeof(info), "%d clips  ·  %s", n_total, fn.c_str());
            } else {
                snprintf(info, sizeof(info), "%d clips", n_total);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted(info); ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 4.f});

            int sel_row = -1;
            for (int i = 0; i < n_total; ++i)
                if (entries[i].ti == sel_ti && entries[i].ci == sel_ci) { sel_row = i; break; }

            static int s_srt_last_row = -1;
            float row_h  = 22.f;
            float list_h = fminf(200.f, n_total * row_h + 6.f);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Col::bg_soft);
            ImGui::BeginChild("##srt_list", {0.f, list_h}, false);
            for (int i = 0; i < n_total; ++i) {
                const SRTEntry& e  = entries[i];
                Clip& sc2          = state.tracks[e.ti].clips[e.ci];
                bool  is_sel       = (i == sel_row);
                if (is_sel && s_srt_last_row != i) ImGui::SetScrollHereY(0.5f);
                ImGui::PushStyleColor(ImGuiCol_Text, is_sel ? Col::fg : Col::muted);
                char row_id[32]; snprintf(row_id, sizeof(row_id), "##srt%d", i);
                if (ImGui::Selectable(row_id, is_sel, 0, {0.f, row_h})) {
                    state.selected_track = e.ti; state.selected_clip = e.ci;
                    strncpy(s_edit_buf, sc2.text.c_str(), sizeof(s_edit_buf)-1);
                    s_edit_buf[sizeof(s_edit_buf)-1] = '\0';
                    seek_to(state, sc2.start);
                }
                ImGui::SameLine(0.f, 4.f);
                float dur2 = sc2.end - sc2.start;
                std::string preview = sc2.text.size() > 30 ? sc2.text.substr(0, 28) + "\xe2\x80\xa6" : sc2.text;
                char rowlbl[96];
                snprintf(rowlbl, sizeof(rowlbl), "%2d  %s  %.2fs  %s",
                    i+1, fmt_time(sc2.start).c_str(), dur2, preview.c_str());
                ImGui::TextUnformatted(rowlbl);
                ImGui::PopStyleColor();
            }
            s_srt_last_row = sel_row;
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 4.f});

            // Edit selected
            if (sel_row >= 0 && sel_row < n_total) {
                Clip& sc2 = state.tracks[entries[sel_row].ti].clips[entries[sel_row].ci];
                ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
                ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
                ImGui::SetNextItemWidth(w - 16.f);
                if (s_edit_focus_next) { ImGui::SetKeyboardFocusHere(); s_edit_focus_next = false; }
                if (ImGui::InputText("##srt_text", s_edit_buf, sizeof(s_edit_buf),
                        ImGuiInputTextFlags_EnterReturnsTrue))
                    sc2.text = s_edit_buf;
                if (ImGui::IsItemDeactivated()) {
                    if (sc2.text != s_edit_buf) history_push(state, "Edit subtitle text");
                    sc2.text = s_edit_buf;
                }
                ImGui::PopStyleColor(2);

                float half = (w - 24.f) * 0.5f;
                ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
                ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
                ImGui::BeginGroup();
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted("Start"); ImGui::PopStyleColor();
                ImGui::SetNextItemWidth(half);
                float ss = sc2.start;
                if (ImGui::InputFloat("##srt_s", &ss, 0.01f, 0.1f, "%.3f"))
                    if (ss < sc2.end - 0.01f) { sc2.start = fmaxf(0.f, ss); history_push(state, "Subtitle timing"); }
                ImGui::EndGroup(); ImGui::SameLine(0.f, 8.f); ImGui::BeginGroup();
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted("End"); ImGui::PopStyleColor();
                ImGui::SetNextItemWidth(half);
                float se = sc2.end;
                if (ImGui::InputFloat("##srt_e", &se, 0.01f, 0.1f, "%.3f"))
                    if (se > sc2.start + 0.01f) { sc2.end = se; history_push(state, "Subtitle timing"); }
                ImGui::EndGroup();
                ImGui::PopStyleColor(2);

                ImGui::Dummy({0.f, 4.f});
                bool nudged = false;
                if (ui_btn("-100ms", false, true)) { sc2.start-=0.1f; sc2.end-=0.1f; if(sc2.start<0.f){sc2.end-=sc2.start;sc2.start=0.f;} nudged=true; }
                ImGui::SameLine(0.f, 4.f);
                if (ui_btn("-10ms",  false, true)) { sc2.start-=0.01f; sc2.end-=0.01f; nudged=true; }
                ImGui::SameLine(0.f, 4.f);
                if (ui_btn("+10ms",  false, true)) { sc2.start+=0.01f; sc2.end+=0.01f; nudged=true; }
                ImGui::SameLine(0.f, 4.f);
                if (ui_btn("+100ms", false, true)) { sc2.start+=0.1f;  sc2.end+=0.1f;  nudged=true; }
                if (nudged) history_push(state, "Nudge subtitle");

                ImGui::Dummy({0.f, 6.f});
                if (ui_btn("+ Add below", false, true)) {
                    Clip nc; nc.clip_type = ClipType::Subtitle; nc.source_id = sc2.source_id;
                    nc.start = sc2.end; nc.end = sc2.end + 2.f;
                    Track& tgt_track = state.tracks[entries[sel_row].ti];
                    int ins = entries[sel_row].ci + 1;
                    tgt_track.clips.insert(tgt_track.clips.begin() + ins, nc);
                    state.selected_clip = ins;
                    strncpy(s_edit_buf, "", 1); s_edit_focus_next = true;
                    history_push(state, "Add subtitle");
                }
                ImGui::SameLine(0.f, 4.f);
                if (ui_btn("Delete##srtdel", false, true)) {
                    Track& del_track = state.tracks[entries[sel_row].ti];
                    del_track.clips.erase(del_track.clips.begin() + entries[sel_row].ci);
                    int new_row = std::min(sel_row, n_total - 2);
                    if (new_row >= 0 && new_row < n_total - 1) {
                        state.selected_track = entries[new_row].ti;
                        state.selected_clip  = entries[new_row].ci;
                        if (entries[sel_row].ti == entries[new_row].ti &&
                            entries[sel_row].ci  < entries[new_row].ci)
                            state.selected_clip--;
                        if (state.selected_clip >= 0 &&
                            state.selected_clip < (int)state.tracks[state.selected_track].clips.size()) {
                            strncpy(s_edit_buf,
                                state.tracks[state.selected_track].clips[state.selected_clip].text.c_str(),
                                sizeof(s_edit_buf)-1);
                            s_edit_buf[sizeof(s_edit_buf)-1] = '\0';
                        }
                    } else { state.selected_clip = -1; }
                    history_push(state, "Delete subtitle");
                }
            }
            ImGui::Dummy({0.f, 4.f});
        }

        if (ImGui::CollapsingHeader("Style")) {
            ImGui::Dummy({0.f, 4.f});
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted("Position"); ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 2.f});
            section_position(state, clip, w);
            ImGui::Dummy({0.f, 6.f});
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted("Color"); ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 2.f});
            section_color(state, clip, w);
            ImGui::Dummy({0.f, 4.f});
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // VIDEO CLIP
    // ═══════════════════════════════════════════════════════════════════════════
    else if (clip.clip_type == ClipType::Video) {
        if (ImGui::CollapsingHeader("Source", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Dummy({0.f, 4.f});
            std::string fname = clip.text.empty() ? "(no file)" : fs::path(clip.text).filename().string();
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted(fname.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && !clip.text.empty()) {
                ImGui::BeginTooltip(); ImGui::TextUnformatted(clip.text.c_str()); ImGui::EndTooltip();
            }
            ImGui::Dummy({0.f, 4.f});
            if (ui_btn("Replace file…", false, true)) {
                std::string np = filepicker_open("Replace video", "Video/Audio",
                    "*.mp4 *.mov *.mkv *.webm *.avi *.mp3 *.wav *.flac *.aac");
                if (!np.empty()) { clip.text = np; history_push(state, "Replace source"); }
            }
            ImGui::Dummy({0.f, 4.f});
        }

        if (ImGui::CollapsingHeader("Playback")) {
            ImGui::Dummy({0.f, 4.f});
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, Col::fg);
            ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
            ImGui::SetNextItemWidth(w - 16.f);
            ImGui::SliderFloat("Speed##vspd", &clip.speed, 0.25f, 4.f, "%.2f\xc3\x97");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Speed");
            ImGui::PopStyleColor(2);
            ImGui::Dummy({0.f, 4.f});
            struct SP { float f; const char* l; };
            SP spresets[] = {{0.25f,"¼×"},{0.5f,"½×"},{1.f,"1×"},{2.f,"2×"},{4.f,"4×"}};
            for (auto& p : spresets) {
                if (ui_btn(p.l, fabsf(clip.speed - p.f) < 0.01f, true)) { clip.speed = p.f; history_push(state, "Speed"); }
                ImGui::SameLine(0.f, 4.f);
            }
            ImGui::NewLine(); ImGui::Dummy({0.f, 4.f});
        }

        if (ImGui::CollapsingHeader("Appearance", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Dummy({0.f, 4.f});
            kf_slider("opacity", "Opacity", &clip.opacity, 0.f, 1.f, "%.2f");
            kf_interp_bar();
            ImGui::Dummy({0.f, 6.f});
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted("Blend mode"); ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 2.f});
            struct BM { int v; const char* l; };
            BM bms[] = {{0,"Normal"},{1,"Multiply"},{2,"Screen"},{3,"Overlay"}};
            for (auto& bm : bms) {
                if (ui_btn(bm.l, clip.blend_mode == bm.v, true)) { clip.blend_mode = bm.v; history_push(state, "Blend mode"); }
                ImGui::SameLine(0.f, 4.f);
            }
            ImGui::NewLine(); ImGui::Dummy({0.f, 4.f});
        }

        if (ImGui::CollapsingHeader("Fade")) {
            ImGui::Dummy({0.f, 4.f});
            section_fade(state, clip, w);
            ImGui::Dummy({0.f, 4.f});
        }

        if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Dummy({0.f, 4.f});
            kf_slider("pos_x",    "X",     &clip.pos_x,    0.f,  1.f,    "%.2f");
            kf_slider("pos_y",    "Y",     &clip.pos_y,    0.f,  1.f,    "%.2f");
            kf_slider("scale_x",  "ScX",   &clip.scale_x,  0.f,  4.f,    "%.2f");
            kf_slider("scale_y",  "ScY",   &clip.scale_y,  0.f,  4.f,    "%.2f");
            kf_slider("rotation", "Rot",   &clip.rotation, -180.f, 180.f, "%.1f\xc2\xb0");
            kf_interp_bar();
            ImGui::Dummy({0.f, 4.f});
        }

        if (ImGui::CollapsingHeader("Audio Track")) {
            ImGui::Dummy({0.f, 4.f});
            kf_slider("volume", "Volume", &clip.volume, 0.f, 2.f, "%.2f\xc3\x97");
            ImGui::Dummy({0.f, 6.f});
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, Col::fg);
            ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
            ImGui::SetNextItemWidth(w - 16.f);
            ImGui::SliderFloat("Pan##vpan", &clip.pan, -1.f, 1.f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Pan");
            ImGui::PopStyleColor(2);
            ImGui::Dummy({0.f, 4.f});
        }

        if (ImGui::CollapsingHeader("ML Processing")) {
            ImGui::Dummy({0.f, 4.f});
            bool busy     = transcribe_running();
            bool has_path = !clip.text.empty();
            bool ml_avail = state.models_ready;
            if (!ml_avail && !busy) {
                ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                ImGui::TextWrapped("Models not installed.");
                ImGui::PopStyleColor();
                if (ui_btn("Download Models…", false, true)) state.show_model_dl_modal = true;
                ImGui::Dummy({0.f, 4.f});
            }
            if (!ml_avail) ImGui::BeginDisabled();
            if (busy) {
                float bar_w = w - 16.f;
                ImVec2 bp = ImGui::GetCursorScreenPos();
                ImDrawList* bdl = ImGui::GetWindowDrawList();
                bdl->AddRectFilled(bp, {bp.x+bar_w, bp.y+4.f}, to_u32(Col::line), 2.f);
                bdl->AddRectFilled(bp, {bp.x+bar_w*state.pipeline.progress, bp.y+4.f}, to_u32(Col::fg), 2.f);
                ImGui::Dummy({0.f, 8.f});
                std::string msg = state.pipeline.message.empty() ? "Processing…" : state.pipeline.message;
                char pbuf[128]; snprintf(pbuf, sizeof(pbuf), "%s  %d%%", msg.c_str(), (int)(state.pipeline.progress*100.f));
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted(pbuf); ImGui::PopStyleColor();
                if (!state.pipeline.raw_line.empty()) {
                    std::string raw = state.pipeline.raw_line;
                    if (raw.size() > 100) raw = raw.substr(0, 97) + "...";
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim); ImGui::TextUnformatted(raw.c_str()); ImGui::PopStyleColor();
                }
                ImGui::Dummy({0.f, 4.f});
                if (ui_btn("Cancel", false, true)) transcribe_cancel();
            } else {
                if (!has_path) ImGui::BeginDisabled();
                if (ui_btn("Extract Lyrics", false, true)) kick_pipeline(state, clip.text, PipelineMode::Both);
                ImGui::Dummy({0.f, 2.f});
                if (ui_btn("Extract Subtitles", false, true)) kick_pipeline(state, clip.text, PipelineMode::TranscribeOnly);
                ImGui::Dummy({0.f, 2.f});
                if (ui_btn("Separate Vocals", false, true)) kick_pipeline(state, clip.text, PipelineMode::SeparateOnly);
                ImGui::Dummy({0.f, 2.f});
                ImGui::BeginDisabled();
                ui_btn("Remove Background  (rembg)", false, true);
                ImGui::EndDisabled();
                if (!has_path) ImGui::EndDisabled();
            }
            if (!ml_avail) ImGui::EndDisabled();
            ImGui::Dummy({0.f, 4.f});
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // AUDIO CLIP
    // ═══════════════════════════════════════════════════════════════════════════
    else if (clip.clip_type == ClipType::Audio) {
        if (ImGui::CollapsingHeader("Source", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Dummy({0.f, 4.f});
            std::string fname = clip.text.empty() ? "(no file)" : fs::path(clip.text).filename().string();
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted(fname.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && !clip.text.empty()) {
                ImGui::BeginTooltip(); ImGui::TextUnformatted(clip.text.c_str()); ImGui::EndTooltip();
            }
            ImGui::Dummy({0.f, 4.f});
            if (ui_btn("Replace file…", false, true)) {
                std::string np = filepicker_open("Replace audio", "Audio",
                    "*.mp3 *.wav *.flac *.aac *.ogg *.m4a");
                if (!np.empty()) { clip.text = np; history_push(state, "Replace source"); }
            }
            ImGui::Dummy({0.f, 4.f});
        }

        if (ImGui::CollapsingHeader("Playback", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Dummy({0.f, 4.f});
            kf_slider("volume", "Volume", &clip.volume, 0.f, 2.f, "%.2f\xc3\x97");
            kf_interp_bar();
            ImGui::Dummy({0.f, 6.f});
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, Col::fg);
            ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
            ImGui::SetNextItemWidth(w - 16.f);
            ImGui::SliderFloat("Speed##aspd", &clip.speed, 0.25f, 4.f, "%.2f\xc3\x97");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Speed");
            ImGui::PopStyleColor(2);
            ImGui::Dummy({0.f, 4.f});
            struct SP { float f; const char* l; };
            SP spresets[] = {{0.25f,"¼×"},{0.5f,"½×"},{1.f,"1×"},{2.f,"2×"},{4.f,"4×"}};
            for (auto& p : spresets) {
                if (ui_btn(p.l, fabsf(clip.speed - p.f) < 0.01f, true)) { clip.speed = p.f; history_push(state, "Speed"); }
                ImGui::SameLine(0.f, 4.f);
            }
            ImGui::NewLine(); ImGui::Dummy({0.f, 4.f});
        }

        if (ImGui::CollapsingHeader("Fade")) {
            ImGui::Dummy({0.f, 4.f}); section_fade(state, clip, w); ImGui::Dummy({0.f, 4.f});
        }

        if (ImGui::CollapsingHeader("Spatial")) {
            ImGui::Dummy({0.f, 4.f});
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, Col::fg);
            ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
            ImGui::SetNextItemWidth(w - 16.f);
            ImGui::SliderFloat("Pan##apan", &clip.pan, -1.f, 1.f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Pan");
            ImGui::PopStyleColor(2);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextUnformatted("-1 = full left  ·  0 = center  ·  +1 = full right");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 4.f});
        }

        if (ImGui::CollapsingHeader("ML Processing")) {
            ImGui::Dummy({0.f, 4.f});
            bool busy     = transcribe_running();
            bool has_path = !clip.text.empty();
            bool ml_avail = state.models_ready;
            if (!ml_avail && !busy) {
                ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                ImGui::TextWrapped("Models not installed.");
                ImGui::PopStyleColor();
                if (ui_btn("Download Models…", false, true)) state.show_model_dl_modal = true;
                ImGui::Dummy({0.f, 4.f});
            }
            if (!ml_avail) ImGui::BeginDisabled();
            if (busy) {
                float bar_w = w - 16.f;
                ImVec2 bp = ImGui::GetCursorScreenPos();
                ImDrawList* bdl = ImGui::GetWindowDrawList();
                bdl->AddRectFilled(bp, {bp.x+bar_w, bp.y+4.f}, to_u32(Col::line), 2.f);
                bdl->AddRectFilled(bp, {bp.x+bar_w*state.pipeline.progress, bp.y+4.f}, to_u32(Col::fg), 2.f);
                ImGui::Dummy({0.f, 8.f});
                std::string msg = state.pipeline.message.empty() ? "Processing…" : state.pipeline.message;
                char pbuf[128]; snprintf(pbuf, sizeof(pbuf), "%s  %d%%", msg.c_str(), (int)(state.pipeline.progress*100.f));
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted(pbuf); ImGui::PopStyleColor();
                if (!state.pipeline.raw_line.empty()) {
                    std::string raw = state.pipeline.raw_line;
                    if (raw.size() > 100) raw = raw.substr(0, 97) + "...";
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim); ImGui::TextUnformatted(raw.c_str()); ImGui::PopStyleColor();
                }
                ImGui::Dummy({0.f, 4.f});
                if (ui_btn("Cancel", false, true)) transcribe_cancel();
            } else {
                if (!has_path) ImGui::BeginDisabled();
                if (ui_btn("Extract Lyrics", false, true)) kick_pipeline(state, clip.text, PipelineMode::Both);
                ImGui::Dummy({0.f, 2.f});
                if (ui_btn("Extract Subtitles", false, true)) kick_pipeline(state, clip.text, PipelineMode::TranscribeOnly);
                ImGui::Dummy({0.f, 2.f});
                if (ui_btn("Separate Vocals", false, true)) kick_pipeline(state, clip.text, PipelineMode::SeparateOnly);
                if (!has_path) ImGui::EndDisabled();
            }
            if (!ml_avail) ImGui::EndDisabled();
            ImGui::Dummy({0.f, 4.f});
        }
    }
}

// ── Right panel: Lyrics tab ───────────────────────────────────────────────────

static void panel_lyrics(AppState& state, float w) {
    if (state.selected_track < 0 || state.selected_track >= (int)state.tracks.size()) return;
    Track& track = state.tracks[state.selected_track];
    if (state.selected_clip < 0 || state.selected_clip >= (int)track.clips.size()) return;
    Clip& clip = track.clips[state.selected_clip];

    ImGui::Dummy({0.f, 8.f});

    // ── Clip info ─────────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    char tlabel[80];
    snprintf(tlabel, sizeof(tlabel), "%s  ·  clip %d of %d",
        track.name.c_str(), state.selected_clip + 1, (int)track.clips.size());
    ImGui::TextUnformatted(tlabel);
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 4.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

    // Word count and duration from words_cache
    if (!state.words_cache.empty()) {
        int wcount = 0;
        for (auto& we : state.words_cache)
            if (we.end > clip.start && we.start < clip.end) ++wcount;

        float dur = clip.end - clip.start;
        int dur_s = (int)dur, dur_cs = (int)((dur - dur_s) * 100);
        char info[80];
        snprintf(info, sizeof(info), "%d word%s  ·  %d.%02ds",
            wcount, wcount == 1 ? "" : "s", dur_s, dur_cs);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted(info);
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 8.f});
    }

    // ── Grouping mode ─────────────────────────────────────────────────────────
    ui_label("Grouping"); ImGui::Dummy({0.f, 4.f});

    if (!state.words_json_path.empty() && fs::exists(state.words_json_path)) {
        // ── Grouping mode ─────────────────────────────────────────────────────
        // Karaoke is now a per-clip toggle — not a grouping mode.
        struct ModeBtn { SubtitleMode m; const char* label; const char* tip; };
        static const ModeBtn modes[] = {
            {SubtitleMode::Word,    "Word",    "One clip per word"},
            {SubtitleMode::Phrase,  "Phrase",  "Group by short pauses (>0.3s)"},
            {SubtitleMode::Line,    "Line",    "Group by breath gaps (>0.8s)"},
            {SubtitleMode::Segment, "Segment", "WhisperX sentence boundaries"},
            {SubtitleMode::CustomN, "Custom",  "N words per clip"},
        };
        for (auto& mb : modes) {
            bool sel = (state.subtitle_mode == mb.m ||
                       (mb.m == SubtitleMode::Line && state.subtitle_mode == SubtitleMode::Karaoke));
            if (ui_btn(mb.label, sel, true)) state.subtitle_mode = mb.m;
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip(); ImGui::TextUnformatted(mb.tip); ImGui::EndTooltip();
            }
            ImGui::SameLine(0.f, 4.f);
        }
        ImGui::NewLine();

        if (state.subtitle_mode == SubtitleMode::CustomN) {
            ImGui::Dummy({0.f, 4.f});
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
            ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
            ImGui::SetNextItemWidth(80.f);
            int n = state.subtitle_n;
            if (ImGui::InputInt("words/clip##cnl", &n))
                state.subtitle_n = (n < 1) ? 1 : (n > 20) ? 20 : n;
            ImGui::PopStyleColor(2);
        }

        // ── Karaoke toggle (not available for Word mode) ───────────────────
        if (state.subtitle_mode != SubtitleMode::Word) {
            ImGui::Dummy({0.f, 8.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
            ui_label("Karaoke"); ImGui::Dummy({0.f, 4.f});

            bool kar = clip.karaoke;
            if (ImGui::Checkbox("Karaoke highlight##ltab", &kar)) {
                clip.karaoke = kar;
                history_push(state, "Karaoke toggle");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Highlight each word as it plays");
                ImGui::EndTooltip();
            }

            if (clip.karaoke) {
                ImGui::Dummy({0.f, 6.f});
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                ImGui::TextUnformatted("Base color"); ImGui::PopStyleColor();
                ImGui::SetNextItemWidth(w - 16.f);
                if (ImGui::ColorEdit4("##kar_base", clip.sub_color,
                        ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar))
                    if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Karaoke base color");
                ImGui::Dummy({0.f, 4.f});
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                ImGui::TextUnformatted("Highlight color"); ImGui::PopStyleColor();
                ImGui::SetNextItemWidth(w - 16.f);
                if (ImGui::ColorEdit4("##kar_hl", clip.karaoke_highlight_color,
                        ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar))
                    if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Karaoke highlight color");

                ImGui::Dummy({0.f, 6.f});
                if (ui_btn("Apply karaoke colors to all", false, true)) {
                    for (auto& tr : state.tracks)
                        for (auto& cl2 : tr.clips)
                            if (cl2.clip_type == ClipType::Lyrics && &cl2 != &clip) {
                                memcpy(cl2.sub_color, clip.sub_color, sizeof(clip.sub_color));
                                memcpy(cl2.karaoke_highlight_color, clip.karaoke_highlight_color,
                                       sizeof(clip.karaoke_highlight_color));
                            }
                    history_push(state, "Apply karaoke colors to all");
                }
            }
        }

        ImGui::Dummy({0.f, 8.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        if (ui_btn("Apply grouping", true, true)) {
            apply_subtitle_mode(state);
            const char* mname =
                state.subtitle_mode == SubtitleMode::Word    ? "Word"    :
                state.subtitle_mode == SubtitleMode::Phrase  ? "Phrase"  :
                state.subtitle_mode == SubtitleMode::Line    ? "Line"    :
                state.subtitle_mode == SubtitleMode::Karaoke ? "Karaoke" :
                state.subtitle_mode == SubtitleMode::Segment ? "Segment" : "Custom";
            history_push(state, std::string("Grouping — ") + mname);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Re-bucket all Lyrics clips from saved word JSON");
            ImGui::EndTooltip();
        }

        ImGui::Dummy({0.f, 4.f});
        if (ui_btn("Apply to all Lyrics", false, true)) {
            for (auto& tr : state.tracks)
                for (auto& cl2 : tr.clips)
                    if (cl2.clip_type == ClipType::Lyrics)
                        cl2.karaoke = clip.karaoke;
            history_push(state, "Apply karaoke to all Lyrics");
        }

        int sel_lyrics = 0;
        for (auto& [st, sc] : state.clip_selection) {
            if (st == state.selected_track && sc == state.selected_clip) continue;
            if (st < (int)state.tracks.size() && sc < (int)state.tracks[st].clips.size() &&
                state.tracks[st].clips[sc].clip_type == ClipType::Lyrics)
                ++sel_lyrics;
        }
        if (sel_lyrics > 0) {
            ImGui::SameLine(0.f, 6.f);
            char glbl[56];
            snprintf(glbl, sizeof(glbl), "Apply to %d selected##grp", sel_lyrics);
            if (ui_btn(glbl, false, true)) {
                for (auto& [st, sc] : state.clip_selection) {
                    if (st == state.selected_track && sc == state.selected_clip) continue;
                    if (st >= (int)state.tracks.size() || sc >= (int)state.tracks[st].clips.size()) continue;
                    Clip& tgt = state.tracks[st].clips[sc];
                    if (tgt.clip_type != ClipType::Lyrics) continue;
                    tgt.karaoke = clip.karaoke;
                }
                history_push(state, "Apply karaoke to selected");
            }
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextWrapped("Run ML Processing on an audio clip to generate word JSON.");
        ImGui::PopStyleColor();
    }
}

// ── FX preset helpers ─────────────────────────────────────────────────────────

static void preset_apply(Clip& clip, const EffectPreset& p) {
    clip.fx_color_on    = p.fx_color_on;
    clip.fx_brightness  = p.fx_brightness;
    clip.fx_contrast    = p.fx_contrast;
    clip.fx_saturation  = p.fx_saturation;
    clip.fx_hue         = p.fx_hue;
    clip.fx_blur_on     = p.fx_blur_on;
    clip.fx_blur        = p.fx_blur;
    clip.fx_vignette_on = p.fx_vignette_on;
    clip.fx_vignette    = p.fx_vignette;
    clip.fx_text_on     = p.fx_text_on;
    clip.fx_opacity_mul = p.fx_opacity_mul;
    clip.fx_scale_mul   = p.fx_scale_mul;
}

static EffectPreset preset_from_clip(const Clip& clip, const std::string& name) {
    EffectPreset p;
    p.name           = name;
    p.category       = PresetCategory::User;
    p.fx_color_on    = clip.fx_color_on;
    p.fx_brightness  = clip.fx_brightness;
    p.fx_contrast    = clip.fx_contrast;
    p.fx_saturation  = clip.fx_saturation;
    p.fx_hue         = clip.fx_hue;
    p.fx_blur_on     = clip.fx_blur_on;
    p.fx_blur        = clip.fx_blur;
    p.fx_vignette_on = clip.fx_vignette_on;
    p.fx_vignette    = clip.fx_vignette;
    p.fx_text_on     = clip.fx_text_on;
    p.fx_opacity_mul = clip.fx_opacity_mul;
    p.fx_scale_mul   = clip.fx_scale_mul;
    return p;
}

// Color swatch for a preset card — derived from its dominant effect.
static ImU32 preset_swatch(const EffectPreset& p) {
    if (p.fx_blur_on  && !p.fx_color_on && !p.fx_vignette_on)
        return IM_COL32(80, 140, 220, 200);   // blue-ish for blur
    if (p.fx_vignette_on && !p.fx_color_on && !p.fx_blur_on)
        return IM_COL32(40, 40, 60, 220);     // near-black for vignette
    if (!p.fx_color_on)
        return IM_COL32(90, 90, 110, 200);    // grey for mixed/none
    // Rough tint from hue + saturation
    float h = fmodf(p.fx_hue + 360.f, 360.f);
    float s = p.fx_saturation;
    if (s < 0.05f) return IM_COL32(110, 110, 110, 220);  // desaturated
    int hi = (int)(h / 60.f) % 6;
    static const ImU32 hue_cols[6] = {
        IM_COL32(220,80,80,200), IM_COL32(200,160,60,200), IM_COL32(100,200,70,200),
        IM_COL32(60,180,200,200), IM_COL32(80,100,220,200), IM_COL32(180,70,200,200)
    };
    return hue_cols[hi];
}

// ── Right panel: FX Library tab ──────────────────────────────────────────────

static void panel_fx_library(AppState& state, float w) {
    ImGui::Dummy({0.f, 8.f});

    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200,160,255,255));
    ImGui::TextUnformatted("FX Library");
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
    ImGui::TextWrapped("Click to apply to selected Effect clip. Drag to timeline to create one.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 8.f});

    // Determine if an Effect clip is currently selected (for click-to-apply).
    bool has_fx_clip = (state.selected_track >= 0 &&
                        state.selected_track < (int)state.tracks.size() &&
                        state.selected_clip  >= 0 &&
                        state.selected_clip  < (int)state.tracks[state.selected_track].clips.size() &&
                        state.tracks[state.selected_track].clips[state.selected_clip].clip_type == ClipType::Effect);

    float card_w  = (w - 12.f) * 0.5f;  // 2-column grid
    float card_h  = 54.f;
    float swatch_w = 8.f;

    auto draw_preset_card = [&](const EffectPreset& p, int unique_id) {
        ImGui::PushID(unique_id);
        ImVec2 cp = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        bool hov = ImGui::IsMouseHoveringRect(cp, {cp.x + card_w, cp.y + card_h});
        ImU32 bg  = hov ? to_u32(Col::bg_soft_hov) : to_u32(Col::bg_soft);
        ImU32 brd = hov ? IM_COL32(180,130,255,180) : to_u32(Col::line);

        dl->AddRectFilled(cp, {cp.x+card_w, cp.y+card_h}, bg, 4.f);
        dl->AddRect(cp, {cp.x+card_w, cp.y+card_h}, brd, 4.f);
        // Left color swatch strip
        dl->AddRectFilled(cp, {cp.x+swatch_w, cp.y+card_h}, preset_swatch(p), 4.f);

        // Name + badge line
        float tx = cp.x + swatch_w + 8.f;
        float ty = cp.y + 10.f;
        ImGui::PushFont(g_font_bold);
        dl->AddText(ImGui::GetFont(), 13.f, {tx, ty}, IM_COL32(230,230,230,255), p.name.c_str());
        ImGui::PopFont();

        // Mini badges for active effects
        float bx = tx, by = cp.y + 30.f;
        auto badge = [&](const char* lbl, ImU32 col) {
            ImVec2 ts = ImGui::CalcTextSize(lbl);
            float bw = ts.x + 8.f, bh = ts.y + 4.f;
            dl->AddRectFilled({bx,by},{bx+bw,by+bh}, col, 3.f);
            dl->AddText({bx+4.f,by+2.f}, IM_COL32(255,255,255,220), lbl);
            bx += bw + 4.f;
        };
        if (p.fx_color_on)    badge("Col", IM_COL32(80,140,220,180));
        if (p.fx_blur_on)     badge("Blur",IM_COL32(60,120,180,180));
        if (p.fx_vignette_on) badge("Vig", IM_COL32(60,60,90,200));
        if (p.fx_text_on)     badge("Txt", IM_COL32(100,80,160,200));

        // Invisible button over the card for click and drag-drop
        ImGui::SetCursorScreenPos(cp);
        ImGui::InvisibleButton("##card", {card_w, card_h});
        bool clicked = ImGui::IsItemClicked(0);

        // Drag-drop source — payload is index into the combined preset list
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload("FX_PRESET", &unique_id, sizeof(int));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200,160,255,255));
            ImGui::TextUnformatted(p.name.c_str());
            ImGui::PopStyleColor();
            ImGui::TextUnformatted("Drop onto timeline track");
            ImGui::EndDragDropSource();
        }

        ImGui::PopID();

        if (clicked) {
            if (has_fx_clip) {
                preset_apply(state.tracks[state.selected_track].clips[state.selected_clip], p);
                history_push(state, "Apply preset: " + p.name);
            } else {
                // Find topmost track or create one, add Effect clip at playhead
                if (state.tracks.empty()) {
                    Track t; t.name = "FX"; state.tracks.push_back(t);
                }
                Clip cl;
                cl.clip_type = ClipType::Effect;
                cl.start     = state.playhead;
                cl.end       = state.playhead + 2.f;
                preset_apply(cl, p);
                state.tracks[0].clips.push_back(cl);
                state.selected_track = 0;
                state.selected_clip  = (int)state.tracks[0].clips.size() - 1;
                history_push(state, "Add Effect: " + p.name);
            }
        }
    };

    // Combined preset list: built-ins first, then user presets.
    // Index 0..N-1 = built-ins, N.. = user.
    int builtin_count = (int)g_builtin_presets.size();

    auto draw_section = [&](const char* label, PresetCategory cat) {
        ui_separator();
        ImGui::Dummy({0.f, 4.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 4.f});

        bool col_left = true;
        float base_x = ImGui::GetCursorScreenPos().x;
        ImVec2 left_cursor = ImGui::GetCursorPos();

        for (int i = 0; i < (int)g_builtin_presets.size(); ++i) {
            if (g_builtin_presets[i].category != cat) continue;
            if (!col_left) {
                ImGui::SameLine(0.f, 8.f);
            }
            draw_preset_card(g_builtin_presets[i], i);
            if (!col_left) ImGui::Dummy({0.f, 4.f});
            col_left = !col_left;
        }
        if (!col_left) ImGui::Dummy({0.f, 4.f});
        ImGui::Dummy({0.f, 4.f});
        (void)base_x; (void)left_cursor;
    };

    draw_section("Color",    PresetCategory::Color);
    draw_section("Blur",     PresetCategory::Blur);
    draw_section("Vignette", PresetCategory::Vignette);
    draw_section("Combo",    PresetCategory::Combo);

    // User presets section
    if (!state.user_presets.empty()) {
        ui_separator();
        ImGui::Dummy({0.f, 4.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted("My Presets");
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 4.f});

        bool col_left = true;
        for (int i = 0; i < (int)state.user_presets.size(); ++i) {
            if (!col_left) ImGui::SameLine(0.f, 8.f);
            draw_preset_card(state.user_presets[i], builtin_count + i);

            // Right-click to delete user preset
            if (ImGui::BeginPopupContextItem(("##userpctx" + std::to_string(i)).c_str())) {
                if (ImGui::MenuItem("Delete preset")) {
                    state.user_presets.erase(state.user_presets.begin() + i);
                    presets_save_user(state.user_presets);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (!col_left) ImGui::Dummy({0.f, 4.f});
            col_left = !col_left;
        }
        if (!col_left) ImGui::Dummy({0.f, 4.f});
    }

    ImGui::Dummy({0.f, 16.f});
}

// ── Right panel: Effect tab ───────────────────────────────────────────────────

static void panel_effect(AppState& state, float w) {
    if (state.selected_track < 0 || state.selected_track >= (int)state.tracks.size()) return;
    Track& track = state.tracks[state.selected_track];
    if (state.selected_clip < 0 || state.selected_clip >= (int)track.clips.size()) return;
    Clip& clip = track.clips[state.selected_clip];

    ImGui::Dummy({0.f, 8.f});

    // Header
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200,160,255,255));
    ImGui::TextUnformatted("Effect Clip");
    ImGui::PopStyleColor();
    int n_below = (int)state.tracks.size() - state.selected_track - 1;
    char info[128];
    snprintf(info, sizeof(info), "%s  ·  %.2fs – %.2fs  ·  %d track%s below",
        track.name.c_str(), clip.start, clip.end, n_below, n_below==1?"":"s");
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextWrapped("%s", info);
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 4.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

    ImGui::PushStyleColor(ImGuiCol_SliderGrab,      IM_COL32(180,130,255,255));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,IM_COL32(210,170,255,255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,         Col::bg_soft);
    ImGui::PushStyleColor(ImGuiCol_CheckMark,       IM_COL32(180,130,255,255));

    auto fx_reset_btn = [&](const char* id, float* v, float def) {
        ImGui::SameLine(0.f, 6.f);
        char lbl[16]; snprintf(lbl, sizeof(lbl), "R##%s", id);
        if (ui_btn(lbl, false, true)) { *v = def; history_push(state, "Effect: reset"); }
    };
    float sw = w - 72.f;  // slider width leaving room for label + reset

    // ── Color Grade ───────────────────────────────────────────────────────────
    bool cg = clip.fx_color_on;
    if (ImGui::Checkbox("Color Grade##fx", &cg)) {
        clip.fx_color_on = cg;
        history_push(state, "Effect: color grade");
    }
    if (clip.fx_color_on) {
        ImGui::Dummy({0.f, 4.f});
        ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Brightness##fx",&clip.fx_brightness,-1.f,1.f,"%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Effect: brightness");
        fx_reset_btn("br", &clip.fx_brightness, 0.f);

        ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Contrast##fx",  &clip.fx_contrast, 0.5f,2.f,"%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Effect: contrast");
        fx_reset_btn("co", &clip.fx_contrast, 1.f);

        ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Saturation##fx",&clip.fx_saturation,0.f,2.f,"%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Effect: saturation");
        fx_reset_btn("sa", &clip.fx_saturation, 1.f);

        ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Hue##fx",       &clip.fx_hue,-180.f,180.f,"%.0f\xc2\xb0");
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Effect: hue");
        fx_reset_btn("hu", &clip.fx_hue, 0.f);
        ImGui::Dummy({0.f, 4.f});
    }

    ImGui::Dummy({0.f, 4.f}); ui_separator(); ImGui::Dummy({0.f, 4.f});

    // ── Blur ──────────────────────────────────────────────────────────────────
    bool bl = clip.fx_blur_on;
    if (ImGui::Checkbox("Blur##fx", &bl)) {
        clip.fx_blur_on = bl;
        history_push(state, "Effect: blur");
    }
    if (clip.fx_blur_on) {
        ImGui::Dummy({0.f, 4.f});
        ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Radius##fx", &clip.fx_blur, 0.f, 20.f, "%.1f px");
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Effect: blur radius");
        fx_reset_btn("bl", &clip.fx_blur, 0.f);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
        ImGui::TextUnformatted("Live preview shows badge — rendered on export");
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 4.f});
    }

    ImGui::Dummy({0.f, 4.f}); ui_separator(); ImGui::Dummy({0.f, 4.f});

    // ── Vignette ──────────────────────────────────────────────────────────────
    bool vi = clip.fx_vignette_on;
    if (ImGui::Checkbox("Vignette##fx", &vi)) {
        clip.fx_vignette_on = vi;
        history_push(state, "Effect: vignette");
    }
    if (clip.fx_vignette_on) {
        ImGui::Dummy({0.f, 4.f});
        ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Strength##fx", &clip.fx_vignette, 0.f, 1.f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Effect: vignette strength");
        fx_reset_btn("vi", &clip.fx_vignette, 0.f);
        ImGui::Dummy({0.f, 4.f});
    }

    ImGui::Dummy({0.f, 4.f}); ui_separator(); ImGui::Dummy({0.f, 4.f});

    // ── Text Overrides ────────────────────────────────────────────────────────
    bool tx = clip.fx_text_on;
    if (ImGui::Checkbox("Text Overrides##fx", &tx)) {
        clip.fx_text_on = tx;
        history_push(state, "Effect: text overrides");
    }
    if (clip.fx_text_on) {
        ImGui::Dummy({0.f, 4.f});
        ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Opacity mul##fx",&clip.fx_opacity_mul,0.f,1.f,"%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Effect: opacity mul");
        fx_reset_btn("op", &clip.fx_opacity_mul, 1.f);

        ImGui::SetNextItemWidth(sw); ImGui::SliderFloat("Scale mul##fx",  &clip.fx_scale_mul,0.5f,2.f,"%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Effect: scale mul");
        fx_reset_btn("sc", &clip.fx_scale_mul, 1.f);
        ImGui::Dummy({0.f, 4.f});
    }

    ImGui::PopStyleColor(4);

    ImGui::Dummy({0.f, 8.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
    ImGui::TextWrapped("Effects stack — multiple Effect clips at the same time compound (brightness adds, contrast/saturation multiply, blur adds).");
    ImGui::PopStyleColor();

    ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

    // ── Save as Preset ────────────────────────────────────────────────────────
    static char s_preset_name[64] = {};
    static bool s_naming = false;
    if (!s_naming) {
        if (ui_btn("Save as Preset", false, true)) {
            s_naming = true;
            s_preset_name[0] = '\0';
        }
    } else {
        ImGui::SetNextItemWidth(w - 80.f);
        bool enter = ImGui::InputText("##pname", s_preset_name, sizeof(s_preset_name),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ui_btn("Save", true, true) || enter) {
            if (s_preset_name[0] != '\0') {
                state.user_presets.push_back(preset_from_clip(clip, s_preset_name));
                presets_save_user(state.user_presets);
            }
            s_naming = false;
        }
        ImGui::SameLine();
        if (ui_btn("X", false, true)) s_naming = false;
    }

    ImGui::Dummy({0.f, 4.f});
    if (ui_btn("Delete clip", false, true)) {
        track.clips.erase(track.clips.begin() + state.selected_clip);
        state.selected_clip = -1;
        history_push(state, "Delete effect clip");
    }
}

// ── Right panel: Animation tab ───────────────────────────────────────────────

static const struct { AnimStyle style; const char* name; const char* desc; const char* tag; } STYLES[] = {
    {AnimStyle::Fade,       "Fade",       "Opacity in/out — clean",         "soft"  },
    {AnimStyle::Glitch,     "Glitch",     "Digital artefact — corrupt",     "glitch"},
    {AnimStyle::Typewriter, "Typewriter", "Character-by-character reveal",  "retro" },
    {AnimStyle::Bounce,     "Bounce",     "Drops in with spring overshoot", "lively"},
    {AnimStyle::Scale,      "Scale",      "Punches in from small — zoom",   "punchy"},
    {AnimStyle::Slide,      "Slide",      "Enters left, exits right",       "motion"},
    {AnimStyle::Stack,      "Stack",      "Lines pile, prev dims",          "dense" },
    {AnimStyle::Block,      "Block",      "White fill — high contrast",     "sharp" },
};

static void panel_animation(AppState& state, float w) {
    ImGui::Dummy({0.f, 8.f});

    // Resolve focused clip (single selection or primary)
    Clip* focused_clip = nullptr;
    if (state.selected_track >= 0 && state.selected_clip >= 0 &&
        state.selected_track < (int)state.tracks.size() &&
        state.selected_clip  < (int)state.tracks[state.selected_track].clips.size()) {
        focused_clip = &state.tracks[state.selected_track].clips[state.selected_clip];
    }

    // The "active" style shown in cards: clip override if set, else project default
    AnimStyle active_style = focused_clip
        ? (focused_clip->clip_style != AnimStyle::None ? focused_clip->clip_style : state.style)
        : state.style;
    bool clip_has_override = focused_clip && focused_clip->clip_style != AnimStyle::None;

    ui_label("Animation style");
    if (focused_clip) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted(clip_has_override ? " (clip)" : " (project)");
        ImGui::PopStyleColor();
    }
    ImGui::Dummy({0.f, 8.f});

    float card_w = (w - 20.f) * 0.5f;
    float card_h = 82.f;

    for (int i = 0; i < 8; ++i) {
        if (i % 2 == 1) ImGui::SameLine(0.f, 8.f);
        const auto& sc = STYLES[i];
        bool sel = active_style == sc.style;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, sel ? Col::bg_soft_hov : Col::bg_soft);
        ImGui::PushStyleColor(ImGuiCol_Border,  sel ? Col::fg : Col::line);
        char cid[16]; snprintf(cid, sizeof(cid), "##sc%d", i);
        if (ImGui::BeginChild(cid, {card_w, card_h}, ImGuiChildFlags_Borders)) {
            float t     = (float)ImGui::GetTime();
            float phase = fmodf(t, 2.f) / 2.f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 pp = ImGui::GetCursorScreenPos();
            float ph = 36.f, pw = card_w - 16.f;
            pp.x += 8.f;
            dl->AddRectFilled(pp, {pp.x+pw, pp.y+ph}, to_u32(Col::accent_dark), 2.f);

            ImU32 txt_col = to_u32(Col::fg);
            ImVec2 rpp = pp;
            switch (sc.style) {
                case AnimStyle::Fade:
                    txt_col = ImGui::ColorConvertFloat4ToU32({1,1,1, phase<0.5f?phase*2.f:1.f-(phase-0.5f)*2.f});
                    break;
                case AnimStyle::Glitch:
                    rpp.x += sinf(t*40.f)*3.f*phase; break;
                case AnimStyle::Bounce:
                    rpp.y += phase<0.3f?(0.3f-phase)/0.3f*8.f:0.f; break;
                case AnimStyle::Slide:
                    rpp.x += phase<0.3f?(0.3f-phase)/0.3f*-20.f:phase>0.7f?(phase-0.7f)/0.3f*20.f:0.f; break;
                case AnimStyle::Block: {
                    float bw=pw*0.5f;
                    dl->AddRectFilled({pp.x+(pw-bw)*0.5f,pp.y+ph*0.2f},{pp.x+(pw+bw)*0.5f,pp.y+ph*0.8f},to_u32(Col::fg),2.f);
                    txt_col=to_u32(Col::bg); break;
                }
                default: break;
            }
            ImVec2 tsz = ImGui::CalcTextSize(sc.name);
            dl->AddText({rpp.x+(pw-tsz.x)*0.5f, rpp.y+(ph-tsz.y)*0.5f}, txt_col, sc.name);
            ImGui::Dummy({0.f, ph+4.f});
            ImGui::SetCursorPosX(8.f);
            ImGui::PushStyleColor(ImGuiCol_Text, sel ? Col::fg : Col::muted);
            ImGui::TextUnformatted(sc.name);
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        if (ImGui::IsItemClicked()) {
            if (focused_clip) {
                focused_clip->clip_style = sc.style;
            } else {
                state.style = sc.style;
            }
            history_push(state, std::string("Style — ") + sc.name);
        }
        ImGui::PopStyleColor(2);
        if (i % 2 == 1 && i < 7) ImGui::Dummy({0.f, 4.f});
    }

    // Apply / reset row
    ImGui::Dummy({0.f, 8.f});
    if (focused_clip) {
        if (clip_has_override && ui_btn("Use project default", false, false)) {
            focused_clip->clip_style = AnimStyle::None;
            history_push(state, "Reset clip style");
        }
        if (clip_has_override) ImGui::SameLine(0.f, 6.f);
    }

    // Apply to selected clips
    int n_sel = (int)state.clip_selection.size();
    if (n_sel > 1) {
        char slbl[48]; snprintf(slbl, sizeof(slbl), "Apply to %d selected##anim", n_sel);
        if (ui_btn(slbl, false, false)) {
            for (auto& [ti, ci] : state.clip_selection) {
                if (ti < (int)state.tracks.size() && ci < (int)state.tracks[ti].clips.size())
                    state.tracks[ti].clips[ci].clip_style = active_style;
            }
            history_push(state, "Style — apply to selected");
        }
        ImGui::SameLine(0.f, 6.f);
    }

    // Apply to all text/lyrics clips
    if (ui_btn("Apply to all##anim", false, false)) {
        for (auto& tr : state.tracks)
            for (auto& cl : tr.clips)
                if (cl.clip_type == ClipType::Text || cl.clip_type == ClipType::Lyrics ||
                    cl.clip_type == ClipType::Subtitle)
                    cl.clip_style = active_style;
        // Also set as project default
        state.style = active_style;
        history_push(state, std::string("Style — apply all — ") + STYLES[0].name);
    }

    ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
    ui_label("Font weight"); ImGui::Dummy({0.f, 6.f});
    for (int fw : {400, 700, 900}) {
        char wl[8]; snprintf(wl, sizeof(wl), "%d", fw);
        if (ui_btn(wl, state.font_weight == fw, true)) {
            state.font_weight = fw;
            history_push(state, "Font weight");
        }
        ImGui::SameLine(0.f, 4.f);
    }

    // ── Beat sync ─────────────────────────────────────────────────────────────
    ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
    ui_label("Beat sync"); ImGui::Dummy({0.f, 6.f});

    if (state.beats_running) {
        ImGui::ProgressBar(-1.f * (float)ImGui::GetTime(), {-1.f, 6.f}, "");
        ImGui::Dummy({0.f, 4.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted("Detecting beats…");
        ImGui::PopStyleColor();
    } else if (!state.beats.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        char bpmbuf[32];
        snprintf(bpmbuf, sizeof(bpmbuf), "%.1f BPM  ·  %d beats", state.beat_bpm, (int)state.beats.size());
        ImGui::TextUnformatted(bpmbuf);
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 4.f});
        if (ImGui::Button("Re-detect beats")) run_beat_detect(state);
        ImGui::SameLine(0.f, 8.f);
        // "Pulse on beats" — adds scale keyframes at every beat for selected clips
        bool has_lyrics_sel = false;
        for (auto& [ti, ci] : state.clip_selection) {
            if (ti >= 0 && ti < (int)state.tracks.size() &&
                ci >= 0 && ci < (int)state.tracks[ti].clips.size() &&
                (state.tracks[ti].clips[ci].clip_type == ClipType::Lyrics ||
                 state.tracks[ti].clips[ci].clip_type == ClipType::Text)) {
                has_lyrics_sel = true; break;
            }
        }
        if (!has_lyrics_sel && state.selected_track >= 0 && state.selected_clip >= 0) {
            int sti = state.selected_track, sci = state.selected_clip;
            if (sti < (int)state.tracks.size() && sci < (int)state.tracks[sti].clips.size())
                has_lyrics_sel = true;
        }
        ImGui::BeginDisabled(!has_lyrics_sel);
        if (ImGui::Button("Pulse on beats")) {
            auto pulse_clip = [&](Clip& cl) {
                for (float bt : state.beats) {
                    float rel = bt - cl.start;
                    if (rel < 0.f || rel > cl.end - cl.start) continue;
                    cl.ktracks["scale_x"].set(rel, 1.12f, InterpType::EaseOut);
                    cl.ktracks["scale_y"].set(rel, 1.12f, InterpType::EaseOut);
                    float decay = fminf(0.18f, (cl.end - cl.start - rel));
                    cl.ktracks["scale_x"].set(rel + decay, 1.f, InterpType::EaseIn);
                    cl.ktracks["scale_y"].set(rel + decay, 1.f, InterpType::EaseIn);
                }
            };
            if (!state.clip_selection.empty()) {
                for (auto& [ti, ci] : state.clip_selection) {
                    if (ti < (int)state.tracks.size() && ci < (int)state.tracks[ti].clips.size())
                        pulse_clip(state.tracks[ti].clips[ci]);
                }
            } else if (state.selected_track >= 0 && state.selected_clip >= 0) {
                int sti = state.selected_track, sci = state.selected_clip;
                if (sti < (int)state.tracks.size() && sci < (int)state.tracks[sti].clips.size())
                    pulse_clip(state.tracks[sti].clips[sci]);
            }
            history_push(state, "Pulse on beats");
        }
        ImGui::EndDisabled();
    } else {
        bool no_src = state.audio_path.empty() && state.vocals_path.empty();
        ImGui::BeginDisabled(no_src);
        if (ImGui::Button("Detect beats")) run_beat_detect(state);
        ImGui::EndDisabled();
        if (no_src) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted("(import audio first)");
            ImGui::PopStyleColor();
        }
    }

    // ── Audio-reactive envelope ───────────────────────────────────────────────
    ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
    ui_label("Audio-reactive"); ImGui::Dummy({0.f, 6.f});

    if (state.envelope_running) {
        ImGui::ProgressBar(-1.f * (float)ImGui::GetTime(), {-1.f, 6.f}, "");
        ImGui::Dummy({0.f, 4.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted("Analysing amplitude…");
        ImGui::PopStyleColor();
    } else if (!state.amplitude_envelope.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        char ebuf[48];
        snprintf(ebuf, sizeof(ebuf), "%.1f fps  ·  %d frames", state.envelope_fps, (int)state.amplitude_envelope.size());
        ImGui::TextUnformatted(ebuf);
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 4.f});

        static const char* env_props[] = { "scale_x+y", "opacity", "scale_x", "scale_y" };
        static int env_prop_idx = 0;
        ImGui::SetNextItemWidth(w - 16.f);
        ImGui::Combo("##envprop", &env_prop_idx, env_props, 4);
        ImGui::Dummy({0.f, 4.f});
        static float env_min = 0.8f, env_max = 1.2f;
        ImGui::SetNextItemWidth((w - 16.f) * 0.5f - 4.f);
        ImGui::DragFloat("##emin", &env_min, 0.01f, 0.f, 2.f, "min %.2f");
        ImGui::SameLine(0.f, 4.f);
        ImGui::SetNextItemWidth((w - 16.f) * 0.5f - 4.f);
        ImGui::DragFloat("##emax", &env_max, 0.01f, 0.f, 4.f, "max %.2f");
        ImGui::Dummy({0.f, 4.f});

        bool has_sel2 = !state.clip_selection.empty() ||
                        (state.selected_track >= 0 && state.selected_clip >= 0);
        ImGui::BeginDisabled(!has_sel2);
        if (ImGui::Button("Bake to keyframes")) {
            auto bake_clip = [&](Clip& cl) {
                // Sample local maxima from the envelope and write keyframes
                float dur_cl = cl.end - cl.start;
                if (dur_cl <= 0.f || state.envelope_fps <= 0.f) return;
                float dt = 1.f / state.envelope_fps;
                int n = (int)state.amplitude_envelope.size();
                for (int fi = 1; fi < n - 1; ++fi) {
                    float v = state.amplitude_envelope[fi];
                    if (v < state.amplitude_envelope[fi-1] || v < state.amplitude_envelope[fi+1]) continue;
                    float abs_t = fi * dt;
                    float rel   = abs_t - cl.start;
                    if (rel < 0.f || rel > dur_cl) continue;
                    float mapped = env_min + v * (env_max - env_min);
                    if (env_prop_idx == 0 || env_prop_idx == 2) cl.ktracks["scale_x"].set(rel, mapped);
                    if (env_prop_idx == 0 || env_prop_idx == 3) cl.ktracks["scale_y"].set(rel, mapped);
                    if (env_prop_idx == 1) cl.ktracks["opacity"].set(rel, mapped);
                }
            };
            if (!state.clip_selection.empty()) {
                for (auto& [ti, ci] : state.clip_selection) {
                    if (ti < (int)state.tracks.size() && ci < (int)state.tracks[ti].clips.size())
                        bake_clip(state.tracks[ti].clips[ci]);
                }
            } else if (state.selected_track >= 0 && state.selected_clip >= 0) {
                int sti = state.selected_track, sci = state.selected_clip;
                if (sti < (int)state.tracks.size() && sci < (int)state.tracks[sti].clips.size())
                    bake_clip(state.tracks[sti].clips[sci]);
            }
            history_push(state, "Bake envelope to keyframes");
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0.f, 8.f);
        if (ImGui::Button("Re-analyse##env")) run_envelope_extract(state);
    } else {
        bool no_src2 = state.audio_path.empty() && state.vocals_path.empty();
        ImGui::BeginDisabled(no_src2);
        if (ImGui::Button("Analyse amplitude")) run_envelope_extract(state);
        ImGui::EndDisabled();
        if (no_src2) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted("(import audio first)");
            ImGui::PopStyleColor();
        }
    }
}

// ── Right panel: Project tab ──────────────────────────────────────────────────

static void panel_project(AppState& state, float w) {
    ImGui::Dummy({0.f, 8.f});
    ui_label("Frame rate"); ImGui::Dummy({0.f, 6.f});
    for (int f : {24, 30, 60}) {
        char lbl[8]; snprintf(lbl, sizeof(lbl), "%d fps", f);
        if (ui_btn(lbl, state.fps == f, true)) state.fps = f;
        ImGui::SameLine(0.f, 4.f);
    }
    ImGui::NewLine();
    ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

    ui_label("Output format"); ImGui::Dummy({0.f, 8.f});
    struct Fmt { OutputFormat fmt; const char* name; const char* ratio; float sw, sh; };
    Fmt fmts[] = {
        {OutputFormat::Vertical,   "TikTok / Reels", "9:16", 24.f, 42.f},
        {OutputFormat::Horizontal, "YouTube",        "16:9", 54.f, 30.f},
        {OutputFormat::Square,     "Instagram",      "1:1",  36.f, 36.f},
    };
    float fw = (w - 16.f) / 3.f;
    for (int i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine(0.f, 4.f);
        bool sel = state.format == fmts[i].fmt;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, sel ? Col::bg_soft_hov : Col::bg_soft);
        ImGui::PushStyleColor(ImGuiCol_Border,  sel ? Col::fg : Col::line);
        char fid[8]; snprintf(fid, sizeof(fid), "##pf%d", i);
        if (ImGui::BeginChild(fid, {fw, 80.f}, ImGuiChildFlags_Borders)) {
            ImVec2 sp = ImGui::GetCursorScreenPos();
            float cx = (fw - fmts[i].sw) * 0.5f;
            ImGui::GetWindowDrawList()->AddRect(
                {sp.x+cx, sp.y+4.f}, {sp.x+cx+fmts[i].sw, sp.y+4.f+fmts[i].sh},
                to_u32(sel ? Col::fg : Col::line), 2.f);
            ImVec2 rsz = ImGui::CalcTextSize(fmts[i].ratio);
            ImGui::GetWindowDrawList()->AddText(
                {sp.x+cx+(fmts[i].sw-rsz.x)*0.5f, sp.y+4.f+(fmts[i].sh-rsz.y)*0.5f},
                to_u32(sel ? Col::fg : Col::muted), fmts[i].ratio);
            ImGui::Dummy({0.f, fmts[i].sh+8.f});
            ImGui::SetCursorPosX(4.f);
            ImGui::PushStyleColor(ImGuiCol_Text, sel ? Col::fg : Col::muted);
            ImGui::TextUnformatted(fmts[i].name);
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        if (ImGui::IsItemClicked()) {
            state.format = fmts[i].fmt;
            history_push(state, std::string("Format — ") + fmts[i].name);
        }
        ImGui::PopStyleColor(2);
    }
}

// ── Right panel: Export tab ───────────────────────────────────────────────────

static void panel_export(AppState& state, float w) {
    ImGui::Dummy({0.f, 8.f});

    // Advanced settings (collapsible)
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    bool adv_open = ImGui::TreeNodeEx("Advanced##adv",
        ImGuiTreeNodeFlags_SpanFullWidth |
        (state.render_settings.advanced_open ? ImGuiTreeNodeFlags_DefaultOpen : 0));
    ImGui::PopStyleColor();
    state.render_settings.advanced_open = adv_open;
    if (adv_open) {
        ImGui::Dummy({0.f, 6.f});

        // CRF
        ui_label("Quality (CRF)"); ImGui::Dummy({0.f, 4.f});
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, Col::fg);
        ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
        ImGui::SetNextItemWidth(w - 16.f);
        ImGui::SliderInt("##crf", &state.render_settings.crf, 0, 51, "CRF %d");
        ImGui::PopStyleColor(2);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
        ImGui::TextUnformatted("18 = near-lossless  ·  23 = default  ·  28 = smaller file");
        ImGui::PopStyleColor();

        // Audio bitrate
        ImGui::Dummy({0.f, 8.f}); ui_label("Audio bitrate"); ImGui::Dummy({0.f, 4.f});
        for (int abr : {128, 192, 320}) {
            char lbl[8]; snprintf(lbl, sizeof(lbl), "%dk", abr);
            if (ui_btn(lbl, state.render_settings.audio_bitrate == abr, true))
                state.render_settings.audio_bitrate = abr;
            ImGui::SameLine(0.f, 4.f);
        }
        ImGui::NewLine();

        // Encode preset
        ImGui::Dummy({0.f, 8.f}); ui_label("Encode speed"); ImGui::Dummy({0.f, 4.f});
        const char* presets[] = {"ultrafast","fast","medium","slow","veryslow"};
        for (auto& ps : presets) {
            if (ui_btn(ps, state.render_settings.preset == ps, true))
                state.render_settings.preset = ps;
            ImGui::SameLine(0.f, 4.f);
        }
        ImGui::NewLine();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
        ImGui::TextUnformatted("Slower = better compression, same quality.");
        ImGui::PopStyleColor();

        // Profile
        ImGui::Dummy({0.f, 8.f}); ui_label("H.264 profile"); ImGui::Dummy({0.f, 4.f});
        if (ui_btn("Main", !state.render_settings.high_profile, true))
            state.render_settings.high_profile = false;
        ImGui::SameLine(0.f, 4.f);
        if (ui_btn("High", state.render_settings.high_profile, true))
            state.render_settings.high_profile = true;
        ImGui::NewLine();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
        ImGui::TextUnformatted("Main = max compatibility  ·  High = better compression");
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 4.f});
        ImGui::TreePop();
    }

    ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
    ui_label("Render"); ImGui::Dummy({0.f, 6.f});

    // Progress bar
    float bar_w = w - 16.f;
    ImVec2 bp = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(bp, {bp.x+bar_w, bp.y+4.f}, to_u32(Col::line), 2.f);
    ImGui::GetWindowDrawList()->AddRectFilled(bp, {bp.x+bar_w*state.render.progress, bp.y+4.f}, to_u32(Col::fg), 2.f);
    ImGui::Dummy({0.f, 8.f});

    char pct[16]; snprintf(pct, sizeof(pct), "%d%%", (int)(state.render.progress*100.f));
    ImGui::PushFont(g_font_bold); ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted(pct);
    ImGui::SetWindowFontScale(1.f); ImGui::PopFont();

    ImGui::SameLine(0.f, 8.f);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextUnformatted(state.render.running ? state.render.stage.c_str() : "Idle");
    ImGui::PopStyleColor();

    ImGui::Dummy({0.f, 6.f});
    if (state.render.running) {
        if (ui_btn("Cancel", false, true)) render_cancel();
    } else {
        if (ui_btn("Start render  ->", true, true)) {
            state.render_done = false;
            if (!state.audio_path.empty()) {
                fs::path audio(state.audio_path);
                fs::path outdir = audio.parent_path() / audio.stem();
                fs::create_directories(outdir);
                state.out_mp4 = (outdir / (audio.stem().string() + ".mp4")).string();
                state.out_srt = (outdir / (audio.stem().string() + ".srt")).string();
            }
            render_start(state);
        }
    }

    ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
    ui_label("Downloads"); ImGui::Dummy({0.f, 6.f});

    struct Dl { const char* tag; const char* name; std::string path; bool ok; };
    const char* fmt_res = state.format==OutputFormat::Vertical?"9:16":state.format==OutputFormat::Horizontal?"16:9":"1:1";
    (void)fmt_res;
    Dl dls[] = {
        {".MP4", "Lyric video",  state.out_mp4, state.render_done && !state.out_mp4.empty()},
        {".WAV", "Vocals stem",  state.out_wav, !state.out_wav.empty() && fs::exists(state.out_wav)},
        {".SRT", "Subtitles",    state.out_srt, !state.out_srt.empty() && fs::exists(state.out_srt)},
    };
    for (auto& dl : dls) {
        if (!dl.ok) ImGui::BeginDisabled();
        char did[32]; snprintf(did, sizeof(did), "%s %s##dl", dl.tag, dl.name);
        if (ui_btn(did, false, true)) {
            std::string cmd = "xdg-open \"" + fs::path(dl.path).parent_path().string() + "\"";
            system(cmd.c_str());
        }
        if (!dl.ok) ImGui::EndDisabled();
        ImGui::SameLine(0.f, 4.f);
    }

    ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
    ui_label("Blender"); ImGui::Dummy({0.f, 6.f});
    static std::string blender_status;
    bool has_tracks = !state.tracks.empty();
    if (!has_tracks) ImGui::BeginDisabled();
    if (ui_btn("Export .py  ->", false, true)) {
        std::string sp = state.audio_path.empty() ? "pop_maker_blender.py" :
            (fs::path(state.audio_path).parent_path() /
             (fs::path(state.audio_path).stem().string() + "_blender.py")).string();
        if (blender_export_script(state, sp)) {
            blender_status = sp;
            system(("xdg-open \"" + fs::path(sp).parent_path().string() + "\"").c_str());
        }
    }
    if (!has_tracks) ImGui::EndDisabled();
    if (!blender_status.empty()) {
        ImGui::SameLine(0.f, 8.f);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted(("✓ " + fs::path(blender_status).filename().string()).c_str());
        ImGui::PopStyleColor();
    }
}

// ── Timeline constants ────────────────────────────────────────────────────────

static constexpr float TL_LABEL_W = 120.f;
static constexpr float TL_TRACK_H = 42.f;
static constexpr float TL_RULER_H = 24.f;

// ── Timeline ──────────────────────────────────────────────────────────────────

static void draw_timeline(AppState& state, ImVec2 origin, float total_w, float total_h) {
    ImDrawList* dl      = ImGui::GetWindowDrawList();
    float clip_area_w   = total_w - TL_LABEL_W;
    float dur           = fmaxf(state.duration, 1.f);
    float& zoom         = state.tl_zoom;
    float& scroll       = state.tl_scroll;
    float tl_content_w  = dur * zoom;

    // Tick drop flash timer
    if (s_drop_flash_t > 0.f)
        s_drop_flash_t -= ImGui::GetIO().DeltaTime;

    ImVec2 mouse = ImGui::GetIO().MousePos;
    bool in_tl = mouse.x >= origin.x && mouse.x <= origin.x + total_w &&
                 mouse.y >= origin.y && mouse.y <= origin.y + total_h;

    if (in_tl) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (fabsf(wheel) > 0.f) {
            bool in_track_body = mouse.y >= origin.y + TL_RULER_H && mouse.y < origin.y + total_h;
            if (ImGui::GetIO().KeyCtrl) {
                // Ctrl+scroll = zoom (anchor under cursor)
                float old_zoom = zoom;
                zoom = fmaxf(20.f, fminf(zoom * (1.f + wheel * 0.1f), 4000.f));
                float mouse_t = (mouse.x - origin.x - TL_LABEL_W + scroll) / old_zoom;
                scroll = fmaxf(0.f, mouse_t * zoom - (mouse.x - origin.x - TL_LABEL_W));
                scroll = fminf(scroll, fmaxf(0.f, tl_content_w - clip_area_w + 60.f));
            } else if (!in_track_body) {
                // Plain scroll in ruler/header = horizontal pan only
                scroll = fmaxf(0.f, scroll - wheel * 60.f);
                scroll = fminf(scroll, fmaxf(0.f, tl_content_w - clip_area_w + 60.f));
            }
            // Plain scroll in track body is handled by the vertical scroll block below
        }
    }

    // Background
    dl->AddRectFilled(origin, {origin.x+total_w, origin.y+total_h}, to_u32(Col::bg));
    dl->AddRect(origin, {origin.x+total_w, origin.y+total_h}, to_u32(Col::line));

    // ── Fps + frame snap helper ───────────────────────────────────────────────
    float fps = (state.proxy_ready && video_info(0).fps > 0.0)
                ? (float)video_info(0).fps : 30.f;
    // snap(t): round t to nearest frame unless Ctrl is held
    auto snap = [&](float t) -> float {
        if (ImGui::GetIO().KeyCtrl || fps <= 0.f) return t;
        return roundf(t * fps) / fps;
    };

    // ── Edge / playhead snapping ──────────────────────────────────────────────
    static bool s_snap_enabled = true;
    static float s_snap_indicator = -1.f;  // time of active snap line, -1 = none

    // Build candidate list (all clip edges + playhead), excluding one clip.
    auto build_snap_candidates = [&](int ex_ti, int ex_ci) -> std::vector<float> {
        std::vector<float> cands;
        cands.push_back(state.playhead);
        cands.push_back(0.f);
        for (int ti = 0; ti < (int)state.tracks.size(); ++ti)
            for (int ci = 0; ci < (int)state.tracks[ti].clips.size(); ++ci) {
                if (ti == ex_ti && ci == ex_ci) continue;
                cands.push_back(state.tracks[ti].clips[ci].start);
                cands.push_back(state.tracks[ti].clips[ci].end);
            }
        for (float bt : state.beats) cands.push_back(bt);
        return cands;
    };

    // Try to snap t to a nearby candidate; returns snapped value and sets indicator.
    // threshold_px: snap radius in screen pixels.
    const float SNAP_PX = 8.f;
    auto edge_snap = [&](float t, const std::vector<float>& cands) -> float {
        if (!s_snap_enabled || ImGui::GetIO().KeyCtrl) { s_snap_indicator = -1.f; return t; }
        float best_dt = SNAP_PX / zoom;
        float result  = t;
        for (float c : cands) {
            float dt = fabsf(c - t);
            if (dt < best_dt) { best_dt = dt; result = c; }
        }
        s_snap_indicator = (result != t) ? result : -1.f;
        return result;
    };

    // ── Ruler ─────────────────────────────────────────────────────────────────
    float ruler_y = origin.y;
    dl->AddRectFilled({origin.x, ruler_y},
        {origin.x+total_w, ruler_y+TL_RULER_H}, to_u32(Col::bg_soft));
    dl->AddLine({origin.x, ruler_y+TL_RULER_H},
        {origin.x+total_w, ruler_y+TL_RULER_H}, to_u32(Col::line));

    // Snap toggle button in label column of ruler
    {
        const char* lbl = s_snap_enabled ? "⊿ Snap" : "  Snap";
        ImVec2 btn_min = {origin.x + 4.f, ruler_y + 2.f};
        ImVec2 btn_max = {origin.x + TL_LABEL_W - 4.f, ruler_y + TL_RULER_H - 2.f};
        bool hov = mouse.x >= btn_min.x && mouse.x <= btn_max.x &&
                   mouse.y >= btn_min.y && mouse.y <= btn_max.y;
        ImU32 btn_col = s_snap_enabled
            ? IM_COL32(120, 200, 255, hov ? 180 : 120)
            : to_u32(hov ? Col::line_hover : Col::line);
        dl->AddRectFilled(btn_min, btn_max, IM_COL32(0,0,0,0));
        dl->AddRect(btn_min, btn_max, btn_col, 3.f);
        ImU32 txt_col = s_snap_enabled
            ? IM_COL32(120, 200, 255, 255)
            : to_u32(Col::muted);
        float tw = ImGui::CalcTextSize(lbl).x;
        float th = ImGui::CalcTextSize(lbl).y;
        dl->AddText({btn_min.x + ((btn_max.x-btn_min.x)-tw)*0.5f,
                     btn_min.y + ((btn_max.y-btn_min.y)-th)*0.5f}, txt_col, lbl);
        if (hov && ImGui::IsMouseClicked(0))
            s_snap_enabled = !s_snap_enabled;
    }

    // Adaptive tick ladder {major_interval_secs, subdivisions}.
    // Selected so that major ticks are always ≥ 80 px apart.
    struct TickLevel { float secs; int subdivs; };
    const float f1 = 1.f / fps;
    const TickLevel levels[] = {
        {f1,      1},   // 1 frame
        {2*f1,    2},   // 2 frames,  minor every 1f
        {5*f1,    5},   // 5 frames,  minor every 1f
        {10*f1,   2},   // 10 frames, minor every 5f
        {0.5f,    5},   // 0.5 s,     minor every 0.1 s
        {1.f,     4},   // 1 s,       minor every 0.25 s
        {2.f,     4},   // 2 s,       minor every 0.5 s
        {5.f,     5},   // 5 s,       minor every 1 s
        {10.f,    2},   // 10 s,      minor every 5 s
        {30.f,    3},   // 30 s,      minor every 10 s
        {60.f,    4},   // 1 min,     minor every 15 s
        {300.f,   5},   // 5 min,     minor every 1 min
        {600.f,   2},   // 10 min,    minor every 5 min
    };
    const int NUM_LEVELS = (int)(sizeof(levels)/sizeof(levels[0]));
    const float MIN_MAJOR_PX = 80.f;

    TickLevel chosen = levels[NUM_LEVELS-1];
    for (int li = 0; li < NUM_LEVELS; ++li) {
        if (levels[li].secs * zoom >= MIN_MAJOR_PX) { chosen = levels[li]; break; }
    }

    float minor_secs = chosen.secs / (float)chosen.subdivs;
    float first_tick = floorf((scroll / zoom) / minor_secs) * minor_secs;

    for (float t = first_tick; t <= dur + chosen.secs; t += minor_secs) {
        float px = origin.x + TL_LABEL_W + t * zoom - scroll;
        if (px < origin.x + TL_LABEL_W - 1.f || px > origin.x + total_w) continue;

        int   tick_idx = (int)roundf(t / minor_secs);
        bool  is_major = (tick_idx % chosen.subdivs == 0);

        if (is_major) {
            dl->AddLine({px, ruler_y + 4.f}, {px, ruler_y + TL_RULER_H},
                        to_u32(Col::muted));
            char tbuf[16];
            if (chosen.secs >= 1.f)
                snprintf(tbuf, sizeof(tbuf), "%s", fmt_time_short(t).c_str());
            else
                snprintf(tbuf, sizeof(tbuf), "%d", (int)roundf(t * fps));
            float tw = ImGui::CalcTextSize(tbuf).x;
            float lx = px - tw * 0.5f;
            if (lx >= origin.x + TL_LABEL_W + 2.f && lx + tw <= origin.x + total_w - 2.f)
                dl->AddText({lx, ruler_y + 3.f}, to_u32(Col::muted), tbuf);
        } else {
            dl->AddLine({px, ruler_y + 10.f}, {px, ruler_y + TL_RULER_H},
                        to_u32(Col::dim));
        }
    }

    // Beat tick marks (small orange triangles at the bottom of the ruler)
    if (!state.beats.empty()) {
        ImU32 beat_col = IM_COL32(255, 160, 50, 180);
        float ty = ruler_y + TL_RULER_H - 5.f;
        for (float bt : state.beats) {
            float px = origin.x + TL_LABEL_W + bt * zoom - scroll;
            if (px < origin.x + TL_LABEL_W || px > origin.x + total_w) continue;
            dl->AddTriangleFilled({px-3.f, ty}, {px+3.f, ty}, {px, ty+5.f}, beat_col);
        }
    }

    // Tracks
    // Vertical scroll: mouse wheel in the track body area
    float track_area_top = origin.y + TL_RULER_H;
    float track_area_bot = origin.y + total_h;
    float tracks_total_h = ((int)state.tracks.size() + 1) * TL_TRACK_H;  // +1 for add-track row
    float max_v_scroll   = fmaxf(0.f, tracks_total_h - (track_area_bot - track_area_top));
    if (ImGui::IsMouseHoveringRect({origin.x, track_area_top}, {origin.x+total_w, track_area_bot})) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f && !ImGui::GetIO().KeyCtrl)
            state.tl_v_scroll -= wheel * TL_TRACK_H;
    }
    state.tl_v_scroll = fmaxf(0.f, fminf(max_v_scroll, state.tl_v_scroll));

    float track_y = track_area_top - state.tl_v_scroll;
    s_tl_hover_track = -1;  // reset each frame; set below as we scan rows
    static int   drag_track = -1, drag_clip = -1;
    static float drag_offset = 0.f;
    static bool  drag_left = false, drag_right = false;
    static int drag_hot_track = -1;  // track under mouse during body drag
    static int  s_rename_track = -1;
    static char s_rename_buf[64] = {};
    static bool s_rename_focus = false;
    static int   s_track_drag_src    = -1;
    static bool  s_track_dragging    = false;
    static float s_track_drag_start_y = 0.f;
    static int   s_track_drag_insert  = -1;
    static int   drag_hot_gap = -1;  // insert-before index when dragging clip near a boundary

    // Box select state
    static bool  s_box_selecting = false;
    static ImVec2 s_box_start    = {0.f, 0.f};
    static bool  s_clip_hit      = false;  // did mousedown land on a clip this frame?

    // Context menu state
    static int ctx_track = -1, ctx_clip = -1;
    static bool open_clip_ctx = false, open_track_ctx = false, open_tl_ctx = false;

    // Transition glass state
    static int    s_trans_track = -1, s_trans_left_ci = -1;
    static ImVec2 s_trans_popup_pos = {0.f, 0.f};
    static int    s_glass_drag = 0;   // 0=none 1=left-handle 2=right-handle 3=body
    static float  s_glass_drag_ref_x = 0.f;
    static float  s_glass_drag_ref_pre = 0.f, s_glass_drag_ref_post = 0.f;
    static float  s_glass_drag_ref_start = 0.f;
    bool s_trans_hit_this_frame = false;

    // Clip all track drawing to the scrollable area (below ruler, above add-track row)
    dl->PushClipRect({origin.x, track_area_top}, {origin.x+total_w, track_area_bot}, true);

    for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
        Track& track = state.tracks[ti];
        ImVec2 row_tl = {origin.x, track_y};
        ImVec2 row_br = {origin.x+total_w, track_y+TL_TRACK_H};
        bool row_hov = mouse.y >= row_tl.y && mouse.y < row_br.y;
        if (row_hov) s_tl_hover_track = ti;

        dl->AddRectFilled(row_tl, row_br,
            to_u32(row_hov ? Col::bg_soft_hov : Col::bg_soft));
        dl->AddLine({origin.x, row_br.y}, {origin.x+total_w, row_br.y}, to_u32(Col::line));

        // FX preset drag-drop target on the clip area of this row
        {
            ImVec2 drop_tl = {origin.x + TL_LABEL_W, track_y};
            ImVec2 drop_br = {origin.x + total_w,     track_y + TL_TRACK_H};
            ImGui::SetCursorScreenPos(drop_tl);
            ImGui::InvisibleButton(("##fxdrop" + std::to_string(ti)).c_str(),
                                   {drop_br.x - drop_tl.x, drop_br.y - drop_tl.y});
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("FX_PRESET")) {
                    int idx = *(const int*)pay->Data;
                    const EffectPreset* preset = nullptr;
                    int bn = (int)g_builtin_presets.size();
                    if (idx < bn) preset = &g_builtin_presets[idx];
                    else if (idx - bn < (int)state.user_presets.size())
                        preset = &state.user_presets[idx - bn];
                    if (preset) {
                        // Place clip at cursor time position
                        float drop_x = ImGui::GetMousePos().x - (origin.x + TL_LABEL_W);
                        float drop_t = (drop_x + scroll) / zoom;
                        drop_t = fmaxf(0.f, drop_t);
                        Clip cl;
                        cl.clip_type = ClipType::Effect;
                        cl.start     = drop_t;
                        cl.end       = drop_t + 2.f;
                        preset_apply(cl, *preset);
                        state.tracks[ti].clips.push_back(cl);
                        state.selected_track = ti;
                        state.selected_clip  = (int)state.tracks[ti].clips.size() - 1;
                        s_drop_flash_track = ti;
                        s_drop_flash_t     = 0.6f;
                        history_push(state, "Drop Effect preset: " + preset->name);
                    }
                }
                // Highlight the row while dragging over it
                dl->AddRectFilled(drop_tl, drop_br, IM_COL32(180,130,255,40));
                dl->AddRect(drop_tl, drop_br, IM_COL32(180,130,255,180), 2.f);
                ImGui::EndDragDropTarget();
            }
        }

        // Drop flash — briefly illuminate the row that just received a drop.
        if (s_drop_flash_t > 0.f && s_drop_flash_track == ti) {
            float alpha = s_drop_flash_t / 0.6f;  // 1→0 linear fade
            // Pulse: quick bright flash then fade
            float pulse = alpha > 0.7f ? (1.f - alpha) / 0.3f : 1.f;
            ImU32 flash_col = IM_COL32(120, 200, 255, (int)(pulse * alpha * 120));
            dl->AddRectFilled(row_tl, row_br, flash_col, 2.f);
            dl->AddRect(row_tl, row_br, IM_COL32(120, 200, 255, (int)(alpha * 200)), 2.f);
        }

        // Cross-track drag ghost — show target landing zone
        if (drag_hot_track == ti && drag_track >= 0 && drag_clip >= 0 &&
            drag_hot_track != drag_track && !drag_left && !drag_right &&
            drag_track < (int)state.tracks.size() &&
            drag_clip  < (int)state.tracks[drag_track].clips.size()) {
            const Clip& gc = state.tracks[drag_track].clips[drag_clip];
            float gx0 = fmaxf(origin.x+TL_LABEL_W, origin.x+TL_LABEL_W+gc.start*zoom-scroll);
            float gx1 = fminf(origin.x+total_w,     origin.x+TL_LABEL_W+gc.end*zoom-scroll);
            float gy0 = track_y+3.f, gy1 = track_y+TL_TRACK_H-3.f;
            if (gx1 > gx0) {
                dl->AddRectFilled({gx0,gy0},{gx1,gy1}, IM_COL32(255,255,255,35), 2.f);
                dl->AddRect({gx0,gy0},{gx1,gy1}, IM_COL32(255,255,255,160), 2.f, 0, 1.5f);
            }
        }

        // Track label — single click selects, double-click renames
        bool track_sel = state.selected_track == ti;
        bool in_label = mouse.x >= origin.x+2.f && mouse.x < origin.x+TL_LABEL_W-20.f &&
                        mouse.y >= track_y && mouse.y < track_y+TL_TRACK_H;

        if (s_rename_track == ti) {
            // Inline rename field
            ImGui::SetCursorScreenPos({origin.x+4.f, track_y+(TL_TRACK_H-18.f)*0.5f});
            ImGui::SetNextItemWidth(TL_LABEL_W - 26.f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
            ImGui::PushStyleColor(ImGuiCol_Border,  Col::fg);
            if (s_rename_focus) { ImGui::SetKeyboardFocusHere(); s_rename_focus = false; }
            bool committed = ImGui::InputText("##rename", s_rename_buf, sizeof(s_rename_buf),
                ImGuiInputTextFlags_EnterReturnsTrue);
            if (committed || (ImGui::IsItemDeactivated() && !ImGui::IsKeyPressed(ImGuiKey_Escape))) {
                std::string newname(s_rename_buf);
                if (!newname.empty() && newname != track.name) {
                    track.name = newname;
                    history_push(state, "Rename track");
                }
                s_rename_track = -1;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) s_rename_track = -1;
            ImGui::PopStyleColor(2);
        } else {
            dl->AddText({origin.x+8.f, track_y+(TL_TRACK_H-13.f)*0.5f},
                to_u32(track_sel ? Col::fg : Col::muted), track.name.c_str());
            if (ImGui::IsMouseClicked(0) && in_label) {
                state.selected_track  = ti;
                state.selected_clip   = -1;
                state.clip_selection.clear();
                state.panel_tab       = 0;
                s_track_drag_src      = ti;
                s_track_drag_start_y  = mouse.y;
                s_track_dragging      = false;
            }
            if (ImGui::IsMouseDoubleClicked(0) && in_label && !s_track_dragging) {
                s_track_drag_src = -1;
                s_rename_track = ti;
                strncpy(s_rename_buf, track.name.c_str(), sizeof(s_rename_buf)-1);
                s_rename_buf[sizeof(s_rename_buf)-1] = '\0';
                s_rename_focus = true;
            }
        }

        // Visibility dot
        ImVec2 vc = {origin.x+TL_LABEL_W-16.f, track_y+TL_TRACK_H*0.5f};
        dl->AddCircleFilled(vc, 4.f, to_u32(track.visible ? Col::fg : Col::dim));
        if (ImGui::IsMouseClicked(0) && fabsf(mouse.x-vc.x)<8.f && fabsf(mouse.y-vc.y)<8.f)
            track.visible = !track.visible;

        dl->AddLine({origin.x+TL_LABEL_W, track_y},
                    {origin.x+TL_LABEL_W, track_y+TL_TRACK_H}, to_u32(Col::line));

        // Right-click track label
        if (ImGui::IsMouseClicked(1) &&
            mouse.x >= origin.x && mouse.x < origin.x+TL_LABEL_W &&
            mouse.y >= track_y  && mouse.y < track_y+TL_TRACK_H) {
            ctx_track = ti; ctx_clip = -1;
            open_track_ctx = true;
            ImGui::OpenPopup("##track_ctx");
        }

        // Clips
        bool clip_ctx_opened_this_frame = false;
        for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
            Clip& clip = track.clips[ci];
            float cx0 = origin.x+TL_LABEL_W+clip.start*zoom-scroll;
            float cx1 = origin.x+TL_LABEL_W+clip.end*zoom-scroll;
            if (cx1 < origin.x+TL_LABEL_W || cx0 > origin.x+total_w) continue;
            float vis_x0 = fmaxf(cx0, origin.x+TL_LABEL_W);
            float vis_x1 = fminf(cx1, origin.x+total_w);
            float cy0 = track_y+3.f, cy1 = track_y+TL_TRACK_H-3.f;
            bool sel = state.clip_selection.count({ti, ci}) > 0;

            if (clip.clip_type == ClipType::Effect) {
                // Effect clip — translucent purple adjustment-layer bar
                ImU32 fx_fill   = sel ? IM_COL32(160,100,255,200) : IM_COL32(120,80,220,110);
                ImU32 fx_border = sel ? IM_COL32(200,160,255,255) : IM_COL32(160,110,255,200);
                dl->AddRectFilled({vis_x0,cy0},{vis_x1,cy1}, fx_fill, 2.f);
                dl->AddRect({vis_x0,cy0},{vis_x1,cy1}, fx_border, 2.f, 0, 1.5f);

                ImGui::PushClipRect({vis_x0,cy0},{vis_x1,cy1},true);
                ImU32 lbl_col = IM_COL32(220,190,255,240);
                float ly = cy0 + (cy1-cy0-13.f)*0.5f;
                dl->AddText({vis_x0+5.f, ly}, lbl_col, "FX");
                float bx = vis_x0 + 28.f;
                if (clip.fx_color_on    && bx+28.f<vis_x1) { dl->AddText({bx,ly},lbl_col,"Col"); bx+=28.f; }
                if (clip.fx_blur_on     && bx+30.f<vis_x1) { dl->AddText({bx,ly},lbl_col,"Blur");bx+=32.f; }
                if (clip.fx_vignette_on && bx+24.f<vis_x1) { dl->AddText({bx,ly},lbl_col,"Vig"); bx+=28.f; }
                if (clip.fx_text_on     && bx+22.f<vis_x1) { dl->AddText({bx,ly},lbl_col,"Txt"); }
                // Down-arrow scope indicator at right edge
                if (vis_x1-vis_x0 > 30.f) {
                    float ax=vis_x1-12.f, ay=cy0+(cy1-cy0)*0.35f;
                    dl->AddTriangleFilled({ax-5.f,ay},{ax+5.f,ay},{ax,ay+7.f},IM_COL32(220,190,255,180));
                }
                ImGui::PopClipRect();

                // Active-time glow on tracks below
                if (state.playhead >= clip.start && state.playhead < clip.end) {
                    float gx = origin.x + TL_LABEL_W;
                    for (int gti = ti+1; gti < (int)state.tracks.size(); ++gti) {
                        float gty = (track_area_top - state.tl_v_scroll)
                                  + gti * TL_TRACK_H;
                        if (gty+TL_TRACK_H < track_area_top || gty > track_area_bot) continue;
                        dl->AddRectFilled({gx,gty},{gx+2.f,gty+TL_TRACK_H},
                                          IM_COL32(160,110,255,80));
                    }
                }
            } else {

            ImVec4 clip_fill = (clip.clip_type==ClipType::Lyrics)   ? Col::clip_lyrics
                             : (clip.clip_type==ClipType::Subtitle) ? Col::clip_subtitle
                             : (clip.clip_type==ClipType::Text)     ? Col::clip_sub
                             : (clip.clip_type==ClipType::Audio)    ? Col::clip_audio
                                                                     : Col::clip_video;
            dl->AddRectFilled({vis_x0,cy0},{vis_x1,cy1},
                to_u32(sel ? Col::fg : clip_fill), 2.f);
            dl->AddRect({vis_x0,cy0},{vis_x1,cy1},
                to_u32(sel ? Col::fg : Col::line), 2.f);

            // Clip label / waveform
            if ((clip.clip_type==ClipType::Text || clip.clip_type==ClipType::Lyrics ||
                 clip.clip_type==ClipType::Subtitle) && !clip.text.empty()) {
                ImGui::PushClipRect({vis_x0,cy0},{vis_x1,cy1},true);
                dl->AddText({vis_x0+4.f, cy0+(cy1-cy0-13.f)*0.5f},
                    to_u32(sel ? Col::bg : Col::fg), clip.text.c_str());
                ImGui::PopClipRect();
            } else if (clip.clip_type==ClipType::Audio || clip.clip_type==ClipType::Video) {
                const std::string& wave_path = clip.text;
                const WaveformData* wd = !wave_path.empty()
                    ? waveform_get(wave_path) : nullptr;

                ImGui::PushClipRect({vis_x0,cy0},{vis_x1,cy1},true);

                // Filename label at top of brick
                if (clip.clip_type==ClipType::Video) {
                    dl->AddText({vis_x0+4.f, cy0+3.f},
                        to_u32(sel?Col::bg:Col::fg),
                        clip.text.empty()?"Video":fs::path(clip.text).filename().string().c_str());
                }

                if (wd && !wd->samples.empty()) {
                    float mid   = (cy0 + cy1) * 0.5f;
                    float half  = (cy1 - cy0) * 0.44f;
                    ImU32 wcol  = sel ? IM_COL32(0,0,0,160) : IM_COL32(255,255,255,120);

                    // Draw one vertical bar per screen pixel in visible range
                    for (float px = vis_x0; px < vis_x1; px += 1.f) {
                        float dt    = 1.f / WAVEFORM_FPS;
                        float t_src = clip.in_point + (px - cx0) / zoom;
                        int   fi    = (int)(t_src / dt);
                        if (fi < 0 || fi >= (int)wd->samples.size()) continue;
                        float amp = wd->samples[fi] * half;
                        if (amp < 1.f) amp = 1.f;
                        dl->AddLine({px, mid-amp}, {px, mid+amp}, wcol);
                    }
                }

                ImGui::PopClipRect();
            }

            } // end else (non-Effect clips)

            // Edge handles — common to all clip types
            const float ew     = 6.f;   // drawn width
            const float ew_hit = 12.f;  // hit zone width
            if (sel) {
                dl->AddRectFilled({vis_x0,cy0},{vis_x0+ew,cy1},to_u32(Col::muted),1.f);
                dl->AddRectFilled({vis_x1-ew,cy0},{vis_x1,cy1},to_u32(Col::muted),1.f);
            }
            // Resize cursor — show on hover even before clicking
            {
                float orig_cx0h = origin.x+TL_LABEL_W+clip.start*zoom-scroll;
                float orig_cx1h = origin.x+TL_LABEL_W+clip.end*zoom-scroll;
                bool in_clip = mouse.y>=cy0 && mouse.y<=cy1 &&
                               mouse.x>=vis_x0 && mouse.x<=vis_x1;
                if (in_clip && (mouse.x <= orig_cx0h+ew_hit || mouse.x >= orig_cx1h-ew_hit))
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }

            // Keyframe diamond markers — drawn for all keyframed properties
            if (!clip.ktracks.empty()) {
                float kf_mid_y = (cy0 + cy1) * 0.5f;
                float d = 4.f;  // half-size of diamond
                for (auto& [pname, pt] : clip.ktracks) {
                    for (auto& kf : pt.keys) {
                        float kx = origin.x + TL_LABEL_W + (clip.start + kf.time) * zoom - scroll;
                        if (kx < vis_x0 || kx > vis_x1) continue;
                        bool kf_sel = (state.kf_sel_track == ti &&
                                       state.kf_sel_clip  == ci &&
                                       state.kf_sel_prop  == pname &&
                                       &kf == &pt.keys[state.kf_sel_idx > 0 &&
                                           state.kf_sel_idx < (int)pt.keys.size()
                                           ? state.kf_sel_idx : 0]);
                        ImU32 kc = kf_sel ? IM_COL32(255,200,60,255) : IM_COL32(220,220,220,200);
                        dl->AddQuadFilled(
                            {kx, kf_mid_y-d}, {kx+d, kf_mid_y},
                            {kx, kf_mid_y+d}, {kx-d, kf_mid_y}, kc);
                        // Click to select keyframe
                        if (ImGui::IsMouseClicked(0) &&
                            fabsf(mouse.x - kx) < d+2.f &&
                            fabsf(mouse.y - kf_mid_y) < d+2.f) {
                            state.kf_sel_track = ti;
                            state.kf_sel_clip  = ci;
                            state.kf_sel_prop  = pname;
                            int idx = (int)(&kf - pt.keys.data());
                            state.kf_sel_idx   = idx;
                            state.selected_track = ti;
                            state.selected_clip  = ci;
                        }
                    }
                }
            }

            // Left click to select / drag
            bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
            if (!s_trans_hit_this_frame && !any_popup && ImGui::IsMouseClicked(0)) {
                if (mouse.y>=cy0 && mouse.y<=cy1 && mouse.x>=vis_x0 && mouse.x<=vis_x1) {
                    s_clip_hit = true;
                    auto key = std::make_pair(ti, ci);
                    bool ctrl  = ImGui::GetIO().KeyCtrl;
                    bool shift = ImGui::GetIO().KeyShift;

                    if (ctrl) {
                        // Toggle this clip in/out of selection
                        if (state.clip_selection.count(key))
                            state.clip_selection.erase(key);
                        else
                            state.clip_selection.insert(key);
                    } else if (shift && state.selected_track >= 0 && state.selected_clip >= 0) {
                        // Range select: collect all clips between focus and this clip by start time
                        struct TL { float start; int ti, ci; };
                        std::vector<TL> all;
                        for (int t2=0; t2<(int)state.tracks.size(); ++t2)
                            for (int c2=0; c2<(int)state.tracks[t2].clips.size(); ++c2)
                                all.push_back({state.tracks[t2].clips[c2].start, t2, c2});
                        std::sort(all.begin(), all.end(), [](const TL& a, const TL& b){ return a.start < b.start; });
                        float t_focus = state.tracks[state.selected_track].clips[state.selected_clip].start;
                        float t_click = clip.start;
                        float t_lo = fminf(t_focus, t_click), t_hi = fmaxf(t_focus, t_click);
                        for (auto& e : all)
                            if (e.start >= t_lo && e.start <= t_hi)
                                state.clip_selection.insert({e.ti, e.ci});
                    } else {
                        // Plain click — clear and select only this clip
                        state.clip_selection.clear();
                        state.clip_selection.insert(key);
                    }

                    // Always update focus clip
                    state.selected_track = ti;
                    state.selected_clip  = ci;
                    strncpy(s_edit_buf, clip.text.c_str(), sizeof(s_edit_buf)-1);
                    s_edit_buf[sizeof(s_edit_buf)-1] = '\0';
                    s_edit_focus_next = (clip.clip_type==ClipType::Text || clip.clip_type==ClipType::Lyrics ||
                                         clip.clip_type==ClipType::Subtitle);
                    if (!state.playing) seek_to(state, clip.start);

                    float orig_cx0 = origin.x+TL_LABEL_W+clip.start*zoom-scroll;
                    float orig_cx1 = origin.x+TL_LABEL_W+clip.end*zoom-scroll;
                    if (mouse.x <= orig_cx0+ew_hit) {
                        drag_track=ti; drag_clip=ci; drag_left=true; drag_right=false; drag_offset=0.f;
                    } else if (mouse.x >= orig_cx1-ew_hit) {
                        drag_track=ti; drag_clip=ci; drag_right=true; drag_left=false; drag_offset=0.f;
                    } else {
                        drag_track=ti; drag_clip=ci; drag_left=false; drag_right=false;
                        drag_offset = mouse.x - orig_cx0;
                    }
                }
            }

            // Right-click clip context
            if (!clip_ctx_opened_this_frame && ImGui::IsMouseClicked(1) &&
                mouse.y>=cy0 && mouse.y<=cy1 && mouse.x>=vis_x0 && mouse.x<=vis_x1) {
                ctx_track = ti; ctx_clip = ci;
                state.selected_track = ti; state.selected_clip = ci;
                open_clip_ctx = true;
                clip_ctx_opened_this_frame = true;
                ImGui::OpenPopup("##clip_ctx");
            }
        }

        // Right-click empty timeline area (this track row, no clip hit)
        if (!clip_ctx_opened_this_frame && ImGui::IsMouseClicked(1) &&
            mouse.y >= track_y && mouse.y < track_y+TL_TRACK_H &&
            mouse.x >= origin.x+TL_LABEL_W && mouse.x <= origin.x+total_w) {
            open_tl_ctx = true;
            ImGui::OpenPopup("##tl_ctx");
        }

        // Transition glass — drawn at every adjacent video clip cut point
        for (int ci = 0; ci + 1 < (int)track.clips.size(); ++ci) {
            Clip& a = track.clips[ci];
            Clip& b = track.clips[ci + 1];
            if (a.clip_type != ClipType::Video || b.clip_type != ClipType::Video) continue;
            if (fabsf(b.start - a.end) > 0.5f) continue;

            float cut_x   = origin.x + TL_LABEL_W + a.end   * zoom - scroll;
            float cy0     = track_y + 2.f;
            float cy1     = track_y + TL_TRACK_H - 2.f;
            float mid_y   = (cy0 + cy1) * 0.5f;
            bool  has_trans = (a.transition_type != TransitionType::None);
            bool  this_glass_active = (s_trans_track == ti && s_trans_left_ci == ci);

            if (!has_trans) {
                // Diamond affordance: click to add a transition
                float r = 7.f;
                ImVec2 pts[4] = {{cut_x,cut_x-r},{cut_x+r,mid_y},{cut_x,mid_y+r},{cut_x-r,mid_y}};
                pts[0] = {cut_x, mid_y - r};
                pts[1] = {cut_x + r, mid_y};
                pts[2] = {cut_x, mid_y + r};
                pts[3] = {cut_x - r, mid_y};
                bool hov = fabsf(mouse.x - cut_x) + fabsf(mouse.y - mid_y) <= r + 3.f;
                dl->AddConvexPolyFilled(pts, 4, IM_COL32(18,18,18,220));
                dl->AddPolyline(pts, 4, hov ? IM_COL32(255,255,255,230) : IM_COL32(210,210,210,200),
                                ImDrawFlags_Closed, 1.5f);
                if (hov && ImGui::IsMouseClicked(0)) {
                    s_trans_track    = ti; s_trans_left_ci = ci;
                    s_trans_popup_pos = {cut_x - 80.f, cy0 - 8.f};
                    s_trans_hit_this_frame = true;
                    ImGui::OpenPopup("##trans_picker");
                }
                if (hov) ImGui::SetTooltip("Add transition");
            } else {
                // Glass block
                float pre_px  = a.transition_pre  * zoom;
                float post_px = a.transition_post * zoom;
                float gx0 = cut_x - pre_px;
                float gx1 = cut_x + post_px;
                float vis_gx0 = fmaxf(gx0, origin.x + TL_LABEL_W);
                float vis_gx1 = fminf(gx1, origin.x + total_w);

                // Glass fill + border
                ImU32 glass_fill   = IM_COL32(130,190,255,55);
                ImU32 glass_border = this_glass_active
                    ? IM_COL32(160,210,255,255) : IM_COL32(130,190,255,200);
                dl->AddRectFilled({vis_gx0, cy0}, {vis_gx1, cy1}, glass_fill, 3.f);
                // Diagonal lines inside glass to suggest blend
                dl->PushClipRect({vis_gx0,cy0},{vis_gx1,cy1}, true);
                float step = 10.f;
                float gh   = cy1 - cy0;
                for (float ox = gx0 - gh; ox < gx1 + gh; ox += step)
                    dl->AddLine({ox, cy0}, {ox + gh, cy1}, IM_COL32(160,210,255,30), 1.f);
                dl->PopClipRect();
                dl->AddRect({vis_gx0, cy0}, {vis_gx1, cy1}, glass_border, 3.f, 0, 1.5f);

                // Edge drag handles
                float hw = 5.f;
                auto draw_handle = [&](float hx, bool hov_h) {
                    ImU32 hc = hov_h ? IM_COL32(255,255,255,255) : IM_COL32(160,210,255,230);
                    dl->AddRectFilled({hx-hw, cy0+2.f}, {hx+hw, cy1-2.f}, hc, 2.f);
                };

                float handle_tol = hw + 4.f;
                bool in_glass = mouse.y >= cy0 && mouse.y <= cy1 &&
                                mouse.x >= vis_gx0 && mouse.x <= vis_gx1;
                bool hov_left  = in_glass && fabsf(mouse.x - gx0) <= handle_tol && s_glass_drag == 0;
                bool hov_right = in_glass && fabsf(mouse.x - gx1) <= handle_tol && !hov_left && s_glass_drag == 0;
                bool hov_body  = in_glass && !hov_left && !hov_right;

                if (gx0 >= origin.x + TL_LABEL_W) draw_handle(gx0, hov_left);
                if (gx1 <= origin.x + total_w)     draw_handle(gx1, hov_right);

                // Cursor
                if (hov_left || hov_right || (s_glass_drag == 1 && this_glass_active) ||
                    (s_glass_drag == 2 && this_glass_active))
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

                // Tooltip
                if (hov_body && s_glass_drag == 0) {
                    const char* tname = (a.transition_type == TransitionType::Dissolve)  ? "Dissolve"
                                      : (a.transition_type == TransitionType::FadeBlack) ? "Fade to Black"
                                      : "Dip to White";
                    ImGui::SetTooltip("%s  %.2fs | %.2fs", tname, a.transition_pre, a.transition_post);
                }

                // Mouse down — start drag or open picker
                if (ImGui::IsMouseClicked(0) && in_glass) {
                    s_trans_track    = ti; s_trans_left_ci = ci;
                    s_trans_hit_this_frame = true;
                    s_glass_drag_ref_x    = mouse.x;
                    s_glass_drag_ref_pre  = a.transition_pre;
                    s_glass_drag_ref_post = a.transition_post;
                    s_glass_drag_ref_start = a.start;
                    if (hov_left)       s_glass_drag = 1;
                    else if (hov_right) s_glass_drag = 2;
                    else                s_glass_drag = 3;
                    // Cancel any clip drag the clip loop already registered this frame
                    drag_track = -1; drag_clip = -1; drag_left = false; drag_right = false;
                }

                // Right-click removes transition
                if (ImGui::IsMouseClicked(1) && in_glass) {
                    a.transition_type = TransitionType::None;
                    a.transition_pre  = 0.f; a.transition_post = 0.f;
                    s_glass_drag = 0;
                    s_trans_track = -1; s_trans_left_ci = -1;
                    history_push(state, "Remove transition");
                    s_trans_hit_this_frame = true;
                }
            }
        }

        // Glass drag update (runs once per frame, outside clip loop)
        if (s_glass_drag != 0 && s_trans_track == ti &&
            s_trans_left_ci >= 0 && s_trans_left_ci < (int)track.clips.size()) {
            Clip& a = track.clips[s_trans_left_ci];
            Clip* b_ptr = (s_trans_left_ci + 1 < (int)track.clips.size())
                          ? &track.clips[s_trans_left_ci + 1] : nullptr;
            float dx = (mouse.x - s_glass_drag_ref_x) / zoom;

            if (ImGui::IsMouseDragging(0)) {
                if (s_glass_drag == 1) {
                    // Left handle → adjust pre (drag left = more pre)
                    float new_pre = fmaxf(0.01f, fminf(s_glass_drag_ref_pre - dx,
                                                       a.end - a.start - 0.05f));
                    a.transition_pre = new_pre;
                } else if (s_glass_drag == 2) {
                    // Right handle → adjust post
                    float new_post = fmaxf(0.01f, fminf(s_glass_drag_ref_post + dx,
                                                        b_ptr ? (b_ptr->end - b_ptr->start - 0.05f) : 10.f));
                    a.transition_post = new_post;
                } else {
                    // Body drag → move linked pair
                    float new_start = fmaxf(0.f, s_glass_drag_ref_start + dx);
                    float dur_a = a.end - a.start;
                    a.start = new_start; a.end = new_start + dur_a;
                    if (b_ptr) {
                        float dur_b = b_ptr->end - b_ptr->start;
                        b_ptr->start = a.end; b_ptr->end = a.end + dur_b;
                    }
                }
            }
            // Single click (no meaningful drag) on glass body → open type picker
            if (ImGui::IsMouseReleased(0)) {
                if (s_glass_drag == 3 && fabsf(mouse.x - s_glass_drag_ref_x) < 4.f) {
                    float cut_x2 = origin.x + TL_LABEL_W + a.end * zoom - scroll;
                    s_trans_popup_pos = {cut_x2 - 80.f, track_y - 8.f};
                    ImGui::OpenPopup("##trans_picker");
                } else if (s_glass_drag != 3) {
                    history_push(state, "Adjust transition");
                }
                s_glass_drag = 0;
            }
        }

        track_y += TL_TRACK_H;
    }

    // "+ Add Track" — scrolls with the track list as the last row
    {
        ImVec2 row_tl = {origin.x,           track_y};
        ImVec2 row_br = {origin.x + total_w,  track_y + TL_TRACK_H};
        bool add_hov = mouse.y >= track_y && mouse.y < track_y + TL_TRACK_H &&
                       mouse.x >= origin.x && mouse.x <= origin.x + total_w;
        dl->AddRectFilled(row_tl, {origin.x + TL_LABEL_W, track_y + TL_TRACK_H},
                          to_u32(add_hov ? Col::bg_soft_hov : Col::bg_soft));
        dl->AddRectFilled({origin.x + TL_LABEL_W, track_y}, row_br, to_u32(Col::bg));
        dl->AddLine(row_tl, {origin.x + total_w, track_y}, to_u32(Col::line));
        dl->AddLine({origin.x + TL_LABEL_W, track_y}, {origin.x + TL_LABEL_W, track_y + TL_TRACK_H},
                    to_u32(Col::line));
        float lh = ImGui::GetTextLineHeight();
        dl->AddText({origin.x + 8.f, track_y + (TL_TRACK_H - lh) * 0.5f},
                    to_u32(add_hov ? Col::fg : Col::muted), "+ Add Track");
        if (add_hov && ImGui::IsMouseClicked(0)) {
            Track t;
            char name[32]; snprintf(name, sizeof(name), "Track %d", (int)state.tracks.size() + 1);
            t.name = name; state.tracks.insert(state.tracks.begin(), std::move(t));
        }
    }

    dl->PopClipRect();  // end scrollable track area clip

    // ── Track reorder drag ────────────────────────────────────────────────────
    if (s_track_drag_src >= 0) {
        if (ImGui::IsMouseDown(0)) {
            if (!s_track_dragging && fabsf(mouse.y - s_track_drag_start_y) > 4.f)
                s_track_dragging = true;
            if (s_track_dragging) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                float rel = mouse.y - (origin.y + TL_RULER_H);
                int   ins = (int)roundf(rel / TL_TRACK_H);
                s_track_drag_insert = std::clamp(ins, 0, (int)state.tracks.size());
            }
        } else {
            if (s_track_dragging && s_track_drag_insert >= 0) {
                int src = s_track_drag_src;
                int dst = s_track_drag_insert;
                if (dst > src) dst--;
                if (dst != src && dst >= 0 && dst < (int)state.tracks.size()) {
                    Track moved = std::move(state.tracks[src]);
                    state.tracks.erase(state.tracks.begin() + src);
                    state.tracks.insert(state.tracks.begin() + dst, std::move(moved));
                    if      (state.selected_track == src)                                       state.selected_track = dst;
                    else if (src < dst && state.selected_track > src && state.selected_track <= dst) state.selected_track--;
                    else if (src > dst && state.selected_track >= dst && state.selected_track < src) state.selected_track++;
                    history_push(state, "Reorder track");
                }
            }
            s_track_drag_src    = -1;
            s_track_dragging    = false;
            s_track_drag_insert = -1;
        }
    }

    if (s_track_dragging && s_track_drag_src >= 0 &&
        s_track_drag_src < (int)state.tracks.size()) {
        // Insert line
        float ins_y = origin.y + TL_RULER_H + s_track_drag_insert * TL_TRACK_H;
        dl->AddLine({origin.x, ins_y}, {origin.x + total_w, ins_y},
                    IM_COL32(120, 200, 255, 220), 2.f);
        dl->AddCircleFilled({origin.x + 5.f, ins_y}, 4.f, IM_COL32(120, 200, 255, 220));

        // Ghost label following cursor
        float gy0 = mouse.y - TL_TRACK_H * 0.5f;
        float gy1 = gy0 + TL_TRACK_H;
        dl->AddRectFilled({origin.x, gy0}, {origin.x + TL_LABEL_W, gy1},
                          IM_COL32(20, 20, 20, 230), 3.f);
        dl->AddRect({origin.x, gy0}, {origin.x + TL_LABEL_W, gy1},
                    IM_COL32(120, 200, 255, 200), 3.f, 0, 1.5f);
        dl->AddText({origin.x + 8.f, gy0 + (TL_TRACK_H - ImGui::GetTextLineHeight()) * 0.5f},
                    IM_COL32(255, 255, 255, 255),
                    state.tracks[s_track_drag_src].name.c_str());
    }

    // ── Box select ───────────────────────────────────────────────────────────
    {
        bool in_body = mouse.x > origin.x+TL_LABEL_W && mouse.x < origin.x+total_w &&
                       mouse.y > origin.y+TL_RULER_H  && mouse.y < origin.y+total_h;
        bool ldown  = ImGui::IsMouseDown(0);
        bool lclick = ImGui::IsMouseClicked(0);

        // Start box select when clicking empty body space (no clip was hit)
        if (lclick && in_body && !s_clip_hit && !ImGui::IsAnyItemActive()) {
            s_box_selecting = true;
            s_box_start     = mouse;
            if (!ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) {
                state.clip_selection.clear();
                state.selected_track = -1;
                state.selected_clip  = -1;
            }
        }
        s_clip_hit = false; // reset for next frame

        if (s_box_selecting) {
            if (ldown) {
                // Draw rubber-band rect
                float bx0 = fminf(s_box_start.x, mouse.x);
                float bx1 = fmaxf(s_box_start.x, mouse.x);
                float by0 = fminf(s_box_start.y, mouse.y);
                float by1 = fmaxf(s_box_start.y, mouse.y);
                dl->AddRectFilled({bx0,by0},{bx1,by1}, IM_COL32(120,200,255,30));
                dl->AddRect({bx0,by0},{bx1,by1}, IM_COL32(120,200,255,180), 0.f, 0, 1.f);

                // Live update selection while dragging
                float t0 = (bx0 - origin.x - TL_LABEL_W + scroll) / zoom;
                float t1 = (bx1 - origin.x - TL_LABEL_W + scroll) / zoom;
                int   tr0 = (int)((by0 - origin.y - TL_RULER_H) / TL_TRACK_H);
                int   tr1 = (int)((by1 - origin.y - TL_RULER_H) / TL_TRACK_H);

                if (!ImGui::GetIO().KeyCtrl) state.clip_selection.clear();
                for (int t2=0; t2<(int)state.tracks.size(); ++t2) {
                    if (t2 < tr0 || t2 > tr1) continue;
                    for (int c2=0; c2<(int)state.tracks[t2].clips.size(); ++c2) {
                        const Clip& bc = state.tracks[t2].clips[c2];
                        if (bc.end > t0 && bc.start < t1)
                            state.clip_selection.insert({t2, c2});
                    }
                }
            } else {
                // Released — finalise, update focus to first selected clip
                s_box_selecting = false;
                if (!state.clip_selection.empty()) {
                    auto first = *state.clip_selection.begin();
                    state.selected_track = first.first;
                    state.selected_clip  = first.second;
                }
            }
        }

        // Escape clears selection
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && s_rename_track < 0) {
            state.clip_selection.clear();
            state.selected_track = -1;
            state.selected_clip  = -1;
            s_box_selecting = false;
        }
    }

    // ── Transition picker popup ───────────────────────────────────────────────
    {
        ImVec2 disp = ImGui::GetIO().DisplaySize;
        constexpr float PW = 200.f, PH = 160.f;
        ImVec2 pp = s_trans_popup_pos;
        pp.x = fmaxf(4.f, fminf(pp.x, disp.x - PW - 4.f));
        pp.y = fmaxf(4.f, fminf(pp.y, disp.y - PH - 4.f));
        ImGui::SetNextWindowPos(pp, ImGuiCond_Appearing);
    }
    ImGui::SetNextWindowSize({200.f, 0.f}, ImGuiCond_Appearing);
    if (ImGui::BeginPopup("##trans_picker")) {
        if (s_trans_track >= 0 && s_trans_left_ci >= 0 &&
            s_trans_track < (int)state.tracks.size() &&
            s_trans_left_ci < (int)state.tracks[s_trans_track].clips.size()) {
            Clip& tc = state.tracks[s_trans_track].clips[s_trans_left_ci];

            struct TBtn { TransitionType t; const char* l; };
            TBtn btns[] = {
                {TransitionType::Dissolve, "Dissolve"},
                {TransitionType::FadeBlack,"Fade to Black"},
                {TransitionType::DipWhite, "Dip to White"},
            };
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4.f, 4.f});
            for (auto& b : btns) {
                bool sel = (tc.transition_type == b.t);
                if (sel) ImGui::PushStyleColor(ImGuiCol_Button, to_u32(Col::fg));
                if (ImGui::Button(b.l, {ImGui::GetContentRegionAvail().x, 0.f})) {
                    tc.transition_type = b.t;
                    if (tc.transition_pre  <= 0.f) tc.transition_pre  = 0.25f;
                    if (tc.transition_post <= 0.f) tc.transition_post = 0.25f;
                    history_push(state, "Transition");
                    ImGui::CloseCurrentPopup();
                }
                if (sel) ImGui::PopStyleColor();
            }
            ImGui::Dummy({0.f, 2.f});
            if (tc.transition_type != TransitionType::None) {
                ImGui::Separator();
                ImGui::Dummy({0.f, 2.f});
                if (ImGui::Button("Remove", {ImGui::GetContentRegionAvail().x, 0.f})) {
                    tc.transition_type = TransitionType::None;
                    tc.transition_pre  = 0.f; tc.transition_post = 0.f;
                    history_push(state, "Remove transition");
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::PopStyleVar();
        }
        ImGui::EndPopup();
    }

    // ── Past-end dim overlay ──────────────────────────────────────────────────
    {
        float end_x = origin.x + TL_LABEL_W + dur * zoom - scroll;
        if (end_x < origin.x + total_w) {
            float dim_x = fmaxf(end_x, origin.x + TL_LABEL_W);
            // Ruler area
            dl->AddRectFilled({dim_x, origin.y},
                {origin.x + total_w, origin.y + TL_RULER_H},
                IM_COL32(0, 0, 0, 110));
            // Track rows
            dl->AddRectFilled({dim_x, origin.y + TL_RULER_H},
                {origin.x + total_w, origin.y + total_h},
                IM_COL32(0, 0, 0, 85));
            // Subtle end-of-project line
            if (end_x >= origin.x + TL_LABEL_W)
                dl->AddLine({end_x, origin.y},
                    {end_x, origin.y + total_h},
                    IM_COL32(255, 255, 255, 50));
        }
    }

    // Drag handling (frame-snapped + edge-snapped, Ctrl bypasses both)
    if (drag_track>=0 && drag_clip>=0 && (drag_left||drag_right))
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (drag_track>=0 && drag_clip>=0 && s_glass_drag==0 && ImGui::IsMouseDragging(0)) {
        Clip& dc = state.tracks[drag_track].clips[drag_clip];
        auto cands = build_snap_candidates(drag_track, drag_clip);
        float new_t = (mouse.x - origin.x - TL_LABEL_W + scroll - drag_offset) / zoom;
        // Helper: find transition-linked neighbour clip on same track (same-track adjacency only)
        auto linked_right = [&]() -> Clip* {
            auto& clips = state.tracks[drag_track].clips;
            if (drag_clip + 1 >= (int)clips.size()) return nullptr;
            Clip& nb = clips[drag_clip + 1];
            if (nb.clip_type != ClipType::Video) return nullptr;
            return (dc.transition_type != TransitionType::None) ? &nb : nullptr;
        };
        auto linked_left = [&]() -> Clip* {
            if (drag_clip <= 0) return nullptr;
            Clip& nb = state.tracks[drag_track].clips[drag_clip - 1];
            if (nb.clip_type != ClipType::Video) return nullptr;
            return (nb.transition_type != TransitionType::None) ? &nb : nullptr;
        };

        // Inner edges (shared with a transition) are locked — only outer edges trim
        bool right_locked = (dc.clip_type == ClipType::Video &&
                             dc.transition_type != TransitionType::None);
        bool left_locked  = (dc.clip_type == ClipType::Video &&
                             linked_left() != nullptr);

        if (drag_left && !left_locked) {
            float t = edge_snap(snap(new_t), cands);
            float new_start = fmaxf(0.f, fminf(t, dc.end - f1));
            dc.in_point = fmaxf(0.f, dc.in_point + (new_start - dc.start));
            dc.start = new_start;
        } else if (drag_right && !right_locked) {
            float et = (mouse.x - origin.x - TL_LABEL_W + scroll) / zoom;
            float t = edge_snap(snap(et), cands);
            dc.end = fmaxf(dc.start + f1, t);
        } else if (!drag_left && !drag_right) {
            float dur_clip = dc.end - dc.start;
            // Try snapping both edges; use whichever is closer to a candidate.
            float left_raw  = snap(new_t);
            float right_raw = left_raw + dur_clip;
            float thresh = SNAP_PX / zoom;
            float best_dt = thresh;
            float best_start = left_raw;
            s_snap_indicator = -1.f;
            if (s_snap_enabled && !ImGui::GetIO().KeyCtrl) {
                for (float c : cands) {
                    float dl = fabsf(c - left_raw);
                    if (dl < best_dt) { best_dt = dl; best_start = c;           s_snap_indicator = c; }
                    float dr = fabsf(c - right_raw);
                    if (dr < best_dt) { best_dt = dr; best_start = c - dur_clip; s_snap_indicator = c; }
                }
            }
            dc.start = fmaxf(0.f, best_start);
            dc.end   = dc.start + dur_clip;
            // Co-move transition-linked neighbours
            if (Clip* nb = linked_right()) { float d = nb->end - nb->start; nb->start = dc.end;   nb->end = nb->start + d; }
            if (Clip* nb = linked_left())  { float d = nb->end - nb->start; nb->end   = dc.start; nb->start = fmaxf(0.f, nb->end - d); }
            // Track which row the mouse is hovering for cross-track transfer.
            // hot == tracks.size() means below all tracks → "New Track" ghost row.
            // drag_hot_gap >= 0 means between two existing tracks → insert new track there.
            float ruler_bottom = origin.y + TL_RULER_H;
            int n_tracks = (int)state.tracks.size();
            const float GAP_PX = 8.f;
            drag_hot_gap = -1;
            for (int gi = 0; gi < n_tracks; ++gi) {
                float boundary_y = ruler_bottom + gi * TL_TRACK_H;
                if (fabsf(mouse.y - boundary_y) < GAP_PX) {
                    drag_hot_gap = gi;
                    break;
                }
            }
            if (drag_hot_gap >= 0) {
                drag_hot_track = -1;
            } else {
                int hot = (int)((mouse.y - ruler_bottom) / TL_TRACK_H);
                drag_hot_track = (hot >= 0 && hot <= n_tracks) ? hot : -1;
            }
        }
    } else {
        s_snap_indicator = -1.f;
    }
    if (ImGui::IsMouseReleased(0)) {
        if (drag_track >= 0 && drag_clip >= 0) {
            if (!drag_left && !drag_right && drag_hot_gap >= 0) {
                // Drop into gap between tracks — insert new track there
                Clip moved = state.tracks[drag_track].clips[drag_clip];
                state.tracks[drag_track].clips.erase(
                    state.tracks[drag_track].clips.begin() + drag_clip);
                Track nt;
                char name[32];
                snprintf(name, sizeof(name), "Track %d", (int)state.tracks.size() + 1);
                nt.name = name;
                nt.clips.push_back(moved);
                state.tracks.insert(state.tracks.begin() + drag_hot_gap, std::move(nt));
                state.selected_track = drag_hot_gap;
                state.selected_clip  = 0;
                history_push(state, "Move clip to new track");
            } else if (!drag_left && !drag_right &&
                drag_hot_track >= 0 && drag_hot_track != drag_track) {
                Clip moved = state.tracks[drag_track].clips[drag_clip];
                state.tracks[drag_track].clips.erase(
                    state.tracks[drag_track].clips.begin() + drag_clip);
                if (drag_hot_track == (int)state.tracks.size()) {
                    // Drop below all tracks — create new track
                    Track nt;
                    char name[32];
                    snprintf(name, sizeof(name), "Track %d", (int)state.tracks.size() + 1);
                    nt.name = name;
                    nt.clips.push_back(moved);
                    state.tracks.push_back(nt);
                    state.selected_track = (int)state.tracks.size() - 1;
                    state.selected_clip  = 0;
                    history_push(state, "Move clip to new track");
                } else {
                    state.tracks[drag_hot_track].clips.push_back(moved);
                    state.selected_track = drag_hot_track;
                    state.selected_clip  = (int)state.tracks[drag_hot_track].clips.size() - 1;
                    history_push(state, "Move clip to track");
                }
            } else {
                const char* act = drag_left  ? "Trim clip start" :
                                  drag_right ? "Trim clip end"   : "Move clip";
                history_push(state, act);
            }
        }
        drag_track=-1; drag_clip=-1; drag_left=false; drag_right=false;
        drag_hot_track=-1; drag_hot_gap=-1;
    }

    // Playhead
    float ph_x = origin.x+TL_LABEL_W+state.playhead*zoom-scroll;
    if (ph_x >= origin.x+TL_LABEL_W && ph_x <= origin.x+total_w) {
        dl->AddLine({ph_x, origin.y}, {ph_x, origin.y+total_h}, to_u32(Col::fg));
        dl->AddTriangleFilled({ph_x-5.f,origin.y},{ph_x+5.f,origin.y},{ph_x,origin.y+10.f},to_u32(Col::fg));
    }

    // Click ruler to seek (frame-snapped + edge-snapped, Ctrl bypasses both)
    bool any_popup_global = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (!any_popup_global && (ImGui::IsMouseClicked(0)||ImGui::IsMouseDragging(0)) && drag_track<0) {
        if (mouse.y>=origin.y && mouse.y<=origin.y+TL_RULER_H &&
            mouse.x>=origin.x+TL_LABEL_W && mouse.x<=origin.x+total_w) {
            float raw = (mouse.x - origin.x - TL_LABEL_W + scroll) / zoom;
            auto cands = build_snap_candidates(drag_track, drag_clip);
            // For ruler seek, snap to clip edges (exclude playhead from candidates)
            std::vector<float> edge_cands;
            for (int ti = 0; ti < (int)state.tracks.size(); ++ti)
                for (auto& cl : state.tracks[ti].clips) {
                    edge_cands.push_back(cl.start);
                    edge_cands.push_back(cl.end);
                }
            edge_cands.push_back(0.f);
            float t = edge_snap(snap(fmaxf(0.f, fminf(raw, dur))), edge_cands);
            seek_to(state, t);
        }
    }

    // ── Snap indicator line ───────────────────────────────────────────────────
    if (s_snap_indicator >= 0.f) {
        float sx = origin.x + TL_LABEL_W + s_snap_indicator * zoom - scroll;
        if (sx >= origin.x + TL_LABEL_W && sx <= origin.x + total_w) {
            dl->AddLine({sx, origin.y + TL_RULER_H}, {sx, origin.y + total_h},
                        IM_COL32(120, 200, 255, 200), 1.f);
        }
    }

    // ── Between-track gap ghost (insert new track between two existing ones) ────
    if (drag_track >= 0 && drag_clip >= 0 && !drag_left && !drag_right &&
        drag_hot_gap >= 0 && drag_hot_gap < (int)state.tracks.size()) {
        float gap_y = origin.y + TL_RULER_H + drag_hot_gap * TL_TRACK_H;
        dl->AddLine({origin.x, gap_y}, {origin.x + total_w, gap_y},
                    IM_COL32(120, 200, 255, 220), 2.f);
        dl->AddCircleFilled({origin.x + 5.f, gap_y}, 4.f, IM_COL32(120, 200, 255, 220));
        float lh = ImGui::GetTextLineHeight();
        float label_off = (drag_hot_gap == 0) ? 3.f : (-lh - 3.f);
        dl->AddText({origin.x + 14.f, gap_y + label_off},
                    IM_COL32(120, 200, 255, 200), "New Track");
    }

    // ── New-track ghost row (shown when dragging below all tracks) ────────────
    if (drag_track >= 0 && drag_clip >= 0 && !drag_left && !drag_right &&
        drag_hot_track == (int)state.tracks.size() &&
        drag_track < (int)state.tracks.size() &&
        drag_clip  < (int)state.tracks[drag_track].clips.size()) {
        ImVec2 gr_tl = {origin.x,           track_y};
        ImVec2 gr_br = {origin.x + total_w,  track_y + TL_TRACK_H};
        // Dashed outline row
        dl->AddRectFilled(gr_tl, gr_br, IM_COL32(255,255,255,8));
        dl->AddRect(gr_tl, gr_br, IM_COL32(255,255,255,60), 0.f, 0, 1.f);
        // "New Track" label
        dl->AddText({origin.x + 8.f, track_y + (TL_TRACK_H - ImGui::GetTextLineHeight()) * 0.5f},
                    IM_COL32(255,255,255,100), "New Track");
        // Clip ghost
        const Clip& gc = state.tracks[drag_track].clips[drag_clip];
        float gx0 = fmaxf(origin.x+TL_LABEL_W, origin.x+TL_LABEL_W+gc.start*zoom-scroll);
        float gx1 = fminf(origin.x+total_w,     origin.x+TL_LABEL_W+gc.end*zoom-scroll);
        float gy0 = track_y + 3.f, gy1 = track_y + TL_TRACK_H - 3.f;
        if (gx1 > gx0) {
            dl->AddRectFilled({gx0,gy0},{gx1,gy1}, IM_COL32(255,255,255,35), 2.f);
            dl->AddRect({gx0,gy0},{gx1,gy1}, IM_COL32(255,255,255,160), 2.f, 0, 1.5f);
        }
        track_y += TL_TRACK_H;  // push "+ Add Track" down so they don't overlap
    }


    // ── Context menus ─────────────────────────────────────────────────────────

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.f, 6.f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   {8.f, 4.f});

    if (ImGui::BeginPopup("##clip_ctx")) {
        open_clip_ctx = false;
        int ti = ctx_track, ci = ctx_clip;
        bool valid = ti>=0 && ti<(int)state.tracks.size() &&
                     ci>=0 && ci<(int)state.tracks[ti].clips.size();
        Track* ct = valid ? &state.tracks[ti] : nullptr;
        Clip*  cc = valid ? &ct->clips[ci]    : nullptr;

        // ── Extract raw audio (video clips only) ─────────────────────────────
        if (cc && cc->clip_type==ClipType::Video) {
            bool ext_busy = state.extract_running;
            if (ext_busy) ImGui::BeginDisabled();
            if (ImGui::MenuItem(ext_busy ? "Extracting audio…" : "Extract audio as track")) {
                state.extract_source_track = ctx_track;
                extract_audio_start(state, cc->text);
            }
            if (ext_busy) ImGui::EndDisabled();
            ImGui::Separator();
        }

        // ── ML Processing — Audio & Video clips ──────────────────────────────
        if (cc && (cc->clip_type==ClipType::Audio || cc->clip_type==ClipType::Video)) {
            bool busy = transcribe_running();
            if (busy || !state.models_ready) ImGui::BeginDisabled();
            if (ImGui::BeginMenu("ML Processing")) {
                if (ImGui::MenuItem("Extract Lyrics  (separate + transcribe)")) {
                    state.audio_path = cc->text;
                    kick_pipeline(state, cc->text, PipelineMode::Both);
                }
                if (ImGui::MenuItem("Extract Subtitles  (transcribe only)")) {
                    state.audio_path = cc->text;
                    kick_pipeline(state, cc->text, PipelineMode::TranscribeOnly);
                }
                if (ImGui::MenuItem("Separate Vocals  (Demucs)")) {
                    kick_pipeline(state, cc->text, PipelineMode::SeparateOnly);
                }
                ImGui::EndMenu();
            }
            if (busy || !state.models_ready) ImGui::EndDisabled();
            if (!state.models_ready) {
                if (ImGui::MenuItem("Download Lyric Extraction Models…"))
                    state.show_model_dl_modal = true;
            }
            ImGui::Separator();
        }

        // ── Subtitle clips ────────────────────────────────────────────────────
        if (cc && (cc->clip_type==ClipType::Text || cc->clip_type==ClipType::Lyrics)) {
            if (ImGui::MenuItem("Edit text")) {
                state.panel_tab = 0;
                s_edit_focus_next = true;
            }
            // Re-group submenu — only if we have source JSON
            bool has_json = !state.words_json_path.empty() &&
                            fs::exists(state.words_json_path);
            if (ImGui::BeginMenu("Re-group track as…")) {
                struct { SubtitleMode m; const char* label; } rmodes[] = {
                    {SubtitleMode::Word,    "Word-by-word"},
                    {SubtitleMode::Phrase,  "Phrase  (pauses >0.3s)"},
                    {SubtitleMode::Line,    "Line  (gaps >0.8s)"},
                    {SubtitleMode::Karaoke, "Karaoke  (line + per-word highlight)"},
                    {SubtitleMode::Segment, "Segment  (sentence)"},
                    {SubtitleMode::CustomN, "Custom N words"},
                };
                for (auto& rm : rmodes) {
                    bool cur = state.subtitle_mode == rm.m;
                    if (!has_json) ImGui::BeginDisabled();
                    if (ImGui::MenuItem(rm.label, nullptr, cur)) {
                        state.subtitle_mode = rm.m;
                        apply_subtitle_mode(state);
                        history_push(state, std::string("Grouping — ") + rm.label);
                    }
                    if (!has_json) ImGui::EndDisabled();
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
        }

        if (ImGui::MenuItem("Split at playhead", "S")) {
            if (valid) {
                float cut = state.playhead;
                if (cut > cc->start+0.02f && cut < cc->end-0.02f) {
                    Clip right = *cc; cc->end = cut; right.start = cut;
                    ct->clips.insert(ct->clips.begin()+ci+1, right);
                    history_push(state, "Split clip");
                }
            }
        }
        if (ImGui::MenuItem("Duplicate clip")) {
            if (valid) {
                Clip dup = *cc; dup.start = cc->end; dup.end = dup.start + (cc->end - cc->start);
                ct->clips.insert(ct->clips.begin()+ci+1, dup);
                history_push(state, "Duplicate clip");
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Seek to clip start")) {
            if (valid) { seek_to(state, cc->start); }
        }
        if (ImGui::MenuItem("Seek to clip end")) {
            if (valid) { seek_to(state, cc->end); }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete clip")) {
            if (valid) {
                ct->clips.erase(ct->clips.begin()+ci);
                state.selected_clip = -1;
                history_push(state, "Delete clip");
            }
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##track_ctx")) {
        open_track_ctx = false;
        int ti = ctx_track;
        bool valid = ti>=0 && ti<(int)state.tracks.size();
        Track* ct = valid ? &state.tracks[ti] : nullptr;

        if (ct) {
            // Rename — inline edit
            static char rename_buf[64] = {};
            static bool rename_open = false;
            if (!rename_open) { strncpy(rename_buf, ct->name.c_str(), 63); }
            ImGui::SetNextItemWidth(140.f);
            if (ImGui::InputText("##rename", rename_buf, sizeof(rename_buf),
                    ImGuiInputTextFlags_EnterReturnsTrue)) {
                ct->name = rename_buf;
                history_push(state, "Rename track");
                ImGui::CloseCurrentPopup();
            }
            rename_open = ImGui::IsItemActive();
            ImGui::Separator();
        }

        // ── Add clip to this track ────────────────────────────────────────────
        if (valid) {
            if (ImGui::MenuItem("Add Text Clip")) {
                add_clip_to_track(state, ti, "", ClipType::Text);
                s_edit_focus_next = true;
            }
            if (ImGui::MenuItem("Add Video Clip…")) {
                std::string p = filepicker_open("Add video clip",
                    "Video", "*.mp4 *.mov *.mkv *.avi *.webm");
                if (!p.empty()) add_clip_to_track(state, ti, p, ClipType::Video);
            }
            if (ImGui::MenuItem("Add Audio Clip…")) {
                std::string p = filepicker_open("Add audio clip",
                    "Audio", "*.wav *.mp3 *.m4a *.flac *.aac");
                if (!p.empty()) add_clip_to_track(state, ti, p, ClipType::Audio);
            }
            if (ImGui::MenuItem("Add Effect Clip")) {
                add_clip_to_track(state, ti, "", ClipType::Effect);
            }
            ImGui::Separator();
        }

        if (valid && ti > 0) {
            if (ImGui::MenuItem("Move up")) {
                std::swap(state.tracks[ti], state.tracks[ti-1]);
                state.selected_track = ti-1;
                history_push(state, "Move track up");
            }
        }
        if (valid && ti < (int)state.tracks.size()-1) {
            if (ImGui::MenuItem("Move down")) {
                std::swap(state.tracks[ti], state.tracks[ti+1]);
                state.selected_track = ti+1;
                history_push(state, "Move track down");
            }
        }
        ImGui::Separator();
        if (ct) {
            if (ImGui::MenuItem(ct->visible ? "Hide track" : "Show track")) {
                ct->visible = !ct->visible;
                history_push(state, "Track visibility");
            }
            if (ImGui::MenuItem(ct->muted ? "Unmute" : "Mute")) {
                ct->muted = !ct->muted;
                history_push(state, "Track mute");
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete track", nullptr, false, valid)) {
            std::string tname = ct ? ct->name : "";
            if (state.selected_track == ti) { state.selected_track=-1; state.selected_clip=-1; }
            state.tracks.erase(state.tracks.begin()+ti);
            history_push(state, "Delete track — " + tname);
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##tl_ctx")) {
        open_tl_ctx = false;
        if (ImGui::MenuItem("Add Track")) {
            Track t;
            char n[32]; snprintf(n,sizeof(n),"Track %d",(int)state.tracks.size()+1);
            t.name=n; state.tracks.insert(state.tracks.begin(), std::move(t));
            history_push(state, "Add Track");
        }
        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(2);
    (void)open_clip_ctx; (void)open_track_ctx; (void)open_tl_ctx;
}

// ── Split / delete keyboard shortcuts ────────────────────────────────────────

static void handle_shortcuts(AppState& state) {
    if (ImGui::IsAnyItemActive()) return;
    ImGuiIO& io = ImGui::GetIO();

    // ── Undo / Redo ───────────────────────────────────────────────────────────
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) {
        history_undo(state); return;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) ||
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) {
        history_redo(state); return;
    }

    // ── Save / Open ───────────────────────────────────────────────────────────
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
        if (state.project_path.empty())
            state.project_path = filepicker_save("Save project", "PMS Project", "*.pms");
        if (!state.project_path.empty()) project_save(state, state.project_path);
        return;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S)) {
        std::string p = filepicker_save("Save project as", "PMS Project", "*.pms");
        if (!p.empty()) { state.project_path = p; project_save(state, p); }
        return;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) {
        transcribe_cancel(); history_clear();
        audio_shutdown(); audio_clips_clear(); video_close();
        state = AppState{}; state.splash_timer = 0.f;
        audio_init();
        return;
    }

    // ── Playback ──────────────────────────────────────────────────────────────
    float fps    = tl_fps(state);
    float f_dt   = fps > 0.f ? 1.f / fps : 1.f / 30.f;
    float dur    = fmaxf(state.duration, 0.f);

    if (ImGui::IsKeyPressed(ImGuiKey_Space) ||
        ImGui::IsKeyPressed(ImGuiKey_K)) {
        toggle_play(state); return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_L) && !state.playing) {
        toggle_play(state); return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        float step = io.KeyShift ? 5.f : f_dt;
        seek_to(state, fminf(state.playhead + step, dur));
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        float step = io.KeyShift ? 5.f : f_dt;
        seek_to(state, fmaxf(state.playhead - step, 0.f));
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) { seek_to(state, 0.f);  return; }
    if (ImGui::IsKeyPressed(ImGuiKey_End))  { seek_to(state, dur);  return; }

    // ── Clip operations (need a selected clip) ────────────────────────────────
    if (state.selected_track<0 || state.selected_clip<0) return;
    if (state.selected_track>=(int)state.tracks.size()) return;
    Track& track = state.tracks[state.selected_track];
    if (state.selected_clip>=(int)track.clips.size()) return;
    Clip& clip = track.clips[state.selected_clip];

    if (ImGui::IsKeyPressed(ImGuiKey_S) ||
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl|ImGuiKey_B)) {
        float cut = state.playhead;
        if (cut > clip.start + f_dt && cut < clip.end - f_dt) {
            Clip right = clip; clip.end = cut; right.start = cut;
            track.clips.insert(track.clips.begin()+state.selected_clip+1, right);
            history_push(state, "Split clip");
        }
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) ||
        ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        track.clips.erase(track.clips.begin()+state.selected_clip);
        state.selected_clip = -1;
        history_push(state, "Delete clip");
        return;
    }
}

// ── Studio ────────────────────────────────────────────────────────────────────

void ui_studio(AppState& state) {
    ImGuiIO& io   = ImGui::GetIO();
    float    win_w = io.DisplaySize.x;
    float    win_h = io.DisplaySize.y;

    // Stop scrub audio once the blip window expires.
    if (s_scrub_until > 0.0 && ImGui::GetTime() >= s_scrub_until && !state.playing) {
        audio_pause();
        s_scrub_until = 0.0;
    }

    handle_shortcuts(state);

    // Keep state.duration in sync with actual clip content every frame.
    if (!state.tracks.empty()) {
        float pe = project_end(state);
        if (pe > 0.01f) state.duration = pe;
    }

    // Handle OS drop — SRT, audio, or video.
    extern std::string g_dropped_file;
    if (!g_dropped_file.empty()) {
        const std::string& dp = g_dropped_file;
        fs::path fp(dp);
        std::string ext = fp.extension().string();
        for (auto& c : ext) c = (char)tolower((unsigned char)c);

        if (ext == ".srt") {
            // Parse SRT and place clips on hovered track or a new track.
            auto clips = parse_srt(dp);
            if (!clips.empty()) {
                int target = -1;
                if (s_tl_hover_track >= 0 && s_tl_hover_track < (int)state.tracks.size()) {
                    target = s_tl_hover_track;
                    Track& tr = state.tracks[target];
                    tr.clips.insert(tr.clips.end(), clips.begin(), clips.end());
                } else {
                    Track t;
                    t.name = fp.stem().string();
                    t.clips = clips;
                    state.tracks.insert(state.tracks.begin(), std::move(t));
                    target = 0;
                }
                s_drop_flash_track = target;
                s_drop_flash_t     = 0.6f;
                history_push(state, "Import SRT \"" + fp.filename().string() + "\"");
            }
        } else if (is_audio_file(dp)) {
            bool is_vid = (ext==".mp4"||ext==".mov"||ext==".mkv"||ext==".avi"||ext==".webm");
            ClipType drop_ct = is_vid ? ClipType::Video : ClipType::Audio;

            if (s_tl_hover_track >= 0 && s_tl_hover_track < (int)state.tracks.size()) {
                add_clip_to_track(state, s_tl_hover_track, dp, drop_ct);
                if (state.audio_path.empty()) {
                    state.audio_path = dp;
                    audio_load(dp);
                    if (state.duration <= 0.f) state.duration = audio_duration();
                }
                s_drop_flash_track = s_tl_hover_track;
            } else {
                import_file(state, dp);
                s_drop_flash_track = (int)state.tracks.size() - 1;
            }
            s_drop_flash_t = 0.6f;
        }
        g_dropped_file.clear();
    }

    // Proxy ready → upgrade from still to proxy for each registered slot.
    for (int slot = 0; slot < MAX_VIDEO_TRACKS; ++slot) {
        const std::string& key = state.proxy_paths[slot];
        if (key.empty()) continue;
        if (!video_is_open(slot) || video_info(slot).fps > 0.0) continue;
        std::string src = source_from_key(key);
        if (!proxy_is_ready(src)) continue;

        ProxyInfo pi;
        if (!proxy_load(src, pi)) continue;
        video_open_proxy(slot, pi);

        if (slot == 0) {
            state.proxy_ready = true;
            float pd = (float)video_info(0).duration;
            if (pd > 0.f) {
                for (auto& tr : state.tracks)
                    for (auto& cl : tr.clips)
                        if (cl.clip_type == ClipType::Video && cl.text == src && cl.end < pd)
                            cl.end = pd;
            }
        }
    }

    // GC slots whose clips have been deleted or moved.
    gc_video_slots(state);

    // Audio just finished loading while playing — start from current playhead
    {
        static bool s_was_loading = false;
        bool loading = audio_loading();
        if (s_was_loading && !loading && state.playing) {
            state.play_start_pos  = state.playhead;
            state.play_start_wall = std::chrono::steady_clock::now();
            audio_seek(state.playhead);
            audio_play();
        }
        s_was_loading = loading;
    }

    // Extract audio done → add Audio track directly below the source video track
    if (state.extract_done) {
        state.extract_done = false;
        if (!state.extract_wav_path.empty() && fs::exists(state.extract_wav_path)) {
            Track at;
            at.name = fs::path(state.extract_wav_path).stem().string();
            AudioMeta meta;
            float dur = audio_probe(state.extract_wav_path, meta) ? meta.duration_secs : state.duration;
            Clip ac; ac.clip_type = ClipType::Audio;
            ac.start = 0.f; ac.end = dur; ac.text = state.extract_wav_path;
            at.clips.push_back(ac);
            // Insert directly below the source video track; fall back to bottom
            int insert_pos = (int)state.tracks.size();
            if (state.extract_source_track >= 0 &&
                state.extract_source_track < (int)state.tracks.size())
                insert_pos = state.extract_source_track + 1;
            state.tracks.insert(state.tracks.begin() + insert_pos, std::move(at));
            state.extract_source_track = -1;
            history_push(state, "Extract audio from video");
        }
        state.extract_wav_path.clear();
    }

    // Pipeline done → apply grouping + save all SRTs + push history
    static PipelineStage last_stage = PipelineStage::Idle;
    if (last_stage != PipelineStage::Done &&
        state.pipeline.stage == PipelineStage::Done) {
        load_words_cache(state);
        if (state.pipeline_produces_subtitles)
            apply_subtitle_pipeline(state);
        else
            apply_subtitle_mode(state);
        save_all_srts(state);
        std::string stem = state.audio_path.empty() ? "audio"
            : fs::path(state.audio_path).stem().string();
        history_push(state, "Pipeline complete — " + stem);
        // Auto-trigger beat detection and envelope extraction on the stems
        run_beat_detect(state);
        run_envelope_extract(state);
    }
    last_stage = state.pipeline.stage;

    // Push clip snapshots to audio system every frame.
    // The callback reads these to position audio correctly — no separate volume hack needed.
    {
        std::vector<AudioClipDesc> vdescs, adescs;
        for (auto& tr : state.tracks) {
            if (tr.muted) continue;
            for (auto& cl : tr.clips) {
                if (cl.text.empty()) continue;
                AudioClipDesc d;
                d.tl_start = cl.start;    d.tl_end   = cl.end;
                d.in_point = cl.in_point; d.speed    = cl.speed;
                d.volume   = cl.volume;   d.pan      = cl.pan;
                d.fade_in  = cl.fade_in;  d.fade_out = cl.fade_out;
                d.path     = cl.text;
                if      (cl.clip_type == ClipType::Video) vdescs.push_back(d);
                else if (cl.clip_type == ClipType::Audio) adescs.push_back(d);
            }
        }
        video_audio_clips_update(vdescs);
        audio_clips_update(adescs);
    }

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_H))
        state.panel_tab = 3;

    // ── Menu bar ─────────────────────────────────────────────────────────────
    if (ImGui::BeginMenuBar()) {
        // App name
        ImGui::PushFont(g_font_black);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::fg);
        ImGui::TextUnformatted("POP MAKER STUDIO");
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SameLine(0.f, 24.f);

        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Project", "Ctrl+N")) {
                transcribe_cancel();
                history_clear();
                state = AppState{};
                state.splash_timer = 0.f;
                audio_shutdown(); audio_clips_clear(); audio_init();
                video_close();
            }
            if (ImGui::MenuItem("Open Project…", "Ctrl+Shift+O")) {
                std::string picked = filepicker_open("Open project", "PMS Project", "*.pms");
                if (!picked.empty()) {
                    AppState loaded;
                    if (project_load(loaded, picked)) {
                        transcribe_cancel(); history_clear();
                        audio_shutdown(); audio_clips_clear(); video_close();
                        state = std::move(loaded);
                        state.project_path = picked;
                        audio_init();
                        if (!state.audio_path.empty()) audio_load(state.audio_path.c_str());
                        reopen_video_slots(state);
                        // Ensure sources for all Audio clips
                        for (auto& tr : state.tracks)
                            for (auto& cl : tr.clips)
                                if (cl.clip_type == ClipType::Audio && !cl.text.empty())
                                    audio_source_ensure(cl.text);
                    }
                }
            }
            if (ImGui::MenuItem("Save Project", "Ctrl+S")) {
                if (state.project_path.empty())
                    state.project_path = filepicker_save("Save project", "PMS Project", "*.pms");
                if (!state.project_path.empty()) project_save(state, state.project_path);
            }
            if (ImGui::MenuItem("Save Project As…", "Ctrl+Shift+S")) {
                std::string p = filepicker_save("Save project as", "PMS Project", "*.pms");
                if (!p.empty()) { state.project_path = p; project_save(state, p); }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Import Audio / Video…", "Ctrl+O")) {
                std::string picked = filepicker_open(
                    "Import audio or video",
                    "Audio & Video", "*.wav *.mp3 *.m4a *.flac *.aac *.mp4 *.mov *.mkv");
                if (!picked.empty()) import_file(state, picked);
            }
            ImGui::Separator();
            bool has_tracks = !state.tracks.empty();
            if (!has_tracks) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Export SRT")) {
                std::string sp;
                if (!state.audio_path.empty())
                    sp = (fs::path(state.audio_path).parent_path() /
                         (fs::path(state.audio_path).stem().string() + ".srt")).string();
                else sp = "subtitles.srt";
                render_export_srt(state, sp);
                system(("xdg-open \"" + fs::path(sp).parent_path().string() + "\"").c_str());
            }
            if (ImGui::MenuItem("Export Blender Script")) {
                std::string sp = state.audio_path.empty() ? "pop_maker_blender.py" :
                    (fs::path(state.audio_path).parent_path() /
                     (fs::path(state.audio_path).stem().string()+"_blender.py")).string();
                blender_export_script(state, sp);
                system(("xdg-open \"" + fs::path(sp).parent_path().string() + "\"").c_str());
            }
            if (!has_tracks) ImGui::EndDisabled();
            ImGui::Separator();
            if (ImGui::MenuItem("Settings…"))
                state.show_settings_modal = true;
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit")) {
            bool can_undo = history_can_undo();
            bool can_redo = history_can_redo();
            if (!can_undo) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Undo", "Ctrl+Z")) history_undo(state);
            if (!can_undo) ImGui::EndDisabled();
            if (!can_redo) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z")) history_redo(state);
            if (!can_redo) ImGui::EndDisabled();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Track")) {
            if (ImGui::MenuItem("Add Track")) {
                Track t;
                char n[32]; snprintf(n,sizeof(n),"Track %d",(int)state.tracks.size()+1);
                t.name=n; state.tracks.push_back(t);
                history_push(state, "Add Track");
            }
            ImGui::EndMenu();
        }

        bool has_clip = state.selected_track>=0 && state.selected_clip>=0 &&
                        state.selected_track<(int)state.tracks.size() &&
                        state.selected_clip<(int)state.tracks[state.selected_track].clips.size();
        if (ImGui::BeginMenu("Clip")) {
            if (!has_clip) { ImGui::BeginDisabled(); }
            if (ImGui::MenuItem("Split at playhead", "S") && has_clip) {
                Track& t = state.tracks[state.selected_track];
                Clip& c = t.clips[state.selected_clip];
                float cut = state.playhead;
                if (cut>c.start+0.02f && cut<c.end-0.02f) {
                    Clip r=c; c.end=cut; r.start=cut;
                    t.clips.insert(t.clips.begin()+state.selected_clip+1, r);
                    history_push(state, "Split clip");
                }
            }
            if (ImGui::MenuItem("Duplicate clip") && has_clip) {
                Track& t = state.tracks[state.selected_track];
                Clip dup = t.clips[state.selected_clip];
                float len = dup.end - dup.start;
                dup.start = dup.end; dup.end = dup.start+len;
                t.clips.insert(t.clips.begin()+state.selected_clip+1, dup);
                history_push(state, "Duplicate clip");
            }
            if (ImGui::MenuItem("Delete clip", "Del") && has_clip) {
                state.tracks[state.selected_track].clips.erase(
                    state.tracks[state.selected_track].clips.begin()+state.selected_clip);
                state.selected_clip=-1;
                history_push(state, "Delete clip");
            }
            if (!has_clip) ImGui::EndDisabled();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Zoom in",  "Ctrl++")) state.tl_zoom = fminf(state.tl_zoom*1.25f, 4000.f);
            if (ImGui::MenuItem("Zoom out", "Ctrl+-")) state.tl_zoom = fmaxf(state.tl_zoom*0.8f,  20.f);
            if (ImGui::MenuItem("Fit timeline")) {
                if (state.duration > 0.f) {
                    float avail = win_w - TL_LABEL_W - 20.f;
                    state.tl_zoom   = avail / state.duration;
                    state.tl_scroll = 0.f;
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("History", "Ctrl+Shift+H")) state.panel_tab = 3;
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            bool already = state.models_ready;
            if (already) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Download Lyric Extraction Models…"))
                state.show_model_dl_modal = true;
            if (already) ImGui::EndDisabled();
            ImGui::EndMenu();
        }

        // Export button — far right of menu bar
        {
            float btn_w = 80.f;
            float avail = ImGui::GetContentRegionAvail().x;
            if (avail > btn_w)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - btn_w);
            ImGui::PushStyleColor(ImGuiCol_Button,        Col::fg);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col::bg_soft_hov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Col::line);
            ImGui::PushStyleColor(ImGuiCol_Text,          Col::bg);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.f, 2.f});
            if (ImGui::Button("Export")) state.panel_tab = 2;
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(4);
        }

        ImGui::EndMenuBar();
    }

    ui_model_download_modal(state);

    // ── Settings modal ────────────────────────────────────────────────────────
    if (state.show_settings_modal) {
        ImGui::OpenPopup("##settings_modal");
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({480.f, 0.f});
        ImGui::PushStyleColor(ImGuiCol_PopupBg, to_u32(Col::bg));
        ImGui::PushStyleColor(ImGuiCol_Border,  to_u32(Col::line));

        if (ImGui::BeginPopupModal("##settings_modal", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {

            ImGui::Dummy({0.f, 12.f});
            ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
            ImGui::PushFont(g_font_bold);
            ImGui::TextUnformatted("Settings");
            ImGui::PopFont();

            ImGui::Dummy({0.f, 12.f});
            ui_separator();
            ImGui::Dummy({0.f, 10.f});


            // ── Python path ───────────────────────────────────────────────────
            ui_label("Python path");
            ImGui::Dummy({0.f, 4.f});
            ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
            static char s_python_buf[512] = {};
            static bool s_python_init = false;
            if (!s_python_init) {
                snprintf(s_python_buf, sizeof(s_python_buf), "%s", state.python_path.c_str());
                s_python_init = true;
            }
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
            ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 16.f);
            if (ImGui::InputText("##python_path", s_python_buf, sizeof(s_python_buf),
                    ImGuiInputTextFlags_EnterReturnsTrue))
                state.python_path = s_python_buf;
            if (ImGui::IsItemDeactivated())
                state.python_path = s_python_buf;
            ImGui::PopStyleColor(2);

            ImGui::Dummy({0.f, 12.f});
            ui_separator();
            ImGui::Dummy({0.f, 10.f});

            // ── Lyric extraction models ───────────────────────────────────────
            ui_label("Lyric extraction models");
            ImGui::Dummy({0.f, 4.f});
            ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
            if (state.models_ready) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(100, 220, 130, 255));
                ImGui::TextUnformatted("Installed");
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                ImGui::TextUnformatted("Not installed");
                ImGui::PopStyleColor();
                ImGui::SameLine(0.f, 12.f);
                if (ui_btn("Download…", false, true)) {
                    state.show_settings_modal = false;
                    state.show_model_dl_modal = true;
                    ImGui::CloseCurrentPopup();
                }
            }

            ImGui::Dummy({0.f, 16.f});
            ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
            if (ui_btn("Close", false, false)) {
                state.show_settings_modal = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::Dummy({0.f, 8.f});

            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);
    }

    // ── Body layout ──────────────────────────────────────────────────────────
    float menubar_h  = ImGui::GetFrameHeight() + 2.f;
    float body_top   = ImGui::GetCursorPosY();
    float pipeline_h = (state.pipeline.stage != PipelineStage::Idle &&
                        state.pipeline.stage != PipelineStage::Done &&
                        state.pipeline.stage != PipelineStage::Error) ? 28.f : 0.f;
    float avail_h    = win_h - menubar_h - body_top - pipeline_h - 2.f;

    // Timeline height — user-draggable, defaults to minimum on first open
    static const float TL_MIN_H = TL_RULER_H + 4 * TL_TRACK_H;
    float tl_h       = (state.tl_h_frac > 0.f)
                        ? fmaxf(TL_MIN_H, fminf(avail_h * 0.7f, state.tl_h_frac * avail_h))
                        : TL_MIN_H;
    float body_h     = avail_h - tl_h;

    // Right panel width — user-draggable, default auto
    float props_w   = (state.panel_w > 0.f)
                       ? fmaxf(200.f, fminf(win_w * 0.6f, state.panel_w))
                       : fmaxf(260.f, win_w * 0.27f);
    float preview_w = win_w - props_w - 1.f;

    // ── Preview ───────────────────────────────────────────────────────────────
    ImGui::SetCursorPos({0.f, body_top});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col::bg);
    ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
    if (ImGui::BeginChild("##preview_zone", {preview_w, body_h}, ImGuiChildFlags_Borders)) {
        float aw = ImGui::GetContentRegionAvail().x;
        float ah = ImGui::GetContentRegionAvail().y;

        float asp_w = 9.f, asp_h = 16.f;
        if (state.format == OutputFormat::Horizontal) { asp_w = 16.f; asp_h = 9.f; }
        else if (state.format == OutputFormat::Square) { asp_w = 1.f; asp_h = 1.f; }
        float sw, sh;
        if (aw / ah > asp_w / asp_h) { sh = ah; sw = sh * asp_w / asp_h; }
        else                          { sw = aw; sh = sw * asp_h / asp_w; }
        float ox = roundf((aw - sw) * 0.5f);
        float oy = roundf((ah - sh) * 0.5f);
        ImGui::SetCursorPos({ox, oy});
        ImVec2 stage_p = ImGui::GetCursorScreenPos();
        ImGui::Dummy({sw, sh});
        draw_preview(state, stage_p, sw, sh);

        // ── Glass transport overlay ───────────────────────────────────────────
        {
            ImDrawList* dl  = ImGui::GetWindowDrawList();
            float fps_v     = tl_fps(state);
            float f_dt_v    = fps_v > 0.f ? 1.f / fps_v : 1.f / 30.f;
            float dur       = fmaxf(state.duration, 0.01f);
            bool  busy      = audio_loading() || proxy_is_generating() || state.extract_running;

            // ── Geometry ──────────────────────────────────────────────────────
            const float PILL_PAD_X = 16.f;
            const float PILL_PAD_Y = 10.f;
            const float SCRUB_H    = 4.f;   // resting height of scrubber track
            const float SCRUB_H_H  = 6.f;   // hovered height
            const float BTN_ROW_H  = 36.f;
            const float TC_ROW_H   = 18.f;  // timecode row below buttons
            const float PILL_H     = SCRUB_H + BTN_ROW_H + TC_ROW_H + PILL_PAD_Y * 2.f + 10.f;
            const float PILL_W     = fminf(sw, fmaxf(360.f, sw * 0.85f));
            const float PILL_R     = 16.f;

            float pill_x0 = stage_p.x + (sw - PILL_W) * 0.5f;
            float pill_x1 = pill_x0 + PILL_W;
            float pill_y1 = stage_p.y + sh - 14.f;
            float pill_y0 = pill_y1 - PILL_H;

            // Gradient shadow behind pill
            dl->AddRectFilledMultiColor(
                {stage_p.x, pill_y0 - 40.f}, {stage_p.x + sw, pill_y1 + 8.f},
                IM_COL32(0,0,0,0),   IM_COL32(0,0,0,0),
                IM_COL32(0,0,0,160), IM_COL32(0,0,0,160));

            // Glass pill body
            dl->AddRectFilled({pill_x0, pill_y0}, {pill_x1, pill_y1},
                              IM_COL32(18, 18, 22, 210), PILL_R);
            // Top-edge glass highlight
            dl->AddLine({pill_x0 + PILL_R, pill_y0 + 1.f},
                        {pill_x1 - PILL_R, pill_y0 + 1.f},
                        IM_COL32(255,255,255,28), 1.f);
            // Outer border
            dl->AddRect({pill_x0, pill_y0}, {pill_x1, pill_y1},
                        IM_COL32(255,255,255,22), PILL_R, 0, 1.f);

            // ── Scrubber ──────────────────────────────────────────────────────
            float scrub_margin = PILL_PAD_X + 4.f;
            float scrub_x0 = pill_x0 + scrub_margin;
            float scrub_x1 = pill_x1 - scrub_margin;
            float scrub_w  = scrub_x1 - scrub_x0;
            float scrub_cy = pill_y0 + PILL_PAD_Y + SCRUB_H * 0.5f;

            ImGui::SetCursorScreenPos({scrub_x0, scrub_cy - 10.f});
            ImGui::InvisibleButton("##scrub", {scrub_w, 20.f});
            bool scrub_hov  = ImGui::IsItemHovered();
            bool scrub_held = ImGui::IsItemActive();

            static float s_scrub_h_anim = SCRUB_H;
            float scrub_h_target = (scrub_hov || scrub_held) ? SCRUB_H_H : SCRUB_H;
            s_scrub_h_anim += (scrub_h_target - s_scrub_h_anim) * ImGui::GetIO().DeltaTime * 18.f;

            float mouse_t  = 0.f;
            bool  has_scrub_hov = false;
            if (scrub_hov || scrub_held) {
                float frac = (ImGui::GetIO().MousePos.x - scrub_x0) / scrub_w;
                frac = fmaxf(0.f, fminf(1.f, frac));
                mouse_t = frac * dur;
                mouse_t = roundf(mouse_t * 30.f) / 30.f;
                has_scrub_hov = true;
                if (scrub_held) seek_to(state, mouse_t);
            }

            float played_frac = fmaxf(0.f, fminf(1.f, state.playhead / dur));
            float play_sx = scrub_x0 + played_frac * scrub_w;
            float bh2 = s_scrub_h_anim * 0.5f;

            // Track
            dl->AddRectFilled({scrub_x0, scrub_cy - bh2}, {scrub_x1, scrub_cy + bh2},
                              IM_COL32(255,255,255,30), bh2);
            // Played
            dl->AddRectFilled({scrub_x0, scrub_cy - bh2}, {play_sx, scrub_cy + bh2},
                              IM_COL32(220,220,255,200), bh2);

            // Hover ghost
            if (has_scrub_hov) {
                float hsx = scrub_x0 + (mouse_t / dur) * scrub_w;
                dl->AddRectFilled({scrub_x0, scrub_cy - bh2}, {hsx, scrub_cy + bh2},
                                  IM_COL32(255,255,255,20), bh2);
                // Timecode bubble
                char htc[16]; snprintf(htc, sizeof(htc), "%s", fmt_time(mouse_t).c_str());
                float htc_w = ImGui::CalcTextSize(htc).x + 10.f;
                float htc_x = fmaxf(scrub_x0, fminf(hsx - htc_w*0.5f, scrub_x1 - htc_w));
                float htc_y = scrub_cy - bh2 - 22.f;
                dl->AddRectFilled({htc_x-2.f, htc_y-2.f}, {htc_x+htc_w+2.f, htc_y+16.f},
                                  IM_COL32(30,30,35,220), 4.f);
                dl->AddText({htc_x+5.f, htc_y+1.f}, IM_COL32(220,220,220,220), htc);
                // Hover dot
                dl->AddCircleFilled({hsx, scrub_cy}, s_scrub_h_anim + 1.f, IM_COL32(0,0,0,80));
                dl->AddCircleFilled({hsx, scrub_cy}, s_scrub_h_anim,       IM_COL32(255,255,255,160));
            }

            // Playhead knob
            float knob_r = (scrub_hov || scrub_held) ? s_scrub_h_anim + 2.f : s_scrub_h_anim;
            dl->AddCircleFilled({play_sx, scrub_cy}, knob_r + 1.5f, IM_COL32(0,0,0,120));
            dl->AddCircleFilled({play_sx, scrub_cy}, knob_r,        IM_COL32(255,255,255,255));

            // ── Thumbnail above pill on scrub hover ───────────────────────────
            if (has_scrub_hov) {
                float hsx = scrub_x0 + (mouse_t / dur) * scrub_w;
                int th_w = 0, th_h = 0;
                uintptr_t th_tex = video_get_thumbnail((double)mouse_t, &th_w, &th_h);
                if (th_tex && th_w > 0 && th_h > 0) {
                    float td_w = 120.f;
                    float td_h = td_w * (float)th_h / (float)th_w;
                    float tx = fmaxf(scrub_x0, fminf(hsx - td_w*0.5f, scrub_x1 - td_w));
                    float ty = pill_y0 - td_h - 8.f;
                    dl->AddRectFilled({tx-3.f,ty-3.f},{tx+td_w+3.f,ty+td_h+3.f},
                                      IM_COL32(20,20,20,220), 4.f);
                    dl->AddImage((ImTextureID)(uintptr_t)th_tex, {tx,ty}, {tx+td_w,ty+td_h});
                }
            }

            // ── Transport button row ──────────────────────────────────────────
            const float SB  = 26.f;
            const float PB  = 38.f;
            const float GAP = 8.f;
            float btns_total = SB * 4.f + PB + GAP * 4.f;
            float btn_row_y  = pill_y0 + PILL_PAD_Y + SCRUB_H + 10.f;
            float bx = pill_x0 + (PILL_W - btns_total) * 0.5f;
            float btn_cy = btn_row_y + BTN_ROW_H * 0.5f;

            // Glass circle button helper
            auto glass_btn = [&](const char* id, float sz) -> std::pair<bool, ImU32> {
                float cy2 = btn_row_y + (BTN_ROW_H - sz) * 0.5f;
                ImGui::SetCursorScreenPos({bx, cy2});
                ImGui::InvisibleButton(id, {sz, sz});
                bool h = ImGui::IsItemHovered();
                bool a = ImGui::IsItemActive();
                bool c = ImGui::IsItemClicked();
                float cx2 = bx + sz * 0.5f, cy3 = cy2 + sz * 0.5f;
                // Glass circle bg
                dl->AddCircleFilled({cx2, cy3}, sz * 0.5f,
                    IM_COL32(255,255,255, a ? 45 : h ? 28 : 12));
                dl->AddCircle({cx2, cy3}, sz * 0.5f - 0.5f,
                    IM_COL32(255,255,255, a ? 80 : h ? 50 : 22), 0, 1.f);
                ImU32 ic = IM_COL32(255,255,255, a ? 255 : h ? 230 : 180);
                bx += sz + GAP;
                return {c, ic};
            };

            // |< to start
            {
                auto [c, ic] = glass_btn("##t_start", SB);
                float cx2 = ImGui::GetItemRectMin().x + SB*0.5f;
                float r = SB * 0.22f;
                dl->AddRectFilled({cx2 - r*1.6f, btn_cy - r}, {cx2 - r*1.1f, btn_cy + r}, ic, 1.f);
                dl->AddTriangleFilled({cx2-r*0.9f, btn_cy-r}, {cx2-r*0.9f, btn_cy+r}, {cx2+r*0.9f, btn_cy}, ic);
                if (c) seek_to(state, 0.f);
            }

            // < frame back
            {
                auto [c, ic] = glass_btn("##t_prev", SB);
                float cx2 = ImGui::GetItemRectMin().x + SB*0.5f;
                float r = SB * 0.22f;
                dl->AddTriangleFilled({cx2+r, btn_cy-r}, {cx2+r, btn_cy+r}, {cx2-r, btn_cy}, ic);
                if (c) seek_to(state, fmaxf(0.f, state.playhead - f_dt_v));
            }

            // Play / Pause (larger glass circle)
            {
                float sz = PB;
                float cy2 = btn_row_y + (BTN_ROW_H - sz) * 0.5f;
                ImGui::SetCursorScreenPos({bx, cy2});
                ImGui::InvisibleButton("##t_play", {sz, sz});
                bool h = ImGui::IsItemHovered();
                bool a = ImGui::IsItemActive();
                bool c = ImGui::IsItemClicked();
                float cx2 = bx + sz*0.5f, cy3 = cy2 + sz*0.5f;
                // Glow ring
                dl->AddCircleFilled({cx2, cy3}, sz*0.5f + 2.f, IM_COL32(180,180,255, h||a ? 18 : 8));
                // Glass body
                dl->AddCircleFilled({cx2, cy3}, sz*0.5f,
                    IM_COL32(255,255,255, a ? 60 : h ? 42 : 25));
                dl->AddCircle({cx2, cy3}, sz*0.5f - 0.5f,
                    IM_COL32(255,255,255, a ? 100 : h ? 70 : 40), 0, 1.2f);
                // Top highlight arc — fake refraction
                dl->AddCircle({cx2, cy3 - 1.f}, sz*0.5f - 2.f,
                    IM_COL32(255,255,255, 18), 0, 1.f);

                ImU32 ic = IM_COL32(255,255,255, a ? 255 : h ? 235 : 200);
                float r = sz * 0.18f;
                if (busy) {
                    float t_spin = fmodf((float)ImGui::GetTime(), 1.2f) / 1.2f;
                    for (int i = 0; i < 3; ++i) {
                        float ang = (t_spin + i / 3.f) * 6.2832f;
                        dl->AddCircleFilled({cx2 + cosf(ang)*r, cy3 + sinf(ang)*r},
                                            2.2f, IM_COL32(255,255,255,200));
                    }
                } else if (state.playing) {
                    float bw = r*0.5f, bh3 = r*1.5f;
                    dl->AddRectFilled({cx2-bw*1.5f, cy3-bh3}, {cx2-bw*0.4f, cy3+bh3}, ic, 1.f);
                    dl->AddRectFilled({cx2+bw*0.4f, cy3-bh3}, {cx2+bw*1.5f, cy3+bh3}, ic, 1.f);
                } else {
                    dl->AddTriangleFilled({cx2-r*0.65f, cy3-r*1.05f},
                                          {cx2-r*0.65f, cy3+r*1.05f},
                                          {cx2+r*1.1f,  cy3}, ic);
                }
                if (c && !busy) toggle_play(state);
                bx += sz + GAP;
            }

            // > frame forward
            {
                auto [c, ic] = glass_btn("##t_next", SB);
                float cx2 = ImGui::GetItemRectMin().x + SB*0.5f;
                float r = SB * 0.22f;
                dl->AddTriangleFilled({cx2-r, btn_cy-r}, {cx2-r, btn_cy+r}, {cx2+r, btn_cy}, ic);
                if (c) seek_to(state, fminf(dur, state.playhead + f_dt_v));
            }

            // >| to end
            {
                auto [c, ic] = glass_btn("##t_end", SB);
                float cx2 = ImGui::GetItemRectMin().x + SB*0.5f;
                float r = SB * 0.22f;
                dl->AddTriangleFilled({cx2-r*0.9f, btn_cy-r}, {cx2-r*0.9f, btn_cy+r}, {cx2+r*0.9f, btn_cy}, ic);
                dl->AddRectFilled({cx2+r*1.1f, btn_cy-r}, {cx2+r*1.6f, btn_cy+r}, ic, 1.f);
                if (c) seek_to(state, dur);
            }

            // ── Timecode — centered row below buttons ─────────────────────────
            char tcbuf[32];
            snprintf(tcbuf, sizeof(tcbuf), "%s / %s",
                fmt_time(state.playhead).c_str(), fmt_time(dur).c_str());
            float tc_w = ImGui::CalcTextSize(tcbuf).x;
            float tc_y = btn_row_y + BTN_ROW_H + 2.f;
            float tc_x = pill_x0 + (PILL_W - tc_w) * 0.5f;
            dl->AddText({tc_x, tc_y}, IM_COL32(160,160,160,160), tcbuf);

            // Status text left of timecode when busy
            if (busy) {
                const char* st = audio_loading()       ? "loading…"
                               : proxy_is_generating() ? "building preview…"
                                                       : "extracting…";
                float st_w = ImGui::CalcTextSize(st).x;
                float st_x = pill_x0 + (PILL_W - st_w) * 0.5f;
                dl->AddText({st_x, tc_y}, IM_COL32(140,140,140,160), st);
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);

    // ── Properties panel ─────────────────────────────────────────────────────
    ImGui::SameLine(0.f, 1.f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col::bg_soft);
    ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
    if (ImGui::BeginChild("##props_zone", {props_w, body_h}, ImGuiChildFlags_Borders)) {
        // Tab bar
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12.f, 6.f});
        ImGui::PushStyleColor(ImGuiCol_Tab,       Col::bg_soft);
        ImGui::PushStyleColor(ImGuiCol_TabActive, Col::line);
        // Determine if a Lyrics clip is selected
        // Lyrics tab visible if any clip in the selection is a Lyrics clip
        bool lyrics_selected = false;
        for (auto& [st, sc] : state.clip_selection) {
            if (st < (int)state.tracks.size() && sc < (int)state.tracks[st].clips.size() &&
                state.tracks[st].clips[sc].clip_type == ClipType::Lyrics)
                { lyrics_selected = true; break; }
        }
        // Also check focus clip in case selection is empty
        if (!lyrics_selected &&
            state.selected_track >= 0 && state.selected_track < (int)state.tracks.size() &&
            state.selected_clip  >= 0 && state.selected_clip  < (int)state.tracks[state.selected_track].clips.size())
            lyrics_selected = state.tracks[state.selected_track].clips[state.selected_clip].clip_type == ClipType::Lyrics;

        if (!lyrics_selected && state.panel_tab == 4) state.panel_tab = 0;

        if (ImGui::BeginTabBar("##panel_tabs")) {
            if (ImGui::BeginTabItem("Clip"))      { state.panel_tab=0; ImGui::EndTabItem(); }
            if (lyrics_selected && ImGui::BeginTabItem("Lyrics")) { state.panel_tab=4; ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Animation")) { state.panel_tab=1; ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("FX"))        { state.panel_tab=5; ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Project"))   { state.panel_tab=6; ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("History"))   { state.panel_tab=3; ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();

        ImGui::BeginChild("##panel_scroll", {0.f, 0.f});
        ImGui::SetCursorPosX(8.f);
        float pw = props_w - 16.f;

        // Route to effect panel when an Effect clip is focused
        bool focused_is_effect = (state.selected_track >= 0 &&
            state.selected_track < (int)state.tracks.size() &&
            state.selected_clip  >= 0 &&
            state.selected_clip  < (int)state.tracks[state.selected_track].clips.size() &&
            state.tracks[state.selected_track].clips[state.selected_clip].clip_type == ClipType::Effect);

        if      (focused_is_effect)        panel_effect(state, pw);
        else if (state.panel_tab == 0)     panel_clip(state, pw);
        else if (state.panel_tab == 1)     panel_animation(state, pw);
        else if (state.panel_tab == 2)     panel_export(state, pw);
        else if (state.panel_tab == 4)     panel_lyrics(state, pw);
        else if (state.panel_tab == 5)     panel_fx_library(state, pw);
        else if (state.panel_tab == 6)     panel_project(state, pw);
        else                               panel_history(state, pw);
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);

    // ── Drag splitters ────────────────────────────────────────────────────────
    {
        static bool s_drag_vsplit = false, s_drag_hsplit = false;
        ImVec2 wpos = ImGui::GetWindowPos();
        ImVec2 mpos = ImGui::GetIO().MousePos;

        // Vertical splitter between preview and props
        float vborder_x = wpos.x + preview_w + 1.f;
        bool near_v = fabsf(mpos.x - vborder_x) < 6.f &&
                      mpos.y > wpos.y + body_top &&
                      mpos.y < wpos.y + body_top + body_h;
        if (near_v || s_drag_vsplit) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsMouseClicked(0)) s_drag_vsplit = true;
        }
        if (s_drag_vsplit) {
            state.panel_w = fmaxf(200.f, fminf(win_w * 0.6f, wpos.x + win_w - mpos.x));
        }
        if (ImGui::IsMouseReleased(0)) s_drag_vsplit = false;

        // Horizontal splitter between body and timeline
        float hborder_y = wpos.y + body_top + body_h + pipeline_h;
        bool near_h = fabsf(mpos.y - hborder_y) < 6.f &&
                      mpos.x > wpos.x &&
                      mpos.x < wpos.x + win_w;
        if (near_h || s_drag_hsplit) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (ImGui::IsMouseClicked(0)) s_drag_hsplit = true;
        }
        if (s_drag_hsplit) {
            float new_tl_h = wpos.y + body_top + avail_h - mpos.y;
            state.tl_h_frac = fmaxf(0.1f, fminf(0.7f, new_tl_h / avail_h));
        }
        if (ImGui::IsMouseReleased(0)) s_drag_hsplit = false;
    }

    // ── Pipeline strip ────────────────────────────────────────────────────────
    if (pipeline_h > 0.f) {
        ImGui::SetCursorPos({0.f, ImGui::GetCursorPosY()});
        draw_pipeline_strip(state, win_w);
    }

    // ── Timeline panel ────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col::bg);
    ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
    if (ImGui::BeginChild("##tl_zone", {win_w, tl_h}, ImGuiChildFlags_Borders,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        // Header bar
        ImDrawList* tl_dl = ImGui::GetWindowDrawList();
        ImVec2 hdr_tl = ImGui::GetCursorScreenPos();
        float  hdr_w  = ImGui::GetContentRegionAvail().x;
        constexpr float HDR_H = 22.f;
        tl_dl->AddRectFilled(hdr_tl, {hdr_tl.x+hdr_w, hdr_tl.y+HDR_H}, to_u32(Col::bg_soft));
        tl_dl->AddLine({hdr_tl.x, hdr_tl.y+HDR_H}, {hdr_tl.x+hdr_w, hdr_tl.y+HDR_H}, to_u32(Col::line));
        ImGui::PushFont(g_font_bold);
        tl_dl->AddText({hdr_tl.x+8.f, hdr_tl.y+4.f}, to_u32(Col::muted), "TIMELINE");
        ImGui::PopFont();

        // Zoom controls in header
        {
            char zbuf[20]; snprintf(zbuf, sizeof(zbuf), "%.0f%%", state.tl_zoom / 80.f * 100.f);
            float zx = hdr_tl.x + hdr_w - 120.f;
            ImGui::SetCursorScreenPos({zx, hdr_tl.y+2.f});
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4.f, 2.f});
            ImGui::PushStyleColor(ImGuiCol_Button,        Col::bg_soft);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col::bg_soft_hov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Col::bg_soft_hov);
            if (ImGui::SmallButton("-##zout")) state.tl_zoom = fmaxf(state.tl_zoom*0.8f, 20.f);
            ImGui::SameLine(0.f,4.f);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::SetNextItemWidth(48.f);
            ImGui::TextUnformatted(zbuf);
            ImGui::PopStyleColor();
            ImGui::SameLine(0.f,4.f);
            if (ImGui::SmallButton("+##zin"))  state.tl_zoom = fminf(state.tl_zoom*1.25f, 4000.f);
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();
        }

        // Advance cursor past header, then draw timeline
        ImGui::SetCursorScreenPos({hdr_tl.x, hdr_tl.y + HDR_H});
        ImVec2 tl_origin = ImGui::GetCursorScreenPos();
        float  tl_w      = ImGui::GetContentRegionAvail().x;
        float  tl_h_real = ImGui::GetContentRegionAvail().y;
        ImGui::Dummy({tl_w, tl_h_real});
        draw_timeline(state, tl_origin, tl_w, tl_h_real);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}
