#include "studio_types.h"
#include "studio_shared.h"
#include "panel_clip.h"
#include "panel_fx.h"
#include "pipeline.h"
#include "app.h"
#include "audio.h"
#include "audio_fx.h"
#include "video.h"
#include "proxy.h"
#include "history.h"
#include "filepicker.h"
#include "transcribe.h"
#include "beat_detect.h"
#include "waveform.h"
#include "theme.h"
#include "render.h"
#include "palette_presets.h"
#include "bg_remove.h"
#include "body_fx.h"
#include "noise_reduce.h"
#include "generated/fx_clip_set_dispatch.h"  // fx_clip_set_param for beauty preset chips
#include "../recorder.h"
#include "../video_recorder.h"
#include "../video.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;
extern ImFont* g_font_bold;
extern ImFont* g_font_black;

static std::vector<std::string> split_words_panel(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (unsigned char)s[i] <= ' ') ++i;
        if (i >= s.size()) break;
        size_t j = i;
        while (j < s.size() && (unsigned char)s[j] > ' ') ++j;
        out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

// Text edit state — declared extern in panel_clip.h
char s_edit_buf[512] = {};
bool s_edit_focus_next = false;

static void draw_clip_header(AppState& state, Clip& clip, Track& track, float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::Dummy({0.f, 8.f});

    // Type badge
    ImVec2 bp = ImGui::GetCursorScreenPos();
    const char* tname = clip_display_name(clip);
    ImVec2 tsz = ImGui::CalcTextSize(tname);
    float bpad = 6.f, bh = tsz.y + 6.f;
    float bw = tsz.x + bpad * 2.f;
    ImU32 bcol = clip_badge_color(clip);
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

    // Action row
    ImGui::Dummy({0.f, 4.f});
    if (clip.clip_type == ClipType::Video) {
        bool cropping = (state.crop_edit_track == state.selected_track &&
                         state.crop_edit_clip  == state.selected_clip);
        // Toggle canvas crop-edit mode; "*" marks an existing crop.
        if (ui_btn(cropping ? "Cropping…" : (clip.has_crop() ? "Crop *" : "Crop"),
                   cropping, true)) {
            if (cropping) {
                state.crop_edit_track = state.crop_edit_clip = -1;
            } else {
                state.crop_edit_track = state.selected_track;
                state.crop_edit_clip  = state.selected_clip;
            }
        }
        ImGui::SameLine(0.f, 6.f);
    }
    if (ui_btn("Split", false, true)) {
        float cut = state.playhead;
        if (cut > clip.start + 0.02f && cut < clip.end - 0.02f) {
            Clip right = clip_split_at(clip, cut);
            track.clips.insert(track.clips.begin() + state.selected_clip + 1, std::move(right));
            history_push(state, "Split clip");
        }
    }
    ImGui::SameLine(0.f, 6.f);
    {
        bool multi = state.clip_selection.size() > 1;
        const char* dup_label = multi ? "Duplicate group" : "Duplicate";
        const char* del_label = multi ? "Delete group"    : "Delete";
        if (ui_btn(dup_label, false, true)) {
            if (duplicate_selected_clips(state))
                history_push(state, multi ? "Duplicate clips" : "Duplicate clip");
        }
        ImGui::SameLine(0.f, 6.f);
        if (ui_btn(del_label, false, true)) {
            if (delete_selected_clips(state))
                history_push(state, multi ? "Delete clips" : "Delete clip");
            if (track.locked) ImGui::EndDisabled();
            return;
        }
    }
    if (track.locked) ImGui::EndDisabled();

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
                Clip right = clip_split_at(clip, split_t);
                right.words.assign(clip.words.begin() + s_word_sel, clip.words.end());
                clip.words.erase(clip.words.begin() + s_word_sel, clip.words.end());
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
    float bar_w = w - 16.f;
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, Col::fg);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);

    // Vertical preset buttons + custom Y slider
    struct PosBtn { int v; const char* label; };
    PosBtn pbtns[] = {{0,"Bottom"},{1,"Center"},{2,"Top"}};
    for (auto& pb : pbtns) {
        if (ui_btn(pb.label, clip.sub_pos == pb.v, true)) {
            clip.sub_pos = pb.v; history_push(state, "Position");
        }
        ImGui::SameLine(0.f, 4.f);
    }
    ImGui::NewLine();
    ImGui::Dummy({0.f, 4.f});
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted("Y"); ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(bar_w);
    if (ImGui::SliderFloat("##sub_y", &clip.sub_pos_y, 0.f, 1.f, "%.2f")) {
        clip.sub_pos = 3;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Position Y");

    ImGui::Dummy({0.f, 4.f});
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted("X"); ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(bar_w);
    if (ImGui::SliderFloat("##sub_x", &clip.sub_pos_x, 0.f, 1.f, "%.2f"))
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Position X");
    if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Position X");

    ImGui::Dummy({0.f, 4.f});
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted("Wrap width"); ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(bar_w);
    if (ImGui::SliderFloat("##sub_wrap", &clip.sub_wrap_w, 0.1f, 1.f, "%.2f"))
        if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Wrap width");
    if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Wrap width");

    ImGui::Dummy({0.f, 4.f});
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted("Font size"); ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(bar_w - 52.f);
    // stored as fraction of canvas height; expose as 1–50 percent
    float fs_pct = clip.font_size > 0.f ? clip.font_size * 100.f : 0.f;
    if (ImGui::SliderFloat("##font_sz", &fs_pct, 1.f, 50.f, "%.1f%%")) {
        clip.font_size = fs_pct / 100.f;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Font size");
    ImGui::SameLine(0.f, 4.f);
    if (ui_btn("Reset##fs", clip.font_size == 0.f, true)) {
        clip.font_size = 0.f; history_push(state, "Font size");
    }

    ImGui::PopStyleColor(2);
}

// ── Palette widget ────────────────────────────────────────────────────────────
// 423 named palettes + search + category filter.
// slots[0..n_slots-1] are float[3] or float[4]; only RGB is written.
// has_alpha=true means each slot is float[4] and alpha is preserved.

#include "palette_presets.h"

void palette_widget(const char* id, float** slots, int n_slots, bool has_alpha) {
    struct WidgetState {
        char search[48] = {};
        int  tag        = 0;   // index into g_palette_tags; 0 = All
    };
    static std::unordered_map<std::string, WidgetState> s_ws;
    WidgetState& ws = s_ws[id];

    ImGui::PushID(id);
    ImGui::Dummy({0.f, 6.f});

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float avail    = ImGui::GetContentRegionAvail().x;

    // ── Collapsible palette section ──────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Header,        IM_COL32(30, 28, 48, 255));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(50, 45, 80, 255));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  IM_COL32(70, 60, 120, 255));
    bool open = ImGui::CollapsingHeader("Palettes");
    ImGui::PopStyleColor(3);

    if (!open) { ImGui::PopID(); return; }

    ImGui::Dummy({0.f, 4.f});

    // ── Search box ───────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(28, 28, 40, 255));
    ImGui::SetNextItemWidth(avail);
    ImGui::InputTextWithHint("##palsearch", "Search palettes…", ws.search, sizeof(ws.search));
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 4.f});

    // ── Category pills ───────────────────────────────────────────────────────
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.f, 2.f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  {4.f, 4.f});
    for (int t = 0; t < g_n_palette_tags; ++t) {
        bool sel = (ws.tag == t);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(90, 70, 180, 255));
        else     ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(32, 32, 48, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(70, 60, 140, 255));
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? IM_COL32(255,255,255,255)
                                                  : IM_COL32(160,158,190,220));
        if (ImGui::SmallButton(g_palette_tags[t])) ws.tag = t;
        ImGui::PopStyleColor(3);
        if (t < g_n_palette_tags - 1) ImGui::SameLine(0.f, 4.f);
    }
    ImGui::PopStyleVar(2);
    ImGui::Dummy({0.f, 5.f});

    // ── Scrollable palette grid ──────────────────────────────────────────────
    const float CARD_H   = 30.f;
    const float NAME_H   = 11.f;
    const float STRIP_H  = CARD_H - NAME_H - 2.f;
    const float GAP      = 5.f;
    const float COL_W    = floorf((avail - GAP) * 0.5f);

    // Build filtered list
    static std::vector<int> s_filtered;
    s_filtered.clear();
    bool has_search = ws.search[0] != '\0';
    for (int i = 0; i < g_n_palettes; ++i) {
        if (ws.tag != 0 && strcmp(g_palettes[i].tag, g_palette_tags[ws.tag]) != 0)
            continue;
        if (has_search) {
            // case-insensitive substring match
            const char* src = g_palettes[i].name;
            const char* q   = ws.search;
            bool found = false;
            for (int si = 0; src[si]; ++si) {
                bool match = true;
                for (int qi = 0; q[qi] && src[si+qi]; ++qi)
                    if (tolower((unsigned char)src[si+qi]) != tolower((unsigned char)q[qi]))
                        { match = false; break; }
                if (match && !q[0]) { found = true; break; }
                if (match) {
                    // verify full query matched
                    bool full = true;
                    for (int qi = 0; q[qi]; ++qi)
                        if (!src[si+qi] || tolower((unsigned char)src[si+qi]) != tolower((unsigned char)q[qi]))
                            { full = false; break; }
                    if (full) { found = true; break; }
                }
            }
            if (!found) continue;
        }
        s_filtered.push_back(i);
    }

    int n_rows = ((int)s_filtered.size() + 1) / 2;
    float scroll_h = fminf((float)n_rows * (CARD_H + GAP) + GAP, 210.f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(14, 14, 22, 255));
    ImGui::BeginChild("##palscroll", {avail, scroll_h}, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);

    float base_y = ImGui::GetCursorPosY();

    for (int fi = 0; fi < (int)s_filtered.size(); ++fi) {
        int pi  = s_filtered[fi];
        int col = fi % 2;
        int row = fi / 2;
        float cx = GAP * 0.5f + col * (COL_W + GAP);
        float cy = base_y + row * (CARD_H + GAP);

        ImGui::SetCursorPos({cx, cy});
        ImVec2 cp = ImGui::GetCursorScreenPos();

        ImGui::PushID(fi);
        ImGui::InvisibleButton("##pc", {COL_W, CARD_H});
        bool hov = ImGui::IsItemHovered();

        // Card background
        ImU32 card_bg = hov ? IM_COL32(38,36,58,255) : IM_COL32(22,20,34,255);
        dl->AddRectFilled(cp, {cp.x+COL_W, cp.y+CARD_H}, card_bg, 5.f);

        // Color strip
        const auto& pe = g_palettes[pi];
        float sw = COL_W / pe.n;
        for (int ci = 0; ci < pe.n; ++ci) {
            ImVec2 s0 = {cp.x + ci*sw, cp.y + 2.f};
            ImVec2 s1 = {cp.x + (ci+1)*sw, cp.y + 2.f + STRIP_H};
            ImDrawFlags rf = ImDrawFlags_RoundCornersNone;
            if (ci == 0)        rf = ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersBottomLeft;
            if (ci == pe.n - 1) rf = ImDrawFlags_RoundCornersTopRight | ImDrawFlags_RoundCornersBottomRight;
            if (pe.n == 1)      rf = ImDrawFlags_RoundCornersAll;
            dl->AddRectFilled(s0, s1, IM_COL32(pe.c[ci][0], pe.c[ci][1], pe.c[ci][2], 255), 4.f, rf);
        }

        // Name
        dl->AddText(ImGui::GetFont(), 10.f,
            {cp.x + 4.f, cp.y + STRIP_H + 4.f},
            hov ? IM_COL32(255,255,255,220) : IM_COL32(155,155,175,180),
            pe.name);

        // Border
        dl->AddRect(cp, {cp.x+COL_W, cp.y+CARD_H},
            hov ? IM_COL32(140,120,255,200) : IM_COL32(45,42,65,180), 5.f, 0, 1.f);

        // Click → apply to all slots
        if (ImGui::IsItemClicked()) {
            for (int si = 0; si < n_slots; ++si) {
                int ci = si < pe.n ? si : pe.n - 1;
                slots[si][0] = pe.c[ci][0] / 255.f;
                slots[si][1] = pe.c[ci][1] / 255.f;
                slots[si][2] = pe.c[ci][2] / 255.f;
                (void)has_alpha; // alpha always preserved — we never touch [3]
            }
        }

        if (hov) ImGui::SetTooltip("%s  ·  %s", pe.name, pe.tag);
        ImGui::PopID();
    }

    // Advance cursor so child window knows its height
    ImGui::SetCursorPosY(base_y + n_rows * (CARD_H + GAP) + 4.f);
    ImGui::Dummy({0.f, 0.f});

    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Dummy({0.f, 4.f});
    ImGui::PopID();
}

