#include "screens.h"
#include "theme.h"
#include "app.h"
#include "audio.h"
#include "video.h"
#include "proxy.h"
#include "filepicker.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <cmath>
#include <string>
#include <cstdio>
#include <algorithm>

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string fmt_time(float s) {
    int m   = (int)(s / 60);
    int sec = (int)s % 60;
    int cs  = (int)((s - floorf(s)) * 100.f);
    char buf[20];
    snprintf(buf, sizeof(buf), "%d:%02d.%02d", m, sec, cs);
    return buf;
}

static std::string fmt_time_short(float s) {
    int m   = (int)(s / 60);
    int sec = (int)s % 60;
    char buf[12];
    snprintf(buf, sizeof(buf), "%d:%02d", m, sec);
    return buf;
}

// ── State local to this screen ────────────────────────────────────────────────

static char  s_edit_buf[512] = {};
static bool  s_edit_focus_next = false;

// ── Preview ───────────────────────────────────────────────────────────────────

static void draw_preview(AppState& state, ImVec2 p, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    extern ImFont* g_font_bold;

    // Background
    if (state.video_loaded && video_is_open()) {
        uintptr_t tex = video_get_texture((double)state.playhead);
        if (tex)
            dl->AddImage(ImTextureRef((ImTextureID)tex), p, {p.x + w, p.y + h});
        else
            dl->AddRectFilled(p, {p.x + w, p.y + h}, to_u32(Col::accent_dark), 2.f);
    } else {
        dl->AddRectFilled(p, {p.x + w, p.y + h}, to_u32(Col::accent_dark), 2.f);
    }
    dl->AddRect(p, {p.x + w, p.y + h}, to_u32(Col::line), 2.f);

    // Corner marks
    float cm = 10.f;
    ImU32 cc = to_u32(Col::muted);
    dl->AddLine(p,              {p.x + cm, p.y},       cc);
    dl->AddLine(p,              {p.x, p.y + cm},       cc);
    dl->AddLine({p.x+w, p.y},  {p.x+w-cm, p.y},       cc);
    dl->AddLine({p.x+w, p.y},  {p.x+w, p.y+cm},       cc);
    dl->AddLine({p.x, p.y+h},  {p.x+cm, p.y+h},       cc);
    dl->AddLine({p.x, p.y+h},  {p.x, p.y+h-cm},       cc);
    dl->AddLine({p.x+w, p.y+h},{p.x+w-cm, p.y+h},     cc);
    dl->AddLine({p.x+w, p.y+h},{p.x+w, p.y+h-cm},     cc);

    dl->AddText({p.x+8.f, p.y+6.f}, to_u32(Col::muted), "Preview  1080p");
    dl->AddText({p.x+w-44.f, p.y+6.f}, to_u32(Col::fg), "● Live");

    // Active subtitle clips across all subtitle tracks — stack them vertically
    int rendered = 0;
    for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
        auto& track = state.tracks[ti];
        bool has_text = !track.clips.empty() &&
            std::any_of(track.clips.begin(), track.clips.end(),
                [](const Clip& c){ return c.clip_type == ClipType::Text; });
        if (!has_text || !track.visible) continue;

        // Find active clip for this track at current playhead
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

        // If a clip on this track is selected, always show its text in preview
        const Clip* show = active;
        if (!show && state.selected_track == ti && state.selected_clip >= 0 &&
            state.selected_clip < (int)track.clips.size())
            show = &track.clips[state.selected_clip];

        if (!show) { ++rendered; continue; }

        // Vertical slot — bottom of preview area, stack upward
        float slot_h  = 40.f;
        float slot_y  = p.y + h - 24.f - (rendered + 1) * slot_h;

        ImGui::PushFont(g_font_bold);
        ImGui::SetWindowFontScale(1.6f);
        ImVec2 tsz = ImGui::CalcTextSize(show->text.c_str());
        float  tx  = p.x + (w - tsz.x) * 0.5f;
        // Shadow
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.6f,
            {tx + 2.f, slot_y + 2.f}, IM_COL32(0,0,0,180), show->text.c_str());
        // Text — dim if it's the selected (not actually playing) clip
        ImU32 col = (active_ci >= 0) ? to_u32(Col::fg) : to_u32(Col::muted);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.6f,
            {tx, slot_y}, col, show->text.c_str());
        ImGui::SetWindowFontScale(1.f);
        ImGui::PopFont();
        ++rendered;
    }
}

