// pipeline_ui.cpp — the on-screen status strips for the ML pipeline and the
// windowed-search runs. The pipeline LOGIC lives engine-side in
// src/pipeline_core.cpp; these draw its state.
#include "studio_types.h"
#include "studio_shared.h"
#include "pipeline.h"
#include "theme.h"
#include "../app.h"
#include <imgui.h>
#include <imgui_internal.h>

extern ImFont* g_font_bold;

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

    // Clean progress bar along the bottom edge — same look as bg removal. An
    // indeterminate sweep until real progress starts (model download / warmup).
    float pp = state.pipeline.progress > 0.001f ? state.pipeline.progress : -1.f;
    ui_progress_bar(dl, p.x, p.y + h - 3.f, w, pp, IM_COL32(255, 165, 0, 255), 3.f);

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