// Convenience: single slot (float[3] or float[4])
void palette_widget(const char* id, float* rgb) {
    float* s[1] = { rgb };
    palette_widget(id, s, 1, false);
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
        palette_widget("##pal_subcol", clip.sub_color);
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

static void section_text_style(AppState& state, Clip& clip, float w) {
    TextStyle& ts = clip.ts;
    float sw = w - 16.f;
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, Col::fg);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);

    // Shadow
    bool shad = ts.shadow_enabled;
    if (ImGui::Checkbox("Shadow##ts_shad", &shad)) { ts.shadow_enabled = shad; history_push(state, "Text shadow"); }
    if (ts.shadow_enabled) {
        ImGui::SameLine(0.f, 8.f); ImGui::SetNextItemWidth(54.f);
        if (ImGui::SliderFloat("ox##ts_sox", &ts.shadow_ox, -20.f, 20.f, "%.0f"))
            history_push(state, "Shadow offset");
        ImGui::SameLine(0.f, 4.f); ImGui::SetNextItemWidth(54.f);
        if (ImGui::SliderFloat("oy##ts_soy", &ts.shadow_oy, -20.f, 20.f, "%.0f"))
            history_push(state, "Shadow offset");
        ImGui::SameLine(0.f, 4.f); ImGui::SetNextItemWidth(sw - 130.f);
        if (ImGui::ColorEdit4("##ts_scol", ts.shadow_col,
                ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar))
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Shadow color");
    }

    ImGui::Dummy({0.f, 2.f});

    // Stroke
    bool stk = ts.stroke_enabled;
    if (ImGui::Checkbox("Stroke##ts_stk", &stk)) { ts.stroke_enabled = stk; history_push(state, "Text stroke"); }
    if (ts.stroke_enabled) {
        ImGui::SameLine(0.f, 8.f); ImGui::SetNextItemWidth(70.f);
        if (ImGui::SliderFloat("w##ts_sw", &ts.stroke_w, 0.5f, 10.f, "%.1f"))
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Stroke width");
        ImGui::SameLine(0.f, 4.f); ImGui::SetNextItemWidth(sw - 100.f);
        if (ImGui::ColorEdit4("##ts_stkcol", ts.stroke_col,
                ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar))
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Stroke color");
    }

    ImGui::Dummy({0.f, 2.f});

    // Glow
    bool glow = ts.glow_enabled;
    if (ImGui::Checkbox("Glow##ts_glow", &glow)) { ts.glow_enabled = glow; history_push(state, "Text glow"); }
    if (ts.glow_enabled) {
        ImGui::SameLine(0.f, 8.f); ImGui::SetNextItemWidth(70.f);
        if (ImGui::SliderFloat("r##ts_gr", &ts.glow_r, 1.f, 30.f, "%.0f"))
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Glow radius");
        ImGui::SameLine(0.f, 4.f); ImGui::SetNextItemWidth(sw - 100.f);
        if (ImGui::ColorEdit4("##ts_gcol", ts.glow_col,
                ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar))
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Glow color");
    }

    ImGui::Dummy({0.f, 2.f});

    // Background box
    bool bg = ts.bg_enabled;
    if (ImGui::Checkbox("Background##ts_bg", &bg)) { ts.bg_enabled = bg; history_push(state, "Text background"); }
    if (ts.bg_enabled) {
        ImGui::SetNextItemWidth(sw);
        if (ImGui::ColorEdit4("##ts_bgcol", ts.bg_col,
                ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar))
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Background color");
        ImGui::SetNextItemWidth(54.f);
        if (ImGui::SliderFloat("pad x##ts_bpx", &ts.bg_pad_x, 0.f, 40.f, "%.0f"))
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Background padding");
        ImGui::SameLine(0.f, 4.f); ImGui::SetNextItemWidth(54.f);
        if (ImGui::SliderFloat("pad y##ts_bpy", &ts.bg_pad_y, 0.f, 40.f, "%.0f"))
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Background padding");
        ImGui::SameLine(0.f, 4.f); ImGui::SetNextItemWidth(60.f);
        if (ImGui::SliderFloat("corner##ts_bc", &ts.bg_corner, 0.f, 20.f, "%.0f"))
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Background corner");
    }

    ImGui::PopStyleColor(2);
}

