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
    return (state.proxy_ready && video_info().fps > 0.0)
           ? (float)video_info().fps : 30.f;
}

static bool is_audio_file(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    return ext==".wav"||ext==".mp3"||ext==".m4a"||
           ext==".flac"||ext==".mp4"||ext==".mov"||ext==".aac";
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

    case SubtitleMode::Segment:
        // Handled separately via segments JSON — fall back to Line grouping
        // if segment data isn't available.
        return group_words(words, SubtitleMode::Line, custom_n);
    }

    return out;
}

// Load word JSON and apply current grouping mode to the Lyrics track.
static void apply_subtitle_mode(AppState& state) {
    if (state.words_json_path.empty()) return;

    // For Segment mode, read _segments.json instead
    if (state.subtitle_mode == SubtitleMode::Segment &&
        !state.segments_json_path.empty() &&
        fs::exists(state.segments_json_path)) {
        std::ifstream f(state.segments_json_path);
        if (!f) return;
        try {
            auto j = nlohmann::json::parse(f);
            // Find or create Lyrics track
            Track* lyrics = nullptr;
            for (auto& t : state.tracks)
                if (t.name == "Lyrics") { lyrics = &t; break; }
            if (!lyrics) {
                state.tracks.insert(state.tracks.begin(), Track{});
                lyrics = &state.tracks[0];
                lyrics->type = TrackType::Subtitle;
                lyrics->name = "Lyrics";
            }
            lyrics->clips.clear();
            for (auto& seg : j) {
                Clip c;
                c.text  = seg["text"].get<std::string>();
                c.start = seg["start"].get<float>();
                c.end   = seg["end"].get<float>();
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

        Track* lyrics = nullptr;
        for (auto& t : state.tracks)
            if (t.name == "Lyrics") { lyrics = &t; break; }
        if (!lyrics) {
            state.tracks.insert(state.tracks.begin(), Track{});
            lyrics = &state.tracks[0];
            lyrics->type = TrackType::Subtitle;
            lyrics->name = "Lyrics";
        }
        lyrics->clips = grouped;
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
        state.video_path  = path;
        state.video_loaded = true;
        state.proxy_ready  = false;

        // Start proxy generation (no-op if proxy already exists on disk).
        // This also extracts the preview still synchronously (< 1 s).
        proxy_start(path);

        // Show still immediately while proxy generates; switch to proxy
        // once ready (checked each frame in the main loop below).
        std::string still = proxy_still_path(path);
        video_open_still(still);

        // If the proxy already existed from a previous session, open it now.
        if (proxy_is_ready(path)) {
            ProxyInfo pi;
            if (proxy_load(path, pi)) {
                video_open_proxy(pi);
                state.proxy_ready = true;
            }
        }

        // audio_load probes duration from the container header synchronously
        // before spawning its background decode thread — use that as the
        // primary duration source since it works on all formats.
        audio_load(path);
        state.duration = audio_duration();
        // video_probe_duration is a faster path that works when the container
        // header carries duration; use it only as an override if it succeeds.
        float vprobed = video_probe_duration(path);
        if (vprobed > 0.f) state.duration = vprobed;

        // Add or update Video track
        Track* vt = nullptr;
        for (auto& t : state.tracks) if (t.type==TrackType::Video) { vt=&t; break; }
        if (!vt) { state.tracks.push_back({}); vt=&state.tracks.back(); }
        vt->type = TrackType::Video; vt->name = "Video";
        vt->clips.clear();
        Clip vc; vc.start=0.f; vc.end=state.duration; vc.text=path;
        vt->clips.push_back(vc);
    } else {
        state.audio_path = path;
        audio_load(path);  // async — also probes container duration
        state.duration = audio_duration();

        // Add Audio track
        Track* at = nullptr;
        for (auto& t : state.tracks)
            if (t.type==TrackType::Audio && t.name=="Audio") { at=&t; break; }
        if (!at) { state.tracks.push_back({}); at=&state.tracks.back(); }
        at->type = TrackType::Audio; at->name = "Audio";
        at->clips.clear();
        Clip ac; ac.start=0.f; ac.end=state.duration; ac.text=path;
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

    if (mode != PipelineMode::SeparateOnly) {
        // Add placeholder Lyrics track while processing
        bool has_lyrics = false;
        for (auto& t : state.tracks) if (t.name=="Lyrics") { has_lyrics=true; break; }
        if (!has_lyrics) {
            Track ph; ph.type=TrackType::Subtitle; ph.name="Lyrics";
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
    float  h = 28.f;

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

    if (state.video_loaded && video_is_open()) {
        // Look one frame ahead so the decoded frame matches the audio position
        // by the time it reaches the screen after the vsync buffer swap.
        float lookahead = ImGui::GetIO().DeltaTime;
        uintptr_t tex = video_get_texture((double)(state.playhead + lookahead));
        if (tex)
            dl->AddImage(ImTextureRef((ImTextureID)tex), p, {p.x+w, p.y+h});
        else
            dl->AddRectFilled(p, {p.x+w, p.y+h}, to_u32(Col::accent_dark), 2.f);
    } else {
        dl->AddRectFilled(p, {p.x+w, p.y+h}, to_u32(Col::accent_dark), 2.f);
    }
    dl->AddRect(p, {p.x+w, p.y+h}, to_u32(Col::line), 2.f);
    // Format label
    {
        const char* fmt_lbl = state.format == OutputFormat::Vertical   ? "9:16" :
                              state.format == OutputFormat::Horizontal  ? "16:9" : "1:1";
        dl->AddText({p.x+6.f, p.y+6.f}, to_u32(Col::dim), fmt_lbl);
    }

    // Corner marks
    float cm = 10.f; ImU32 cc = to_u32(Col::muted);
    dl->AddLine(p,             {p.x+cm, p.y},       cc);
    dl->AddLine(p,             {p.x, p.y+cm},       cc);
    dl->AddLine({p.x+w, p.y}, {p.x+w-cm, p.y},     cc);
    dl->AddLine({p.x+w, p.y}, {p.x+w, p.y+cm},     cc);
    dl->AddLine({p.x, p.y+h}, {p.x+cm, p.y+h},     cc);
    dl->AddLine({p.x, p.y+h}, {p.x, p.y+h-cm},     cc);
    dl->AddLine({p.x+w,p.y+h},{p.x+w-cm,p.y+h},    cc);
    dl->AddLine({p.x+w,p.y+h},{p.x+w,p.y+h-cm},    cc);

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
        return;
    }

    // Active subtitle clips — stack vertically from bottom
    // Drag state: raw mouse detection, no ImGui widget (avoids backward-cursor assert)
    static int   s_drag_ti   = -1, s_drag_ci   = -1;
    static bool  s_dragging  = false;
    ImVec2 mpos = ImGui::GetIO().MousePos;
    bool   ldown = ImGui::IsMouseDown(0);
    bool   lclick = ImGui::IsMouseClicked(0);

    int rendered = 0;
    for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
        auto& track = state.tracks[ti];
        if (track.type != TrackType::Subtitle || !track.visible) continue;

        const Clip* active = nullptr;
        int active_ci = -1;
        for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
            if (state.playhead >= track.clips[ci].start &&
                state.playhead <  track.clips[ci].end) {
                active = &track.clips[ci];
                active_ci = ci;
                break;
            }
        }
        const Clip* show = active;
        int show_ci = active_ci;
        if (!show && state.selected_track == ti && state.selected_clip >= 0 &&
            state.selected_clip < (int)track.clips.size()) {
            show    = &track.clips[state.selected_clip];
            show_ci = state.selected_clip;
        }
        if (!show) { ++rendered; continue; }

        // Determine Y position from per-clip override
        float slot_h = 40.f;
        float slot_y;
        if (show->sub_pos == 1) {
            slot_y = p.y + h * 0.5f - slot_h * 0.5f;
        } else if (show->sub_pos == 2) {
            slot_y = p.y + 24.f + rendered * slot_h;
        } else if (show->sub_pos == 3) {
            slot_y = p.y + show->sub_pos_y * h - slot_h * 0.5f;
        } else {
            slot_y = p.y + h - 24.f - (rendered + 1) * slot_h;
        }

        ImGui::PushFont(g_font_black);
        ImGui::SetWindowFontScale(1.8f);
        ImVec2 tsz = ImGui::CalcTextSize(show->text.c_str());
        float  tx  = p.x + (w - tsz.x) * 0.5f;
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.8f,
            {tx+2.f, slot_y+2.f}, IM_COL32(0,0,0,180), show->text.c_str());

        ImU32 tcol;
        if (show->sub_color_override) {
            float alpha = (active_ci >= 0) ? show->sub_color[3] : show->sub_color[3] * 0.5f;
            tcol = IM_COL32(
                (int)(show->sub_color[0]*255), (int)(show->sub_color[1]*255),
                (int)(show->sub_color[2]*255), (int)(alpha*255));
        } else {
            tcol = (active_ci >= 0) ? to_u32(Col::fg) : to_u32(Col::muted);
        }
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.8f,
            {tx, slot_y}, tcol, show->text.c_str());
        ImGui::SetWindowFontScale(1.f);
        ImGui::PopFont();

        // Drag handle — raw mouse hit detection, no ImGui widget calls
        if (show_ci >= 0 && slot_y >= p.y && slot_y + tsz.y <= p.y + h) {
            float pad = 8.f;
            bool in_handle = mpos.x >= tx - pad && mpos.x <= tx + tsz.x + pad &&
                             mpos.y >= slot_y - pad && mpos.y <= slot_y + tsz.y + pad;
            bool is_this_drag = (s_drag_ti == ti && s_drag_ci == show_ci);

            if (in_handle || (is_this_drag && ldown))
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

            if (in_handle && lclick) {
                s_drag_ti = ti;
                s_drag_ci = show_ci;
            }

            if (is_this_drag && ldown) {
                s_dragging = true;
                Clip& mc = state.tracks[ti].clips[show_ci];
                float new_y = (mpos.y - p.y) / h;
                mc.sub_pos   = 3;
                mc.sub_pos_y = fmaxf(0.02f, fminf(0.98f, new_y));
            }

            // Dot indicator when hoverable
            if (in_handle && !ldown) {
                float mid_x = tx + tsz.x * 0.5f;
                for (int d = -1; d <= 1; ++d)
                    dl->AddCircleFilled({mid_x + d*6.f, slot_y - 6.f},
                        2.f, to_u32(Col::muted));
            }
        }

        ++rendered;
    }

    // Commit drag on mouse release
    if (s_dragging && !ldown) {
        history_push(state, "Subtitle position Y");
        s_dragging = false;
        s_drag_ti  = -1;
        s_drag_ci  = -1;
    }
}

