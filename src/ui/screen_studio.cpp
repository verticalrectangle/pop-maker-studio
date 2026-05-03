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
#include <imgui.h>
#include <imgui_internal.h>
#include "json.hpp"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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

    // Load audio for playback
    audio_load(path);
    state.duration = audio_duration();

    if (is_video) {
        if (video_open(path)) {
            state.video_path   = path;
            state.video_loaded = true;
        }
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

    // Text
    std::string msg = state.pipeline.message.empty() ?
        "Processing…" : state.pipeline.message;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s  %d%%", msg.c_str(),
        (int)(state.pipeline.progress * 100.f));
    dl->AddText({p.x + 26.f, p.y + (h - 13.f) * 0.5f}, to_u32(Col::muted), buf);

    // Cancel button (draw as text, handle click)
    const char* cancel_lbl = "Cancel";
    float cx = p.x + w - ImGui::CalcTextSize(cancel_lbl).x - 16.f;
    ImVec2 mp = ImGui::GetIO().MousePos;
    bool hov = mp.x >= cx && mp.y >= p.y && mp.y < p.y + h;
    dl->AddText({cx, p.y + (h - 13.f) * 0.5f},
        to_u32(hov ? Col::fg : Col::muted), cancel_lbl);
    if (hov && ImGui::IsMouseClicked(0)) transcribe_cancel();

    ImGui::Dummy({w, h});

    // When pipeline finishes, group and load
    if (state.pipeline.stage == PipelineStage::Done &&
        !state.words_json_path.empty()) {
        apply_subtitle_mode(state);
        save_all_srts(state);
    }
}

// ── Preview ───────────────────────────────────────────────────────────────────

static void draw_preview(AppState& state, ImVec2 p, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (state.video_loaded && video_is_open()) {
        uintptr_t tex = video_get_texture((double)state.playhead);
        if (tex)
            dl->AddImage(ImTextureRef((ImTextureID)tex), p, {p.x+w, p.y+h});
        else
            dl->AddRectFilled(p, {p.x+w, p.y+h}, to_u32(Col::accent_dark), 2.f);
    } else {
        dl->AddRectFilled(p, {p.x+w, p.y+h}, to_u32(Col::accent_dark), 2.f);
    }
    dl->AddRect(p, {p.x+w, p.y+h}, to_u32(Col::line), 2.f);

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
        if (!show && state.selected_track == ti && state.selected_clip >= 0 &&
            state.selected_clip < (int)track.clips.size())
            show = &track.clips[state.selected_clip];
        if (!show) { ++rendered; continue; }

        float slot_h = 40.f;
        float slot_y = p.y + h - 24.f - (rendered + 1) * slot_h;

        ImGui::PushFont(g_font_black);
        ImGui::SetWindowFontScale(1.8f);
        ImVec2 tsz = ImGui::CalcTextSize(show->text.c_str());
        float  tx  = p.x + (w - tsz.x) * 0.5f;
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.8f,
            {tx+2.f, slot_y+2.f}, IM_COL32(0,0,0,180), show->text.c_str());
        ImU32 tcol = (active_ci >= 0) ? to_u32(Col::fg) : to_u32(Col::muted);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.8f,
            {tx, slot_y}, tcol, show->text.c_str());
        ImGui::SetWindowFontScale(1.f);
        ImGui::PopFont();
        ++rendered;
    }
}

// ── Right panel: Clip tab ─────────────────────────────────────────────────────

static char s_edit_buf[512] = {};
static bool s_edit_focus_next = false;

