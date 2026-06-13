#include "studio_types.h"
#include "studio_shared.h"
#include "panel_media.h"
#include "timeline.h"
#include "app.h"
#include "audio.h"
#include "video.h"
#include "proxy.h"
#include "history.h"
#include "filepicker.h"
#include "theme.h"
#include "render.h"
#include "transcribe.h"
#include "noise_reduce.h"
#include "bg_remove.h"
#include <imgui.h>
#include <imgui_internal.h>
#include "json.hpp"
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;
extern ImFont* g_font_bold;
extern ImFont* g_font_black;

// ── Right panel: History tab ──────────────────────────────────────────────────

void panel_history(AppState& state, float /*w*/) {
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
// ── Right panel: Project tab ──────────────────────────────────────────────────

void panel_project(AppState& state, float w) {
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
// ── Media browser helpers ─────────────────────────────────────────────────────

static std::string media_recents_path() {
    const char* h = getenv("HOME");
    return h ? std::string(h) + "/.config/pop-maker-studio/recent_media.json"
             : "/tmp/pop_maker_recent_media.json";
}

// RecentMedia struct defined in panel_media.h

RecentMedia& recent_media() {
    static RecentMedia s;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        try {
            std::ifstream f(media_recents_path());
            if (f) {
                auto j = nlohmann::json::parse(f, nullptr, false);
                if (!j.is_discarded()) {
                    if (j.contains("videos")) s.videos = j["videos"].get<std::vector<std::string>>();
                    if (j.contains("images")) s.images = j["images"].get<std::vector<std::string>>();
                    if (j.contains("audio"))  s.audio  = j["audio"].get<std::vector<std::string>>();
                }
            }
        } catch (...) {}
    }
    return s;
}

// MediaKind enum in panel_media.h { Video, Image, Audio };

void recent_media_push(const std::string& path, MediaKind kind) {
    auto& r   = recent_media();
    auto& vec = kind == MediaKind::Video ? r.videos
              : kind == MediaKind::Audio ? r.audio
                                        : r.images;
    vec.erase(std::remove(vec.begin(), vec.end(), path), vec.end());
    vec.insert(vec.begin(), path);
    if (vec.size() > 24) vec.resize(24);
    try {
        nlohmann::json j;
        j["videos"] = r.videos;
        j["images"] = r.images;
        j["audio"]  = r.audio;
        fs::create_directories(fs::path(media_recents_path()).parent_path());
        std::ofstream(media_recents_path()) << j.dump(2);
    } catch (...) {}
}

bool is_image_path(const std::string& p) {
    fs::path fp(p);
    std::string ext = fp.extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    return ext==".jpg"||ext==".jpeg"||ext==".png"||ext==".bmp"||ext==".webp"||ext==".tiff"
        || ext==".heic"||ext==".heif"||ext==".gif";
}

// ── Bin helpers ──────────────────────────────────────────────────────────────

MediaKind kind_for_path(const std::string& path) {
    if (is_image_path(path)) return MediaKind::Image;
    fs::path fp(path);
    std::string ext = fp.extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    // Anything with a video container extension counts as Video; everything
    // else audio-shaped counts as Audio. Matches what add_clip_to_track does
    // when classifying drops, so the bin's kind tag agrees with how the clip
    // would actually be placed.
    if (ext==".mp4"||ext==".mov"||ext==".mkv"||ext==".avi"||ext==".webm") return MediaKind::Video;
    return MediaKind::Audio;
}

bool bin_contains(const AppState& state, const std::string& path) {
    for (auto& p : state.bin) if (p == path) return true;
    return false;
}

void bin_add(AppState& state, const std::string& path) {
    if (path.empty() || bin_contains(state, path)) return;
    state.bin.push_back(path);
    // Mirror into the cross-project Recent list so it shows up next time too.
    recent_media_push(path, kind_for_path(path));
}

void bin_remove(AppState& state, const std::string& path) {
    state.bin.erase(std::remove(state.bin.begin(), state.bin.end(), path),
                    state.bin.end());
}

int bin_used_count(const AppState& state, const std::string& path) {
    int n = 0;
    for (auto& tr : state.tracks)
        for (auto& cl : tr.clips)
            if ((cl.clip_type == ClipType::Video || cl.clip_type == ClipType::Audio)
                && cl.text == path)
                ++n;
    return n;
}

void bin_backfill_from_timeline(AppState& state) {
    if (!state.bin.empty()) return;
    for (auto& tr : state.tracks) {
        for (auto& cl : tr.clips) {
            if ((cl.clip_type == ClipType::Video || cl.clip_type == ClipType::Audio)
                && !cl.text.empty())
                bin_add(state, cl.text);
        }
    }
}


// ── Timeline ──────────────────────────────────────────────────────────────────
void panel_media_browser(AppState& state, float w, bool is_video) {
    ImGui::Dummy({0.f, 8.f});

    // Header
    ImGui::PushStyleColor(ImGuiCol_Text, is_video ? IM_COL32(255,180,60,255)
                                                   : IM_COL32(60,220,190,255));
    ImGui::TextUnformatted(is_video ? "Videos" : "Images");
    ImGui::PopStyleColor();

    // Browse button right-aligned
    {
        float bw = ImGui::CalcTextSize("Browse…").x + 20.f;
        ImGui::SameLine(w - bw - 4.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.f, 3.f});
        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(30,30,46,255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(50,50,72,255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(60,60,90,255));
        if (ImGui::SmallButton("Browse…")) {
            std::string picked = is_video
                ? filepicker_open("Open video", "Video", "*.mp4 *.mov *.mkv *.avi *.webm *.gif")
                : filepicker_open("Open image", "Image", "*.jpg *.jpeg *.png *.bmp *.webp *.heic *.heif *.gif");
            if (!picked.empty()) {
                recent_media_push(picked, is_video ? MediaKind::Video : MediaKind::Image);
                // Clip at playhead — reuse an empty track if one exists,
                // otherwise insert a new track at the top.
                float dur = is_video ? video_probe_duration(picked) : 0.f;
                if (dur <= 0.f) dur = is_video ? 4.f : 5.f;
                Clip cl; cl.clip_type = ClipType::Video; cl.text = picked;
                cl.source_id = picked; cl.start = state.playhead; cl.end = cl.start + dur;
                s_source_durations[picked] = dur;
                int target = find_empty_track(state);
                if (target < 0) {
                    Track nt; nt.name = fs::path(picked).stem().string();
                    state.tracks.insert(state.tracks.begin(), std::move(nt));
                    target = 0;
                }
                state.tracks[target].clips.push_back(cl);
                state.selected_track = target;
                state.selected_clip  = (int)state.tracks[target].clips.size() - 1;
                proxy_start(picked);
                int slot = slot_for_video(state, clip_slot_key(picked, cl.start), picked);
                if (slot >= 0) video_open_still(slot, proxy_still_path(picked));
                state.video_loaded = true;
                s_panel_view = PanelView::Clip;
                history_push(state, (is_video ? "Import video: " : "Import image: ") +
                             fs::path(picked).filename().string());
            }
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
    }

    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
    ImGui::TextWrapped("Drag onto a track, or tap + to add at the playhead.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 8.f});

    auto& recents = is_video ? recent_media().videos : recent_media().images;

    // Filter out missing files
    std::vector<std::string> valid;
    valid.reserve(recents.size());
    for (auto& p : recents) if (fs::exists(p)) valid.push_back(p);

    if (valid.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
        ImGui::TextWrapped("No recent files. Click Browse… to add one.");
        ImGui::PopStyleColor();
        return;
    }

    // 2-column thumbnail grid
    const float GAP    = 6.f;
    const float COL_W  = floorf((w - GAP * 3.f) * 0.5f);
    const float THUMB_H = floorf(COL_W * 9.f / 16.f);
    const float CARD_H  = THUMB_H + 30.f;
    float base_y = ImGui::GetCursorPosY();

    for (int i = 0; i < (int)valid.size(); ++i) {
        const std::string& path = valid[i];
        int  col  = i % 2;
        int  row  = i / 2;
        float cx_ = GAP + col * (COL_W + GAP);
        float cy_ = base_y + row * (CARD_H + GAP);

        ImGui::SetCursorPos({cx_, cy_});
        ImVec2 cp = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        ImGui::PushID(i);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##mc", {COL_W, CARD_H});
        bool hov = ImGui::IsItemHovered();

        // Card background
        ImU32 bg = hov ? IM_COL32(34,34,52,255) : IM_COL32(18,18,28,255);
        dl->AddRectFilled(cp, {cp.x+COL_W, cp.y+CARD_H}, bg, 6.f);

        // Thumbnail — letterboxed to preserve aspect ratio.
        // HEIC/HEIF aren't supported by stb_image; route through the proxy
        // JPEG still (generated by proxy_ensure_still via ffmpeg/libheif).
        int tw = 0, th = 0;
        std::string ext = fs::path(path).extension().string();
        for (auto& c : ext) c = (char)tolower((unsigned char)c);
        bool needs_proxy_thumb = (ext == ".heic" || ext == ".heif");
        uintptr_t tex = (is_image_path(path) && !needs_proxy_thumb)
            ? video_load_thumb(path, &tw, &th)
            : video_load_thumb(proxy_still_path(path), &tw, &th);

        if (tex) {
            float aspect = (tw > 0 && th > 0) ? (float)tw / (float)th : 16.f/9.f;
            float disp_w = COL_W, disp_h = COL_W / aspect;
            if (disp_h > THUMB_H) { disp_h = THUMB_H; disp_w = THUMB_H * aspect; }
            float ox = (COL_W - disp_w) * 0.5f;
            float oy = (THUMB_H - disp_h) * 0.5f;
            ImVec2 img0 = {cp.x + ox, cp.y + oy};
            ImVec2 img1 = {img0.x + disp_w, img0.y + disp_h};
            dl->PushClipRect(cp, {cp.x+COL_W, cp.y+THUMB_H}, true);
            dl->AddImageRounded((ImTextureID)(uintptr_t)tex,
                                img0, img1,
                                {0,0},{1,1}, IM_COL32(255,255,255,220), 6.f,
                                ImDrawFlags_RoundCornersTop);
            dl->PopClipRect();
        } else {
            // Placeholder — gentle gradient while proxy generates
            ImU32 ph0 = is_video ? IM_COL32(35,22,8,255)  : IM_COL32(8,28,28,255);
            ImU32 ph1 = is_video ? IM_COL32(55,38,16,255) : IM_COL32(14,46,44,255);
            dl->AddRectFilledMultiColor(cp, {cp.x+COL_W, cp.y+THUMB_H}, ph0, ph0, ph1, ph1);
            const char* lbl = is_video ? "loading…" : "·";
            ImVec2 tsz = ImGui::CalcTextSize(lbl);
            dl->AddText({cp.x+(COL_W-tsz.x)*0.5f, cp.y+(THUMB_H-tsz.y)*0.5f},
                        IM_COL32(90,80,70,200), lbl);
        }

        // Filename
        fs::path fp(path);
        std::string name = fp.filename().string();
        if ((int)name.size() > 20) name = name.substr(0,17) + "…";
        ImVec2 tsz = ImGui::CalcTextSize(name.c_str());
        dl->AddText({cp.x+(COL_W-tsz.x)*0.5f, cp.y+THUMB_H+8.f},
                    hov ? IM_COL32(255,255,255,230) : IM_COL32(190,190,205,200),
                    name.c_str());

        // Border
        dl->AddRect(cp, {cp.x+COL_W, cp.y+CARD_H},
                    hov ? IM_COL32(255,255,255,160) : IM_COL32(48,48,68,180),
                    6.f, 0, hov ? 1.5f : 1.f);

        // Large hover preview of the thumbnail (after a short dwell).
        ui_card_hover_secs(40000 + i, hov);
        if (tex && ui_card_hover_ready(40000 + i, 0.30f)) {
            std::string full = fp.filename().string();
            char sub[64];
            if (tw > 0 && th > 0)
                snprintf(sub, sizeof(sub), "%d x %d  -  %s", tw, th, is_video ? "video" : "image");
            else
                snprintf(sub, sizeof(sub), "%s", is_video ? "video" : "image");
            ui_card_image_popover(cp, (ImTextureID)(uintptr_t)tex,
                                  (float)tw, (float)th, false, full.c_str(), sub);
        }

        // Drag-drop source (whole card)
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            const char* ptype = is_video ? "MEDIA_VID" : "MEDIA_IMG";
            ImGui::SetDragDropPayload(ptype, path.c_str(), path.size()+1);
            if (tex) ImGui::Image((ImTextureID)(uintptr_t)tex, {96.f, 54.f});
            ImGui::TextUnformatted(name.c_str());
            ImGui::TextDisabled("Drop onto timeline track");
            ImGui::EndDragDropSource();
        }

        // "+" → add clip at playhead on an empty track (new track if none)
        if (ui_card_add_btn(cp, COL_W, i)) {
            float dur = is_video ? video_probe_duration(path) : 0.f;
            if (dur <= 0.f) dur = is_video ? 4.f : 5.f;
            Clip cl; cl.clip_type = ClipType::Video; cl.text = path;
            cl.source_id = path; cl.start = state.playhead; cl.end = cl.start + dur;
            s_source_durations[path] = dur;
            int target = find_empty_track(state);
            if (target < 0) {
                Track nt; nt.name = fp.stem().string();
                state.tracks.insert(state.tracks.begin(), std::move(nt));
                target = 0;
            }
            state.tracks[target].clips.push_back(cl);
            state.selected_track = target;
            state.selected_clip  = (int)state.tracks[target].clips.size() - 1;
            proxy_start(path);
            int slot = slot_for_video(state, clip_slot_key(path, cl.start), path);
            if (slot >= 0) video_open_still(slot, proxy_still_path(path));
            state.video_loaded = true;
            recent_media_push(path, is_video ? MediaKind::Video : MediaKind::Image);
            s_panel_view = PanelView::Clip;
            history_push(state, (is_video ? "Add video: " : "Add image: ") + fp.filename().string());
        }

        ImGui::PopID();
    }

    // Advance cursor past all cards — Dummy() required so ImGui registers the height extension
    int rows = ((int)valid.size() + 1) / 2;
    ImGui::SetCursorPosY(base_y + rows * (CARD_H + GAP) + 4.f);
    ImGui::Dummy({0.f, 0.f});
}

// ── Right panel: Bin tab ──────────────────────────────────────────────────────
//
// Project-scoped media library. The "Library" tabs above (Videos / Images /
// Audio) show cross-project recents — files used across sessions. The Bin
// shows files in *this project*: things the user has added, including from
// multi-file drops. Click to place at playhead, drag to drop on a specific
// track, hover × to remove from the project.
void panel_bin(AppState& state, float w) {
    ImGui::Dummy({0.f, 8.f});

    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(220, 200, 120, 255));
    ImGui::TextUnformatted("Bin");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
    char count_buf[32]; snprintf(count_buf, sizeof(count_buf), "  %d", (int)state.bin.size());
    ImGui::TextUnformatted(count_buf);
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
    ImGui::TextWrapped("Project media. Click to place, drag to a track.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 8.f});

    if (state.bin.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
        ImGui::TextWrapped("Empty. Drop files onto the canvas to add them here.");
        ImGui::PopStyleColor();
        return;
    }

    const float ROW_H  = 52.f;
    const float THUMB_W = 64.f;
    const float GAP    = 4.f;
    const float CARD_W = w - 8.f;

    std::string pending_remove;

    for (int i = 0; i < (int)state.bin.size(); ++i) {
        const std::string& path = state.bin[i];
        fs::path fp(path);
        bool exists = fs::exists(path);
        MediaKind kind = kind_for_path(path);
        int used = bin_used_count(state, path);

        ImGui::PushID(i);
        ImGui::SetCursorPosX(4.f);
        ImVec2 cp = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        ImGui::InvisibleButton("##bin_row", {CARD_W, ROW_H});
        bool hov = ImGui::IsItemHovered();
        bool clk = ImGui::IsItemClicked();

        ImU32 bg = hov ? IM_COL32(28, 28, 38, 255) : IM_COL32(16, 16, 22, 255);
        dl->AddRectFilled(cp, {cp.x + CARD_W, cp.y + ROW_H}, bg, 5.f);

        // Thumbnail — reuses proxy still / direct image load
        int tw = 0, th = 0;
        uintptr_t tex = 0;
        if (kind == MediaKind::Image) {
            std::string ext = fp.extension().string();
            for (auto& c : ext) c = (char)tolower((unsigned char)c);
            bool needs_proxy_thumb = (ext == ".heic" || ext == ".heif");
            tex = needs_proxy_thumb
                ? video_load_thumb(proxy_still_path(path), &tw, &th)
                : video_load_thumb(path, &tw, &th);
        } else if (kind == MediaKind::Video) {
            tex = video_load_thumb(proxy_still_path(path), &tw, &th);
        }

        float thumb_h = ROW_H - 8.f;
        ImVec2 t0 = {cp.x + 4.f, cp.y + 4.f};
        ImVec2 t1 = {t0.x + THUMB_W, t0.y + thumb_h};
        if (tex && kind != MediaKind::Audio) {
            float aspect = (tw > 0 && th > 0) ? (float)tw / (float)th : 16.f / 9.f;
            float dw = THUMB_W, dh = THUMB_W / aspect;
            if (dh > thumb_h) { dh = thumb_h; dw = thumb_h * aspect; }
            float ox = (THUMB_W - dw) * 0.5f;
            float oy = (thumb_h - dh) * 0.5f;
            dl->PushClipRect(t0, t1, true);
            dl->AddImageRounded((ImTextureID)(uintptr_t)tex,
                                {t0.x + ox, t0.y + oy},
                                {t0.x + ox + dw, t0.y + oy + dh},
                                {0, 0}, {1, 1},
                                IM_COL32(255, 255, 255, exists ? 220 : 100), 4.f);
            dl->PopClipRect();
        } else {
            // Audio or no-thumb-yet: plain block with kind glyph
            ImU32 fill = (kind == MediaKind::Audio) ? IM_COL32(20, 38, 26, 255)
                                                    : IM_COL32(22, 22, 30, 255);
            dl->AddRectFilled(t0, t1, fill, 4.f);
            const char* glyph = (kind == MediaKind::Audio) ? "AUD"
                              : (kind == MediaKind::Image) ? "IMG" : "VID";
            ImVec2 gsz = ImGui::CalcTextSize(glyph);
            ImU32 gcol = (kind == MediaKind::Audio) ? IM_COL32(80, 200, 130, 200)
                                                    : IM_COL32(140, 140, 170, 200);
            dl->AddText({t0.x + (THUMB_W - gsz.x) * 0.5f, t0.y + (thumb_h - gsz.y) * 0.5f},
                        gcol, glyph);
        }

        // Filename
        std::string name = fp.filename().string();
        if ((int)name.size() > 28) name = name.substr(0, 25) + "…";
        ImVec2 nx = {t1.x + 10.f, cp.y + 8.f};
        ImGui::PushFont(g_font_bold);
        dl->AddText(ImGui::GetFont(), 12.f, nx,
                    exists ? (hov ? IM_COL32(255, 255, 255, 235)
                                  : IM_COL32(200, 205, 220, 210))
                           : IM_COL32(140, 100, 100, 200),
                    name.c_str());
        ImGui::PopFont();

        // Sub-line: duration + "used N×"
        auto dit = s_source_durations.find(path);
        float dur = (dit != s_source_durations.end()) ? dit->second : 0.f;
        char sub[64];
        if (dur > 0.f && used > 0)
            snprintf(sub, sizeof(sub), "%s   used %d\xc3\x97",
                     fmt_time_short(dur).c_str(), used);
        else if (dur > 0.f)
            snprintf(sub, sizeof(sub), "%s", fmt_time_short(dur).c_str());
        else if (used > 0)
            snprintf(sub, sizeof(sub), "used %d\xc3\x97", used);
        else
            snprintf(sub, sizeof(sub), "%s", exists ? "" : "missing");
        dl->AddText({nx.x, cp.y + 28.f},
                    exists ? IM_COL32(140, 145, 160, 200)
                           : IM_COL32(200, 100, 100, 220),
                    sub);

        // Border
        dl->AddRect(cp, {cp.x + CARD_W, cp.y + ROW_H},
                    hov ? IM_COL32(255, 255, 255, 140) : IM_COL32(40, 40, 56, 180),
                    5.f, 0, hov ? 1.4f : 1.f);

        // Hover × — top-right small button. Use InvisibleButton over the X
        // area so the row's click handler doesn't also fire on the X.
        if (hov) {
            float xsz = 16.f;
            ImVec2 xp = {cp.x + CARD_W - xsz - 4.f, cp.y + 4.f};
            ImGui::SetCursorScreenPos(xp);
            ImGui::InvisibleButton("##bin_x", {xsz, xsz});
            bool x_hov = ImGui::IsItemHovered();
            bool x_clk = ImGui::IsItemClicked();
            dl->AddRectFilled(xp, {xp.x + xsz, xp.y + xsz},
                              x_hov ? IM_COL32(180, 60, 60, 220)
                                    : IM_COL32(60, 60, 80, 180), 4.f);
            // Draw an X
            float pad = 4.f;
            ImU32 xcol = IM_COL32(240, 240, 240, 230);
            dl->AddLine({xp.x + pad, xp.y + pad}, {xp.x + xsz - pad, xp.y + xsz - pad}, xcol, 1.4f);
            dl->AddLine({xp.x + xsz - pad, xp.y + pad}, {xp.x + pad, xp.y + xsz - pad}, xcol, 1.4f);
            if (x_clk) {
                pending_remove = path;
                clk = false;  // suppress row click in the same frame
            }
        }

        // Row click → place at playhead. Lands on the hovered track if the
        // user is hovering one, otherwise on a fresh track at the top.
        if (clk && exists) {
            ClipType ct = (kind == MediaKind::Audio) ? ClipType::Audio : ClipType::Video;
            int target;
            if (s_tl_hover_track >= 0 && s_tl_hover_track < (int)state.tracks.size()) {
                target = s_tl_hover_track;
            } else if ((target = find_empty_track(state)) < 0) {
                Track nt; nt.name = fp.stem().string();
                state.tracks.insert(state.tracks.begin(), std::move(nt));
                target = 0;
            }
            add_clip_to_track(state, target, path, ct);
            s_panel_view = PanelView::Clip;
        }

        // Drag-drop source — payload kind matches existing MEDIA_VID/IMG/AUD
        // so timeline drop sites accept it without changes.
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            const char* ptype = (kind == MediaKind::Audio) ? "MEDIA_AUD"
                              : (kind == MediaKind::Image) ? "MEDIA_IMG" : "MEDIA_VID";
            ImGui::SetDragDropPayload(ptype, path.c_str(), path.size() + 1);
            if (tex && kind != MediaKind::Audio)
                ImGui::Image((ImTextureID)(uintptr_t)tex, {96.f, 54.f});
            ImGui::TextUnformatted(name.c_str());
            ImGui::TextDisabled("Drop onto timeline track");
            ImGui::EndDragDropSource();
        }

        ImGui::Dummy({0.f, GAP});
        ImGui::PopID();
    }

    if (!pending_remove.empty()) {
        bin_remove(state, pending_remove);
        history_push(state, "Remove from bin: " +
                     fs::path(pending_remove).filename().string());
    }
}

void panel_audio_browser(AppState& state, float w) {
    ImGui::Dummy({0.f, 8.f});

    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(80, 200, 120, 255));
    ImGui::TextUnformatted("Audio");
    ImGui::PopStyleColor();

    {
        float bw = ImGui::CalcTextSize("Browse…").x + 20.f;
        ImGui::SameLine(w - bw - 4.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.f, 3.f});
        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(30,30,46,255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(50,50,72,255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(60,60,90,255));
        if (ImGui::SmallButton("Browse…")) {
            std::string picked = filepicker_open("Open audio", "Audio",
                                                 "*.wav *.mp3 *.m4a *.flac *.aac *.ogg");
            if (!picked.empty()) {
                recent_media_push(picked, MediaKind::Audio);
                AudioMeta meta{};
                float dur = audio_probe(picked, meta) ? meta.duration_secs : 4.f;
                if (dur <= 0.f) dur = 4.f;
                Clip cl; cl.clip_type = ClipType::Audio; cl.text = picked;
                cl.source_id = picked; cl.start = state.playhead; cl.end = cl.start + dur;
                s_source_durations[picked] = dur;
                int target = find_empty_track(state);
                if (target < 0) {
                    Track nt; nt.name = fs::path(picked).stem().string();
                    state.tracks.insert(state.tracks.begin(), std::move(nt));
                    target = 0;
                }
                state.tracks[target].clips.push_back(cl);
                state.selected_track = target;
                state.selected_clip  = (int)state.tracks[target].clips.size() - 1;
                audio_source_ensure(picked);
                s_panel_view = PanelView::Clip;
                history_push(state, "Import audio: " + fs::path(picked).filename().string());
            }
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
    }

    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
    ImGui::TextWrapped("Drag onto a track, or tap + to add at the playhead.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 8.f});

    auto& recents = recent_media().audio;
    std::vector<std::string> valid;
    valid.reserve(recents.size());
    for (auto& p : recents) if (fs::exists(p)) valid.push_back(p);

    if (valid.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
        ImGui::TextWrapped("No recent audio. Click Browse… to add one.");
        ImGui::PopStyleColor();
        return;
    }

    const float CARD_H  = 62.f;
    const float CARD_W  = w - 8.f;
    const float BAR_W   = 36.f;  // waveform bars area
    const float GAP     = 5.f;

    for (int i = 0; i < (int)valid.size(); ++i) {
        const std::string& path = valid[i];
        fs::path fp(path);

        ImGui::PushID(i);
        ImVec2 cp = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        ImGui::SetCursorPosX(4.f);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##ac", {CARD_W, CARD_H});
        bool hov = ImGui::IsItemHovered();

        ImU32 bg = hov ? IM_COL32(30,38,32,255) : IM_COL32(16,22,18,255);
        dl->AddRectFilled(cp, {cp.x+CARD_W, cp.y+CARD_H}, bg, 6.f);

        // Fake waveform bars on the left
        {
            float bx = cp.x + 10.f;
            float by = cp.y + CARD_H * 0.5f;
            const int BARS = 9;
            const float BSPC = 3.8f;
            ImU32 bar_col = hov ? IM_COL32(80,220,130,200) : IM_COL32(50,160,90,160);
            // deterministic heights from filename hash
            uint32_t h = 0;
            for (char c : fp.filename().string()) h = h * 31 + (uint8_t)c;
            for (int b = 0; b < BARS; ++b) {
                float frac = 0.25f + 0.65f * ((float)((h >> (b*3)) & 0x7) / 7.f);
                float hh = floorf(frac * (CARD_H * 0.65f));
                dl->AddRectFilled({bx + b*BSPC, by - hh*0.5f},
                                  {bx + b*BSPC + 2.2f, by + hh*0.5f},
                                  bar_col, 1.f);
            }
        }

        // Filename
        std::string name = fp.filename().string();
        if ((int)name.size() > 28) name = name.substr(0,25) + "…";
        float tx = cp.x + BAR_W + 14.f;
        ImGui::PushFont(g_font_bold);
        dl->AddText(ImGui::GetFont(), 12.f, {tx, cp.y + 12.f},
                    hov ? IM_COL32(255,255,255,230) : IM_COL32(200,215,205,210),
                    name.c_str());
        ImGui::PopFont();

        // Extension badge
        std::string ext = fp.extension().string();
        for (auto& c : ext) c = (char)toupper((unsigned char)c);
        if (!ext.empty() && ext[0]=='.') ext = ext.substr(1);
        dl->AddText({tx, cp.y + 30.f}, IM_COL32(80,160,100,180), ext.c_str());

        // Duration (probe from cache if available)
        auto dit = s_source_durations.find(path);
        if (dit == s_source_durations.end()) {
            AudioMeta meta{};
            float dur = audio_probe(path, meta) ? meta.duration_secs : 0.f;
            s_source_durations[path] = dur;
            dit = s_source_durations.find(path);
        }
        if (dit != s_source_durations.end() && dit->second > 0.f) {
            std::string ds = fmt_time_short(dit->second);
            dl->AddText({tx, cp.y + 44.f}, IM_COL32(100,130,110,180), ds.c_str());
        }

        dl->AddRect(cp, {cp.x+CARD_W, cp.y+CARD_H},
                    hov ? IM_COL32(80,220,130,160) : IM_COL32(38,58,44,180),
                    6.f, 0, hov ? 1.5f : 1.f);

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload("MEDIA_AUD", path.c_str(), path.size()+1);
            ImGui::TextUnformatted(name.c_str());
            ImGui::TextDisabled("Drop onto timeline track");
            ImGui::EndDragDropSource();
        }
        if (ui_card_add_btn(cp, CARD_W, i)) {
            AudioMeta meta{};
            float dur = audio_probe(path, meta) ? meta.duration_secs : 4.f;
            if (dur <= 0.f) dur = 4.f;
            Clip cl; cl.clip_type = ClipType::Audio; cl.text = path;
            cl.source_id = path; cl.start = state.playhead; cl.end = cl.start + dur;
            s_source_durations[path] = dur;
            int target = find_empty_track(state);
            if (target < 0) {
                Track nt; nt.name = fp.stem().string();
                state.tracks.insert(state.tracks.begin(), std::move(nt));
                target = 0;
            }
            state.tracks[target].clips.push_back(cl);
            state.selected_track = target;
            state.selected_clip  = (int)state.tracks[target].clips.size() - 1;
            audio_source_ensure(path);
            recent_media_push(path, MediaKind::Audio);
            s_panel_view = PanelView::Clip;
            history_push(state, "Add audio: " + fp.filename().string());
        }

        ImGui::Dummy({0.f, GAP});
        ImGui::PopID();
    }
}