// ── Right panel: Track tab ────────────────────────────────────────────────────

static void panel_track(AppState& state, float w) {
    if (state.selected_track < 0 || state.selected_track >= (int)state.tracks.size()) {
        ImGui::Dummy({0.f, 24.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        const char* hint = "Click a track label to select it";
        ImGui::SetCursorPosX((w - ImGui::CalcTextSize(hint).x) * 0.5f);
        ImGui::TextUnformatted(hint);
        ImGui::PopStyleColor();
        return;
    }

    Track& track = state.tracks[state.selected_track];

    ImGui::Dummy({0.f, 8.f});
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    const char* type_tag =
        track.type == TrackType::Subtitle ? "Subtitle" :
        track.type == TrackType::Audio    ? "Audio"    : "Video";
    char header[80];
    snprintf(header, sizeof(header), "Track — %s  ·  %s", track.name.c_str(), type_tag);
    ImGui::TextUnformatted(header);
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 4.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

    if (track.type == TrackType::Subtitle &&
        !state.words_json_path.empty() && fs::exists(state.words_json_path)) {

        ui_label("Subtitle grouping"); ImGui::Dummy({0.f, 4.f});

        struct ModeBtn { SubtitleMode m; const char* label; const char* tip; };
        ModeBtn modes[] = {
            {SubtitleMode::Word,    "Word",    "One clip per word"},
            {SubtitleMode::Phrase,  "Phrase",  "Group by short pauses (>0.3s)"},
            {SubtitleMode::Line,    "Line",    "Group by breath gaps (>0.8s)"},
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
            if (ImGui::InputInt("words/clip##tn", &n))
                state.subtitle_n = (n < 1) ? 1 : (n > 20) ? 20 : n;
            ImGui::PopStyleColor(2);
        }

        ImGui::Dummy({0.f, 8.f});
        if (ui_btn("Apply grouping", true, true)) {
            apply_subtitle_mode(state);
            const char* mode_name =
                state.subtitle_mode == SubtitleMode::Word    ? "Word"    :
                state.subtitle_mode == SubtitleMode::Phrase  ? "Phrase"  :
                state.subtitle_mode == SubtitleMode::Line    ? "Line"    :
                state.subtitle_mode == SubtitleMode::Segment ? "Segment" : "Custom";
            history_push(state, std::string("Grouping — ") + mode_name);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Re-build this track from saved word JSON");
            ImGui::EndTooltip();
        }

    } else if (track.type == TrackType::Subtitle) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextWrapped("Run ML Processing on an audio clip first to generate word-level JSON, then grouping controls will appear here.");
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted("Click a clip on this track to edit it.");
        ImGui::PopStyleColor();
    }
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

    // ── Track face — track selected, no clip — delegate to panel_track ────────
    if (state.selected_clip < 0 || state.selected_clip >= (int)track.clips.size()) {
        panel_track(state, w);
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

    if (track.type == TrackType::Subtitle) {
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
        if (track.type == TrackType::Video) {
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
        }

        ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

        // ML Processing
        ui_label("ML Processing"); ImGui::Dummy({0.f, 6.f});
        bool busy     = transcribe_running();
        bool has_path = !clip.text.empty();

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
            if (ui_btn("Transcribe + Separate", false, true))
                kick_pipeline(state, clip.text, PipelineMode::Both);
            ImGui::Dummy({0.f, 2.f});
            if (ui_btn("Transcribe only", false, true))
                kick_pipeline(state, clip.text, PipelineMode::TranscribeOnly);
            ImGui::Dummy({0.f, 2.f});
            if (ui_btn("Separate Vocals only", false, true))
                kick_pipeline(state, clip.text, PipelineMode::SeparateOnly);
            if (track.type == TrackType::Video) {
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
    }

    ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
    if (ui_btn("Delete clip", false, true)) {
        track.clips.erase(track.clips.begin() + state.selected_clip);
        state.selected_clip = -1;
        history_push(state, "Delete clip");
    }
}

// ── Right panel: Style tab ────────────────────────────────────────────────────

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

static void panel_style(AppState& state, float w) {
    ImGui::Dummy({0.f, 8.f});
    ui_label("Animation style"); ImGui::Dummy({0.f, 8.f});

    float card_w = (w - 20.f) * 0.5f;
    float card_h = 72.f;

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

static constexpr float TL_LABEL_W = 110.f;
static constexpr float TL_TRACK_H = 36.f;
static constexpr float TL_RULER_H = 20.f;

// ── Timeline ──────────────────────────────────────────────────────────────────

static void draw_timeline(AppState& state, ImVec2 origin, float total_w, float total_h) {
    ImDrawList* dl      = ImGui::GetWindowDrawList();
    float clip_area_w   = total_w - TL_LABEL_W;
    float dur           = fmaxf(state.duration, 1.f);
    float& zoom         = state.tl_zoom;
    float& scroll       = state.tl_scroll;
    float tl_content_w  = dur * zoom;

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
    float fps = (state.proxy_ready && video_info().fps > 0.0)
                ? (float)video_info().fps : 30.f;
    // snap(t): round t to nearest frame unless Ctrl is held
    auto snap = [&](float t) -> float {
        if (ImGui::GetIO().KeyCtrl || fps <= 0.f) return t;
        return roundf(t * fps) / fps;
    };

    // ── Ruler ─────────────────────────────────────────────────────────────────
    float ruler_y = origin.y;
    dl->AddRectFilled({origin.x+TL_LABEL_W, ruler_y},
        {origin.x+total_w, ruler_y+TL_RULER_H}, to_u32(Col::bg_soft));
    dl->AddLine({origin.x, ruler_y+TL_RULER_H},
        {origin.x+total_w, ruler_y+TL_RULER_H}, to_u32(Col::line));

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
    static int   drag_track = -1, drag_clip = -1;
    static float drag_offset = 0.f;
    static bool  drag_left = false, drag_right = false;

    // Context menu state
    static int ctx_track = -1, ctx_clip = -1;
    static bool open_clip_ctx = false, open_track_ctx = false, open_tl_ctx = false;

    for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
        Track& track = state.tracks[ti];
        ImVec2 row_tl = {origin.x, track_y};
        ImVec2 row_br = {origin.x+total_w, track_y+TL_TRACK_H};
        bool row_hov = mouse.y >= row_tl.y && mouse.y < row_br.y;

        dl->AddRectFilled(row_tl, row_br,
            to_u32(row_hov ? Col::bg_soft_hov : Col::bg_soft));
        dl->AddLine({origin.x, row_br.y}, {origin.x+total_w, row_br.y}, to_u32(Col::line));

        // Track label — left-click selects the track (clears clip selection)
        bool track_sel = state.selected_track == ti;
        dl->AddText({origin.x+8.f, track_y+(TL_TRACK_H-13.f)*0.5f},
            to_u32(track_sel ? Col::fg : Col::muted), track.name.c_str());
        if (ImGui::IsMouseClicked(0) &&
            mouse.x >= origin.x+2.f && mouse.x < origin.x+TL_LABEL_W-20.f &&
            mouse.y >= track_y && mouse.y < track_y+TL_TRACK_H) {
            state.selected_track = ti;
            state.selected_clip  = -1;
            state.panel_tab      = 2;  // Track tab
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
            bool sel = (state.selected_track==ti && state.selected_clip==ci);

            ImVec4 clip_fill = (track.type==TrackType::Subtitle) ? Col::clip_sub
                             : (track.type==TrackType::Audio)    ? Col::clip_audio
                                                                 : Col::clip_video;
            dl->AddRectFilled({vis_x0,cy0},{vis_x1,cy1},
                to_u32(sel ? Col::fg : clip_fill), 2.f);
            dl->AddRect({vis_x0,cy0},{vis_x1,cy1},
                to_u32(sel ? Col::fg : Col::line), 2.f);

            // Clip label / waveform
            if (track.type==TrackType::Subtitle && !clip.text.empty()) {
                ImGui::PushClipRect({vis_x0,cy0},{vis_x1,cy1},true);
                dl->AddText({vis_x0+4.f, cy0+(cy1-cy0-13.f)*0.5f},
                    to_u32(sel ? Col::bg : Col::fg), clip.text.c_str());
                ImGui::PopClipRect();
            } else if (track.type==TrackType::Audio) {
                int bars = (int)((vis_x1-vis_x0)/3.f);
                for (int b=0;b<bars;++b) {
                    float bx=vis_x0+b*3.f+1.f;
                    float amp=0.25f+0.55f*fabsf(sinf(b*0.37f+ti)*cosf(b*0.11f));
                    float mid=(cy0+cy1)*0.5f, ht=amp*(cy1-cy0-6.f)*0.5f;
                    dl->AddLine({bx,mid-ht},{bx,mid+ht},to_u32(Col::muted));
                }
            } else if (track.type==TrackType::Video) {
                ImGui::PushClipRect({vis_x0,cy0},{vis_x1,cy1},true);
                dl->AddText({vis_x0+4.f,cy0+3.f},
                    to_u32(sel?Col::bg:Col::fg),
                    clip.text.empty()?"Video":fs::path(clip.text).filename().string().c_str());
                ImGui::PopClipRect();
            }

            // Edge handles
            float ew=4.f;
            if (sel) {
                dl->AddRectFilled({vis_x0,cy0},{vis_x0+ew,cy1},to_u32(Col::muted),1.f);
                dl->AddRectFilled({vis_x1-ew,cy0},{vis_x1,cy1},to_u32(Col::muted),1.f);
            }

            // Transition out indicator — diagonal slash at right edge of video clips
            if (track.type == TrackType::Video && clip.transition_out > 0.f) {
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
                    state.selected_track = ti;
                    state.selected_clip  = ci;
                    strncpy(s_edit_buf, clip.text.c_str(), sizeof(s_edit_buf)-1);
                    s_edit_buf[sizeof(s_edit_buf)-1] = '\0';
                    s_edit_focus_next = (track.type==TrackType::Subtitle);
                    seek_to(state, clip.start);
                    state.panel_tab = 0;  // switch to Clip tab

                    float orig_cx0 = origin.x+TL_LABEL_W+clip.start*zoom-scroll;
                    float orig_cx1 = origin.x+TL_LABEL_W+clip.end*zoom-scroll;
                    if (mouse.x <= orig_cx0+ew+2.f) {
                        drag_track=ti; drag_clip=ci; drag_left=true; drag_right=false; drag_offset=0.f;
                    } else if (mouse.x >= orig_cx1-ew-2.f) {
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

    // Drag handling (frame-snapped, Ctrl bypasses)
    if (drag_track>=0 && drag_clip>=0 && ImGui::IsMouseDragging(0)) {
        Clip& dc = state.tracks[drag_track].clips[drag_clip];
        float new_t = (mouse.x - origin.x - TL_LABEL_W + scroll - drag_offset) / zoom;
        if (drag_left) {
            dc.start = fmaxf(0.f, fminf(snap(new_t), dc.end - f1));
        } else if (drag_right) {
            float et = (mouse.x - origin.x - TL_LABEL_W + scroll) / zoom;
            dc.end = fmaxf(dc.start + f1, snap(et));
        } else {
            float dur_clip = dc.end - dc.start;
            dc.start = fmaxf(0.f, snap(new_t));
            dc.end   = dc.start + dur_clip;
        }
    }
    if (ImGui::IsMouseReleased(0)) {
        if (drag_track >= 0 && drag_clip >= 0) {
            const char* act = drag_left  ? "Trim clip start" :
                              drag_right ? "Trim clip end"   : "Move clip";
            history_push(state, act);
        }
        drag_track=-1; drag_clip=-1; drag_left=false; drag_right=false;
    }

    // Playhead
    float ph_x = origin.x+TL_LABEL_W+state.playhead*zoom-scroll;
    if (ph_x >= origin.x+TL_LABEL_W && ph_x <= origin.x+total_w) {
        dl->AddLine({ph_x, origin.y}, {ph_x, origin.y+total_h}, to_u32(Col::fg));
        dl->AddTriangleFilled({ph_x-5.f,origin.y},{ph_x+5.f,origin.y},{ph_x,origin.y+10.f},to_u32(Col::fg));
    }

    // Click ruler to seek (frame-snapped, Ctrl bypasses)
    if ((ImGui::IsMouseClicked(0)||ImGui::IsMouseDragging(0)) && drag_track<0) {
        if (mouse.y>=origin.y && mouse.y<=origin.y+TL_RULER_H &&
            mouse.x>=origin.x+TL_LABEL_W && mouse.x<=origin.x+total_w) {
            float t = (mouse.x - origin.x - TL_LABEL_W + scroll) / zoom;
            seek_to(state, snap(fmaxf(0.f, fminf(t, dur))));
        }
    }

    // "+ Add Track" row
    ImVec2 add_p = {origin.x+8.f, track_y+6.f};
    ImVec2 mp = ImGui::GetIO().MousePos;
    bool add_hov = mp.y>=track_y && mp.y<track_y+TL_TRACK_H &&
                   mp.x>=origin.x && mp.x<=origin.x+total_w;
    dl->AddText(add_p, to_u32(add_hov ? Col::fg : Col::muted), "+ Add Subtitle Track");
    if (add_hov && ImGui::IsMouseClicked(0)) {
        Track t; t.type=TrackType::Subtitle;
        char name[32]; snprintf(name,sizeof(name),"Sub %d",(int)state.tracks.size()+1);
        t.name=name; state.tracks.push_back(t);
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
        if (ct && ct->type==TrackType::Video && cc) {
            bool ext_busy = state.extract_running;
            if (ext_busy) ImGui::BeginDisabled();
            if (ImGui::MenuItem(ext_busy ? "Extracting audio…" : "Extract audio as track")) {
                extract_audio_start(state, cc->text);
            }
            if (ext_busy) ImGui::EndDisabled();
            ImGui::Separator();
        }

        // ── ML Processing — Audio & Video clips ──────────────────────────────
        if (ct && (ct->type==TrackType::Audio || ct->type==TrackType::Video) && cc) {
            bool busy = transcribe_running();
            if (busy) ImGui::BeginDisabled();
            if (ImGui::BeginMenu("ML Processing")) {
                if (ImGui::MenuItem("Transcribe + Separate Vocals")) {
                    state.audio_path = cc->text;
                    kick_pipeline(state, cc->text, PipelineMode::Both);
                }
                if (ImGui::MenuItem("Transcribe only  (WhisperX)")) {
                    state.audio_path = cc->text;
                    kick_pipeline(state, cc->text, PipelineMode::TranscribeOnly);
                }
                if (ImGui::MenuItem("Separate Vocals only  (Demucs)")) {
                    kick_pipeline(state, cc->text, PipelineMode::SeparateOnly);
                }
                ImGui::EndMenu();
            }
            if (busy) ImGui::EndDisabled();
            ImGui::Separator();
        }

        // ── Subtitle clips ────────────────────────────────────────────────────
        if (ct && ct->type==TrackType::Subtitle) {
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
            if (ct->type != TrackType::Subtitle) {
                if (ImGui::MenuItem(ct->muted ? "Unmute" : "Mute")) {
                    ct->muted = !ct->muted;
                    history_push(state, "Track mute");
                }
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
        if (ImGui::MenuItem("Add Subtitle Track")) {
            Track t; t.type=TrackType::Subtitle;
            char n[32]; snprintf(n,sizeof(n),"Sub %d",(int)state.tracks.size()+1);
            t.name=n; state.tracks.push_back(t);
            history_push(state, "Add Subtitle Track");
        }
        if (ImGui::MenuItem("Add Audio Track")) {
            Track t; t.type=TrackType::Audio;
            char n[32]; snprintf(n,sizeof(n),"Audio %d",(int)state.tracks.size()+1);
            t.name=n; state.tracks.push_back(t);
            history_push(state, "Add Audio Track");
        }
        if (ImGui::MenuItem("Add Video Track")) {
            Track t; t.type=TrackType::Video;
            char n[32]; snprintf(n,sizeof(n),"Video %d",(int)state.tracks.size()+1);
            t.name=n; state.tracks.push_back(t);
            history_push(state, "Add Video Track");
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

    // Handle OS drop — just import, no auto-pipeline
    extern std::string g_dropped_file;
    if (!g_dropped_file.empty() && is_audio_file(g_dropped_file)) {
        import_file(state, g_dropped_file);
        g_dropped_file.clear();
    }

    // Proxy ready → upgrade from still to interactive proxy preview
    if (state.video_loaded && !state.proxy_ready && !state.video_path.empty()) {
        if (proxy_is_ready(state.video_path)) {
            ProxyInfo pi;
            if (proxy_load(state.video_path, pi)) {
                video_open_proxy(pi);
                state.proxy_ready = true;
                // Proxy frame count / fps is the most accurate duration source.
                float pd = (float)video_info().duration;
                if (pd > 0.f) {
                    state.duration = pd;
                    // Extend any full-span video clips to match corrected duration.
                    for (auto& t : state.tracks) {
                        if (t.type != TrackType::Video) continue;
                        for (auto& c : t.clips)
                            if (c.end < pd) c.end = pd;
                    }
                }
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
            at.type = TrackType::Audio;
            at.name = fs::path(state.extract_wav_path).stem().string();
            AudioMeta meta;
            float dur = audio_probe(state.extract_wav_path, meta) ? meta.duration_secs : state.duration;
            Clip ac; ac.start = 0.f; ac.end = dur; ac.text = state.extract_wav_path;
            at.clips.push_back(ac);
            state.tracks.push_back(at);
            history_push(state, "Extract audio from video");
        }
        state.extract_wav_path.clear();
    }

    // Pipeline done → apply grouping + save all SRTs + push history
    static PipelineStage last_stage = PipelineStage::Idle;
    if (last_stage != PipelineStage::Done &&
        state.pipeline.stage == PipelineStage::Done) {
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
            if (tr.type != TrackType::Audio && tr.type != TrackType::Video) continue;
            if (tr.muted) { vol = 0.f; break; }
            for (auto& cl : tr.clips) {
                if (state.playhead >= cl.start && state.playhead < cl.end) {
                    vol = cl.volume; break;
                }
            }
        }
        audio_set_volume(vol);
    }

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_H))
        state.panel_tab = 4;

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
            if (ImGui::MenuItem("Add Subtitle Track")) {
                Track t; t.type=TrackType::Subtitle;
                char n[32]; snprintf(n,sizeof(n),"Sub %d",(int)state.tracks.size()+1);
                t.name=n; state.tracks.push_back(t);
                history_push(state, "Add Subtitle Track");
            }
            if (ImGui::MenuItem("Add Audio Track")) {
                Track t; t.type=TrackType::Audio;
                char n[32]; snprintf(n,sizeof(n),"Audio %d",(int)state.tracks.size()+1);
                t.name=n; state.tracks.push_back(t);
                history_push(state, "Add Audio Track");
            }
            if (ImGui::MenuItem("Add Video Track")) {
                Track t; t.type=TrackType::Video;
                char n[32]; snprintf(n,sizeof(n),"Video %d",(int)state.tracks.size()+1);
                t.name=n; state.tracks.push_back(t);
                history_push(state, "Add Video Track");
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
            if (ImGui::MenuItem("History", "Ctrl+Shift+H")) state.panel_tab = 4;
            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
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
        if (ImGui::BeginTabBar("##panel_tabs")) {
            if (ImGui::BeginTabItem("Clip"))    { state.panel_tab=0; ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Style"))   { state.panel_tab=1; ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Track"))   { state.panel_tab=2; ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Export"))  { state.panel_tab=3; ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("History")) { state.panel_tab=4; ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();

        ImGui::BeginChild("##panel_scroll", {0.f, 0.f});
        ImGui::SetCursorPosX(8.f);
        float pw = props_w - 16.f;
        if      (state.panel_tab == 0) panel_clip(state, pw);
        else if (state.panel_tab == 1) panel_style(state, pw);
        else if (state.panel_tab == 2) panel_track(state, pw);
        else if (state.panel_tab == 3) panel_export(state, pw);
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