// ── Right panel ───────────────────────────────────────────────────────────────

static void draw_properties(AppState& state, float w) {

    if (state.selected_track < 0 || state.selected_track >= (int)state.tracks.size() ||
        state.selected_clip  < 0 || state.selected_clip  >= (int)state.tracks[state.selected_track].clips.size()) {
        ImGui::Dummy({0.f, 32.f});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImVec2 hint = ImGui::CalcTextSize("Click a clip to edit");
        ImGui::SetCursorPosX((w - hint.x) * 0.5f);
        ImGui::TextUnformatted("Click a clip to edit");
        ImGui::PopStyleColor();
        return;
    }

    Track& track = state.tracks[state.selected_track];
    Clip&  clip  = track.clips[state.selected_clip];

    // Track label
    ImGui::Dummy({0.f, 12.f});
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    char track_label[64];
    snprintf(track_label, sizeof(track_label), "Track — %s", track.name.c_str());
    ImGui::TextUnformatted(track_label);
    ImGui::PopStyleColor();

    ImGui::Dummy({0.f, 4.f});
    ui_separator();
    ImGui::Dummy({0.f, 8.f});

    if (clip.clip_type == ClipType::Text) {
        // Text edit
        ui_label("Text");
        ImGui::Dummy({0.f, 4.f});

        if (s_edit_focus_next) {
            ImGui::SetKeyboardFocusHere();
            s_edit_focus_next = false;
        }

        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
        ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
        ImGui::SetNextItemWidth(w - 16.f);
        if (ImGui::InputText("##clip_text", s_edit_buf, sizeof(s_edit_buf),
                ImGuiInputTextFlags_EnterReturnsTrue)) {
            clip.text = s_edit_buf;
        }
        if (ImGui::IsItemDeactivated())
            clip.text = s_edit_buf;
        ImGui::PopStyleColor(2);

        ImGui::Dummy({0.f, 16.f});
        ui_separator();
        ImGui::Dummy({0.f, 8.f});

        // Timing
        ui_label("Timing");
        ImGui::Dummy({0.f, 4.f});

        float half = (w - 24.f) * 0.5f;

        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg_soft);
        ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);

        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted("Start");
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(half);
        float start = clip.start;
        if (ImGui::InputFloat("##start", &start, 0.05f, 0.1f, "%.3f")) {
            if (start < clip.end - 0.05f) clip.start = start;
        }
        ImGui::EndGroup();

        ImGui::SameLine(0.f, 8.f);

        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted("End");
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(half);
        float end = clip.end;
        if (ImGui::InputFloat("##end", &end, 0.05f, 0.1f, "%.3f")) {
            if (end > clip.start + 0.05f) clip.end = end;
        }
        ImGui::EndGroup();

        ImGui::PopStyleColor(2);

        // Duration display
        ImGui::Dummy({0.f, 4.f});
        char dur_buf[32];
        snprintf(dur_buf, sizeof(dur_buf), "Duration  %.3fs", clip.end - clip.start);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted(dur_buf);
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 16.f});
        ui_separator();
        ImGui::Dummy({0.f, 8.f});

        // Nudge timing buttons
        ui_label("Nudge");
        ImGui::Dummy({0.f, 4.f});
        if (ui_btn("-100ms", false, true)) {
            clip.start -= 0.1f; clip.end -= 0.1f;
            if (clip.start < 0.f) { clip.end -= clip.start; clip.start = 0.f; }
        }
        ImGui::SameLine(0.f, 4.f);
        if (ui_btn("-10ms", false, true)) { clip.start -= 0.01f; clip.end -= 0.01f; }
        ImGui::SameLine(0.f, 4.f);
        if (ui_btn("+10ms", false, true)) { clip.start += 0.01f; clip.end += 0.01f; }
        ImGui::SameLine(0.f, 4.f);
        if (ui_btn("+100ms", false, true)) { clip.start += 0.1f; clip.end += 0.1f; }

    } else {
        // Non-subtitle track — just show path/name
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        const char* tname = clip.clip_type == ClipType::Audio ? "Audio clip" : "Video clip";
        ImGui::TextUnformatted(tname);
        ImGui::Dummy({0.f, 4.f});
        ImGui::TextWrapped("%s", clip.text.empty() ? "(no path)" : clip.text.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Dummy({0.f, 16.f});
    ui_separator();
    ImGui::Dummy({0.f, 8.f});

    if (ui_btn("Delete clip", false, true)) {
        track.clips.erase(track.clips.begin() + state.selected_clip);
        state.selected_clip = -1;
    }
}

// ── Timeline ──────────────────────────────────────────────────────────────────

static constexpr float TL_LABEL_W  = 110.f;
static constexpr float TL_TRACK_H  = 36.f;
static constexpr float TL_RULER_H  = 20.f;

static void draw_timeline(AppState& state, ImVec2 origin, float total_w, float total_h) {
    ImDrawList* dl    = ImGui::GetWindowDrawList();
    float clip_area_w = total_w - TL_LABEL_W;
    float dur         = fmaxf(state.duration, 1.f);
    float& zoom       = state.tl_zoom;   // px/sec
    float& scroll     = state.tl_scroll; // px offset

    float tl_content_w = dur * zoom;

    // ── Scroll wheel for horizontal scroll / pinch-zoom ───────────────────────
    ImVec2 mouse = ImGui::GetIO().MousePos;
    bool in_tl = mouse.x >= origin.x + TL_LABEL_W && mouse.x <= origin.x + total_w &&
                 mouse.y >= origin.y && mouse.y <= origin.y + total_h;
    if (in_tl) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (ImGui::GetIO().KeyCtrl) {
            // Ctrl+scroll = zoom
            float old_zoom = zoom;
            zoom = fmaxf(20.f, fminf(zoom * (1.f + wheel * 0.1f), 4000.f));
            // Keep position under mouse stable
            float mouse_t = (mouse.x - origin.x - TL_LABEL_W + scroll) / old_zoom;
            scroll = fmaxf(0.f, mouse_t * zoom - (mouse.x - origin.x - TL_LABEL_W));
        } else {
            scroll = fmaxf(0.f, scroll - wheel * 60.f);
        }
        scroll = fminf(scroll, fmaxf(0.f, tl_content_w - clip_area_w + 60.f));
    }

    // ── Background ────────────────────────────────────────────────────────────
    dl->AddRectFilled(origin, {origin.x + total_w, origin.y + total_h},
        to_u32(Col::bg));
    dl->AddRect(origin, {origin.x + total_w, origin.y + total_h},
        to_u32(Col::line));

    // ── Time ruler ────────────────────────────────────────────────────────────
    float ruler_y = origin.y;
    dl->AddRectFilled({origin.x + TL_LABEL_W, ruler_y},
        {origin.x + total_w, ruler_y + TL_RULER_H}, to_u32(Col::bg_soft));

    // Choose tick interval based on zoom
    float tick_secs = 1.f;
    if (zoom > 500.f)       tick_secs = 0.1f;
    else if (zoom > 200.f)  tick_secs = 0.5f;
    else if (zoom > 80.f)   tick_secs = 1.f;
    else if (zoom > 30.f)   tick_secs = 5.f;
    else                    tick_secs = 10.f;

    float first_t = floorf(scroll / zoom / tick_secs) * tick_secs;
    for (float t = first_t; t <= dur + tick_secs; t += tick_secs) {
        float px = origin.x + TL_LABEL_W + t * zoom - scroll;
        if (px < origin.x + TL_LABEL_W || px > origin.x + total_w) continue;
        bool big = (fmodf(t, tick_secs * 5.f) < 0.001f);
        dl->AddLine({px, ruler_y + (big ? 4.f : 10.f)}, {px, ruler_y + TL_RULER_H},
            to_u32(big ? Col::muted : Col::dim));
        if (big || zoom > 100.f) {
            char tbuf[12];
            snprintf(tbuf, sizeof(tbuf), "%s", fmt_time_short(t).c_str());
            dl->AddText({px + 3.f, ruler_y + 4.f}, to_u32(Col::muted), tbuf);
        }
    }

    // ── Tracks ────────────────────────────────────────────────────────────────
    float track_y = origin.y + TL_RULER_H;

    // Drag state for clips
    static int   drag_track = -1, drag_clip = -1;
    static float drag_offset = 0.f;
    static bool  drag_left_edge = false, drag_right_edge = false;

    for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
        Track& track = state.tracks[ti];

        // Track background
        ImVec2 row_tl = {origin.x, track_y};
        ImVec2 row_br = {origin.x + total_w, track_y + TL_TRACK_H};
        bool row_hov = mouse.y >= row_tl.y && mouse.y < row_br.y;
        dl->AddRectFilled(row_tl, row_br,
            to_u32(row_hov ? Col::bg_soft_hov : Col::bg_soft));
        dl->AddLine({origin.x, row_br.y}, {origin.x + total_w, row_br.y},
            to_u32(Col::line));

        // Track label
        dl->AddText({origin.x + 8.f, track_y + (TL_TRACK_H - 13.f) * 0.5f},
            to_u32(state.selected_track == ti ? Col::fg : Col::muted),
            track.name.c_str());

        // Visibility toggle dot
        ImVec2 vis_c = {origin.x + TL_LABEL_W - 16.f, track_y + TL_TRACK_H * 0.5f};
        dl->AddCircleFilled(vis_c, 4.f, to_u32(track.visible ? Col::fg : Col::dim));
        if (ImGui::IsMouseClicked(0)) {
            ImVec2 mp = ImGui::GetIO().MousePos;
            if (fabsf(mp.x - vis_c.x) < 7.f && fabsf(mp.y - vis_c.y) < 7.f)
                track.visible = !track.visible;
        }

        // Clip area separator line
        dl->AddLine({origin.x + TL_LABEL_W, track_y},
                    {origin.x + TL_LABEL_W, track_y + TL_TRACK_H},
                    to_u32(Col::line));

        // Clips
        for (int ci = 0; ci < (int)track.clips.size(); ++ci) {
            Clip& clip = track.clips[ci];
            float cx0 = origin.x + TL_LABEL_W + clip.start * zoom - scroll;
            float cx1 = origin.x + TL_LABEL_W + clip.end   * zoom - scroll;
            if (cx1 < origin.x + TL_LABEL_W || cx0 > origin.x + total_w) continue;
            cx0 = fmaxf(cx0, origin.x + TL_LABEL_W);
            cx1 = fminf(cx1, origin.x + total_w);

            bool selected = (state.selected_track == ti && state.selected_clip == ci);

            ImVec4 fill_col = selected ? Col::fg : Col::bg_soft_hov;
            ImVec4 text_col = selected ? Col::bg : Col::fg;

            float cy0 = track_y + 3.f;
            float cy1 = track_y + TL_TRACK_H - 3.f;

            dl->AddRectFilled({cx0, cy0}, {cx1, cy1}, to_u32(fill_col), 2.f);
            dl->AddRect({cx0, cy0}, {cx1, cy1},
                to_u32(selected ? Col::fg : Col::line), 2.f);

            // Clip text
            if (clip.clip_type == ClipType::Text && !clip.text.empty()) {
                ImVec2 tsz = ImGui::CalcTextSize(clip.text.c_str());
                if (tsz.x < (cx1 - cx0) - 8.f) {
                    ImGui::PushClipRect({cx0, cy0}, {cx1, cy1}, true);
                    dl->AddText({cx0 + 4.f, cy0 + (cy1 - cy0 - 13.f) * 0.5f},
                        to_u32(text_col), clip.text.c_str());
                    ImGui::PopClipRect();
                }
            } else if (clip.clip_type == ClipType::Audio) {
                // Simple waveform bars
                int bars = (int)((cx1 - cx0) / 3.f);
                for (int b = 0; b < bars; ++b) {
                    float bx  = cx0 + b * 3.f + 1.f;
                    float amp = 0.25f + 0.55f * fabsf(sinf(b * 0.37f + ti) *
                                                       cosf(b * 0.11f));
                    float mid = (cy0 + cy1) * 0.5f;
                    float ht  = amp * (cy1 - cy0 - 6.f) * 0.5f;
                    dl->AddLine({bx, mid - ht}, {bx, mid + ht}, to_u32(Col::muted));
                }
            } else if (clip.clip_type == ClipType::Video) {
                dl->AddText({cx0 + 4.f, cy0 + 3.f},
                    to_u32(text_col), clip.text.empty() ? "Video" : clip.text.c_str());
            }

            // Edge handles
            float edge_w = 4.f;
            dl->AddRectFilled({cx0, cy0}, {cx0 + edge_w, cy1},
                to_u32(selected ? Col::muted : ImVec4{0,0,0,0}), 1.f);
            dl->AddRectFilled({cx1 - edge_w, cy0}, {cx1, cy1},
                to_u32(selected ? Col::muted : ImVec4{0,0,0,0}), 1.f);

            // Click handling
            if (ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemActive()) {
                ImVec2 mp = ImGui::GetIO().MousePos;
                if (mp.y >= cy0 && mp.y <= cy1 && mp.x >= cx0 && mp.x <= cx1) {
                    state.selected_track = ti;
                    state.selected_clip  = ci;
                    // Sync edit buffer
                    strncpy(s_edit_buf, clip.text.c_str(), sizeof(s_edit_buf) - 1);
                    s_edit_buf[sizeof(s_edit_buf)-1] = '\0';
                    s_edit_focus_next = (clip.clip_type == ClipType::Text);

                    // Seek to clip start
                    state.playhead = clip.start;
                    audio_seek(clip.start);

                    // Begin drag — detect edge
                    float orig_cx0 = origin.x + TL_LABEL_W + clip.start * zoom - scroll;
                    float orig_cx1 = origin.x + TL_LABEL_W + clip.end   * zoom - scroll;
                    if (mp.x <= orig_cx0 + edge_w + 2.f) {
                        drag_track = ti; drag_clip = ci;
                        drag_left_edge = true; drag_right_edge = false;
                        drag_offset = 0.f;
                    } else if (mp.x >= orig_cx1 - edge_w - 2.f) {
                        drag_track = ti; drag_clip = ci;
                        drag_right_edge = true; drag_left_edge = false;
                        drag_offset = 0.f;
                    } else {
                        drag_track = ti; drag_clip = ci;
                        drag_left_edge = false; drag_right_edge = false;
                        drag_offset = mp.x - orig_cx0;
                    }
                }
            }
        }

        track_y += TL_TRACK_H;
    }

    // ── Handle drag ───────────────────────────────────────────────────────────
    if (drag_track >= 0 && drag_clip >= 0 && ImGui::IsMouseDragging(0)) {
        ImVec2 mp = ImGui::GetIO().MousePos;
        float new_t = (mp.x - origin.x - TL_LABEL_W + scroll - drag_offset) / zoom;
        Clip& dc = state.tracks[drag_track].clips[drag_clip];
        if (drag_left_edge) {
            float new_start = fmaxf(0.f, fminf(new_t, dc.end - 0.05f));
            dc.start = new_start;
        } else if (drag_right_edge) {
            float new_end_t = (mp.x - origin.x - TL_LABEL_W + scroll) / zoom;
            dc.end = fmaxf(dc.start + 0.05f, new_end_t);
        } else {
            float dur_clip = dc.end - dc.start;
            float new_start = fmaxf(0.f, new_t);
            dc.start = new_start;
            dc.end   = new_start + dur_clip;
        }
    }
    if (ImGui::IsMouseReleased(0)) {
        drag_track = -1; drag_clip = -1;
        drag_left_edge = false; drag_right_edge = false;
    }

    // ── Playhead ──────────────────────────────────────────────────────────────
    float ph_x = origin.x + TL_LABEL_W + state.playhead * zoom - scroll;
    if (ph_x >= origin.x + TL_LABEL_W && ph_x <= origin.x + total_w) {
        dl->AddLine({ph_x, origin.y}, {ph_x, origin.y + total_h},
            to_u32(Col::fg));
        // Playhead handle
        dl->AddTriangleFilled(
            {ph_x - 5.f, origin.y},
            {ph_x + 5.f, origin.y},
            {ph_x, origin.y + 10.f},
            to_u32(Col::fg));
    }

    // Click ruler to seek
    if (ImGui::IsMouseClicked(0) || ImGui::IsMouseDragging(0)) {
        ImVec2 mp = ImGui::GetIO().MousePos;
        if (mp.y >= origin.y && mp.y <= origin.y + TL_RULER_H &&
            mp.x >= origin.x + TL_LABEL_W && mp.x <= origin.x + total_w) {
            float t = (mp.x - origin.x - TL_LABEL_W + scroll) / zoom;
            state.playhead = fmaxf(0.f, fminf(t, dur));
            audio_seek(state.playhead);
        }
    }

    // ── Add Track button ──────────────────────────────────────────────────────
    ImVec2 add_p = {origin.x + 8.f, track_y + 6.f};
    dl->AddText(add_p, to_u32(Col::muted), "+ Add Track");
    if (ImGui::IsMouseClicked(0)) {
        ImVec2 mp = ImGui::GetIO().MousePos;
        if (mp.y >= track_y && mp.y <= track_y + TL_TRACK_H &&
            mp.x >= origin.x && mp.x <= origin.x + TL_LABEL_W + 80.f) {
            Track t;
            char name[32];
            snprintf(name, sizeof(name), "Track %d", (int)state.tracks.size() + 1);
            t.name = name;
            state.tracks.push_back(t);
        }
    }
}

