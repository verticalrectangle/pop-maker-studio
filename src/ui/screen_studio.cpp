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
static void seek_to(AppState& state, float t) {
    state.playhead = t;
    audio_seek(t);
    if (state.playing) {
        state.play_start_pos  = t;
        state.play_start_wall = std::chrono::steady_clock::now();
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
           ? (float)video_info(0).fps : 30.f;
}

static int slot_for_video(AppState& state, const std::string& path); // forward decl

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
        int slot = slot_for_video(state, path);
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
        float dur = audio_probe(path, meta) ? meta.duration_secs : 4.f;
        cl.end = cl.start + dur;
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

// Proxy slot for a video file: reuse if already registered, else claim next free.
// O(MAX_VIDEO_TRACKS) scan — effectively O(1).  Returns -1 when all slots full.
static int slot_for_video(AppState& state, const std::string& path) {
    if (path.empty()) return -1;
    for (int i = 0; i < MAX_VIDEO_TRACKS; ++i)
        if (state.proxy_paths[i] == path) return i;
    for (int i = 0; i < MAX_VIDEO_TRACKS; ++i)
        if (state.proxy_paths[i].empty()) { state.proxy_paths[i] = path; return i; }
    return -1;
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

    auto stamp = [&](Clip& c){ c.clip_type = ClipType::Lyrics; c.source_id = src; };

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

        // Claim or reuse proxy slot for this file path.
        int slot = slot_for_video(state, path);
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
        ac.start=0.f; ac.end=state.duration; ac.text=path;
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
            const Clip* active = nullptr;
            for (auto& cl : track.clips)
                if (cl.clip_type == ClipType::Video &&
                    state.playhead >= cl.start && state.playhead < cl.end)
                    { active = &cl; break; }
            if (active) {
                int slot = slot_for_video(const_cast<AppState&>(state), active->text);
                uintptr_t tex = (slot >= 0 && video_is_open(slot))
                    ? video_get_texture(slot, (double)(state.playhead + lookahead)) : 0;
                if (tex) {
                    float px    = active->eval_prop("pos_x",    state.playhead);
                    float py    = active->eval_prop("pos_y",    state.playhead);
                    float sx    = active->eval_prop("scale_x",  state.playhead);
                    float sy    = active->eval_prop("scale_y",  state.playhead);
                    float rot   = active->eval_prop("rotation", state.playhead);
                    float alpha = active->eval_prop("opacity",  state.playhead);

                    float cx = p.x + px * w,  cy = p.y + py * h;
                    float hw = w * sx * 0.5f, hh = h * sy * 0.5f;
                    float rad   = rot * 3.14159265f / 180.f;
                    float cos_r = cosf(rad), sin_r = sinf(rad);
                    auto rot_pt = [&](float ox, float oy) -> ImVec2 {
                        return { cx + ox*cos_r - oy*sin_r, cy + ox*sin_r + oy*cos_r };
                    };
                    ImU32 col = IM_COL32(255, 255, 255, (int)(alpha * 255.f));
                    dl->AddImageQuad(ImTextureRef((ImTextureID)tex),
                        rot_pt(-hw,-hh), rot_pt(hw,-hh), rot_pt(hw,hh), rot_pt(-hw,hh),
                        {0,0}, {1,0}, {1,1}, {0,1}, col);
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

            // Shadow pass
            dl->AddText(ImGui::GetFont(), fsz, {tx+2.f, slot_y+2.f}, IM_COL32(0,0,0,180), show->text.c_str());

            // Karaoke: per-word coloring when words_cache is available and clip is active.
            // Falls back to solid color for selected-but-not-playing previews.
            bool did_karaoke = false;
            if (active_ci >= 0 && show->karaoke && !state.words_cache.empty()) {
                // Collect words that belong to this clip (their timestamps fall within clip range)
                std::vector<const WordEntry*> clip_words;
                for (auto& we : state.words_cache)
                    if (we.end > show->start && we.start < show->end)
                        clip_words.push_back(&we);

                if (!clip_words.empty()) {
                    did_karaoke = true;
                    float cur_x = tx;
                    for (int wi = 0; wi < (int)clip_words.size(); ++wi) {
                        const WordEntry* we = clip_words[wi];
                        bool is_active = state.playhead >= we->start && state.playhead < we->end;

                        ImU32 wcol;
                        if (show->sub_color_override) {
                            float a = is_active ? show->sub_color[3] : show->sub_color[3] * 0.45f;
                            wcol = IM_COL32((int)(show->sub_color[0]*255),
                                            (int)(show->sub_color[1]*255),
                                            (int)(show->sub_color[2]*255), (int)(a*255));
                        } else {
                            wcol = is_active ? IM_COL32(255,255,255,255) : IM_COL32(255,255,255,100);
                        }

                        std::string word_sp = we->text + (wi+1 < (int)clip_words.size() ? " " : "");
                        ImVec2 wsz = ImGui::CalcTextSize(word_sp.c_str());
                        dl->AddText(ImGui::GetFont(), fsz, {cur_x, slot_y}, wcol, word_sp.c_str());
                        cur_x += wsz.x;
                    }
                }
            }

            if (!did_karaoke) {
                ImU32 tcol;
                if (show->sub_color_override) {
                    float a = (active_ci >= 0) ? show->sub_color[3] : show->sub_color[3] * 0.5f;
                    tcol = IM_COL32((int)(show->sub_color[0]*255), (int)(show->sub_color[1]*255),
                                    (int)(show->sub_color[2]*255), (int)(a*255));
                } else {
                    tcol = (active_ci >= 0) ? to_u32(Col::fg) : to_u32(Col::muted);
                }
                dl->AddText(ImGui::GetFont(), fsz, {tx, slot_y}, tcol, show->text.c_str());
            }

            ImGui::PopFont();

            if (show_ci >= 0 && slot_y >= p.y && slot_y + tsz.y <= p.y + h) {
                float pad = 8.f;
                bool in_handle = mpos.x >= tx - pad && mpos.x <= tx + tsz.x + pad &&
                                 mpos.y >= slot_y - pad && mpos.y <= slot_y + tsz.y + pad;
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
                    float mid_x = tx + tsz.x * 0.5f;
                    for (int d = -1; d <= 1; ++d)
                        dl->AddCircleFilled({mid_x + d*6.f, slot_y - 6.f}, 2.f, to_u32(Col::muted));
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

static void panel_clip(AppState& state, float w) {
    // ── Nothing selected ──────────────────────────────────────────────────────
    if (state.selected_track < 0 || state.selected_track >= (int)state.tracks.size()) {
        ImGui::Dummy({0.f, 24.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        auto centre = [&](const char* s) {
            ImGui::SetCursorPosX((w - ImGui::CalcTextSize(s).x) * 0.5f);
            ImGui::TextUnformatted(s);
        };
        centre("Click a track label to configure it");
        ImGui::Dummy({0.f, 4.f});
        centre("Click a clip to edit it");
        ImGui::PopStyleColor();
        return;
    }

    Track& track = state.tracks[state.selected_track];

    // ── Track face — track selected, no clip ──────────────────────────────────
    if (state.selected_clip < 0 || state.selected_clip >= (int)track.clips.size()) {
        ImGui::Dummy({0.f, 24.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::SetCursorPosX((w - ImGui::CalcTextSize("Click a clip to edit it").x) * 0.5f);
        ImGui::TextUnformatted("Click a clip to edit it");
        ImGui::PopStyleColor();
        return;
    }

    Clip& clip = track.clips[state.selected_clip];

    ImGui::Dummy({0.f, 8.f});
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    char tlabel[80];
    snprintf(tlabel, sizeof(tlabel), "%s  ·  clip %d of %d",
        track.name.c_str(), state.selected_clip + 1, (int)track.clips.size());
    ImGui::TextUnformatted(tlabel);
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 4.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

    // Helper: copy position + color from this clip to all clips with same source_id + type
    auto propagate_style = [&]() {
        if (clip.source_id.empty()) return;
        int count = 0;
        for (auto& t : state.tracks)
            for (auto& c : t.clips)
                if (c.clip_type == clip.clip_type && c.source_id == clip.source_id && &c != &clip) {
                    c.sub_pos            = clip.sub_pos;
                    c.sub_pos_y          = clip.sub_pos_y;
                    c.sub_color_override = clip.sub_color_override;
                    memcpy(c.sub_color, clip.sub_color, sizeof(clip.sub_color));
                    ++count;
                }
        if (count > 0) history_push(state, "Apply style to all");
    };

    if ((clip.clip_type == ClipType::Lyrics || clip.clip_type == ClipType::Subtitle) &&
        !clip.source_id.empty()) {
        const char* btn_lbl = clip.clip_type == ClipType::Lyrics
            ? "Apply style to all Lyrics clips"
            : "Apply style to all Subtitle clips";
        if (ui_btn(btn_lbl, false, true)) propagate_style();
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Push position + color to every clip from this source");
            ImGui::EndTooltip();
        }

        // Apply to multi-selection
        int sel_count = 0;
        for (auto& [st, sc] : state.clip_selection) {
            if (st == state.selected_track && sc == state.selected_clip) continue;
            if (st < (int)state.tracks.size() && sc < (int)state.tracks[st].clips.size() &&
                state.tracks[st].clips[sc].clip_type == clip.clip_type)
                ++sel_count;
        }
        if (sel_count > 0) {
            ImGui::SameLine(0.f, 6.f);
            char slbl[48];
            snprintf(slbl, sizeof(slbl), "Apply to %d selected##clip", sel_count);
            if (ui_btn(slbl, false, true)) {
                for (auto& [st, sc] : state.clip_selection) {
                    if (st == state.selected_track && sc == state.selected_clip) continue;
                    if (st >= (int)state.tracks.size() || sc >= (int)state.tracks[st].clips.size()) continue;
                    Clip& tgt = state.tracks[st].clips[sc];
                    if (tgt.clip_type != clip.clip_type) continue;
                    tgt.karaoke            = clip.karaoke;
                    tgt.sub_pos            = clip.sub_pos;
                    tgt.sub_pos_y          = clip.sub_pos_y;
                    tgt.sub_color_override = clip.sub_color_override;
                    memcpy(tgt.sub_color, clip.sub_color, sizeof(clip.sub_color));
                }
                history_push(state, "Apply style to selected clips");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Push position, color and karaoke flag to selected clips");
                ImGui::EndTooltip();
            }
        }

        ImGui::Dummy({0.f, 8.f});
    }

    if (clip.clip_type == ClipType::Text || clip.clip_type == ClipType::Lyrics) {
        // ── Text ─────────────────────────────────────────────────────────────
        ui_label("Text"); ImGui::Dummy({0.f, 4.f});
        if (s_edit_focus_next) { ImGui::SetKeyboardFocusHere(); s_edit_focus_next = false; }
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
        ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
        ImGui::SetNextItemWidth(w - 16.f);
        if (ImGui::InputText("##clip_text", s_edit_buf, sizeof(s_edit_buf),
                ImGuiInputTextFlags_EnterReturnsTrue))
            clip.text = s_edit_buf;
        if (ImGui::IsItemDeactivated()) {
            if (clip.text != s_edit_buf) history_push(state, "Edit clip text");
            clip.text = s_edit_buf;
        }
        ImGui::PopStyleColor(2);

        ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

        // ── Timing ───────────────────────────────────────────────────────────
        ui_label("Timing"); ImGui::Dummy({0.f, 4.f});
        float half = (w - 24.f) * 0.5f;
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
        ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted("Start"); ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(half);
        float st_start = clip.start;
        if (ImGui::InputFloat("##start", &st_start, 0.05f, 0.1f, "%.3f"))
            if (st_start < clip.end - 0.05f) clip.start = st_start;
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Adjust timing");
        ImGui::EndGroup();
        ImGui::SameLine(0.f, 8.f);
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted("End"); ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(half);
        float st_end = clip.end;
        if (ImGui::InputFloat("##end", &st_end, 0.05f, 0.1f, "%.3f"))
            if (st_end > clip.start + 0.05f) clip.end = st_end;
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Adjust timing");
        ImGui::EndGroup();
        ImGui::PopStyleColor(2);
        ImGui::Dummy({0.f, 4.f});
        char dur_buf[32];
        snprintf(dur_buf, sizeof(dur_buf), "Duration  %.3fs", clip.end - clip.start);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted(dur_buf); ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 8.f}); ui_label("Nudge"); ImGui::Dummy({0.f, 4.f});
        bool nudged = false;
        if (ui_btn("-100ms", false, true)) { clip.start-=0.1f; clip.end-=0.1f; if(clip.start<0){clip.end-=clip.start;clip.start=0;} nudged=true; }
        ImGui::SameLine(0.f, 4.f);
        if (ui_btn("-10ms",  false, true)) { clip.start-=0.01f; clip.end-=0.01f; nudged=true; }
        ImGui::SameLine(0.f, 4.f);
        if (ui_btn("+10ms",  false, true)) { clip.start+=0.01f; clip.end+=0.01f; nudged=true; }
        ImGui::SameLine(0.f, 4.f);
        if (ui_btn("+100ms", false, true)) { clip.start+=0.1f;  clip.end+=0.1f;  nudged=true; }
        if (nudged) history_push(state, "Nudge clip");

        ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

        ImGui::Dummy({0.f, 8.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

        // ── Position override ─────────────────────────────────────────────────
        ui_label("Position"); ImGui::Dummy({0.f, 4.f});
        struct PosBtn { int v; const char* label; };
        PosBtn pbtns[] = {{0,"Bottom"},{1,"Center"},{2,"Top"},{3,"Custom Y"}};
        for (auto& pb : pbtns) {
            if (ui_btn(pb.label, clip.sub_pos == pb.v, true)) {
                clip.sub_pos = pb.v;
                history_push(state, "Subtitle position");
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
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Subtitle position Y");
            ImGui::PopStyleColor(2);
        }

        ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

        // ── Color override ────────────────────────────────────────────────────
        ui_label("Color"); ImGui::Dummy({0.f, 4.f});
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
        bool col_ov = clip.sub_color_override;
        if (ImGui::Checkbox("Override color##col_ov", &col_ov)) {
            clip.sub_color_override = col_ov;
            history_push(state, "Subtitle color override");
        }
        if (clip.sub_color_override) {
            ImGui::Dummy({0.f, 4.f});
            ImGui::SetNextItemWidth(w - 16.f);
            ImGui::ColorEdit4("##sub_col", clip.sub_color,
                ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar);
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Subtitle color");
        }
        ImGui::PopStyleColor();

        // ── Lyrics grouping controls (Lyrics clips only) ──────────────────
        if (clip.clip_type == ClipType::Lyrics) {
            ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
            ui_label("Lyrics grouping"); ImGui::Dummy({0.f, 4.f});

            if (!state.words_json_path.empty() && fs::exists(state.words_json_path)) {
                struct ModeBtn { SubtitleMode m; const char* label; const char* tip; };
                static const ModeBtn modes[] = {
                    {SubtitleMode::Word,    "Word",    "One clip per word"},
                    {SubtitleMode::Phrase,  "Phrase",  "Group by short pauses (>0.3s)"},
                    {SubtitleMode::Line,    "Line",    "Group by breath gaps (>0.8s)"},
                    {SubtitleMode::Karaoke, "Karaoke", "Line groups with per-word highlight"},
                    {SubtitleMode::Segment, "Segment", "WhisperX sentence boundaries"},
                    {SubtitleMode::CustomN, "Custom",  "N words per clip"},
                };
                for (auto& mb : modes) {
                    bool sel = state.subtitle_mode == mb.m;
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
                    if (ImGui::InputInt("words/clip##cn", &n))
                        state.subtitle_n = (n < 1) ? 1 : (n > 20) ? 20 : n;
                    ImGui::PopStyleColor(2);
                }

                ImGui::Dummy({0.f, 8.f});
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
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                ImGui::TextWrapped("Run ML Processing on an audio clip to generate word JSON, then grouping controls appear here.");
                ImGui::PopStyleColor();
            }
        }

    } else if (clip.clip_type == ClipType::Subtitle) {
        // ── SRT editor — shows all clips sharing this source_id across all tracks ──

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
            return state.tracks[a.ti].clips[a.ci].start <
                   state.tracks[b.ti].clips[b.ci].start;
        });

        int n_total = (int)entries.size();
        char info[64];
        snprintf(info, sizeof(info), "%d clips", n_total);
        if (!clip.source_id.empty()) {
            std::string fn = fs::path(clip.source_id).filename().string();
            snprintf(info, sizeof(info), "%d clips  ·  %s", n_total, fn.c_str());
        }
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted(info);
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 6.f});

        // Find which row index is currently selected
        int sel_row = -1;
        for (int i = 0; i < n_total; ++i)
            if (entries[i].ti == state.selected_track && entries[i].ci == state.selected_clip)
                { sel_row = i; break; }

        static int s_srt_last_row = -1;
        float row_h  = 22.f;
        float list_h = fminf(180.f, n_total * row_h + 6.f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Col::bg_soft);
        ImGui::BeginChild("##srt_list", {0.f, list_h}, false);
        for (int i = 0; i < n_total; ++i) {
            const SRTEntry& e  = entries[i];
            Clip& sc           = state.tracks[e.ti].clips[e.ci];
            bool  is_sel       = (i == sel_row);

            if (is_sel && s_srt_last_row != i) ImGui::SetScrollHereY(0.5f);

            ImGui::PushStyleColor(ImGuiCol_Text, is_sel ? Col::fg : Col::muted);
            char row_id[32]; snprintf(row_id, sizeof(row_id), "##srt%d", i);
            if (ImGui::Selectable(row_id, is_sel, 0, {0.f, row_h})) {
                state.selected_track = e.ti;
                state.selected_clip  = e.ci;
                strncpy(s_edit_buf, sc.text.c_str(), sizeof(s_edit_buf)-1);
                s_edit_buf[sizeof(s_edit_buf)-1] = '\0';
                seek_to(state, sc.start);
            }
            ImGui::SameLine(0.f, 4.f);
            std::string preview = sc.text.size() > 34 ? sc.text.substr(0, 32) + "…" : sc.text;
            char rowlbl[96];
            snprintf(rowlbl, sizeof(rowlbl), "%2d  %s  %s", i+1,
                fmt_time(sc.start).c_str(), preview.c_str());
            ImGui::TextUnformatted(rowlbl);
            ImGui::PopStyleColor();
        }
        s_srt_last_row = sel_row;
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 8.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

        // Edit controls for the currently selected clip
        if (sel_row >= 0) {
            Clip& sc = state.tracks[entries[sel_row].ti].clips[entries[sel_row].ci];

            ui_label("Text"); ImGui::Dummy({0.f, 4.f});
            if (s_edit_focus_next) { ImGui::SetKeyboardFocusHere(); s_edit_focus_next = false; }
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
            ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
            ImGui::SetNextItemWidth(w - 16.f);
            if (ImGui::InputText("##srt_text", s_edit_buf, sizeof(s_edit_buf),
                    ImGuiInputTextFlags_EnterReturnsTrue))
                sc.text = s_edit_buf;
            if (ImGui::IsItemDeactivated()) {
                if (sc.text != s_edit_buf) history_push(state, "Edit subtitle text");
                sc.text = s_edit_buf;
            }
            ImGui::PopStyleColor(2);

            ImGui::Dummy({0.f, 8.f});
            ui_label("Timing"); ImGui::Dummy({0.f, 4.f});
            float half = (w - 24.f) * 0.5f;
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
            ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
            ImGui::BeginGroup();
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted("Start"); ImGui::PopStyleColor();
            ImGui::SetNextItemWidth(half);
            float ss = sc.start;
            if (ImGui::InputFloat("##srt_s", &ss, 0.01f, 0.1f, "%.3f"))
                if (ss < sc.end - 0.01f) { sc.start = fmaxf(0.f, ss); history_push(state, "Subtitle timing"); }
            ImGui::EndGroup();
            ImGui::SameLine(0.f, 8.f);
            ImGui::BeginGroup();
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted("End"); ImGui::PopStyleColor();
            ImGui::SetNextItemWidth(half);
            float se = sc.end;
            if (ImGui::InputFloat("##srt_e", &se, 0.01f, 0.1f, "%.3f"))
                if (se > sc.start + 0.01f) { sc.end = se; history_push(state, "Subtitle timing"); }
            ImGui::EndGroup();
            ImGui::PopStyleColor(2);

            ImGui::Dummy({0.f, 6.f});
            bool nudged = false;
            if (ui_btn("-100ms", false, true)) { sc.start-=0.1f; sc.end-=0.1f; if(sc.start<0.f){sc.end-=sc.start;sc.start=0.f;} nudged=true; }
            ImGui::SameLine(0.f, 4.f);
            if (ui_btn("-10ms",  false, true)) { sc.start-=0.01f; sc.end-=0.01f; nudged=true; }
            ImGui::SameLine(0.f, 4.f);
            if (ui_btn("+10ms",  false, true)) { sc.start+=0.01f; sc.end+=0.01f; nudged=true; }
            ImGui::SameLine(0.f, 4.f);
            if (ui_btn("+100ms", false, true)) { sc.start+=0.1f;  sc.end+=0.1f;  nudged=true; }
            if (nudged) history_push(state, "Nudge subtitle");

            ImGui::Dummy({0.f, 10.f});
            if (ui_btn("+ Add below", false, true)) {
                Clip nc; nc.clip_type = ClipType::Subtitle;
                nc.source_id = sc.source_id;
                nc.start = sc.end; nc.end = sc.end + 2.f;
                Track& target_track = state.tracks[entries[sel_row].ti];
                int ins = entries[sel_row].ci + 1;
                target_track.clips.insert(target_track.clips.begin() + ins, nc);
                state.selected_clip = ins;
                strncpy(s_edit_buf, "", 1);
                s_edit_focus_next = true;
                history_push(state, "Add subtitle");
            }
            ImGui::SameLine(0.f, 4.f);
            if (ui_btn("Delete", false, true)) {
                Track& del_track = state.tracks[entries[sel_row].ti];
                del_track.clips.erase(del_track.clips.begin() + entries[sel_row].ci);
                // Reselect: next row in the same source
                int new_row = std::min(sel_row, n_total - 2);
                if (new_row >= 0 && new_row < n_total - 1) {
                    state.selected_track = entries[new_row].ti;
                    state.selected_clip  = entries[new_row].ci;
                    // Adjust ci if we deleted from same track before it
                    if (entries[sel_row].ti == entries[new_row].ti &&
                        entries[sel_row].ci  <  entries[new_row].ci)
                        state.selected_clip--;
                    if (state.selected_clip >= 0 &&
                        state.selected_clip < (int)state.tracks[state.selected_track].clips.size()) {
                        strncpy(s_edit_buf,
                            state.tracks[state.selected_track].clips[state.selected_clip].text.c_str(),
                            sizeof(s_edit_buf)-1);
                        s_edit_buf[sizeof(s_edit_buf)-1] = '\0';
                    }
                } else {
                    state.selected_clip = -1;
                }
                history_push(state, "Delete subtitle");
            }
        }

    } else {
        // ── Audio / Video clip ────────────────────────────────────────────────

        // File info
        ui_label("File"); ImGui::Dummy({0.f, 4.f});
        std::string fname = clip.text.empty() ? "(no file)" : fs::path(clip.text).filename().string();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted(fname.c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && !clip.text.empty()) {
            ImGui::BeginTooltip(); ImGui::TextUnformatted(clip.text.c_str()); ImGui::EndTooltip();
        }

        ImGui::Dummy({0.f, 4.f});
        char timebuf[64];
        snprintf(timebuf, sizeof(timebuf), "%s — %s  (%.2fs)",
            fmt_time(clip.start).c_str(), fmt_time(clip.end).c_str(), clip.end - clip.start);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted(timebuf);
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

        // Volume
        ui_label("Volume"); ImGui::Dummy({0.f, 4.f});
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, Col::fg);
        ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
        ImGui::SetNextItemWidth(w - 16.f);
        ImGui::SliderFloat("##vol", &clip.volume, 0.f, 2.f, "%.2f×");
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Volume");
        ImGui::PopStyleColor(2);

        ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

        // Speed
        ui_label("Speed"); ImGui::Dummy({0.f, 4.f});
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, Col::fg);
        ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
        ImGui::SetNextItemWidth(w - 16.f);
        ImGui::SliderFloat("##spd", &clip.speed, 0.25f, 4.f, "%.2f\xc3\x97");
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Speed");
        ImGui::PopStyleColor(2);
        ImGui::Dummy({0.f, 4.f});
        { struct SP { float f; const char* l; };
          SP presets[] = {{0.25f,"¼×"},{0.5f,"½×"},{1.f,"1×"},{2.f,"2×"},{4.f,"4×"}};
          for (auto& p : presets) {
              if (ui_btn(p.l, fabsf(clip.speed - p.f) < 0.01f, true)) {
                  clip.speed = p.f;
                  history_push(state, "Speed");
              }
              ImGui::SameLine(0.f, 4.f);
          }
          ImGui::NewLine();
        }

        // Opacity + Transition — video only
        if (clip.clip_type == ClipType::Video) {
            ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
            ui_label("Opacity"); ImGui::Dummy({0.f, 4.f});
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, Col::fg);
            ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
            ImGui::SetNextItemWidth(w - 16.f);
            ImGui::SliderFloat("##opa", &clip.opacity, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Opacity");
            ImGui::PopStyleColor(2);

            ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
            ui_label("Transition out"); ImGui::Dummy({0.f, 4.f});
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, Col::fg);
            ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
            ImGui::SetNextItemWidth(w - 16.f);
            ImGui::SliderFloat("##trans", &clip.transition_out, 0.f, 2.f, "%.2fs");
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Transition");
            ImGui::PopStyleColor(2);
            ImGui::Dummy({0.f, 4.f});
            struct TP { float f; const char* l; };
            TP tpresets[] = {{0.f,"None"},{0.5f,"0.5s"},{1.f,"1s"},{2.f,"2s"}};
            for (auto& tp : tpresets) {
                if (ui_btn(tp.l, fabsf(clip.transition_out - tp.f) < 0.01f, true)) {
                    clip.transition_out = tp.f;
                    history_push(state, "Transition");
                }
                ImGui::SameLine(0.f, 4.f);
            }
            ImGui::NewLine();
            ImGui::Dummy({0.f, 2.f});
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted("Crossfade — applied on render");
            ImGui::PopStyleColor();

            // ── Transform (keyframeable) ──────────────────────────────────────
            ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
            ui_label("Transform"); ImGui::Dummy({0.f, 4.f});

            float ph = state.playhead;
            float t_local = ph - clip.start;
            int   sel_ti = state.selected_track, sel_ci = state.selected_clip;

            // Helper: slider + ◆ keyframe button for one property.
            // Returns true if the value changed.
            auto kf_slider = [&](const char* prop, const char* label,
                                 float* val_ptr, float vmin, float vmax,
                                 const char* fmt) -> bool
            {
                bool changed = false;
                PropTrack& pt = clip.ktracks[prop];
                bool has_kf   = (pt.find_nearest(t_local, 0.05f) >= 0);

                // ◆ button — active colour when keyframe exists at current time
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
                        float cur = clip.eval_prop(prop, ph);
                        pt.set(t_local, cur);
                        state.kf_sel_track = sel_ti;
                        state.kf_sel_clip  = sel_ci;
                        state.kf_sel_prop  = prop;
                        state.kf_sel_idx   = pt.find_nearest(t_local, 0.1f);
                        history_push(state, std::string("Add KF ") + prop);
                    }
                }
                ImGui::PopStyleColor(2);
                ImGui::SameLine(0.f, 4.f);

                // Label
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                ImGui::TextUnformatted(label);
                ImGui::PopStyleColor();
                ImGui::SameLine(0.f, 4.f);

                // Slider — editing live updates the static field; if keyframe exists at
                // this time, also updates that keyframe's value.
                ImGui::PushStyleColor(ImGuiCol_SliderGrab, has_kf ? IM_COL32(255,200,60,255) : to_u32(Col::fg));
                ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
                float slider_w = w - 20.f - ImGui::CalcTextSize(label).x - 28.f;
                ImGui::SetNextItemWidth(fmaxf(40.f, slider_w));
                char sid[64]; snprintf(sid, sizeof(sid), "##kfs_%s", prop);
                if (ImGui::SliderFloat(sid, val_ptr, vmin, vmax, fmt)) {
                    changed = true;
                    if (has_kf) {
                        int ki = pt.find_nearest(t_local, 0.05f);
                        if (ki >= 0) pt.keys[ki].value = *val_ptr;
                    }
                }
                ImGui::PopStyleColor(2);

                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    history_push(state, std::string("Edit ") + prop);
                }

                return changed;
            };

            kf_slider("pos_x",    "X",    &clip.pos_x,    0.f, 1.f, "%.2f");
            kf_slider("pos_y",    "Y",    &clip.pos_y,    0.f, 1.f, "%.2f");
            kf_slider("scale_x",  "ScX",  &clip.scale_x,  0.f, 4.f, "%.2f");
            kf_slider("scale_y",  "ScY",  &clip.scale_y,  0.f, 4.f, "%.2f");
            kf_slider("rotation", "Rot",  &clip.rotation, -180.f, 180.f, "%.1f\xc2\xb0");
            kf_slider("opacity",  "Opa",  &clip.opacity,  0.f, 1.f, "%.2f");

            // ── Keyframe interp selector ──────────────────────────────────────
            bool sel_this = (state.kf_sel_track == sel_ti &&
                             state.kf_sel_clip  == sel_ci &&
                             !state.kf_sel_prop.empty() &&
                             state.kf_sel_idx   >= 0);
            if (sel_this) {
                auto it2 = clip.ktracks.find(state.kf_sel_prop);
                if (it2 != clip.ktracks.end() &&
                    state.kf_sel_idx < (int)it2->second.keys.size()) {
                    Keyframe& kf = it2->second.keys[state.kf_sel_idx];
                    ImGui::Dummy({0.f, 4.f});
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                    ImGui::TextUnformatted("Interpolation:");
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0.f, 6.f);
                    struct IT { InterpType t; const char* n; };
                    IT its[] = {{InterpType::Linear,"Lin"},{InterpType::EaseIn,"In"},
                                {InterpType::EaseOut,"Out"},{InterpType::EaseBoth,"Both"},
                                {InterpType::Hold,"Hold"}};
                    for (auto& it3 : its) {
                        if (ui_btn(it3.n, kf.interp == it3.t, true)) {
                            kf.interp = it3.t;
                            history_push(state, "KF interp");
                        }
                        ImGui::SameLine(0.f, 4.f);
                    }
                    ImGui::NewLine();
                }
            }
        }

        ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

        // ML Processing
        ui_label("ML Processing"); ImGui::Dummy({0.f, 6.f});
        bool busy      = transcribe_running();
        bool has_path  = !clip.text.empty();
        bool ml_avail  = state.models_ready;

        if (!ml_avail && !busy) {
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextWrapped("Lyric extraction models not installed.");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 2.f});
            if (ui_btn("Download Models…", false, true))
                state.show_model_dl_modal = true;
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
            char pbuf[128];
            snprintf(pbuf, sizeof(pbuf), "%s  %d%%", msg.c_str(), (int)(state.pipeline.progress * 100.f));
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted(pbuf);
            if (!state.pipeline.raw_line.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                ImGui::SetNextItemWidth(w - 16.f);
                std::string raw = state.pipeline.raw_line;
                if (raw.size() > 100) raw = raw.substr(0, 97) + "...";
                ImGui::TextUnformatted(raw.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 4.f});
            if (ui_btn("Cancel", false, true)) transcribe_cancel();
        } else {
            if (!has_path) ImGui::BeginDisabled();
            if (ui_btn("Extract Lyrics", false, true))
                kick_pipeline(state, clip.text, PipelineMode::Both);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Separate vocals + transcribe → Lyrics track");
                ImGui::EndTooltip();
            }
            ImGui::Dummy({0.f, 2.f});
            if (ui_btn("Extract Subtitles", false, true))
                kick_pipeline(state, clip.text, PipelineMode::TranscribeOnly);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Transcribe only → Subtitles track");
                ImGui::EndTooltip();
            }
            ImGui::Dummy({0.f, 2.f});
            if (ui_btn("Separate Vocals", false, true))
                kick_pipeline(state, clip.text, PipelineMode::SeparateOnly);
            if (clip.clip_type == ClipType::Video) {
                ImGui::Dummy({0.f, 2.f});
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted("Coming soon — requires rembg in the venv");
                    ImGui::EndTooltip();
                }
                ImGui::BeginDisabled();
                ui_btn("Remove Background  (rembg)", false, true);
                ImGui::EndDisabled();
            }
            if (!has_path) ImGui::EndDisabled();
        }
        if (!ml_avail) ImGui::EndDisabled();
    }

    ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
    if (ui_btn("Delete clip", false, true)) {
        track.clips.erase(track.clips.begin() + state.selected_clip);
        state.selected_clip = -1;
        history_push(state, "Delete clip");
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
        struct ModeBtn { SubtitleMode m; const char* label; const char* tip; };
        static const ModeBtn modes[] = {
            {SubtitleMode::Word,    "Word",    "One clip per word"},
            {SubtitleMode::Phrase,  "Phrase",  "Group by short pauses (>0.3s)"},
            {SubtitleMode::Line,    "Line",    "Group by breath gaps (>0.8s)"},
            {SubtitleMode::Karaoke, "Karaoke", "Line groups with per-word highlight"},
            {SubtitleMode::Segment, "Segment", "WhisperX sentence boundaries"},
            {SubtitleMode::CustomN, "Custom",  "N words per clip"},
        };
        for (auto& mb : modes) {
            bool sel = state.subtitle_mode == mb.m;
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

        ImGui::Dummy({0.f, 8.f});
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

        // Apply grouping to selected clips only
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
                // Apply karaoke flag (set by Karaoke mode) to selected clips
                bool is_karaoke = state.subtitle_mode == SubtitleMode::Karaoke;
                for (auto& [st, sc] : state.clip_selection) {
                    if (st == state.selected_track && sc == state.selected_clip) continue;
                    if (st >= (int)state.tracks.size() || sc >= (int)state.tracks[st].clips.size()) continue;
                    Clip& tgt = state.tracks[st].clips[sc];
                    if (tgt.clip_type != ClipType::Lyrics) continue;
                    tgt.karaoke = is_karaoke;
                }
                history_push(state, "Apply grouping mode to selected");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Apply current grouping mode to selected Lyrics clips");
                ImGui::EndTooltip();
            }
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextWrapped("Run ML Processing on an audio clip to generate word JSON.");
        ImGui::PopStyleColor();
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
    ui_label("Animation style"); ImGui::Dummy({0.f, 8.f});

    float card_w = (w - 20.f) * 0.5f;
    float card_h = 82.f;

    for (int i = 0; i < 8; ++i) {
        if (i % 2 == 1) ImGui::SameLine(0.f, 8.f);
        const auto& sc = STYLES[i];
        bool sel = state.style == sc.style;

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
            state.style = sc.style;
            history_push(state, std::string("Style — ") + sc.name);
        }
        ImGui::PopStyleColor(2);
        if (i % 2 == 1 && i < 7) ImGui::Dummy({0.f, 4.f});
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
}