static void panel_clip(AppState& state, float w) {
    if (state.selected_track < 0 ||
        state.selected_track >= (int)state.tracks.size() ||
        state.selected_clip  < 0 ||
        state.selected_clip  >= (int)state.tracks[state.selected_track].clips.size()) {
        ImGui::Dummy({0.f, 24.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        const char* hint = "Click a clip to edit";
        ImGui::SetCursorPosX((w - ImGui::CalcTextSize(hint).x) * 0.5f);
        ImGui::TextUnformatted(hint);
        ImGui::PopStyleColor();
        return;
    }

    Track& track = state.tracks[state.selected_track];
    Clip&  clip  = track.clips[state.selected_clip];

    ImGui::Dummy({0.f, 8.f});
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    char tlabel[64]; snprintf(tlabel, sizeof(tlabel), "Track — %s", track.name.c_str());
    ImGui::TextUnformatted(tlabel);
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 4.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});

    if (track.type == TrackType::Subtitle) {
        ui_label("Text"); ImGui::Dummy({0.f, 4.f});
        if (s_edit_focus_next) { ImGui::SetKeyboardFocusHere(); s_edit_focus_next = false; }
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
        ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
        ImGui::SetNextItemWidth(w - 16.f);
        if (ImGui::InputText("##clip_text", s_edit_buf, sizeof(s_edit_buf),
                ImGuiInputTextFlags_EnterReturnsTrue))
            clip.text = s_edit_buf;
        if (ImGui::IsItemDeactivated()) clip.text = s_edit_buf;
        ImGui::PopStyleColor(2);

        ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
        ui_label("Timing"); ImGui::Dummy({0.f, 4.f});

        float half = (w - 24.f) * 0.5f;
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
        ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);

        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted("Start"); ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(half);
        float start = clip.start;
        if (ImGui::InputFloat("##start", &start, 0.05f, 0.1f, "%.3f"))
            if (start < clip.end - 0.05f) clip.start = start;
        ImGui::EndGroup();

        ImGui::SameLine(0.f, 8.f);

        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted("End"); ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(half);
        float end = clip.end;
        if (ImGui::InputFloat("##end", &end, 0.05f, 0.1f, "%.3f"))
            if (end > clip.start + 0.05f) clip.end = end;
        ImGui::EndGroup();

        ImGui::PopStyleColor(2);

        ImGui::Dummy({0.f, 4.f});
        char dur_buf[32];
        snprintf(dur_buf, sizeof(dur_buf), "Duration  %.3fs", clip.end - clip.start);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted(dur_buf); ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
        ui_label("Nudge"); ImGui::Dummy({0.f, 4.f});
        if (ui_btn("-100ms", false, true)) { clip.start-=0.1f; clip.end-=0.1f; if(clip.start<0){clip.end-=clip.start;clip.start=0;} }
        ImGui::SameLine(0.f, 4.f);
        if (ui_btn("-10ms",  false, true)) { clip.start-=0.01f; clip.end-=0.01f; }
        ImGui::SameLine(0.f, 4.f);
        if (ui_btn("+10ms",  false, true)) { clip.start+=0.01f; clip.end+=0.01f; }
        ImGui::SameLine(0.f, 4.f);
        if (ui_btn("+100ms", false, true)) { clip.start+=0.1f;  clip.end+=0.1f;  }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted(track.type == TrackType::Audio ? "Audio clip" : "Video clip");
        ImGui::TextWrapped("%s", clip.text.empty() ? "(no path)" : clip.text.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
    if (ui_btn("Delete clip", false, true)) {
        track.clips.erase(track.clips.begin() + state.selected_clip);
        state.selected_clip = -1;
    }

    // Subtitle grouping mode — shown for any subtitle track that has a source JSON
    if (track.type == TrackType::Subtitle && !state.words_json_path.empty() &&
        fs::exists(state.words_json_path)) {
        ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
        ui_label("Subtitle grouping"); ImGui::Dummy({0.f, 4.f});

        struct ModeBtn { SubtitleMode m; const char* label; const char* tip; };
        ModeBtn modes[] = {
            {SubtitleMode::Word,    "Word",    "One clip per word"},
            {SubtitleMode::Phrase,  "Phrase",  "Group by short pauses (>0.3s)"},
            {SubtitleMode::Line,    "Line",    "Group by breath gaps (>0.8s)"},
            {SubtitleMode::Segment, "Segment", "WhisperX sentence boundaries"},
            {SubtitleMode::CustomN, "Custom",  "N words per clip"},
        };

        SubtitleMode prev = state.subtitle_mode;
        for (auto& mb : modes) {
            bool sel = state.subtitle_mode == mb.m;
            if (ui_btn(mb.label, sel, true)) state.subtitle_mode = mb.m;
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(mb.tip);
                ImGui::EndTooltip();
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
            if (ImGui::InputInt("words/clip##n", &n)) {
                state.subtitle_n = (n < 1) ? 1 : (n > 20) ? 20 : n;
            }
            ImGui::PopStyleColor(2);
        }

        if (state.subtitle_mode != prev ||
            (state.subtitle_mode == SubtitleMode::CustomN)) {
            // Re-apply whenever mode changes; button click handles commit
        }

        ImGui::Dummy({0.f, 6.f});
        bool can_regroup = !state.words_json_path.empty() && fs::exists(state.words_json_path);
        if (!can_regroup) ImGui::BeginDisabled();
        if (ui_btn("Apply grouping", true, true)) {
            apply_subtitle_mode(state);
        }
        if (!can_regroup) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Re-build Lyrics track from saved word JSON");
            ImGui::EndTooltip();
        }
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
        if (ImGui::IsItemClicked()) state.style = sc.style;
        ImGui::PopStyleColor(2);
        if (i % 2 == 1 && i < 7) ImGui::Dummy({0.f, 4.f});
    }

    ImGui::Dummy({0.f, 12.f}); ui_separator(); ImGui::Dummy({0.f, 8.f});
    ui_label("Font weight"); ImGui::Dummy({0.f, 6.f});
    for (int fw : {400, 700, 900}) {
        char wl[8]; snprintf(wl, sizeof(wl), "%d", fw);
        if (ui_btn(wl, state.font_weight == fw, true)) state.font_weight = fw;
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
        {OutputFormat::Square,     "Square",         "1:1",  "1080×1080", 36.f, 36.f},
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
        if (ImGui::IsItemClicked()) state.format = fmts[i].fmt;
        ImGui::PopStyleColor(2);
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

    // Ruler
    float ruler_y = origin.y;
    dl->AddRectFilled({origin.x+TL_LABEL_W, ruler_y},
        {origin.x+total_w, ruler_y+TL_RULER_H}, to_u32(Col::bg_soft));
    dl->AddLine({origin.x, ruler_y+TL_RULER_H},
        {origin.x+total_w, ruler_y+TL_RULER_H}, to_u32(Col::line));

    float tick_secs = zoom>500?0.1f:zoom>200?0.5f:zoom>80?1.f:zoom>30?5.f:10.f;
    float first_t = floorf(scroll/zoom/tick_secs)*tick_secs;
    for (float t = first_t; t <= dur+tick_secs; t += tick_secs) {
        float px = origin.x + TL_LABEL_W + t*zoom - scroll;
        if (px < origin.x+TL_LABEL_W || px > origin.x+total_w) continue;
        bool big = (fmodf(t, tick_secs*5.f) < 0.001f);
        dl->AddLine({px, ruler_y+(big?4.f:10.f)}, {px, ruler_y+TL_RULER_H},
            to_u32(big?Col::muted:Col::dim));
        if (big || zoom > 100.f) {
            char tbuf[12]; snprintf(tbuf,sizeof(tbuf),"%s",fmt_time_short(t).c_str());
            dl->AddText({px+3.f, ruler_y+4.f}, to_u32(Col::muted), tbuf);
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

        // Track label
        bool track_sel = state.selected_track == ti;
        dl->AddText({origin.x+8.f, track_y+(TL_TRACK_H-13.f)*0.5f},
            to_u32(track_sel ? Col::fg : Col::muted), track.name.c_str());

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

            dl->AddRectFilled({vis_x0,cy0},{vis_x1,cy1},
                to_u32(sel ? Col::fg : Col::bg_soft_hov), 2.f);
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

            // Left click to select / drag
            if (ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemActive()) {
                if (mouse.y>=cy0 && mouse.y<=cy1 && mouse.x>=vis_x0 && mouse.x<=vis_x1) {
                    state.selected_track = ti;
                    state.selected_clip  = ci;
                    strncpy(s_edit_buf, clip.text.c_str(), sizeof(s_edit_buf)-1);
                    s_edit_buf[sizeof(s_edit_buf)-1] = '\0';
                    s_edit_focus_next = (track.type==TrackType::Subtitle);
                    state.playhead = clip.start;
                    audio_seek(clip.start);
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

    // Drag handling
    if (drag_track>=0 && drag_clip>=0 && ImGui::IsMouseDragging(0)) {
        Clip& dc = state.tracks[drag_track].clips[drag_clip];
        float new_t = (mouse.x - origin.x - TL_LABEL_W + scroll - drag_offset) / zoom;
        if (drag_left) {
            dc.start = fmaxf(0.f, fminf(new_t, dc.end-0.05f));
        } else if (drag_right) {
            float et = (mouse.x - origin.x - TL_LABEL_W + scroll) / zoom;
            dc.end = fmaxf(dc.start+0.05f, et);
        } else {
            float dur_clip = dc.end - dc.start;
            dc.start = fmaxf(0.f, new_t);
            dc.end   = dc.start + dur_clip;
        }
    }
    if (ImGui::IsMouseReleased(0)) {
        drag_track=-1; drag_clip=-1; drag_left=false; drag_right=false;
    }

    // Playhead
    float ph_x = origin.x+TL_LABEL_W+state.playhead*zoom-scroll;
    if (ph_x >= origin.x+TL_LABEL_W && ph_x <= origin.x+total_w) {
        dl->AddLine({ph_x, origin.y}, {ph_x, origin.y+total_h}, to_u32(Col::fg));
        dl->AddTriangleFilled({ph_x-5.f,origin.y},{ph_x+5.f,origin.y},{ph_x,origin.y+10.f},to_u32(Col::fg));
    }

    // Click ruler to seek
    if ((ImGui::IsMouseClicked(0)||ImGui::IsMouseDragging(0)) && drag_track<0) {
        if (mouse.y>=origin.y && mouse.y<=origin.y+TL_RULER_H &&
            mouse.x>=origin.x+TL_LABEL_W && mouse.x<=origin.x+total_w) {
            float t = (mouse.x - origin.x - TL_LABEL_W + scroll) / zoom;
            state.playhead = fmaxf(0.f, fminf(t, dur));
            audio_seek(state.playhead);
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
                }
            }
        }
        if (ImGui::MenuItem("Duplicate clip")) {
            if (valid) {
                Clip dup = *cc; dup.start = cc->end; dup.end = dup.start + (cc->end - cc->start);
                ct->clips.insert(ct->clips.begin()+ci+1, dup);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Seek to clip start")) {
            if (valid) { state.playhead=cc->start; audio_seek(cc->start); }
        }
        if (ImGui::MenuItem("Seek to clip end")) {
            if (valid) { state.playhead=cc->end; audio_seek(cc->end); }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete clip")) {
            if (valid) {
                ct->clips.erase(ct->clips.begin()+ci);
                state.selected_clip = -1;
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
                ct->name = rename_buf; ImGui::CloseCurrentPopup();
            }
            rename_open = ImGui::IsItemActive();
            ImGui::Separator();
        }

        if (valid && ti > 0) {
            if (ImGui::MenuItem("Move up")) {
                std::swap(state.tracks[ti], state.tracks[ti-1]);
                state.selected_track = ti-1;
            }
        }
        if (valid && ti < (int)state.tracks.size()-1) {
            if (ImGui::MenuItem("Move down")) {
                std::swap(state.tracks[ti], state.tracks[ti+1]);
                state.selected_track = ti+1;
            }
        }
        ImGui::Separator();
        if (ct) {
            if (ImGui::MenuItem(ct->visible ? "Hide track" : "Show track"))
                ct->visible = !ct->visible;
            if (ct->type != TrackType::Subtitle) {
                if (ImGui::MenuItem(ct->muted ? "Unmute" : "Mute"))
                    ct->muted = !ct->muted;
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete track", nullptr, false, valid)) {
            if (state.selected_track == ti) { state.selected_track=-1; state.selected_clip=-1; }
            state.tracks.erase(state.tracks.begin()+ti);
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##tl_ctx")) {
        open_tl_ctx = false;
        if (ImGui::MenuItem("Add Subtitle Track")) {
            Track t; t.type=TrackType::Subtitle;
            char n[32]; snprintf(n,sizeof(n),"Sub %d",(int)state.tracks.size()+1);
            t.name=n; state.tracks.push_back(t);
        }
        if (ImGui::MenuItem("Add Audio Track")) {
            Track t; t.type=TrackType::Audio;
            char n[32]; snprintf(n,sizeof(n),"Audio %d",(int)state.tracks.size()+1);
            t.name=n; state.tracks.push_back(t);
        }
        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(2);
    (void)open_clip_ctx; (void)open_track_ctx; (void)open_tl_ctx;
}

// ── Split / delete keyboard shortcuts ────────────────────────────────────────

static void handle_shortcuts(AppState& state) {
    if (ImGui::IsAnyItemActive()) return;
    if (state.selected_track<0 || state.selected_clip<0) return;
    if (state.selected_track>=(int)state.tracks.size()) return;
    Track& track = state.tracks[state.selected_track];
    if (state.selected_clip>=(int)track.clips.size()) return;
    Clip& clip = track.clips[state.selected_clip];

    if (ImGui::IsKeyPressed(ImGuiKey_S) ||
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl|ImGuiKey_B)) {
        float cut = state.playhead;
        if (cut > clip.start+0.02f && cut < clip.end-0.02f) {
            Clip right = clip; clip.end = cut; right.start = cut;
            track.clips.insert(track.clips.begin()+state.selected_clip+1, right);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        track.clips.erase(track.clips.begin()+state.selected_clip);
        state.selected_clip = -1;
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

    // Pipeline done → apply grouping + save all SRTs
    static PipelineStage last_stage = PipelineStage::Idle;
    if (last_stage != PipelineStage::Done &&
        state.pipeline.stage == PipelineStage::Done) {
        apply_subtitle_mode(state);
        save_all_srts(state);
    }
    last_stage = state.pipeline.stage;

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
                state = AppState{};
                state.splash_timer = 0.f;  // don't re-show splash
                audio_shutdown(); audio_init();
                video_close();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Track")) {
            if (ImGui::MenuItem("Add Subtitle Track")) {
                Track t; t.type=TrackType::Subtitle;
                char n[32]; snprintf(n,sizeof(n),"Sub %d",(int)state.tracks.size()+1);
                t.name=n; state.tracks.push_back(t);
            }
            if (ImGui::MenuItem("Add Audio Track")) {
                Track t; t.type=TrackType::Audio;
                char n[32]; snprintf(n,sizeof(n),"Audio %d",(int)state.tracks.size()+1);
                t.name=n; state.tracks.push_back(t);
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
                }
            }
            if (ImGui::MenuItem("Duplicate clip") && has_clip) {
                Track& t = state.tracks[state.selected_track];
                Clip dup = t.clips[state.selected_clip];
                float len = dup.end - dup.start;
                dup.start = dup.end; dup.end = dup.start+len;
                t.clips.insert(t.clips.begin()+state.selected_clip+1, dup);
            }
            if (ImGui::MenuItem("Delete clip", "Del") && has_clip) {
                state.tracks[state.selected_track].clips.erase(
                    state.tracks[state.selected_track].clips.begin()+state.selected_clip);
                state.selected_clip=-1;
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
            ImGui::EndMenu();
        }

        // Right side: timecode + play
        float right_x = win_w - 280.f;
        ImGui::SameLine(right_x);
        char tcbuf[32];
        snprintf(tcbuf, sizeof(tcbuf), "%s / %s",
            fmt_time(state.playhead).c_str(),
            fmt_time(fmaxf(state.duration, 0.01f)).c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted(tcbuf);
        ImGui::PopStyleColor();

        ImGui::SameLine(0.f, 12.f);
        if (ui_btn(state.playing ? "||" : ">", false, true)) {
            state.playing = !state.playing;
            if (state.playing) audio_play(); else audio_pause();
        }

        ImGui::EndMenuBar();
    }

    // ── Body layout ──────────────────────────────────────────────────────────
    float menubar_h   = ImGui::GetFrameHeight() + 2.f;  // already consumed by BeginMenuBar
    float body_top    = ImGui::GetCursorPosY();
    float tl_h        = fminf(200.f, TL_RULER_H + ((int)state.tracks.size()+2) * TL_TRACK_H);
    float pipeline_h  = (state.pipeline.stage != PipelineStage::Idle &&
                         state.pipeline.stage != PipelineStage::Done &&
                         state.pipeline.stage != PipelineStage::Error) ? 28.f : 0.f;
    float body_h      = win_h - menubar_h - body_top - tl_h - pipeline_h - 2.f;

    float props_w = fmaxf(260.f, win_w * 0.27f);
    float preview_w = win_w - props_w - 1.f;

    // ── Preview ───────────────────────────────────────────────────────────────
    ImGui::SetCursorPos({0.f, body_top});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col::bg);
    ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
    if (ImGui::BeginChild("##preview_zone", {preview_w, body_h}, ImGuiChildFlags_Borders)) {
        float aw = ImGui::GetContentRegionAvail().x;
        float ah = ImGui::GetContentRegionAvail().y - 36.f; // leave room for scrub

        float sw, sh;
        if (aw / ah > 16.f/9.f) { sh = ah; sw = sh*16.f/9.f; }
        else                     { sw = aw; sh = sw*9.f/16.f; }
        float ox = (aw - sw) * 0.5f;
        float oy = (ah - sh) * 0.5f;
        ImGui::SetCursorPos({ox, oy});
        ImVec2 stage_p = ImGui::GetCursorScreenPos();
        ImGui::Dummy({sw, sh});
        draw_preview(state, stage_p, sw, sh);

        // Scrub bar
        ImGui::SetCursorPos({ox, oy + sh + 8.f});
        float dur = fmaxf(state.duration, 0.01f);
        float fill = state.playhead / dur;
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, Col::fg);
        ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0.f, 0.f});
        ImGui::SetNextItemWidth(sw);
        if (ImGui::SliderFloat("##scrub", &fill, 0.f, 1.f, "")) {
            state.playhead = fill * dur;
            audio_seek(state.playhead);
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
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
            if (ImGui::BeginTabItem("Clip"))   { state.panel_tab=0; ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Style"))  { state.panel_tab=1; ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Export")) { state.panel_tab=2; ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();

        // Force tab selection when panel_tab was set by click-on-clip
        // (ImGui tab bar manages this via its own state; panel_tab is advisory)

        ImGui::BeginChild("##panel_scroll", {0.f, 0.f});
        ImGui::SetCursorPosX(8.f);
        float pw = props_w - 16.f;
        if      (state.panel_tab == 0) panel_clip(state, pw);
        else if (state.panel_tab == 1) panel_style(state, pw);
        else                           panel_export(state, pw);
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);

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