// ── Split and delete keyboard shortcuts ───────────────────────────────────────

static void handle_edit_shortcuts(AppState& state) {
    if (state.selected_track < 0 || state.selected_clip < 0) return;
    Track& track = state.tracks[state.selected_track];
    if (state.selected_clip >= (int)track.clips.size()) return;
    Clip& clip = track.clips[state.selected_clip];

    // S or Ctrl+B — split at playhead
    if ((ImGui::IsKeyPressed(ImGuiKey_S) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_B))
        && !ImGui::IsAnyItemActive()) {
        float cut = state.playhead;
        if (cut > clip.start + 0.02f && cut < clip.end - 0.02f) {
            Clip right = clip;
            clip.end   = cut;

            // Rebuild text for left half — approximate by time fraction
            right.start = cut;
            right.text  = clip.text;  // both halves keep full text; user edits after

            int ins_idx = state.selected_clip + 1;
            track.clips.insert(track.clips.begin() + ins_idx, right);
        }
    }

    // Delete — remove selected clip
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !ImGui::IsAnyItemActive()) {
        track.clips.erase(track.clips.begin() + state.selected_clip);
        state.selected_clip = -1;
    }
}

// ── Main screen ───────────────────────────────────────────────────────────────

void ui_screen_editor(AppState& state) {
    ImGui::SetNextWindowPos(ImGui::GetCursorScreenPos());
    ImGui::SetNextWindowSize(ImGui::GetContentRegionAvail());
    ImGui::BeginChild("##editor_body", {0, 0});

    float win_w  = ImGui::GetContentRegionAvail().x;
    float win_h  = ImGui::GetContentRegionAvail().y;

    // Handle keyboard shortcuts (split, delete) before ImGui item processing
    handle_edit_shortcuts(state);

    // ── Toolbar ───────────────────────────────────────────────────────────────
    float toolbar_h = 36.f;
    ImGui::Dummy({0.f, 6.f});
    ImGui::SetCursorPosX(12.f);

    const char* vid_label = state.video_loaded ? "Video  ✓" : "+ Video";
    if (ui_btn(vid_label, state.video_loaded, true)) {
        std::string picked = filepicker_open(
            "Select background video", "Video", "*.mp4 *.mov *.mkv *.avi *.webm");
        if (!picked.empty()) {
            proxy_start(picked);
            video_open_still(proxy_still_path(picked));
            if (proxy_is_ready(picked)) {
                ProxyInfo pi;
                if (proxy_load(picked, pi)) { video_open_proxy(pi); state.proxy_ready = true; }
            }
            {
                state.video_path   = picked;
                state.video_loaded = true;
                state.proxy_ready  = false;
                // Add a video track if none exists
                bool has_vid = false;
                for (auto& t : state.tracks)
                    for (auto& c : t.clips) if (c.clip_type == ClipType::Video) { has_vid = true; break; }
                if (!has_vid) {
                    Track vt;
                    vt.name = "Video";
                    Clip vc;
                    vc.clip_type = ClipType::Video;
                    vc.start = 0.f;
                    vc.end   = state.duration > 0.f ? state.duration : 300.f;
                    vc.text  = picked;
                    vt.clips.push_back(vc);
                    state.tracks.push_back(vt);
                }
            }
        }
    }
    if (state.video_loaded) {
        ImGui::SameLine(0.f, 4.f);
        if (ui_btn("x", false, true)) {
            video_close();
            state.video_loaded = false;
            state.video_path.clear();
            state.tracks.erase(
                std::remove_if(state.tracks.begin(), state.tracks.end(),
                    [](const Track& t){
                        return !t.clips.empty() && t.clips[0].clip_type == ClipType::Video;
                    }),
                state.tracks.end());
        }
    }

    ImGui::SameLine(0.f, 12.f);
    if (ui_btn("+ Track", false, true)) {
        Track t;
        char name[32];
        snprintf(name, sizeof(name), "Track %d", (int)state.tracks.size() + 1);
        t.name = name;
        state.tracks.push_back(t);
    }

    ImGui::SameLine(0.f, 4.f);
    bool can_split = state.selected_track >= 0 && state.selected_clip >= 0;
    if (!can_split) ImGui::BeginDisabled();
    if (ui_btn("Split  [S]", false, true)) {
        // Trigger same logic as keyboard shortcut
        if (can_split) {
            Track& track = state.tracks[state.selected_track];
            Clip&  clip  = track.clips[state.selected_clip];
            float  cut   = state.playhead;
            if (cut > clip.start + 0.02f && cut < clip.end - 0.02f) {
                Clip right = clip;
                clip.end   = cut;
                right.start = cut;
                track.clips.insert(
                    track.clips.begin() + state.selected_clip + 1, right);
            }
        }
    }
    if (!can_split) ImGui::EndDisabled();

    // Transport controls right-aligned
    float ctrl_w = 220.f;
    ImGui::SameLine(win_w - ctrl_w - 12.f);
    if (ui_btn(state.playing ? "||" : ">", false, true)) {
        state.playing = !state.playing;
        if (state.playing) audio_play();
        else               audio_pause();
    }
    ImGui::SameLine(0.f, 8.f);
    char tbuf[32];
    float dur = fmaxf(state.duration, 0.01f);
    snprintf(tbuf, sizeof(tbuf), "%s / %s",
        fmt_time(state.playhead).c_str(), fmt_time(dur).c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextUnformatted(tbuf);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.f, 12.f);
    if (ui_btn("Render  ->", true, true)) state.go(Screen::Export);

    ImGui::Dummy({0.f, 4.f});
    ImGui::SetCursorPosX(0.f);
    ui_separator();

    // ── Layout zones ─────────────────────────────────────────────────────────
    float tl_h       = 200.f;    // timeline strip height
    float body_h     = win_h - toolbar_h - tl_h - 24.f;
    float props_w    = fmaxf(260.f, win_w * 0.28f);
    float preview_w  = win_w - props_w - 1.f;

    // ── Preview zone ─────────────────────────────────────────────────────────
    ImGui::SetCursorPos({0.f, (float)ImGui::GetCursorPosY()});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col::bg);
    ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
    if (ImGui::BeginChild("##preview_zone", {preview_w, body_h}, ImGuiChildFlags_Borders)) {
        float avail_w = ImGui::GetContentRegionAvail().x;
        float avail_h = ImGui::GetContentRegionAvail().y;

        // Stage — maintain aspect ratio
        float stage_w, stage_h;
        if (avail_w / avail_h > 16.f / 9.f) {
            stage_h = avail_h - 16.f;
            stage_w = stage_h * 16.f / 9.f;
        } else {
            stage_w = avail_w - 16.f;
            stage_h = stage_w * 9.f / 16.f;
        }
        float ox = (avail_w - stage_w) * 0.5f;
        float oy = (avail_h - stage_h) * 0.5f;
        ImGui::SetCursorPos({ox, oy});
        ImVec2 stage_p = ImGui::GetCursorScreenPos();
        ImGui::Dummy({stage_w, stage_h});

        draw_preview(state, stage_p, stage_w, stage_h);

        // Scrub bar under preview
        float sb_y = oy + stage_h + 4.f;
        ImGui::SetCursorPos({ox, sb_y});
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, Col::fg);
        ImGui::PushStyleColor(ImGuiCol_FrameBg,    Col::bg_soft);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0.f, 0.f});
        ImGui::SetNextItemWidth(stage_w);
        float fill = state.playhead / dur;
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
        // Header
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Col::transparent);
        ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
        if (ImGui::BeginChild("##props_head", {0.f, 28.f}, ImGuiChildFlags_Borders)) {
            ui_label("Clip Properties");
            if (state.selected_track >= 0 && state.selected_clip >= 0) {
                const char* shortcut = "  Del=delete  S=split";
                ImGui::SameLine(props_w - ImGui::CalcTextSize(shortcut).x - 8.f);
                ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
                ImGui::TextUnformatted(shortcut);
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);

        ImGui::BeginChild("##props_scroll", {0.f, 0.f});
        ImGui::SetCursorPosX(8.f);
        draw_properties(state, props_w - 16.f);
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);

    // ── Timeline ─────────────────────────────────────────────────────────────
    float tl_track_rows = fmaxf(3.f, (float)state.tracks.size() + 1.f); // +1 for add row
    float tl_needed     = TL_RULER_H + tl_track_rows * TL_TRACK_H + TL_TRACK_H;
    float tl_actual     = fminf(tl_h, tl_needed);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col::bg);
    ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
    if (ImGui::BeginChild("##tl_zone", {win_w, tl_actual}, ImGuiChildFlags_Borders)) {
        ImVec2 tl_origin = ImGui::GetCursorScreenPos();
        float  tl_w      = ImGui::GetContentRegionAvail().x;
        float  tl_h_real = ImGui::GetContentRegionAvail().y;

        // Invisible full-size item so ImGui tracks mouse over this region
        ImGui::Dummy({tl_w, tl_h_real});

        draw_timeline(state, tl_origin, tl_w, tl_h_real);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);

    ImGui::EndChild();
}
