#include "studio_types.h"
#include "studio_shared.h"
#include "panel_clip.h"
#include "pipeline.h"
#include "app.h"
#include "audio.h"
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
#include "noise_reduce.h"
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
    if (ui_btn("Split", false, true)) {
        float cut = state.playhead;
        if (cut > clip.start + 0.02f && cut < clip.end - 0.02f) {
            Clip right = clip; clip.end = cut; right.start = cut;
            right.in_point += (cut - clip.start) * clip.speed;
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
        if (track.locked) ImGui::EndDisabled();
        return;
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
        float bar_w = w - 16.f;
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
            float opacity_pct = clip.opacity * 100.f;
            if (plain_slider("##vid_opacity", "Opacity", &opacity_pct, 0.f, 100.f, "%.0f%%"))
                clip.opacity = opacity_pct / 100.f;
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
            plain_slider("##vid_px", "Left \xe2\x86\x94 Right", &clip.pos_x, -1.f, 2.f, "%.2f");
            ImGui::Dummy({0.f, 4.f});
            plain_slider("##vid_py", "Up \xe2\x86\x95 Down",   &clip.pos_y, -1.f, 2.f, "%.2f");
            ImGui::Dummy({0.f, 4.f});
            // Unified size — drives scale_x and scale_y together
            float size = (clip.scale_x + clip.scale_y) * 0.5f;
            if (plain_slider("##vid_sz", "Size", &size, 0.f, 4.f, "%.2f")) {
                clip.scale_x = size; clip.scale_y = size;
            }
            ImGui::Dummy({0.f, 4.f});
            plain_slider("##vid_rot", "Rotation", &clip.rotation, -180.f, 180.f, "%.1f\xc2\xb0");
        }

        // ── Speed ────────────────────────────────────────────────────────────
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        ui_label("Speed");
        ImGui::Dummy({0.f, 6.f});
        {
            plain_slider("##vid_spd", "Speed", &clip.speed, 0.25f, 4.f, "%.2f\xc3\x97");
            ImGui::Dummy({0.f, 6.f});
            struct SP { float f; const char* l; };
            SP spresets[] = {{0.25f,"\xc2\xbc\xc3\x97"},{0.5f,"\xc2\xbd\xc3\x97"},
                             {1.f,"1\xc3\x97"},{2.f,"2\xc3\x97"},{4.f,"4\xc3\x97"}};
            for (auto& p : spresets) {
                if (ui_btn(p.l, fabsf(clip.speed - p.f) < 0.01f, true))
                    { clip.speed = p.f; history_push(state, "Speed"); }
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
            if (plain_slider("##vid_vol", "Volume", &vol_pct, 0.f, 200.f, "%.0f%%"))
                clip.volume = vol_pct / 100.f;
            ImGui::Dummy({0.f, 4.f});
            plain_slider("##vid_pan", "Pan  (left \xe2\x86\x94 right)", &clip.pan, -1.f, 1.f, "%.2f");
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

            bool is_lyrics   = busy && !state.pipeline_is_separate_only && !state.pipeline_produces_subtitles;
            bool is_subs     = busy && state.pipeline_produces_subtitles && !state.pipeline_is_separate_only;
            bool is_separate = busy && state.pipeline_is_separate_only;

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
                   if (ui_btn("Extract Lyrics", false, true)) kick_pipeline(state, clip.text, PipelineMode::Both);
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
                   if (ui_btn("Extract Subtitles", false, true)) kick_pipeline(state, clip.text, PipelineMode::TranscribeOnly);
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
            extern std::string g_noise_reduce_script;
            bool nr_installed = noise_reduce_is_installed(state.python_path);
            bool nr_running   = state.noise_reduce_running;
            bool has_path2    = !clip.text.empty();
            if (!nr_installed) {
                ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                ImGui::TextWrapped("Removes background hum and mic noise from audio.\nRequires: pip install noisereduce soundfile");
                ImGui::PopStyleColor();
            } else if (nr_running) {
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
                    noise_reduce_start(state, clip.text, state.python_path, g_noise_reduce_script);
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

            auto inst = rembg_install_status();
            bool rembg_ok = rembg_is_installed(state.python_path);
            if (!rembg_ok) {
                if (inst == RembgInstallStatus::Running) {
                    float t = fmodf((float)ImGui::GetTime() * 0.8f, 1.f);
                    ImVec2 bp = ImGui::GetCursorScreenPos();
                    bgdl->AddRectFilled(bp, {bp.x+bar_w, bp.y+4.f}, IM_COL32(255,165,0,40), 2.f);
                    bgdl->AddRectFilled({bp.x+bar_w*t, bp.y}, {bp.x+bar_w*fminf(1.f,t+0.3f), bp.y+4.f}, IM_COL32(255,165,0,220), 2.f);
                    ImGui::Dummy({0.f, 8.f});
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f,0.65f,0.f,1.f));
                    ImGui::TextUnformatted("Installing rembg…");
                    ImGui::PopStyleColor();
                } else if (inst == RembgInstallStatus::Failed) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f,0.3f,0.3f,1.f));
                    ImGui::TextUnformatted("Install failed.");
                    ImGui::PopStyleColor();
                    std::string ie = rembg_install_error();
                    if (!ie.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                        if (ie.size() > 120) ie = ie.substr(ie.size()-120);
                        ImGui::TextWrapped("%s", ie.c_str());
                        ImGui::PopStyleColor();
                    }
                    ImGui::Dummy({0.f,4.f});
                    if (ui_btn("Retry install", false, true)) rembg_install_start(state.python_path);
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                    ImGui::TextWrapped("rembg is not installed. It's a small package needed to remove backgrounds.");
                    ImGui::PopStyleColor();
                    ImGui::Dummy({0.f,4.f});
                    if (ui_btn("Install rembg", false, true)) rembg_install_start(state.python_path);
                }
                ImGui::Dummy({0.f, 4.f});
            }

            auto status = clip.bg_remove_status;
            if (!rembg_ok) ImGui::BeginDisabled();

            bool tog = clip.bg_remove_on;
            if (ImGui::Checkbox("Enable##bgr", &tog)) {
                clip.bg_remove_on = tog;
                history_push(state, "Remove Background");
            }
            ImGui::Dummy({0.f, 6.f});

            static BgRemoveStatus s_prev_bgr_status = BgRemoveStatus::Idle;
            static int            s_prev_bgr_clip   = -1;
            int cur_clip = state.selected_clip;

            if (status == BgRemoveStatus::Processing && clip.bg_remove_progress > 0.f) {
                float dur = clip.end - clip.start;
                if (dur > 0.f) {
                    float t = fminf(clip.bg_remove_progress, 0.99f);
                    state.playhead = clip.start + t * dur;
                }
            } else if (status == BgRemoveStatus::Ready &&
                       s_prev_bgr_status == BgRemoveStatus::Processing &&
                       s_prev_bgr_clip == cur_clip) {
                seek_to(state, clip.start);
            }
            s_prev_bgr_status = status;
            s_prev_bgr_clip   = cur_clip;

            if (status == BgRemoveStatus::Processing) {
                ImVec2 bp = ImGui::GetCursorScreenPos();
                ImU32  amber     = IM_COL32(255, 165, 0, 255);
                ImU32  amber_dim = IM_COL32(255, 165, 0, 60);

                if (clip.bg_remove_progress < 0.f) {
                    float t   = fmodf((float)ImGui::GetTime() * 0.6f, 1.f);
                    float seg = bar_w * 0.35f;
                    float x0  = bp.x + (bar_w - seg) * t;
                    bgdl->AddRectFilled(bp, {bp.x+bar_w, bp.y+6.f}, amber_dim, 3.f);
                    bgdl->AddRectFilled({x0, bp.y}, {x0+seg, bp.y+6.f}, amber, 3.f);
                    ImGui::Dummy({0.f, 10.f});
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.65f, 0.f, 1.f));
                    ImGui::TextUnformatted("Downloading AI model…");
                    ImGui::PopStyleColor();
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                    ImGui::TextWrapped("First run only (~180 MB). Please wait.");
                    ImGui::PopStyleColor();
                } else {
                    float fill = fmaxf(0.01f, fminf(1.f, clip.bg_remove_progress));
                    bgdl->AddRectFilled(bp, {bp.x+bar_w, bp.y+6.f}, amber_dim, 3.f);
                    bgdl->AddRectFilled(bp, {bp.x+bar_w*fill, bp.y+6.f}, amber, 3.f);
                    ImGui::Dummy({0.f, 10.f});
                    char pct[64];
                    snprintf(pct, sizeof(pct), "Removing background…  %d%%", (int)(fill * 100.f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.65f, 0.f, 1.f));
                    ImGui::TextUnformatted(pct);
                    ImGui::PopStyleColor();
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                    ImGui::TextWrapped("Please wait. This processes every frame using the AI model.");
                    ImGui::PopStyleColor();
                }
                ImGui::Dummy({0.f, 4.f});

            } else if (status == BgRemoveStatus::Ready) {
                ImVec2 bp = ImGui::GetCursorScreenPos();
                bgdl->AddRectFilled(bp, {bp.x+bar_w, bp.y+6.f}, IM_COL32(40, 200, 80, 255), 3.f);
                ImGui::Dummy({0.f, 10.f});
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
                if (ui_btn("Re-run", false, true)) {
                    extern std::string g_rembg_script;
                    bg_remove_start(state, state.selected_track, state.selected_clip,
                                    state.python_path, g_rembg_script);
                }

            } else if (status == BgRemoveStatus::Error) {
                ImVec2 bp = ImGui::GetCursorScreenPos();
                bgdl->AddRectFilled(bp, {bp.x+bar_w, bp.y+6.f}, IM_COL32(220, 60, 60, 255), 3.f);
                ImGui::Dummy({0.f, 10.f});
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
                if (ui_btn("Retry", false, true)) {
                    extern std::string g_rembg_script;
                    bg_remove_start(state, state.selected_track, state.selected_clip,
                                    state.python_path, g_rembg_script);
                }

            } else {
                bool proxy_ok = !clip.text.empty() && proxy_is_ready(clip.text);
                if (!proxy_ok) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                    ImGui::TextWrapped("Waiting for video proxy to finish before processing.");
                    ImGui::PopStyleColor();
                    ImGui::Dummy({0.f, 4.f});
                }
                if (!proxy_ok || !clip.bg_remove_on) ImGui::BeginDisabled();
                if (ui_btn("Process  (removes background via AI)", false, true)) {
                    extern std::string g_rembg_script;
                    bg_remove_start(state, state.selected_track, state.selected_clip,
                                    state.python_path, g_rembg_script);
                }
                if (!proxy_ok || !clip.bg_remove_on) ImGui::EndDisabled();
                if (!clip.bg_remove_on) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                    ImGui::TextWrapped("Enable the toggle above to start.");
                    ImGui::PopStyleColor();
                }
                ImGui::Dummy({0.f, 4.f});
            }
            if (!rembg_ok) ImGui::EndDisabled();
        }

        // ── Advanced ─────────────────────────────────────────────────────────
        ImGui::Dummy({0.f, 10.f}); ui_separator(); ImGui::Dummy({0.f, 6.f});
        if (ImGui::CollapsingHeader("Advanced##vid_adv")) {
            ImGui::Dummy({0.f, 4.f});
            kf_slider("opacity",  "Opacity",          &clip.opacity,   0.f,   1.f,    "%.2f");           kf_interp_bar();
            ImGui::Dummy({0.f, 2.f});
            kf_slider("pos_x",    "Left \xe2\x86\x94 Right", &clip.pos_x, -1.f,   2.f,    "%.2f");       kf_interp_bar();
            ImGui::Dummy({0.f, 2.f});
            kf_slider("pos_y",    "Up \xe2\x86\x95 Down",    &clip.pos_y, -1.f,   2.f,    "%.2f");       kf_interp_bar();
            ImGui::Dummy({0.f, 2.f});
            kf_slider("scale_x",  "Scale X",          &clip.scale_x,   0.f,   4.f,    "%.2f");           kf_interp_bar();
            ImGui::Dummy({0.f, 2.f});
            kf_slider("scale_y",  "Scale Y",          &clip.scale_y,   0.f,   4.f,    "%.2f");           kf_interp_bar();
            ImGui::Dummy({0.f, 2.f});
            kf_slider("rotation", "Rotation",         &clip.rotation, -180.f, 180.f,  "%.1f\xc2\xb0");  kf_interp_bar();
            ImGui::Dummy({0.f, 2.f});
            kf_slider("volume",   "Volume",           &clip.volume,    0.f,   2.f,    "%.2f\xc3\x97");   kf_interp_bar();
            ImGui::Dummy({0.f, 2.f});
            kf_slider("pan",      "Pan",              &clip.pan,      -1.f,   1.f,    "%.2f");           kf_interp_bar();
            ImGui::Dummy({0.f, 4.f});
        }

    }

    // ═══════════════════════════════════════════════════════════════════════════
    // AUDIO CLIP
    // ═══════════════════════════════════════════════════════════════════════════
    else if (clip.clip_type == ClipType::Audio) {
        float bar_w = w - 16.f;
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
            plain_slider("##aud_spd", "Speed", &clip.speed, 0.25f, 4.f, "%.2f\xc3\x97");
            ImGui::Dummy({0.f, 6.f});
            struct SP { float f; const char* l; };
            SP spresets[] = {{0.25f,"\xc2\xbc\xc3\x97"},{0.5f,"\xc2\xbd\xc3\x97"},
                             {1.f,"1\xc3\x97"},{2.f,"2\xc3\x97"},{4.f,"4\xc3\x97"}};
            for (auto& p : spresets) {
                if (ui_btn(p.l, fabsf(clip.speed - p.f) < 0.01f, true))
                    { clip.speed = p.f; history_push(state, "Speed"); }
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

            bool is_lyrics   = busy && !state.pipeline_is_separate_only && !state.pipeline_produces_subtitles;
            bool is_subs     = busy && state.pipeline_produces_subtitles && !state.pipeline_is_separate_only;
            bool is_separate = busy && state.pipeline_is_separate_only;

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
                   if (ui_btn("Extract Lyrics##a", false, true)) kick_pipeline(state, clip.text, PipelineMode::Both);
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
                   if (ui_btn("Extract Subtitles##a", false, true)) kick_pipeline(state, clip.text, PipelineMode::TranscribeOnly);
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
            extern std::string g_noise_reduce_script;
            bool nr_installed = noise_reduce_is_installed(state.python_path);
            bool nr_running   = state.noise_reduce_running;
            bool has_path2    = !clip.text.empty();
            if (!nr_installed) {
                ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                ImGui::TextWrapped("Removes background hum and mic noise from audio.\nRequires: pip install noisereduce soundfile");
                ImGui::PopStyleColor();
            } else if (nr_running) {
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
                    noise_reduce_start(state, clip.text, state.python_path, g_noise_reduce_script);
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
}