// ── Right panel: Export tab ───────────────────────────────────────────────────

static void panel_export(AppState& state, float w) {
    ImGui::Dummy({0.f, 8.f});
    ui_label("Output format"); ImGui::Dummy({0.f, 8.f});

    struct Fmt { OutputFormat fmt; const char* name; const char* ratio; const char* res; float sw, sh; };
    Fmt fmts[] = {
        {OutputFormat::Vertical,   "TikTok / Reels", "9:16", "1080×1920", 24.f, 42.f},
        {OutputFormat::Horizontal, "YouTube",        "16:9", "1920×1080", 54.f, 30.f},
        {OutputFormat::Square,     "Instagram",      "1:1",  "1080×1080", 36.f, 36.f},
    };
    float fw = (w - 16.f) / 3.f;
    for (int i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine(0.f, 4.f);
        bool sel = state.format == fmts[i].fmt;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, sel ? Col::bg_soft_hov : Col::bg_soft);
        ImGui::PushStyleColor(ImGuiCol_Border,  sel ? Col::fg : Col::line);
        char fid[8]; snprintf(fid, sizeof(fid), "##f%d", i);
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

    ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

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
        if (ImGui::GetIO().KeyCtrl && fabsf(wheel) > 0.f) {
            float old_zoom = zoom;
            zoom = fmaxf(20.f, fminf(zoom * (1.f + wheel * 0.1f), 4000.f));
            float mouse_t = (mouse.x - origin.x - TL_LABEL_W + scroll) / old_zoom;
            scroll = fmaxf(0.f, mouse_t * zoom - (mouse.x - origin.x - TL_LABEL_W));
        } else if (fabsf(wheel) > 0.f) {
            scroll = fmaxf(0.f, scroll - wheel * 60.f);
        }
        scroll = fminf(scroll, fmaxf(0.f, tl_content_w - clip_area_w + 60.f));
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

    // Tracks
    float track_y = origin.y + TL_RULER_H;
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

    for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
        Track& track = state.tracks[ti];
        ImVec2 row_tl = {origin.x, track_y};
        ImVec2 row_br = {origin.x+total_w, track_y+TL_TRACK_H};
        bool row_hov = mouse.y >= row_tl.y && mouse.y < row_br.y;
        if (row_hov) s_tl_hover_track = ti;

        dl->AddRectFilled(row_tl, row_br,
            to_u32(row_hov ? Col::bg_soft_hov : Col::bg_soft));
        dl->AddLine({origin.x, row_br.y}, {origin.x+total_w, row_br.y}, to_u32(Col::line));

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
            } else if (clip.clip_type==ClipType::Audio) {
                int bars = (int)((vis_x1-vis_x0)/3.f);
                for (int b=0;b<bars;++b) {
                    float bx=vis_x0+b*3.f+1.f;
                    float amp=0.25f+0.55f*fabsf(sinf(b*0.37f+ti)*cosf(b*0.11f));
                    float mid=(cy0+cy1)*0.5f, ht=amp*(cy1-cy0-6.f)*0.5f;
                    dl->AddLine({bx,mid-ht},{bx,mid+ht},to_u32(Col::muted));
                }
            } else if (clip.clip_type==ClipType::Video) {
                ImGui::PushClipRect({vis_x0,cy0},{vis_x1,cy1},true);
                dl->AddText({vis_x0+4.f,cy0+3.f},
                    to_u32(sel?Col::bg:Col::fg),
                    clip.text.empty()?"Video":fs::path(clip.text).filename().string().c_str());
                ImGui::PopClipRect();
            }

            // Edge handles
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
                            state.panel_tab      = 0;
                        }
                    }
                }
            }

            // Transition out indicator — diagonal slash at right edge of video clips
            if (clip.clip_type == ClipType::Video && clip.transition_out > 0.f) {
                float trans_px = fminf(clip.transition_out * zoom, cx1 - cx0);
                float tx = vis_x1 - trans_px;
                if (tx < vis_x1 && tx >= vis_x0) {
                    dl->AddTriangleFilled(
                        {tx, cy0}, {vis_x1, cy0}, {vis_x1, cy1},
                        IM_COL32(255,255,255,40));
                    dl->AddLine({tx, cy0}, {vis_x1, cy1},
                        IM_COL32(255,255,255,130));
                }
            }

            // Left click to select / drag
            if (ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemActive()) {
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
                    state.panel_tab = (clip.clip_type == ClipType::Lyrics) ? 4 : 0;

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

        track_y += TL_TRACK_H;
    }

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
    if (drag_track>=0 && drag_clip>=0 && ImGui::IsMouseDragging(0)) {
        Clip& dc = state.tracks[drag_track].clips[drag_clip];
        auto cands = build_snap_candidates(drag_track, drag_clip);
        float new_t = (mouse.x - origin.x - TL_LABEL_W + scroll - drag_offset) / zoom;
        if (drag_left) {
            float t = edge_snap(snap(new_t), cands);
            dc.start = fmaxf(0.f, fminf(t, dc.end - f1));
        } else if (drag_right) {
            float et = (mouse.x - origin.x - TL_LABEL_W + scroll) / zoom;
            float t = edge_snap(snap(et), cands);
            dc.end = fmaxf(dc.start + f1, t);
        } else {
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
    if ((ImGui::IsMouseClicked(0)||ImGui::IsMouseDragging(0)) && drag_track<0) {
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

    // "+ Add Track" row
    ImVec2 add_p = {origin.x+8.f, track_y+6.f};
    ImVec2 mp = ImGui::GetIO().MousePos;
    bool add_hov = mp.y>=track_y && mp.y<track_y+TL_TRACK_H &&
                   mp.x>=origin.x && mp.x<=origin.x+total_w;
    dl->AddText(add_p, to_u32(add_hov ? Col::fg : Col::muted), "+ Add Track");
    if (add_hov && ImGui::IsMouseClicked(0)) {
        Track t;
        char name[32]; snprintf(name,sizeof(name),"Track %d",(int)state.tracks.size()+1);
        t.name=name; state.tracks.insert(state.tracks.begin(), std::move(t));
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

    // Proxy ready → upgrade from still to proxy for each registered file path.
    for (int slot = 0; slot < MAX_VIDEO_TRACKS; ++slot) {
        const std::string& vpath = state.proxy_paths[slot];
        if (vpath.empty()) continue;
        if (!video_is_open(slot) || video_info(slot).fps > 0.0) continue; // still or already proxy
        if (!proxy_is_ready(vpath)) continue;

        ProxyInfo pi;
        if (!proxy_load(vpath, pi)) continue;
        video_open_proxy(slot, pi);

        if (slot == 0) {
            state.proxy_ready = true;
            float pd = (float)video_info(0).duration;
            if (pd > 0.f) {
                // Extend all video clips for this file to match corrected duration.
                for (auto& tr : state.tracks)
                    for (auto& cl : tr.clips)
                        if (cl.clip_type == ClipType::Video && cl.text == vpath && cl.end < pd)
                            cl.end = pd;
            }
        }
    }

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

    // Extract audio done → add Audio track
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
            state.tracks.insert(state.tracks.begin(), std::move(at));
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
    }
    last_stage = state.pipeline.stage;

    // Per-clip volume — find the audio/video clip covering the playhead and apply its gain
    {
        float vol = 1.f;
        for (auto& tr : state.tracks) {
            if (tr.muted) { vol = 0.f; break; }
            for (auto& cl : tr.clips) {
                if (cl.clip_type == ClipType::Text || cl.clip_type == ClipType::Lyrics) continue;
                if (state.playhead >= cl.start && state.playhead < cl.end) {
                    vol = cl.volume; break;
                }
            }
        }
        audio_set_volume(vol);
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
            ImGui::Separator();
            if (ImGui::MenuItem("Close project")) {
                transcribe_cancel();
                history_clear();
                state = AppState{};
                state.splash_timer = 0.f;  // don't re-show splash
                audio_shutdown(); audio_init();
                video_close();
            }
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

    // Timeline height — user-draggable, default auto
    float tl_h_auto  = fminf(200.f, TL_RULER_H + ((int)state.tracks.size()+2) * TL_TRACK_H);
    float tl_h       = (state.tl_h_frac > 0.f)
                        ? fmaxf(60.f, fminf(avail_h * 0.7f, state.tl_h_frac * avail_h))
                        : tl_h_auto;
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
        float ah = ImGui::GetContentRegionAvail().y - 36.f; // leave room for transport bar

        float asp_w = 9.f, asp_h = 16.f;
        if (state.format == OutputFormat::Horizontal) { asp_w = 16.f; asp_h = 9.f; }
        else if (state.format == OutputFormat::Square) { asp_w = 1.f; asp_h = 1.f; }
        float sw, sh;
        if (aw / ah > asp_w / asp_h) { sh = ah; sw = sh * asp_w / asp_h; }
        else                          { sw = aw; sh = sw * asp_h / asp_w; }
        float ox = (aw - sw) * 0.5f;
        float oy = (ah - sh) * 0.5f;
        ImGui::SetCursorPos({ox, oy});
        ImVec2 stage_p = ImGui::GetCursorScreenPos();
        ImGui::Dummy({sw, sh});
        draw_preview(state, stage_p, sw, sh);

        // ── YouTube-style overlay scrub bar ───────────────────────────────────
        {
            static float s_bar_anim = 3.f;
            static float s_dim_anim = 0.f;

            const float BAR_FULL = 8.f;
            const float BAR_THIN = 3.f;
            const float BAR_ZONE = 22.f;  // hover hit area height

            float fps_ov  = tl_fps(state);
            float dur_ov  = fmaxf(state.duration, 0.01f);
            float bar_bottom = stage_p.y + sh;
            float bar_left   = stage_p.x;
            float bar_right  = stage_p.x + sw;

            // Invisible hit area covering bottom of video
            ImGui::SetCursorScreenPos({bar_left, bar_bottom - BAR_ZONE});
            ImGui::InvisibleButton("##scrub_ov", {sw, BAR_ZONE});
            bool hov  = ImGui::IsItemHovered();
            bool held = ImGui::IsItemActive();

            // Dim video while scrubbing
            float dim_target = held ? 110.f : 0.f;
            s_dim_anim += (dim_target - s_dim_anim) * ImGui::GetIO().DeltaTime * 12.f;
            if (s_dim_anim > 1.f) {
                ImDrawList* dl_dim = ImGui::GetWindowDrawList();
                dl_dim->AddRectFilled(stage_p, {stage_p.x + sw, stage_p.y + sh},
                                      IM_COL32(0, 0, 0, (int)s_dim_anim));
            }

            // Animate bar height
            float target_h = (hov || held) ? BAR_FULL : BAR_THIN;
            s_bar_anim += (target_h - s_bar_anim) * ImGui::GetIO().DeltaTime * 14.f;
            s_bar_anim = fmaxf(BAR_THIN, fminf(BAR_FULL, s_bar_anim));
            float bh = s_bar_anim;

            // Resolve hover time from mouse x
            float mouse_t = 0.f;
            bool  has_hover = false;
            if (hov || held) {
                float frac = (ImGui::GetIO().MousePos.x - bar_left) / sw;
                frac  = fmaxf(0.f, fminf(1.f, frac));
                mouse_t = frac * dur_ov;
                if (!ImGui::GetIO().KeyCtrl && fps_ov > 0.f)
                    mouse_t = roundf(mouse_t * fps_ov) / fps_ov;
                has_hover = true;
                if (held) seek_to(state, mouse_t);
            }

            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Gradient behind bar for readability
            float grad_h = 48.f;
            dl->AddRectFilledMultiColor(
                {bar_left, bar_bottom - bh - grad_h},
                {bar_right, bar_bottom - bh},
                IM_COL32(0,0,0,0),   IM_COL32(0,0,0,0),
                IM_COL32(0,0,0,120), IM_COL32(0,0,0,120));

            // Track background
            dl->AddRectFilled({bar_left, bar_bottom - bh}, {bar_right, bar_bottom},
                              IM_COL32(90, 90, 90, 180));

            // Played portion
            float play_x = bar_left + (state.playhead / dur_ov) * sw;
            dl->AddRectFilled({bar_left, bar_bottom - bh}, {play_x, bar_bottom},
                              IM_COL32(255, 60, 60, 230));

            // Hover thumbnail + ghost dot
            if (has_hover) {
                float hx = bar_left + (mouse_t / dur_ov) * sw;

                // Hover highlight on played portion
                dl->AddRectFilled({bar_left, bar_bottom - bh}, {hx, bar_bottom},
                                  IM_COL32(255, 90, 90, 60));

                // Thumbnail
                int th_w = 0, th_h = 0;
                uintptr_t th_tex = video_get_thumbnail((double)mouse_t, &th_w, &th_h);
                if (th_tex && th_w > 0 && th_h > 0) {
                    float td_w = 120.f;
                    float td_h = td_w * (float)th_h / (float)th_w;
                    float tx = hx - td_w * 0.5f;
                    float ty = bar_bottom - bh - 6.f - td_h - 18.f; // 18 for timecode
                    if (tx < bar_left)             tx = bar_left;
                    if (tx + td_w > bar_right)     tx = bar_right - td_w;
                    dl->AddRectFilled({tx-2.f, ty-2.f}, {tx+td_w+2.f, ty+td_h+2.f},
                                      IM_COL32(20, 20, 20, 230), 3.f);
                    dl->AddImage((ImTextureID)(uintptr_t)th_tex,
                                 {tx, ty}, {tx+td_w, ty+td_h});
                    // Timecode below thumbnail
                    char tcbuf[16];
                    snprintf(tcbuf, sizeof(tcbuf), "%s", fmt_time(mouse_t).c_str());
                    float tc_w = ImGui::CalcTextSize(tcbuf).x;
                    dl->AddText({tx + (td_w - tc_w)*0.5f, ty + td_h + 4.f},
                                IM_COL32(220, 220, 220, 220), tcbuf);
                } else {
                    // No video — timecode only
                    char tcbuf[16];
                    snprintf(tcbuf, sizeof(tcbuf), "%s", fmt_time(mouse_t).c_str());
                    float tc_w = ImGui::CalcTextSize(tcbuf).x;
                    dl->AddText({hx - tc_w*0.5f, bar_bottom - bh - 20.f},
                                IM_COL32(220, 220, 220, 220), tcbuf);
                }

                // Hover dot on bar
                dl->AddCircleFilled({hx, bar_bottom - bh*0.5f}, bh*0.9f + 1.f,
                                    IM_COL32(0,0,0,80));
                dl->AddCircleFilled({hx, bar_bottom - bh*0.5f}, bh*0.9f,
                                    IM_COL32(255,255,255,180));
            }

            // Playhead dot
            float dot_r = (hov || held) ? bh : bh * 0.7f;
            dl->AddCircleFilled({play_x, bar_bottom - bh*0.5f}, dot_r + 1.f,
                                IM_COL32(0,0,0,100));
            dl->AddCircleFilled({play_x, bar_bottom - bh*0.5f}, dot_r,
                                IM_COL32(255, 255, 255, 255));
        }

        // ── Transport buttons ─────────────────────────────────────────────────
        float fps_v  = tl_fps(state);
        float f_dt_v = fps_v > 0.f ? 1.f / fps_v : 1.f / 30.f;
        float dur    = fmaxf(state.duration, 0.01f);

        const float SB  = 22.f;
        const float PB  = 30.f;
        const float GAP = 6.f;
        float btns_w = SB*4 + PB + GAP*4;
        float bx = ox + (sw - btns_w) * 0.5f;
        float by = oy + sh + 6.f;   // just below video
        ImDrawList* wdl = ImGui::GetWindowDrawList();

        // Helper: invisible button → returns click; out_col is icon colour
        auto tbtn = [&](const char* id, float w, float h) -> bool {
            ImGui::SetCursorPos({bx - ImGui::GetWindowPos().x +
                                 ImGui::GetWindowPos().x,   // no-op, just use SetCursorScreenPos
                                 by - ImGui::GetWindowPos().y});
            // Use screen-space cursor directly
            ImGui::SetCursorScreenPos({bx, by});
            ImGui::InvisibleButton(id, {w, h});
            return ImGui::IsItemClicked();
        };

        // Better helper using screen pos
        auto icon_btn = [&](const char* id, float sz) -> std::pair<bool,ImU32> {
            ImGui::SetCursorScreenPos({bx, by});
            ImGui::InvisibleButton(id, {sz, sz});
            bool hov  = ImGui::IsItemHovered();
            bool held = ImGui::IsItemActive();
            bool clk  = ImGui::IsItemClicked();
            if (hov || held)
                wdl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                   IM_COL32(255,255,255, held?40:20), 5.f);
            ImU32 col = IM_COL32(255,255,255, held?255 : hov?210 : 140);
            bx += sz + GAP;
            return {clk, col};
        };
        (void)tbtn; // suppress unused warning

        // |< skip to start
        {
            auto [clk, col] = icon_btn("##t_start", SB);
            ImVec2 p = ImGui::GetItemRectMin();
            float cx = p.x + SB*0.5f, cy = p.y + SB*0.5f;
            float r = SB*0.28f;
            wdl->AddRectFilled({p.x+3.f, cy-r},{p.x+5.5f, cy+r}, col, 1.f);
            wdl->AddTriangleFilled({cx+r*0.3f,cy-r},{cx+r*0.3f,cy+r},{cx-r*1.1f,cy}, col);
            if (clk) seek_to(state, 0.f);
        }

        // < step back
        {
            auto [clk, col] = icon_btn("##t_prev", SB);
            ImVec2 p = ImGui::GetItemRectMin();
            float cx = p.x + SB*0.5f, cy = p.y + SB*0.5f;
            float r = SB*0.28f;
            wdl->AddTriangleFilled({cx+r,cy-r},{cx+r,cy+r},{cx-r,cy}, col);
            if (clk) seek_to(state, fmaxf(0.f, state.playhead - f_dt_v));
        }

        // play / pause  (larger, with subtle circle bg)
        {
            ImGui::SetCursorScreenPos({bx, by - (PB-SB)*0.5f});
            ImGui::InvisibleButton("##t_play", {PB, PB});
            bool hov  = ImGui::IsItemHovered();
            bool held = ImGui::IsItemActive();
            bool clk  = ImGui::IsItemClicked();
            ImVec2 p  = ImGui::GetItemRectMin();
            float cx = p.x + PB*0.5f, cy = p.y + PB*0.5f;
            // Circle background
            wdl->AddCircleFilled({cx,cy}, PB*0.5f,
                                 IM_COL32(255,255,255, held?55 : hov?38 : 22));
            wdl->AddCircle({cx,cy}, PB*0.5f-1.f, IM_COL32(255,255,255,60), 0, 1.f);
            ImU32 col = IM_COL32(255,255,255, held?255 : hov?230 : 190);
            float r = PB*0.22f;
            if (audio_loading() || proxy_is_generating() || state.extract_running) {
                // Spinner dots — just three small dots for "busy"
                for (int i=0;i<3;++i)
                    wdl->AddCircleFilled({cx-r+i*r, cy}, 2.f,
                                        IM_COL32(255,255,255,120));
            } else if (state.playing) {
                // Pause: two bars
                float bw=r*0.55f, bh=r*1.6f;
                wdl->AddRectFilled({cx-bw*1.4f, cy-bh},{cx-bw*0.3f, cy+bh}, col, 1.f);
                wdl->AddRectFilled({cx+bw*0.3f, cy-bh},{cx+bw*1.4f, cy+bh}, col, 1.f);
            } else {
                // Play triangle (offset slightly right to look centred)
                wdl->AddTriangleFilled({cx-r*0.7f, cy-r*1.1f},
                                       {cx-r*0.7f, cy+r*1.1f},
                                       {cx+r*1.1f, cy}, col);
            }
            if (clk && !audio_loading() && !proxy_is_generating() && !state.extract_running)
                toggle_play(state);
            bx += PB + GAP;
        }

        // > step forward
        {
            auto [clk, col] = icon_btn("##t_next", SB);
            ImVec2 p = ImGui::GetItemRectMin();
            float cx = p.x + SB*0.5f, cy = p.y + SB*0.5f;
            float r = SB*0.28f;
            wdl->AddTriangleFilled({cx-r,cy-r},{cx-r,cy+r},{cx+r,cy}, col);
            if (clk) seek_to(state, fminf(dur, state.playhead + f_dt_v));
        }

        // >| skip to end
        {
            auto [clk, col] = icon_btn("##t_end", SB);
            ImVec2 p = ImGui::GetItemRectMin();
            float cx = p.x + SB*0.5f, cy = p.y + SB*0.5f;
            float r = SB*0.28f;
            wdl->AddTriangleFilled({cx-r*1.1f,cy-r},{cx-r*1.1f,cy+r},{cx+r*0.3f,cy}, col);
            wdl->AddRectFilled({cx+r*0.3f, cy-r},{cx+r*0.3f+2.5f, cy+r}, col, 1.f);
            if (clk) seek_to(state, dur);
        }

        // Status text (right of buttons, left of timecode)
        if (audio_loading() || proxy_is_generating() || state.extract_running) {
            ImGui::SetCursorScreenPos({bx, by + (SB-13.f)*0.5f});
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            if      (audio_loading())       ImGui::TextUnformatted("loading…");
            else if (proxy_is_generating()) ImGui::TextUnformatted("building preview…");
            else                            ImGui::TextUnformatted("extracting…");
            ImGui::PopStyleColor();
        }

        // Timecode right-aligned
        char tcbuf[32];
        snprintf(tcbuf, sizeof(tcbuf), "%s / %s",
            fmt_time(state.playhead).c_str(),
            fmt_time(dur).c_str());
        float tc_w = ImGui::CalcTextSize(tcbuf).x;
        ImGui::SetCursorScreenPos({ImGui::GetWindowPos().x + ox + sw - tc_w,
                                   by + (SB - ImGui::GetTextLineHeight())*0.5f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted(tcbuf);
        ImGui::PopStyleColor();
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
        bool lyrics_selected = false;
        if (state.selected_track >= 0 && state.selected_track < (int)state.tracks.size() &&
            state.selected_clip  >= 0 && state.selected_clip  < (int)state.tracks[state.selected_track].clips.size())
            lyrics_selected = state.tracks[state.selected_track].clips[state.selected_clip].clip_type == ClipType::Lyrics;

        if (!lyrics_selected && state.panel_tab == 4) state.panel_tab = 0;

        if (ImGui::BeginTabBar("##panel_tabs")) {
            if (ImGui::BeginTabItem("Clip"))      { state.panel_tab=0; ImGui::EndTabItem(); }
            if (lyrics_selected && ImGui::BeginTabItem("Lyrics")) { state.panel_tab=4; ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Animation")) { state.panel_tab=1; ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Export"))    { state.panel_tab=2; ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("History"))   { state.panel_tab=3; ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();

        ImGui::BeginChild("##panel_scroll", {0.f, 0.f});
        ImGui::SetCursorPosX(8.f);
        float pw = props_w - 16.f;
        if      (state.panel_tab == 0) panel_clip(state, pw);
        else if (state.panel_tab == 1) panel_animation(state, pw);
        else if (state.panel_tab == 2) panel_export(state, pw);
        else if (state.panel_tab == 4) panel_lyrics(state, pw);
        else                           panel_history(state, pw);
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

    // ── Timeline ─────────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col::bg);
    ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
    if (ImGui::BeginChild("##tl_zone", {win_w, tl_h}, ImGuiChildFlags_Borders)) {
        ImVec2 tl_origin = ImGui::GetCursorScreenPos();
        float  tl_w      = ImGui::GetContentRegionAvail().x;
        float  tl_h_real = ImGui::GetContentRegionAvail().y;
        ImGui::Dummy({tl_w, tl_h_real});
        draw_timeline(state, tl_origin, tl_w, tl_h_real);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}