void panel_clip(AppState& state, float w) {
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

    if (track.locked) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted("Track is locked");
        ImGui::PopStyleColor();
        return;
    }

    ImGui::Dummy({0.f, 4.f});

    // ── Keyframe slider helper (shared by transform + audio sections) ─────────
    float t_local = state.playhead - clip.start;
    int   sel_ti  = state.selected_track, sel_ci = state.selected_clip;

    // Keyframable slider: ◆ toggle + label row, full-width slider beneath
    // (same layout as the plain sliders so sections stay visually uniform).
    // disp scales the field value for display (100 → percent sliders);
    // vmin/vmax/fmt are in display units, the field and keys store raw.
    // prop2 mirrors key add/remove/update onto a second track — used by the
    // unified Size slider which drives scale_x + scale_y together.
    auto kf_slider = [&](const char* prop, const char* label,
                          float* val_ptr, float vmin, float vmax, const char* fmt,
                          float disp = 1.f, const char* prop2 = nullptr) -> bool
    {
        bool changed = false;
        auto  it_pt  = clip.ktracks.find(prop);
        PropTrack* pt = (it_pt != clip.ktracks.end()) ? &it_pt->second : nullptr;
        bool has_keys = pt && !pt->empty();
        bool has_kf   = pt && pt->find_nearest(t_local, 0.05f) >= 0;

        ImGui::PushStyleColor(ImGuiCol_Button,
            has_kf ? IM_COL32(255,200,60,200)
                   : has_keys ? IM_COL32(140,120,60,180) : IM_COL32(80,80,80,180));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255,220,80,220));
        char kbid[64]; snprintf(kbid, sizeof(kbid), "\xe2\x97\x86##kf_%s", prop);
        if (ImGui::Button(kbid, {20.f, 0.f})) {
            if (has_kf) {
                pt->remove_at(t_local, 0.05f);
                if (pt->empty()) clip.ktracks.erase(prop);
                if (prop2) {
                    auto it2 = clip.ktracks.find(prop2);
                    if (it2 != clip.ktracks.end()) {
                        it2->second.remove_at(t_local, 0.05f);
                        if (it2->second.empty()) clip.ktracks.erase(it2);
                    }
                }
                history_push(state, std::string("Remove KF ") + prop);
            } else {
                clip.ktracks[prop].set(t_local, clip.eval_prop(prop, state.playhead));
                if (prop2)
                    clip.ktracks[prop2].set(t_local, clip.eval_prop(prop2, state.playhead));
                state.kf_sel_track = sel_ti; state.kf_sel_clip = sel_ci;
                state.kf_sel_prop  = prop;
                state.kf_sel_idx   = clip.ktracks[prop].find_nearest(t_local, 0.1f);
                history_push(state, std::string("Add KF ") + prop);
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(has_kf ? "Remove keyframe at playhead"
                                     : "Add keyframe at playhead");
        ImGui::PopStyleColor(2);
        ImGui::SameLine(0.f, 6.f);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted(label); ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, has_keys ? IM_COL32(255,200,60,255) : to_u32(Col::fg));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
        ImGui::SetNextItemWidth(w - 16.f);
        char sid[64]; snprintf(sid, sizeof(sid), "##kfs_%s", prop);
        // When keys exist, the slider shows/edits the animated value at the
        // playhead — editing it retargets the key under the playhead (if any)
        // instead of the static field, which the renderer would ignore.
        float dv = (has_keys ? clip.eval_prop(prop, state.playhead) : *val_ptr) * disp;
        if (ImGui::SliderFloat(sid, &dv, vmin, vmax, fmt)) {
            changed = true;
            float raw = dv / disp;
            *val_ptr = raw;
            if (has_keys) {
                // Auto-key: retarget the key under the playhead, or drop a new
                // one there — otherwise the edit would go to the static field,
                // which the renderer ignores once keys exist.
                int ki = pt->find_nearest(t_local, 0.05f);
                if (ki >= 0) pt->keys[ki].value = raw; else pt->set(t_local, raw);
                if (prop2) {
                    PropTrack& p2 = clip.ktracks[prop2];
                    int k2 = p2.find_nearest(t_local, 0.05f);
                    if (k2 >= 0) p2.keys[k2].value = raw; else p2.set(t_local, raw);
                }
            }
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
            if (ui_btn(it3.n, kf.interp == it3.t, true)) {
                kf.interp = it3.t;
                // Size keys come in scale_x/scale_y pairs — keep their easing
                // in lockstep when a twin key sits at the same time.
                const char* twin = state.kf_sel_prop == "scale_x" ? "scale_y"
                                 : state.kf_sel_prop == "scale_y" ? "scale_x" : nullptr;
                if (twin) {
                    auto tt = clip.ktracks.find(twin);
                    if (tt != clip.ktracks.end()) {
                        int k2 = tt->second.find_nearest(kf.time, 0.02f);
                        if (k2 >= 0) tt->second.keys[k2].interp = it3.t;
                    }
                }
                history_push(state, "KF interp");
            }
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
        if (ImGui::CollapsingHeader("Text Style")) {
            ImGui::Dummy({0.f, 4.f}); section_text_style(state, clip, w); ImGui::Dummy({0.f, 4.f});
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
                std::string new_text = s_edit_buf;
                if (new_text != clip.text) {
                    // Diff against original word entries to record which words changed.
                    // Only store when token count matches — structural edits (adding/
                    // removing words) can't be reliably replayed across regroupings.
                    if (!clip.words.empty()) {
                        auto tokens = split_words_panel(new_text);
                        if (tokens.size() == clip.words.size()) {
                            for (int wi = 0; wi < (int)clip.words.size(); ++wi) {
                                if (tokens[wi] != clip.words[wi].text) {
                                    int frame = (int)(clip.words[wi].start * (float)state.fps);
                                    state.lyrics_edits[frame] = tokens[wi];
                                    clip.words[wi].text = tokens[wi];
                                }
                            }
                        }
                    }
                    history_push(state, "Edit lyrics text");
                }
                clip.text = new_text;
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
        if (ImGui::CollapsingHeader("Text Style")) {
            ImGui::Dummy({0.f, 4.f}); section_text_style(state, clip, w); ImGui::Dummy({0.f, 4.f});
        }

        if (ImGui::CollapsingHeader("Grouping")) {
            ImGui::Dummy({0.f, 4.f});
            if (!state.words_json_path.empty() && fs::exists(state.words_json_path)) {
                struct ModeBtn { SubtitleMode m; const char* label; const char* tip; };
                static const ModeBtn modes[] = {
                    {SubtitleMode::Word,    "Word",    "One clip per word"},
                    {SubtitleMode::Phrase,  "Phrase",  "Group by short pauses (>0.3s)"},
                    {SubtitleMode::Line,    "Line",    "Group by breath gaps (>0.8s)"},
                    {SubtitleMode::Segment, "Segment", "Sentence boundaries"},
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
                            tgt.ts = clip.ts;
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
        float bar_w = w - 16.f;
        auto plain_slider = [&](const char* id, const char* label, float* v,
                                float vmin, float vmax, const char* fmt,
                                ImGuiSliderFlags flags = 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, to_u32(Col::fg));
            ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
            ImGui::SetNextItemWidth(bar_w);
            bool ch = ImGui::SliderFloat(id, v, vmin, vmax, fmt, flags);
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, label);
            return ch;
        };

        // ── File ─────────────────────────────────────────────────────────────
        ImGui::Dummy({0.f, 4.f});
        {
            std::string fname = clip.text.empty() ? "(no file)" : fs::path(clip.text).filename().string();
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted(fname.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && !clip.text.empty()) {
                ImGui::BeginTooltip(); ImGui::TextUnformatted(clip.text.c_str()); ImGui::EndTooltip();
            }
            ImGui::SameLine(0.f, 8.f);
            if (ui_btn("Replace…", false, true)) {
                std::string np = filepicker_open("Replace video", "Video/Audio",
                    "*.mp4 *.mov *.mkv *.webm *.avi *.mp3 *.wav *.flac *.aac");
                if (!np.empty()) { clip.text = np; history_push(state, "Replace source"); }
            }
        }

        // ── Look ─────────────────────────────────────────────────────────────
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        ui_label("Look");
        ImGui::Dummy({0.f, 6.f});
        {
            kf_slider("opacity", "Opacity", &clip.opacity, 0.f, 100.f, "%.0f%%", 100.f);
            kf_interp_bar();
            ImGui::Dummy({0.f, 4.f});
            plain_slider("##vid_fi", "Fade in",  &clip.fade_in,  0.f, 4.f, "%.2fs");
            ImGui::Dummy({0.f, 4.f});
            plain_slider("##vid_fo", "Fade out", &clip.fade_out, 0.f, 4.f, "%.2fs");
        }

        // ── Layout ───────────────────────────────────────────────────────────
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        ui_label("Layout");
        ImGui::Dummy({0.f, 6.f});
        {
            kf_slider("pos_x", "Left \xe2\x86\x94 Right", &clip.pos_x, -1.f, 2.f, "%.2f");
            kf_interp_bar();
            ImGui::Dummy({0.f, 4.f});
            kf_slider("pos_y", "Up \xe2\x86\x95 Down",   &clip.pos_y, -1.f, 2.f, "%.2f");
            kf_interp_bar();
            ImGui::Dummy({0.f, 4.f});
            // Unified size — drives scale_x and scale_y together (keys included)
            float size = (clip.scale_x + clip.scale_y) * 0.5f;
            if (kf_slider("scale_x", "Size", &size, 0.f, 4.f, "%.2f", 1.f, "scale_y")) {
                clip.scale_x = size; clip.scale_y = size;
            }
            kf_interp_bar();
            ImGui::Dummy({0.f, 4.f});
            kf_slider("rotation", "Rotation", &clip.rotation, -180.f, 180.f, "%.1f\xc2\xb0");
            kf_interp_bar();
        }

        // ── Crop (non-destructive UV window; edit visually via Crop button) ──
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        ui_label("Crop");
        ImGui::Dummy({0.f, 6.f});
        {
            // Sliders in percent; each side clamps against its opposite so a
            // sliver always stays visible (same rule as set_clip_prop).
            struct CS { const char* id; const char* lbl; float* v; float* opp; };
            CS sides[] = {{"##crop_l", "Left",   &clip.crop_l, &clip.crop_r},
                          {"##crop_r", "Right",  &clip.crop_r, &clip.crop_l},
                          {"##crop_t", "Top",    &clip.crop_t, &clip.crop_b},
                          {"##crop_b", "Bottom", &clip.crop_b, &clip.crop_t}};
            for (auto& s : sides) {
                float pct = *s.v * 100.f;
                if (plain_slider(s.id, s.lbl, &pct, 0.f, 95.f, "%.0f%%"))
                    *s.v = fmaxf(0.f, fminf(pct / 100.f, 0.95f - *s.opp));
                if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Crop clip");
                ImGui::Dummy({0.f, 4.f});
            }
            if (clip.has_crop() && ui_btn("Reset crop", false, true)) {
                clip.crop_l = clip.crop_t = clip.crop_r = clip.crop_b = 0.f;
                history_push(state, "Reset crop");
            }
        }

        // ── Speed ────────────────────────────────────────────────────────────
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        ui_label("Speed");
        ImGui::Dummy({0.f, 6.f});
        {
            float old_spd = clip.speed;
            // Log scale: 100× cap for screen-recording retimes — most of the
            // slider's travel stays in the everyday 0.25–10× range.
            plain_slider("##vid_spd", "Speed", &clip.speed, 0.25f, 100.f, "%.2f\xc3\x97",
                         ImGuiSliderFlags_Logarithmic);
            if (fabsf(clip.speed - old_spd) > 1e-5f)
                rescale_glass_bricks(state, state.selected_track, state.selected_clip,
                                     clip.speed / old_spd);
            ImGui::Dummy({0.f, 6.f});
            struct SP { float f; const char* l; };
            SP spresets[] = {{0.25f,"\xc2\xbc\xc3\x97"},{0.5f,"\xc2\xbd\xc3\x97"},
                             {1.f,"1\xc3\x97"},{2.f,"2\xc3\x97"},{4.f,"4\xc3\x97"},
                             {10.f,"10\xc3\x97"},{30.f,"30\xc3\x97"},{100.f,"100\xc3\x97"}};
            for (auto& p : spresets) {
                if (ui_btn(p.l, fabsf(clip.speed - p.f) < 0.01f, true)) {
                    float prev = clip.speed;
                    clip.speed = p.f;
                    rescale_glass_bricks(state, state.selected_track, state.selected_clip,
                                         clip.speed / prev);
                    history_push(state, "Speed");
                }
                ImGui::SameLine(0.f, 4.f);
            }
            ImGui::NewLine();
        }

        // ── Sound ────────────────────────────────────────────────────────────
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        ui_label("Sound");
        ImGui::Dummy({0.f, 6.f});
        {
            kf_slider("volume", "Volume", &clip.volume, 0.f, 200.f, "%.0f%%", 100.f);
            kf_interp_bar();
            ImGui::Dummy({0.f, 4.f});
            kf_slider("pan", "Pan  (left \xe2\x86\x94 right)", &clip.pan, -1.f, 1.f, "%.2f");
            kf_interp_bar();
        }

        // ── AI Tools ─────────────────────────────────────────────────────────
        bool busy     = transcribe_running();
        bool has_path = !clip.text.empty();
        bool ml_avail = state.models_ready;

        // Shared install gate — shown once above all AI tools if needed
        if (!ml_avail) {
            ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
            ui_label("AI Tools");
            ImGui::Dummy({0.f, 6.f});
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextWrapped("AI models not set up yet.");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 4.f});
            if (ui_btn("Set Up AI Features", false, true)) state.show_model_dl_modal = true;
        } else {
            // Shared progress bar helper — renders under whichever tool is running
            auto ai_progress = [&]() {
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
                if (ui_btn("Cancel##aipipe", false, true)) transcribe_cancel();
            };

            bool is_lyrics   = busy && state.last_pipeline_mode == PipelineMode::Both;
            bool is_subs     = busy && state.last_pipeline_mode == PipelineMode::TranscribeOnly;
            bool is_separate = busy && state.last_pipeline_mode == PipelineMode::SeparateOnly;

            // Extract Lyrics
            ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
            ui_label("Extract Lyrics");
            ImGui::Dummy({0.f, 6.f});
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextWrapped("Transcribes speech into lyric clips synced to the audio.");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 4.f});
            if (is_lyrics) { ai_progress(); }
            else { if (!has_path || busy) ImGui::BeginDisabled();
                   if (ui_btn("Extract Lyrics", false, true)) {
                       state.pipeline_on_done = apply_subtitle_mode;
                       kick_pipeline(state, clip.text, PipelineMode::Both);
                   }
                   if (!has_path || busy) ImGui::EndDisabled(); }

            // Extract Subtitles
            ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
            ui_label("Extract Subtitles");
            ImGui::Dummy({0.f, 6.f});
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextWrapped("Generates subtitle clips from speech — no word-level timing.");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 4.f});
            if (is_subs) { ai_progress(); }
            else { if (!has_path || busy) ImGui::BeginDisabled();
                   if (ui_btn("Extract Subtitles", false, true)) {
                       state.pipeline_on_done = apply_subtitle_mode;
                       kick_pipeline(state, clip.text, PipelineMode::TranscribeOnly);
                   }
                   if (!has_path || busy) ImGui::EndDisabled(); }

            // Separate Vocals
            ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
            ui_label("Separate Vocals");
            ImGui::Dummy({0.f, 6.f});
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextWrapped("Splits the track into vocals and backing music.");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 4.f});
            if (is_separate) { ai_progress(); }
            else { if (!has_path || busy) ImGui::BeginDisabled();
                   if (ui_btn("Separate Vocals", false, true)) kick_pipeline(state, clip.text, PipelineMode::SeparateOnly);
                   if (!has_path || busy) ImGui::EndDisabled(); }
        }

        // ── Noise Reduction ───────────────────────────────────────────────────
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        ui_label("Noise Reduction");
        ImGui::Dummy({0.f, 6.f});
        {
            bool nr_running = state.noise_reduce_running;
            bool has_path2  = !clip.text.empty();
            if (nr_running) {
                ImVec2 bp = ImGui::GetCursorScreenPos();
                ImDrawList* nrdl = ImGui::GetWindowDrawList();
                float t2 = fmodf((float)ImGui::GetTime() * 0.8f, 1.f);
                float p  = state.noise_reduce_progress;
                nrdl->AddRectFilled(bp, {bp.x+bar_w, bp.y+4.f}, to_u32(Col::line), 2.f);
                if (p > 0.f)
                    nrdl->AddRectFilled(bp, {bp.x+bar_w*p, bp.y+4.f}, to_u32(Col::fg), 2.f);
                else
                    nrdl->AddRectFilled({bp.x+bar_w*t2, bp.y}, {bp.x+bar_w*fminf(1.f,t2+0.3f), bp.y+4.f}, to_u32(Col::fg), 2.f);
                ImGui::Dummy({0.f, 8.f});
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                ImGui::TextUnformatted("Reducing noise…");
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                ImGui::TextWrapped("Removes room hum, AC and mic self-noise.");
                ImGui::PopStyleColor();
                if (!state.noise_reduce_output.empty()) {
                    ImGui::Dummy({0.f, 4.f});
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.25f, 0.85f, 0.4f, 1.f));
                    ImGui::TextWrapped("Done — denoised file applied.");
                    ImGui::PopStyleColor();
                }
                if (!state.noise_reduce_error.empty()) {
                    ImGui::Dummy({0.f, 4.f});
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.3f, 0.3f, 1.f));
                    std::string e = state.noise_reduce_error;
                    if (e.size() > 120) e = e.substr(0, 117) + "...";
                    ImGui::TextWrapped("%s", e.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::Dummy({0.f, 4.f});
                if (!has_path2) ImGui::BeginDisabled();
                if (ui_btn("Reduce Noise", false, true)) {
                    state.noise_reduce_error.clear();
                    noise_reduce_start(state, clip.text);
                }
                if (!has_path2) ImGui::EndDisabled();
                ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                ImGui::TextWrapped("Removes room hum, AC and mic self-noise before transcription.");
                ImGui::PopStyleColor();
            }
            ImGui::Dummy({0.f, 4.f});
        }

        // ── Remove Background ─────────────────────────────────────────────────
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        ui_label("Remove Background");
        ImGui::Dummy({0.f, 6.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
        ImGui::TextWrapped("Cuts out the video background using AI.");
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 4.f});
        {
            ImDrawList* bgdl = ImGui::GetWindowDrawList();
            auto status = clip.bg_remove_status;

            static BgRemoveStatus s_prev_bgr_status = BgRemoveStatus::Idle;
            static int            s_prev_bgr_clip   = -1;
            int cur_clip = state.selected_clip;

            if (status == BgRemoveStatus::Processing && clip.bg_remove_progress > 0.f) {
                float dur = clip.end - clip.start;
                if (dur > 0.f)
                    state.playhead = clip.start + fminf(clip.bg_remove_progress, 0.99f) * dur;
            } else if (status == BgRemoveStatus::Ready &&
                       s_prev_bgr_status == BgRemoveStatus::Processing &&
                       s_prev_bgr_clip == cur_clip) {
                seek_to(state, clip.start);
            }
            s_prev_bgr_status = status;
            s_prev_bgr_clip   = cur_clip;

            if (status == BgRemoveStatus::Processing) {
                ImVec2 bp  = ImGui::GetCursorScreenPos();
                ImU32 fill_col = IM_COL32(255, 165, 0, 255);
                ImU32 track_col = IM_COL32(255, 165, 0, 40);
                bgdl->AddRectFilled(bp, {bp.x + bar_w, bp.y + 4.f}, track_col, 2.f);
                if (clip.bg_remove_progress < 0.f) {
                    float t = fmodf((float)ImGui::GetTime() * 0.8f, 1.f);
                    float x0 = bp.x + (bar_w - bar_w * 0.35f) * t;
                    bgdl->AddRectFilled({x0, bp.y}, {x0 + bar_w * 0.35f, bp.y + 4.f}, fill_col, 2.f);
                    ImGui::Dummy({0.f, 8.f});
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                    ImGui::TextUnformatted("Downloading AI model…");
                    ImGui::PopStyleColor();
                } else {
                    float pct = fmaxf(0.01f, fminf(1.f, clip.bg_remove_progress));
                    bgdl->AddRectFilled(bp, {bp.x + bar_w * pct, bp.y + 4.f}, fill_col, 2.f);
                    ImGui::Dummy({0.f, 8.f});
                    char msg[64];
                    snprintf(msg, sizeof(msg), "Removing background…  %d%%", (int)(pct * 100.f));
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                    ImGui::TextUnformatted(msg);
                    ImGui::PopStyleColor();
                }
                ImGui::Dummy({0.f, 4.f});

            } else if (status == BgRemoveStatus::Ready) {
                ImVec2 bp = ImGui::GetCursorScreenPos();
                bgdl->AddRectFilled(bp, {bp.x + bar_w, bp.y + 4.f}, IM_COL32(40, 200, 80, 255), 2.f);
                ImGui::Dummy({0.f, 8.f});
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.25f, 0.85f, 0.4f, 1.f));
                ImGui::TextUnformatted("Ready");
                ImGui::PopStyleColor();
                ImGui::Dummy({0.f, 6.f});
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted("Edge softness"); ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_SliderGrab, to_u32(Col::fg));
                ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
                ImGui::SetNextItemWidth(bar_w);
                if (ImGui::SliderFloat("##bgrsoft", &clip.bg_remove_softness, 0.f, 1.f, "%.2f"))
                    history_push(state, "BG Softness");
                ImGui::PopStyleColor(2);
                ImGui::Dummy({0.f, 6.f});
                bool box_tog = clip.bg_remove_box_on;
                if (ImGui::Checkbox("Limit area##bgrbox", &box_tog)) {
                    clip.bg_remove_box_on = box_tog;
                    history_push(state, "BG Box");
                }
                if (clip.bg_remove_box_on) {
                    ImGui::PushStyleColor(ImGuiCol_SliderGrab, to_u32(Col::fg));
                    ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
                    float half_w = (bar_w - 4.f) * 0.5f;
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                    ImGui::TextUnformatted("Left"); ImGui::PopStyleColor();
                    ImGui::SetNextItemWidth(half_w);
                    if (ImGui::SliderFloat("##bgrl", &clip.bg_remove_box_l, 0.f, clip.bg_remove_box_r - 0.01f, "%.2f"))
                        history_push(state, "BG Box");
                    ImGui::SameLine(0.f, 4.f);
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                    ImGui::TextUnformatted("Right"); ImGui::PopStyleColor();
                    ImGui::SetNextItemWidth(half_w);
                    if (ImGui::SliderFloat("##bgrr", &clip.bg_remove_box_r, clip.bg_remove_box_l + 0.01f, 1.f, "%.2f"))
                        history_push(state, "BG Box");
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                    ImGui::TextUnformatted("Top"); ImGui::PopStyleColor();
                    ImGui::SetNextItemWidth(half_w);
                    if (ImGui::SliderFloat("##bgrt", &clip.bg_remove_box_t, 0.f, clip.bg_remove_box_b - 0.01f, "%.2f"))
                        history_push(state, "BG Box");
                    ImGui::SameLine(0.f, 4.f);
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                    ImGui::TextUnformatted("Bottom"); ImGui::PopStyleColor();
                    ImGui::SetNextItemWidth(half_w);
                    if (ImGui::SliderFloat("##bgrb", &clip.bg_remove_box_b, clip.bg_remove_box_t + 0.01f, 1.f, "%.2f"))
                        history_push(state, "BG Box");
                    ImGui::PopStyleColor(2);
                }
                ImGui::Dummy({0.f, 4.f});
                if (ui_btn("Re-run", false, true))
                    bg_remove_start(state, state.selected_track, state.selected_clip);

            } else if (status == BgRemoveStatus::Error) {
                ImVec2 bp = ImGui::GetCursorScreenPos();
                bgdl->AddRectFilled(bp, {bp.x + bar_w, bp.y + 4.f}, IM_COL32(220, 60, 60, 255), 2.f);
                ImGui::Dummy({0.f, 8.f});
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.3f, 0.3f, 1.f));
                ImGui::TextUnformatted("Failed");
                ImGui::PopStyleColor();
                if (!clip.bg_remove_error.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                    std::string err = clip.bg_remove_error;
                    if (err.size() > 120) err = err.substr(0, 117) + "...";
                    ImGui::TextWrapped("%s", err.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::Dummy({0.f, 4.f});
                if (ui_btn("Retry", false, true))
                    bg_remove_start(state, state.selected_track, state.selected_clip);

            } else {
                bool proxy_ok = !clip.text.empty() && proxy_is_ready(clip.text);
                if (!proxy_ok) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                    ImGui::TextWrapped("Waiting for video proxy…");
                    ImGui::PopStyleColor();
                    ImGui::Dummy({0.f, 4.f});
                }
                if (!proxy_ok) ImGui::BeginDisabled();
                if (ui_btn("Remove Background", false, true))
                    bg_remove_start(state, state.selected_track, state.selected_clip);
                if (!proxy_ok) ImGui::EndDisabled();
                ImGui::Dummy({0.f, 4.f});
            }
        }

        // ── Advanced ─────────────────────────────────────────────────────────
        // Everything keyframable lives in its own section now (Look / Layout /
        // Sound); only asymmetric scaling stays tucked away here.
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        if (ImGui::CollapsingHeader("Advanced##vid_adv")) {
            ImGui::Dummy({0.f, 4.f});
            kf_slider("scale_x", "Scale X", &clip.scale_x, 0.f, 4.f, "%.2f"); kf_interp_bar();
            ImGui::Dummy({0.f, 2.f});
            kf_slider("scale_y", "Scale Y", &clip.scale_y, 0.f, 4.f, "%.2f"); kf_interp_bar();
            ImGui::Dummy({0.f, 4.f});
        }

    }

    // ═══════════════════════════════════════════════════════════════════════════
    // AUDIO CLIP
    // ═══════════════════════════════════════════════════════════════════════════
    else if (clip.clip_type == ClipType::Audio) {
        float bar_w = w - 16.f;
        auto plain_slider = [&](const char* id, const char* label, float* v,
                                float vmin, float vmax, const char* fmt,
                                ImGuiSliderFlags flags = 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, to_u32(Col::fg));
            ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
            ImGui::SetNextItemWidth(bar_w);
            bool ch = ImGui::SliderFloat(id, v, vmin, vmax, fmt, flags);
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, label);
            return ch;
        };

        // ── File ─────────────────────────────────────────────────────────────
        ImGui::Dummy({0.f, 4.f});
        {
            std::string fname = clip.text.empty() ? "(no file)" : fs::path(clip.text).filename().string();
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted(fname.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && !clip.text.empty()) {
                ImGui::BeginTooltip(); ImGui::TextUnformatted(clip.text.c_str()); ImGui::EndTooltip();
            }
            ImGui::SameLine(0.f, 8.f);
            if (ui_btn("Replace…", false, true)) {
                std::string np = filepicker_open("Replace audio", "Audio",
                    "*.mp3 *.wav *.flac *.aac *.ogg *.m4a");
                if (!np.empty()) { clip.text = np; history_push(state, "Replace source"); }
            }
        }

        // ── Look ─────────────────────────────────────────────────────────────
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        ui_label("Look");
        ImGui::Dummy({0.f, 6.f});
        {
            plain_slider("##aud_fi", "Fade in",  &clip.fade_in,  0.f, 4.f, "%.2fs");
            ImGui::Dummy({0.f, 4.f});
            plain_slider("##aud_fo", "Fade out", &clip.fade_out, 0.f, 4.f, "%.2fs");
        }

        // ── Speed ────────────────────────────────────────────────────────────
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        ui_label("Speed");
        ImGui::Dummy({0.f, 6.f});
        {
            float old_spd = clip.speed;
            // Log scale, 100× cap (atempo chain in export supports 0.5–100).
            plain_slider("##aud_spd", "Speed", &clip.speed, 0.25f, 100.f, "%.2f\xc3\x97",
                         ImGuiSliderFlags_Logarithmic);
            if (fabsf(clip.speed - old_spd) > 1e-5f)
                rescale_glass_bricks(state, state.selected_track, state.selected_clip,
                                     clip.speed / old_spd);
            ImGui::Dummy({0.f, 6.f});
            struct SP { float f; const char* l; };
            SP spresets[] = {{0.25f,"\xc2\xbc\xc3\x97"},{0.5f,"\xc2\xbd\xc3\x97"},
                             {1.f,"1\xc3\x97"},{2.f,"2\xc3\x97"},{4.f,"4\xc3\x97"},
                             {10.f,"10\xc3\x97"},{30.f,"30\xc3\x97"},{100.f,"100\xc3\x97"}};
            for (auto& p : spresets) {
                if (ui_btn(p.l, fabsf(clip.speed - p.f) < 0.01f, true)) {
                    float prev = clip.speed;
                    clip.speed = p.f;
                    rescale_glass_bricks(state, state.selected_track, state.selected_clip,
                                         clip.speed / prev);
                    history_push(state, "Speed");
                }
                ImGui::SameLine(0.f, 4.f);
            }
            ImGui::NewLine();
        }

        // ── Sound ────────────────────────────────────────────────────────────
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        ui_label("Sound");
        ImGui::Dummy({0.f, 6.f});
        {
            float vol_pct = clip.volume * 100.f;
            if (plain_slider("##aud_vol", "Volume", &vol_pct, 0.f, 200.f, "%.0f%%"))
                clip.volume = vol_pct / 100.f;
            ImGui::Dummy({0.f, 4.f});
            plain_slider("##aud_pan", "Pan  (left \xe2\x86\x94 right)", &clip.pan, -1.f, 1.f, "%.2f");
        }

        // ── Text to Speech ────────────────────────────────────────────────────
        {
            extern void import_file(AppState&, const std::string&);
            ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
            ui_label("Text to Speech");
            ImGui::Dummy({0.f, 6.f});

            static char s_tts_text[2048] = {};
            static int  s_tts_voice      = 0;
            static TTSState s_tts_state;
            static std::string s_tts_added;

            static const char* tts_voices[] = { "female", "male", "whisper", "narrator" };

            ImGui::SetNextItemWidth(bar_w);
            ImGui::InputTextMultiline("##tts_in", s_tts_text, sizeof(s_tts_text),
                                      {bar_w, 64.f});
            ImGui::Dummy({0.f, 4.f});
            ImGui::SetNextItemWidth(bar_w);
            ImGui::Combo("##tts_voice", &s_tts_voice, tts_voices, 4);
            ImGui::Dummy({0.f, 6.f});

            bool tts_busy = (s_tts_state.status == TTSStatus::Running);
            if (tts_busy) {
                ImVec2 bp = ImGui::GetCursorScreenPos();
                ImDrawList* dl2 = ImGui::GetWindowDrawList();
                dl2->AddRectFilled(bp, {bp.x+bar_w, bp.y+4.f}, to_u32(Col::line), 2.f);
                dl2->AddRectFilled(bp, {bp.x+bar_w*s_tts_state.progress, bp.y+4.f},
                                   to_u32(Col::fg), 2.f);
                ImGui::Dummy({0.f, 8.f});
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                ImGui::TextUnformatted("Generating…");
                ImGui::PopStyleColor();
            } else {
                if (s_tts_state.status == TTSStatus::Done && !s_tts_state.out_path.empty()
                    && s_tts_added != s_tts_state.out_path) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                    ImGui::TextUnformatted("Ready.");
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0.f, 8.f);
                    if (ui_btn("Add to timeline##tts_add", false, true)) {
                        import_file(state, s_tts_state.out_path);
                        s_tts_added = s_tts_state.out_path;
                        s_tts_state = TTSState{};
                    }
                }
                if (!s_tts_state.error.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f,0.3f,0.3f,1.f));
                    ImGui::TextWrapped("%s", s_tts_state.error.c_str());
                    ImGui::PopStyleColor();
                    ImGui::Dummy({0.f, 4.f});
                }
                bool can_gen = s_tts_text[0] != '\0';
                if (!can_gen) ImGui::BeginDisabled();
                if (ui_btn("Generate##tts_go", false, true)) {
                    s_tts_state = TTSState{};
                    tts_generate(s_tts_text, tts_voices[s_tts_voice], s_tts_state);
                }
                if (!can_gen) ImGui::EndDisabled();
            }
            ImGui::Dummy({0.f, 6.f});
        }

        // ── AI Tools ─────────────────────────────────────────────────────────
        bool busy     = transcribe_running();
        bool has_path = !clip.text.empty();
        bool ml_avail = state.models_ready;

        if (!ml_avail) {
            ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
            ui_label("AI Tools");
            ImGui::Dummy({0.f, 6.f});
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextWrapped("AI models not set up yet.");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 4.f});
            if (ui_btn("Set Up AI Features", false, true)) state.show_model_dl_modal = true;
        } else {
            auto ai_progress = [&]() {
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
                if (ui_btn("Cancel##aipipe_a", false, true)) transcribe_cancel();
            };

            bool is_lyrics   = busy && state.last_pipeline_mode == PipelineMode::Both;
            bool is_subs     = busy && state.last_pipeline_mode == PipelineMode::TranscribeOnly;
            bool is_separate = busy && state.last_pipeline_mode == PipelineMode::SeparateOnly;

            // Extract Lyrics
            ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
            ui_label("Extract Lyrics");
            ImGui::Dummy({0.f, 6.f});
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextWrapped("Transcribes speech into lyric clips synced to the audio.");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 4.f});
            if (is_lyrics) { ai_progress(); }
            else { if (!has_path || busy) ImGui::BeginDisabled();
                   if (ui_btn("Extract Lyrics##a", false, true)) {
                       state.pipeline_on_done = apply_subtitle_mode;
                       kick_pipeline(state, clip.text, PipelineMode::Both);
                   }
                   if (!has_path || busy) ImGui::EndDisabled(); }

            // Extract Subtitles
            ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
            ui_label("Extract Subtitles");
            ImGui::Dummy({0.f, 6.f});
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextWrapped("Generates subtitle clips from speech — no word-level timing.");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 4.f});
            if (is_subs) { ai_progress(); }
            else { if (!has_path || busy) ImGui::BeginDisabled();
                   if (ui_btn("Extract Subtitles##a", false, true)) {
                       state.pipeline_on_done = apply_subtitle_mode;
                       kick_pipeline(state, clip.text, PipelineMode::TranscribeOnly);
                   }
                   if (!has_path || busy) ImGui::EndDisabled(); }

            // Separate Vocals
            ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
            ui_label("Separate Vocals");
            ImGui::Dummy({0.f, 6.f});
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextWrapped("Splits the track into vocals and backing music.");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 4.f});
            if (is_separate) { ai_progress(); }
            else { if (!has_path || busy) ImGui::BeginDisabled();
                   if (ui_btn("Separate Vocals##a", false, true)) kick_pipeline(state, clip.text, PipelineMode::SeparateOnly);
                   if (!has_path || busy) ImGui::EndDisabled(); }
        }

        // ── Noise Reduction ───────────────────────────────────────────────────
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        ui_label("Noise Reduction");
        ImGui::Dummy({0.f, 6.f});
        {
            bool nr_running = state.noise_reduce_running;
            bool has_path2  = !clip.text.empty();
            if (nr_running) {
                ImVec2 bp = ImGui::GetCursorScreenPos();
                ImDrawList* nrdl = ImGui::GetWindowDrawList();
                float t2 = fmodf((float)ImGui::GetTime() * 0.8f, 1.f);
                float p  = state.noise_reduce_progress;
                nrdl->AddRectFilled(bp, {bp.x+bar_w, bp.y+4.f}, to_u32(Col::line), 2.f);
                if (p > 0.f)
                    nrdl->AddRectFilled(bp, {bp.x+bar_w*p, bp.y+4.f}, to_u32(Col::fg), 2.f);
                else
                    nrdl->AddRectFilled({bp.x+bar_w*t2, bp.y}, {bp.x+bar_w*fminf(1.f,t2+0.3f), bp.y+4.f}, to_u32(Col::fg), 2.f);
                ImGui::Dummy({0.f, 8.f});
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                ImGui::TextUnformatted("Reducing noise…");
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                ImGui::TextWrapped("Removes room hum, AC and mic self-noise.");
                ImGui::PopStyleColor();
                if (!state.noise_reduce_output.empty()) {
                    ImGui::Dummy({0.f, 4.f});
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.25f, 0.85f, 0.4f, 1.f));
                    ImGui::TextWrapped("Done — denoised file applied.");
                    ImGui::PopStyleColor();
                }
                if (!state.noise_reduce_error.empty()) {
                    ImGui::Dummy({0.f, 4.f});
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.3f, 0.3f, 1.f));
                    std::string e = state.noise_reduce_error;
                    if (e.size() > 120) e = e.substr(0, 117) + "...";
                    ImGui::TextWrapped("%s", e.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::Dummy({0.f, 4.f});
                if (!has_path2) ImGui::BeginDisabled();
                if (ui_btn("Reduce Noise##a", false, true)) {
                    state.noise_reduce_error.clear();
                    noise_reduce_start(state, clip.text);
                }
                if (!has_path2) ImGui::EndDisabled();
            }
            ImGui::Dummy({0.f, 4.f});
        }

        // ── Advanced ─────────────────────────────────────────────────────────
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        if (ImGui::CollapsingHeader("Advanced##aud_adv")) {
            ImGui::Dummy({0.f, 4.f});
            kf_slider("volume", "Volume", &clip.volume, 0.f, 2.f, "%.2f\xc3\x97"); kf_interp_bar();
            ImGui::Dummy({0.f, 2.f});
            kf_slider("pan",    "Pan",    &clip.pan,   -1.f, 1.f, "%.2f");          kf_interp_bar();
            ImGui::Dummy({0.f, 4.f});
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // RECORD BRICK — cycle recording: transport loops the brick, every pass
    // lands a take in the tray; the selected take plays like an audio clip.
    // ═══════════════════════════════════════════════════════════════════════════
    else if (clip.clip_type == ClipType::Record) {
        float bar_w = w - 16.f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        bool rec_here  = recorder_is_target(state.selected_track, state.selected_clip);

        auto plain_slider = [&](const char* id, const char* label, float* v,
                                float vmin, float vmax, const char* fmt) {
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, to_u32(Col::fg));
            ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
            ImGui::SetNextItemWidth(bar_w);
            bool ch = ImGui::SliderFloat(id, v, vmin, vmax, fmt);
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, label);
            return ch;
        };

        // ── Transport ────────────────────────────────────────────────────────
        ImGui::Dummy({0.f, 4.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextWrapped("Loops %s \xe2\x80\x93 %s while recording. Every pass "
                           "over the loop lands a take below; the newest take "
                           "plays back on the brick.",
                           fmt_time(clip.start).c_str(), fmt_time(clip.end).c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 6.f});

        if (rec_here) {
            // Pulsing stop button + live mic meter.
            float t = (float)ImGui::GetTime();
            float pulse = 0.65f + 0.35f * sinf(t * 6.f);
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.75f*pulse, 0.10f, 0.10f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.15f, 0.15f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.65f, 0.10f, 0.10f, 1.f));
            char stop_lbl[64];
            snprintf(stop_lbl, sizeof(stop_lbl), "\xe2\x96\xa0 Stop  (%d take%s)",
                     recorder_take_count(), recorder_take_count() == 1 ? "" : "s");
            if (ImGui::Button(stop_lbl, {bar_w, 34.f}))
                recorder_stop(state);
            ImGui::PopStyleColor(3);

            // Mic level meter
            ImGui::Dummy({0.f, 4.f});
            ImVec2 mp = ImGui::GetCursorScreenPos();
            float lvl = fminf(1.f, recorder_input_level());
            dl->AddRectFilled(mp, {mp.x + bar_w, mp.y + 6.f}, IM_COL32(35, 35, 50, 255), 3.f);
            ImU32 lc = lvl > 0.9f ? IM_COL32(230, 60, 60, 255)
                     : lvl > 0.7f ? IM_COL32(230, 180, 60, 255)
                                  : IM_COL32(70, 200, 110, 255);
            dl->AddRectFilled(mp, {mp.x + bar_w * lvl, mp.y + 6.f}, lc, 3.f);
            ImGui::Dummy({0.f, 10.f});
        } else {
            bool busy = recorder_active();   // recording a different brick
            if (busy) ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.10f, 0.10f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.12f, 0.12f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.45f, 0.08f, 0.08f, 1.f));
            static std::string s_rec_error;
            if (ImGui::Button("\xe2\x97\x8f Record", {bar_w, 34.f})) {
                s_rec_error.clear();
                if (!recorder_start(state, state.selected_track, state.selected_clip))
                    s_rec_error = "Could not open the microphone.";
            }
            ImGui::PopStyleColor(3);
            if (busy) ImGui::EndDisabled();
            if (!s_rec_error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.3f, 0.3f, 1.f));
                ImGui::TextWrapped("%s", s_rec_error.c_str());
                ImGui::PopStyleColor();
            }
        }

        // ── Input ────────────────────────────────────────────────────────────
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        ui_label("Input");
        ImGui::Dummy({0.f, 6.f});
        {
            // Mic picker — list refreshes when the combo opens; locked while
            // any recording runs (the device is in use).
            static std::vector<std::string> s_devs;
            static bool s_devs_init = false;
            if (!s_devs_init) { s_devs = audio_capture_devices(); s_devs_init = true; }
            bool busy_rec = recorder_active();
            int  mic_sel  = audio_capture_selected();
            const char* cur = (mic_sel >= 0 && mic_sel < (int)s_devs.size())
                              ? s_devs[mic_sel].c_str() : "System default";
            if (busy_rec) ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
            ImGui::SetNextItemWidth(bar_w);
            if (ImGui::BeginCombo("##rec_mic", cur)) {
                if (ImGui::IsWindowAppearing()) s_devs = audio_capture_devices();
                if (ImGui::Selectable("System default", mic_sel < 0))
                    audio_capture_select(-1);
                for (int di = 0; di < (int)s_devs.size(); ++di) {
                    ImGui::PushID(di);
                    if (ImGui::Selectable(s_devs[di].c_str(), mic_sel == di))
                        audio_capture_select(di);
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::PopStyleColor();
            if (busy_rec) ImGui::EndDisabled();

            ImGui::Dummy({0.f, 4.f});
            bool mon = audio_monitor_get();
            if (ImGui::Checkbox("Hear yourself", &mon)) {
                if (mon) {
                    // Monitoring swaps in the low-latency duplex device so
                    // you hear yourself near-instantly, before arming.
                    if (audio_capture_start()) audio_monitor_set(true);
                } else {
                    audio_monitor_set(false);
                    if (!recorder_active()) audio_capture_stop();
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Your mic plays in your headphones while you\n"
                                       "record \xe2\x80\x94 tight enough to sing against.");
                ImGui::EndTooltip();
            }

            ImGui::SameLine(0.f, 12.f);
            bool gate = audio_gate_get();
            if (ImGui::Checkbox("Reduce mic noise", &gate)) audio_gate_set(gate);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Mutes the mic between phrases so hiss and room\n"
                                       "noise don't pile up.");
                ImGui::EndTooltip();
            }
            if (gate) {
                ImGui::Dummy({0.f, 2.f});
                ImGui::Indent(22.f);
                bool bake = audio_gate_bake_get();
                if (ImGui::Checkbox("Apply to recordings", &bake)) audio_gate_bake_set(bake);
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted("Off: recordings keep the raw mic and only what\n"
                                           "you hear is cleaned. On: the cleanup is saved\n"
                                           "into the take itself.");
                    ImGui::EndTooltip();
                }
                ImGui::Unindent(22.f);
            }

            // "Hear effects": the brick's audio FX chain (autotune, reverb…)
            // applied to what YOU hear while singing. Takes stay dry.
            {
                auto segs = collect_audio_fx_segments(state, state.selected_track, clip);
                bool has_fx = !segs.empty();
                if (!has_fx) ImGui::BeginDisabled();
                bool hfx = audio_monitor_fx_get();
                if (ImGui::Checkbox("Hear effects", &hfx)) audio_monitor_fx_set(hfx);
                if (!has_fx) ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::BeginTooltip();
                    if (has_fx)
                        ImGui::TextUnformatted("Sing through this brick's audio effects\n"
                                               "(autotune, reverb\xe2\x80\xa6) live in your headphones.\n"
                                               "Recordings stay dry \xe2\x80\x94 playback applies the\n"
                                               "same effects.");
                    else
                        ImGui::TextUnformatted("Drop an audio effect on this brick first \xe2\x80\x94\n"
                                               "autotune, reverb, delay\xe2\x80\xa6");
                    ImGui::EndTooltip();
                }
            }

            // Perf-mode health line: actual device latency + stalls. Only
            // shown while the duplex device runs — the numbers are honest,
            // straight from the device, not the config request.
            if (audio_perf_mode()) {
                float rt_ms = (audio_latency() + audio_capture_latency()) * 1000.f;
                uint32_t xr = audio_perf_xruns();
                ImGui::Dummy({0.f, 2.f});
                if (xr == 0) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                    ImGui::Text("mic delay %.0f ms", rt_ms);
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(242, 140, 64, 255));
                    ImGui::Text("mic delay %.0f ms \xc2\xb7 %u dropouts", rt_ms, xr);
                }
                ImGui::PopStyleColor();
            }
        }

        // ── Take tray ────────────────────────────────────────────────────────
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        ui_label("Takes");
        ImGui::Dummy({0.f, 6.f});
        if (clip.rec_takes.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextUnformatted("No takes yet \xe2\x80\x94 hit Record.");
            ImGui::PopStyleColor();
        }
        int   place_idx  = -1;   // deferred: placing mutates state.tracks
        int   delete_idx = -1;
        for (int i = 0; i < (int)clip.rec_takes.size(); ++i) {
            ImGui::PushID(i);
            bool sel = (clip.rec_take_sel == i);
            float dur = recorder_wav_duration(clip.rec_takes[i]);
            char lbl[80];
            snprintf(lbl, sizeof(lbl), "%s Take %d  \xc2\xb7  %s",
                     sel ? "\xe2\x96\xb6" : " ", i + 1, fmt_time_short(dur).c_str());
            if (ImGui::Selectable(lbl, sel, 0, {bar_w - 110.f, 0.f})) {
                clip.rec_take_sel = i;
                history_push(state, "Select take");
            }
            ImGui::SameLine(bar_w - 100.f);
            if (ui_btn("Place", false, true)) place_idx = i;
            ImGui::SameLine(0.f, 4.f);
            if (ui_btn("\xc3\x97", false, true)) delete_idx = i;
            ImGui::PopID();
        }
        if (delete_idx >= 0) {
            // Drop the tray entry; the WAV stays in the takes dir (cheap, and
            // a placed copy of it may still be on the timeline).
            clip.rec_takes.erase(clip.rec_takes.begin() + delete_idx);
            if (clip.rec_take_sel == delete_idx) clip.rec_take_sel = -1;
            else if (clip.rec_take_sel > delete_idx) --clip.rec_take_sel;
            if (clip.rec_take_sel < 0 && !clip.rec_takes.empty())
                clip.rec_take_sel = (int)clip.rec_takes.size() - 1;
            history_push(state, "Delete take");
        }

        // ── Sound ────────────────────────────────────────────────────────────
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        ui_label("Sound");
        ImGui::Dummy({0.f, 6.f});
        plain_slider("##rec_vol", "Volume", &clip.volume, 0.f, 2.f, "%.2f\xc3\x97");
        ImGui::Dummy({0.f, 4.f});
        plain_slider("##rec_pan", "Pan", &clip.pan, -1.f, 1.f, "%.2f");

        // Deferred take placement — inserting a track invalidates `clip`, so
        // copy everything first and touch nothing after the insert.
        if (place_idx >= 0 && place_idx < (int)clip.rec_takes.size()) {
            std::string path  = clip.rec_takes[place_idx];
            float       start = clip.start;
            float dur = recorder_wav_duration(path);
            if (dur > 0.f) {
                float qfps = tl_fps(state);
                Clip nc;
                nc.clip_type = ClipType::Audio;
                nc.text      = path;
                nc.source_id = path;
                nc.start     = start;
                nc.end       = snap_end_to_frame(start + dur, (int)qfps);
                Track t;
                char n[48];
                snprintf(n, sizeof(n), "Take %d", place_idx + 1);
                t.name = n;
                t.clips.push_back(std::move(nc));
                state.tracks.insert(state.tracks.begin(), std::move(t));
                state.selected_track += 1;   // brick shifted down one row
                history_push(state, "Place take on track");
                return;
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // CAMERA BRICK — cycle video recording: transport loops the brick, every
    // pass lands a video take; the selected take plays on the brick like a
    // video clip (mirrored into clip.text).
    // ═══════════════════════════════════════════════════════════════════════════
    else if (clip.clip_type == ClipType::VideoRecord) {
        float bar_w = w - 16.f;
        bool rec_here = vrecorder_is_target(state.selected_track, state.selected_clip);

        ImGui::Dummy({0.f, 4.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextWrapped("Loops %s \xe2\x80\x93 %s while recording. Every pass "
                           "over the loop lands a video take below; the newest "
                           "take plays back on the brick.",
                           fmt_time(clip.start).c_str(), fmt_time(clip.end).c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 6.f});

        if (rec_here) {
            float t = (float)ImGui::GetTime();
            float pulse = 0.65f + 0.35f * sinf(t * 6.f);
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.85f*pulse, 0.30f, 0.08f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.38f, 0.12f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.75f, 0.28f, 0.08f, 1.f));
            char stop_lbl[64];
            if (vrecorder_warming())
                snprintf(stop_lbl, sizeof(stop_lbl), "Starting camera\xe2\x80\xa6");
            else
                snprintf(stop_lbl, sizeof(stop_lbl), "\xe2\x96\xa0 Stop  (%d take%s)",
                         vrecorder_take_count(), vrecorder_take_count() == 1 ? "" : "s");
            if (ImGui::Button(stop_lbl, {bar_w, 34.f}))
                vrecorder_stop(state);
            ImGui::PopStyleColor(3);
            if (vrecorder_using_test_pattern()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.3f, 1.f));
                ImGui::TextWrapped("No camera found \xe2\x80\x94 recording a test pattern.");
                ImGui::PopStyleColor();
            }
            ImGui::Dummy({0.f, 6.f});
        } else {
            bool busy = vrecorder_active();   // recording a different brick
            if (busy) ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.70f, 0.28f, 0.08f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.34f, 0.10f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.60f, 0.24f, 0.07f, 1.f));
            if (ImGui::Button("\xe2\x97\x8f Record video", {bar_w, 34.f}))
                vrecorder_start(state, state.selected_track, state.selected_clip);
            ImGui::PopStyleColor(3);
            if (busy) ImGui::EndDisabled();
            ImGui::Dummy({0.f, 6.f});
        }
        if (!vrecorder_error().empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.3f, 0.3f, 1.f));
            ImGui::TextWrapped("%s", vrecorder_error().c_str());
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 4.f});
        }

        // ── Camera ───────────────────────────────────────────────────────────
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        ui_label("Camera");
        ImGui::Dummy({0.f, 6.f});
        {
            static std::vector<VCamDevice> s_cams;
            static bool s_cams_init = false;
            if (!s_cams_init) { s_cams = vrecorder_devices(); s_cams_init = true; }
            bool busy_cam = vrecorder_active() || vrecorder_monitor_get();
            int  cam_sel  = vrecorder_device_selected();
            const char* cur = s_cams.empty() ? "No camera (test pattern)"
                : (cam_sel >= 0 && cam_sel < (int)s_cams.size())
                  ? s_cams[(size_t)cam_sel].name.c_str()
                  : s_cams[0].name.c_str();
            if (busy_cam) ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
            ImGui::SetNextItemWidth(bar_w);
            if (ImGui::BeginCombo("##vrec_cam", cur)) {
                if (ImGui::IsWindowAppearing()) s_cams = vrecorder_devices();
                for (int di = 0; di < (int)s_cams.size(); ++di) {
                    ImGui::PushID(di);
                    char lbl2[128];
                    snprintf(lbl2, sizeof(lbl2), "%s  (%s)",
                             s_cams[(size_t)di].name.c_str(),
                             s_cams[(size_t)di].path.c_str());
                    if (ImGui::Selectable(lbl2, cam_sel == di))
                        vrecorder_device_select(di);
                    ImGui::PopID();
                }
                if (s_cams.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                    ImGui::TextUnformatted("No V4L2 capture devices");
                    ImGui::PopStyleColor();
                }
                ImGui::EndCombo();
            }
            ImGui::PopStyleColor();
            if (busy_cam) ImGui::EndDisabled();

            ImGui::Dummy({0.f, 4.f});
            bool mon = vrecorder_monitor_get();
            if (ImGui::Checkbox("Preview camera", &mon))
                vrecorder_monitor_set(mon);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Show the live camera on the canvas before\n"
                                       "recording \xe2\x80\x94 frame yourself, check the light.");
                ImGui::EndTooltip();
            }
        }

        // ── Layout ───────────────────────────────────────────────────────────
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        ui_label("Layout");
        ImGui::Dummy({0.f, 6.f});
        {
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted("Rotation");
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, to_u32(Col::fg));
            ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
            ImGui::SetNextItemWidth(bar_w - 70.f);
            ImGui::SliderFloat("##vrec_rot", &clip.rotation, -180.f, 180.f, "%.0f\xc2\xb0");
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Rotate camera");
            ImGui::SameLine(0.f, 6.f);
            if (ui_btn("\xe2\x9f\xb3 90\xc2\xb0", false, true)) {
                clip.rotation = fmodf(clip.rotation + 90.f + 180.f, 360.f) - 180.f;
                history_push(state, "Rotate camera");
            }
        }

        // ── Beauty presets ───────────────────────────────────────────────────
        // One-tap looks: each chip drops (or retunes) a glass Multi-FX brick
        // spanning this camera brick. The brick is a normal Multi-FX clip —
        // open it to tweak or extend the chain.
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        ui_label("Beauty");
        ImGui::Dummy({0.f, 6.f});
        {
            struct BParam  { const char* param; float v; };
            struct BFx     { const char* id; FXType type; BParam ps[4]; int np; };
            struct BPreset { const char* name; BFx fx[2]; int nfx; };
            static const BPreset k_beauty[] = {
                {"No Makeup",
                 {{"skin_smooth", FXType::SkinSmooth,
                   {{"amount",0.50f},{"radius",2.5f},{"tone",0.50f}}, 3}}, 1},
                {"Soft Glam",
                 {{"skin_smooth", FXType::SkinSmooth,
                   {{"amount",0.85f},{"radius",3.5f},{"tone",0.55f}}, 3},
                  {"glow_up", FXType::GlowUp,
                   {{"glow",0.45f},{"warmth",0.20f},{"brighten",0.10f}}, 3}}, 2},
                {"Golden Hour",
                 {{"skin_smooth", FXType::SkinSmooth, {{"amount",0.70f}}, 1},
                  {"golden_hour", FXType::GoldenHour, {{"amount",0.90f}}, 1}}, 2},
            };
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextWrapped("One-tap looks \xe2\x80\x94 lands a Multi-FX brick on the camera.");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 4.f});
            int hit = -1;
            for (int bi = 0; bi < (int)(sizeof(k_beauty)/sizeof(k_beauty[0])); ++bi) {
                if (bi > 0) ImGui::SameLine(0.f, 6.f);
                if (ui_btn(k_beauty[bi].name, false, true)) hit = bi;
            }
            if (hit >= 0) {
                const BPreset& bp = k_beauty[hit];
                std::vector<Clip> chain;
                for (int fi = 0; fi < bp.nfx; ++fi) {
                    Clip se;
                    se.clip_type = ClipType::Effect;
                    se.fx_type   = bp.fx[fi].type;
                    for (int pi = 0; pi < bp.fx[fi].np; ++pi)
                        fx_clip_set_param(se, bp.fx[fi].id,
                                          bp.fx[fi].ps[pi].param, bp.fx[fi].ps[pi].v);
                    chain.push_back(se);
                }
                Track& tr = state.tracks[state.selected_track];
                int existing = -1;
                for (int ci = 0; ci < (int)tr.clips.size(); ++ci) {
                    Clip& mc = tr.clips[(size_t)ci];
                    if (mc.clip_type == ClipType::MultiFX &&
                        mc.start < clip.end && mc.end > clip.start) { existing = ci; break; }
                }
                if (existing >= 0) {
                    tr.clips[(size_t)existing].fx_chain = std::move(chain);
                    tr.clips[(size_t)existing].fx_chain_selected = 0;
                    tr.clips[(size_t)existing].fx_coupled  = true;
                    tr.clips[(size_t)existing].fx_host_sid = fx_host_fingerprint(clip);
                } else {
                    Clip nb;
                    nb.clip_type = ClipType::MultiFX;
                    nb.start     = clip.start;
                    nb.end       = clip.end;
                    nb.fx_chain  = std::move(chain);
                    nb.fx_chain_selected = 0;
                    nb.fx_coupled  = true;
                    nb.fx_host_sid = fx_host_fingerprint(clip);
                    tr.clips.push_back(std::move(nb));   // invalidates `clip` —
                }
                history_push(state, std::string("Beauty preset: ") + bp.name);
                return;                                  // — bail out this frame
            }
        }

        // ── Take tray ────────────────────────────────────────────────────────
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        ui_label("Takes");
        ImGui::Dummy({0.f, 6.f});
        if (clip.rec_takes.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextUnformatted("No takes yet \xe2\x80\x94 hit Record.");
            ImGui::PopStyleColor();
        }
        int place_idx = -1, delete_idx = -1;
        for (int i = 0; i < (int)clip.rec_takes.size(); ++i) {
            ImGui::PushID(20000 + i);
            bool sel = (clip.rec_take_sel == i);
            char lbl[80];
            snprintf(lbl, sizeof(lbl), "%s Take %d",
                     sel ? "\xe2\x96\xb6" : " ", i + 1);
            if (ImGui::Selectable(lbl, sel, 0, {bar_w - 110.f, 0.f})) {
                clip.rec_take_sel = i;
                clip.text = clip.rec_takes[(size_t)i];   // mirror for draw/export
                state.proxy_scan_needed = true;
                history_push(state, "Select take");
            }
            ImGui::SameLine(bar_w - 100.f);
            if (ui_btn("Place", false, true)) place_idx = i;
            ImGui::SameLine(0.f, 4.f);
            if (ui_btn("\xc3\x97", false, true)) delete_idx = i;
            ImGui::PopID();
        }
        if (delete_idx >= 0) {
            clip.rec_takes.erase(clip.rec_takes.begin() + delete_idx);
            if (clip.rec_take_sel == delete_idx) clip.rec_take_sel = -1;
            else if (clip.rec_take_sel > delete_idx) --clip.rec_take_sel;
            if (clip.rec_take_sel < 0 && !clip.rec_takes.empty())
                clip.rec_take_sel = (int)clip.rec_takes.size() - 1;
            clip.text = (clip.rec_take_sel >= 0)
                        ? clip.rec_takes[(size_t)clip.rec_take_sel] : "";
            state.proxy_scan_needed = true;
            history_push(state, "Delete take");
        }

        // Deferred placement as a plain Video clip on a new track.
        if (place_idx >= 0 && place_idx < (int)clip.rec_takes.size()) {
            std::string path  = clip.rec_takes[(size_t)place_idx];
            float       start = clip.start;
            float dur = video_probe_duration(path);
            if (dur <= 0.f) dur = clip.end - clip.start;
            float qfps = tl_fps(state);
            Clip nc;
            nc.clip_type = ClipType::Video;
            nc.text      = path;
            nc.source_id = path;
            nc.start     = start;
            nc.end       = snap_end_to_frame(start + dur, (int)qfps);
            Track t;
            char n[48];
            snprintf(n, sizeof(n), "Cam take %d", place_idx + 1);
            t.name = n;
            t.clips.push_back(std::move(nc));
            state.tracks.insert(state.tracks.begin(), std::move(t));
            state.selected_track += 1;
            history_push(state, "Place take on track");
            return;
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // BODY FX CLIP (always glass — lives on same track as a video clip)
    // ═══════════════════════════════════════════════════════════════════════════
    else if (clip.clip_type == ClipType::BodyFX) {
        float bar_w = w - 16.f;
        ImDrawList* bgdl = ImGui::GetWindowDrawList();

        // ── Effect name + params ──────────────────────────────────────────────
        ImGui::Dummy({0.f, 4.f});
        {
            const BodyFXInfo* info = body_fx_find_info(clip.body_fx_type);
            if (info) {
                ImGui::PushFont(g_font_bold);
                ImGui::TextUnformatted(info->name);
                ImGui::PopFont();
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                ImGui::TextUnformatted(info->tagline);
                ImGui::PopStyleColor();
            }
        }

        if (clip.body_fx_type != BodyFXType::RemoveBackground) {
            // Amount only shown for effects other than RemoveBackground
            ImGui::Dummy({0.f, 6.f});
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted("Amount");
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, to_u32(Col::fg));
            ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
            ImGui::SetNextItemWidth(bar_w);
            if (ImGui::SliderFloat("##bfx_amount", &clip.body_fx_amount, 0.f, 1.f, "%.2f"))
                {}
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, "Amount");

            const BodyFXInfo* info = body_fx_find_info(clip.body_fx_type);
            if (info && info->n_params > 0) {
                ImGui::Dummy({0.f, 4.f});
                for (int pi = 0; pi < info->n_params && pi < 4; ++pi) {
                    const BodyFXParamDef& pd = info->params[pi];
                    char id[32]; snprintf(id, sizeof(id), "##bfxp%d", pi);
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                    ImGui::TextUnformatted(pd.label);
                    ImGui::PopStyleColor();
                    ImGui::PushStyleColor(ImGuiCol_SliderGrab, to_u32(Col::fg));
                    ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
                    ImGui::SetNextItemWidth(bar_w);
                    ImGui::SliderFloat(id, &clip.body_fx_params[pi], pd.min_val, pd.max_val, "%.3f");
                    ImGui::PopStyleColor(2);
                    if (ImGui::IsItemDeactivatedAfterEdit()) history_push(state, pd.label);
                    ImGui::Dummy({0.f, 2.f});
                }
            }
        }

        // ── Remove Background — use the sibling video clip's bg_remove UI ────
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        ui_label("Remove Background");
        ImGui::Dummy({0.f, 6.f});

        // Find the video clip that overlaps this brick's time range
        Clip* vid_clip = nullptr;
        int   vid_ci   = -1;
        for (int ci2 = 0; ci2 < (int)track.clips.size(); ++ci2) {
            Clip& vc = track.clips[ci2];
            if (vc.clip_type != ClipType::Video) continue;
            if (vc.end <= clip.start || vc.start >= clip.end) continue;
            vid_clip = &vc;
            vid_ci   = ci2;
            break;
        }

        if (!vid_clip) {
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextWrapped("No video clip on this track. Body FX must be on the same track as a video clip.");
            ImGui::PopStyleColor();
        } else {
            auto& vc = *vid_clip;
            auto status = vc.bg_remove_status;

            static BgRemoveStatus s_prev_bfx_bgr = BgRemoveStatus::Idle;
            static int            s_prev_bfx_ci  = -1;
            if (status == BgRemoveStatus::Processing && vc.bg_remove_progress > 0.f) {
                float dur = vc.end - vc.start;
                if (dur > 0.f) state.playhead = vc.start + fminf(vc.bg_remove_progress, 0.99f) * dur;
            } else if (status == BgRemoveStatus::Ready &&
                       s_prev_bfx_bgr == BgRemoveStatus::Processing &&
                       s_prev_bfx_ci == vid_ci) {
                seek_to(state, vc.start);
            }
            s_prev_bfx_bgr = status; s_prev_bfx_ci = vid_ci;

            if (status == BgRemoveStatus::WaitingForProxy) {
                ImVec2 bp = ImGui::GetCursorScreenPos();
                ImU32 fill_col  = IM_COL32(120, 160, 255, 255);
                ImU32 track_col = IM_COL32(120, 160, 255, 40);
                bgdl->AddRectFilled(bp, {bp.x + bar_w, bp.y + 4.f}, track_col, 2.f);
                float t = fmodf((float)ImGui::GetTime() * 0.6f, 1.f);
                float x0 = bp.x + (bar_w - bar_w * 0.35f) * t;
                bgdl->AddRectFilled({x0, bp.y}, {x0 + bar_w * 0.35f, bp.y + 4.f}, fill_col, 2.f);
                ImGui::Dummy({0.f, 8.f});
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                ImGui::TextUnformatted("Waiting for proxy…");
                ImGui::PopStyleColor();
                ImGui::Dummy({0.f, 4.f});
            } else if (status == BgRemoveStatus::Processing) {
                ImVec2 bp = ImGui::GetCursorScreenPos();
                ImU32 fill_col  = IM_COL32(255, 165, 0, 255);
                ImU32 track_col = IM_COL32(255, 165, 0, 40);
                bgdl->AddRectFilled(bp, {bp.x + bar_w, bp.y + 4.f}, track_col, 2.f);
                if (vc.bg_remove_progress < 0.f) {
                    float t = fmodf((float)ImGui::GetTime() * 0.8f, 1.f);
                    float x0 = bp.x + (bar_w - bar_w * 0.35f) * t;
                    bgdl->AddRectFilled({x0, bp.y}, {x0 + bar_w * 0.35f, bp.y + 4.f}, fill_col, 2.f);
                    ImGui::Dummy({0.f, 8.f});
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                    ImGui::TextUnformatted("Downloading AI model…");
                    ImGui::PopStyleColor();
                } else {
                    float pct = fmaxf(0.01f, fminf(1.f, vc.bg_remove_progress));
                    bgdl->AddRectFilled(bp, {bp.x + bar_w * pct, bp.y + 4.f}, fill_col, 2.f);
                    ImGui::Dummy({0.f, 8.f});
                    char msg[64];
                    snprintf(msg, sizeof(msg), "Removing background…  %d%%", (int)(pct * 100.f));
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                    ImGui::TextUnformatted(msg);
                    ImGui::PopStyleColor();
                }
                ImGui::Dummy({0.f, 4.f});
            } else if (status == BgRemoveStatus::Ready) {
                ImVec2 bp = ImGui::GetCursorScreenPos();
                bgdl->AddRectFilled(bp, {bp.x + bar_w, bp.y + 4.f}, IM_COL32(40, 200, 80, 255), 2.f);
                ImGui::Dummy({0.f, 8.f});
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.25f, 0.85f, 0.4f, 1.f));
                ImGui::TextUnformatted("Ready");
                ImGui::PopStyleColor();
                ImGui::Dummy({0.f, 6.f});
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted); ImGui::TextUnformatted("Edge softness"); ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_SliderGrab, to_u32(Col::fg));
                ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
                ImGui::SetNextItemWidth(bar_w);
                if (ImGui::SliderFloat("##bfx_bgrsoft", &vc.bg_remove_softness, 0.f, 1.f, "%.2f"))
                    history_push(state, "BG Softness");
                ImGui::PopStyleColor(2);
                ImGui::Dummy({0.f, 6.f});
                if (ui_btn("Re-run", false, true))
                    bg_remove_start(state, state.selected_track, vid_ci);
            } else if (status == BgRemoveStatus::Error) {
                ImVec2 bp = ImGui::GetCursorScreenPos();
                bgdl->AddRectFilled(bp, {bp.x + bar_w, bp.y + 4.f}, IM_COL32(220, 60, 60, 255), 2.f);
                ImGui::Dummy({0.f, 8.f});
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.3f, 0.3f, 1.f));
                ImGui::TextUnformatted("Failed");
                ImGui::PopStyleColor();
                if (!vc.bg_remove_error.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                    std::string err = vc.bg_remove_error;
                    if (err.size() > 120) err = err.substr(0, 117) + "...";
                    ImGui::TextWrapped("%s", err.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::Dummy({0.f, 4.f});
                if (ui_btn("Retry", false, true))
                    bg_remove_start(state, state.selected_track, vid_ci);
            } else {
                bool proxy_ok = !vc.text.empty() && proxy_is_ready(vc.text);
                if (!proxy_ok) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                    ImGui::TextWrapped("Waiting for video proxy…");
                    ImGui::PopStyleColor();
                    ImGui::Dummy({0.f, 4.f});
                }
                if (!proxy_ok) ImGui::BeginDisabled();
                if (ui_btn("Remove Background", false, true))
                    bg_remove_start(state, state.selected_track, vid_ci);
                if (!proxy_ok) ImGui::EndDisabled();
                ImGui::Dummy({0.f, 4.f});
            }
        }

        glass_host_layout(state, clip, w);
    }
}
